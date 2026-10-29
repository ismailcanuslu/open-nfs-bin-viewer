#pragma once

#include "GeometryParser.hpp"
#include <string>

namespace NFSU2 {

class MapParser {
public:
    static CarModel LoadMapDistrict(const std::string& streamBunPath, size_t maxParts = 500);
};

} // namespace NFSU2
