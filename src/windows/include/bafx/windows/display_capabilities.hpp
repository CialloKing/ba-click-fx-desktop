#pragma once

#include "bafx/windows/display_topology.hpp"

#include <windows.h>
#include <dxgicommon.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace bafx::windows
{

enum class DisplayColorMode : std::uint8_t
{
    Unknown,
    Sdr,
    WideColorGamut,
    Hdr
};

struct DisplayColorCapabilities final
{
    DXGI_COLOR_SPACE_TYPE colorSpace{DXGI_COLOR_SPACE_CUSTOM};
    std::uint32_t bitsPerColor{0U};
    float minimumLuminanceNits{0.0F};
    float maximumLuminanceNits{0.0F};
    float maximumFullFrameLuminanceNits{0.0F};
    bool luminanceMetadataValid{false};
    DisplayColorMode activeColorMode{DisplayColorMode::Unknown};
    LUID adapterLuid{};
    std::uint32_t targetId{0U};
    std::uint32_t physicalTargetCount{0U};
    DISPLAYCONFIG_COLOR_ENCODING colorEncoding{
        DISPLAYCONFIG_COLOR_ENCODING_FORCE_UINT32};
    std::uint32_t displayConfigBitsPerColorChannel{0U};
    float sdrWhiteLevelNits{0.0F};
    LONG advancedColorQueryResult{ERROR_NOT_SUPPORTED};
    LONG sdrWhiteLevelQueryResult{ERROR_NOT_SUPPORTED};
    bool displayPathResolved{false};
    bool advancedColorSupported{false};
    bool advancedColorActive{false};
    bool advancedColorLimitedByPolicy{false};
    bool highDynamicRangeSupported{false};
    bool highDynamicRangeUserEnabled{false};
    bool wideColorSupported{false};
    bool wideColorUserEnabled{false};
    bool sdrWhiteLevelValid{false};
    bool advancedColorInfoV2{false};
    bool advancedColorStateConsistent{false};
    bool sdrWhiteLevelConsistent{false};
    bool physicalTargetAdaptersConsistent{false};
};

// This is capability evidence only. It does not mean the application's final
// composition path has passed the HDR or Advanced Color validation matrix.
[[nodiscard]] std::optional<DisplayColorCapabilities>
queryDisplayColorCapabilities(HMONITOR monitor) noexcept;

// A DXGI-only snapshot is complete on older systems. Once DisplayConfig has
// resolved the physical path, every Advanced Color target must have answered
// consistently before the snapshot may replace a last-known output contract.
[[nodiscard]] bool displayColorStateComplete(
    const DisplayColorCapabilities& capabilities) noexcept;

[[nodiscard]] std::string_view displayColorModeName(
    DisplayColorMode mode) noexcept;

// The product currently targets the primary monitor only. DWM's rational
// composition cadence is more useful for latency budgets than a rounded DEVMODE Hz.
[[nodiscard]] std::optional<DisplayRefreshRate>
queryPrimaryCompositionRefreshRate() noexcept;

}
