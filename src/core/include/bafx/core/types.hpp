#pragma once

#include <cstdint>

namespace bafx::core
{

struct Float3
{
    float r{0.0F};
    float g{0.0F};
    float b{0.0F};
};

struct RectI
{
    std::int32_t left{0};
    std::int32_t top{0};
    std::int32_t right{0};
    std::int32_t bottom{0};
};

}

