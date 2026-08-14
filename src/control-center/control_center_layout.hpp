#pragma once

#include <cstdint>

namespace bafx::control_center
{

inline constexpr int minimumControlCenterClientWidth = 860;
inline constexpr int minimumControlCenterClientHeight = 520;
inline constexpr int defaultControlCenterClientWidth = 960;
inline constexpr int defaultControlCenterClientHeight = 600;

struct PixelSize final
{
    int width{0};
    int height{0};
};

// Keep the monitor's requested DPI whenever its work area can contain the
// complete layout. Constrained displays use discrete 12-DPI steps so controls
// do not continuously change size while a monitor topology settles.
[[nodiscard]] std::uint32_t controlCenterLayoutDpi(
    PixelSize maximumClientSize,
    std::uint32_t monitorDpi) noexcept;

[[nodiscard]] PixelSize clampPixelSize(
    PixelSize desired,
    PixelSize available) noexcept;

}
