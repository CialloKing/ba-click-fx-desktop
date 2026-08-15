#pragma once

#include "bafx/windows/fx_gpu_renderer.hpp"

#include <windows.h>

#include <cstdint>
#include <string>

namespace bafx::desktop
{

struct DisplayTarget
{
    HMONITOR monitor{nullptr};
    std::wstring deviceName{};
    RECT bounds{};
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
        && sameDisplayTarget(left.target, right.target);
}

[[nodiscard]] std::string displayTargetDeviceUtf8(
    const DisplayTarget& target);
[[nodiscard]] std::string formatDisplayTargetMonitor(
    const DisplayTarget& target);
[[nodiscard]] std::string formatDisplayTargetBounds(
    const DisplayTarget& target);

}
