#pragma once

#include "bafx/windows/display_topology.hpp"
#include "bafx/windows/fx_gpu_renderer.hpp"

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bafx::desktop
{

struct DisplayTarget
{
    HMONITOR monitor{nullptr};
    std::wstring deviceName{};
    RECT bounds{};
    LUID sourceAdapterLuid{};
    std::uint32_t sourceId{0U};
    std::uint32_t dpiX{96U};
    std::uint32_t dpiY{96U};
    std::optional<bafx::windows::DisplayRefreshRate> refreshRate{};
    std::optional<bafx::windows::DisplayRefreshRate> captureRefreshRate{};
    std::size_t physicalTargetCount{0U};
    bool primary{false};
    bool sourceIdentityResolved{false};
};

struct DisplayTargetSnapshot
{
    bafx::windows::DisplayTopologyStatus status{
        bafx::windows::DisplayTopologyStatus::QueryFailed};
    LONG error{ERROR_GEN_FAILURE};
    std::vector<DisplayTarget> displays{};
};

[[nodiscard]] inline bool sameDisplayBounds(
    const RECT& left,
    const RECT& right) noexcept
{
    return left.left == right.left
        && left.top == right.top
        && left.right == right.right
        && left.bottom == right.bottom;
}

[[nodiscard]] inline bool sameDisplayTarget(
    const DisplayTarget& left,
    const DisplayTarget& right) noexcept
{
    return left.monitor == right.monitor
        && left.deviceName == right.deviceName
        && sameDisplayBounds(left.bounds, right.bounds);
}

[[nodiscard]] inline bool sameDisplaySource(
    const DisplayTarget& left,
    const DisplayTarget& right) noexcept
{
    if (left.sourceIdentityResolved && right.sourceIdentityResolved)
    {
        return left.sourceAdapterLuid.HighPart
                == right.sourceAdapterLuid.HighPart
            && left.sourceAdapterLuid.LowPart
                == right.sourceAdapterLuid.LowPart
            && left.sourceId == right.sourceId;
    }
    return left.deviceName == right.deviceName;
}

[[nodiscard]] inline bool sameDisplaySourceIdentity(
    const DisplayTarget& left,
    const DisplayTarget& right) noexcept
{
    if (left.sourceIdentityResolved != right.sourceIdentityResolved)
    {
        return false;
    }
    return !left.sourceIdentityResolved
        || (left.sourceAdapterLuid.HighPart
                == right.sourceAdapterLuid.HighPart
            && left.sourceAdapterLuid.LowPart
                == right.sourceAdapterLuid.LowPart
            && left.sourceId == right.sourceId);
}

[[nodiscard]] inline bafx::windows::WindowSize displayTargetSize(
    const DisplayTarget& target) noexcept
{
    return bafx::windows::WindowSize{
        static_cast<std::uint32_t>(target.bounds.right - target.bounds.left),
        static_cast<std::uint32_t>(target.bounds.bottom - target.bounds.top)};
}

struct DisplayTargetIntent
{
    DisplayTarget target{};
    bool applyBounds{false};
};

[[nodiscard]] inline bool sameDisplayTargetIntent(
    const DisplayTargetIntent& left,
    const DisplayTargetIntent& right) noexcept
{
    return left.applyBounds == right.applyBounds
        && sameDisplayTarget(left.target, right.target)
        && sameDisplaySourceIdentity(left.target, right.target);
}

[[nodiscard]] std::string displayTargetDeviceUtf8(
    const DisplayTarget& target);
[[nodiscard]] std::string formatDisplayTargetMonitor(
    const DisplayTarget& target);
[[nodiscard]] std::string formatDisplayTargetBounds(
    const DisplayTarget& target);

[[nodiscard]] DisplayTargetSnapshot queryDisplayTargets() noexcept;
[[nodiscard]] const DisplayTarget* findPrimaryDisplayTarget(
    const DisplayTargetSnapshot& snapshot) noexcept;
[[nodiscard]] const DisplayTarget* findDisplayTarget(
    const DisplayTargetSnapshot& snapshot,
    HMONITOR monitor) noexcept;
[[nodiscard]] const DisplayTarget* findDisplayTargetBySource(
    const DisplayTargetSnapshot& snapshot,
    const DisplayTarget& reference) noexcept;
[[nodiscard]] const DisplayTarget* findDisplayTargetAtPoint(
    const DisplayTargetSnapshot& snapshot,
    POINT point) noexcept;

}
