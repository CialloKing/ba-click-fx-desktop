#pragma once

#include <windows.h>
#include <dxgicommon.h>

#include <cstdint>
#include <optional>

namespace bafx::windows
{

struct DisplayColorCapabilities final
{
    DXGI_COLOR_SPACE_TYPE colorSpace{DXGI_COLOR_SPACE_CUSTOM};
    std::uint32_t bitsPerColor{0U};
    float minimumLuminanceNits{0.0F};
    float maximumLuminanceNits{0.0F};
    float maximumFullFrameLuminanceNits{0.0F};
    bool luminanceMetadataValid{false};
};

struct DisplayRefreshRate final
{
    std::uint32_t numerator{0U};
    std::uint32_t denominator{0U};
};

// This is capability evidence only. It does not mean the application's final
// composition path has passed the HDR or Advanced Color validation matrix.
[[nodiscard]] std::optional<DisplayColorCapabilities>
queryDisplayColorCapabilities(HMONITOR monitor) noexcept;

// The product currently targets the primary monitor only. DWM's rational
// composition cadence is more useful for latency budgets than a rounded DEVMODE Hz.
[[nodiscard]] std::optional<DisplayRefreshRate>
queryPrimaryCompositionRefreshRate() noexcept;

}
