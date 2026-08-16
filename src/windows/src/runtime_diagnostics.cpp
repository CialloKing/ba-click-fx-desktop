#include "bafx/windows/runtime_diagnostics.hpp"

#include "bafx/windows/portable_paths.hpp"
#include "bafx/windows/unique_handle.hpp"

#include <windows.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <locale>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

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

[[nodiscard]] std::array<char, 96U> makeDiagnosticSessionId() noexcept
{
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);

    SYSTEMTIME time{};
    GetSystemTime(&time);
    std::array<char, 96U> result{};
    const int written = std::snprintf(
        result.data(),
        result.size(),
        "%04u%02u%02uT%02u%02u%02u.%03uZ-%lu-%llX",
        static_cast<unsigned int>(time.wYear),
        static_cast<unsigned int>(time.wMonth),
        static_cast<unsigned int>(time.wDay),
        static_cast<unsigned int>(time.wHour),
        static_cast<unsigned int>(time.wMinute),
        static_cast<unsigned int>(time.wSecond),
        static_cast<unsigned int>(time.wMilliseconds),
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long long>(counter.QuadPart));
    if (written <= 0 || static_cast<std::size_t>(written) >= result.size())
    {
        constexpr std::string_view fallback = "unknown";
        std::copy(fallback.begin(), fallback.end(), result.begin());
    }
    return result;
}

struct DiagnosticSessionContext
{
    std::array<char, 96U> id{makeDiagnosticSessionId()};
    std::atomic<std::uint64_t> nextSequence{1U};
    std::int64_t startedAtQpc{[]() noexcept
        {
            LARGE_INTEGER counter{};
            return QueryPerformanceCounter(&counter) ? counter.QuadPart : 0LL;
        }()};
    std::int64_t qpcFrequency{[]() noexcept
        {
            LARGE_INTEGER frequency{};
            return QueryPerformanceFrequency(&frequency)
                    && frequency.QuadPart > 0
                ? frequency.QuadPart
                : 0LL;
        }()};
};

[[nodiscard]] DiagnosticSessionContext& diagnosticSession() noexcept
{
    static DiagnosticSessionContext session;
    return session;
}

[[nodiscard]] std::mutex& diagnosticLogMutex() noexcept
{
    static std::mutex mutex;
    return mutex;
}

[[nodiscard]] std::uint64_t diagnosticMonotonicMicroseconds() noexcept
{
    const DiagnosticSessionContext& session = diagnosticSession();
    LARGE_INTEGER counter{};
    if (session.startedAtQpc <= 0
        || session.qpcFrequency <= 0
        || !QueryPerformanceCounter(&counter)
        || counter.QuadPart < session.startedAtQpc)
    {
        return 0U;
    }

    constexpr std::uint64_t microsecondsPerSecond = 1'000'000U;
    const std::uint64_t elapsed = static_cast<std::uint64_t>(
        counter.QuadPart - session.startedAtQpc);
    const std::uint64_t frequency = static_cast<std::uint64_t>(
        session.qpcFrequency);
    return (elapsed / frequency) * microsecondsPerSecond
        + (elapsed % frequency) * microsecondsPerSecond / frequency;
}

[[nodiscard]] std::filesystem::path diagnosticBackupPath(
    const std::filesystem::path& path,
    const std::uint32_t index)
{
    std::filesystem::path backup(path);
    backup += L"." + std::to_wstring(index);
    return backup;
}

void rotateDiagnosticLogUnlocked(
    const std::filesystem::path& path,
    const DiagnosticLogRetention retention) noexcept
{
    constexpr std::uint32_t maximumBackupCount = 16U;
    if (path.empty()
        || retention.maximumBytes == 0U
        || retention.backupCount == 0U
        || retention.backupCount > maximumBackupCount)
    {
        return;
    }

    std::error_code error;
    const std::uintmax_t currentSize = std::filesystem::file_size(path, error);
    if (error || currentSize < retention.maximumBytes)
    {
        return;
    }

    for (std::uint32_t index = retention.backupCount; index > 0U; --index)
    {
        const std::filesystem::path source = index == 1U
            ? path
            : diagnosticBackupPath(path, index - 1U);
        const std::filesystem::path destination = diagnosticBackupPath(path, index);
        error.clear();
        if (!std::filesystem::exists(source, error) || error)
        {
            continue;
        }
        error.clear();
        std::filesystem::remove(destination, error);
        if (error)
        {
            return;
        }
        std::filesystem::rename(source, destination, error);
        if (error)
        {
            return;
        }
    }
}

[[nodiscard]] std::string sanitizeLogValue(const std::string_view value)
{
    std::string result(value);
    for (char& character : result)
    {
        if (character == '\r' || character == '\n' || character == '\0')
        {
            character = ' ';
        }
    }
    return result;
}

[[nodiscard]] std::string sanitizeLogKey(const std::string_view value)
{
    if (value.empty())
    {
        return "Field";
    }

    std::string result(value);
    for (char& character : result)
    {
        const unsigned char code = static_cast<unsigned char>(character);
        if (std::isalnum(code) == 0
            && character != '.'
            && character != '_'
            && character != '-')
        {
            character = '_';
        }
    }
    return result;
}

[[nodiscard]] std::string_view diagnosticLevelName(
    const DiagnosticLevel level) noexcept
{
    switch (level)
    {
    case DiagnosticLevel::Debug:
        return "Debug";
    case DiagnosticLevel::Info:
        return "Info";
    case DiagnosticLevel::Warning:
        return "Warning";
    case DiagnosticLevel::Error:
        return "Error";
    }
    return "Unknown";
}

void appendDiagnosticRecordUnlocked(
    const std::filesystem::path& path,
    const std::string_view eventName,
    const std::span<const DiagnosticField> fields,
    const std::string_view body,
    const DiagnosticLevel level) noexcept
{
    rotateDiagnosticLogUnlocked(path, DiagnosticLogRetention{});

    DiagnosticSessionContext& session = diagnosticSession();
    const std::uint64_t sequence = session.nextSequence.fetch_add(
        1U,
        std::memory_order_relaxed);
    std::ostringstream record;
    record << "Log.SchemaVersion=" << diagnosticLogSchemaVersion << '\n'
           << "Log.SessionId=" << session.id.data() << '\n'
           << "Event.Sequence=" << sequence << '\n'
           << "Event.Utc=" << utcTimestamp() << '\n'
           << "Event.MonotonicUs=" << diagnosticMonotonicMicroseconds() << '\n'
           << "Event.ProcessId=" << GetCurrentProcessId() << '\n'
           << "Event.ThreadId=" << GetCurrentThreadId() << '\n'
           << "Event.Level=" << diagnosticLevelName(level) << '\n'
           << "Event.Name=" << sanitizeLogValue(eventName) << '\n';
    for (const DiagnosticField& field : fields)
    {
        record << sanitizeLogKey(field.key) << '='
               << sanitizeLogValue(field.value) << '\n';
    }
    if (!body.empty())
    {
        record << body;
        if (body.back() != '\n')
        {
            record << '\n';
        }
    }
    record << "---\n";

    const std::string text = record.str();
    if (text.size() > std::numeric_limits<DWORD>::max())
    {
        return;
    }
    const UniqueHandle output(CreateFileW(
        path.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (output.get() == nullptr || output.get() == INVALID_HANDLE_VALUE)
    {
        return;
    }
    DWORD written = 0U;
    static_cast<void>(WriteFile(
        output.get(),
        text.data(),
        static_cast<DWORD>(text.size()),
        &written,
        nullptr));
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

[[nodiscard]] std::string_view outputFormatName(
    const DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return "r16g16b16a16-float";
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return "b8g8r8a8-unorm";
    case DXGI_FORMAT_UNKNOWN:
        return "unknown";
    default:
        return "other";
    }
}

[[nodiscard]] std::string_view outputTransferName(
    const CompositionOutputTransfer transfer) noexcept
{
    switch (transfer)
    {
    case CompositionOutputTransfer::Unknown:
        return "unknown";
    case CompositionOutputTransfer::LinearScRgb:
        return "linear-scrgb";
    case CompositionOutputTransfer::SdrGamma22:
        return "sdr-gamma22";
    }
    return "unknown";
}

[[nodiscard]] std::string_view outputPreferenceName(
    const CompositionOutputPreference preference) noexcept
{
    switch (preference)
    {
    case CompositionOutputPreference::ConservativeSdr:
        return "conservative-sdr";
    case CompositionOutputPreference::PreferLinearScRgb:
        return "prefer-linear-scrgb";
    }
    return "unknown";
}

[[nodiscard]] std::string_view outputFallbackName(
    const CompositionOutputFallback fallback) noexcept
{
    switch (fallback)
    {
    case CompositionOutputFallback::None:
        return "none";
    case CompositionOutputFallback::ConservativeSdr:
        return "conservative-sdr";
    }
    return "unknown";
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

[[nodiscard]] std::string_view colorEncodingName(
    const DISPLAYCONFIG_COLOR_ENCODING encoding) noexcept
{
    switch (encoding)
    {
    case DISPLAYCONFIG_COLOR_ENCODING_RGB:
        return "rgb";
    case DISPLAYCONFIG_COLOR_ENCODING_YCBCR444:
        return "ycbcr444";
    case DISPLAYCONFIG_COLOR_ENCODING_YCBCR422:
        return "ycbcr422";
    case DISPLAYCONFIG_COLOR_ENCODING_YCBCR420:
        return "ycbcr420";
    case DISPLAYCONFIG_COLOR_ENCODING_INTENSITY:
        return "intensity";
    case DISPLAYCONFIG_COLOR_ENCODING_FORCE_UINT32:
        return "unknown";
    }
    return "unknown";
}

[[nodiscard]] std::string_view refreshRateSourceName(
    const DisplayRefreshRateSource source) noexcept
{
    switch (source)
    {
    case DisplayRefreshRateSource::DwmCompositionTiming:
        return "dwm-composition-timing";
    case DisplayRefreshRateSource::DisplayConfigPath:
        return "display-config-path";
    case DisplayRefreshRateSource::DisplayConfigVirtualRefresh:
        return "display-config-virtual-refresh";
    case DisplayRefreshRateSource::DisplayConfigPhysicalRefresh:
        return "display-config-physical-refresh";
    }
    return "unknown";
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

void SupportReport::setPrimaryRefreshRate(
    const DisplayRefreshRate& refreshRate) noexcept
{
    if (refreshRate.numerator == 0U || refreshRate.denominator == 0U)
    {
        primaryRefreshRate_.reset();
        return;
    }
    primaryRefreshRate_ = refreshRate;
}

void SupportReport::setPrimaryDisplayColorCapabilities(
    const DisplayColorCapabilities& capabilities) noexcept
{
    primaryDisplayColorCapabilities_ = capabilities;
}

void SupportReport::clearPrimaryDisplayColorCapabilities() noexcept
{
    primaryDisplayColorCapabilities_.reset();
}

void SupportReport::setPrimaryDisplayColorMonitorResult(
    const DisplayColorMonitorResult& result) noexcept
{
    primaryDisplayColorMonitorResult_ = result;
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

void SupportReport::setDisplayRuntimeSummary(
    const DisplayRuntimeSummary& summary) noexcept
{
    displayRuntimeSummary_ = summary;
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
           << "Support.Scope=multi-display-runtime;fx-only-or-wgc;hardware-validation-not-run\n"
           << "Support.HDR=implemented-not-verified\n"
           << "Support.HDR.Validation=not-run\n"
           << "Support.WGC=" << backgroundStatus() << '\n'
           << "Support.MultiDisplay=implemented-not-verified\n"
           << "Support.MultiDisplay.Validation=not-run\n"
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
    stream << '\n';
    if (displayRuntimeSummary_.has_value())
    {
        const DisplayRuntimeSummary& summary = *displayRuntimeSummary_;
        stream << "Display.SessionCount=" << summary.sessionCount << '\n'
               << "Display.Output.RequestedPreference="
               << outputPreferenceName(summary.requestedOutputPreference)
               << '\n'
               << "Display.Output.ResolvedPreference="
               << outputPreferenceName(summary.resolvedOutputPreference)
               << '\n'
               << "Display.Output.ActualPreference=";
        if (summary.actualOutputPreference.has_value())
        {
            stream << outputPreferenceName(
                *summary.actualOutputPreference);
        }
        else
        {
            stream << "unknown";
        }
        stream << '\n'
               << "Display.Output.PreferenceSatisfied="
               << (summary.outputPreferenceSatisfied ? "true" : "false")
               << '\n'
               << "Display.ColorSnapshotComplete="
               << (summary.colorSnapshotComplete ? "true" : "false") << '\n'
               << "Display.HdrCapabilityObserved="
               << (summary.hdrCapabilityObserved ? "true" : "false") << '\n'
               << "Display.HdrActive="
               << (summary.hdrActive ? "true" : "false") << '\n';
    }
    else
    {
        stream << "Display.SessionCount=not-observed\n"
               << "Display.Output.RequestedPreference=unknown\n"
               << "Display.Output.ResolvedPreference=unknown\n"
               << "Display.Output.ActualPreference=unknown\n"
               << "Display.Output.PreferenceSatisfied=unknown\n"
               << "Display.ColorSnapshotComplete=unknown\n"
               << "Display.HdrCapabilityObserved=unknown\n"
               << "Display.HdrActive=unknown\n";
    }
    if (primaryRefreshRate_.has_value())
    {
        const DisplayRefreshRate& refresh = *primaryRefreshRate_;
        const double hertz = static_cast<double>(refresh.numerator)
            / static_cast<double>(refresh.denominator);
        const double periodMicroseconds = 1'000'000.0 / hertz;
        stream << "Display.RefreshRateSource="
               << refreshRateSourceName(refresh.source) << '\n'
               << "Display.RefreshRateNumerator=" << refresh.numerator << '\n'
               << "Display.RefreshRateDenominator=" << refresh.denominator << '\n'
               << std::fixed << std::setprecision(3)
               << "Display.RefreshRateHz=" << hertz << '\n'
               << "Display.RefreshPeriodUs=" << periodMicroseconds << '\n';
    }
    else
    {
        stream << "Display.RefreshRateSource=not-probed\n"
               << "Display.RefreshRateNumerator=unknown\n"
               << "Display.RefreshRateDenominator=unknown\n"
               << "Display.RefreshRateHz=unknown\n"
               << "Display.RefreshPeriodUs=unknown\n";
    }
    if (primaryDisplayColorMonitorResult_.has_value())
    {
        const DisplayColorMonitorResult& monitor =
            *primaryDisplayColorMonitorResult_;
        stream << "Display.ColorMonitor="
               << displayColorMonitorStatusName(monitor.status) << '\n'
               << "Display.ColorMonitorHRESULT="
               << hex32(static_cast<std::uint32_t>(monitor.error)) << '\n'
               << "Display.ColorMonitorGeneration="
               << monitor.generation << '\n';
    }
    else
    {
        stream << "Display.ColorMonitor=not-probed\n"
               << "Display.ColorMonitorHRESULT=unknown\n"
               << "Display.ColorMonitorGeneration=unknown\n";
    }
    stream << "Display.ColorMode=";
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
                << "Display.ActiveColorMode="
                << displayColorModeName(color.activeColorMode) << '\n'
                << "Display.AdvancedColorProbe="
                << (color.advancedColorInfoV2
                        ? "advanced-color-info-2"
                        : (color.advancedColorQueryResult == ERROR_SUCCESS
                            ? "advanced-color-info-legacy"
                            : "unavailable"))
                << '\n'
                << "Display.AdvancedColorQueryResult="
                << hex32(static_cast<std::uint32_t>(
                       color.advancedColorQueryResult))
                << '\n'
                << "Display.AdvancedColorSupported="
                << (color.advancedColorSupported ? "true" : "false") << '\n'
                << "Display.AdvancedColorActive="
                << (color.advancedColorActive ? "true" : "false") << '\n'
                << "Display.AdvancedColorLimitedByPolicy="
                << (color.advancedColorLimitedByPolicy ? "true" : "false")
                << '\n'
                << "Display.AdvancedColorStateConsistent="
                << (color.displayPathResolved
                        ? (color.advancedColorStateConsistent
                            ? "true"
                            : "false")
                        : "unknown")
                << '\n'
                << "Display.AdvancedColorBitsPerChannel=";
        if (color.displayConfigBitsPerColorChannel > 0U)
        {
            stream << color.displayConfigBitsPerColorChannel;
        }
        else
        {
            stream << "unknown";
        }
        stream << '\n'
                << "Display.HdrSupported="
                << (color.advancedColorInfoV2
                        ? (color.highDynamicRangeSupported ? "true" : "false")
                        : "unknown")
                << '\n'
                << "Display.HdrUserEnabled="
                << (color.advancedColorInfoV2
                        ? (color.highDynamicRangeUserEnabled ? "true" : "false")
                        : "unknown")
                << '\n'
                << "Display.WideColorSupported="
                << (color.advancedColorInfoV2
                        ? (color.wideColorSupported ? "true" : "false")
                        : "unknown")
                << '\n'
                << "Display.WideColorUserEnabled="
                << (color.advancedColorInfoV2
                        ? (color.wideColorUserEnabled ? "true" : "false")
                        : "unknown")
                << '\n'
                << "Display.ColorPathResolved="
                << (color.displayPathResolved ? "true" : "false") << '\n'
                << "Display.ColorPathPhysicalTargetCount=";
        if (color.displayPathResolved)
        {
            stream << color.physicalTargetCount;
        }
        else
        {
            stream << "unknown";
        }
        stream << '\n'
                << "Display.ColorPathAdapterConsistent="
                << (color.displayPathResolved
                        ? (color.physicalTargetAdaptersConsistent
                            ? "true"
                            : "false")
                        : "unknown")
                << '\n'
                << "Display.ColorPathAdapterLuid="
                << (!color.displayPathResolved
                        ? "unknown"
                        : (color.physicalTargetAdaptersConsistent
                            ? luid(color.adapterLuid)
                            : "multiple"))
                << '\n'
                << "Display.ColorPathTargetId=";
        if (!color.displayPathResolved)
        {
            stream << "unknown";
        }
        else if (color.physicalTargetCount == 1U)
        {
            stream << color.targetId;
        }
        else
        {
            stream << "multiple";
        }
        stream << '\n'
                << "Display.ColorEncoding="
                << colorEncodingName(color.colorEncoding) << '\n'
                << "Display.SdrWhiteLevelQueryResult="
                << hex32(static_cast<std::uint32_t>(
                       color.sdrWhiteLevelQueryResult))
                << '\n'
                << "Display.SdrWhiteLevelConsistent="
                << (color.displayPathResolved
                        ? (color.sdrWhiteLevelConsistent ? "true" : "false")
                        : "unknown")
                << '\n'
                << "Display.SdrWhiteLevelNits=";
        if (color.sdrWhiteLevelValid)
        {
            stream << std::fixed << std::setprecision(3)
                   << color.sdrWhiteLevelNits;
        }
        else
        {
            stream << "unknown";
        }
        stream << '\n'
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
                << "Display.ActiveColorMode=unknown\n"
                << "Display.AdvancedColorProbe=not-probed\n"
                << "Display.AdvancedColorQueryResult=unknown\n"
                << "Display.AdvancedColorSupported=unknown\n"
                << "Display.AdvancedColorActive=unknown\n"
                << "Display.AdvancedColorLimitedByPolicy=unknown\n"
                << "Display.AdvancedColorStateConsistent=unknown\n"
                << "Display.AdvancedColorBitsPerChannel=unknown\n"
                << "Display.HdrSupported=unknown\n"
                << "Display.HdrUserEnabled=unknown\n"
                << "Display.WideColorSupported=unknown\n"
                << "Display.WideColorUserEnabled=unknown\n"
                << "Display.ColorPathResolved=unknown\n"
                << "Display.ColorPathPhysicalTargetCount=unknown\n"
                << "Display.ColorPathAdapterConsistent=unknown\n"
                << "Display.ColorPathAdapterLuid=unknown\n"
                << "Display.ColorPathTargetId=unknown\n"
                << "Display.ColorEncoding=unknown\n"
                << "Display.SdrWhiteLevelQueryResult=unknown\n"
                << "Display.SdrWhiteLevelConsistent=unknown\n"
                << "Display.SdrWhiteLevelNits=unknown\n"
                << "Display.MinLuminanceNits=unknown\n"
               << "Display.MaxLuminanceNits=unknown\n"
               << "Display.MaxFullFrameLuminanceNits=unknown\n";
    }
    stream << "Log.Path=" << (logPath_.empty() ? "unknown" : logPath_) << '\n'
           << "Log.SchemaVersion=" << diagnosticLogSchemaVersion << '\n'
           << "Log.SessionId=" << diagnosticSessionId() << '\n';

    if (hasDeviceInfo_)
    {
        stream << "Graphics.DriverType=" << driverType(deviceInfo_.driverType) << '\n'
               << "Graphics.Adapter="
               << sanitize(wideToUtf8(deviceInfo_.adapterDescription)) << '\n'
               << "Graphics.AdapterLuid=" << luid(deviceInfo_.adapterLuid) << '\n'
               << "Graphics.RequestedAdapterLuid=";
        if (deviceInfo_.requestedAdapterLuid.has_value())
        {
            stream << luid(*deviceInfo_.requestedAdapterLuid);
        }
        else
        {
            stream << "default";
        }
        stream << '\n'
               << "Graphics.RequestedAdapterFound="
               << (deviceInfo_.requestedAdapterFound ? "true" : "false")
               << '\n'
               << "Graphics.RequestedAdapterMatched="
               << (deviceInfo_.requestedAdapterMatched ? "true" : "false")
               << '\n'
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
               << "Graphics.OutputFormat="
               << outputFormatName(deviceInfo_.output.format) << '\n'
               << "Graphics.OutputFormatValue="
               << hex32(static_cast<std::uint32_t>(
                      deviceInfo_.output.format))
               << '\n'
               << "Graphics.OutputColorSpace="
               << colorSpaceName(deviceInfo_.output.colorSpace) << '\n'
               << "Graphics.OutputColorSpaceValue="
               << hex32(static_cast<std::uint32_t>(
                      deviceInfo_.output.colorSpace))
               << '\n'
               << "Graphics.OutputTransfer="
               << outputTransferName(deviceInfo_.output.transfer) << '\n'
               << "Graphics.OutputPreference="
               << outputPreferenceName(deviceInfo_.outputPreference) << '\n'
               << "Graphics.OutputPreferenceSatisfied="
               << (compositionOutputSatisfiesPreference(
                       deviceInfo_.output,
                       deviceInfo_.outputPreference)
                       ? "true"
                       : "false")
               << '\n'
               << "Graphics.OutputExtendedPremultiplied="
               << (deviceInfo_.output.extendedPremultiplied
                       ? "true"
                       : "false")
               << '\n'
               << "Graphics.OutputFallback="
               << outputFallbackName(deviceInfo_.output.fallback) << '\n'
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

std::string_view diagnosticSessionId() noexcept
{
    return diagnosticSession().id.data();
}

void rotateDiagnosticLog(
    const std::filesystem::path& path,
    const DiagnosticLogRetention retention) noexcept
{
    try
    {
        const std::lock_guard lock(diagnosticLogMutex());
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }
        rotateDiagnosticLogUnlocked(path, retention);
    }
    catch (...)
    {
        // Logging remains best-effort when retention maintenance is unavailable.
    }
}

void appendDiagnosticEvent(
    const std::filesystem::path& path,
    const std::string_view eventName,
    const std::span<const DiagnosticField> fields,
    const DiagnosticLevel level) noexcept
{
    try
    {
        const std::lock_guard lock(diagnosticLogMutex());
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }
        appendDiagnosticRecordUnlocked(path, eventName, fields, {}, level);
    }
    catch (...)
    {
        // Diagnostics must never turn a recoverable rendering path into a failure.
    }
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
    const std::array fields{
        DiagnosticField{"Event.Message", event}};
    appendDiagnosticEvent(path, "Message", fields);
}

void appendDiagnosticLog(
    const std::filesystem::path& path,
    const SupportReport& report) noexcept
{
    try
    {
        const std::lock_guard lock(diagnosticLogMutex());
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(path.parent_path());
        }
        appendDiagnosticRecordUnlocked(
            path,
            "SupportReport",
            {},
            report.serialize(),
            DiagnosticLevel::Info);
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

std::string captureExclusionQueryDiagnostic(
    const CaptureExclusionQueryStatus& status)
{
    std::ostringstream stream;
    stream << "Capture.Exclusion.Health.Expected="
           << hex32(status.expectedAffinity)
           << ";Observed=" << hex32(status.observedAffinity)
           << ";Query=" << (status.querySucceeded ? "succeeded" : "failed")
           << ";QueryError=" << hex32(status.queryError)
           << ";Confirmed=" << (status.confirmed() ? "true" : "false");
    return stream.str();
}

}
