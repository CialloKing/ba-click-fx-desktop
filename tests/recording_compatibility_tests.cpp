#include "test_support.hpp"

#include "bafx/windows/recording_compatibility.hpp"

#include <cstdint>

BAFX_TEST(recording_compatibility_accepts_minimum_and_future_builds)
{
    constexpr std::uint32_t supportedBuilds[] = {28000U, 28001U, 29000U, 99999U};
    for (const std::uint32_t build : supportedBuilds)
    {
        const auto availability =
            bafx::windows::recordingCompatibleAvailabilityForBuild(build);
        BAFX_CHECK(availability.versionQuerySucceeded);
        BAFX_CHECK(availability.supported);
        BAFX_CHECK(
            availability.reason
            == bafx::windows::RecordingCompatibleAvailabilityReason::Available);
    }
}

BAFX_TEST(recording_compatibility_rejects_older_builds_without_an_upper_bound)
{
    constexpr std::uint32_t unsupportedBuilds[] = {19045U, 26100U, 27999U};
    for (const std::uint32_t build : unsupportedBuilds)
    {
        const auto availability =
            bafx::windows::recordingCompatibleAvailabilityForBuild(build);
        BAFX_CHECK(availability.versionQuerySucceeded);
        BAFX_CHECK(!availability.supported);
        BAFX_CHECK(
            availability.reason
            == bafx::windows::RecordingCompatibleAvailabilityReason::UnsupportedBuild);
    }
}

BAFX_TEST(recording_compatibility_treats_version_query_failure_as_unavailable)
{
    const auto availability =
        bafx::windows::recordingCompatibleAvailabilityForBuild(
            99999U,
            false);
    BAFX_CHECK(!availability.versionQuerySucceeded);
    BAFX_CHECK(!availability.supported);
    BAFX_CHECK(
        availability.reason
        == bafx::windows::RecordingCompatibleAvailabilityReason::VersionQueryFailed);
    BAFX_CHECK(
        bafx::windows::recordingCompatibleVersionString(availability)
        == "unknown");
}

BAFX_TEST(recording_compatibility_diagnostic_record_keeps_requested_and_effective_modes_distinct)
{
    const auto availability =
        bafx::windows::recordingCompatibleAvailabilityForBuild(28000U);
    const std::string record =
        bafx::windows::recordingCompatibleControlCenterDiagnosticRecord(
            availability,
            "recording-compatible-test: selected",
            "recording-compatible",
            "light-background",
            "available",
            7U);
    BAFX_CHECK(!bafx::windows::recordingCompatibleApplicationRevision().empty());
    BAFX_CHECK(
        bafx::windows::recordingCompatibleApplicationRevision() != "unknown");
    BAFX_CHECK(record.find("ApplicationRevision=") != std::string::npos);
    BAFX_CHECK(record.find("RequestedMode=recording-compatible")
        != std::string::npos);
    BAFX_CHECK(record.find("AppliedProfile=LightBackground")
        != std::string::npos);
    BAFX_CHECK(record.find("EffectiveMode=light-background")
        != std::string::npos);
    BAFX_CHECK(record.find("EffectivePath=fx-only") != std::string::npos);
    BAFX_CHECK(record.find("WGC=disabled") != std::string::npos);
    BAFX_CHECK(record.find("AlphaLimit=0.85") != std::string::npos);
    BAFX_CHECK(record.find("Generation=7") != std::string::npos);
}
