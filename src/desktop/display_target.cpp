#include "display_target.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bafx::desktop
{
namespace
{

constexpr std::string_view displayKeyDomain = "bafx-display-target-key-v1";
constexpr std::string_view displayKeyPrefix = "displayconfig-v1-sha256:";
constexpr std::size_t sha256ByteCount = 32U;

[[nodiscard]] std::optional<std::string> normalizeDevicePathUtf8(
    const std::wstring_view path)
{
    if (path.empty()
        || path.size()
            > static_cast<std::size_t>((std::numeric_limits<int>::max)())
        || std::ranges::any_of(
            path,
            [](const wchar_t character) noexcept
            {
                return static_cast<std::uint32_t>(character) < 0x20U;
            }))
    {
        return std::nullopt;
    }

    const int sourceLength = static_cast<int>(path.size());
    const int normalizedLength = LCMapStringEx(
        LOCALE_NAME_INVARIANT,
        LCMAP_LOWERCASE,
        path.data(),
        sourceLength,
        nullptr,
        0,
        nullptr,
        nullptr,
        0);
    if (normalizedLength <= 0)
    {
        return std::nullopt;
    }

    std::wstring normalized(
        static_cast<std::size_t>(normalizedLength),
        L'\0');
    const int normalizedWritten = LCMapStringEx(
        LOCALE_NAME_INVARIANT,
        LCMAP_LOWERCASE,
        path.data(),
        sourceLength,
        normalized.data(),
        normalizedLength,
        nullptr,
        nullptr,
        0);
    if (normalizedWritten != normalizedLength)
    {
        return std::nullopt;
    }

    const int utf8Length = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        normalized.data(),
        normalizedLength,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (utf8Length <= 0)
    {
        return std::nullopt;
    }

    std::string utf8(static_cast<std::size_t>(utf8Length), '\0');
    const int utf8Written = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        normalized.data(),
        normalizedLength,
        utf8.data(),
        utf8Length,
        nullptr,
        nullptr);
    if (utf8Written != utf8Length)
    {
        return std::nullopt;
    }
    return utf8;
}

void appendBigEndianUint64(
    std::string& output,
    const std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
    {
        output.push_back(static_cast<char>((value >> shift) & 0xFFU));
    }
}

[[nodiscard]] std::optional<std::array<UCHAR, sha256ByteCount>> sha256(
    const std::string_view input) noexcept
{
    if (input.size() > (std::numeric_limits<ULONG>::max)())
    {
        return std::nullopt;
    }

    std::array<UCHAR, sha256ByteCount> digest{};
    // The SHA-256 pseudo-handle is immutable and available on every supported
    // Windows 10/11 target, so key generation owns no provider lifetime.
    const NTSTATUS hashStatus = BCryptHash(
        BCRYPT_SHA256_ALG_HANDLE,
        nullptr,
        0U,
        reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
        static_cast<ULONG>(input.size()),
        digest.data(),
        static_cast<ULONG>(digest.size()));
    if (hashStatus < 0)
    {
        return std::nullopt;
    }
    return digest;
}

[[nodiscard]] std::string hexDigest(
    const std::array<UCHAR, sha256ByteCount>& digest)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(displayKeyPrefix.size() + digest.size() * 2U);
    result.append(displayKeyPrefix);
    for (const UCHAR byte : digest)
    {
        result.push_back(digits[(byte >> 4U) & 0x0FU]);
        result.push_back(digits[byte & 0x0FU]);
    }
    return result;
}

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

std::optional<std::string> displayTargetPersistentKey(
    const DisplayTarget& target) noexcept
{
    try
    {
        if (target.physicalTargetCount == 0U
            || target.physicalTargetCount
                != target.physicalTargetIdentities.size())
        {
            return std::nullopt;
        }

        std::vector<std::string> paths;
        paths.reserve(target.physicalTargetIdentities.size());
        for (const DisplayPhysicalTargetIdentity& identity :
             target.physicalTargetIdentities)
        {
            std::optional<std::string> path = normalizeDevicePathUtf8(
                identity.devicePath);
            if (!path.has_value())
            {
                // A partial clone must not collapse onto the key of its one
                // resolved endpoint and silently consume that panel's policy.
                return std::nullopt;
            }
            paths.push_back(std::move(*path));
        }
        std::sort(paths.begin(), paths.end());

        std::string canonical;
        canonical.reserve(displayKeyDomain.size() + 1U + 8U);
        canonical.append(displayKeyDomain);
        canonical.push_back('\0');
        appendBigEndianUint64(
            canonical,
            static_cast<std::uint64_t>(paths.size()));
        for (const std::string& path : paths)
        {
            appendBigEndianUint64(
                canonical,
                static_cast<std::uint64_t>(path.size()));
            canonical.append(path);
        }

        const std::optional<std::array<UCHAR, sha256ByteCount>> digest =
            sha256(canonical);
        return digest.has_value()
            ? std::optional<std::string>(hexDigest(*digest))
            : std::nullopt;
    }
    catch (...)
    {
        // Persistent identity is optional. Resource or conversion failures
        // must disable overrides rather than fabricate a transient fallback.
        return std::nullopt;
    }
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
            target.captureCadenceFallbackReason =
                display.captureCadenceFallbackReason;
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
                        physicalTarget.available,
                        validRefreshRate(physicalTarget.refreshRate)
                            ? std::optional<bafx::windows::DisplayRefreshRate>(
                                physicalTarget.refreshRate)
                            : std::nullopt,
                        physicalTarget.physicalRefreshRate,
                        physicalTarget.captureRefreshRate,
                        physicalTarget.dynamicRefreshRateBoosted});
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
    for (DisplayTarget& display : result.displays)
    {
        // Every target must carry the final snapshot quality. An invalid later
        // monitor can downgrade an observation after an earlier target was read.
        display.topologyStatus = result.status;
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

const DisplayTarget* findDisplayTargetByLogicalSlot(
    const DisplayTargetSnapshot& snapshot,
    const DisplayTarget& reference) noexcept
{
    const auto found = std::find_if(
        snapshot.displays.begin(),
        snapshot.displays.end(),
        [&reference](const DisplayTarget& display)
        {
            return sameDisplayLogicalSlot(reference, display);
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
    stabilized.topologyStatus = topologyStatus;
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
        stabilized.captureCadenceFallbackReason =
            previous.captureCadenceFallbackReason;
        stabilized.physicalTargetCount = previous.physicalTargetCount;
        stabilized.physicalTargetIdentities =
            previous.physicalTargetIdentities;
    }
    return stabilized;
}

}
