#pragma once

#include <cstdint>
#include <string>

namespace bafx::windows
{

inline constexpr std::uint32_t minimumRecordingCompatibleBuild = 28000U;

enum class RecordingCompatibleAvailabilityReason : std::uint8_t
{
    Available,
    UnsupportedBuild,
    VersionQueryFailed
};

struct RecordingCompatibleAvailability final
{
    bool versionQuerySucceeded{false};
    bool supported{false};
    std::uint32_t major{0U};
    std::uint32_t minor{0U};
    std::uint32_t build{0U};
    RecordingCompatibleAvailabilityReason reason{
        RecordingCompatibleAvailabilityReason::VersionQueryFailed};
};

// Queries ntdll directly because VersionHelpers can report the manifest
// compatibility version instead of the actual Windows build used for gating.
[[nodiscard]] RecordingCompatibleAvailability
queryRecordingCompatibleAvailability() noexcept;

// This deterministic constructor keeps the policy unit-testable without
// teaching tests to impersonate a particular Windows installation.
[[nodiscard]] RecordingCompatibleAvailability
recordingCompatibleAvailabilityForBuild(
    std::uint32_t build,
    bool versionQuerySucceeded = true,
    std::uint32_t major = 10U,
    std::uint32_t minor = 0U) noexcept;

[[nodiscard]] std::string recordingCompatibleVersionString(
    const RecordingCompatibleAvailability& availability);

[[nodiscard]] const char* recordingCompatibleAvailabilityReasonName(
    RecordingCompatibleAvailabilityReason reason) noexcept;

}
