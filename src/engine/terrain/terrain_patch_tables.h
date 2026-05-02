#pragma once
#include <cstdint>
#include <cstddef>

namespace tables {
    constexpr int32_t kNumVec4 = 2;
    extern const char* s_shaderOptions[];

    extern const float s_verticesL0[];
    extern const uint32_t s_indexesL0[];

    extern const float s_verticesL1[];
    extern const uint32_t s_indexesL1[];

    extern const float s_verticesL2[];
    extern const uint32_t s_indexesL2[];

    extern const float s_verticesL3[];
    extern const uint32_t s_indexesL3[];
} // namespace tables
