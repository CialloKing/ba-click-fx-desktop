#include "display_target.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <new>
#include <sstream>
#include <string>
#include <utility>

namespace bafx::desktop
{
namespace
{

[[nodiscard]] bool validBounds(const RECT& bounds) noexcept
{
    return bounds.right > bounds.left && bounds.bottom > bounds.top;
}

[[nodiscard]] bool validRefreshRate(
    const bafx::windows::DisplayRefreshRate refreshRate) noexcept
{
    if (refreshRate.numerator == 0U || refreshRate.denominator == 0U)
    {
        return false;
    }
    const double hertz = static_cast<double>(refreshRate.numerator)
        / static_cast<double>(refreshRate.denominator);
    return std::isfinite(hertz) && hertz >= 1.0 && hertz <= 1000.0;
}

[[nodiscard]] std::optional<bafx::windows::DisplayRefreshRate>
commonRefreshRate(const bafx::windows::ActiveDisplayMonitor& display) noexcept
{
    if (display.physicalTargets.empty())
    {
        return std::nullopt;
    }
    const bafx::windows::DisplayRefreshRate refreshRate =
        display.physicalTargets.front().refreshRate;
    if (!validRefreshRate(refreshRate))
    {
        return std::nullopt;
    }
    for (const bafx::windows::DisplayPhysicalTarget& target :
         display.physicalTargets)
    {
        if (!bafx::windows::equivalentDisplayRefreshRate(
                target.refreshRate,
                refreshRate))
        {
            return std::nullopt;
        }
    }
    return refreshRate;
}

}

std::string displayTargetDeviceUtf8(const DisplayTarget& target)
{
    if (target.deviceName.empty())
    {
        return "unknown";
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        target.deviceName.data(),
        static_cast<int>(target.deviceName.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0)
    {
        return "invalid-utf8";
    }

    std::string converted(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        target.deviceName.data(),
        static_cast<int>(target.deviceName.size()),
        converted.data(),
        required,
        nullptr,
        nullptr);
    return written == required ? converted : "invalid-utf8";
}

std::string formatDisplayTargetMonitor(const DisplayTarget& target)
{
    std::ostringstream stream;
    stream << "0x"
           << std::uppercase
           << std::hex
           << std::setw(static_cast<int>(sizeof(std::uintptr_t) * 2U))
           << std::setfill('0')
           << reinterpret_cast<std::uintptr_t>(target.monitor);
    return stream.str();
}

std::string formatDisplayTargetBounds(const DisplayTarget& target)
{
    std::ostringstream stream;
    stream << target.bounds.right - target.bounds.left
           << 'x'
           << target.bounds.bottom - target.bounds.top
           << '@'
           << target.bounds.left
           << ','
           << target.bounds.top;
    return stream.str();
}

DisplayTargetSnapshot queryDisplayTargets() noexcept
{
    DisplayTargetSnapshot result{};
    const bafx::windows::DisplayTopologySnapshot topology =
        bafx::windows::queryActiveDisplayTopology();
    result.status = topology.status;
    result.error = topology.error;
    try
    {
        result.displays.reserve(topology.displays.size());
        for (const bafx::windows::ActiveDisplayMonitor& display :
             topology.displays)
        {
            if (!validBounds(display.bounds))
            {
                result.status =
                    bafx::windows::DisplayTopologyStatus::Incomplete;
                if (result.error == ERROR_SUCCESS)
                {
                    result.error = ERROR_INVALID_DATA;
                }
                continue;
            }

            DisplayTarget target{};
            target.monitor = display.monitor;
            target.deviceName = display.gdiDeviceName;
            target.bounds = display.bounds;
            target.sourceAdapterLuid = display.sourceAdapterLuid;
            target.sourceId = display.sourceId;
            target.dpiX = display.dpiX;
            target.dpiY = display.dpiY;
            target.refreshRate = commonRefreshRate(display);
            target.captureRefreshRate = display.captureRefreshRate;
            target.physicalTargetCount = display.physicalTargets.size();
            target.primary = display.primary;
            target.sourceAdapterResolved = display.sourceAdapterResolved;
            target.sourceIdentityResolved = display.sourceIdentityResolved;
            target.physicalTargetIdentities.reserve(
                display.physicalTargets.size());
            for (const bafx::windows::DisplayPhysicalTarget& physicalTarget :
                 display.physicalTargets)
            {
                target.physicalTargetIdentities.push_back(
                    DisplayPhysicalTargetIdentity{
                        physicalTarget.adapterLuid,
                        physicalTarget.targetId,
                        physicalTarget.devicePath,
                        physicalTarget.rotation,
                        physicalTarget.scaling,
                        physicalTarget.outputTechnology,
                        physicalTarget.available});
            }
            std::sort(
                target.physicalTargetIdentities.begin(),
                target.physicalTargetIdentities.end(),
                [](const DisplayPhysicalTargetIdentity& left,
                   const DisplayPhysicalTargetIdentity& right) noexcept
                {
                    if (left.adapterLuid.HighPart
                        != right.adapterLuid.HighPart)
                    {
                        return left.adapterLuid.HighPart
                            < right.adapterLuid.HighPart;
                    }
                    if (left.adapterLuid.LowPart
                        != right.adapterLuid.LowPart)
                    {
                        return left.adapterLuid.LowPart
                            < right.adapterLuid.LowPart;
                    }
                    if (left.targetId != right.targetId)
                    {
                        return left.targetId < right.targetId;
                    }
                    // DisplayConfig target IDs identify adapter endpoints and
                    // may be reused when a different panel replaces the old one.
                    return left.devicePath < right.devicePath;
                });
            result.displays.push_back(std::move(target));
        }
    }
    catch (const std::bad_alloc&)
    {
        result.status = bafx::windows::DisplayTopologyStatus::QueryFailed;
        result.error = ERROR_OUTOFMEMORY;
        result.displays.clear();
    }
    catch (...)
    {
        result.status = bafx::windows::DisplayTopologyStatus::QueryFailed;
        result.error = ERROR_GEN_FAILURE;
        result.displays.clear();
    }
    return result;
}

const DisplayTarget* findPrimaryDisplayTarget(
    const DisplayTargetSnapshot& snapshot) noexcept
{
    const auto found = std::find_if(
        snapshot.displays.begin(),
        snapshot.displays.end(),
        [](const DisplayTarget& display)
        {
            return display.primary;
        });
    if (found != snapshot.displays.end())
    {
        return &*found;
    }
    return snapshot.displays.empty() ? nullptr : &snapshot.displays.front();
}

const DisplayTarget* findDisplayTarget(
    const DisplayTargetSnapshot& snapshot,
    const HMONITOR monitor) noexcept
{
    const auto found = std::find_if(
        snapshot.displays.begin(),
        snapshot.displays.end(),
        [monitor](const DisplayTarget& display)
        {
            return display.monitor == monitor;
        });
    return found == snapshot.displays.end() ? nullptr : &*found;
}

const DisplayTarget* findDisplayTargetBySource(
    const DisplayTargetSnapshot& snapshot,
    const DisplayTarget& reference) noexcept
{
    const auto found = std::find_if(
        snapshot.displays.begin(),
        snapshot.displays.end(),
        [&reference](const DisplayTarget& display)
        {
            return sameDisplaySource(reference, display);
        });
    return found == snapshot.displays.end() ? nullptr : &*found;
}

const DisplayTarget* findDisplayTargetAtPoint(
    const DisplayTargetSnapshot& snapshot,
    const POINT point) noexcept
{
    const HMONITOR monitor = MonitorFromPoint(
        point,
        MONITOR_DEFAULTTONEAREST);
    return findDisplayTarget(snapshot, monitor);
}

DisplayTarget stabilizeDisplayTargetObservation(
    const DisplayTarget& previous,
    const DisplayTarget& observed,
    const bafx::windows::DisplayTopologyStatus topologyStatus)
{
    DisplayTarget stabilized = observed;
    if (topologyStatus
            == bafx::windows::DisplayTopologyStatus::Complete
        || !previous.sourceAdapterResolved
        || previous.deviceName != observed.deviceName)
    {
        return stabilized;
    }

    const bool sourceAdapterChanged = observed.sourceAdapterResolved
        && (observed.sourceAdapterLuid.HighPart
                != previous.sourceAdapterLuid.HighPart
            || observed.sourceAdapterLuid.LowPart
                != previous.sourceAdapterLuid.LowPart);
    if (sourceAdapterChanged)
    {
        // A unique DXGI output mapping is sufficient evidence that the old GPU
        // domain is stale even while DisplayConfig has not recovered sourceId.
        return stabilized;
    }
    const bool sourceIdentityChanged = previous.sourceIdentityResolved
        && observed.sourceIdentityResolved
        && (previous.sourceAdapterLuid.HighPart
                != observed.sourceAdapterLuid.HighPart
            || previous.sourceAdapterLuid.LowPart
                != observed.sourceAdapterLuid.LowPart
            || previous.sourceId != observed.sourceId);
    if (sourceIdentityChanged)
    {
        return stabilized;
    }

    // EnumDisplayMonitors remains authoritative for screen geometry while
    // QueryDisplayConfig can temporarily omit the GPU source during hot-plug.
    // Retain the last resolved GPU domain until a complete observation can
    // prove that the display source changed.
    const bool sourceAdapterResolutionRegressed =
        !observed.sourceAdapterResolved;
    if (sourceAdapterResolutionRegressed)
    {
        stabilized.sourceAdapterLuid = previous.sourceAdapterLuid;
        stabilized.sourceAdapterResolved = true;
    }
    const bool sourceIdentityResolutionRegressed =
        previous.sourceIdentityResolved
        && !observed.sourceIdentityResolved;
    if (sourceIdentityResolutionRegressed)
    {
        stabilized.sourceId = previous.sourceId;
        stabilized.sourceIdentityResolved = true;
    }

    const bool physicalTargetsRegressed =
        observed.physicalTargetCount < previous.physicalTargetCount
        || observed.physicalTargetIdentities.size()
            < previous.physicalTargetIdentities.size();
    if (sourceAdapterResolutionRegressed
        || sourceIdentityResolutionRegressed
        || physicalTargetsRegressed)
    {
        // A cloned path can disappear from only the partial snapshot. Its
        // cadence and endpoint set remain authoritative until a complete
        // query confirms an actual clone removal.
        stabilized.refreshRate = previous.refreshRate;
        stabilized.captureRefreshRate = previous.captureRefreshRate;
        stabilized.physicalTargetCount = previous.physicalTargetCount;
        stabilized.physicalTargetIdentities =
            previous.physicalTargetIdentities;
    }
    return stabilized;
}

}
