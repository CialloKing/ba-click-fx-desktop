#pragma once

#include "bafx/core/types.hpp"

#include <algorithm>
#include <cmath>

namespace bafx::core
{

[[nodiscard]] inline float srgbToLinearChannel(const float value) noexcept
{
    const float srgb = std::clamp(value, 0.0F, 1.0F);
    if (srgb <= 0.04045F)
    {
        return srgb / 12.92F;
    }
    return std::pow((srgb + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] inline Float3 srgbToLinear(const Float3 value) noexcept
{
    return Float3{
        srgbToLinearChannel(value.r),
        srgbToLinearChannel(value.g),
        srgbToLinearChannel(value.b)};
}

}
