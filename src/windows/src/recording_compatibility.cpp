#include "bafx/windows/recording_compatibility.hpp"

#include "bafx/windows/portable_paths.hpp"

#include <windows.h>
#include <winternl.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef BAFX_RECORDING_COMPATIBILITY_APPLICATION_REVISION
#define BAFX_RECORDING_COMPATIBILITY_APPLICATION_REVISION "unknown"
#endif

namespace bafx::windows
{
namespace
{

using RtlGetVersionFunction = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

[[nodiscard]] RecordingCompatibleAvailability makeAvailability(
    const bool versionQuerySucceeded,
    const std::uint32_t major,
    const std::uint32_t minor,
    const std::uint32_t build) noexcept
{
    RecordingCompatibleAvailability availability{};
    availability.versionQuerySucceeded = versionQuerySucceeded;
    availability.major = major;
    availability.minor = minor;
    availability.build = build;
    availability.supported = versionQuerySucceeded
        && build >= minimumRecordingCompatibleBuild;
    availability.reason = !versionQuerySucceeded
        ? RecordingCompatibleAvailabilityReason::VersionQueryFailed
        : (availability.supported
            ? RecordingCompatibleAvailabilityReason::Available
            : RecordingCompatibleAvailabilityReason::UnsupportedBuild);
    return availability;
}

}

RecordingCompatibleAvailability queryRecordingCompatibleAvailability() noexcept
{
    const HMODULE module = GetModuleHandleW(L"ntdll.dll");
    if (module == nullptr)
    {
        return makeAvailability(false, 0U, 0U, 0U);
    }

    const auto getVersion = reinterpret_cast<RtlGetVersionFunction>(
        GetProcAddress(module, "RtlGetVersion"));
    if (getVersion == nullptr)
    {
        return makeAvailability(false, 0U, 0U, 0U);
    }

    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (getVersion(&version) != 0L)
    {
        return makeAvailability(false, 0U, 0U, 0U);
    }

    return makeAvailability(
        true,
        version.dwMajorVersion,
        version.dwMinorVersion,
        version.dwBuildNumber);
}

RecordingCompatibleAvailability recordingCompatibleAvailabilityForBuild(
    const std::uint32_t build,
    const bool versionQuerySucceeded,
    const std::uint32_t major,
    const std::uint32_t minor) noexcept
{
    return makeAvailability(versionQuerySucceeded, major, minor, build);
}

std::string recordingCompatibleVersionString(
    const RecordingCompatibleAvailability& availability)
{
    if (!availability.versionQuerySucceeded)
    {
        return "unknown";
    }

    std::ostringstream stream;
    stream << availability.major << '.'
           << availability.minor << '.'
           << availability.build;
    return stream.str();
}

const char* recordingCompatibleAvailabilityReasonName(
    const RecordingCompatibleAvailabilityReason reason) noexcept
{
    switch (reason)
    {
    case RecordingCompatibleAvailabilityReason::Available:
        return "available";
    case RecordingCompatibleAvailabilityReason::UnsupportedBuild:
        return "unsupported-build";
    case RecordingCompatibleAvailabilityReason::VersionQueryFailed:
        return "version-query-failed";
    }
    return "version-query-failed";
}

std::string_view recordingCompatibleApplicationRevision() noexcept
{
    return BAFX_RECORDING_COMPATIBILITY_APPLICATION_REVISION;
}

std::string recordingCompatibleControlCenterDiagnosticRecord(
    const RecordingCompatibleAvailability& availability,
    const std::string_view eventName,
    const std::string_view requestedMode,
    const std::string_view effectiveMode,
    const std::string_view reason,
    const std::uint64_t generation)
{
    const bool recordingCompatible = effectiveMode == "recording-compatible";
    const bool lightBackground = effectiveMode == "light-background";
    const std::string_view appliedProfile = recordingCompatible
        ? std::string_view("RecordingCompatible")
        : (lightBackground
            ? std::string_view("LightBackground")
            : std::string_view("BackgroundAware"));
    const std::string_view effectivePath =
        recordingCompatible || lightBackground
        ? std::string_view("fx-only")
        : std::string_view("background-aware");
    const std::string_view wgc = recordingCompatible || lightBackground
        ? std::string_view("disabled")
        : std::string_view("runtime-managed");
    const std::string_view alphaLimit = recordingCompatible
        ? std::string_view("0.90")
        : (lightBackground
            ? std::string_view("0.85")
            : std::string_view("profile-dependent"));

    std::ostringstream output;
    output << "Log.SchemaVersion=2\n"
           << "Event.Name=" << eventName << '\n'
           << "Event.Application=BAFX.ControlCenter\n"
           << "ApplicationRevision="
           << recordingCompatibleApplicationRevision() << '\n'
           << "OS.Version="
           << recordingCompatibleVersionString(availability) << '\n'
           << "OS.Build=" << availability.build << '\n'
           << "RecordingCompatible.MinimumBuild="
           << minimumRecordingCompatibleBuild << '\n'
           << "RecordingCompatible.Eligibility="
           << (availability.supported ? "available" : "unavailable")
           << '\n'
           << "RequestedMode=" << requestedMode << '\n'
           << "AppliedProfile=" << appliedProfile << '\n'
           << "EffectiveMode=" << effectiveMode << '\n'
           << "EffectivePath=" << effectivePath << '\n'
           << "WGC=" << wgc << '\n'
           << "AlphaLimit=" << alphaLimit << '\n'
           << "Generation=" << generation << '\n'
           << "Reason=" << reason << '\n'
           << "---\n";
    return output.str();
}

void appendRecordingCompatibleControlCenterDiagnostic(
    const RecordingCompatibleAvailability& availability,
    const std::string_view eventName,
    const std::string_view requestedMode,
    const std::string_view effectiveMode,
    const std::string_view reason,
    const std::uint64_t generation) noexcept
{
    try
    {
        const std::filesystem::path path = executableFilePath(
            L"ba-click-fx-desktop-support.log",
            L"ba-click-fx-desktop-support.log");
        std::ofstream output(path, std::ios::binary | std::ios::app);
        if (!output)
        {
            return;
        }
        output << recordingCompatibleControlCenterDiagnosticRecord(
            availability,
            eventName,
            requestedMode,
            effectiveMode,
            reason,
            generation);
    }
    catch (...)
    {
        // A diagnostic failure must not block a mode selection or rejection.
    }
}

}
