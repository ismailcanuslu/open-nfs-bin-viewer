#pragma once

#include "Math3D.hpp"
#include "TextureParser.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>

namespace NFSU2 {

struct Vertex {
    Vec3 pos;
    Vec3 normal;
    Vec4 color;
    Vec2 uv;
};

struct SubMesh {
    uint32_t firstIndex{0};
    uint32_t indexCount{0};
    uint32_t textureHash{0};
    uint32_t shaderHash{0};
    std::string partName;
};

struct PositionMarker {
    uint32_t hash{0};
    Vec3 pos;
};

struct CarPart {
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<SubMesh> submeshes;
    std::vector<PositionMarker> markers;
};

struct CarModel {
    std::string carName;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<SubMesh> submeshes;
    Vec3 minBound;
    Vec3 maxBound;
    Vec3 center;
    bool isMap{false};
    bool isTextureShowcase{false};

    size_t getTriangleCount() const { return indices.size() / 3; }
};

class GeometryParser {
public:
    static uint32_t BinHash(const std::string& str);
    static std::vector<CarPart> ParseAllParts(const std::string& geomPath);
    static CarModel AssembleCar(const std::string& geomPath, char targetLod = 'A');
    static CarModel CreateTextureShowcase(const std::unordered_map<uint32_t, DecodedTexture>& textures, const std::string& label);
};

} // namespace NFSU2
