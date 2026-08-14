#include "bafx/windows/runtime_diagnostics.hpp"

#include "bafx/windows/portable_paths.hpp"

#include <windows.h>
#include <winternl.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>

namespace bafx::windows
{
namespace
{

[[nodiscard]] std::string wideToUtf8(const std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0)
    {
        return "<invalid-utf8>";
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        required,
        nullptr,
        nullptr);
    if (written != required)
    {
        return "<invalid-utf8>";
    }
    return result;
}

[[nodiscard]] std::string sanitize(const std::string_view value)
{
    std::string result(value);
    for (char& character : result)
    {
        if (character == '\r' || character == '\n' || character == '=')
        {
            character = ' ';
        }
    }
    return result;
}

[[nodiscard]] std::string utcTimestamp()
{
    SYSTEMTIME time{};
    GetSystemTime(&time);
    std::ostringstream stream;
    stream << std::setfill('0')
           << std::setw(4) << time.wYear << '-'
           << std::setw(2) << time.wMonth << '-'
           << std::setw(2) << time.wDay << 'T'
           << std::setw(2) << time.wHour << ':'
           << std::setw(2) << time.wMinute << ':'
           << std::setw(2) << time.wSecond << '.'
           << std::setw(3) << time.wMilliseconds << 'Z';
    return stream.str();
}

[[nodiscard]] std::string osVersion()
{
    using RtlGetVersion = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE module = GetModuleHandleW(L"ntdll.dll");
    if (module == nullptr)
    {
        return "unknown";
    }

    const auto function = reinterpret_cast<RtlGetVersion>(
        GetProcAddress(module, "RtlGetVersion"));
    if (function == nullptr)
    {
        return "unknown";
    }

    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (function(&version) != 0L)
    {
        return "unknown";
    }

    std::ostringstream stream;
    stream << version.dwMajorVersion << '.'
           << version.dwMinorVersion << '.'
           << version.dwBuildNumber;
    return stream.str();
}

[[nodiscard]] std::string nativeArchitecture()
{
    SYSTEM_INFO information{};
    GetNativeSystemInfo(&information);
    switch (information.wProcessorArchitecture)
    {
    case PROCESSOR_ARCHITECTURE_AMD64:
        return "x64";

    case PROCESSOR_ARCHITECTURE_ARM64:
        return "arm64";

    case PROCESSOR_ARCHITECTURE_INTEL:
        return "x86";

    default:
        return "unknown";
    }
}

[[nodiscard]] std::string featureLevel(const D3D_FEATURE_LEVEL level)
{
    switch (level)
    {
    case D3D_FEATURE_LEVEL_11_1:
        return "11_1";

    case D3D_FEATURE_LEVEL_11_0:
        return "11_0";

    default:
        return "unknown";
    }
}

[[nodiscard]] std::string driverType(const GraphicsDriverType type)
{
    return type == GraphicsDriverType::Warp ? "WARP" : "Hardware";
}

[[nodiscard]] std::string_view colorSpaceName(
    const DXGI_COLOR_SPACE_TYPE colorSpace) noexcept
{
    switch (colorSpace)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        return "rgb-full-g22-p709";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        return "rgb-full-g10-p709";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        return "rgb-full-pq-p2020";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
        return "rgb-studio-pq-p2020";
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020:
        return "rgb-full-g22-p2020";
    default:
        return "other";
    }
}

[[nodiscard]] std::string hex32(const std::uint32_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << value;
    return stream.str();
}

[[nodiscard]] std::string luid(const LUID value)
{
    std::ostringstream stream;
    stream << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
           << static_cast<std::uint32_t>(value.HighPart) << ':'
           << std::setw(8) << static_cast<std::uint32_t>(value.LowPart);
    return stream.str();
}

[[nodiscard]] std::string driverVersion(const std::optional<std::uint64_t>& value)
{
    if (!value.has_value())
    {
        return "unknown";
    }

    ULARGE_INTEGER encoded{};
    encoded.QuadPart = value.value();
    std::ostringstream stream;
    stream << HIWORD(encoded.HighPart) << '.'
           << LOWORD(encoded.HighPart) << '.'
           << HIWORD(encoded.LowPart) << '.'
           << LOWORD(encoded.LowPart);
    return stream.str();
}

[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& path)
{
    return wideToUtf8(path.wstring());
}

}

SupportReport::SupportReport(const std::string_view version)
    : version_(version)
    , osVersion_(osVersion())
    , architecture_(nativeArchitecture())
{
}

void SupportReport::setPrimaryMonitor(const RECT bounds)
{
    std::ostringstream stream;
    stream << (bounds.right - bounds.left) << 'x'
           << (bounds.bottom - bounds.top) << '@'
           << bounds.left << ',' << bounds.top;
    primaryMonitor_ = stream.str();
}

void SupportReport::setPrimaryDpi(const std::uint32_t dpi) noexcept
{
    primaryDpi_ = dpi == 0U
        ? std::nullopt
        : std::optional<std::uint32_t>(dpi);
}

void SupportReport::setPrimaryDisplayColorCapabilities(
    const DisplayColorCapabilities& capabilities) noexcept
{
    primaryDisplayColorCapabilities_ = capabilities;
}

void SupportReport::setDeviceInfo(const GraphicsDeviceInfo& info)
{
    deviceInfo_ = info;
    hasDeviceInfo_ = true;
}

void SupportReport::setExitUiStatus(const ExitUiStatus& status)
{
    exitUiStatus_ = status;
    hasExitUiStatus_ = true;
}

void SupportReport::setBackgroundCaptureStatus(
    const BackgroundCaptureStatus status) noexcept
{
    backgroundCaptureStatus_ = status;
}

void SupportReport::setConfigurationSchemaVersion(
    const std::uint32_t version) noexcept
{
    configurationSchemaVersion_ = version;
}

void SupportReport::setControlServiceAvailable(const bool available) noexcept
{
    controlServiceAvailable_ = available;
}

void SupportReport::setLogPath(const std::filesystem::path& path)
{
    logPath_ = pathToUtf8(path);
}

void SupportReport::setFailure(const std::string_view failure)
{
    failure_ = sanitize(failure);
}

std::string SupportReport::serialize() const
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    const auto backgroundStatus = [this]() -> std::string_view
    {
        switch (backgroundCaptureStatus_)
        {
        case BackgroundCaptureStatus::NotProbed:
            return "not-probed";
        case BackgroundCaptureStatus::Active:
            return "active";
        case BackgroundCaptureStatus::FallbackFxOnly:
            return "fallback-fx-only";
        case BackgroundCaptureStatus::FallbackFxOnlyCaptureVisibilityUnknown:
            return "fallback-fx-only-capture-visibility-unknown";
        }
        return "unknown";
    };

    stream << "Report.GeneratedUtc=" << utcTimestamp() << '\n'
           << "Product.Name=ba-click-fx-desktop\n"
           << "Product.Version=" << sanitize(version_) << '\n'
           << "Status=" << (failure_.empty() ? "Ready" : "Failed") << '\n'
           << "Support.Scope=single-primary-monitor;fx-only-or-wgc;sdr-tested\n"
           << "Support.HDR=not-supported\n"
           << "Support.WGC=" << backgroundStatus() << '\n'
           << "Support.MultiDisplay=not-supported\n"
           << "Configuration.SchemaVersion=";
    if (configurationSchemaVersion_.has_value())
    {
        stream << *configurationSchemaVersion_;
    }
    else
    {
        stream << "not-loaded";
    }
    stream << '\n'
           << "IPC.ControlService="
           << (controlServiceAvailable_ ? "active" : "unavailable") << '\n'
           << "OS.Version=" << osVersion_ << '\n'
           << "OS.Architecture=" << architecture_ << '\n'
           << "Display.Primary="
           << (primaryMonitor_.empty() ? "unknown" : primaryMonitor_) << '\n'
           << "Display.PrimaryDpi=";
    if (primaryDpi_.has_value())
    {
        stream << *primaryDpi_;
    }
    else
    {
        stream << "unknown";
    }
    stream << '\n'
           << "Display.ColorMode=";
    if (primaryDisplayColorCapabilities_.has_value())
    {
        const DisplayColorCapabilities& color =
            *primaryDisplayColorCapabilities_;
        stream << colorSpaceName(color.colorSpace)
               << ";capability-only;luminance-"
               << (color.luminanceMetadataValid ? "valid" : "unknown")
               << ";alpha-scope-sdr-only\n"
               << "Display.DxgiColorSpaceValue="
               << hex32(static_cast<std::uint32_t>(color.colorSpace)) << '\n'
               << "Display.BitsPerColor=" << color.bitsPerColor << '\n'
               << std::fixed << std::setprecision(3)
               << "Display.MinLuminanceNits=" << color.minimumLuminanceNits << '\n'
               << "Display.MaxLuminanceNits=" << color.maximumLuminanceNits << '\n'
               << "Display.MaxFullFrameLuminanceNits="
               << color.maximumFullFrameLuminanceNits << '\n';
    }
    else
    {
        stream << "not-probed;alpha-scope-sdr-only\n"
               << "Display.DxgiColorSpaceValue=unknown\n"
               << "Display.BitsPerColor=unknown\n"
               << "Display.MinLuminanceNits=unknown\n"
               << "Display.MaxLuminanceNits=unknown\n"
               << "Display.MaxFullFrameLuminanceNits=unknown\n";
    }
    stream << "Log.Path=" << (logPath_.empty() ? "unknown" : logPath_) << '\n';

    if (hasDeviceInfo_)
    {
        stream << "Graphics.DriverType=" << driverType(deviceInfo_.driverType) << '\n'
               << "Graphics.Adapter="
               << sanitize(wideToUtf8(deviceInfo_.adapterDescription)) << '\n'
               << "Graphics.AdapterLuid=" << luid(deviceInfo_.adapterLuid) << '\n'
               << "Graphics.VendorId=" << hex32(deviceInfo_.vendorId) << '\n'
               << "Graphics.DeviceId=" << hex32(deviceInfo_.deviceId) << '\n'
               << "Graphics.SubsystemId=" << hex32(deviceInfo_.subsystemId) << '\n'
               << "Graphics.Revision=" << deviceInfo_.revision << '\n'
               << "Graphics.DedicatedVideoMemory="
               << deviceInfo_.dedicatedVideoMemory << '\n'
               << "Graphics.DedicatedSystemMemory="
               << deviceInfo_.dedicatedSystemMemory << '\n'
               << "Graphics.SharedSystemMemory="
               << deviceInfo_.sharedSystemMemory << '\n'
               << "Graphics.DriverVersion="
               << driverVersion(deviceInfo_.driverVersion) << '\n'
               << "Graphics.FeatureLevel="
               << featureLevel(deviceInfo_.featureLevel) << '\n'
               << "Graphics.HardwareCreateHResult="
               << hex32(static_cast<std::uint32_t>(deviceInfo_.hardwareCreateResult)) << '\n'
               << "Graphics.HardwareFallback="
               << (deviceInfo_.driverType == GraphicsDriverType::Warp ? "WARP" : "none") << '\n';
    }
    else
    {
        stream << "Graphics.DriverType=not-created\n";
    }

    if (hasExitUiStatus_)
    {
        stream << "Exit.PrimaryHotKey="
               << (exitUiStatus_.primaryHotKeyRegistered
                       ? "registered"
                       : "polling-fallback")
               << '\n'
               << "Exit.FallbackHotKey="
               << (exitUiStatus_.fallbackHotKeyRegistered
                       ? "registered"
                       : "polling-fallback")
               << '\n'
               << "Exit.NotificationIcon="
               << (exitUiStatus_.notificationIconAdded ? "available" : "unavailable")
               << '\n';
    }
    else
    {
        stream << "Exit.PrimaryHotKey=not-created\n"
               << "Exit.FallbackHotKey=not-created\n"
               << "Exit.NotificationIcon=not-created\n";
    }
    stream << "Exit.PollingFallback=enabled\n";

    if (!failure_.empty())
    {
        stream << "Failure=" << failure_ << '\n';
    }
    return stream.str();
}

std::filesystem::path defaultDiagnosticLogPath()
{
    return executableFilePath(
        L"ba-click-fx-desktop-support.log",
        L"ba-click-fx-desktop-support.log");
}

void writeSupportReport(
    const std::filesystem::path& path,
    const SupportReport& report)
{
    if (!path.parent_path().empty())
    {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        throw std::runtime_error(
            "Could not open support report: " + pathToUtf8(path));
    }
    output << report.serialize();
    if (!output)
    {
        throw std::runtime_error(
            "Could not write support report: " + pathToUtf8(path));
    }
}

void appendDiagnosticLog(
    const std::filesystem::path& path,
    const std::string_view event) noexcept
{
    try
    {
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream output(path, std::ios::binary | std::ios::app);
        if (output)
        {
            output << "Event.Utc=" << utcTimestamp() << '\n'
                   << sanitize(event) << "\n---\n";
        }
    }
    catch (...)
    {
        // Diagnostics must never turn a recoverable rendering path into a failure.
    }
}

void appendDiagnosticLog(
    const std::filesystem::path& path,
    const SupportReport& report) noexcept
{
    try
    {
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream output(path, std::ios::binary | std::ios::app);
        if (output)
        {
            output << report.serialize() << "---\n";
        }
    }
    catch (...)
    {
        // Diagnostics must never turn a recoverable rendering path into a failure.
    }
}

std::string captureExclusionDiagnostic(const CaptureExclusionStatus& status)
{
    std::ostringstream stream;
    stream << "Capture.Exclusion.Requested=" << hex32(status.requestedAffinity)
           << ";Observed=" << hex32(status.observedAffinity)
           << ";Set=" << (status.setSucceeded ? "succeeded" : "failed")
           << ";SetError=" << hex32(status.setError)
           << ";Query=" << (status.querySucceeded ? "succeeded" : "failed")
           << ";QueryError=" << hex32(status.queryError);
    return stream.str();
}

}
