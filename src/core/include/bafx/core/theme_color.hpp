#pragma once

#include "bafx/core/types.hpp"

#include <optional>
#include <string_view>

namespace bafx::core
{

// The renderer consumes linear Unity particle colors. Theme conversion crosses
// into ordinary sRGB only for the OKLCH calculation, then returns linear RGB.
struct RelativeOklchTheme final
{
    bool identity{true};
    bool invisible{false};
    float coverageScale{1.0F};
    float targetLightness{0.0F};
    float chromaScale{1.0F};
    float hueShiftRadians{0.0F};
};

// Parses the product's canonical #rrggbb sRGB contract. A failed parse keeps
// the renderer from silently replacing an invalid user choice with another hue.
[[nodiscard]] std::optional<RelativeOklchTheme> createRelativeOklchTheme(
    std::string_view themeColor) noexcept;

// Applies the Web renderer's relative-OKLCH relation to a linear Unity color.
// The default game blue is an explicit identity path to avoid Golden drift.
[[nodiscard]] Float3 applyRelativeOklchTheme(
    Float3 linearColor,
    const RelativeOklchTheme& theme) noexcept;

}
