#include "MapParser.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace NFSU2 {

CarModel MapParser::LoadMapDistrict(const std::string& streamBunPath, size_t maxParts) {
    CarModel model;
    model.isMap = true;
    model.carName = "Bayview District";

    std::vector<CarPart> allParts = GeometryParser::ParseAllParts(streamBunPath);
    if (allParts.empty()) {
        std::cerr << "[MapParser] Failed to parse map parts from: " << streamBunPath << std::endl;
        return model;
    }

    Vec3 minBound(1e9f, 1e9f, 1e9f);
    Vec3 maxBound(-1e9f, -1e9f, -1e9f);
    size_t parsedParts = 0;

    // Pick anchor position from the first world-space road mesh (TRN_..._ROAD)
    Vec3 anchorPos(0.0f, 0.0f, 0.0f);
    bool hasAnchor = false;
    for (const auto& part : allParts) {
        if (part.name.find("ROAD01") != std::string::npos && !part.vertices.empty()) {
            const Vec3& p = part.vertices[0].pos;
            if (std::abs(p.x) > 100.0f || std::abs(p.z) > 100.0f) {
                anchorPos = p;
                hasAnchor = true;
                break;
            }
        }
    }

    for (const auto& part : allParts) {
        if (parsedParts >= maxParts) break;
        const std::string& name = part.name;

        // Skip origin-centered instanced prop templates (XB_, XO_, XS_, XU_), skydome, start/finish banners, and low-LOD duplicates
        if (name.rfind("XB_", 0) == 0 ||
            name.rfind("XO_", 0) == 0 ||
            name.rfind("XS_", 0) == 0 ||
            name.rfind("XU_", 0) == 0 ||
            name.find("SKYDOME") != std::string::npos ||
            name.find("STARTFIN") != std::string::npos ||
            name.find("TEMP_") != std::string::npos ||
            name.find("DRIFTMARKER") != std::string::npos ||
            name.find("DRIFTSZ") != std::string::npos ||
            name.find("TERRAIN02") != std::string::npos ||
            name.find("TERRAIN03") != std::string::npos ||
            name.find("_1Z") != std::string::npos ||
            name.find("_1B") != std::string::npos) {
            continue;
        }

        // Only keep world-space road/terrain/scenery chunks (TRN_ or L4R)
        if (name.rfind("TRN_", 0) != 0 && name.rfind("L4R", 0) != 0) {
            continue;
        }

        // Skip low-LOD pan meshes that don't have _R (reflection/high-detail road/terrain) if it's a CHOP mesh
        if (name.find("_CHOP_") != std::string::npos && name.find("_R") == std::string::npos) {
            continue;
        }

        if (part.vertices.empty()) continue;

        // Keep parts within 800m of our anchor neighborhood so the camera frames a dense, cohesive city sector
        if (hasAnchor) {
            Vec3 d = part.vertices[0].pos - anchorPos;
            float distXZ = std::sqrt(d.x * d.x + d.z * d.z);
            if (distXZ > 750.0f) continue;
        }

        uint32_t vertexBase = static_cast<uint32_t>(model.vertices.size());
        uint32_t indexBase = static_cast<uint32_t>(model.indices.size());

        for (const auto& v : part.vertices) {
            model.vertices.push_back(v);
            minBound.x = std::min(minBound.x, v.pos.x);
            minBound.y = std::min(minBound.y, v.pos.y);
            minBound.z = std::min(minBound.z, v.pos.z);
            maxBound.x = std::max(maxBound.x, v.pos.x);
            maxBound.y = std::max(maxBound.y, v.pos.y);
            maxBound.z = std::max(maxBound.z, v.pos.z);
        }

        for (uint32_t idx : part.indices) {
            model.indices.push_back(vertexBase + idx);
        }

        for (const auto& sm : part.submeshes) {
            SubMesh tsm = sm;
            tsm.firstIndex += indexBase;
            model.submeshes.push_back(tsm);
        }

        parsedParts++;
    }

    if (model.vertices.empty()) return model;

    model.minBound = minBound;
    model.maxBound = maxBound;
    model.center = (minBound + maxBound) * 0.5f;

    // Scale down large city coordinates by 0.015 so it fits comfortably in the showroom camera frustum
    float mapScale = 0.015f;
    for (auto& v : model.vertices) {
        v.pos = (v.pos - model.center) * mapScale;
    }
    model.minBound = (minBound - model.center) * mapScale;
    model.maxBound = (maxBound - model.center) * mapScale;
    model.center = Vec3(0.0f, 0.0f, 0.0f);

    std::cout << "[MapParser] Loaded " << parsedParts << " clean Bayview road/terrain chunks from "
              << streamBunPath << ". Vertices: " << model.vertices.size()
              << ", Triangles: " << model.getTriangleCount()
              << ", Submeshes: " << model.submeshes.size() << std::endl;

    return model;
}

} // namespace NFSU2
