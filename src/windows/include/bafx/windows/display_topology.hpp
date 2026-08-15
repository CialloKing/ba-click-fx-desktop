#pragma once

#include <windows.h>
#include <wingdi.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bafx::windows
{

enum class DisplayRefreshRateSource : std::uint8_t
{
    DwmCompositionTiming,
    DisplayConfigPath,
    DisplayConfigVirtualRefresh,
    DisplayConfigPhysicalRefresh
};

struct DisplayRefreshRate final
{
    std::uint32_t numerator{0U};
    std::uint32_t denominator{0U};
    DisplayRefreshRateSource source{
        DisplayRefreshRateSource::DwmCompositionTiming};
};

struct DisplayPhysicalTarget final
{
    LUID adapterLuid{};
    std::uint32_t targetId{0U};
    std::wstring friendlyName{};
    std::wstring devicePath{};
    DisplayRefreshRate refreshRate{};
    std::optional<DisplayRefreshRate> physicalRefreshRate{};
    DISPLAYCONFIG_ROTATION rotation{DISPLAYCONFIG_ROTATION_IDENTITY};
    DISPLAYCONFIG_SCALING scaling{DISPLAYCONFIG_SCALING_IDENTITY};
    DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY outputTechnology{
        DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER};
    bool available{false};
    bool dynamicRefreshRateBoosted{false};
};

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
    bool primary{false};
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
