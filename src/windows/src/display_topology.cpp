#include "bafx/windows/display_topology.hpp"

#include <shellscalingapi.h>

#include <algorithm>
#include <cwchar>
#include <limits>
#include <new>
#include <utility>

namespace bafx::windows
{
namespace
{

struct MonitorEnumeration
{
    std::vector<ActiveDisplayMonitor> displays{};
    LONG error{ERROR_SUCCESS};
};

[[nodiscard]] bool sameLuid(const LUID left, const LUID right) noexcept
{
    return left.HighPart == right.HighPart
        && left.LowPart == right.LowPart;
}

[[nodiscard]] bool validRefreshRate(
    const DisplayRefreshRate refreshRate) noexcept
{
    if (refreshRate.numerator == 0U || refreshRate.denominator == 0U)
    {
        return false;
    }
    const double hertz = static_cast<double>(refreshRate.numerator)
        / static_cast<double>(refreshRate.denominator);
    return hertz >= 1.0 && hertz <= 1000.0;
}

[[nodiscard]] std::optional<DisplayRefreshRate> physicalRefreshRate(
    const DISPLAYCONFIG_PATH_INFO& path,
    const std::vector<DISPLAYCONFIG_MODE_INFO>& modes) noexcept
{
    constexpr UINT pathSupportsVirtualMode = 0x00000008U;
    const bool virtualMode = (path.flags & pathSupportsVirtualMode) != 0U;
    const UINT32 modeIndex = virtualMode
        ? path.targetInfo.targetModeInfoIdx
        : path.targetInfo.modeInfoIdx;
    const UINT32 invalidIndex = virtualMode
        ? 0x0000FFFFU
        : 0xFFFFFFFFU;
    if (modeIndex == invalidIndex || modeIndex >= modes.size())
    {
        return std::nullopt;
    }

    const DISPLAYCONFIG_MODE_INFO& mode = modes[modeIndex];
    if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET
        || !sameLuid(mode.adapterId, path.targetInfo.adapterId)
        || mode.id != path.targetInfo.id)
    {
        return std::nullopt;
    }

    const DISPLAYCONFIG_RATIONAL& physical =
        mode.targetMode.targetVideoSignalInfo.vSyncFreq;
    const DisplayRefreshRate result{
        physical.Numerator,
        physical.Denominator,
        DisplayRefreshRateSource::DisplayConfigPhysicalRefresh};
    return validRefreshRate(result)
        ? std::optional<DisplayRefreshRate>(result)
        : std::nullopt;
}

[[nodiscard]] DisplayRefreshRate captureCadenceRefreshRate(
    const DisplayPhysicalTarget& target) noexcept
{
    if (target.physicalRefreshRate.has_value()
        && (target.dynamicRefreshRateBoosted
            || !validRefreshRate(target.refreshRate)))
    {
        // A 0/0 virtual rate delegates selection to Windows. The target mode
        // is still an actionable upper-bound cadence for capture freshness.
        return *target.physicalRefreshRate;
    }
    return target.refreshRate;
}

[[nodiscard]] std::optional<DisplayRefreshRate> commonCaptureRefreshRate(
    const ActiveDisplayMonitor& display) noexcept
{
    if (display.physicalTargets.empty())
    {
        return std::nullopt;
    }

    const DisplayRefreshRate refreshRate = captureCadenceRefreshRate(
        display.physicalTargets.front());
    if (!validRefreshRate(refreshRate))
    {
        return std::nullopt;
    }
    for (const DisplayPhysicalTarget& target : display.physicalTargets)
    {
        const DisplayRefreshRate targetRefreshRate =
            captureCadenceRefreshRate(target);
        if (targetRefreshRate.numerator != refreshRate.numerator
            || targetRefreshRate.denominator != refreshRate.denominator)
        {
            // A cloned source with different scan-out rates has no single
            // capture cadence. Callers retain their conservative fallback.
            return std::nullopt;
        }
    }
    return refreshRate;
}

BOOL CALLBACK collectMonitor(
    const HMONITOR monitor,
    HDC,
    RECT*,
    const LPARAM context) noexcept
{
    auto& enumeration = *reinterpret_cast<MonitorEnumeration*>(context);
    try
    {
        MONITORINFOEXW information{};
        information.cbSize = sizeof(information);
        if (!GetMonitorInfoW(monitor, &information))
        {
            enumeration.error = static_cast<LONG>(GetLastError());
            return FALSE;
        }

        UINT dpiX = 96U;
        UINT dpiY = 96U;
        if (FAILED(GetDpiForMonitor(
                monitor,
                MDT_EFFECTIVE_DPI,
                &dpiX,
                &dpiY)))
        {
            dpiX = 96U;
            dpiY = 96U;
        }

        ActiveDisplayMonitor display{};
        display.monitor = monitor;
        display.gdiDeviceName = information.szDevice;
        display.bounds = information.rcMonitor;
        display.workArea = information.rcWork;
        display.dpiX = dpiX == 0U ? 96U : dpiX;
        display.dpiY = dpiY == 0U ? 96U : dpiY;
        display.primary = (information.dwFlags & MONITORINFOF_PRIMARY) != 0U;
        enumeration.displays.push_back(std::move(display));
        return TRUE;
    }
    catch (const std::bad_alloc&)
    {
        enumeration.error = ERROR_OUTOFMEMORY;
        return FALSE;
    }
    catch (...)
    {
        enumeration.error = ERROR_GEN_FAILURE;
        return FALSE;
    }
}

[[nodiscard]] ActiveDisplayMonitor* findByGdiName(
    std::vector<ActiveDisplayMonitor>& displays,
    const wchar_t* const gdiName) noexcept
{
    const auto found = std::find_if(
        displays.begin(),
        displays.end(),
        [gdiName](const ActiveDisplayMonitor& display)
        {
            return _wcsicmp(display.gdiDeviceName.c_str(), gdiName) == 0;
        });
    return found == displays.end() ? nullptr : &*found;
}

[[nodiscard]] bool samePhysicalTarget(
    const DisplayPhysicalTarget& left,
    const DisplayPhysicalTarget& right) noexcept
{
    return sameLuid(left.adapterLuid, right.adapterLuid)
        && left.targetId == right.targetId;
}

void recordFirstError(
    DisplayTopologyStatus& status,
    LONG& error,
    const LONG nextError) noexcept
{
    if (status == DisplayTopologyStatus::Complete)
    {
        status = DisplayTopologyStatus::Incomplete;
        error = nextError;
    }
}

[[nodiscard]] LONG queryDisplayPathsWithFlags(
    const UINT queryFlags,
    std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
    std::vector<DISPLAYCONFIG_MODE_INFO>& modes) noexcept
{
    for (std::uint32_t attempt = 0U; attempt < 3U; ++attempt)
    {
        UINT32 pathCount = 0U;
        UINT32 modeCount = 0U;
        const LONG sizeResult = GetDisplayConfigBufferSizes(
            queryFlags,
            &pathCount,
            &modeCount);
        if (sizeResult != ERROR_SUCCESS)
        {
            return sizeResult;
        }

        try
        {
            paths.assign(pathCount, DISPLAYCONFIG_PATH_INFO{});
            modes.assign(modeCount, DISPLAYCONFIG_MODE_INFO{});
        }
        catch (const std::bad_alloc&)
        {
            return ERROR_OUTOFMEMORY;
        }

        const LONG queryResult = QueryDisplayConfig(
            queryFlags,
            &pathCount,
            paths.data(),
            &modeCount,
            modes.data(),
            nullptr);
        if (queryResult == ERROR_INSUFFICIENT_BUFFER)
        {
            // Hot-plug can invalidate both counts. Restart from a fresh pair;
            // the fixed retry budget prevents topology churn from blocking.
            continue;
        }
        if (queryResult != ERROR_SUCCESS)
        {
            return queryResult;
        }
        paths.resize(pathCount);
        modes.resize(modeCount);
        return ERROR_SUCCESS;
    }
    return ERROR_RETRY;
}

[[nodiscard]] LONG queryDisplayPaths(
    std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
    std::vector<DISPLAYCONFIG_MODE_INFO>& modes,
    bool& virtualRefreshRateAware) noexcept
{
    constexpr UINT baseFlags = QDC_ONLY_ACTIVE_PATHS
        | QDC_VIRTUAL_MODE_AWARE;
    // QDC_VIRTUAL_REFRESH_RATE_AWARE is a Windows 11 query contract. Keep the
    // numeric ABI local so an older SDK can still compile the full binary; an
    // older runtime rejects the flag and takes the bounded fallback below.
    constexpr UINT virtualRefreshRateAwareFlag = 0x00000040U;
    const LONG virtualResult = queryDisplayPathsWithFlags(
        baseFlags | virtualRefreshRateAwareFlag,
        paths,
        modes);
    if (virtualResult == ERROR_SUCCESS)
    {
        virtualRefreshRateAware = true;
        return ERROR_SUCCESS;
    }
    if (virtualResult != ERROR_INVALID_PARAMETER
        && virtualResult != ERROR_NOT_SUPPORTED)
    {
        return virtualResult;
    }

    virtualRefreshRateAware = false;
    return queryDisplayPathsWithFlags(baseFlags, paths, modes);
}

}

DisplayTopologySnapshot queryActiveDisplayTopology() noexcept
{
    DisplayTopologySnapshot snapshot{};
    try
    {
        MonitorEnumeration enumeration{};
        if (!EnumDisplayMonitors(
                nullptr,
                nullptr,
                &collectMonitor,
                reinterpret_cast<LPARAM>(&enumeration)))
        {
            snapshot.error = enumeration.error == ERROR_SUCCESS
                ? static_cast<LONG>(GetLastError())
                : enumeration.error;
            return snapshot;
        }
        if (enumeration.displays.empty())
        {
            snapshot.status = DisplayTopologyStatus::NoActiveDisplays;
            snapshot.error = ERROR_NOT_FOUND;
            return snapshot;
        }

        snapshot.status = DisplayTopologyStatus::Complete;
        snapshot.error = ERROR_SUCCESS;
        snapshot.displays = std::move(enumeration.displays);

        std::vector<DISPLAYCONFIG_PATH_INFO> paths;
        std::vector<DISPLAYCONFIG_MODE_INFO> modes;
        bool virtualRefreshRateAware = false;
        const LONG pathResult = queryDisplayPaths(
            paths,
            modes,
            virtualRefreshRateAware);
        if (pathResult != ERROR_SUCCESS)
        {
            snapshot.status = DisplayTopologyStatus::Incomplete;
            snapshot.error = pathResult;
            return snapshot;
        }

        for (const DISPLAYCONFIG_PATH_INFO& path : paths)
        {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
            sourceName.header.type =
                DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sourceName.header.size = sizeof(sourceName);
            sourceName.header.adapterId = path.sourceInfo.adapterId;
            sourceName.header.id = path.sourceInfo.id;
            const LONG sourceResult =
                DisplayConfigGetDeviceInfo(&sourceName.header);
            if (sourceResult != ERROR_SUCCESS)
            {
                recordFirstError(
                    snapshot.status,
                    snapshot.error,
                    sourceResult);
                continue;
            }

            ActiveDisplayMonitor* const display = findByGdiName(
                snapshot.displays,
                sourceName.viewGdiDeviceName);
            if (display == nullptr)
            {
                recordFirstError(
                    snapshot.status,
                    snapshot.error,
                    ERROR_NOT_FOUND);
                continue;
            }
            if (display->sourceIdentityResolved
                && (!sameLuid(
                        display->sourceAdapterLuid,
                        path.sourceInfo.adapterId)
                    || display->sourceId != path.sourceInfo.id))
            {
                recordFirstError(
                    snapshot.status,
                    snapshot.error,
                    ERROR_INVALID_DATA);
                continue;
            }
            display->sourceAdapterLuid = path.sourceInfo.adapterId;
            display->sourceId = path.sourceInfo.id;
            display->sourceIdentityResolved = true;

            DisplayPhysicalTarget target{};
            target.adapterLuid = path.targetInfo.adapterId;
            target.targetId = path.targetInfo.id;
            target.refreshRate = DisplayRefreshRate{
                path.targetInfo.refreshRate.Numerator,
                path.targetInfo.refreshRate.Denominator,
                virtualRefreshRateAware
                    ? DisplayRefreshRateSource::DisplayConfigVirtualRefresh
                    : DisplayRefreshRateSource::DisplayConfigPath};
            target.physicalRefreshRate = physicalRefreshRate(path, modes);
            constexpr UINT dynamicRefreshRateBoostFlag = 0x00000010U;
            target.dynamicRefreshRateBoosted = virtualRefreshRateAware
                && (path.flags & dynamicRefreshRateBoostFlag) != 0U;
            target.rotation = path.targetInfo.rotation;
            target.scaling = path.targetInfo.scaling;
            target.outputTechnology = path.targetInfo.outputTechnology;
            target.available = path.targetInfo.targetAvailable != FALSE;

            DISPLAYCONFIG_TARGET_DEVICE_NAME targetName{};
            targetName.header.type =
                DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            targetName.header.size = sizeof(targetName);
            targetName.header.adapterId = target.adapterLuid;
            targetName.header.id = target.targetId;
            const LONG targetResult =
                DisplayConfigGetDeviceInfo(&targetName.header);
            if (targetResult == ERROR_SUCCESS)
            {
                target.friendlyName = targetName.monitorFriendlyDeviceName;
                target.devicePath = targetName.monitorDevicePath;
            }
            else
            {
                recordFirstError(
                    snapshot.status,
                    snapshot.error,
                    targetResult);
            }

            const auto duplicate = std::find_if(
                display->physicalTargets.begin(),
                display->physicalTargets.end(),
                [&target](const DisplayPhysicalTarget& existing)
                {
                    return samePhysicalTarget(existing, target);
                });
            if (duplicate == display->physicalTargets.end())
            {
                display->physicalTargets.push_back(std::move(target));
            }
        }

        for (ActiveDisplayMonitor& display : snapshot.displays)
        {
            if (!display.sourceIdentityResolved
                || display.physicalTargets.empty())
            {
                recordFirstError(
                    snapshot.status,
                    snapshot.error,
                    ERROR_NOT_FOUND);
            }
            display.captureRefreshRate = commonCaptureRefreshRate(display);
        }
        return snapshot;
    }
    catch (const std::bad_alloc&)
    {
        snapshot.status = DisplayTopologyStatus::QueryFailed;
        snapshot.error = ERROR_OUTOFMEMORY;
    }
    catch (...)
    {
        snapshot.status = DisplayTopologyStatus::QueryFailed;
        snapshot.error = ERROR_GEN_FAILURE;
    }
    return snapshot;
}

const ActiveDisplayMonitor* findDisplayMonitor(
    const DisplayTopologySnapshot& snapshot,
    const HMONITOR monitor) noexcept
{
    const auto found = std::find_if(
        snapshot.displays.begin(),
        snapshot.displays.end(),
        [monitor](const ActiveDisplayMonitor& display)
        {
            return display.monitor == monitor;
        });
    return found == snapshot.displays.end() ? nullptr : &*found;
}

std::optional<DisplayRefreshRate> queryDisplayRefreshRate(
    const HMONITOR monitor) noexcept
{
    if (monitor == nullptr)
    {
        return std::nullopt;
    }
    const DisplayTopologySnapshot snapshot = queryActiveDisplayTopology();
    const ActiveDisplayMonitor* const display = findDisplayMonitor(
        snapshot,
        monitor);
    if (display == nullptr)
    {
        return std::nullopt;
    }
    return display->captureRefreshRate;
}

}
