#include "GeometryParser.hpp"
#include "TextureParser.hpp"
#include <fstream>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <filesystem>

namespace NFSU2 {

uint32_t GeometryParser::BinHash(const std::string& str) {
    uint32_t h = 0xFFFFFFFFu;
    for (unsigned char c : str) {
        h = h * 33u + c;
    }
    return h;
}

static std::string extractPartName(const uint8_t* hdrData, size_t hdrSize) {
    if (hdrSize < 0xA4) return "";
    // In NFSU2 0x00134011 chunks, the 28-byte part name sits at the end of the header (offset hdrSize - 28, typically 0xA4)
    size_t startOff = (hdrSize >= 28) ? (hdrSize - 28) : 0xA4;
    if (hdrData[startOff] == 0 && hdrSize >= 0xA4) {
        startOff = 0xA4;
    }
    // Scan forward if leading bytes are zero
    while (startOff < hdrSize && hdrData[startOff] == 0) {
        startOff++;
    }
    std::string s;
    for (size_t i = startOff; i < hdrSize; ++i) {
        char c = static_cast<char>(hdrData[i]);
        if (c == 0) break;
        if (c >= 32 && c <= 126) {
            s += c;
        }
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

static float sanitizeFloat(float v, float fallback = 0.0f, float maxAbs = 100000.0f) {
    if (std::isnan(v) || std::isinf(v) || std::abs(v) > maxAbs) {
        return fallback;
    }
    return v;
}

struct RawSubMeshEntry {
    uint32_t startIndex{0};
    uint32_t indexCount{0};
    uint32_t texIndex{0};
    uint32_t shdIndex{0};
};

static void parsePartsFromBuffer(const uint8_t* rawBuf, size_t rawSize, std::vector<CarPart>& parts, int depth = 0) {
    if (!rawBuf || rawSize < 8 || depth > 4) return;
    std::vector<uint8_t> buffer(rawBuf, rawBuf + rawSize);

    size_t offset = 0;
    while (offset + 8 <= buffer.size()) {
        uint32_t cid = *reinterpret_cast<const uint32_t*>(&buffer[offset]);
        uint32_t size = *reinterpret_cast<const uint32_t*>(&buffer[offset + 4]);

        // Universal JDLZ Decompression (NFSU2 / Most Wanted 2005 / Carbon 2006 compressed geometry blocks)
        if (cid == 0x5A4C444Au && offset + 16 <= buffer.size()) {
            uint32_t uncompSize = *reinterpret_cast<const uint32_t*>(&buffer[offset + 8]);
            uint32_t compSize = *reinterpret_cast<const uint32_t*>(&buffer[offset + 12]);
            if (compSize >= 18 && offset + compSize <= buffer.size() && uncompSize > 0 && uncompSize < 64 * 1024 * 1024) {
                std::vector<uint8_t> decomp(uncompSize);
                int decBytes = TextureParser::DecompressJDLZ(&buffer[offset], static_cast<int>(compSize),
                                                             decomp.data(), static_cast<int>(uncompSize));
                if (decBytes > 0) {
                    parsePartsFromBuffer(decomp.data(), static_cast<size_t>(decBytes), parts, depth + 1);
                }
                offset += compSize;
                continue;
            }
        } else if (offset + 24 <= buffer.size() &&
                   buffer[offset + 8] == 'J' && buffer[offset + 9] == 'D' &&
                   buffer[offset + 10] == 'L' && buffer[offset + 11] == 'Z') {
            size_t jdlzOff = offset + 8;
            uint32_t uncompSize = *reinterpret_cast<const uint32_t*>(&buffer[jdlzOff + 8]);
            uint32_t compSize = *reinterpret_cast<const uint32_t*>(&buffer[jdlzOff + 12]);
            size_t avail = std::min(static_cast<size_t>(size), buffer.size() - jdlzOff);
            size_t actualComp = (compSize >= 18 && compSize <= avail) ? compSize : avail;
            if (actualComp >= 18 && uncompSize > 0 && uncompSize < 64 * 1024 * 1024) {
                std::vector<uint8_t> decomp(uncompSize);
                int decBytes = TextureParser::DecompressJDLZ(&buffer[jdlzOff], static_cast<int>(actualComp),
                                                             decomp.data(), static_cast<int>(uncompSize));
                if (decBytes > 0) {
                    parsePartsFromBuffer(decomp.data(), static_cast<size_t>(decBytes), parts, depth + 1);
                }
                offset += 8 + size;
                continue;
            }
        }

        size_t nextTopOffset = (cid & 0x80000000) ? (offset + 8) : (offset + 8 + size);
        if (nextTopOffset <= offset || nextTopOffset > buffer.size()) break;

        if (cid == 0x80134010) { // Solid Object / Part Container (NFSU1, NFSU2, Most Wanted, Carbon)
            size_t subEnd = std::min(offset + 8 + static_cast<size_t>(size), buffer.size());
            size_t subOff = offset + 8;

            std::string partName;
            std::vector<uint32_t> texHashes;
            std::vector<uint32_t> shdHashes;
            std::vector<PositionMarker> markers;
            std::vector<Vertex> partVertices;
            std::vector<uint16_t> rawIndices;
            std::vector<RawSubMeshEntry> rawSubmeshes;
            uint32_t declaredTriangles = 0;
            uint32_t declaredVertices = 0;
            bool hadNormals = true;

            while (subOff + 8 <= subEnd) {
                uint32_t scid = *reinterpret_cast<const uint32_t*>(&buffer[subOff]);
                uint32_t ssize = *reinterpret_cast<const uint32_t*>(&buffer[subOff + 4]);
                size_t chunkDataStart = subOff + 8;
                size_t chunkDataEnd = std::min(chunkDataStart + static_cast<size_t>(ssize), subEnd);

                if (scid == 0x00134011 && ssize >= 0xA4) {
                    partName = extractPartName(&buffer[chunkDataStart], chunkDataEnd - chunkDataStart);
                } else if (scid == 0x00134012 && ssize >= 8) {
                    for (size_t toff = chunkDataStart; toff + 8 <= chunkDataEnd; toff += 8) {
                        uint32_t th = *reinterpret_cast<const uint32_t*>(&buffer[toff]);
                        texHashes.push_back(th);
                    }
                } else if (scid == 0x00134013 && ssize >= 8) {
                    for (size_t soff2 = chunkDataStart; soff2 + 8 <= chunkDataEnd; soff2 += 8) {
                        uint32_t sh = *reinterpret_cast<const uint32_t*>(&buffer[soff2]);
                        shdHashes.push_back(sh);
                    }
                } else if (scid == 0x0013401A && ssize >= 80) {
                    size_t mStart = chunkDataStart;
                    while (mStart < chunkDataEnd && buffer[mStart] == 0x11) mStart++;
                    for (size_t moff = mStart; moff + 80 <= chunkDataEnd; moff += 80) {
                        uint32_t mh = *reinterpret_cast<const uint32_t*>(&buffer[moff]);
                        const float* mf = reinterpret_cast<const float*>(&buffer[moff + 64]);
                        // Transform NFSU2 (X=Front, Y=Left, Z=Up) -> Viewer (X=Front, Y=Up, Z=Left)
                        markers.push_back({mh, Vec3(mf[0], mf[2], mf[1])});
                    }
                } else if (scid == 0x80134100) { // Mesh Container
                    size_t meshEnd = chunkDataEnd;
                    size_t meshOff = chunkDataStart;

                    while (meshOff + 8 <= meshEnd) {
                        uint32_t mcid = *reinterpret_cast<const uint32_t*>(&buffer[meshOff]);
                        uint32_t msize = *reinterpret_cast<const uint32_t*>(&buffer[meshOff + 4]);
                        size_t mStart = meshOff + 8;
                        size_t mEnd = std::min(mStart + static_cast<size_t>(msize), meshEnd);

                        if (mcid == 0x00134900 && msize >= 56) {
                            while (mStart < mEnd && buffer[mStart] == 0x11) mStart++;
                            size_t numInts = (mEnd - mStart) / 4;
                            const uint32_t* ints = reinterpret_cast<const uint32_t*>(&buffer[mStart]);
                            if (numInts >= 14) {
                                declaredTriangles = ints[numInts - 8];
                                declaredVertices = ints[numInts - 4];
                            }
                        } else if (mcid == 0x00134B01 && msize >= 24) { // Vertex Buffer
                            size_t vDataStart = mStart;
                            while (vDataStart < mEnd && buffer[vDataStart] == 0x11) {
                                vDataStart++;
                            }
                            size_t vDataBytes = mEnd > vDataStart ? (mEnd - vDataStart) : 0;

                            // Universal stride detection across NFSU1/U2 (24, 36) and MW/Carbon (36, 40, 48, 56, 60, 64)
                            size_t stride = 36;
                            if (declaredVertices > 0) {
                                size_t cand = vDataBytes / declaredVertices;
                                size_t rem = vDataBytes - (cand * declaredVertices);
                                if ((cand == 24 || cand == 28 || cand == 32 || cand == 36 ||
                                     cand == 40 || cand == 44 || cand == 48 || cand == 52 ||
                                     cand == 56 || cand == 60 || cand == 64) && rem < 16) {
                                    stride = cand;
                                }
                            } else if (vDataBytes % 36 != 0) {
                                if (vDataBytes % 24 == 0) stride = 24;
                                else if (vDataBytes % 48 == 0) stride = 48;
                                else if (vDataBytes % 60 == 0) stride = 60;
                                else if (vDataBytes % 64 == 0) stride = 64;
                            }

                            size_t numVerts = (declaredVertices > 0 && vDataBytes >= declaredVertices * stride)
                                              ? declaredVertices
                                              : (vDataBytes / stride);
                            hadNormals = (stride >= 36);

                            partVertices.reserve(numVerts);
                            for (size_t vi = 0; vi < numVerts; ++vi) {
                                size_t vo = vDataStart + vi * stride;
                                if (vo + stride > buffer.size()) break;

                                const float* f = reinterpret_cast<const float*>(&buffer[vo]);
                                Vertex vert{};

                                float px = sanitizeFloat(f[0], 0.0f, 50000.0f);
                                float py = sanitizeFloat(f[1], 0.0f, 50000.0f);
                                float pz = sanitizeFloat(f[2], 0.0f, 50000.0f);

                                // NFS: X=Front, Y=Left, Z=Up -> Viewer: X=Front, Y=Up, Z=Left
                                vert.pos = Vec3(px, pz, py);

                                uint32_t rawColor = 0xFFFFFFFFu;
                                if (stride >= 36) {
                                    float nx = sanitizeFloat(f[3], 0.0f, 10.0f);
                                    float ny = sanitizeFloat(f[4], 0.0f, 10.0f);
                                    float nz = sanitizeFloat(f[5], 1.0f, 10.0f);
                                    vert.normal = Vec3(nx, nz, ny).normalized();
                                    rawColor = *reinterpret_cast<const uint32_t*>(&buffer[vo + 24]);
                                    vert.uv = Vec2(sanitizeFloat(f[7], 0.0f, 1024.0f),
                                                   sanitizeFloat(f[8], 0.0f, 1024.0f));
                                } else {
                                    vert.normal = Vec3(0.0f, 1.0f, 0.0f);
                                    rawColor = *reinterpret_cast<const uint32_t*>(&buffer[vo + 12]);
                                    vert.uv = Vec2(sanitizeFloat(f[4], 0.0f, 1024.0f),
                                                   sanitizeFloat(f[5], 0.0f, 1024.0f));
                                }

                                float a = ((rawColor >> 24) & 0xFF) / 255.0f;
                                float r = ((rawColor >> 16) & 0xFF) / 255.0f;
                                float g = ((rawColor >> 8) & 0xFF) / 255.0f;
                                float b = (rawColor & 0xFF) / 255.0f;
                                vert.color = Vec4(r, g, b, a);

                                partVertices.push_back(vert);
                            }
                        } else if (mcid == 0x00134B02 && msize >= 60) { // Submesh / Material Draw Table
                            size_t sStart = mStart;
                            while (sStart < mEnd && buffer[sStart] == 0x11) sStart++;
                            size_t numSubs = (mEnd > sStart) ? ((mEnd - sStart) / 60) : 0;
                            for (size_t si = 0; si < numSubs; ++si) {
                                const uint32_t* ints = reinterpret_cast<const uint32_t*>(&buffer[sStart + si * 60]);
                                RawSubMeshEntry entry{};
                                entry.indexCount = ints[3];
                                entry.texIndex = ints[7];
                                entry.shdIndex = ints[8];
                                entry.startIndex = ints[13];
                                rawSubmeshes.push_back(entry);
                            }
                        } else if (mcid == 0x00134B03 && msize >= 6) { // Index Buffer
                            size_t iDataStart = mStart;
                            while (iDataStart < mEnd && buffer[iDataStart] == 0x11) {
                                iDataStart++;
                            }
                            if ((iDataStart % 2) != 0) iDataStart++;

                            size_t iDataBytes = mEnd > iDataStart ? (mEnd - iDataStart) : 0;
                            size_t numIndices = iDataBytes / 2;

                            const uint16_t* idxPtr = reinterpret_cast<const uint16_t*>(&buffer[iDataStart]);
                            rawIndices.assign(idxPtr, idxPtr + numIndices);
                        }

                        meshOff += (mcid & 0x80000000) ? 8 : (8 + msize);
                    }
                }

                // Always advance by full subchunk size so we don't re-traverse 0x80134100 children
                subOff += 8 + ssize;
            }

            if (!partVertices.empty() && !rawIndices.empty()) {
                if (partName.empty()) {
                    partName = "UNNAMED_PART_" + std::to_string(parts.size());
                }

                std::vector<uint32_t> cleanIndices;
                std::vector<SubMesh> cleanSubmeshes;
                size_t vCount = partVertices.size();

                if (!rawSubmeshes.empty()) {
                    for (const auto& sm : rawSubmeshes) {
                        uint32_t firstOut = static_cast<uint32_t>(cleanIndices.size());
                        uint32_t validTriIndices = (sm.indexCount / 3) * 3;
                        for (uint32_t k = 0; k + 2 < validTriIndices; k += 3) {
                            size_t idxPos = static_cast<size_t>(sm.startIndex) + k;
                            if (idxPos + 2 >= rawIndices.size()) break;
                            uint32_t i0 = rawIndices[idxPos];
                            uint32_t i1 = rawIndices[idxPos + 1];
                            uint32_t i2 = rawIndices[idxPos + 2];
                            if (i0 < vCount && i1 < vCount && i2 < vCount &&
                                i0 != i1 && i1 != i2 && i0 != i2) {
                                cleanIndices.push_back(i0);
                                cleanIndices.push_back(i1);
                                cleanIndices.push_back(i2);
                            }
                        }
                        uint32_t addedCount = static_cast<uint32_t>(cleanIndices.size()) - firstOut;
                        if (addedCount > 0) {
                            SubMesh sub{};
                            sub.firstIndex = firstOut;
                            sub.indexCount = addedCount;
                            sub.textureHash = (sm.texIndex < texHashes.size()) ? texHashes[sm.texIndex] : 0;
                            sub.shaderHash = (sm.shdIndex < shdHashes.size()) ? shdHashes[sm.shdIndex] : 0;
                            sub.partName = partName;
                            cleanSubmeshes.push_back(sub);
                        }
                    }
                } else {
                    size_t maxIdx = (declaredTriangles > 0 && declaredTriangles * 3 <= rawIndices.size())
                                    ? (declaredTriangles * 3)
                                    : ((rawIndices.size() / 3) * 3);
                    for (size_t k = 0; k + 2 < maxIdx; k += 3) {
                        uint32_t i0 = rawIndices[k];
                        uint32_t i1 = rawIndices[k + 1];
                        uint32_t i2 = rawIndices[k + 2];
                        if (i0 < vCount && i1 < vCount && i2 < vCount &&
                            i0 != i1 && i1 != i2 && i0 != i2) {
                            cleanIndices.push_back(i0);
                            cleanIndices.push_back(i1);
                            cleanIndices.push_back(i2);
                        }
                    }
                    if (!cleanIndices.empty()) {
                        SubMesh sub{};
                        sub.firstIndex = 0;
                        sub.indexCount = static_cast<uint32_t>(cleanIndices.size());
                        sub.textureHash = texHashes.empty() ? 0 : texHashes[0];
                        sub.shaderHash = shdHashes.empty() ? 0 : shdHashes[0];
                        sub.partName = partName;
                        cleanSubmeshes.push_back(sub);
                    }
                }

                if (!hadNormals && !cleanIndices.empty()) {
                    for (auto& v : partVertices) v.normal = Vec3(0.0f, 0.0f, 0.0f);
                    for (size_t k = 0; k + 2 < cleanIndices.size(); k += 3) {
                        uint32_t i0 = cleanIndices[k];
                        uint32_t i1 = cleanIndices[k + 1];
                        uint32_t i2 = cleanIndices[k + 2];
                        Vec3 e1 = partVertices[i1].pos - partVertices[i0].pos;
                        Vec3 e2 = partVertices[i2].pos - partVertices[i0].pos;
                        Vec3 fn = Vec3::cross(e1, e2);
                        partVertices[i0].normal = partVertices[i0].normal + fn;
                        partVertices[i1].normal = partVertices[i1].normal + fn;
                        partVertices[i2].normal = partVertices[i2].normal + fn;
                    }
                    for (auto& v : partVertices) {
                        if (v.normal.length() > 1e-5f) v.normal = v.normal.normalized();
                        else v.normal = Vec3(0.0f, 1.0f, 0.0f);
                    }
                }

                if (!cleanIndices.empty()) {
                    parts.push_back({partName, std::move(partVertices), std::move(cleanIndices),
                                     std::move(cleanSubmeshes), std::move(markers)});
                }
            }
            offset += 8 + size;
            continue;
        }

        offset = nextTopOffset;
    }
}

std::vector<CarPart> GeometryParser::ParseAllParts(const std::string& geomPath) {
    std::vector<CarPart> parts;
    std::ifstream file(geomPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[GeometryParser] Failed to open file: " << geomPath << std::endl;
        return parts;
    }

    std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(fileSize);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), fileSize)) {
        std::cerr << "[GeometryParser] Failed to read file data" << std::endl;
        return parts;
    }

    parsePartsFromBuffer(buffer.data(), buffer.size(), parts, 0);
    return parts;
}

struct WheelMount {
    Vec3 pos;       // In Viewer coords: X=Front, Y=Up, Z=Left
    bool isLeft;    // Left side (+Z) needs Y-mirroring in NFSU2 (Z-mirroring in viewer) so spokes face outward
    bool isFront;
};

static std::vector<WheelMount> findWheelMounts(const std::string& geomPath, const std::string& carPrefix) {
    // Default 350Z/Sports coupe wheel mounts in Viewer coordinates (X=Front, Y=Up, Z=Left)
    std::vector<WheelMount> mounts = {
        {Vec3( 1.35f, 0.33f,  0.76f), true,  true},  // Front Left
        {Vec3( 1.35f, 0.33f, -0.76f), false, true},  // Front Right
        {Vec3(-1.30f, 0.33f, -0.76f), false, false}, // Rear Right
        {Vec3(-1.30f, 0.33f,  0.76f), true,  false}  // Rear Left
    };

    if (carPrefix.empty()) return mounts;

    std::vector<std::string> globalCandidates = {
        "../GLOBAL/GLOBALB.BUN",
        "GLOBAL/GLOBALB.BUN",
        "/Volumes/KIOXIA/MagiPacks/Need for Speed - Underground 2/GLOBAL/GLOBALB.BUN"
    };
    auto slashPos = geomPath.find("/CARS/");
    if (slashPos != std::string::npos) {
        globalCandidates.insert(globalCandidates.begin(), geomPath.substr(0, slashPos) + "/GLOBAL/GLOBALB.BUN");
    }

    for (const auto& gpath : globalCandidates) {
        if (!std::filesystem::exists(gpath)) continue;
        std::ifstream f(gpath, std::ios::binary | std::ios::ate);
        if (!f.is_open()) continue;
        size_t sz = f.tellg();
        f.seekg(0, std::ios::beg);
        std::vector<uint8_t> buf(sz);
        f.read(reinterpret_cast<char*>(buf.data()), sz);

        std::string key = carPrefix;
        for (size_t i = 0; i + key.size() + 0x1D0 <= buf.size(); i += 16) {
            if (std::memcmp(&buf[i], key.c_str(), key.size()) == 0 && buf[i + key.size()] == 0) {
                const float* fl = reinterpret_cast<const float*>(&buf[i + 0x120]);
                const float* fr = reinterpret_cast<const float*>(&buf[i + 0x150]);
                const float* rr = reinterpret_cast<const float*>(&buf[i + 0x180]);
                const float* rl = reinterpret_cast<const float*>(&buf[i + 0x1B0]);

                if (std::abs(fl[0]) > 0.8f && std::abs(fl[0]) < 2.5f &&
                    std::abs(fl[1]) > 0.4f && std::abs(fl[1]) < 1.5f &&
                    fl[4] > 0.2f && fl[4] < 0.6f) {
                    mounts[0] = {Vec3(fl[0], fl[4], fl[1]), true,  true};
                    mounts[1] = {Vec3(fr[0], fr[4], fr[1]), false, true};
                    mounts[2] = {Vec3(rr[0], rr[4], rr[1]), false, false};
                    mounts[3] = {Vec3(rl[0], rl[4], rl[1]), true,  false};
                    return mounts;
                }
            }
        }
        break;
    }

    return mounts;
}

static void appendPartInstance(CarModel& model, const CarPart& part, const Vec3& offset,
                               bool mirrorZ, Vec3& minBound, Vec3& maxBound) {
    uint32_t vertexBase = static_cast<uint32_t>(model.vertices.size());
    uint32_t indexBase = static_cast<uint32_t>(model.indices.size());

    for (const auto& v : part.vertices) {
        Vertex tv = v;
        if (mirrorZ) {
            tv.pos.z = -tv.pos.z;
            tv.normal.z = -tv.normal.z;
        }
        tv.pos = tv.pos + offset;
        model.vertices.push_back(tv);

        minBound.x = std::min(minBound.x, tv.pos.x);
        minBound.y = std::min(minBound.y, tv.pos.y);
        minBound.z = std::min(minBound.z, tv.pos.z);
        maxBound.x = std::max(maxBound.x, tv.pos.x);
        maxBound.y = std::max(maxBound.y, tv.pos.y);
        maxBound.z = std::max(maxBound.z, tv.pos.z);
    }

    for (size_t k = 0; k + 2 < part.indices.size(); k += 3) {
        if (mirrorZ) {
            model.indices.push_back(vertexBase + part.indices[k]);
            model.indices.push_back(vertexBase + part.indices[k + 2]);
            model.indices.push_back(vertexBase + part.indices[k + 1]);
        } else {
            model.indices.push_back(vertexBase + part.indices[k]);
            model.indices.push_back(vertexBase + part.indices[k + 1]);
            model.indices.push_back(vertexBase + part.indices[k + 2]);
        }
    }

    for (const auto& sm : part.submeshes) {
        SubMesh tsm = sm;
        tsm.firstIndex += indexBase;
        model.submeshes.push_back(tsm);
    }
}

CarModel GeometryParser::AssembleCar(const std::string& geomPath, char targetLod) {
    CarModel model;
    std::vector<CarPart> allParts = ParseAllParts(geomPath);
    if (allParts.empty()) return model;

    std::string carPrefix;
    for (const auto& part : allParts) {
        auto pos = part.name.find("_BASE_");
        if (pos != std::string::npos) {
            carPrefix = part.name.substr(0, pos);
            break;
        }
    }
    model.carName = carPrefix;

    Vec3 minBound(1e9f, 1e9f, 1e9f);
    Vec3 maxBound(-1e9f, -1e9f, -1e9f);

    size_t assembledPartCount = 0;
    std::unordered_set<std::string> seenPartNames;

    const CarPart* frontWheelPart = nullptr;
    const CarPart* frontBrakePart = nullptr;
    const CarPart* rearBrakePart = nullptr;
    const CarPart* exhaustPart = nullptr;
    std::vector<Vec3> exhaustPositions;

    for (const auto& part : allParts) {
        const std::string& name = part.name;

        // Keep stock BASE and KIT00 parts across NFSU2, Most Wanted (2005), and Carbon (2006)
        bool isBase = (name.find("_BASE_") != std::string::npos || name.rfind("BASE_", 0) == 0);
        bool isKit00 = (name.find("_KIT00_") != std::string::npos || name.rfind("KIT00_", 0) == 0);
        if (!isBase && !isKit00) continue;

        // Exclude lower LODs (_B, _C, _D, _E)
        if (targetLod == 'A') {
            if (name.size() >= 2) {
                std::string tail = name.substr(name.size() - 2);
                if (tail == "_B" || tail == "_C" || tail == "_D" || tail == "_E") continue;
            }
        }

        // Because 28-char part names like 350Z_KIT00_HEADLIGHT_RIGHT_A truncate at 27 chars,
        // the first occurrence in the file is always LOD A and subsequent identical names are LOD B/C.
        if (seenPartNames.count(name)) continue;
        seenPartNames.insert(name);

        // Skip internal/unlit under-hood/trunk audio meshes that Z-fight with closed hood & trunk
        if (name.find("ENGINE") != std::string::npos ||
            name.find("TRUNK_AUDIO") != std::string::npos ||
            name.find("HOOD_UNDER") != std::string::npos ||
            name.find("TRUNK_UNDER") != std::string::npos ||
            name.find("DOOR_PANEL") != std::string::npos) {
            continue;
        }

        // Capture origin-centered instanced parts (wheels, brakes, exhaust) to place at their mount points
        if (name.find("FRONT_WHEEL") != std::string::npos) {
            frontWheelPart = &part;
            continue;
        }
        if (name.find("FRONT_BRAKE") != std::string::npos) {
            frontBrakePart = &part;
            continue;
        }
        if (name.find("REAR_BRAKE") != std::string::npos) {
            rearBrakePart = &part;
            continue;
        }
        if (name.find("EXHAUST") != std::string::npos) {
            exhaustPart = &part;
            continue;
        }

        if (name.find("REAR_BUMPER") != std::string::npos) {
            for (const auto& m : part.markers) {
                // LEFT_EXHAUST (0x69fdf59d) and RIGHT_EXHAUST (0xe2527054)
                if (m.hash == 0x69fdf59du || m.hash == 0xe2527054u) {
                    exhaustPositions.push_back(m.pos);
                }
            }
        }

        appendPartInstance(model, part, Vec3(0.0f, 0.0f, 0.0f), false, minBound, maxBound);
        assembledPartCount++;
    }

    // Fallback for standalone part libraries (e.g. WHEELS, SPOILER, BRAKES, EXHAUST, ROOF)
    // or 3D showroom rooms (FRONTEND/PLATFORMS/CarLot01.BIN, MegaloShop01.BIN)
    bool isShowroomScene = false;
    if (assembledPartCount == 0 && !frontWheelPart && !allParts.empty()) {
        if (geomPath.find("PLATFORMS") != std::string::npos ||
            geomPath.find("CarLot") != std::string::npos ||
            geomPath.find("Shop") != std::string::npos) {
            isShowroomScene = true;
            model.isMap = true; // Use environment material shading instead of car body paint
            for (const auto& part : allParts) {
                // Avoid Z-fighting between alternate turntable discs (keep DISC01, skip DISC02/DISC03)
                if (part.name == "DISC02" || part.name == "DISC03") continue;
                appendPartInstance(model, part, Vec3(0.0f, 0.0f, 0.0f), false, minBound, maxBound);
                assembledPartCount++;
            }
            model.carName = "Showroom";
        } else {
            appendPartInstance(model, allParts[0], Vec3(0.0f, 0.0f, 0.0f), false, minBound, maxBound);
            model.carName = allParts[0].name;
            assembledPartCount = 1;
        }
    } else {
        // Instance the 4 wheels and brakes at their exact wheel well positions
        auto wheelMounts = findWheelMounts(geomPath, carPrefix);
        for (const auto& wm : wheelMounts) {
            if (frontWheelPart) {
                // Adjust Z so outer rim sits flush with the fender arch
                float zAdjust = wm.isLeft ? 0.08f : -0.08f;
                Vec3 wheelPos(wm.pos.x, wm.pos.y, wm.pos.z + zAdjust);
                appendPartInstance(model, *frontWheelPart, wheelPos, wm.isLeft, minBound, maxBound);
                assembledPartCount++;
            }
            const CarPart* brakePart = wm.isFront ? frontBrakePart : (rearBrakePart ? rearBrakePart : frontBrakePart);
            if (brakePart) {
                appendPartInstance(model, *brakePart, wm.pos, wm.isLeft, minBound, maxBound);
                assembledPartCount++;
            }
        }

        // Instance exhaust tips at rear bumper markers
        if (exhaustPart && !exhaustPositions.empty()) {
            for (const auto& epos : exhaustPositions) {
                appendPartInstance(model, *exhaustPart, epos, false, minBound, maxBound);
                assembledPartCount++;
            }
        }
    }

    model.minBound = minBound;
    model.maxBound = maxBound;
    if (isShowroomScene) {
        // Keep showroom centered around its turntable at eye height (0, 1.2, 0) instead of shifting into the distant backdrop
        model.center = Vec3(0.0f, 1.2f, 0.0f);
    } else {
        model.center = (minBound + maxBound) * 0.5f;
    }

    // Center the model at origin
    for (auto& v : model.vertices) {
        v.pos -= model.center;
    }

    std::cout << "[GeometryParser] Assembled " << assembledPartCount << " clean parts for "
              << (carPrefix.empty() ? "Car" : carPrefix) << " (LOD " << targetLod
              << "). Vertices: " << model.vertices.size()
              << ", Triangles: " << model.getTriangleCount()
              << ", Submeshes: " << model.submeshes.size() << std::endl;

    return model;
}

CarModel GeometryParser::CreateTextureShowcase(const std::unordered_map<uint32_t, DecodedTexture>& textures, const std::string& label) {
    CarModel model;
    model.carName = label;
    model.isTextureShowcase = true;

    std::vector<const DecodedTexture*> sortedTex;
    sortedTex.reserve(textures.size());
    for (const auto& [hash, tex] : textures) {
        if (!tex.rgba.empty() && tex.width > 0 && tex.height > 0) {
            sortedTex.push_back(&tex);
        }
    }
    if (sortedTex.empty()) return model;

    std::sort(sortedTex.begin(), sortedTex.end(), [](const DecodedTexture* a, const DecodedTexture* b) {
        if (a->name != b->name) return a->name < b->name;
        return a->hash < b->hash;
    });

    if (sortedTex.size() > 64) {
        sortedTex.resize(64);
    }

    size_t count = sortedTex.size();
    int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    int rows = static_cast<int>((count + cols - 1) / cols);

    float totalSpan = 3.2f;
    float cellSize = totalSpan / static_cast<float>( std::max(cols, rows) );
    float pad = (count == 1) ? 0.0f : (cellSize * 0.08f);
    float quadMax = cellSize - pad;

    float gridW = cols * cellSize;
    float gridH = rows * cellSize;

    Vec3 minBound(1e9f, 1e9f, 1e9f);
    Vec3 maxBound(-1e9f, -1e9f, -1e9f);

    for (size_t i = 0; i < count; ++i) {
        const DecodedTexture* tex = sortedTex[i];
        int r = static_cast<int>(i) / cols;
        int c = static_cast<int>(i) % cols;

        float cx = -gridW * 0.5f + (c + 0.5f) * cellSize;
        float cy =  gridH * 0.5f - (r + 0.5f) * cellSize;

        float aspect = static_cast<float>(tex->width) / static_cast<float>(std::max(1u, tex->height));
        aspect = std::clamp(aspect, 0.25f, 4.0f);

        float w = quadMax;
        float h = quadMax;
        if (aspect > 1.0f) {
            h = quadMax / aspect;
        } else {
            w = quadMax * aspect;
        }

        float hw = w * 0.5f;
        float hh = h * 0.5f;

        uint32_t vBase = static_cast<uint32_t>(model.vertices.size());
        uint32_t iBase = static_cast<uint32_t>(model.indices.size());

        Vec3 p0(cx - hw, cy + hh, 0.0f);
        Vec3 p1(cx + hw, cy + hh, 0.0f);
        Vec3 p2(cx + hw, cy - hh, 0.0f);
        Vec3 p3(cx - hw, cy - hh, 0.0f);
        Vec3 n(0.0f, 0.0f, 1.0f);
        Vec4 col(1.0f, 1.0f, 1.0f, 1.0f);

        model.vertices.push_back({p0, n, col, Vec2(0.0f, 0.0f)});
        model.vertices.push_back({p1, n, col, Vec2(1.0f, 0.0f)});
        model.vertices.push_back({p2, n, col, Vec2(1.0f, 1.0f)});
        model.vertices.push_back({p3, n, col, Vec2(0.0f, 1.0f)});

        model.indices.push_back(vBase + 0);
        model.indices.push_back(vBase + 2);
        model.indices.push_back(vBase + 1);
        model.indices.push_back(vBase + 0);
        model.indices.push_back(vBase + 3);
        model.indices.push_back(vBase + 2);

        SubMesh sm;
        sm.firstIndex = iBase;
        sm.indexCount = 6;
        sm.textureHash = tex->hash;
        sm.shaderHash = 0x54455832u; // 'TEX2' showcase marker
        sm.partName = tex->name.empty() ? "TEXTURE_TILE" : tex->name;
        model.submeshes.push_back(sm);

        minBound.x = std::min(minBound.x, p0.x);
        minBound.y = std::min(minBound.y, p2.y);
        minBound.z = -0.01f;
        maxBound.x = std::max(maxBound.x, p1.x);
        maxBound.y = std::max(maxBound.y, p0.y);
        maxBound.z = 0.01f;
    }

    model.minBound = minBound;
    model.maxBound = maxBound;
    model.center = Vec3(0.0f, 0.0f, 0.0f);
    return model;
}

} // namespace NFSU2
