#include "control_center_layout.hpp"

#include <algorithm>
#include <cstdint>

namespace bafx::control_center
{
namespace
{

constexpr std::uint32_t defaultDpi = 96U;
constexpr std::uint32_t compactDpiStep = 12U;

[[nodiscard]] std::uint32_t fittingDpi(
    const int pixels,
    const int logicalPixels) noexcept
{
    if (pixels <= 0 || logicalPixels <= 0)
    {
        return defaultDpi;
    }

    const auto scaled = static_cast<std::uint64_t>(pixels) * defaultDpi;
    return static_cast<std::uint32_t>(
        scaled / static_cast<std::uint64_t>(logicalPixels));
}

}

std::uint32_t controlCenterLayoutDpi(
    const PixelSize maximumClientSize,
    const std::uint32_t monitorDpi) noexcept
{
    const std::uint32_t requestedDpi = (std::max)(monitorDpi, defaultDpi);
    const std::uint32_t widthDpi = fittingDpi(
        maximumClientSize.width,
        minimumControlCenterClientWidth);
    const std::uint32_t heightDpi = fittingDpi(
        maximumClientSize.height,
        minimumControlCenterClientHeight);
    std::uint32_t fittedDpi = (std::min)({requestedDpi, widthDpi, heightDpi});
    fittedDpi = (std::max)(fittedDpi, defaultDpi);
    if (fittedDpi < requestedDpi)
    {
        fittedDpi = (fittedDpi / compactDpiStep) * compactDpiStep;
        fittedDpi = (std::max)(fittedDpi, defaultDpi);
    }
    return fittedDpi;
}

PixelSize clampPixelSize(
    const PixelSize desired,
    const PixelSize available) noexcept
{
    return PixelSize{
        (std::clamp)(desired.width, 1, (std::max)(1, available.width)),
        (std::clamp)(desired.height, 1, (std::max)(1, available.height))};
}

}
