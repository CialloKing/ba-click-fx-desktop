#pragma once

#include "bafx/windows/display_capabilities.hpp"
#include "bafx/windows/display_topology.hpp"
#include "bafx/windows/fx_gpu_renderer.hpp"

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bafx::desktop
{

struct DisplayPhysicalTargetIdentity final
{
    LUID adapterLuid{};
    std::uint32_t targetId{0U};
    std::wstring devicePath{};
    DISPLAYCONFIG_ROTATION rotation{DISPLAYCONFIG_ROTATION_IDENTITY};
    DISPLAYCONFIG_SCALING scaling{DISPLAYCONFIG_SCALING_IDENTITY};
    DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY outputTechnology{
        DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER};
    bool available{false};
};

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
    bool sourceAdapterResolved{false};
    bool sourceIdentityResolved{false};
    std::vector<DisplayPhysicalTargetIdentity> physicalTargetIdentities{};
    bafx::windows::DisplayTopologyStatus topologyStatus{
        bafx::windows::DisplayTopologyStatus::QueryFailed};
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

[[nodiscard]] inline bool sameDisplayPhysicalTargetIdentity(
    const DisplayPhysicalTargetIdentity& left,
    const DisplayPhysicalTargetIdentity& right) noexcept
{
    const bool stableEndpoint =
        left.adapterLuid.HighPart == right.adapterLuid.HighPart
        && left.adapterLuid.LowPart == right.adapterLuid.LowPart
        && left.targetId == right.targetId;
    if (!stableEndpoint)
    {
        return false;
    }

    // GET_TARGET_NAME can fail transiently during hot-plug. An unresolved path
    // is not evidence of replacement; compare it only when both snapshots have
    // an authoritative monitor device path. Device interface paths follow the
    // Win32 case-insensitive identity contract even if a driver changes casing.
    const bool sameDevicePath = left.devicePath.empty()
        || right.devicePath.empty()
        || CompareStringOrdinal(
                left.devicePath.c_str(),
                -1,
                right.devicePath.c_str(),
                -1,
                TRUE) == CSTR_EQUAL;
    if (!sameDevicePath)
    {
        return false;
    }

    // A 180-degree rotation or GPU scaling change can preserve rcMonitor and
    // the target ID while invalidating capture/output coordinate contracts.
    return left.rotation == right.rotation
        && left.scaling == right.scaling
        && left.outputTechnology == right.outputTechnology
        && left.available == right.available;
}

[[nodiscard]] inline bool sameDisplayPhysicalTargets(
    const DisplayTarget& left,
    const DisplayTarget& right) noexcept
{
    if (left.physicalTargetIdentities.size()
        != right.physicalTargetIdentities.size())
    {
        return false;
    }
    for (std::size_t index = 0U;
         index < left.physicalTargetIdentities.size();
         ++index)
    {
        if (!sameDisplayPhysicalTargetIdentity(
                left.physicalTargetIdentities[index],
                right.physicalTargetIdentities[index]))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool displayPhysicalTargetIdentityResolutionImproved(
    const DisplayTarget& previous,
    const DisplayTarget& current) noexcept
{
    if (previous.physicalTargetIdentities.size()
        != current.physicalTargetIdentities.size())
    {
        return false;
    }

    bool improved = false;
    for (std::size_t index = 0U;
         index < previous.physicalTargetIdentities.size();
         ++index)
    {
        const DisplayPhysicalTargetIdentity& oldIdentity =
            previous.physicalTargetIdentities[index];
        const DisplayPhysicalTargetIdentity& newIdentity =
            current.physicalTargetIdentities[index];
        if (!oldIdentity.devicePath.empty()
            && newIdentity.devicePath.empty())
        {
            // Keep an authoritative path while DisplayConfig is transiently
            // incomplete; losing evidence is not a metadata improvement.
            return false;
        }
        if (oldIdentity.devicePath.empty()
            && !newIdentity.devicePath.empty())
        {
            improved = true;
        }
    }
    return improved;
}

[[nodiscard]] inline bool displaySourceIdentityResolutionImproved(
    const DisplayTarget& previous,
    const DisplayTarget& current) noexcept
{
    const bool knownAdapterChanged = previous.sourceAdapterResolved
        && current.sourceAdapterResolved
        && (previous.sourceAdapterLuid.HighPart
                != current.sourceAdapterLuid.HighPart
            || previous.sourceAdapterLuid.LowPart
                != current.sourceAdapterLuid.LowPart);
    if (knownAdapterChanged)
    {
        return false;
    }

    return (!previous.sourceAdapterResolved
            && current.sourceAdapterResolved)
        || (!previous.sourceIdentityResolved
            && current.sourceIdentityResolved);
}

[[nodiscard]] inline bool displayColorCapabilityEvidenceImproved(
    const DisplayTarget& previous,
    const DisplayTarget& current) noexcept
{
    const bool topologyRecovered =
        previous.topologyStatus
            != bafx::windows::DisplayTopologyStatus::Complete
        && current.topologyStatus
            == bafx::windows::DisplayTopologyStatus::Complete;
    return displaySourceIdentityResolutionImproved(previous, current)
        || displayPhysicalTargetIdentityResolutionImproved(previous, current)
        || topologyRecovered;
}

[[nodiscard]] inline bool sameDisplayTarget(
    const DisplayTarget& left,
    const DisplayTarget& right) noexcept
{
    return left.monitor == right.monitor
        && left.deviceName == right.deviceName
        && sameDisplayBounds(left.bounds, right.bounds)
        && left.physicalTargetCount == right.physicalTargetCount
        && sameDisplayPhysicalTargets(left, right);
}

[[nodiscard]] inline bool sameDisplayLogicalSlot(
    const DisplayTarget& left,
    const DisplayTarget& right) noexcept
{
    const bool sameMonitor = left.monitor != nullptr
        && left.monitor == right.monitor;
    const bool sameGdiSource = !left.deviceName.empty()
        && left.deviceName == right.deviceName;
    // HMONITOR and \\.\DISPLAYn identify desktop placement, not a D3D
    // resource domain. Use them only to keep a session attached while the
    // stronger DisplayConfig source identity is being replaced.
    return sameMonitor || sameGdiSource;
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
    if (left.sourceAdapterResolved != right.sourceAdapterResolved)
    {
        return false;
    }
    if (left.sourceAdapterResolved
        && (left.sourceAdapterLuid.HighPart
                != right.sourceAdapterLuid.HighPart
            || left.sourceAdapterLuid.LowPart
                != right.sourceAdapterLuid.LowPart))
    {
        return false;
    }
    if (left.sourceIdentityResolved != right.sourceIdentityResolved)
    {
        return false;
    }
    return !left.sourceIdentityResolved
        || left.sourceId == right.sourceId;
}

[[nodiscard]] inline bool sameDisplayRefreshRate(
    const std::optional<bafx::windows::DisplayRefreshRate>& left,
    const std::optional<bafx::windows::DisplayRefreshRate>& right) noexcept
{
    if (left.has_value() != right.has_value())
    {
        return false;
    }
    return !left.has_value()
        || (bafx::windows::equivalentDisplayRefreshRate(*left, *right)
            && left->source == right->source);
}

[[nodiscard]] inline bool sameDisplayRuntimeMetadata(
    const DisplayTarget& left,
    const DisplayTarget& right) noexcept
{
    // The rate source is part of the contract: an equal rational can switch
    // from a virtual DRR rate to a physical scan-out rate with different
    // capture freshness semantics.
    return left.dpiX == right.dpiX
        && left.dpiY == right.dpiY
        && sameDisplayRefreshRate(left.refreshRate, right.refreshRate)
        && sameDisplayRefreshRate(
            left.captureRefreshRate,
            right.captureRefreshRate)
        && left.primary == right.primary
        && left.physicalTargetCount == right.physicalTargetCount
        && left.topologyStatus == right.topologyStatus;
}

[[nodiscard]] inline bool displayTargetMetadataChanged(
    const DisplayTarget& previous,
    const DisplayTarget& current) noexcept
{
    return !sameDisplayRuntimeMetadata(previous, current)
        || displayPhysicalTargetIdentityResolutionImproved(previous, current);
}

[[nodiscard]] inline bool displayTargetResourceAdapterMatches(
    const DisplayTarget& target,
    const LUID actualAdapter,
    const bool hardwareDevice) noexcept
{
    return !target.sourceAdapterResolved
        || (hardwareDevice
            && target.sourceAdapterLuid.HighPart == actualAdapter.HighPart
            && target.sourceAdapterLuid.LowPart == actualAdapter.LowPart);
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
    // A display migration must carry the target monitor's resolved transport
    // through any asynchronous WGC permission wait. Omitting it preserves the
    // current swap-chain contract for size-only and capture-only transactions.
    std::optional<bafx::windows::CompositionOutputPolicy> outputPolicy{};
    // Keep the capability sample that resolved outputPolicy beside the
    // target. A post-move query can fail transiently during hot-plug; that
    // failure must not immediately undo the transport this transaction chose.
    std::optional<bafx::windows::DisplayColorCapabilities>
        outputColorCapabilities{};
};

[[nodiscard]] inline bool sameDisplayTargetIntent(
    const DisplayTargetIntent& left,
    const DisplayTargetIntent& right) noexcept
{
    // The capability sample is evidence for commit-time fallback, not intent
    // identity. A newer sample with the same resolved transport must not cancel
    // an in-flight permission request.
    return left.applyBounds == right.applyBounds
        && left.outputPolicy == right.outputPolicy
        && sameDisplayTarget(left.target, right.target)
        && sameDisplaySourceIdentity(left.target, right.target);
}

[[nodiscard]] std::string displayTargetDeviceUtf8(
    const DisplayTarget& target);
// Persistent policy identity is intentionally narrower than session identity:
// only authoritative DisplayConfig physical target paths may contribute.
// The returned value is an opaque versioned SHA-256 key; raw paths are never
// persisted in the configuration contract.
[[nodiscard]] std::optional<std::string> displayTargetPersistentKey(
    const DisplayTarget& target) noexcept;
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
[[nodiscard]] const DisplayTarget* findDisplayTargetByLogicalSlot(
    const DisplayTargetSnapshot& snapshot,
    const DisplayTarget& reference) noexcept;
[[nodiscard]] const DisplayTarget* findDisplayTargetAtPoint(
    const DisplayTargetSnapshot& snapshot,
    POINT point) noexcept;
[[nodiscard]] DisplayTarget stabilizeDisplayTargetObservation(
    const DisplayTarget& previous,
    const DisplayTarget& observed,
    bafx::windows::DisplayTopologyStatus topologyStatus);

}
