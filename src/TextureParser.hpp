#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>

namespace NFSU2 {

struct DecodedTexture {
    uint32_t hash{0};
    std::string name;
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> rgba; // 32-bit RGBA uncompressed pixels
};

class TextureParser {
public:
    static std::unordered_map<uint32_t, DecodedTexture> LoadTextures(const std::string& binPath);
    static void LoadTexturesInto(const std::string& binPath, std::unordered_map<uint32_t, DecodedTexture>& outTextures);
    static int DecompressJDLZ(const uint8_t* in, int inSize, uint8_t* out, int outSize);
    static void DecodeDXT1(const uint8_t* dxtData, uint32_t width, uint32_t height, std::vector<uint8_t>& outRgba);
    static void DecodeDXT3(const uint8_t* dxtData, uint32_t width, uint32_t height, std::vector<uint8_t>& outRgba);
    static void DecodeDXT5(const uint8_t* dxtData, uint32_t width, uint32_t height, std::vector<uint8_t>& outRgba);
};

} // namespace NFSU2
