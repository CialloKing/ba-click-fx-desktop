#pragma once

#include "bafx/core/types.hpp"

#include <algorithm>
#include <cmath>

namespace bafx::core
{

inline constexpr float halfFloatMaximum = 65504.0F;

// Unity's Bloom inspector values are Gamma-domain values, including HDR
// values above one. Keep this branch separate from the display-range sRGB
// helper below, which intentionally clamps input to one.
[[nodiscard]] inline float unityGammaToLinearChannel(const float value) noexcept
{
    const float gamma = std::max(value, 0.0F);
    float linear = 0.0F;
    if (gamma <= 0.04045F)
    {
        linear = gamma / 12.92F;
    }
    else if (gamma < 1.0F)
    {
        linear = std::pow((gamma + 0.055F) / 1.055F, 2.4F);
    }
    else
    {
        linear = std::pow(gamma, 2.2F);
    }
    return std::clamp(linear, 0.0F, halfFloatMaximum);
}

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
