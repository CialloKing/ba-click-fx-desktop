#pragma once

#include <windows.h>
#include <wingdi.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bafx::windows
{

enum class DisplayRefreshRateSource : std::uint8_t
{
    DwmCompositionTiming,
    DisplayConfigPath,
    DisplayConfigVirtualRefresh,
    DisplayConfigPhysicalRefresh,
    ConservativeFallback
};

struct DisplayRefreshRate final
{
    std::uint32_t numerator{0U};
    std::uint32_t denominator{0U};
    DisplayRefreshRateSource source{
        DisplayRefreshRateSource::DwmCompositionTiming};
};

enum class DisplayCaptureCadenceFallbackReason : std::uint8_t
{
    None,
    NoPhysicalTargets,
    PhysicalTargetUnavailable,
    DrrPhysicalRefreshRateUnavailable,
    InvalidEffectiveRefreshRate,
    MixedCloneRefreshRates
};

[[nodiscard]] constexpr std::string_view
displayCaptureCadenceFallbackReasonName(
    const DisplayCaptureCadenceFallbackReason reason) noexcept
{
    switch (reason)
    {
    case DisplayCaptureCadenceFallbackReason::None:
        return "none";
    case DisplayCaptureCadenceFallbackReason::NoPhysicalTargets:
        return "no-physical-targets";
    case DisplayCaptureCadenceFallbackReason::PhysicalTargetUnavailable:
        return "physical-target-unavailable";
    case DisplayCaptureCadenceFallbackReason::
        DrrPhysicalRefreshRateUnavailable:
        return "drr-physical-refresh-rate-unavailable";
    case DisplayCaptureCadenceFallbackReason::InvalidEffectiveRefreshRate:
        return "invalid-effective-refresh-rate";
    case DisplayCaptureCadenceFallbackReason::MixedCloneRefreshRates:
        return "mixed-clone-refresh-rates";
    }
    return "unknown";
}

[[nodiscard]] constexpr bool equivalentDisplayRefreshRate(
    const DisplayRefreshRate left,
    const DisplayRefreshRate right) noexcept
{
    if (left.numerator == 0U
        || left.denominator == 0U
        || right.numerator == 0U
        || right.denominator == 0U)
    {
        return false;
    }
    return static_cast<std::uint64_t>(left.numerator) * right.denominator
        == static_cast<std::uint64_t>(right.numerator) * left.denominator;
}

struct DisplayPhysicalTarget final
{
    LUID adapterLuid{};
    std::uint32_t targetId{0U};
    std::wstring friendlyName{};
    std::wstring devicePath{};
    DisplayRefreshRate refreshRate{};
    std::optional<DisplayRefreshRate> physicalRefreshRate{};
    std::optional<DisplayRefreshRate> captureRefreshRate{};
    DISPLAYCONFIG_ROTATION rotation{DISPLAYCONFIG_ROTATION_IDENTITY};
    DISPLAYCONFIG_SCALING scaling{DISPLAYCONFIG_SCALING_IDENTITY};
    DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY outputTechnology{
        DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER};
    bool available{false};
    bool dynamicRefreshRateBoosted{false};
};

struct DisplayCaptureCadenceResolution final
{
    std::optional<DisplayRefreshRate> refreshRate{};
    DisplayCaptureCadenceFallbackReason fallbackReason{
        DisplayCaptureCadenceFallbackReason::NoPhysicalTargets};
};

[[nodiscard]] std::optional<DisplayRefreshRate>
resolveDisplayPhysicalCaptureRefreshRate(
    const DisplayPhysicalTarget& target) noexcept;

[[nodiscard]] DisplayCaptureCadenceResolution resolveDisplayCaptureCadence(
    const std::vector<DisplayPhysicalTarget>& targets) noexcept;

struct ActiveDisplayMonitor final
{
    HMONITOR monitor{nullptr};
    std::wstring gdiDeviceName{};
    RECT bounds{};
    RECT workArea{};
    LUID sourceAdapterLuid{};
    std::uint32_t sourceId{0U};
    std::uint32_t dpiX{96U};
    std::uint32_t dpiY{96U};
    std::vector<DisplayPhysicalTarget> physicalTargets{};
    std::optional<DisplayRefreshRate> captureRefreshRate{};
    DisplayCaptureCadenceFallbackReason captureCadenceFallbackReason{
        DisplayCaptureCadenceFallbackReason::NoPhysicalTargets};
    bool primary{false};
    bool sourceAdapterResolved{false};
    bool sourceIdentityResolved{false};
};

enum class DisplayTopologyStatus : std::uint8_t
{
    Complete,
    Incomplete,
    NoActiveDisplays,
    QueryFailed
};

struct DisplayTopologySnapshot final
{
    DisplayTopologyStatus status{DisplayTopologyStatus::QueryFailed};
    LONG error{ERROR_GEN_FAILURE};
    std::vector<ActiveDisplayMonitor> displays{};
};

[[nodiscard]] DisplayTopologySnapshot queryActiveDisplayTopology() noexcept;

[[nodiscard]] const ActiveDisplayMonitor* findDisplayMonitor(
    const DisplayTopologySnapshot& snapshot,
    HMONITOR monitor) noexcept;

[[nodiscard]] std::optional<DisplayRefreshRate> queryDisplayRefreshRate(
    HMONITOR monitor) noexcept;

}
