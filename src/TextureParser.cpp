#include "TextureParser.hpp"
#include <fstream>
#include <iostream>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace NFSU2 {

int TextureParser::DecompressJDLZ(const uint8_t* in, int inSize, uint8_t* out, int outSize) {
    // Expects 'in' to point to the 16-byte JDLZ header ("JDLZ\x02\x10\x00\x00" + uncompSize + compSize)
    if (inSize < 18 || outSize <= 0) return -1;
    if (in[0] != 'J' || in[1] != 'D' || in[2] != 'L' || in[3] != 'Z') return -1;

    uint32_t f1 = static_cast<uint32_t>(in[16]) | 0x100u;
    uint32_t f2 = static_cast<uint32_t>(in[17]) | 0x100u;
    int pos = 18;
    int outPos = 0;

    while (pos < inSize && outPos < outSize) {
        if (f1 & 1u) {
            if (pos + 2 > inSize) break;
            uint8_t b0 = in[pos];
            uint8_t b1 = in[pos + 1];
            pos += 2;

            int length = 0;
            int dist = 0;
            if (f2 & 1u) {
                length = ((static_cast<int>(b0 & 0xF0) << 4) | b1) + 3;
                dist = (b0 & 0x0F) + 1;
            } else {
                dist = ((static_cast<int>(b0 & 0xE0) << 3) | b1) + 17;
                length = (b0 & 0x1F) + 3;
            }
            f2 >>= 1;

            if (outPos < dist) return -1;
            for (int k = 0; k < length && outPos < outSize; ++k) {
                out[outPos] = out[outPos - dist];
                outPos++;
            }
        } else {
            out[outPos++] = in[pos++];
        }

        f1 >>= 1;
        if (f1 == 1u && pos < inSize) {
            f1 = static_cast<uint32_t>(in[pos++]) | 0x100u;
        }
        if (f2 == 1u && pos < inSize) {
            f2 = static_cast<uint32_t>(in[pos++]) | 0x100u;
        }
    }
    return outPos;
}

static inline void rgb565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = ((c >> 11) & 0x1F) * 255 / 31;
    g = ((c >> 5) & 0x3F) * 255 / 63;
    b = (c & 0x1F) * 255 / 31;
}

void TextureParser::DecodeDXT1(const uint8_t* dxtData, uint32_t width, uint32_t height, std::vector<uint8_t>& outRgba) {
    outRgba.assign(width * height * 4, 255);
    size_t blockOffset = 0;

    for (uint32_t by = 0; by < height; by += 4) {
        for (uint32_t bx = 0; bx < width; bx += 4) {
            uint16_t c0 = *reinterpret_cast<const uint16_t*>(&dxtData[blockOffset]);
            uint16_t c1 = *reinterpret_cast<const uint16_t*>(&dxtData[blockOffset + 2]);
            uint32_t bits = *reinterpret_cast<const uint32_t*>(&dxtData[blockOffset + 4]);
            blockOffset += 8;

            uint8_t r[4], g[4], b[4], a[4];
            rgb565(c0, r[0], g[0], b[0]); a[0] = 255;
            rgb565(c1, r[1], g[1], b[1]); a[1] = 255;

            if (c0 > c1) {
                r[2] = (2 * r[0] + r[1]) / 3; g[2] = (2 * g[0] + g[1]) / 3; b[2] = (2 * b[0] + b[1]) / 3; a[2] = 255;
                r[3] = (r[0] + 2 * r[1]) / 3; g[3] = (g[0] + 2 * g[1]) / 3; b[3] = (b[0] + 2 * b[1]) / 3; a[3] = 255;
            } else {
                r[2] = (r[0] + r[1]) / 2;     g[2] = (g[0] + g[1]) / 2;     b[2] = (b[0] + b[1]) / 2;     a[2] = 255;
                r[3] = 0;                     g[3] = 0;                     b[3] = 0;                     a[3] = 255;
            }

            for (uint32_t y = 0; y < 4 && (by + y) < height; ++y) {
                for (uint32_t x = 0; x < 4 && (bx + x) < width; ++x) {
                    uint32_t idx = (bits >> (2 * (y * 4 + x))) & 3;
                    size_t pixelOffset = ((by + y) * width + (bx + x)) * 4;
                    outRgba[pixelOffset + 0] = r[idx];
                    outRgba[pixelOffset + 1] = g[idx];
                    outRgba[pixelOffset + 2] = b[idx];
                    outRgba[pixelOffset + 3] = a[idx];
                }
            }
        }
    }
}

void TextureParser::DecodeDXT3(const uint8_t* dxtData, uint32_t width, uint32_t height, std::vector<uint8_t>& outRgba) {
    outRgba.assign(width * height * 4, 255);
    size_t blockOffset = 0;

    for (uint32_t by = 0; by < height; by += 4) {
        for (uint32_t bx = 0; bx < width; bx += 4) {
            const uint8_t* alphaData = &dxtData[blockOffset];
            uint16_t c0 = *reinterpret_cast<const uint16_t*>(&dxtData[blockOffset + 8]);
            uint16_t c1 = *reinterpret_cast<const uint16_t*>(&dxtData[blockOffset + 10]);
            uint32_t bits = *reinterpret_cast<const uint32_t*>(&dxtData[blockOffset + 12]);
            blockOffset += 16;

            uint8_t r[4], g[4], b[4];
            rgb565(c0, r[0], g[0], b[0]);
            rgb565(c1, r[1], g[1], b[1]);

            r[2] = (2 * r[0] + r[1]) / 3; g[2] = (2 * g[0] + g[1]) / 3; b[2] = (2 * b[0] + b[1]) / 3;
            r[3] = (r[0] + 2 * r[1]) / 3; g[3] = (g[0] + 2 * g[1]) / 3; b[3] = (b[0] + 2 * b[1]) / 3;

            for (uint32_t y = 0; y < 4 && (by + y) < height; ++y) {
                uint16_t rowAlpha = *reinterpret_cast<const uint16_t*>(&alphaData[y * 2]);
                for (uint32_t x = 0; x < 4 && (bx + x) < width; ++x) {
                    uint32_t idx = (bits >> (2 * (y * 4 + x))) & 3;
                    uint8_t a4 = (rowAlpha >> (x * 4)) & 0xF;
                    uint8_t a8 = (a4 << 4) | a4;

                    size_t pixelOffset = ((by + y) * width + (bx + x)) * 4;
                    outRgba[pixelOffset + 0] = r[idx];
                    outRgba[pixelOffset + 1] = g[idx];
                    outRgba[pixelOffset + 2] = b[idx];
                    outRgba[pixelOffset + 3] = a8;
                }
            }
        }
    }
}

void TextureParser::DecodeDXT5(const uint8_t* dxtData, uint32_t width, uint32_t height, std::vector<uint8_t>& outRgba) {
    outRgba.assign(width * height * 4, 255);
    size_t blockOffset = 0;

    for (uint32_t by = 0; by < height; by += 4) {
        for (uint32_t bx = 0; bx < width; bx += 4) {
            uint8_t a0 = dxtData[blockOffset + 0];
            uint8_t a1 = dxtData[blockOffset + 1];
            uint64_t alphaBits = 0;
            for (int k = 0; k < 6; ++k) {
                alphaBits |= (static_cast<uint64_t>(dxtData[blockOffset + 2 + k]) << (8 * k));
            }

            uint8_t alphaLut[8];
            alphaLut[0] = a0;
            alphaLut[1] = a1;
            if (a0 > a1) {
                for (int k = 1; k <= 6; ++k) alphaLut[k + 1] = ((7 - k) * a0 + k * a1) / 7;
            } else {
                for (int k = 1; k <= 4; ++k) alphaLut[k + 1] = ((5 - k) * a0 + k * a1) / 5;
                alphaLut[6] = 0;
                alphaLut[7] = 255;
            }

            uint16_t c0 = *reinterpret_cast<const uint16_t*>(&dxtData[blockOffset + 8]);
            uint16_t c1 = *reinterpret_cast<const uint16_t*>(&dxtData[blockOffset + 10]);
            uint32_t bits = *reinterpret_cast<const uint32_t*>(&dxtData[blockOffset + 12]);
            blockOffset += 16;

            uint8_t r[4], g[4], b[4];
            rgb565(c0, r[0], g[0], b[0]);
            rgb565(c1, r[1], g[1], b[1]);
            r[2] = (2 * r[0] + r[1]) / 3; g[2] = (2 * g[0] + g[1]) / 3; b[2] = (2 * b[0] + b[1]) / 3;
            r[3] = (r[0] + 2 * r[1]) / 3; g[3] = (g[0] + 2 * g[1]) / 3; b[3] = (b[0] + 2 * b[1]) / 3;

            for (uint32_t y = 0; y < 4 && (by + y) < height; ++y) {
                for (uint32_t x = 0; x < 4 && (bx + x) < width; ++x) {
                    int pxIdx = y * 4 + x;
                    uint32_t cIdx = (bits >> (2 * pxIdx)) & 3;
                    uint32_t aIdx = (alphaBits >> (3 * pxIdx)) & 7;

                    size_t pixelOffset = ((by + y) * width + (bx + x)) * 4;
                    outRgba[pixelOffset + 0] = r[cIdx];
                    outRgba[pixelOffset + 1] = g[cIdx];
                    outRgba[pixelOffset + 2] = b[cIdx];
                    outRgba[pixelOffset + 3] = alphaLut[aIdx];
                }
            }
        }
    }
}

static bool decodeTextureEntry(const uint8_t* ti, const uint8_t* pf,
                               const uint8_t* buf, size_t bufSize, int64_t baseOffset,
                               DecodedTexture& outTex) {
    uint32_t nameHash = *reinterpret_cast<const uint32_t*>(&ti[0x24]);
    uint32_t imgOff   = *reinterpret_cast<const uint32_t*>(&ti[0x30]);
    uint32_t palOff   = *reinterpret_cast<const uint32_t*>(&ti[0x34]);
    uint32_t palSize  = *reinterpret_cast<const uint32_t*>(&ti[0x3C]);
    uint16_t width    = *reinterpret_cast<const uint16_t*>(&ti[0x44]);
    uint16_t height   = *reinterpret_cast<const uint16_t*>(&ti[0x46]);

    if (width == 0 || height == 0 || width > 4096 || height > 4096 || nameHash == 0) {
        return false;
    }

    uint32_t fmtCode = *reinterpret_cast<const uint32_t*>(&pf[0x14]);
    int64_t pixelStart = baseOffset + static_cast<int64_t>(imgOff);
    if (pixelStart < 0 || static_cast<size_t>(pixelStart) >= bufSize) {
        return false;
    }

    std::string texName;
    for (size_t k = 0x0C; k < 0x24; ++k) {
        char c = static_cast<char>(ti[k]);
        if (c == 0) break;
        if (c >= 32 && c <= 126) texName += c;
    }

    outTex.hash = nameHash;
    outTex.name = texName;
    outTex.width = width;
    outTex.height = height;

    const uint8_t* src = &buf[pixelStart];
    size_t avail = bufSize - static_cast<size_t>(pixelStart);

    constexpr uint32_t FOURCC_DXT1 = 0x31545844u;
    constexpr uint32_t FOURCC_DXT3 = 0x33545844u;
    constexpr uint32_t FOURCC_DXT5 = 0x35545844u;
    constexpr uint32_t D3DFMT_A8R8G8B8 = 21u;
    constexpr uint32_t D3DFMT_P8 = 41u;

    if (fmtCode == FOURCC_DXT1) {
        size_t req = std::max<size_t>(1, (width + 3) / 4) * std::max<size_t>(1, (height + 3) / 4) * 8;
        if (avail < req) return false;
        TextureParser::DecodeDXT1(src, width, height, outTex.rgba);
        return true;
    } else if (fmtCode == FOURCC_DXT3) {
        size_t req = std::max<size_t>(1, (width + 3) / 4) * std::max<size_t>(1, (height + 3) / 4) * 16;
        if (avail < req) return false;
        TextureParser::DecodeDXT3(src, width, height, outTex.rgba);
        return true;
    } else if (fmtCode == FOURCC_DXT5) {
        size_t req = std::max<size_t>(1, (width + 3) / 4) * std::max<size_t>(1, (height + 3) / 4) * 16;
        if (avail < req) return false;
        TextureParser::DecodeDXT5(src, width, height, outTex.rgba);
        return true;
    } else if (fmtCode == D3DFMT_A8R8G8B8) {
        size_t req = static_cast<size_t>(width) * height * 4;
        if (avail < req) return false;
        outTex.rgba.resize(req);
        // Convert BGRA -> RGBA
        for (size_t p = 0; p < static_cast<size_t>(width) * height; ++p) {
            outTex.rgba[p * 4 + 0] = src[p * 4 + 2];
            outTex.rgba[p * 4 + 1] = src[p * 4 + 1];
            outTex.rgba[p * 4 + 2] = src[p * 4 + 0];
            outTex.rgba[p * 4 + 3] = src[p * 4 + 3];
        }
        return true;
    } else if (fmtCode == D3DFMT_P8 && palSize >= 1024) {
        int64_t palStart = baseOffset + static_cast<int64_t>(palOff);
        size_t req = static_cast<size_t>(width) * height;
        if (avail < req || palStart < 0 || static_cast<size_t>(palStart) + 1024 > bufSize) return false;
        const uint8_t* pal = &buf[palStart];
        outTex.rgba.resize(req * 4);
        for (size_t p = 0; p < req; ++p) {
            uint8_t idx = src[p];
            outTex.rgba[p * 4 + 0] = pal[idx * 4 + 2];
            outTex.rgba[p * 4 + 1] = pal[idx * 4 + 1];
            outTex.rgba[p * 4 + 2] = pal[idx * 4 + 0];
            outTex.rgba[p * 4 + 3] = pal[idx * 4 + 3];
        }
        return true;
    }

    return false;
}

static void parseTpkContainer(const std::vector<uint8_t>& buffer, size_t b, size_t e,
                              std::unordered_map<uint32_t, DecodedTexture>& textures) {
    size_t infoB = 0, infoE = 0;
    size_t dataB = 0, dataE = 0;

    size_t off = b;
    while (off + 8 <= e) {
        uint32_t cid = *reinterpret_cast<const uint32_t*>(&buffer[off]);
        uint32_t sz  = *reinterpret_cast<const uint32_t*>(&buffer[off + 4]);
        if (off + 8 + sz > e) break;
        if (cid == 0xB3310000u) { infoB = off + 8; infoE = off + 8 + sz; }
        else if (cid == 0xB3320000u) { dataB = off + 8; dataE = off + 8 + sz; }
        off += 8 + sz;
    }
    if (!infoB || !dataB) return;

    size_t c3B = 0, c3E = 0;
    size_t c4B = 0, c4E = 0;
    size_t c5B = 0, c5E = 0;
    off = infoB;
    while (off + 8 <= infoE) {
        uint32_t cid = *reinterpret_cast<const uint32_t*>(&buffer[off]);
        uint32_t sz  = *reinterpret_cast<const uint32_t*>(&buffer[off + 4]);
        if (off + 8 + sz > infoE) break;
        if (cid == 0x33310003u) { c3B = off + 8; c3E = off + 8 + sz; }
        else if (cid == 0x33310004u) { c4B = off + 8; c4E = off + 8 + sz; }
        else if (cid == 0x33310005u) { c5B = off + 8; c5E = off + 8 + sz; }
        off += 8 + sz;
    }

    size_t pixBase = 0;
    off = dataB;
    while (off + 8 <= dataE) {
        uint32_t cid = *reinterpret_cast<const uint32_t*>(&buffer[off]);
        uint32_t sz  = *reinterpret_cast<const uint32_t*>(&buffer[off + 4]);
        if (off + 8 + sz > dataE) break;
        if (cid == 0x33320002u) {
            pixBase = ((off + 8) + 0x7Fu) & ~static_cast<size_t>(0x7Fu);
            break;
        }
        off += 8 + sz;
    }

    if (c4B && pixBase) {
        size_t count = (c4E - c4B) / 0x7C;
        for (size_t i = 0; i < count; ++i) {
            if (c5B + (i + 1) * 0x20 > c5E) break;
            const uint8_t* ti = &buffer[c4B + i * 0x7C];
            const uint8_t* pf = &buffer[c5B + i * 0x20];
            DecodedTexture tex;
            if (decodeTextureEntry(ti, pf, buffer.data(), buffer.size(), static_cast<int64_t>(pixBase), tex)) {
                textures[tex.hash] = std::move(tex);
            }
        }
    } else if (c3B) {
        size_t count = (c3E - c3B) / 24;
        for (size_t i = 0; i < count; ++i) {
            const uint32_t* rec = reinterpret_cast<const uint32_t*>(&buffer[c3B + i * 24]);
            uint32_t dataOff = rec[1];
            uint32_t compSize = rec[2];
            uint32_t uncompSize = rec[3];
            if (dataOff + compSize > buffer.size() || compSize < 18 || uncompSize < 0x9C) continue;
            if (buffer[dataOff] != 'J' || buffer[dataOff + 1] != 'D' ||
                buffer[dataOff + 2] != 'L' || buffer[dataOff + 3] != 'Z') {
                continue;
            }

            uint32_t jdlzUncomp = *reinterpret_cast<const uint32_t*>(&buffer[dataOff + 8]);
            if (jdlzUncomp < 0x9C || jdlzUncomp > 16 * 1024 * 1024) continue;

            std::vector<uint8_t> out(jdlzUncomp);
            int decBytes = TextureParser::DecompressJDLZ(&buffer[dataOff], static_cast<int>(compSize),
                                                         out.data(), static_cast<int>(jdlzUncomp));
            if (decBytes != static_cast<int>(jdlzUncomp)) continue;

            const uint8_t* ti = &out[out.size() - 0x9C];
            const uint8_t* pf = &out[out.size() - 0x20];
            int64_t imgOff = static_cast<int64_t>(*reinterpret_cast<const uint32_t*>(&ti[0x30]));

            DecodedTexture tex;
            if (decodeTextureEntry(ti, pf, out.data(), out.size(), -imgOff, tex)) {
                textures[tex.hash] = std::move(tex);
            }
        }
    }
}

static void walkForTpks(const std::vector<uint8_t>& buffer, size_t off, size_t end, int depth,
                        std::unordered_map<uint32_t, DecodedTexture>& textures) {
    while (off + 8 <= end) {
        uint32_t cid = *reinterpret_cast<const uint32_t*>(&buffer[off]);
        uint32_t sz  = *reinterpret_cast<const uint32_t*>(&buffer[off + 4]);
        if (sz > end - off - 8) break;
        if (cid == 0xB3300000u) {
            parseTpkContainer(buffer, off + 8, off + 8 + sz, textures);
        } else if ((cid & 0x80000000u) && depth < 6) {
            walkForTpks(buffer, off + 8, off + 8 + sz, depth + 1, textures);
        }
        off += 8 + sz;
    }
}

void TextureParser::LoadTexturesInto(const std::string& binPath,
                                     std::unordered_map<uint32_t, DecodedTexture>& outTextures) {
    std::ifstream file(binPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return;

    // Cap read to 24MB so large stream files load fast
    size_t fileSize = std::min<size_t>(static_cast<size_t>(file.tellg()), 24 * 1024 * 1024);
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(fileSize);
    file.read(reinterpret_cast<char*>(buffer.data()), fileSize);

    size_t before = outTextures.size();
    walkForTpks(buffer, 0, buffer.size(), 0, outTextures);

    // Map car DOORLINE texture to SKIN1 and body paint hash (0x3c84d757) if present
    const DecodedTexture* doorlineTex = nullptr;
    for (const auto& [h, tex] : outTextures) {
        if (tex.name.find("DOORLINE") != std::string::npos &&
            tex.name.find("MASK") == std::string::npos &&
            tex.name.find("WIDEBODY") == std::string::npos &&
            tex.name.find("KIT") == std::string::npos) {
            doorlineTex = &tex;
            break;
        }
    }
    if (doorlineTex) {
        DecodedTexture copyTex = *doorlineTex;
        outTextures[0x3c84d757u] = copyTex; // Body paint skin shader slot
    }

    std::cout << "[TextureParser] Decoded " << (outTextures.size() - before)
              << " textures from " << binPath << std::endl;
}

std::unordered_map<uint32_t, DecodedTexture> TextureParser::LoadTextures(const std::string& binPath) {
    std::unordered_map<uint32_t, DecodedTexture> textures;
    LoadTexturesInto(binPath, textures);
    return textures;
}

} // namespace NFSU2
