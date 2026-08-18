#include "bafx/windows/recording_compatibility.hpp"

#include <windows.h>
#include <winternl.h>

#include <sstream>

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

}
