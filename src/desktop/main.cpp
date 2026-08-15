#include "bafx/desktop/version.hpp"
#include "bafx/config/config.hpp"
#include "bafx/fx/simulation_runtime.hpp"
#include "bafx/fx/simulation_timeline.hpp"
#include "bafx/windows/composition_renderer.hpp"
#include "bafx/windows/display_capabilities.hpp"
#include "bafx/windows/display_color_monitor.hpp"
#include "bafx/windows/error.hpp"
#include "bafx/windows/overlay_window.hpp"
#include "bafx/windows/package_identity.hpp"
#include "bafx/windows/portable_paths.hpp"
#include "bafx/windows/runtime_diagnostics.hpp"
#include "bafx/windows/unique_handle.hpp"
#include "background_capture_runtime.hpp"
#include "display_pointer_router.hpp"
#include "display_session.hpp"
#include "display_session_manager.hpp"
#include "frame_pacing.hpp"
#include "host_control.hpp"
#include "performance_logging.hpp"
#include "performance_window.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr std::uint32_t maximumMessagesPerFrame = 256U;
constexpr std::uint32_t maximumInputMessagesPerFrame = 4096U;
constexpr auto smokeTestDeadline = std::chrono::seconds(5);
constexpr auto performanceReportInterval = std::chrono::seconds(10);
constexpr auto framePacingDeviceProbePeriod = std::chrono::milliseconds(250);
constexpr DWORD activeControlPollMilliseconds = 50U;
constexpr DWORD pausedControlPollMilliseconds = 50U;

[[nodiscard]] std::string formatHresult(const HRESULT result)
{
    std::ostringstream stream;
    stream << "0x"
           << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0')
           << static_cast<unsigned long>(result);
    return stream.str();
}

[[nodiscard]] std::string_view framePacingWakeName(
    const bafx::desktop::FramePacingWake wake) noexcept
{
    switch (wake)
    {
    case bafx::desktop::FramePacingWake::FrameReady:
        return "frame-ready";
    case bafx::desktop::FramePacingWake::DeviceRemoved:
        return "device-removed";
    case bafx::desktop::FramePacingWake::ControlChanged:
        return "control-changed";
    case bafx::desktop::FramePacingWake::CadenceReady:
        return "cadence-ready";
    case bafx::desktop::FramePacingWake::MessagesPending:
        return "messages-pending";
    case bafx::desktop::FramePacingWake::TimedOut:
        return "timed-out";
    case bafx::desktop::FramePacingWake::Failed:
        return "failed";
    }
    return "unknown";
}

[[nodiscard]] std::optional<bafx::core::MonotonicTime>
fixedFramePacingPeriod(const bafx::config::FramePacing pacing) noexcept
{
    std::uint32_t framesPerSecond = 0U;
    switch (pacing)
    {
    case bafx::config::FramePacing::MatchDisplay:
        return std::nullopt;
    case bafx::config::FramePacing::Fixed60:
        framesPerSecond = 60U;
        break;
    case bafx::config::FramePacing::Fixed120:
        framesPerSecond = 120U;
        break;
    case bafx::config::FramePacing::Fixed144:
        framesPerSecond = 144U;
        break;
    }
    if (framesPerSecond == 0U)
    {
        return std::nullopt;
    }

    const std::int64_t second = std::chrono::duration_cast<
        bafx::core::MonotonicTime>(std::chrono::seconds(1)).count();
    return bafx::core::MonotonicTime{
        (second + static_cast<std::int64_t>(framesPerSecond) - 1LL)
        / static_cast<std::int64_t>(framesPerSecond)};
}

[[nodiscard]] DWORD cadenceFallbackTimeoutMilliseconds(
    const bafx::core::MonotonicTime delay) noexcept
{
    if (delay <= bafx::core::MonotonicTime::zero())
    {
        return 0U;
    }
    const auto wholeMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(delay);
    const std::int64_t roundedMilliseconds = wholeMilliseconds.count()
        + (wholeMilliseconds < delay ? 1LL : 0LL);
    return static_cast<DWORD>((std::min)(
        roundedMilliseconds,
        static_cast<std::int64_t>(activeControlPollMilliseconds)));
}

void appendFramePacingDeviceRecoveryDetection(
    const std::filesystem::path& logPath,
    const bafx::desktop::FramePacingWaitResult wait,
    const HRESULT deviceResult,
    const bafx::fx::SimulationTime stalledFor)
{
    const std::string wake = std::string(framePacingWakeName(wait.wake));
    const std::string waitError = std::to_string(wait.error);
    const std::string deviceCode = formatHresult(deviceResult);
    const std::string stalledMicroseconds = std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(
            stalledFor).count());
    const std::array fields{
        bafx::windows::DiagnosticField{"Wake", wake},
        bafx::windows::DiagnosticField{"WaitError", waitError},
        bafx::windows::DiagnosticField{"DeviceHRESULT", deviceCode},
        bafx::windows::DiagnosticField{"StalledUs", stalledMicroseconds}};
    bafx::windows::appendDiagnosticEvent(
        logPath,
        "Graphics.DeviceRecovery.FramePacingDetected",
        fields,
        bafx::windows::DiagnosticLevel::Warning);
}

void appendDeviceRemovedNotificationStatus(
    const std::filesystem::path& logPath,
    const bafx::windows::CompositionRenderer& renderer,
    const std::string_view phase)
{
    const bool available = renderer.deviceRemovedWaitableObject() != nullptr;
    const std::string resultCode = formatHresult(
        renderer.deviceRemovedNotificationResult());
    const std::array fields{
        bafx::windows::DiagnosticField{"Phase", phase},
        bafx::windows::DiagnosticField{
            "Available",
            available ? "true" : "false"},
        bafx::windows::DiagnosticField{
            "RegistrationHRESULT",
            resultCode}};
    bafx::windows::appendDiagnosticEvent(
        logPath,
        "Graphics.DeviceRemovalNotification.Status",
        fields,
        available
            ? bafx::windows::DiagnosticLevel::Info
            : bafx::windows::DiagnosticLevel::Warning);
}

[[nodiscard]] std::string_view backgroundCompositeStatusName(
    const bafx::windows::BackgroundCompositeStatus status) noexcept
{
    using bafx::windows::BackgroundCompositeStatus;
    switch (status)
    {
    case BackgroundCompositeStatus::Inactive:
        return "inactive";
    case BackgroundCompositeStatus::WaitingForFrame:
        return "waiting-for-frame";
    case BackgroundCompositeStatus::SizeMismatch:
        return "size-mismatch";
    case BackgroundCompositeStatus::Stale:
        return "stale";
    case BackgroundCompositeStatus::FutureTimestamp:
        return "future-timestamp";
    case BackgroundCompositeStatus::WrongEpoch:
        return "wrong-epoch";
    case BackgroundCompositeStatus::InvalidContract:
        return "invalid-contract";
    case BackgroundCompositeStatus::InvalidPolicy:
        return "invalid-policy";
    case BackgroundCompositeStatus::CaptureFailed:
        return "capture-failed";
    case BackgroundCompositeStatus::LatchedFxOnly:
        return "latched-fx-only";
    case BackgroundCompositeStatus::Participating:
        return "participating";
    }
    return "unknown";
}

[[nodiscard]] bafx::windows::FxBloomSettings makeBloomSettings(
    const bafx::config::EffectsConfig& effects) noexcept
{
    return bafx::windows::FxBloomSettings{
        effects.bloomIntensity,
        bafx::config::bloomDiffusionForQuality(effects.bloomQuality)};
}

[[nodiscard]] bafx::windows::CompositionOutputPreference makeOutputPreference(
    const bafx::config::DisplayConfig& display) noexcept
{
    return display.hdrEnabled
        ? bafx::windows::CompositionOutputPreference::PreferLinearScRgb
        : bafx::windows::CompositionOutputPreference::ConservativeSdr;
}

[[nodiscard]] bool borderlessAccessMonitoringRequired(
    const bafx::config::Config& config) noexcept
{
    return config.background.mode == bafx::config::RenderMode::BackgroundAware
        && !config.background.allowSystemBorder;
}

void appendBorderlessAccessHealth(
    const std::filesystem::path& logPath,
    const std::string_view phase,
    const bool monitorActive,
    const bafx::windows::BorderlessCaptureAccessHealthResult& result) noexcept
{
    try
    {
        const std::string status(
            bafx::windows::borderlessCaptureAccessStatusName(result.status));
        const std::string resultCode = formatHresult(result.error);
        const std::string generation = std::to_string(result.generation);
        const std::array fields{
            bafx::windows::DiagnosticField{"Phase", phase},
            bafx::windows::DiagnosticField{"Status", status},
            bafx::windows::DiagnosticField{"HRESULT", resultCode},
            bafx::windows::DiagnosticField{"Generation", generation},
            bafx::windows::DiagnosticField{
                "MonitorActive",
                monitorActive ? "true" : "false"}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "WGC.BorderlessAccess.Health",
            fields,
            result.status ==
                    bafx::windows::BorderlessCaptureAccessStatus::Allowed
                ? bafx::windows::DiagnosticLevel::Info
                : bafx::windows::DiagnosticLevel::Warning);
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "WGC borderless access health could not be formatted");
    }
}

struct ResolvedDisplayOutputContract final
{
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    DXGI_COLOR_SPACE_TYPE applicationColorSpace{DXGI_COLOR_SPACE_CUSTOM};
    DXGI_COLOR_SPACE_TYPE displayColorSpace{DXGI_COLOR_SPACE_CUSTOM};
    std::uint32_t bitsPerColor{0U};
    bafx::windows::DisplayColorMode activeColorMode{
        bafx::windows::DisplayColorMode::Unknown};
    DISPLAYCONFIG_COLOR_ENCODING colorEncoding{
        DISPLAYCONFIG_COLOR_ENCODING_FORCE_UINT32};
    bool advancedColorActive{false};

    [[nodiscard]] bool operator==(
        const ResolvedDisplayOutputContract&) const noexcept = default;
};

struct PendingOutputRenegotiation final
{
    bafx::windows::CompositionOutputPreference preference{
        bafx::windows::CompositionOutputPreference::ConservativeSdr};
    std::string reason{};
};

[[nodiscard]] std::optional<ResolvedDisplayOutputContract>
resolveDisplayOutputContract(
    const bafx::windows::CompositionOutputPreference preference,
    const std::optional<bafx::windows::DisplayColorCapabilities>&
        capabilities) noexcept
{
    if (preference
        == bafx::windows::CompositionOutputPreference::ConservativeSdr)
    {
        return ResolvedDisplayOutputContract{
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
            DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
            8U,
            bafx::windows::DisplayColorMode::Sdr,
            DISPLAYCONFIG_COLOR_ENCODING_RGB,
            false};
    }
    if (preference
        != bafx::windows::CompositionOutputPreference::PreferLinearScRgb
        || !capabilities.has_value())
    {
        return std::nullopt;
    }

    // scRGB keeps a fixed application-side FP16 contract. Monitor-side color
    // facts are part of the key because DWM can remap that contract when
    // Advanced Color changes without changing the application's preference.
    return ResolvedDisplayOutputContract{
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
        capabilities->colorSpace,
        capabilities->bitsPerColor,
        capabilities->activeColorMode,
        capabilities->colorEncoding,
        capabilities->advancedColorActive};
}

[[nodiscard]] bool displayOutputContractChanged(
    const bafx::windows::CompositionOutputPreference preference,
    const std::optional<bafx::windows::DisplayColorCapabilities>& previous,
    const std::optional<bafx::windows::DisplayColorCapabilities>& current)
    noexcept
{
    const std::optional<ResolvedDisplayOutputContract> currentContract =
        resolveDisplayOutputContract(preference, current);
    if (!currentContract.has_value())
    {
        // A transient capability-query failure is not evidence that the live
        // output contract changed, so keep the working transport intact.
        return false;
    }
    const std::optional<ResolvedDisplayOutputContract> previousContract =
        resolveDisplayOutputContract(preference, previous);
    return !previousContract.has_value()
        || *previousContract != *currentContract;
}

[[nodiscard]] std::string_view outputPreferenceName(
    const bafx::windows::CompositionOutputPreference preference) noexcept
{
    switch (preference)
    {
    case bafx::windows::CompositionOutputPreference::ConservativeSdr:
        return "conservative-sdr";
    case bafx::windows::CompositionOutputPreference::PreferLinearScRgb:
        return "prefer-linear-scrgb";
    }
    return "unknown";
}

[[nodiscard]] std::string_view outputTransferName(
    const bafx::windows::CompositionOutputTransfer transfer) noexcept
{
    switch (transfer)
    {
    case bafx::windows::CompositionOutputTransfer::Unknown:
        return "unknown";
    case bafx::windows::CompositionOutputTransfer::LinearScRgb:
        return "linear-scrgb";
    case bafx::windows::CompositionOutputTransfer::SdrGamma22:
        return "sdr-gamma22";
    }
    return "unknown";
}

[[nodiscard]] std::string_view outputFallbackName(
    const bafx::windows::CompositionOutputFallback fallback) noexcept
{
    switch (fallback)
    {
    case bafx::windows::CompositionOutputFallback::None:
        return "none";
    case bafx::windows::CompositionOutputFallback::ConservativeSdr:
        return "conservative-sdr";
    }
    return "unknown";
}

[[nodiscard]] std::string_view outputRenegotiationStatusName(
    const bafx::windows::OutputRenegotiationStatus status) noexcept
{
    switch (status)
    {
    case bafx::windows::OutputRenegotiationStatus::RecreatedSameContract:
        return "recreated-same-contract";
    case bafx::windows::OutputRenegotiationStatus::ChangedToLinearScRgb:
        return "changed-to-linear-scrgb";
    case bafx::windows::OutputRenegotiationStatus::ChangedToSdr:
        return "changed-to-sdr";
    }
    return "unknown";
}

void appendOutputRenegotiation(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const std::string_view reason,
    const bafx::windows::OutputRenegotiationResult& result) noexcept
{
    try
    {
        const std::string monitor =
            bafx::desktop::formatDisplayTargetMonitor(session.target());
        const std::string previousFormat = std::to_string(
            static_cast<std::uint32_t>(result.previous.format));
        const std::string currentFormat = std::to_string(
            static_cast<std::uint32_t>(result.current.format));
        const std::string deviceRecovered = result.deviceRecovered
            ? "true"
            : "false";
        const std::array fields{
            bafx::windows::DiagnosticField{"Reason", reason},
            bafx::windows::DiagnosticField{"Monitor", monitor},
            bafx::windows::DiagnosticField{
                "Status",
                outputRenegotiationStatusName(result.status)},
            bafx::windows::DiagnosticField{
                "PreviousPreference",
                outputPreferenceName(result.previousPreference)},
            bafx::windows::DiagnosticField{
                "CurrentPreference",
                outputPreferenceName(result.currentPreference)},
            bafx::windows::DiagnosticField{"PreviousFormat", previousFormat},
            bafx::windows::DiagnosticField{"CurrentFormat", currentFormat},
            bafx::windows::DiagnosticField{
                "PreviousTransfer",
                outputTransferName(result.previous.transfer)},
            bafx::windows::DiagnosticField{
                "CurrentTransfer",
                outputTransferName(result.current.transfer)},
            bafx::windows::DiagnosticField{
                "Fallback",
                outputFallbackName(result.current.fallback)},
            bafx::windows::DiagnosticField{
                "DeviceRecovered",
                deviceRecovered}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "Display.Output.Renegotiated",
            fields,
            result.current.fallback ==
                    bafx::windows::CompositionOutputFallback::None
                ? bafx::windows::DiagnosticLevel::Info
                : bafx::windows::DiagnosticLevel::Warning);
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "Display output renegotiation diagnostics could not be formatted");
    }
}

void appendOutputRenegotiationFailure(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const bafx::windows::CompositionOutputPreference preference,
    const std::string_view reason,
    const std::string_view message,
    const bool deviceRecovered = false) noexcept
{
    try
    {
        const std::string monitor =
            bafx::desktop::formatDisplayTargetMonitor(session.target());
        const std::array fields{
            bafx::windows::DiagnosticField{"Reason", reason},
            bafx::windows::DiagnosticField{"Monitor", monitor},
            bafx::windows::DiagnosticField{
                "RequestedPreference",
                outputPreferenceName(preference)},
            bafx::windows::DiagnosticField{"Message", message},
            bafx::windows::DiagnosticField{
                "DeviceRecovered",
                deviceRecovered ? "true" : "false"}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "Display.Output.RenegotiationFailed",
            fields,
            bafx::windows::DiagnosticLevel::Error);
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "Display output renegotiation failure could not be formatted");
    }
}

void appendOutputRenegotiationDiscarded(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const bafx::desktop::DisplayTarget& queuedTarget,
    const bafx::windows::CompositionOutputPreference preference,
    const std::string_view reason) noexcept
{
    try
    {
        const std::string queuedMonitor =
            bafx::desktop::formatDisplayTargetMonitor(queuedTarget);
        const std::string currentMonitor =
            bafx::desktop::formatDisplayTargetMonitor(session.target());
        const std::string queuedDevice =
            bafx::desktop::displayTargetDeviceUtf8(queuedTarget);
        const std::string currentDevice =
            bafx::desktop::displayTargetDeviceUtf8(session.target());
        const std::array fields{
            bafx::windows::DiagnosticField{"Reason", reason},
            bafx::windows::DiagnosticField{
                "RequestedPreference",
                outputPreferenceName(preference)},
            bafx::windows::DiagnosticField{"Cause", "display-target-changed"},
            bafx::windows::DiagnosticField{"QueuedMonitor", queuedMonitor},
            bafx::windows::DiagnosticField{"CurrentMonitor", currentMonitor},
            bafx::windows::DiagnosticField{"QueuedDevice", queuedDevice},
            bafx::windows::DiagnosticField{"CurrentDevice", currentDevice}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "Display.Output.RenegotiationDiscarded",
            fields);
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "Discarded output renegotiation diagnostics could not be formatted");
    }
}

[[nodiscard]] std::optional<bafx::windows::OutputRenegotiationResult>
tryRenegotiateOutput(
    const std::filesystem::path& logPath,
    bafx::desktop::DisplaySession& session,
    const bafx::windows::CompositionOutputPreference preference,
    const std::string_view reason) noexcept
{
    try
    {
        const bafx::windows::OutputRenegotiationResult result =
            session.renderer().renegotiateOutput(preference);
        appendOutputRenegotiation(logPath, session, reason, result);
        return result;
    }
    catch (const std::exception& error)
    {
        appendOutputRenegotiationFailure(
            logPath,
            session,
            preference,
            reason,
            error.what());
        return std::nullopt;
    }
    catch (...)
    {
        appendOutputRenegotiationFailure(
            logPath,
            session,
            preference,
            reason,
            "unknown exception");
        return std::nullopt;
    }
}

void applyVisualConfig(
    bafx::fx::FrameSnapshot& snapshot,
    const bafx::config::Config& config)
{
    if (!config.effects.enabled)
    {
        snapshot = bafx::fx::FrameSnapshot{};
        return;
    }
    if (!config.effects.clickEnabled)
    {
        snapshot.sprites.clear();
    }
    if (!config.effects.trailEnabled)
    {
        snapshot.trail.clear();
        snapshot.trailStrokes.clear();
        snapshot.trailWidthPixels = 0.0F;
    }

    bafx::fx::applyGlobalScale(snapshot, config.effects.globalScale);
    const float trailScale = config.effects.trailWidth;
    snapshot.trailWidthPixels *= trailScale;
    for (bafx::fx::TrailStroke& stroke : snapshot.trailStrokes)
    {
        stroke.widthPixels *= trailScale;
    }
}

[[nodiscard]] std::uint64_t makeRuntimeSeed() noexcept
{
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const std::uint64_t process = static_cast<std::uint64_t>(GetCurrentProcessId());
    const std::uint64_t tick = static_cast<std::uint64_t>(GetTickCount64());

    // The game enables Unity's automatic particle seed. Mix independent
    // process clocks here while keeping explicit simulation seeds repeatable.
    return static_cast<std::uint64_t>(counter.QuadPart)
        ^ (tick << 21U)
        ^ (process << 48U);
}

class ComApartment final
{
public:
    ComApartment()
    {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (result == RPC_E_CHANGED_MODE)
        {
            throw bafx::windows::HResultError(result, "CoInitializeEx");
        }
        bafx::windows::throwIfFailed(result, "CoInitializeEx");
        initialized_ = true;
    }

    ~ComApartment()
    {
        if (initialized_)
        {
            CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool initialized_{false};
};

class BackgroundCaptureShutdownGuard final
{
public:
    BackgroundCaptureShutdownGuard(
        bafx::windows::CompositionRenderer& renderer,
        const std::filesystem::path& logPath) noexcept
        : renderer_(renderer), logPath_(logPath)
    {
    }

    ~BackgroundCaptureShutdownGuard()
    {
        finalize("exception-unwind");
    }

    BackgroundCaptureShutdownGuard(
        const BackgroundCaptureShutdownGuard&) = delete;
    BackgroundCaptureShutdownGuard& operator=(
        const BackgroundCaptureShutdownGuard&) = delete;

    void finalize(const std::string_view phase) noexcept
    {
        if (finalized_)
        {
            return;
        }
        finalized_ = true;
        renderer_.disableBackgroundCapture();
        bafx::desktop::appendBackgroundCaptureStopDiagnostics(
            logPath_,
            renderer_,
            phase);
        bafx::desktop::appendBackgroundCaptureResourceLedger(
            logPath_,
            renderer_,
            phase);
    }

private:
    bafx::windows::CompositionRenderer& renderer_;
    const std::filesystem::path& logPath_;
    bool finalized_{false};
};

[[nodiscard]] bafx::desktop::HostControlStartResult publishControlService(
    bafx::desktop::HostControlPlane& control,
    bafx::windows::SupportReport& report,
    const std::filesystem::path& logPath,
    const bool backgroundCaptureActive)
{
    const bafx::desktop::HostControlStartResult result =
        control.start(backgroundCaptureActive);
    report.setControlServiceAvailable(result.serviceStarted);
    if (!result.serviceStarted)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            std::string("IPC control service unavailable; continuing without Control Center; error=")
                + std::to_string(control.ipcLastError()));
    }
    else
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "IPC control service started");
    }
    return result;
}

class QpcClock final
{
public:
    QpcClock()
    {
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        {
            bafx::windows::throwLastError("QueryPerformanceFrequency");
        }
        frequency_ = frequency.QuadPart;
    }

    [[nodiscard]] bafx::fx::SimulationTime now() const
    {
        LARGE_INTEGER counter{};
        if (!QueryPerformanceCounter(&counter))
        {
            bafx::windows::throwLastError("QueryPerformanceCounter");
        }
        return fromCounter(counter.QuadPart);
    }

    [[nodiscard]] bafx::fx::SimulationTime fromCounter(const std::int64_t counter) const noexcept
    {
        constexpr std::int64_t nanosecondsPerSecond = 1'000'000'000LL;
        const std::int64_t seconds = counter / frequency_;
        const std::int64_t remainder = counter % frequency_;
        return bafx::fx::SimulationTime{
            seconds * nanosecondsPerSecond
            + remainder * nanosecondsPerSecond / frequency_};
    }

private:
    std::int64_t frequency_{0};
};

struct RunOptions
{
    std::optional<std::uint32_t> frameLimit{};
    std::optional<std::uint32_t> demoAgeMilliseconds{};
    std::optional<std::uint32_t> quitAfterMilliseconds{};
    std::optional<std::filesystem::path> supportInfoPath{};
    bool supportInfoOnly{false};
    bool smokeTest{false};
    bool recoveryProbe{false};
    bool framePacingStallProbe{false};
    bool demoClick{false};
    bool disableRawInput{false};
    std::uint32_t demoDelayMilliseconds{0U};
};

struct MessageDispatchDiagnostics
{
    std::uint32_t inputMessages{0U};
    std::uint32_t otherMessages{0U};
    bool inputBudgetExhausted{false};
    bool otherBudgetExhausted{false};
};

void accumulateMessageDispatch(
    MessageDispatchDiagnostics& total,
    const MessageDispatchDiagnostics sample) noexcept
{
    total.inputMessages += sample.inputMessages;
    total.otherMessages += sample.otherMessages;
    total.inputBudgetExhausted = total.inputBudgetExhausted
        || sample.inputBudgetExhausted;
    total.otherBudgetExhausted = total.otherBudgetExhausted
        || sample.otherBudgetExhausted;
}

struct PointerLatencyOrigin
{
    std::int64_t dispatchQpc{0};
    std::uint32_t messageTimeMilliseconds{0U};
    bool messageTimeValid{false};
};

struct PointerConsumptionDiagnostics
{
    std::vector<PointerLatencyOrigin> acceptedDowns{};
};

[[nodiscard]] RunOptions parseOptions()
{
    RunOptions options{};
    const int argumentCount = __argc;
    for (int index = 1; index < argumentCount; ++index)
    {
        const std::wstring_view argument(__wargv[index]);
        if (argument == L"--smoke-test")
        {
            options.smokeTest = true;
            options.demoClick = true;
            options.demoAgeMilliseconds = 130U;
            options.frameLimit = 3U;
        }
        else if (argument == L"--device-recovery-probe")
        {
            options.recoveryProbe = true;
            options.smokeTest = true;
            options.demoClick = true;
            options.demoAgeMilliseconds = 130U;
            options.frameLimit = 2U;
        }
        else if (argument == L"--frame-pacing-stall-probe")
        {
            // This internal probe replaces the DXGI latency handle with a
            // permanently unsignaled event to verify bounded Host shutdown.
            options.framePacingStallProbe = true;
            options.disableRawInput = true;
        }
        else if (argument == L"--support-info")
        {
            options.supportInfoPath = std::filesystem::path(L"ba-click-fx-support.txt");
            options.supportInfoOnly = true;
        }
        else if (argument.starts_with(L"--support-info="))
        {
            const std::wstring_view value = argument.substr(15);
            options.supportInfoPath = value.empty()
                ? std::filesystem::path(L"ba-click-fx-support.txt")
                : std::filesystem::path(std::wstring(value));
            options.supportInfoOnly = true;
        }
        else if (argument == L"--demo-click")
        {
            options.demoClick = true;
        }
        else if (argument == L"--disable-raw-input")
        {
            // Deterministic renderer baselines provide their own harmless
            // message pressure and must not depend on operator mouse activity.
            options.disableRawInput = true;
        }
        else if (argument.starts_with(L"--frames="))
        {
            const std::wstring_view value = argument.substr(9);
            wchar_t* end = nullptr;
            const unsigned long parsed = std::wcstoul(value.data(), &end, 10);
            if (end != value.data() && *end == L'\0' && parsed > 0UL)
            {
                options.frameLimit = static_cast<std::uint32_t>(parsed);
            }
        }
        else if (argument.starts_with(L"--demo-age-ms="))
        {
            const std::wstring_view value = argument.substr(14);
            wchar_t* end = nullptr;
            const unsigned long parsed = std::wcstoul(value.data(), &end, 10);
            if (end != value.data() && *end == L'\0')
            {
                options.demoClick = true;
                options.demoAgeMilliseconds = static_cast<std::uint32_t>(parsed);
            }
        }
        else if (argument.starts_with(L"--demo-delay-ms="))
        {
            const std::wstring_view value = argument.substr(16);
            wchar_t* end = nullptr;
            const unsigned long parsed = std::wcstoul(value.data(), &end, 10);
            if (end != value.data() && *end == L'\0')
            {
                options.demoClick = true;
                options.demoDelayMilliseconds =
                    static_cast<std::uint32_t>(parsed);
            }
        }
        else if (argument.starts_with(L"--quit-after-ms="))
        {
            const std::wstring_view value = argument.substr(16);
            wchar_t* end = nullptr;
            const unsigned long parsed = std::wcstoul(value.data(), &end, 10);
            if (end != value.data() && *end == L'\0' && parsed > 0UL)
            {
                options.quitAfterMilliseconds = static_cast<std::uint32_t>(parsed);
            }
        }
    }
    return options;
}

[[nodiscard]] bafx::desktop::DisplayTarget primaryDisplayTarget()
{
    const bafx::desktop::DisplayTargetSnapshot snapshot =
        bafx::desktop::queryDisplayTargets();
    const bafx::desktop::DisplayTarget* const target =
        bafx::desktop::findPrimaryDisplayTarget(snapshot);
    if (target == nullptr)
    {
        throw std::runtime_error(
            "No active display target with positive physical bounds");
    }
    return *target;
}

[[nodiscard]] std::string_view displayTopologyStatusName(
    const bafx::windows::DisplayTopologyStatus status) noexcept
{
    switch (status)
    {
    case bafx::windows::DisplayTopologyStatus::Complete:
        return "complete";
    case bafx::windows::DisplayTopologyStatus::Incomplete:
        return "incomplete";
    case bafx::windows::DisplayTopologyStatus::NoActiveDisplays:
        return "no-active-displays";
    case bafx::windows::DisplayTopologyStatus::QueryFailed:
        return "query-failed";
    }
    return "unknown";
}

void appendDisplaySessionReconcile(
    const std::filesystem::path& logPath,
    const std::string_view phase,
    const bafx::desktop::DisplaySessionReconcileResult& result,
    const std::size_t activeSessionCount) noexcept
{
    try
    {
        const std::string topologyError = std::to_string(result.topologyError);
        const std::string active = std::to_string(activeSessionCount);
        const std::string added = std::to_string(result.added);
        const std::string updated = std::to_string(result.updated);
        const std::string recreated = std::to_string(result.recreated);
        const std::string removed = std::to_string(result.removed);
        const std::string failures = std::to_string(result.failures.size());
        const std::array fields{
            bafx::windows::DiagnosticField{"Phase", phase},
            bafx::windows::DiagnosticField{
                "TopologyStatus",
                displayTopologyStatusName(result.topologyStatus)},
            bafx::windows::DiagnosticField{"TopologyError", topologyError},
            bafx::windows::DiagnosticField{"ActiveSessions", active},
            bafx::windows::DiagnosticField{"Added", added},
            bafx::windows::DiagnosticField{"Updated", updated},
            bafx::windows::DiagnosticField{"Recreated", recreated},
            bafx::windows::DiagnosticField{"Removed", removed},
            bafx::windows::DiagnosticField{
                "RemovalsDeferred",
                result.removalsDeferred ? "true" : "false"},
            bafx::windows::DiagnosticField{"Failures", failures}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "Display.Sessions.Reconciled",
            fields,
            result.failures.empty() && !result.removalsDeferred
                ? bafx::windows::DiagnosticLevel::Info
                : bafx::windows::DiagnosticLevel::Warning);

        for (const bafx::desktop::DisplaySessionFailure& failure :
             result.failures)
        {
            const std::string device =
                bafx::desktop::displayTargetDeviceUtf8(failure.target);
            const std::string monitor =
                bafx::desktop::formatDisplayTargetMonitor(failure.target);
            const std::string bounds =
                bafx::desktop::formatDisplayTargetBounds(failure.target);
            const std::string sourceId = std::to_string(failure.target.sourceId);
            const std::array failureFields{
                bafx::windows::DiagnosticField{"Phase", phase},
                bafx::windows::DiagnosticField{"Operation", failure.operation},
                bafx::windows::DiagnosticField{"Device", device},
                bafx::windows::DiagnosticField{"Monitor", monitor},
                bafx::windows::DiagnosticField{"Bounds", bounds},
                bafx::windows::DiagnosticField{"SourceId", sourceId},
                bafx::windows::DiagnosticField{"Message", failure.message}};
            bafx::windows::appendDiagnosticEvent(
                logPath,
                "Display.Session.Failed",
                failureFields,
                bafx::windows::DiagnosticLevel::Error);
        }
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "Display session reconciliation diagnostics could not be formatted");
    }
}

[[nodiscard]] bool displayTopologyChangeHasSource(
    const bafx::windows::DisplayTopologyChange& change,
    const bafx::windows::DisplayTopologyChangeSource source) noexcept
{
    return (change.sourceMask
        & bafx::windows::displayTopologyChangeSourceMask(source)) != 0U;
}

[[nodiscard]] std::string formatDisplayTopologyChangeSources(
    const bafx::windows::DisplayTopologyChange& change)
{
    std::string sources;
    const auto append = [&sources](const std::string_view source)
    {
        if (!sources.empty())
        {
            sources += '|';
        }
        sources += source;
    };

    if (displayTopologyChangeHasSource(
            change,
            bafx::windows::DisplayTopologyChangeSource::WindowPosition))
    {
        append("window-position");
    }
    if (displayTopologyChangeHasSource(
            change,
            bafx::windows::DisplayTopologyChangeSource::DisplayConfiguration))
    {
        append("display-configuration");
    }
    if (displayTopologyChangeHasSource(
            change,
            bafx::windows::DisplayTopologyChangeSource::Dpi))
    {
        append("dpi");
    }
    if (sources.empty())
    {
        sources = "unknown";
    }
    return sources;
}

[[nodiscard]] std::string formatTopologySuggestedBounds(
    const bafx::windows::DisplayTopologyChange& change)
{
    if (!change.suggestedBoundsValid)
    {
        return "not-provided";
    }

    const RECT& bounds = change.suggestedBounds;
    return std::to_string(bounds.right - bounds.left)
        + 'x' + std::to_string(bounds.bottom - bounds.top)
        + '@' + std::to_string(bounds.left)
        + ',' + std::to_string(bounds.top);
}

void appendDisplayTopologyInvalidated(
    const std::filesystem::path& logPath,
    const std::string_view sessionRole,
    const bafx::desktop::DisplayTarget* const target,
    const bafx::windows::DisplayTopologyChange& change,
    const std::uint32_t effectiveDpi) noexcept
{
    try
    {
        const std::string device = target != nullptr
            ? bafx::desktop::displayTargetDeviceUtf8(*target)
            : "process-global";
        const std::string monitor = target != nullptr
            ? bafx::desktop::formatDisplayTargetMonitor(*target)
            : "not-bound";
        const std::string sourceId = target != nullptr
            ? std::to_string(target->sourceId)
            : "not-bound";
        const std::string sources =
            formatDisplayTopologyChangeSources(change);
        const std::string sourceMask = std::to_string(change.sourceMask);
        const std::string dpiX = change.dpiValid
            ? std::to_string(change.latestDpiX)
            : "not-provided";
        const std::string dpiY = change.dpiValid
            ? std::to_string(change.latestDpiY)
            : "not-provided";
        const std::string suggestedBounds =
            formatTopologySuggestedBounds(change);
        const std::string observedDpi = std::to_string(effectiveDpi);
        const std::array fields{
            bafx::windows::DiagnosticField{"Session", sessionRole},
            bafx::windows::DiagnosticField{"Device", device},
            bafx::windows::DiagnosticField{"Monitor", monitor},
            bafx::windows::DiagnosticField{"SourceId", sourceId},
            bafx::windows::DiagnosticField{"Sources", sources},
            bafx::windows::DiagnosticField{"SourceMask", sourceMask},
            bafx::windows::DiagnosticField{"DpiX", dpiX},
            bafx::windows::DiagnosticField{"DpiY", dpiY},
            bafx::windows::DiagnosticField{
                "DpiValid",
                change.dpiValid ? "true" : "false"},
            bafx::windows::DiagnosticField{
                "SuggestedBounds",
                suggestedBounds},
            bafx::windows::DiagnosticField{
                "SuggestedBoundsValid",
                change.suggestedBoundsValid ? "true" : "false"},
            bafx::windows::DiagnosticField{"EffectiveDpi", observedDpi}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "Display.Topology.Invalidated",
            fields);
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "Display topology invalidation could not be formatted");
    }
}

[[nodiscard]] MessageDispatchDiagnostics dispatchMessages(bool& quit)
{
    MessageDispatchDiagnostics diagnostics{};
    MSG message{};
    std::uint32_t inputDispatched = 0U;
    while (inputDispatched < maximumInputMessagesPerFrame
        && PeekMessageW(
            &message,
            nullptr,
            WM_INPUT,
            WM_INPUT,
            PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
        ++inputDispatched;
    }
    diagnostics.inputMessages = inputDispatched;
    if (inputDispatched == maximumInputMessagesPerFrame)
    {
        diagnostics.inputBudgetExhausted = PeekMessageW(
            &message,
            nullptr,
            WM_INPUT,
            WM_INPUT,
            PM_NOREMOVE) != FALSE;
    }

    std::uint32_t dispatched = 0U;
    while (dispatched < maximumMessagesPerFrame
        && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            quit = true;
            diagnostics.otherMessages = dispatched;
            return diagnostics;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
        ++dispatched;
    }
    diagnostics.otherMessages = dispatched;
    if (dispatched == maximumMessagesPerFrame)
    {
        diagnostics.otherBudgetExhausted = PeekMessageW(
            &message,
            nullptr,
            0U,
            0U,
            PM_NOREMOVE) != FALSE;
    }
    return diagnostics;
}

[[nodiscard]] bafx::fx::Viewport toViewport(const bafx::windows::WindowSize size) noexcept
{
    return bafx::fx::Viewport{size.width, size.height};
}

[[nodiscard]] std::uint64_t durationMicroseconds(
    const std::chrono::nanoseconds duration) noexcept
{
    if (duration <= std::chrono::nanoseconds::zero())
    {
        return 0U;
    }
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
}

[[nodiscard]] bafx::desktop::FramePerformanceSample wgcPerformanceSample(
    const bafx::windows::WgcBackgroundDrainDiagnostics& wgc,
    const std::chrono::nanoseconds drainInclusiveCpu,
    const std::uint64_t producerCallbacks,
    const bool active,
    const bool drainAttempted,
    const bool idleDrainAttempted,
    const bool idleDrainSkipped) noexcept
{
    bafx::desktop::FramePerformanceSample sample{};
    sample.wgcDrainCpuMicroseconds = durationMicroseconds(drainInclusiveCpu);
    sample.wgcOwnedCopySubmitCpuMicroseconds =
        durationMicroseconds(wgc.ownedCopySubmitCpu);
    sample.wgcProducerCallbacks = producerCallbacks;
    sample.wgcFramesAcquired = wgc.framesAcquired;
    sample.wgcFramesSuperseded = wgc.framesSuperseded;
    sample.wgcTimestampRejectedFrames = wgc.timestampRejectedFrames;
    sample.wgcActive = active;
    sample.wgcDrainAttempted = drainAttempted;
    sample.wgcIdleDrainAttempted = idleDrainAttempted;
    sample.wgcIdleDrainSkipped = idleDrainSkipped;
    sample.wgcOwnedCopySubmitted = wgc.ownedCopySubmitted;
    sample.wgcAccepted = wgc.accepted;
    return sample;
}

[[nodiscard]] bafx::desktop::FramePerformanceSample framePerformanceSample(
    const bafx::windows::CompositionFrameDiagnostics& frame,
    const std::uint64_t wgcProducerCallbacks,
    const bool diagnosticReadbackUsed) noexcept
{
    bafx::desktop::FramePerformanceSample sample = wgcPerformanceSample(
        frame.wgc,
        frame.wgcDrainInclusiveCpu,
        wgcProducerCallbacks,
        frame.wgcActive,
        frame.wgcDrainAttempted,
        frame.wgcIdleDrainAttempted,
        frame.wgcIdleDrainSkipped);
    sample.frameTotalCpuMicroseconds = durationMicroseconds(frame.frameTotalCpu);
    sample.backgroundSnapshotSubmitCpuMicroseconds =
        durationMicroseconds(frame.backgroundSnapshotSubmitCpu);
    sample.fxTotalSubmitCpuMicroseconds =
        durationMicroseconds(frame.fx.totalSubmit);
    sample.fxMaterialsSubmitCpuMicroseconds =
        durationMicroseconds(frame.fx.materialsSubmit);
    sample.bloomAndCompositeSubmitCpuMicroseconds =
        durationMicroseconds(frame.fx.bloomAndCompositeSubmit);
    sample.diagnosticReadbackCpuMicroseconds =
        durationMicroseconds(frame.diagnosticReadbackCpu);
    sample.presentCallCpuMicroseconds =
        durationMicroseconds(frame.presentCallCpu);
    sample.backgroundSampleAgeMicroseconds =
        durationMicroseconds(frame.backgroundSampleAge);
    sample.roiVisualBoundsStatus = frame.roi.visualBoundsStatus;
    sample.roiPlanStatus = frame.roi.planStatus;
    sample.roiDirtyRectAvailable = frame.roi.dirtyRectAvailable;
    sample.roiPlanAvailable = frame.roi.planAvailable;
    sample.roiFullScreenPixels = frame.roi.fullScreenPixels;
    sample.roiBloomOutputPixels = frame.roi.bloomOutputPixels;
    sample.roiAlignedWorkPixels = frame.roi.alignedWorkPixels;
    sample.roiGuardX = frame.roi.guardX;
    sample.roiGuardY = frame.roi.guardY;
    sample.roiPhasePeriod = frame.roi.phasePeriod;
    sample.roiDirtyRect = frame.roi.dirtyRect;
    sample.roiBloomOutput = frame.roi.bloomOutput;
    sample.roiAlignedWork = frame.roi.alignedWork;
    sample.backgroundSnapshotRefreshAttempted =
        frame.backgroundSnapshotRefreshAttempted;
    sample.backgroundSnapshotRefreshed = frame.backgroundSnapshotRefreshed;
    sample.backgroundParticipated = frame.backgroundParticipated;
    sample.backgroundSampleAgeValid = frame.backgroundSampleAgeValid;
    sample.diagnosticReadbackUsed = diagnosticReadbackUsed;

    sample.gpuTimestampProfilerObserved = true;
    sample.gpuTimestampProfilerAvailable =
        frame.gpuTimestampProfilerAvailable;
    sample.gpuTimestampInitializationResult = static_cast<std::uint32_t>(
        frame.gpuTimestampInitializationResult);
    sample.gpuTimestampPendingFrames = static_cast<std::uint32_t>(
        frame.gpuTimestampPendingFrames);
    sample.gpuFrameStarted = frame.gpuTimestampBegin
        == bafx::windows::GpuTimestampBeginStatus::Started;
    sample.gpuFrameSubmitted = frame.gpuTimestampEnd
        == bafx::windows::GpuTimestampEndStatus::Submitted;
    sample.gpuPollPending = frame.gpuTimestampPoll.status
        == bafx::windows::GpuTimestampPollStatus::Pending;
    sample.gpuRingFullSkipped = frame.gpuTimestampBegin
        == bafx::windows::GpuTimestampBeginStatus::RingFullSkipped;
    sample.gpuCancelledSlotReclaimed = frame.gpuTimestampPoll.status
        == bafx::windows::GpuTimestampPollStatus::Cancelled;
    sample.gpuDisjointSample = frame.gpuTimestampPoll.status
        == bafx::windows::GpuTimestampPollStatus::Disjoint;
    sample.gpuQueryFailure = frame.gpuTimestampPoll.status
        == bafx::windows::GpuTimestampPollStatus::QueryFailure;
    sample.gpuStateError = frame.gpuTimestampCheckpointFailure
        || frame.gpuTimestampBegin
            == bafx::windows::GpuTimestampBeginStatus::AlreadyActive
        || frame.gpuTimestampEnd
            == bafx::windows::GpuTimestampEndStatus::IncompleteCancelled
        || frame.gpuTimestampPoll.status
            == bafx::windows::GpuTimestampPollStatus::ActiveFrame
        || frame.gpuTimestampPoll.status
            == bafx::windows::GpuTimestampPollStatus::AlreadyPolled;

    if (frame.gpuTimestampPoll.sample.has_value())
    {
        const bafx::windows::GpuTimestampSample& gpu =
            *frame.gpuTimestampPoll.sample;
        sample.gpuSampleCompleted = true;
        sample.gpuWgcDrainAndCopyMicroseconds =
            durationMicroseconds(gpu.wgcDrainAndCopy);
        sample.gpuBackgroundSnapshotMicroseconds =
            durationMicroseconds(gpu.backgroundSnapshot);
        sample.gpuFxMaterialsMicroseconds =
            durationMicroseconds(gpu.fxMaterials);
        sample.gpuBloomAndFinalCompositeMicroseconds =
            durationMicroseconds(gpu.bloomAndFinalComposite);
        sample.gpuTotalFxMicroseconds = durationMicroseconds(gpu.totalFx);
        sample.gpuRenderCommandSpanMicroseconds =
            durationMicroseconds(gpu.totalFrame);
        sample.gpuWgcTimingValid = gpu.usage.wgcDrainAttempted;
        sample.gpuBackgroundSnapshotTimingValid =
            gpu.usage.backgroundSnapshotAttempted;
        sample.gpuFxTimingValid = gpu.usage.visualContent;
    }
    return sample;
}

struct SecondaryRenderSummary final
{
    std::size_t rendered{0U};
    std::size_t notReady{0U};
    std::size_t recovered{0U};
    std::size_t failed{0U};
};

void appendSecondaryRenderFailure(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const std::string_view operation,
    const std::string_view message) noexcept
{
    try
    {
        const std::string device =
            bafx::desktop::displayTargetDeviceUtf8(session.target());
        const std::string monitor =
            bafx::desktop::formatDisplayTargetMonitor(session.target());
        const std::string bounds =
            bafx::desktop::formatDisplayTargetBounds(session.target());
        const std::array fields{
            bafx::windows::DiagnosticField{"Operation", operation},
            bafx::windows::DiagnosticField{"Device", device},
            bafx::windows::DiagnosticField{"Monitor", monitor},
            bafx::windows::DiagnosticField{"Bounds", bounds},
            bafx::windows::DiagnosticField{"Message", message}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "Display.Session.RenderFailed",
            fields,
            bafx::windows::DiagnosticLevel::Error);
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "Secondary display render failure could not be formatted");
    }
}

void appendSecondaryBackgroundCaptureFailure(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const std::string_view operation,
    const std::string_view message) noexcept
{
    try
    {
        const std::string device =
            bafx::desktop::displayTargetDeviceUtf8(session.target());
        const std::string monitor =
            bafx::desktop::formatDisplayTargetMonitor(session.target());
        const std::array fields{
            bafx::windows::DiagnosticField{"Operation", operation},
            bafx::windows::DiagnosticField{"Device", device},
            bafx::windows::DiagnosticField{"Monitor", monitor},
            bafx::windows::DiagnosticField{"Message", message},
            bafx::windows::DiagnosticField{"Fallback", "fx-only"}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "Display.Session.BackgroundCaptureFailed",
            fields,
            bafx::windows::DiagnosticLevel::Error);
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "Secondary background capture failure could not be formatted");
    }
}

void applySecondaryBackgroundCaptureRequest(
    bafx::desktop::DisplaySessionManager& sessions,
    bafx::desktop::DisplaySession& coordinator,
    const bafx::windows::BackgroundCaptureRequest& request,
    const std::uint64_t controlGeneration,
    const std::filesystem::path& logPath) noexcept
{
    for (const auto& ownedSession : sessions.sessions())
    {
        bafx::desktop::DisplaySession& session = *ownedSession;
        if (&session == &coordinator || session.renderFaulted())
        {
            continue;
        }

        try
        {
            if (session.secondaryBackgroundCaptureInitialized())
            {
                session.updateSecondaryBackgroundCaptureRequest(
                    request,
                    controlGeneration);
            }
            else
            {
                session.initializeSecondaryBackgroundCapture(
                    request,
                    controlGeneration,
                    logPath);
            }
        }
        catch (const std::exception& error)
        {
            // WGC is optional per surface. Retire only this transaction and
            // preserve every other display plus this surface's FX-only path.
            session.shutdownSecondaryBackgroundCapture();
            appendSecondaryBackgroundCaptureFailure(
                logPath,
                session,
                "apply-request",
                error.what());
        }
        catch (...)
        {
            session.shutdownSecondaryBackgroundCapture();
            appendSecondaryBackgroundCaptureFailure(
                logPath,
                session,
                "apply-request",
                "unknown exception");
        }
    }
}

void appendSecondaryBackgroundCaptureServiceResult(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const bafx::desktop::DisplaySessionBackgroundCaptureServiceResult& result)
    noexcept;

[[nodiscard]] bool handleSecondaryBorderlessAccessLosses(
    bafx::desktop::DisplaySessionManager& sessions,
    bafx::desktop::DisplaySession& coordinator,
    const bafx::core::MonotonicTime now,
    const std::filesystem::path& logPath) noexcept
{
    bool renderInvalidated = false;
    for (const auto& ownedSession : sessions.sessions())
    {
        bafx::desktop::DisplaySession& session = *ownedSession;
        if (&session == &coordinator
            || session.renderFaulted()
            || !session.secondaryBackgroundCaptureInitialized())
        {
            continue;
        }

        try
        {
            const bafx::desktop::DisplaySessionBackgroundCaptureServiceResult
                result = session.handleSecondaryBorderlessAccessLost(now);
            renderInvalidated = result.renderInvalidated || renderInvalidated;
            appendSecondaryBackgroundCaptureServiceResult(
                logPath,
                session,
                result);
        }
        catch (const std::exception& error)
        {
            session.shutdownSecondaryBackgroundCapture();
            appendSecondaryBackgroundCaptureFailure(
                logPath,
                session,
                "borderless-access-lost",
                error.what());
        }
        catch (...)
        {
            session.shutdownSecondaryBackgroundCapture();
            appendSecondaryBackgroundCaptureFailure(
                logPath,
                session,
                "borderless-access-lost",
                "unknown exception");
        }
    }
    return renderInvalidated;
}

[[nodiscard]] bool retrySecondaryBorderlessAccess(
    bafx::desktop::DisplaySessionManager& sessions,
    bafx::desktop::DisplaySession& coordinator,
    const std::uint64_t controlGeneration,
    const bafx::core::MonotonicTime now,
    const std::filesystem::path& logPath) noexcept
{
    bool renderInvalidated = false;
    for (const auto& ownedSession : sessions.sessions())
    {
        bafx::desktop::DisplaySession& session = *ownedSession;
        if (&session == &coordinator
            || session.renderFaulted()
            || !session.secondaryBackgroundCaptureInitialized())
        {
            continue;
        }

        try
        {
            if (!session.retrySecondaryBorderlessAccess(controlGeneration))
            {
                continue;
            }
            const bafx::desktop::DisplaySessionBackgroundCaptureServiceResult
                result = session.serviceSecondaryBackgroundCapture(now);
            renderInvalidated = true;
            appendSecondaryBackgroundCaptureServiceResult(
                logPath,
                session,
                result);
        }
        catch (const std::exception& error)
        {
            session.shutdownSecondaryBackgroundCapture();
            appendSecondaryBackgroundCaptureFailure(
                logPath,
                session,
                "borderless-access-retry",
                error.what());
        }
        catch (...)
        {
            session.shutdownSecondaryBackgroundCapture();
            appendSecondaryBackgroundCaptureFailure(
                logPath,
                session,
                "borderless-access-retry",
                "unknown exception");
        }
    }
    return renderInvalidated;
}

void appendSecondaryBackgroundCaptureServiceResult(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const bafx::desktop::DisplaySessionBackgroundCaptureServiceResult& result)
    noexcept
{
    if (result.outputRenegotiationDiscarded
        && result.outputRenegotiationTarget.has_value())
    {
        appendOutputRenegotiationDiscarded(
            logPath,
            session,
            *result.outputRenegotiationTarget,
            result.outputRenegotiationPreference,
            result.outputRenegotiationReason);
    }
    if (result.outputRenegotiation.has_value())
    {
        appendOutputRenegotiation(
            logPath,
            session,
            result.outputRenegotiationReason,
            *result.outputRenegotiation);
    }
    if (!result.outputRenegotiationFailure.empty())
    {
        appendOutputRenegotiationFailure(
            logPath,
            session,
            result.outputRenegotiationPreference,
            result.outputRenegotiationReason,
            result.outputRenegotiationFailure,
            result.deviceRecovered);
    }
}

[[nodiscard]] bool secondaryDeviceRemovalPending(
    const bafx::desktop::DisplaySession& session) noexcept
{
    const HANDLE deviceRemoved =
        session.renderer().deviceRemovedWaitableObject();
    if (deviceRemoved == nullptr)
    {
        return false;
    }

    // RegisterDeviceRemovedEvent uses a manual-reset event. Polling it here
    // does not consume the recovery signal that the frame-pacing owner needs.
    return WaitForSingleObject(deviceRemoved, 0U) == WAIT_OBJECT_0;
}

[[nodiscard]] bool serviceSecondaryBackgroundCaptures(
    bafx::desktop::DisplaySessionManager& sessions,
    bafx::desktop::DisplaySession& coordinator,
    const bafx::core::MonotonicTime now,
    const std::filesystem::path& logPath) noexcept
{
    bool renderInvalidated = false;
    for (const auto& ownedSession : sessions.sessions())
    {
        bafx::desktop::DisplaySession& session = *ownedSession;
        if (&session == &coordinator)
        {
            continue;
        }
        if (session.renderFaulted())
        {
            session.shutdownSecondaryBackgroundCapture();
            continue;
        }
        if (!session.secondaryBackgroundCaptureInitialized())
        {
            continue;
        }
        if (secondaryDeviceRemovalPending(session))
        {
            renderInvalidated = true;
            continue;
        }

        try
        {
            const bafx::desktop::DisplaySessionBackgroundCaptureServiceResult
                result = session.serviceSecondaryBackgroundCapture(now);
            renderInvalidated = result.renderInvalidated || renderInvalidated;
            appendSecondaryBackgroundCaptureServiceResult(
                logPath,
                session,
                result);
        }
        catch (const std::exception& error)
        {
            session.shutdownSecondaryBackgroundCapture();
            appendSecondaryBackgroundCaptureFailure(
                logPath,
                session,
                "service",
                error.what());
        }
        catch (...)
        {
            session.shutdownSecondaryBackgroundCapture();
            appendSecondaryBackgroundCaptureFailure(
                logPath,
                session,
                "service",
                "unknown exception");
        }
    }
    return renderInvalidated;
}

[[nodiscard]] bool maintainSecondaryBackgroundCaptures(
    bafx::desktop::DisplaySessionManager& sessions,
    bafx::desktop::DisplaySession& coordinator,
    const std::span<bafx::desktop::DisplaySession*> readySessions,
    const bafx::core::MonotonicTime now,
    const std::filesystem::path& logPath) noexcept
{
    bool renderInvalidated = false;
    for (const auto& ownedSession : sessions.sessions())
    {
        bafx::desktop::DisplaySession& session = *ownedSession;
        if (&session == &coordinator
            || session.renderFaulted()
            || !session.secondaryBackgroundCaptureInitialized()
            || std::find(
                readySessions.begin(),
                readySessions.end(),
                &session) != readySessions.end())
        {
            continue;
        }
        if (secondaryDeviceRemovalPending(session))
        {
            renderInvalidated = true;
            continue;
        }

        try
        {
            if (session.secondaryBackgroundCaptureActive())
            {
                // A WGC callback does not grant a swap-chain slot. Keep only
                // the newest owned sample so a faster capture source cannot
                // accumulate work behind a slower secondary Present cadence.
                const bafx::windows::BackgroundSensorMaintenanceDiagnostics
                    maintenance =
                        session.renderer().serviceBackgroundCapture(now);
                renderInvalidated =
                    (maintenance.wgc.accepted
                        && session.lastPresentedDrawableContent())
                    || renderInvalidated;
            }
            const bafx::desktop::DisplaySessionBackgroundCaptureServiceResult
                result = session.serviceSecondaryBackgroundCapture(now);
            renderInvalidated = result.renderInvalidated || renderInvalidated;
            appendSecondaryBackgroundCaptureServiceResult(
                logPath,
                session,
                result);
        }
        catch (const std::exception& error)
        {
            session.shutdownSecondaryBackgroundCapture();
            appendSecondaryBackgroundCaptureFailure(
                logPath,
                session,
                "maintenance",
                error.what());
        }
        catch (...)
        {
            session.shutdownSecondaryBackgroundCapture();
            appendSecondaryBackgroundCaptureFailure(
                logPath,
                session,
                "maintenance",
                "unknown exception");
        }
    }
    return renderInvalidated;
}

[[nodiscard]] std::string_view secondaryBackgroundRecoveryStatusName(
    const bafx::desktop::DisplaySessionBackgroundRecoveryStatus status) noexcept
{
    switch (status)
    {
    case bafx::desktop::DisplaySessionBackgroundRecoveryStatus::NotRequired:
        return "not-required";
    case bafx::desktop::DisplaySessionBackgroundRecoveryStatus::Queued:
        return "queued";
    case bafx::desktop::DisplaySessionBackgroundRecoveryStatus::Blocked:
        return "blocked";
    }
    return "unknown";
}

void appendSecondaryDeviceRecovery(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const bafx::desktop::DisplaySessionDeviceRecoveryResult& recovery,
    const std::string_view eventName) noexcept
{
    try
    {
        const std::string monitor =
            bafx::desktop::formatDisplayTargetMonitor(session.target());
        const std::array fields{
            bafx::windows::DiagnosticField{"Monitor", monitor},
            bafx::windows::DiagnosticField{
                "Driver",
                session.renderer().deviceInfo().driverType
                        == bafx::windows::GraphicsDriverType::Hardware
                    ? "hardware"
                    : "warp"},
            bafx::windows::DiagnosticField{
                "Adapter",
                recovery.adapterChanged ? "changed" : "same"},
            bafx::windows::DiagnosticField{
                "WgcWasActive",
                recovery.backgroundWasActive ? "true" : "false"},
            bafx::windows::DiagnosticField{
                "WgcRestart",
                secondaryBackgroundRecoveryStatusName(recovery.background)}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            eventName,
            fields,
            bafx::windows::DiagnosticLevel::Warning);
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "Secondary device recovery could not be formatted");
    }
}

[[nodiscard]] bool recoverSecondaryDisplaySession(
    const std::filesystem::path& logPath,
    bafx::desktop::DisplaySession& session,
    const std::string_view failureOperation,
    const std::string_view successEvent,
    const bool validateRemovalReason) noexcept
{
    if (validateRemovalReason)
    {
        const HRESULT removalReason =
            session.renderer().deviceRemovedReason();
        if (!bafx::windows::isDeviceLostResult(removalReason))
        {
            session.markRenderFaulted();
            try
            {
                appendSecondaryRenderFailure(
                    logPath,
                    session,
                    failureOperation,
                    "device removal notification produced unexpected HRESULT "
                        + formatHresult(removalReason));
            }
            catch (...)
            {
                // Preserve per-display isolation even if formatting the
                // unexpected driver result cannot allocate memory.
                appendSecondaryRenderFailure(
                    logPath,
                    session,
                    failureOperation,
                    "device removal notification produced unexpected HRESULT");
            }
            return false;
        }
    }

    const bafx::desktop::DisplaySessionDeviceRecoveryResult recovery =
        session.tryRecoverDevice();
    if (!recovery.recovered)
    {
        session.markRenderFaulted();
        appendSecondaryRenderFailure(
            logPath,
            session,
            failureOperation,
            session.renderer().deviceRecoveryFailure());
        return false;
    }

    session.clearRenderFault();
    appendSecondaryDeviceRecovery(
        logPath,
        session,
        recovery,
        successEvent);
    return true;
}

SecondaryRenderSummary renderSecondarySessions(
    bafx::desktop::DisplaySessionManager& sessions,
    bafx::desktop::DisplaySession& coordinator,
    const std::span<bafx::desktop::DisplaySession*> readySessions,
    const bafx::config::Config& config,
    const bafx::fx::SimulationTime renderTime,
    const bafx::core::MonotonicTime wallTime,
    const bool commitSimulationFrame,
    const std::filesystem::path& logPath)
{
    SecondaryRenderSummary summary{};
    const bafx::core::MonotonicTime minimumFramePeriod =
        fixedFramePacingPeriod(config.performance.framePacing).value_or(
            bafx::core::MonotonicTime::zero());
    for (const auto& ownedSession : sessions.sessions())
    {
        bafx::desktop::DisplaySession& session = *ownedSession;
        if (&session == &coordinator || session.renderFaulted())
        {
            continue;
        }

        bafx::windows::CompositionRenderer& sessionRenderer =
            session.renderer();
        const HANDLE deviceRemoved =
            sessionRenderer.deviceRemovedWaitableObject();
        if (deviceRemoved != nullptr
            && WaitForSingleObject(deviceRemoved, 0U) == WAIT_OBJECT_0)
        {
            if (!recoverSecondaryDisplaySession(
                    logPath,
                    session,
                    "device-recovery",
                    "Display.Session.DeviceRecovered",
                    true))
            {
                ++summary.failed;
                continue;
            }
            ++summary.recovered;
            // The recovered swap chain owns a new latency handle. An
            // opportunity granted by the released handle cannot authorize a
            // Present on this resource domain.
            continue;
        }

        const bool frameReady = std::find(
            readySessions.begin(),
            readySessions.end(),
            &session) != readySessions.end();
        if (!frameReady)
        {
            ++summary.notReady;
            continue;
        }

        bafx::fx::FrameSnapshot snapshot = config.effects.enabled
            ? session.simulation().snapshot(
                toViewport(session.window().size()),
                renderTime)
            : bafx::fx::FrameSnapshot{};
        applyVisualConfig(snapshot, config);
        try
        {
            static_cast<void>(sessionRenderer.renderFrame(
                snapshot,
                wallTime,
                false));
            session.recordPresentedFrame(
                snapshot.hasDrawableContent(),
                wallTime,
                minimumFramePeriod);
            if (commitSimulationFrame)
            {
                session.simulation().onFrameRendered(renderTime);
            }
            ++summary.rendered;
        }
        catch (const bafx::windows::HResultError& error)
        {
            if (bafx::windows::isDeviceLostResult(error.result()))
            {
                if (recoverSecondaryDisplaySession(
                        logPath,
                        session,
                        "render-device-recovery",
                        "Display.Session.RenderDeviceRecovered",
                        false))
                {
                    ++summary.recovered;
                    continue;
                }
                ++summary.failed;
                continue;
            }
            session.markRenderFaulted();
            ++summary.failed;
            appendSecondaryRenderFailure(
                logPath,
                session,
                "render",
                error.what());
        }
        catch (const std::exception& error)
        {
            session.markRenderFaulted();
            ++summary.failed;
            appendSecondaryRenderFailure(
                logPath,
                session,
                "render",
                error.what());
        }
    }
    return summary;
}

int runApplication(
    const HINSTANCE instance,
    const RunOptions options,
    bafx::windows::SupportReport& report,
    const std::filesystem::path& logPath)
{
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
        && GetLastError() != ERROR_ACCESS_DENIED)
    {
        bafx::windows::throwLastError("SetProcessDpiAwarenessContext");
    }

    ComApartment apartment;
    QpcClock clock;
    const bafx::windows::PackageIdentityInfo packageIdentity =
        bafx::windows::queryCurrentPackageIdentity();
    bafx::windows::appendDiagnosticLog(
        logPath,
        bafx::windows::packageIdentityDiagnostic(packageIdentity));
    const std::filesystem::path configPath = bafx::desktop::defaultConfigPath();
    const bafx::config::ConfigLoadResult loadedConfig =
        bafx::config::loadConfig(configPath);
    bafx::config::Config config = loadedConfig.succeeded()
        ? loadedConfig.config
        : bafx::config::defaultConfig();
    if (!loadedConfig.succeeded())
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "Configuration load failed; using in-memory defaults: "
                + loadedConfig.message);
    }
    else if (loadedConfig.status == bafx::config::ConfigStatus::CreatedDefault)
    {
        const bafx::config::ConfigSaveResult saved =
            bafx::config::saveConfigAtomic(configPath, config);
        if (!saved.succeeded())
        {
            bafx::windows::appendDiagnosticLog(
                logPath,
                "Configuration bootstrap save failed: " + saved.message);
        }
    }
    if (options.smokeTest)
    {
        // Smoke must still exercise the renderer even when a user disabled FX.
        config.effects.enabled = true;
    }
    if (options.recoveryProbe)
    {
        // Keep the probe independent from WGC so it measures only the D3D/DComp
        // resource-domain rebuild. It is not a device-removed hardware test.
        config.background.mode = bafx::config::RenderMode::RecordingCompatible;
    }

    bafx::desktop::HostControlPlane control(configPath, config);
    report.setConfigurationSchemaVersion(config.schemaVersion);
    bafx::desktop::DisplayTarget appliedDisplayTarget = primaryDisplayTarget();
    report.setPrimaryMonitor(appliedDisplayTarget.bounds);
    constexpr RECT hostShellBounds{0L, 0L, 1L, 1L};
    bafx::windows::OverlayWindow hostWindow(
        instance,
        hostShellBounds,
        L"ba-click-fx-desktop Host",
        bafx::windows::OverlayWindowOptions::hostShell(
            options.disableRawInput
                ? bafx::windows::RawMouseRegistration::Disabled
                : bafx::windows::RawMouseRegistration::Enabled));
    bafx::windows::BorderlessCaptureAccessAuthority borderlessAccessAuthority(
        packageIdentity);
    bafx::desktop::BackgroundCaptureStopMonitor backgroundStopMonitor(logPath);
    bafx::desktop::DisplaySessionManager displaySessions(
        bafx::desktop::DisplaySessionManagerOptions{
            instance,
            hostWindow.handle(),
            &borderlessAccessAuthority,
            L"ba-click-fx-desktop",
            makeBloomSettings(config.effects),
            backgroundStopMonitor.observer(),
            makeOutputPreference(config.display),
            makeRuntimeSeed(),
            config.effects.trailLength,
            config.input.samplingRateHz,
            config.effects.enabled
                && config.effects.trailEnabled
                && !config.input.trailOnlyWhilePressed});
    bafx::desktop::DisplaySession& displaySession =
        displaySessions.createCoordinator(appliedDisplayTarget);
    bafx::windows::OverlayWindow& window = displaySession.window();
    bafx::windows::CompositionRenderer& renderer = displaySession.renderer();
    bafx::fx::SimulationRuntime& simulation = displaySession.simulation();
    bafx::fx::SimulationTimeline& simulationTimeline =
        displaySession.timeline();
    bafx::windows::DisplayColorMonitor& displayColorMonitor =
        displaySession.colorMonitor();
    bafx::desktop::DisplayPointerRouter pointerRouter;
    const bafx::desktop::PointerTimestampSource pointerTimestamps{
        &clock,
        [](const void* const context, const std::int64_t counter) noexcept
        {
            return static_cast<const QpcClock*>(context)->fromCounter(counter);
        }};
    std::uint32_t appliedDisplayDpi = window.effectiveDpi();
    report.setPrimaryDpi(appliedDisplayDpi);
    if (const auto refreshRate =
            bafx::windows::queryDisplayRefreshRate(
                appliedDisplayTarget.monitor);
        refreshRate.has_value())
    {
        report.setPrimaryRefreshRate(*refreshRate);
    }
    const bafx::windows::DisplayColorMonitorResult displayColorMonitorStart =
        displaySession.colorMonitorResult();
    report.setPrimaryDisplayColorMonitorResult(displayColorMonitorStart);
    bafx::windows::appendDiagnosticLog(
        logPath,
        bafx::windows::displayColorMonitorDiagnostic(
            displayColorMonitorStart));
    if (displaySession.colorCapabilities().has_value())
    {
        report.setPrimaryDisplayColorCapabilities(
            *displaySession.colorCapabilities());
    }
    appendDeviceRemovedNotificationStatus(logPath, renderer, "startup");
    bafx::windows::UniqueHandle framePacingStallHandle;
    if (options.framePacingStallProbe)
    {
        framePacingStallHandle.reset(
            CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (framePacingStallHandle.get() == nullptr)
        {
            bafx::windows::throwLastError(
                "CreateEventW(frame pacing stall probe)");
        }
    }
    bafx::windows::WindowSize appliedOutputSize = window.size();
    report.setDeviceInfo(renderer.deviceInfo());
    report.setExitUiStatus(hostWindow.exitUiStatus());
    if (options.supportInfoOnly)
    {
        static_cast<void>(publishControlService(
            control,
            report,
            logPath,
            false));
        report.setBackgroundCaptureStatus(
            bafx::windows::BackgroundCaptureStatus::NotProbed);
        bafx::windows::appendDiagnosticLog(logPath, report);
        bafx::windows::writeSupportReport(*options.supportInfoPath, report);
        return 0;
    }
    if (options.smokeTest && renderer.deviceInfo().adapterDescription.empty())
    {
        throw std::runtime_error("Desktop smoke test could not identify the D3D11 adapter");
    }
    renderer.setReadbackDiagnostics(options.smokeTest);
    BackgroundCaptureShutdownGuard backgroundShutdown(renderer, logPath);
    bafx::windows::BorderlessCaptureAccessMonitor borderlessAccessMonitor;
    bool borderlessAccessMonitorRequested = false;
    std::optional<bafx::windows::BorderlessCaptureAccessStatus>
        observedBorderlessAccessStatus{};
    const auto configureBorderlessAccessMonitor =
        [&](const bafx::config::Config& requestedConfig,
            const std::string_view phase)
    {
        const bool required =
            borderlessAccessMonitoringRequired(requestedConfig);
        if (required == borderlessAccessMonitorRequested)
        {
            return;
        }

        borderlessAccessMonitorRequested = required;
        observedBorderlessAccessStatus.reset();
        // The system permission is process-wide. A policy boundary invalidates
        // the shared result once; individual display transactions never do.
        borderlessAccessAuthority.invalidate();
        if (!required)
        {
            borderlessAccessMonitor.stop();
            const std::array fields{
                bafx::windows::DiagnosticField{"Phase", phase},
                bafx::windows::DiagnosticField{"State", "stopped"}};
            bafx::windows::appendDiagnosticEvent(
                logPath,
                "WGC.BorderlessAccess.Monitor",
                fields);
            return;
        }

        // start() owns runtime capability probing. An older OS remains a
        // complete build target and records one unsupported result here.
        const bafx::windows::BorderlessCaptureAccessHealthResult result =
            borderlessAccessMonitor.start();
        observedBorderlessAccessStatus = result.status;
        appendBorderlessAccessHealth(
            logPath,
            phase,
            borderlessAccessMonitor.active(),
            result);
    };
    configureBorderlessAccessMonitor(config, "startup");
    // Startup and runtime changes share one finite transaction. A failed
    // request remains terminal until its key or explicit retry token changes.
    bafx::windows::BackgroundCaptureTransition backgroundTransition;
    bafx::windows::BackgroundCaptureRequest appliedBackgroundRequest =
        bafx::desktop::backgroundCaptureRequest(config);
    if (backgroundTransition.beginRequest(appliedBackgroundRequest)
        != bafx::windows::BackgroundCaptureRequestResult::Started)
    {
        throw std::logic_error("Initial background capture request was invalid");
    }
    bafx::desktop::BackgroundCaptureExecutionResult backgroundExecution{};
    const bafx::desktop::BackgroundCaptureExecutionStatus
        initialBackgroundExecutionStatus =
            bafx::desktop::executeBackgroundCaptureTransition(
                backgroundTransition,
                window,
                renderer,
                bafx::desktop::DisplayTargetIntent{
                    appliedDisplayTarget,
                    false},
                control.snapshot().generation,
                borderlessAccessAuthority,
                backgroundExecution,
                logPath);
    if (backgroundExecution.deviceRecovered)
    {
        appendDeviceRemovedNotificationStatus(
            logPath,
            renderer,
            "startup-background-recovery");
    }
    bool backgroundCaptureEnabled = backgroundTransition.effectivePath()
        == bafx::windows::EffectiveBackgroundCapturePath::BackgroundAware;
    report.setBackgroundCaptureStatus(
        bafx::desktop::backgroundCaptureStatus(backgroundTransition.effectivePath()));
    if (initialBackgroundExecutionStatus
        == bafx::desktop::BackgroundCaptureExecutionStatus::Completed)
    {
        bafx::desktop::appendBackgroundCaptureOutcome(
            logPath,
            appliedBackgroundRequest,
            backgroundTransition,
            backgroundExecution,
            renderer);
    }
    bafx::desktop::appendAppliedConfiguration(
        logPath,
        config,
        appliedOutputSize,
        "startup");
    displaySession.show();
    const bafx::desktop::DisplayTargetSnapshot initialDisplayTopology =
        bafx::desktop::queryDisplayTargets();
    const bafx::desktop::DisplaySessionReconcileResult initialReconcile =
        displaySessions.reconcileSecondaries(initialDisplayTopology);
    appendDisplaySessionReconcile(
        logPath,
        "startup",
        initialReconcile,
        displaySessions.sessions().size());
    applySecondaryBackgroundCaptureRequest(
        displaySessions,
        displaySession,
        bafx::desktop::backgroundCaptureRequest(config),
        control.snapshot().generation,
        logPath);

    // A broker prompt may remain pending for user input. Expose the control
    // plane after the non-blocking first service step so WM_INPUT, rendering,
    // shutdown, and a superseding configuration remain responsive.
    const bafx::desktop::HostControlStartResult controlStart =
        publishControlService(
            control,
            report,
            logPath,
            renderer.backgroundCaptureActive());
    std::uint64_t appliedGeneration = controlStart.appliedGeneration;
    bafx::windows::appendDiagnosticLog(logPath, report);

    const bafx::fx::SimulationTime applicationStartedAt = clock.now();
    const auto runtimeDeadlineReached =
        [&](const bafx::fx::SimulationTime now)
    {
        if (options.quitAfterMilliseconds.has_value()
            && now - applicationStartedAt
                >= std::chrono::milliseconds(*options.quitAfterMilliseconds))
        {
            return true;
        }
        if (options.smokeTest
            && now - applicationStartedAt >= smokeTestDeadline)
        {
            // The deadline must remain reachable even when DXGI never grants
            // another frame slot or the message queue stays continuously busy.
            throw std::runtime_error(
                "Desktop smoke test exceeded its five-second deadline");
        }
        return false;
    };
    std::optional<bafx::fx::SimulationTime> demoStartedAt;
    const auto startDemoClick =
        [&](const bafx::fx::SimulationTime time)
        {
            const bafx::fx::Viewport viewport = toViewport(window.size());
            demoStartedAt = time;
            simulation.pointerDown(
                bafx::fx::PointF{
                    static_cast<float>(viewport.width) * 0.5F,
                    static_cast<float>(viewport.height) * 0.5F},
                viewport,
                time);
        };
    if (options.demoClick && options.demoDelayMilliseconds == 0U)
    {
        startDemoClick(applicationStartedAt);
    }

    bool quit = false;
    std::uint32_t renderedFrames = 0;
    std::uint64_t backgroundCompositeFrames = 0U;
    std::uint64_t backgroundRetryToken = appliedBackgroundRequest.retryToken;
    bool backgroundRetryPending = false;
    std::optional<PendingOutputRenegotiation>
        pendingCoordinatorOutputRenegotiation{};
    std::optional<bafx::desktop::DisplayTarget> pendingDisplayTarget{};
    bool backgroundParticipationLogged = false;
    bool backgroundPendingDiagnosticLogged = false;
    bool renderInvalidationPending = false;
    bool lastPresentedDrawableContent = false;
    bool deviceRecoveryConsumed = backgroundExecution.deviceRecovered;
    bool recoveryProbePending = options.recoveryProbe;
    bafx::fx::SimulationTime lastFrameReadyAt = applicationStartedAt;
    bafx::fx::SimulationTime lastFramePacingDeviceProbeAt = applicationStartedAt;
    bafx::fx::SimulationTime performanceWindowStartedAt = applicationStartedAt;
    bafx::desktop::RuntimePerformanceWindow performanceWindow;
    std::chrono::nanoseconds previousPerformanceLogWriteCpu{};
    bafx::desktop::WgcCallbackDeltaTracker wgcCallbackDeltaTracker;
    bafx::desktop::CaptureExclusionHealthPoller
        captureExclusionHealthPoller;
    MessageDispatchDiagnostics pendingMessageDispatch{};
    const auto renegotiateCoordinatorOutput =
        [&](const bafx::windows::CompositionOutputPreference preference,
            const std::string_view reason)
    {
        const bool backgroundCaptureWasActive =
            renderer.backgroundCaptureActive();
        const bool backgroundCaptureRequested =
            config.background.mode
            == bafx::config::RenderMode::BackgroundAware;
        const bool backgroundCaptureRestartRequired =
            backgroundCaptureRequested && backgroundCaptureEnabled;
        if (backgroundCaptureRestartRequired
            && backgroundRetryToken
                == std::numeric_limits<std::uint64_t>::max())
        {
            throw std::runtime_error(
                "WGC reconciliation token exhausted before output renegotiation");
        }

        const bafx::windows::GraphicsDeviceInfo previousDeviceInfo =
            renderer.deviceInfo();
        // WGC shares this D3D resource domain. Retire it before replacing the
        // final swap chain so no callback can retain an old-domain texture.
        renderer.disableBackgroundCapture();
        const bafx::windows::WgcBackgroundStopDiagnostics stopDiagnostics =
            bafx::desktop::appendBackgroundCaptureStopDiagnostics(
                logPath,
                renderer,
                "output-renegotiation");
        backgroundCaptureEnabled = false;
        control.setBackgroundCaptureActive(false);
        backgroundParticipationLogged = false;
        backgroundPendingDiagnosticLogged = false;
        if (!stopDiagnostics.overallSucceeded)
        {
            const std::string monitor =
                bafx::desktop::formatDisplayTargetMonitor(
                    displaySession.target());
            const std::array fields{
                bafx::windows::DiagnosticField{"Reason", reason},
                bafx::windows::DiagnosticField{"Monitor", monitor},
                bafx::windows::DiagnosticField{
                    "RequestedPreference",
                    outputPreferenceName(preference)},
                bafx::windows::DiagnosticField{
                    "WgcWasActive",
                    backgroundCaptureWasActive ? "true" : "false"},
                bafx::windows::DiagnosticField{
                    "WgcReconcile",
                    backgroundCaptureRestartRequired
                        ? "blocked-stop-failed"
                        : "not-required"}};
            bafx::windows::appendDiagnosticEvent(
                logPath,
                "Display.Output.RenegotiationBlocked",
                fields,
                bafx::windows::DiagnosticLevel::Error);
            return false;
        }
        if (backgroundCaptureRestartRequired)
        {
            ++backgroundRetryToken;
            backgroundRetryPending = true;
        }

        const bool recoveryBudgetWasConsumed =
            renderer.deviceRecoveryBudgetConsumed();
        const auto result = tryRenegotiateOutput(
            logPath,
            displaySession,
            preference,
            reason);
        if (!result.has_value())
        {
            const bool recoveryBudgetConsumedByAttempt =
                !recoveryBudgetWasConsumed
                && renderer.deviceRecoveryBudgetConsumed();
            if (recoveryBudgetConsumedByAttempt)
            {
                deviceRecoveryConsumed = true;
                bafx::desktop::appendBackgroundCaptureStopDiagnostics(
                    logPath,
                    renderer,
                    "output-renegotiation-device-recovery-failed");
                appendDeviceRemovedNotificationStatus(
                    logPath,
                    renderer,
                    "output-renegotiation-device-recovery-failed");
                report.setDeviceInfo(renderer.deviceInfo());
                bafx::windows::appendDiagnosticLog(logPath, report);

                const std::string recoveryFailure(
                    renderer.deviceRecoveryFailure());
                if (!recoveryFailure.empty())
                {
                    throw std::runtime_error(
                        "Output renegotiation device recovery failed: "
                        + recoveryFailure);
                }

                const std::array fields{
                    bafx::windows::DiagnosticField{"Reason", reason},
                    bafx::windows::DiagnosticField{
                        "Outcome",
                        "device-recovered-output-not-applied"}};
                bafx::windows::appendDiagnosticEvent(
                    logPath,
                    "Graphics.DeviceRecovery.OutputRenegotiationIncomplete",
                    fields,
                    bafx::windows::DiagnosticLevel::Error);
            }
            return false;
        }

        report.setDeviceInfo(renderer.deviceInfo());
        if (!result->deviceRecovered)
        {
            return true;
        }

        // The explicit stop above already scheduled reconciliation. Recovery
        // only contributes adapter-domain evidence here.
        deviceRecoveryConsumed = true;
        bafx::desktop::appendBackgroundCaptureStopDiagnostics(
            logPath,
            renderer,
            "output-renegotiation-device-recovery");
        appendDeviceRemovedNotificationStatus(
            logPath,
            renderer,
            "output-renegotiation-device-recovery");
        const bool adapterChanged =
            previousDeviceInfo.adapterLuid.LowPart
                != renderer.deviceInfo().adapterLuid.LowPart
            || previousDeviceInfo.adapterLuid.HighPart
                != renderer.deviceInfo().adapterLuid.HighPart;
        const std::string retryTokenText = std::to_string(
            backgroundRetryToken);
        const std::array fields{
            bafx::windows::DiagnosticField{
                "Reason",
                reason},
            bafx::windows::DiagnosticField{
                "Adapter",
                adapterChanged ? "changed" : "same"},
            bafx::windows::DiagnosticField{
                "WgcWasActive",
                backgroundCaptureWasActive ? "true" : "false"},
            bafx::windows::DiagnosticField{
                "WgcRestart",
                backgroundCaptureRestartRequired
                    ? "scheduled"
                    : "not-required"},
            bafx::windows::DiagnosticField{
                "ReconcileToken",
                retryTokenText}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "Graphics.DeviceRecovery.OutputRenegotiationSucceeded",
            fields,
            bafx::windows::DiagnosticLevel::Warning);
        return true;
    };
    // Advanced Color notifications first refresh monitor facts. A changed
    // transport is queued until the WGC owner reaches an idle transaction
    // boundary; duplicate notifications never recreate an unchanged output.
    const auto refreshDisplayColorState =
        [&](const std::string_view reason,
            const std::uint64_t generation,
            const bool scheduleOutputRenegotiation)
        {
            const std::optional<bafx::windows::DisplayColorCapabilities>
                previousCapabilities = displaySession.colorCapabilities();
            const std::string previousMode =
                previousCapabilities.has_value()
                ? std::string(bafx::windows::displayColorModeName(
                    previousCapabilities->activeColorMode))
                : "unknown";
            displaySession.refreshColorCapabilities();
            if (displaySession.colorCapabilities().has_value())
            {
                report.setPrimaryDisplayColorCapabilities(
                    *displaySession.colorCapabilities());
            }
            else
            {
                report.clearPrimaryDisplayColorCapabilities();
            }
            report.setPrimaryDisplayColorMonitorResult(
                displaySession.colorMonitorResult());

            const std::string currentMode =
                displaySession.colorCapabilities().has_value()
                ? std::string(bafx::windows::displayColorModeName(
                    displaySession.colorCapabilities()->activeColorMode))
                : "unknown";
            const std::string monitor =
                bafx::desktop::formatDisplayTargetMonitor(
                    appliedDisplayTarget);
            const std::string generationText = std::to_string(generation);
            const bool outputContractChanged = scheduleOutputRenegotiation
                && displayOutputContractChanged(
                    renderer.outputPreference(),
                    previousCapabilities,
                    displaySession.colorCapabilities());
            if (outputContractChanged)
            {
                pendingCoordinatorOutputRenegotiation =
                    PendingOutputRenegotiation{
                        renderer.outputPreference(),
                        std::string(reason)};
            }
            else if (!scheduleOutputRenegotiation)
            {
                // A completed display retarget already rebuilt or reaffirmed
                // the output for the new monitor; discard stale notifications.
                pendingCoordinatorOutputRenegotiation.reset();
            }
            const std::array fields{
                bafx::windows::DiagnosticField{"Reason", reason},
                bafx::windows::DiagnosticField{"Monitor", monitor},
                bafx::windows::DiagnosticField{"PreviousMode", previousMode},
                bafx::windows::DiagnosticField{"CurrentMode", currentMode},
                bafx::windows::DiagnosticField{
                    "Query",
                    displaySession.colorCapabilities().has_value()
                        ? "succeeded"
                        : "failed"},
                bafx::windows::DiagnosticField{
                    "Generation",
                    generationText},
                bafx::windows::DiagnosticField{
                    "OutputContract",
                    outputContractChanged ? "changed" : "unchanged"},
                bafx::windows::DiagnosticField{
                    "OutputRenegotiation",
                    outputContractChanged ? "queued" : "not-needed"}};
            bafx::windows::appendDiagnosticEvent(
                logPath,
                "Display.ColorState.Refreshed",
                fields);
            bafx::windows::appendDiagnosticLog(logPath, report);
        };
    const auto appendPendingBackgroundSnapshotInvalidation = [&]() noexcept
    {
        const std::optional<bafx::windows::BackgroundSnapshotInvalidation>
            invalidation = renderer.takeBackgroundSnapshotInvalidation();
        if (invalidation.has_value())
        {
            bafx::desktop::appendBackgroundSnapshotInvalidation(
                logPath,
                appliedGeneration,
                *invalidation);
        }
    };
    const auto finishBackgroundCaptureTransaction =
        [&](const std::string_view recoveryPhase)
    {
        if (backgroundExecution.transactionActive)
        {
            throw std::logic_error(
                "Pending background capture transaction cannot be finalized");
        }
        if (backgroundExecution.deviceRecovered)
        {
            appendDeviceRemovedNotificationStatus(
                logPath,
                renderer,
                recoveryPhase);
            deviceRecoveryConsumed = true;
            report.setDeviceInfo(renderer.deviceInfo());
        }
        if (backgroundExecution.outputAdapterRetargeted)
        {
            // A planned display migration starts a new resource domain with a
            // fresh one-shot device recovery budget.
            deviceRecoveryConsumed = false;
            report.setDeviceInfo(renderer.deviceInfo());
            appendDeviceRemovedNotificationStatus(
                logPath,
                renderer,
                backgroundExecution.outputAdapterWarpFallback
                    ? "display-adapter-retarget-warp"
                    : "display-adapter-retarget-hardware");
        }
        if (backgroundExecution.resizedOutputSize.has_value())
        {
            appliedOutputSize = *backgroundExecution.resizedOutputSize;
        }
        if (bafx::desktop::displayTargetBoundsApplied(backgroundExecution))
        {
            const bafx::desktop::DisplayTarget previousDisplayTarget =
                appliedDisplayTarget;
            appliedDisplayTarget = backgroundExecution.targetIntent.target;
            if (pendingDisplayTarget.has_value()
                && bafx::desktop::sameDisplayTarget(
                    *pendingDisplayTarget,
                    appliedDisplayTarget)
                && bafx::desktop::sameDisplaySourceIdentity(
                    *pendingDisplayTarget,
                    appliedDisplayTarget))
            {
                pendingDisplayTarget.reset();
            }
            report.setPrimaryMonitor(appliedDisplayTarget.bounds);
            const std::uint32_t appliedDpi = window.effectiveDpi();
            appliedDisplayDpi = appliedDpi;
            report.setPrimaryDpi(appliedDpi);
            if (const auto refreshRate =
                    bafx::windows::queryDisplayRefreshRate(
                        appliedDisplayTarget.monitor);
                refreshRate.has_value())
            {
                report.setPrimaryRefreshRate(*refreshRate);
            }
            else
            {
                report.setPrimaryRefreshRate({});
            }
            displaySession.acceptAppliedTarget(
                appliedDisplayTarget,
                hostWindow.handle());
            const std::size_t duplicateSessionsRemoved =
                displaySessions.pruneCoordinatorDuplicates();
            if (duplicateSessionsRemoved > 0U)
            {
                const std::string removed = std::to_string(
                    duplicateSessionsRemoved);
                const std::array fields{
                    bafx::windows::DiagnosticField{"Removed", removed},
                    bafx::windows::DiagnosticField{
                        "Reason",
                        "coordinator-target-applied"}};
                bafx::windows::appendDiagnosticEvent(
                    logPath,
                    "Display.Sessions.CoordinatorDuplicatesRemoved",
                    fields);
            }
            const bafx::windows::DisplayColorMonitorResult monitorResult =
                displaySession.colorMonitorResult();
            bafx::windows::appendDiagnosticLog(
                logPath,
                bafx::windows::displayColorMonitorDiagnostic(monitorResult));
            // Register first, then query. A toggle racing the retarget either
            // lands in this snapshot or increments the monitor generation.
            refreshDisplayColorState(
                "display-target-applied",
                0U,
                false);
            bafx::desktop::appendDisplayTopologyApplied(
                logPath,
                backgroundExecution.controlGeneration,
                previousDisplayTarget,
                appliedDisplayTarget,
                appliedDpi);
        }
        backgroundCaptureEnabled = backgroundTransition.effectivePath()
            == bafx::windows::EffectiveBackgroundCapturePath::BackgroundAware;
        report.setBackgroundCaptureStatus(
            bafx::desktop::backgroundCaptureStatus(
                backgroundTransition.effectivePath()));
        bafx::desktop::appendBackgroundCaptureOutcome(
            logPath,
            appliedBackgroundRequest,
            backgroundTransition,
            backgroundExecution,
            renderer);
        backgroundParticipationLogged = false;
        backgroundPendingDiagnosticLogged = false;
        control.setBackgroundCaptureActive(renderer.backgroundCaptureActive());
        bafx::windows::appendDiagnosticLog(logPath, report);
    };
    const auto processPendingBorderlessAccessChange =
        [&](bool& renderInvalidated)
    {
        if (!borderlessAccessMonitorRequested
            || !borderlessAccessMonitor.notificationPending())
        {
            return;
        }

        const std::optional<bafx::windows::BorderlessCaptureAccessStatus>
            previousStatus = observedBorderlessAccessStatus;
        const bafx::windows::BorderlessCaptureAccessHealthResult health =
            borderlessAccessMonitor.observe();
        borderlessAccessAuthority.invalidate();
        observedBorderlessAccessStatus = health.status;
        appendBorderlessAccessHealth(
            logPath,
            "access-changed",
            borderlessAccessMonitor.active(),
            health);

        const bool accessAllowed = health.status
            == bafx::windows::BorderlessCaptureAccessStatus::Allowed;
        const bafx::fx::SimulationTime accessChangedAt = clock.now();
        if (!accessAllowed)
        {
            backgroundRetryPending = false;
            bool coordinatorCleaned = false;
            bafx::desktop::BackgroundCaptureExecutionStatus cleanupStatus =
                bafx::desktop::BackgroundCaptureExecutionStatus::Completed;
            if (backgroundExecution.transactionActive)
            {
                cleanupStatus =
                    bafx::desktop::cancelBackgroundCaptureTransition(
                        backgroundTransition,
                        window,
                        renderer,
                        borderlessAccessAuthority,
                        backgroundExecution,
                        bafx::desktop::
                            BackgroundCaptureCancelResizePolicy::Preserve,
                        "borderless-access-lost",
                        logPath);
                coordinatorCleaned = true;
            }
            else if (backgroundTransition.effectivePath()
                == bafx::windows::
                    EffectiveBackgroundCapturePath::BackgroundAware)
            {
                if (!backgroundTransition.beginBorderlessAccessLost())
                {
                    throw std::logic_error(
                        "Borderless access loss could not enter cleanup transaction");
                }
                cleanupStatus =
                    bafx::desktop::executeBackgroundCaptureTransition(
                        backgroundTransition,
                        window,
                        renderer,
                        bafx::desktop::DisplayTargetIntent{
                            appliedDisplayTarget,
                            false},
                        appliedGeneration,
                        borderlessAccessAuthority,
                        backgroundExecution,
                        logPath);
                coordinatorCleaned = true;
            }
            if (cleanupStatus
                != bafx::desktop::
                    BackgroundCaptureExecutionStatus::Completed)
            {
                throw std::logic_error(
                    "Borderless access cleanup unexpectedly remained pending");
            }
            if (coordinatorCleaned)
            {
                backgroundExecution.sensorFailure =
                    bafx::windows::borderlessCaptureAccessHealthDiagnostic(
                        health);
                finishBackgroundCaptureTransaction(
                    "borderless-access-lost");
                renderInvalidated = true;
            }
            renderInvalidated = handleSecondaryBorderlessAccessLosses(
                displaySessions,
                displaySession,
                accessChangedAt,
                logPath)
                || renderInvalidated;

            const std::array fields{
                bafx::windows::DiagnosticField{
                    "Coordinator",
                    coordinatorCleaned ? "fallback-fx-only" : "unchanged"},
                bafx::windows::DiagnosticField{
                    "Secondary",
                    "fallback-requested"}};
            bafx::windows::appendDiagnosticEvent(
                logPath,
                "WGC.BorderlessAccess.Revoked",
                fields,
                bafx::windows::DiagnosticLevel::Warning);
            return;
        }

        if (!previousStatus.has_value()
            || *previousStatus
                == bafx::windows::BorderlessCaptureAccessStatus::Allowed)
        {
            return;
        }

        bool coordinatorRetryScheduled = false;
        const bool coordinatorRestartEligible =
            renderer.deviceInfo().driverType
                == bafx::windows::GraphicsDriverType::Hardware
            && renderer.backgroundCaptureRestartAllowed();
        if (coordinatorRestartEligible
            && !backgroundExecution.transactionActive
            && (backgroundTransition.effectivePath()
                    != bafx::windows::
                        EffectiveBackgroundCapturePath::BackgroundAware
                || !renderer.backgroundCaptureActive()))
        {
            if (backgroundRetryToken
                == (std::numeric_limits<std::uint64_t>::max)())
            {
                throw std::runtime_error(
                    "WGC retry token exhausted after borderless access recovery");
            }
            ++backgroundRetryToken;
            backgroundRetryPending = true;
            coordinatorRetryScheduled = true;
        }
        const bool secondaryRetryScheduled = retrySecondaryBorderlessAccess(
            displaySessions,
            displaySession,
            appliedGeneration,
            accessChangedAt,
            logPath);
        renderInvalidated = secondaryRetryScheduled || renderInvalidated;

        const std::string retryToken = std::to_string(backgroundRetryToken);
        const std::array fields{
            bafx::windows::DiagnosticField{
                "Coordinator",
                coordinatorRetryScheduled
                    ? "scheduled"
                    : (coordinatorRestartEligible ? "unchanged" : "blocked")},
            bafx::windows::DiagnosticField{
                "Secondary",
                secondaryRetryScheduled ? "scheduled" : "unchanged"},
            bafx::windows::DiagnosticField{"RetryToken", retryToken}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "WGC.BorderlessAccess.Restored",
            fields);
    };
    std::vector<bafx::desktop::FramePacingWaitable> frameWaitables;
    std::vector<bafx::desktop::PausedWaitable> pausedWaitables;
    std::vector<bafx::desktop::DisplaySession*> readyDisplaySessions;
    bafx::windows::UniqueHandle frameCadenceTimer(
        bafx::desktop::createFrameCadenceWaitableTimer());
    DWORD frameCadenceTimerError = frameCadenceTimer.get() != nullptr
        ? ERROR_SUCCESS
        : GetLastError();
    bool frameCadenceTimerFailureLogged = false;
    frameWaitables.reserve(16U);
    pausedWaitables.reserve(16U);
    readyDisplaySessions.reserve(8U);
    while (!quit && !hostWindow.closeRequested())
    {
        accumulateMessageDispatch(pendingMessageDispatch, dispatchMessages(quit));
        hostWindow.pollExitShortcut();
        hostWindow.pollPointerState();
        bafx::desktop::HostStateSnapshot controlState = control.snapshot();
        if (controlState.shutdownRequested
            || quit
            || hostWindow.closeRequested())
        {
            break;
        }
        if (runtimeDeadlineReached(clock.now()))
        {
            break;
        }

        bool renderInvalidated = renderInvalidationPending;
        renderInvalidationPending = false;
        // Access revocation must win over topology reconciliation and every
        // transaction poll so no display can create a Session from stale state.
        processPendingBorderlessAccessChange(renderInvalidated);
        const std::optional<bafx::windows::DisplayTopologyChange>
            hostTopologyChange = hostWindow.takeDisplayTopologyChange();
        const bool hostDisplayTopologyChanged = hostTopologyChange.has_value();
        if (hostTopologyChange.has_value())
        {
            appendDisplayTopologyInvalidated(
                logPath,
                "host-shell",
                nullptr,
                *hostTopologyChange,
                hostWindow.effectiveDpi());
        }
        bool surfaceDisplayTopologyChanged = false;
        bool coordinatorSurfaceColorChanged = false;
        for (const auto& ownedSession : displaySessions.sessions())
        {
            bafx::desktop::DisplaySession& session = *ownedSession;
            const std::optional<bafx::windows::DisplayTopologyChange>
                topologyChange =
                session.window().takeDisplayTopologyChange();
            const bool topologyPending = topologyChange.has_value();
            if (topologyChange.has_value())
            {
                appendDisplayTopologyInvalidated(
                    logPath,
                    "render-surface",
                    &session.target(),
                    *topologyChange,
                    session.window().effectiveDpi());
            }
            surfaceDisplayTopologyChanged = topologyPending
                || surfaceDisplayTopologyChanged;
            const bool colorPending = session.window().takeDisplayColorChange();
            if (&session == &displaySession)
            {
                coordinatorSurfaceColorChanged = colorPending;
                continue;
            }

            bool refreshSecondaryColor = colorPending;
            std::uint64_t secondaryColorGeneration = 0U;
            if (session.colorMonitor().notificationPending())
            {
                secondaryColorGeneration =
                    session.colorMonitor().consumeNotification();
                refreshSecondaryColor = true;
            }
            if (refreshSecondaryColor)
            {
                const std::optional<
                    bafx::windows::DisplayColorCapabilities>
                    previousCapabilities = session.colorCapabilities();
                const std::string previousMode =
                    previousCapabilities.has_value()
                    ? std::string(bafx::windows::displayColorModeName(
                        previousCapabilities->activeColorMode))
                    : "unknown";
                session.refreshColorCapabilities();
                const std::string currentMode =
                    session.colorCapabilities().has_value()
                    ? std::string(bafx::windows::displayColorModeName(
                        session.colorCapabilities()->activeColorMode))
                    : "unknown";
                const std::string monitor =
                    bafx::desktop::formatDisplayTargetMonitor(session.target());
                const std::string device =
                    bafx::desktop::displayTargetDeviceUtf8(session.target());
                const std::string generation = std::to_string(
                    secondaryColorGeneration);
                const std::string_view reason =
                    secondaryColorGeneration == 0U
                    ? "win32-notification"
                    : "advanced-color-event";
                const bafx::windows::CompositionOutputPreference preference =
                    session.renderer().outputPreference();
                const bool outputContractChanged =
                    displayOutputContractChanged(
                        preference,
                        previousCapabilities,
                        session.colorCapabilities());
                std::string_view outputRenegotiation = "not-needed";
                bool applyFxOnlyOutput = false;
                if (outputContractChanged)
                {
                    renderInvalidated = true;
                    if (session.secondaryBackgroundCaptureInitialized())
                    {
                        try
                        {
                            session.requestSecondaryOutputRenegotiation(
                                preference,
                                reason,
                                session.target());
                            outputRenegotiation = "queued";
                        }
                        catch (const std::exception& error)
                        {
                            // A queue-allocation failure must not strand a
                            // working FX-only surface on the old color contract.
                            session.shutdownSecondaryBackgroundCapture();
                            appendSecondaryBackgroundCaptureFailure(
                                logPath,
                                session,
                                "queue-output-renegotiation",
                                error.what());
                            applyFxOnlyOutput = true;
                        }
                        catch (...)
                        {
                            session.shutdownSecondaryBackgroundCapture();
                            appendSecondaryBackgroundCaptureFailure(
                                logPath,
                                session,
                                "queue-output-renegotiation",
                                "unknown exception");
                            applyFxOnlyOutput = true;
                        }
                    }
                    else
                    {
                        applyFxOnlyOutput = true;
                    }

                    if (applyFxOnlyOutput)
                    {
                        const std::optional<
                            bafx::windows::OutputRenegotiationResult> result =
                            tryRenegotiateOutput(
                                logPath,
                                session,
                                preference,
                                reason);
                        if (result.has_value())
                        {
                            outputRenegotiation = "applied-fx-only";
                        }
                        else
                        {
                            outputRenegotiation = "failed";
                        }
                    }
                }
                const std::array fields{
                    bafx::windows::DiagnosticField{"Device", device},
                    bafx::windows::DiagnosticField{"Monitor", monitor},
                    bafx::windows::DiagnosticField{
                        "PreviousMode",
                        previousMode},
                    bafx::windows::DiagnosticField{"CurrentMode", currentMode},
                    bafx::windows::DiagnosticField{
                        "Query",
                        session.colorCapabilities().has_value()
                            ? "succeeded"
                            : "failed"},
                    bafx::windows::DiagnosticField{"Generation", generation},
                    bafx::windows::DiagnosticField{
                        "OutputContract",
                        outputContractChanged ? "changed" : "unchanged"},
                    bafx::windows::DiagnosticField{
                        "OutputRenegotiation",
                        outputRenegotiation}};
                bafx::windows::appendDiagnosticEvent(
                    logPath,
                    "Display.Session.ColorState.Refreshed",
                    fields);
            }
        }
        const bool displayTopologyChanged = hostDisplayTopologyChanged
            || surfaceDisplayTopologyChanged;
        if (surfaceDisplayTopologyChanged && !hostDisplayTopologyChanged)
        {
            // Per-monitor DPI notifications may reach only the affected
            // render surface. Raw Input is process-global on the Host shell,
            // so retire coordinates captured in the previous screen domain.
            hostWindow.invalidatePointerGeometry();
        }
        const bool hostDisplayColorChanged =
            hostWindow.takeDisplayColorChange();
        bool displayColorRefreshPending = hostDisplayColorChanged
            || coordinatorSurfaceColorChanged;
        std::uint64_t displayColorGeneration = 0U;
        if (displayColorMonitor.notificationPending())
        {
            displayColorGeneration =
                displayColorMonitor.consumeNotification();
            displayColorRefreshPending = true;
        }
        if (displayTopologyChanged)
        {
            const bafx::desktop::DisplayTargetSnapshot topology =
                bafx::desktop::queryDisplayTargets();
            const bafx::desktop::DisplaySessionReconcileResult reconcile =
                displaySessions.reconcileSecondaries(topology);
            appendDisplaySessionReconcile(
                logPath,
                "runtime-notification",
                reconcile,
                displaySessions.sessions().size());
            // New topology sessions join the current request independently;
            // existing sessions treat the stable request as a no-op.
            applySecondaryBackgroundCaptureRequest(
                displaySessions,
                displaySession,
                bafx::desktop::backgroundCaptureRequest(config),
                appliedGeneration,
                logPath);

            const bafx::desktop::DisplayTarget& requestedTarget =
                pendingDisplayTarget.has_value()
                    ? *pendingDisplayTarget
                    : appliedDisplayTarget;
            const bafx::desktop::DisplayTarget* observed =
                bafx::desktop::findDisplayTargetBySource(
                    topology,
                    requestedTarget);
            if (observed == nullptr
                && topology.status
                    == bafx::windows::DisplayTopologyStatus::Complete)
            {
                // A partial snapshot cannot revoke an already queued target.
                // Fall back to the applied source only after the pending
                // source is authoritatively absent.
                observed = bafx::desktop::findDisplayTargetBySource(
                    topology,
                    appliedDisplayTarget);
            }
            if (observed == nullptr
                && topology.status
                    == bafx::windows::DisplayTopologyStatus::Complete)
            {
                // Only an authoritative snapshot can prove the old source is
                // absent. A partial hot-plug query must retain the working
                // resource domain instead of redirecting it to today's primary.
                observed = bafx::desktop::findPrimaryDisplayTarget(topology);
            }
            const bafx::desktop::DisplayTarget observedTarget =
                observed != nullptr ? *observed : requestedTarget;
            const bafx::desktop::DisplayTarget& expectedTarget =
                pendingDisplayTarget.has_value()
                    ? *pendingDisplayTarget
                    : appliedDisplayTarget;
            const bafx::desktop::DisplayTarget stabilizedObservedTarget =
                bafx::desktop::stabilizeDisplayTargetObservation(
                    expectedTarget,
                    observedTarget,
                    topology.status);
            bafx::desktop::appendDisplayTopologyObserved(
                logPath,
                backgroundExecution.transactionActive
                    ? backgroundExecution.controlGeneration
                    : appliedGeneration,
                backgroundExecution.transactionActive,
                appliedDisplayTarget,
                appliedDisplayDpi,
                observedTarget,
                window.effectiveDpi());
            const bool observedResourceDomainMismatch =
                !bafx::desktop::displayTargetResourceAdapterMatches(
                    stabilizedObservedTarget,
                    renderer.deviceInfo().adapterLuid,
                    renderer.deviceInfo().driverType
                        == bafx::windows::GraphicsDriverType::Hardware);
            if (!bafx::desktop::sameDisplayTarget(
                    stabilizedObservedTarget,
                    expectedTarget)
                || !bafx::desktop::sameDisplaySourceIdentity(
                    stabilizedObservedTarget,
                    expectedTarget)
                || observedResourceDomainMismatch)
            {
                pendingDisplayTarget = stabilizedObservedTarget;
            }
            else if (!pendingDisplayTarget.has_value())
            {
                // A DPI-only notification can preserve rcMonitor. Reassert the
                // physical fullscreen bounds without restarting a stable WGC
                // target; any actual size correction is consumed below.
                appliedDisplayTarget = stabilizedObservedTarget;
                displaySession.updateTargetMetadata(appliedDisplayTarget);
                window.setBounds(appliedDisplayTarget.bounds);
                appliedDisplayDpi = window.effectiveDpi();
                report.setPrimaryDpi(appliedDisplayDpi);
                if (appliedDisplayTarget.refreshRate.has_value())
                {
                    report.setPrimaryRefreshRate(
                        *appliedDisplayTarget.refreshRate);
                }
                else
                {
                    report.setPrimaryRefreshRate({});
                }

                const bafx::windows::BackgroundCadenceRefreshResult
                    cadence = renderer.refreshBackgroundCadence(
                        appliedDisplayTarget.monitor);
                const auto cadenceStatusName = [](const auto status)
                    -> std::string_view
                {
                    switch (status)
                    {
                    case bafx::windows::BackgroundCadenceRefreshStatus::Inactive:
                        return "inactive";
                    case bafx::windows::BackgroundCadenceRefreshStatus::WrongMonitor:
                        return "wrong-monitor";
                    case bafx::windows::BackgroundCadenceRefreshStatus::TargetRate:
                        return "target-rate";
                    case bafx::windows::BackgroundCadenceRefreshStatus::
                        ConservativeFallback:
                        return "conservative-fallback";
                    }
                    return "unknown";
                };
                const std::string periodMicroseconds = std::to_string(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        cadence.appliedPeriod).count());
                const std::array cadenceFields{
                    bafx::windows::DiagnosticField{
                        "Status",
                        cadenceStatusName(cadence.status)},
                    bafx::windows::DiagnosticField{
                        "AppliedPeriodUs",
                        periodMicroseconds}};
                bafx::windows::appendDiagnosticEvent(
                    logPath,
                    "Display.Cadence.Refreshed",
                    cadenceFields);
            }
        }
        if (displayColorRefreshPending
            && !pendingDisplayTarget.has_value())
        {
            refreshDisplayColorState(
                displayColorGeneration == 0U
                    ? "win32-notification"
                    : "advanced-color-event",
                displayColorGeneration,
                true);
            renderInvalidated = true;
        }
        const bafx::windows::WindowResizeDiagnostics resizeDiagnostics =
            window.takeWindowResizeDiagnostics();
        if (resizeDiagnostics.clientRectQueryFailures > 0U)
        {
            const std::string failureCount = std::to_string(
                resizeDiagnostics.clientRectQueryFailures);
            const std::string lastError = std::to_string(
                resizeDiagnostics.lastClientRectQueryError);
            const std::array fields{
                bafx::windows::DiagnosticField{
                    "FailureCount",
                    failureCount},
                bafx::windows::DiagnosticField{
                    "LastWin32Error",
                    lastError}};
            bafx::windows::appendDiagnosticEvent(
                logPath,
                "Display.Surface.ClientRectQueryFailed",
                fields,
                bafx::windows::DiagnosticLevel::Warning);
        }
        std::optional<bafx::windows::WindowSize> pendingOutputResize =
            window.takePendingResize();
        if (pendingOutputResize.has_value()
            && pendingOutputResize->width == appliedOutputSize.width
            && pendingOutputResize->height == appliedOutputSize.height)
        {
            // Win32 can report the construction size again after ShowWindow.
            // Do not turn that no-op into a needless capture restart.
            pendingOutputResize.reset();
        }
        const bool configChanged = controlState.generation != appliedGeneration;
        if (backgroundExecution.transactionActive)
        {
            const bool displayTargetSupersedesTransaction =
                pendingDisplayTarget.has_value()
                && (!bafx::desktop::sameDisplayTarget(
                        *pendingDisplayTarget,
                        backgroundExecution.targetIntent.target)
                    || !bafx::desktop::sameDisplaySourceIdentity(
                        *pendingDisplayTarget,
                        backgroundExecution.targetIntent.target));
            bafx::desktop::BackgroundCaptureExecutionStatus executionStatus =
                bafx::desktop::BackgroundCaptureExecutionStatus::Pending;
            if (configChanged
                || pendingOutputResize.has_value()
                || displayTargetSupersedesTransaction
                || backgroundRetryPending)
            {
                const auto cancelResizePolicy =
                    bafx::desktop::backgroundCaptureCancelResizePolicy(
                        pendingOutputResize.has_value(),
                        displayTargetSupersedesTransaction);
                const std::string_view cancellationReason = configChanged
                    ? "control-generation"
                    : (displayTargetSupersedesTransaction
                        ? "display-target"
                        : (pendingOutputResize.has_value()
                            ? "output-resize"
                            : "background-retry"));
                executionStatus =
                    bafx::desktop::cancelBackgroundCaptureTransition(
                        backgroundTransition,
                        window,
                        renderer,
                        borderlessAccessAuthority,
                        backgroundExecution,
                        cancelResizePolicy,
                        cancellationReason,
                        logPath);
            }
            else
            {
                executionStatus =
                    bafx::desktop::executeBackgroundCaptureTransition(
                        backgroundTransition,
                        window,
                        renderer,
                        backgroundExecution.targetIntent,
                        backgroundExecution.controlGeneration,
                        borderlessAccessAuthority,
                        backgroundExecution,
                        logPath);
            }
            if (executionStatus
                == bafx::desktop::BackgroundCaptureExecutionStatus::Completed)
            {
                finishBackgroundCaptureTransaction(
                    "pending-background-recovery");
                renderInvalidated = true;
            }
        }
        if (pendingCoordinatorOutputRenegotiation.has_value()
            && !backgroundExecution.transactionActive
            && !backgroundTransition.transitioning()
            && !configChanged
            && !pendingOutputResize.has_value()
            && !pendingDisplayTarget.has_value())
        {
            const PendingOutputRenegotiation pending =
                *pendingCoordinatorOutputRenegotiation;
            pendingCoordinatorOutputRenegotiation.reset();
            renderInvalidated = renegotiateCoordinatorOutput(
                pending.preference,
                pending.reason)
                || renderInvalidated;
        }
        const bool displayTargetChanged = pendingDisplayTarget.has_value()
            && (!bafx::desktop::sameDisplayTarget(
                    *pendingDisplayTarget,
                    appliedDisplayTarget)
                || !bafx::desktop::sameDisplaySourceIdentity(
                    *pendingDisplayTarget,
                    appliedDisplayTarget)
                || !bafx::desktop::displayTargetResourceAdapterMatches(
                    *pendingDisplayTarget,
                    renderer.deviceInfo().adapterLuid,
                    renderer.deviceInfo().driverType
                        == bafx::windows::GraphicsDriverType::Hardware));
        if (configChanged
            || pendingOutputResize.has_value()
            || displayTargetChanged
            || backgroundRetryPending)
        {
            renderInvalidated = true;
            if (configChanged)
            {
                const bafx::config::FramePacing previousFramePacing =
                    config.performance.framePacing;
                const bafx::windows::CompositionOutputPreference
                    previousOutputPreference = makeOutputPreference(
                        config.display);
                config = controlState.config;
                if (config.performance.framePacing != previousFramePacing)
                {
                    // A new policy starts a fresh cadence epoch. Reusing a
                    // deadline from a different rate would delay its first frame.
                    for (const auto& ownedSession : displaySessions.sessions())
                    {
                        ownedSession->resetFramePacing();
                    }
                }
                configureBorderlessAccessMonitor(config, "configuration");
                const bafx::windows::CompositionOutputPreference
                    currentOutputPreference = makeOutputPreference(
                        config.display);
                const bool outputPreferenceChanged =
                    previousOutputPreference != currentOutputPreference;
                const bool alwaysOnTrailEnabled = config.effects.enabled
                    && config.effects.trailEnabled
                    && !config.input.trailOnlyWhilePressed;
                const bafx::windows::FxBloomSettings bloomSettings =
                    makeBloomSettings(config.effects);
                const bafx::fx::SimulationTime settingsTime =
                    simulationTimeline.fromWallTime(clock.now());
                displaySessions.updateCreationSettings(
                    bloomSettings,
                    currentOutputPreference,
                    config.effects.trailLength,
                    config.input.samplingRateHz,
                    alwaysOnTrailEnabled);
                // Host owns the render thread, so applying the immutable control
                // snapshot here makes input, length and Bloom changes take effect
                // on the next frame without cross-thread renderer mutation.
                simulation.setTrailLengthMultiplier(config.effects.trailLength);
                simulation.setInputSamplingRateHz(config.input.samplingRateHz);
                simulation.setAlwaysOnTrailEnabled(
                    alwaysOnTrailEnabled,
                    settingsTime);
                const bool backgroundCaptureWasActive =
                    renderer.backgroundCaptureActive();
                const bafx::windows::GraphicsDeviceInfo previousDeviceInfo =
                    renderer.deviceInfo();
                const bool bloomDeviceRecovered = renderer.setBloomSettings(
                    bloomSettings);
                if (bloomDeviceRecovered)
                {
                    bafx::desktop::appendBackgroundCaptureStopDiagnostics(
                        logPath,
                        renderer,
                        "bloom-device-recovery");
                    appendDeviceRemovedNotificationStatus(
                        logPath,
                        renderer,
                        "bloom-device-recovery");
                    deviceRecoveryConsumed = true;
                    report.setDeviceInfo(renderer.deviceInfo());
                    const bool adapterChanged =
                        previousDeviceInfo.adapterLuid.LowPart
                            != renderer.deviceInfo().adapterLuid.LowPart
                        || previousDeviceInfo.adapterLuid.HighPart
                            != renderer.deviceInfo().adapterLuid.HighPart;
                    const bool retryEligible =
                        bafx::desktop::canRetryBackgroundCaptureAfterDeviceRecovery(
                            config.background.mode
                                == bafx::config::RenderMode::BackgroundAware,
                            backgroundCaptureWasActive,
                            adapterChanged,
                            renderer.deviceInfo().driverType,
                            renderer.backgroundCaptureRestartAllowed());
                    if (retryEligible)
                    {
                        if (backgroundRetryToken
                            == std::numeric_limits<std::uint64_t>::max())
                        {
                            throw std::runtime_error(
                                "WGC retry token exhausted after Bloom resource recovery");
                        }
                        ++backgroundRetryToken;
                    }
                    const std::array recoveryFields{
                        bafx::windows::DiagnosticField{
                            "Adapter",
                            adapterChanged ? "changed" : "same"},
                        bafx::windows::DiagnosticField{
                            "WgcWasActive",
                            backgroundCaptureWasActive ? "true" : "false"},
                        bafx::windows::DiagnosticField{
                            "WgcRetry",
                            retryEligible ? "scheduled" : "blocked"}};
                    bafx::windows::appendDiagnosticEvent(
                        logPath,
                        "Graphics.DeviceRecovery.BloomSettingsSucceeded",
                        recoveryFields);
                }
                for (const auto& ownedSession : displaySessions.sessions())
                {
                    bafx::desktop::DisplaySession& session = *ownedSession;
                    if (&session == &displaySession)
                    {
                        continue;
                    }
                    session.simulation().setTrailLengthMultiplier(
                        config.effects.trailLength);
                    session.simulation().setInputSamplingRateHz(
                        config.input.samplingRateHz);
                    session.simulation().setAlwaysOnTrailEnabled(
                        alwaysOnTrailEnabled,
                        settingsTime);
                    try
                    {
                        const bafx::desktop::DisplaySessionDeviceRecoveryResult
                            recovery = session.setBloomSettings(bloomSettings);
                        session.clearRenderFault();
                        if (recovery.recovered)
                        {
                            appendSecondaryDeviceRecovery(
                                logPath,
                                session,
                                recovery,
                                "Display.Session.BloomDeviceRecovered");
                        }
                    }
                    catch (const std::exception& error)
                    {
                        session.markRenderFaulted();
                        appendSecondaryRenderFailure(
                            logPath,
                            session,
                            "apply-bloom-settings",
                            error.what());
                    }
                }
                applySecondaryBackgroundCaptureRequest(
                    displaySessions,
                    displaySession,
                    bafx::desktop::backgroundCaptureRequest(config),
                    controlState.generation,
                    logPath);
                if (outputPreferenceChanged)
                {
                    pendingCoordinatorOutputRenegotiation.reset();
                    for (const auto& ownedSession : displaySessions.sessions())
                    {
                        bafx::desktop::DisplaySession& session = *ownedSession;
                        const bool coordinator = &session == &displaySession;
                        bool applied = false;
                        if (coordinator)
                        {
                            applied = renegotiateCoordinatorOutput(
                                currentOutputPreference,
                                "configuration");
                        }
                        else if (session.secondaryBackgroundCaptureInitialized())
                        {
                            try
                            {
                                // The session service stops only this WGC
                                // producer before replacing output resources,
                                // then owns its restart or FX-only fallback.
                                session.requestSecondaryOutputRenegotiation(
                                    currentOutputPreference,
                                    "configuration");
                                applied = true;
                            }
                            catch (const std::exception& error)
                            {
                                session.shutdownSecondaryBackgroundCapture();
                                appendSecondaryBackgroundCaptureFailure(
                                    logPath,
                                    session,
                                    "queue-output-renegotiation",
                                    error.what());
                                applied = tryRenegotiateOutput(
                                    logPath,
                                    session,
                                    currentOutputPreference,
                                    "configuration-fx-only").has_value();
                            }
                            catch (...)
                            {
                                session.shutdownSecondaryBackgroundCapture();
                                appendSecondaryBackgroundCaptureFailure(
                                    logPath,
                                    session,
                                    "queue-output-renegotiation",
                                    "unknown exception");
                                applied = tryRenegotiateOutput(
                                    logPath,
                                    session,
                                    currentOutputPreference,
                                    "configuration-fx-only").has_value();
                            }
                        }
                        else
                        {
                            // Capture initialization can fail independently.
                            // The surviving FX-only surface must still honor
                            // an explicit HDR/SDR output preference change.
                            applied = tryRenegotiateOutput(
                                logPath,
                                session,
                                currentOutputPreference,
                                "configuration-fx-only").has_value();
                        }
                        if (applied && coordinator)
                        {
                            report.setDeviceInfo(renderer.deviceInfo());
                        }
                    }
                }
            }
            const bafx::windows::BackgroundCaptureRequest nextBackgroundRequest =
                bafx::desktop::backgroundCaptureRequest(
                    config,
                    backgroundRetryToken);
            const bafx::desktop::DisplayTargetIntent targetIntent{
                displayTargetChanged
                    ? *pendingDisplayTarget
                    : appliedDisplayTarget,
                displayTargetChanged};
            const std::optional<bafx::windows::WindowSize> outputIntent =
                displayTargetChanged
                    ? std::optional<bafx::windows::WindowSize>(
                        bafx::desktop::displayTargetSize(targetIntent.target))
                    : pendingOutputResize;
            const bafx::windows::BackgroundCaptureRequestResult requestResult =
                backgroundTransition.beginIntent(
                    nextBackgroundRequest,
                    outputIntent);
            switch (requestResult)
            {
            case bafx::windows::BackgroundCaptureRequestResult::Started:
            {
                appliedBackgroundRequest = nextBackgroundRequest;
                const bafx::desktop::BackgroundCaptureExecutionStatus status =
                    bafx::desktop::executeBackgroundCaptureTransition(
                        backgroundTransition,
                        window,
                        renderer,
                        targetIntent,
                        controlState.generation,
                        borderlessAccessAuthority,
                        backgroundExecution,
                        logPath);
                backgroundRetryPending = false;
                if (status
                    == bafx::desktop::BackgroundCaptureExecutionStatus::Completed)
                {
                    finishBackgroundCaptureTransaction(
                        "control-background-recovery");
                }
                break;
            }
            case bafx::windows::BackgroundCaptureRequestResult::NoChange:
                appliedBackgroundRequest = nextBackgroundRequest;
                backgroundRetryPending = false;
                break;
            case bafx::windows::BackgroundCaptureRequestResult::Busy:
                throw std::logic_error(
                    "Background capture request arrived during a transaction");
            case bafx::windows::BackgroundCaptureRequestResult::InvalidRequest:
                throw std::logic_error("Background capture request was invalid");
            }
            appliedGeneration = controlState.generation;
            const std::string_view configurationReason = configChanged
                ? (displayTargetChanged
                    ? "control-and-display-target"
                    : (pendingOutputResize.has_value()
                        ? "control-and-output-resize"
                        : "control-generation"))
                : (displayTargetChanged
                    ? "display-target"
                    : (pendingOutputResize.has_value()
                        ? "output-resize"
                        : "background-retry"));
            bafx::desktop::appendAppliedConfiguration(
                logPath,
                config,
                appliedOutputSize,
                configurationReason);
        }
        // A lifecycle transaction can invalidate a snapshot before frame
        // pacing waits. Consume it now so a timeout cannot shift attribution
        // to a later control generation.
        appendPendingBackgroundSnapshotInvalidation();

        const bafx::fx::SimulationTime captureHealthNow = clock.now();
        if (captureExclusionHealthPoller.shouldQuery(
                renderer.backgroundCaptureActive(),
                captureHealthNow))
        {
            const bafx::windows::CaptureExclusionQueryStatus affinity =
                window.queryCaptureExcluded(true);
            const bool affinityConfirmed = affinity.confirmed();
            performanceWindow.addCaptureExclusionHealthCheck(
                affinityConfirmed);
            if (!affinityConfirmed)
            {
                const bool transactionPending =
                    backgroundExecution.transactionActive;
                const std::uint64_t failureGeneration = transactionPending
                    ? backgroundExecution.controlGeneration
                    : appliedGeneration;
                bafx::desktop::appendCaptureExclusionHealthFailure(
                    logPath,
                    failureGeneration,
                    transactionPending,
                    affinity);

                bafx::desktop::BackgroundCaptureExecutionStatus fallbackStatus =
                    bafx::desktop::BackgroundCaptureExecutionStatus::Pending;
                if (transactionPending)
                {
                    fallbackStatus =
                        bafx::desktop::cancelBackgroundCaptureTransition(
                            backgroundTransition,
                            window,
                            renderer,
                            borderlessAccessAuthority,
                            backgroundExecution,
                            bafx::desktop::BackgroundCaptureCancelResizePolicy::
                                Preserve,
                            "capture-exclusion-lost",
                            logPath);
                }
                else
                {
                    if (!backgroundTransition.beginCaptureExclusionLost())
                    {
                        throw std::logic_error(
                            "Capture exclusion loss could not enter cleanup transaction");
                    }
                    fallbackStatus =
                        bafx::desktop::executeBackgroundCaptureTransition(
                            backgroundTransition,
                            window,
                            renderer,
                            bafx::desktop::DisplayTargetIntent{
                                appliedDisplayTarget,
                                false},
                            appliedGeneration,
                            borderlessAccessAuthority,
                            backgroundExecution,
                            logPath);
                }
                if (fallbackStatus
                    != bafx::desktop::BackgroundCaptureExecutionStatus::Completed)
                {
                    throw std::logic_error(
                        "Capture exclusion cleanup unexpectedly became pending");
                }
                if (backgroundExecution.sensorFailure.empty())
                {
                    backgroundExecution.sensorFailure =
                        bafx::windows::captureExclusionQueryDiagnostic(affinity);
                }
                finishBackgroundCaptureTransaction(
                    "capture-exclusion-health-recovery");
                renderInvalidated = true;
            }
        }
        renderInvalidated = serviceSecondaryBackgroundCaptures(
            displaySessions,
            displaySession,
            captureHealthNow,
            logPath)
            || renderInvalidated;

        const bool enteringPause = controlState.paused
            && !simulationTimeline.paused();
        const bool shouldRender = !controlState.paused
            || enteringPause
            || renderInvalidated;
        std::optional<HRESULT> framePacingDeviceLoss;
        bafx::desktop::DisplaySession* secondaryRecoveredDuringPacing = nullptr;
        bool renderCoordinatorThisIteration = false;
        if (shouldRender)
        {
            frameWaitables.clear();
            readyDisplaySessions.clear();
            const auto& ownedSessions = displaySessions.sessions();
            const bafx::core::MonotonicTime pacingObservedAt = clock.now();
            const std::optional<bafx::core::MonotonicTime>
                minimumFramePeriod =
                    fixedFramePacingPeriod(config.performance.framePacing);
            std::optional<bafx::core::MonotonicTime>
                earliestCadenceDeadline{};
            for (std::size_t index = 0U; index < ownedSessions.size(); ++index)
            {
                bafx::desktop::DisplaySession& session = *ownedSessions[index];
                if (&session != &displaySession && session.renderFaulted())
                {
                    continue;
                }
                const HANDLE deviceRemoved =
                    session.renderer().deviceRemovedWaitableObject();
                if (deviceRemoved != nullptr)
                {
                    frameWaitables.push_back(
                        bafx::desktop::FramePacingWaitable{
                            deviceRemoved,
                            bafx::desktop::FramePacingWaitableKind::
                                DeviceRemoved,
                            index});
                }
            }
            if (const HANDLE accessChanged =
                    borderlessAccessMonitor.changeEvent();
                accessChanged != nullptr)
            {
                // Device removal remains first. Permission changes precede
                // frame slots so revoked capture cannot submit another frame.
                frameWaitables.push_back(
                    bafx::desktop::FramePacingWaitable{
                        accessChanged,
                        bafx::desktop::FramePacingWaitableKind::ControlChanged,
                        0U});
            }
            for (std::size_t index = 0U; index < ownedSessions.size(); ++index)
            {
                bafx::desktop::DisplaySession& session = *ownedSessions[index];
                if (&session != &displaySession && session.renderFaulted())
                {
                    continue;
                }
                if (options.framePacingStallProbe
                    && &session != &displaySession)
                {
                    continue;
                }
                if (minimumFramePeriod.has_value()
                    && !session.framePacingDue(pacingObservedAt))
                {
                    const std::optional<bafx::core::MonotonicTime> deadline =
                        session.nextFramePacingDeadline();
                    if (deadline.has_value()
                        && (!earliestCadenceDeadline.has_value()
                            || *deadline < *earliestCadenceDeadline))
                    {
                        earliestCadenceDeadline = deadline;
                    }
                    continue;
                }
                const HANDLE frameLatency = &session == &displaySession
                        && options.framePacingStallProbe
                    ? framePacingStallHandle.get()
                    : session.renderer().frameLatencyWaitableObject();
                if (frameLatency == nullptr)
                {
                    if (&session == &displaySession)
                    {
                        throw std::runtime_error(
                            "Coordinator returned a null frame latency handle");
                    }
                    session.markRenderFaulted();
                    appendSecondaryRenderFailure(
                        logPath,
                        session,
                        "frame-latency-handle",
                        "renderer returned a null frame latency handle");
                    continue;
                }
                frameWaitables.push_back(
                    bafx::desktop::FramePacingWaitable{
                        frameLatency,
                        bafx::desktop::FramePacingWaitableKind::FrameReady,
                        index});
            }
            DWORD frameWaitTimeout = activeControlPollMilliseconds;
            if (earliestCadenceDeadline.has_value())
            {
                const bafx::core::MonotonicTime delay =
                    *earliestCadenceDeadline - pacingObservedAt;
                if (frameCadenceTimer.get() != nullptr)
                {
                    frameCadenceTimerError =
                        bafx::desktop::armFrameCadenceWaitableTimer(
                            frameCadenceTimer.get(),
                            delay);
                    if (frameCadenceTimerError == ERROR_SUCCESS)
                    {
                        frameWaitables.push_back(
                            bafx::desktop::FramePacingWaitable{
                                frameCadenceTimer.get(),
                                bafx::desktop::FramePacingWaitableKind::
                                    CadenceReady,
                                0U});
                    }
                    else
                    {
                        frameCadenceTimer.reset();
                    }
                }
                if (frameCadenceTimer.get() == nullptr)
                {
                    frameWaitTimeout = cadenceFallbackTimeoutMilliseconds(
                        delay);
                    if (!frameCadenceTimerFailureLogged)
                    {
                        const std::string error = std::to_string(
                            frameCadenceTimerError == ERROR_SUCCESS
                                ? ERROR_GEN_FAILURE
                                : frameCadenceTimerError);
                        const std::array fields{
                            bafx::windows::DiagnosticField{
                                "Error",
                                error},
                            bafx::windows::DiagnosticField{
                                "Fallback",
                                "bounded-message-timeout"}};
                        bafx::windows::appendDiagnosticEvent(
                            logPath,
                            "FramePacing.CadenceTimerUnavailable",
                            fields,
                            bafx::windows::DiagnosticLevel::Warning);
                        frameCadenceTimerFailureLogged = true;
                    }
                }
            }
            const bafx::desktop::FramePacingWaitResult pacingWait =
                bafx::desktop::waitForAnyFrameOpportunity(
                    frameWaitables,
                    frameWaitTimeout);
            performanceWindow.addFramePacingWake(pacingWait.wake);
            bafx::desktop::DisplaySession* awakenedSession =
                pacingWait.token < ownedSessions.size()
                ? ownedSessions[pacingWait.token].get()
                : nullptr;
            switch (pacingWait.wake)
            {
            case bafx::desktop::FramePacingWake::FrameReady:
                if (awakenedSession == nullptr)
                {
                    throw std::logic_error(
                        "Frame pacing returned an unknown display token");
                }
                readyDisplaySessions.push_back(awakenedSession);
                break;
            case bafx::desktop::FramePacingWake::DeviceRemoved:
            {
                if (awakenedSession == nullptr)
                {
                    throw std::logic_error(
                        "Device removal returned an unknown display token");
                }
                if (awakenedSession != &displaySession)
                {
                    // Recover before any WGC maintenance can observe the
                    // failed sensor as inactive and erase restart eligibility.
                    if (recoverSecondaryDisplaySession(
                            logPath,
                            *awakenedSession,
                            "frame-pacing-device-recovery",
                            "Display.Session.FramePacingDeviceRecovered",
                            true))
                    {
                        secondaryRecoveredDuringPacing = awakenedSession;
                        renderInvalidationPending = true;
                    }
                    break;
                }
                const bafx::fx::SimulationTime detectedAt = clock.now();
                const HRESULT deviceResult = renderer.deviceRemovedReason();
                appendFramePacingDeviceRecoveryDetection(
                    logPath,
                    pacingWait,
                    deviceResult,
                    detectedAt - lastFrameReadyAt);
                if (!bafx::windows::isDeviceLostResult(deviceResult))
                {
                    // A manual-reset notification remains signaled until the
                    // device is rebuilt. Reject a false signal instead of
                    // spinning on the same handle indefinitely.
                    throw std::runtime_error(
                        "D3D11 device removal notification produced unexpected HRESULT "
                        + formatHresult(deviceResult));
                }
                framePacingDeviceLoss = deviceResult;
                break;
            }
            case bafx::desktop::FramePacingWake::ControlChanged:
                // The next owner iteration consumes and resets the generation
                // before any WGC maintenance or Present can run.
                continue;
            case bafx::desktop::FramePacingWake::CadenceReady:
                // Rebuild the wait set so only displays whose deadlines have
                // elapsed can consume their independently signaled DXGI slot.
                continue;
            case bafx::desktop::FramePacingWake::MessagesPending:
            case bafx::desktop::FramePacingWake::TimedOut:
            {
                const bafx::fx::SimulationTime waitObservedAt = clock.now();
                if (runtimeDeadlineReached(waitObservedAt))
                {
                    quit = true;
                    continue;
                }
                const bafx::fx::SimulationTime stalledFor =
                    waitObservedAt - lastFrameReadyAt;
                if (renderer.deviceRemovedWaitableObject() == nullptr
                    && stalledFor >= framePacingDeviceProbePeriod
                    && waitObservedAt - lastFramePacingDeviceProbeAt
                        >= framePacingDeviceProbePeriod)
                {
                    lastFramePacingDeviceProbeAt = waitObservedAt;
                    const HRESULT deviceResult = renderer.deviceRemovedReason();
                    if (bafx::windows::isDeviceLostResult(deviceResult))
                    {
                        appendFramePacingDeviceRecoveryDetection(
                            logPath,
                            pacingWait,
                            deviceResult,
                            stalledFor);
                        framePacingDeviceLoss = deviceResult;
                        break;
                    }
                }
                // Preserve one-shot resize/config/background invalidations
                // until a real swap-chain slot is available. WGC callbacks do
                // not grant Present permission, but their producer queues must
                // still be collapsed while every swap chain is back-pressured.
                renderInvalidationPending =
                    maintainSecondaryBackgroundCaptures(
                        displaySessions,
                        displaySession,
                        readyDisplaySessions,
                        waitObservedAt,
                        logPath)
                    || renderInvalidated;
                continue;
            }
            case bafx::desktop::FramePacingWake::Failed:
            {
                const HRESULT deviceResult = renderer.deviceRemovedReason();
                if (!bafx::windows::isDeviceLostResult(deviceResult))
                {
                    throw bafx::windows::HResultError(
                        HRESULT_FROM_WIN32(pacingWait.error),
                        "MsgWaitForMultipleObjectsEx(frame latency)");
                }

                // A removed D3D device can invalidate the latency object before
                // Present reports the loss. Route this frame through the same
                // bounded recovery path that owns all resource-domain changes.
                appendFramePacingDeviceRecoveryDetection(
                    logPath,
                    pacingWait,
                    deviceResult,
                    clock.now() - lastFrameReadyAt);
                framePacingDeviceLoss = deviceResult;
                break;
            }
            }

            if (pacingWait.wake == bafx::desktop::FramePacingWake::FrameReady
                || pacingWait.wake
                    == bafx::desktop::FramePacingWake::DeviceRemoved)
            {
                const bafx::core::MonotonicTime frameReadyPollAt = clock.now();
                for (const auto& ownedSession : ownedSessions)
                {
                    bafx::desktop::DisplaySession& session = *ownedSession;
                    if (&session != &displaySession && session.renderFaulted())
                    {
                        continue;
                    }
                    if (&session == secondaryRecoveredDuringPacing)
                    {
                        // The wake belonged to the released device. Wait for a
                        // fresh opportunity from the replacement swap chain.
                        continue;
                    }
                    if (minimumFramePeriod.has_value()
                        && !session.framePacingDue(frameReadyPollAt))
                    {
                        continue;
                    }
                    if (std::find(
                            readyDisplaySessions.begin(),
                            readyDisplaySessions.end(),
                            &session) != readyDisplaySessions.end())
                    {
                        continue;
                    }
                    const HANDLE frameLatency = &session == &displaySession
                            && options.framePacingStallProbe
                        ? framePacingStallHandle.get()
                        : session.renderer().frameLatencyWaitableObject();
                    if (frameLatency == nullptr)
                    {
                        continue;
                    }
                    const DWORD state = WaitForSingleObject(frameLatency, 0U);
                    if (state == WAIT_OBJECT_0)
                    {
                        readyDisplaySessions.push_back(&session);
                    }
                    else if (state == WAIT_FAILED)
                    {
                        if (&session == &displaySession)
                        {
                            throw bafx::windows::HResultError(
                                HRESULT_FROM_WIN32(GetLastError()),
                                "WaitForSingleObject(coordinator frame latency)");
                        }
                        const DWORD error = GetLastError();
                        session.markRenderFaulted();
                        appendSecondaryRenderFailure(
                            logPath,
                            session,
                            "frame-latency-poll",
                            "wait failed; error=" + std::to_string(error));
                    }
                }
            }

            const bool coordinatorFrameReady = std::find(
                    readyDisplaySessions.begin(),
                    readyDisplaySessions.end(),
                    &displaySession) != readyDisplaySessions.end();
            renderCoordinatorThisIteration =
                framePacingDeviceLoss.has_value() || coordinatorFrameReady;
            if (coordinatorFrameReady)
            {
                lastFrameReadyAt = clock.now();
            }
            else if (renderInvalidated)
            {
                // A faster secondary can wake the Host before the coordinator
                // has a slot. Retain primary resize/config/WGC invalidations.
                renderInvalidationPending = true;
            }
        }

        const MessageDispatchDiagnostics messageDispatch = pendingMessageDispatch;
        pendingMessageDispatch = MessageDispatchDiagnostics{};

        const bafx::fx::SimulationTime wallTime = clock.now();
        if (!shouldRender)
        {
            // The vector is populated only inside the pacing branch. Do not
            // mistake a previous iteration's slot for current Present access.
            readyDisplaySessions.clear();
        }
        renderInvalidationPending = maintainSecondaryBackgroundCaptures(
            displaySessions,
            displaySession,
            readyDisplaySessions,
            wallTime,
            logPath)
            || renderInvalidationPending;
        if (options.demoClick
            && !demoStartedAt.has_value()
            && wallTime - applicationStartedAt
                >= std::chrono::milliseconds(options.demoDelayMilliseconds))
        {
            // A capture baseline can let WGC accumulate a fresh sample before
            // starting the visible batch that is intentionally path-latched.
            startDemoClick(wallTime);
        }
        simulationTimeline.setPaused(controlState.paused, wallTime);
        const bafx::fx::SimulationTime renderTime =
            options.demoAgeMilliseconds.has_value() && demoStartedAt.has_value()
            ? *demoStartedAt + std::chrono::milliseconds(*options.demoAgeMilliseconds)
            : simulationTimeline.fromWallTime(wallTime);
        std::vector<bafx::windows::PointerEvent> pointerEvents =
            hostWindow.takePointerEvents();
        const std::size_t pointerEventsBeforeHostCompaction =
            pointerEvents.size();
        pointerEvents = bafx::windows::coalescePointerMoves(
            std::move(pointerEvents));
        bafx::windows::PointerQueueDiagnostics pointerQueue =
            hostWindow.takePointerQueueDiagnostics();
        pointerQueue.compactedMoveEvents +=
            pointerEventsBeforeHostCompaction - pointerEvents.size();
        performanceWindow.addInput(bafx::desktop::InputPerformanceSample{
            pointerQueue.rawInputMessages,
            pointerQueue.moveEvents,
            pointerQueue.buttonEdges,
            pointerQueue.cancelEvents,
            pointerQueue.compactedMoveEvents,
            pointerQueue.overflowMoveDrops,
            pointerQueue.messageTimeUnavailable,
            messageDispatch.inputMessages,
            messageDispatch.otherMessages,
            pointerQueue.maximumPendingEvents,
            pointerQueue.maximumWin32QueueAgeMilliseconds,
            messageDispatch.inputBudgetExhausted,
            messageDispatch.otherBudgetExhausted});
        PointerConsumptionDiagnostics pointerConsumption{};
        if (options.demoClick || !config.effects.enabled || controlState.paused)
        {
            // Do not let disabled/paused input accumulate and replay after resume.
            pointerRouter.discardFrame(pointerEvents);
            if (enteringPause || !config.effects.enabled)
            {
                // Disabling can drain the physical Up event. Cancel
                // idempotently so re-enabling cannot retain a phantom press.
                pointerRouter.cancelAll(displaySessions, renderTime);
            }
        }
        else
        {
            const bafx::desktop::DisplayPointerRouteResult routed =
                pointerRouter.consumeFrame(
                    displaySessions,
                    renderTime,
                    pointerEvents,
                    pointerTimestamps);
            pointerConsumption.acceptedDowns.reserve(
                routed.acceptedDowns.size());
            for (const bafx::windows::PointerEvent& acceptedDown :
                 routed.acceptedDowns)
            {
                pointerConsumption.acceptedDowns.push_back(
                    PointerLatencyOrigin{
                        acceptedDown.qpcTimestamp,
                        acceptedDown.messageTimeMilliseconds,
                        acceptedDown.messageTimeValid});
            }
            if (routed.displayHandoffs > 0U)
            {
                const std::string handoffs = std::to_string(
                    routed.displayHandoffs);
                const std::array fields{
                    bafx::windows::DiagnosticField{"Count", handoffs}};
                bafx::windows::appendDiagnosticEvent(
                    logPath,
                    "Input.Pointer.DisplayHandoff",
                    fields);
            }
        }
        if (runtimeDeadlineReached(wallTime))
        {
            break;
        }
        if ((!controlState.paused || enteringPause) && config.effects.enabled)
        {
            for (const auto& ownedSession : displaySessions.sessions())
            {
                ownedSession->simulation().advance(renderTime);
            }
        }
        if (renderCoordinatorThisIteration)
        {
            bafx::fx::FrameSnapshot snapshot = config.effects.enabled
                ? simulation.snapshot(toViewport(window.size()), renderTime)
                : bafx::fx::FrameSnapshot{};
            applyVisualConfig(snapshot, config);
            lastPresentedDrawableContent = snapshot.hasDrawableContent();
            // A paused DComp surface can persist indefinitely. Its last frame
            // may only bake a current background; normal animation tolerates
            // short WGC cadence gaps without modulating FX energy.
            std::optional<
                bafx::windows::CompositionFrameDiagnostics> frameDiagnostics;
            // Device loss may synchronously publish Session.Closed while
            // renderFrame unwinds. Recovery eligibility needs the state that
            // existed before the failing GPU/Present call.
            const bool backgroundCaptureWasActive =
                renderer.backgroundCaptureActive();
            try
            {
                if (framePacingDeviceLoss.has_value())
                {
                    // Enter the same one-shot recovery boundary as Present;
                    // no input or simulation work is replayed after recovery.
                    throw bafx::windows::HResultError(
                        *framePacingDeviceLoss,
                        "D3D11 device loss detected during frame pacing");
                }
                frameDiagnostics = renderer.renderFrame(
                    snapshot,
                    wallTime,
                    controlState.paused);
            }
            catch (const bafx::windows::HResultError& error)
            {
                if (!bafx::windows::isDeviceLostResult(error.result())
                    || deviceRecoveryConsumed)
                {
                    if (bafx::windows::isDeviceLostResult(error.result())
                        && deviceRecoveryConsumed)
                    {
                        const std::string resultCode = formatHresult(
                            error.result());
                        const std::array suppressedFields{
                            bafx::windows::DiagnosticField{
                                "HRESULT",
                                resultCode},
                            bafx::windows::DiagnosticField{
                                "Reason",
                                "process recovery budget exhausted"}};
                        bafx::windows::appendDiagnosticEvent(
                            logPath,
                            "Graphics.DeviceRecovery.Suppressed",
                            suppressedFields,
                            bafx::windows::DiagnosticLevel::Error);
                    }
                    throw;
                }
                deviceRecoveryConsumed = true;
                const bafx::windows::GraphicsDeviceInfo previousDeviceInfo =
                    renderer.deviceInfo();
                const std::string originalError(error.what());
                const std::string resultCode = formatHresult(error.result());
                const std::array recoveryFields{
                    bafx::windows::DiagnosticField{
                        "HRESULT",
                        resultCode},
                    bafx::windows::DiagnosticField{
                        "Message",
                        originalError},
                    bafx::windows::DiagnosticField{
                        "Attempt",
                        "1"}};
                bafx::windows::appendDiagnosticEvent(
                    logPath,
                    "Graphics.DeviceRecovery.Begin",
                    recoveryFields,
                    bafx::windows::DiagnosticLevel::Warning);

                const bool deviceRecovered = renderer.tryRecoverDevice();
                bafx::desktop::appendBackgroundCaptureStopDiagnostics(
                    logPath,
                    renderer,
                    "render-device-recovery");
                if (!deviceRecovered)
                {
                    const std::string recoveryFailure(
                        renderer.deviceRecoveryFailure());
                    const bafx::windows::DeviceRecoveryDiagnostics diagnostics =
                        renderer.deviceRecoveryDiagnostics();
                    const std::string totalMicroseconds = std::to_string(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            diagnostics.total).count());
                    const std::array failureFields{
                        bafx::windows::DiagnosticField{
                            "OriginalHRESULT",
                            resultCode},
                        bafx::windows::DiagnosticField{
                            "RecoveryError",
                            recoveryFailure},
                        bafx::windows::DiagnosticField{
                            "TotalUs",
                            totalMicroseconds}};
                    bafx::windows::appendDiagnosticEvent(
                        logPath,
                        "Graphics.DeviceRecovery.Failed",
                        failureFields,
                        bafx::windows::DiagnosticLevel::Error);
                    throw;
                }

                appendDeviceRemovedNotificationStatus(
                    logPath,
                    renderer,
                    "render-device-recovery");
                report.setDeviceInfo(renderer.deviceInfo());
                const bool adapterChanged =
                    previousDeviceInfo.adapterLuid.LowPart
                        != renderer.deviceInfo().adapterLuid.LowPart
                    || previousDeviceInfo.adapterLuid.HighPart
                        != renderer.deviceInfo().adapterLuid.HighPart;
                if (backgroundExecution.transactionActive)
                {
                    const bafx::desktop::BackgroundCaptureExecutionStatus
                        canceled =
                            bafx::desktop::cancelBackgroundCaptureTransition(
                                backgroundTransition,
                                window,
                                renderer,
                                borderlessAccessAuthority,
                                backgroundExecution,
                                bafx::desktop::
                                    BackgroundCaptureCancelResizePolicy::Preserve,
                                "device-recovery",
                                logPath);
                    if (canceled
                        != bafx::desktop::BackgroundCaptureExecutionStatus::
                            Completed)
                    {
                        throw std::logic_error(
                            "Device recovery could not cancel pending WGC access");
                    }
                    finishBackgroundCaptureTransaction(
                        "device-recovery-pending-cancel");
                }
                if (backgroundTransition.effectivePath()
                        == bafx::windows::EffectiveBackgroundCapturePath::
                            BackgroundAware
                    && backgroundTransition.request().has_value()
                    && backgroundTransition.request()->sensorRequired)
                {
                    if (!backgroundTransition.beginSessionStopped())
                    {
                        const std::array failureFields{
                            bafx::windows::DiagnosticField{
                                "OriginalHRESULT",
                                resultCode},
                            bafx::windows::DiagnosticField{
                                "RecoveryError",
                                "WGC stop transaction was not accepted"}};
                        bafx::windows::appendDiagnosticEvent(
                            logPath,
                            "Graphics.DeviceRecovery.Failed",
                            failureFields,
                            bafx::windows::DiagnosticLevel::Error);
                        throw std::logic_error(
                            "WGC stop transaction was not accepted after device recovery");
                    }
                    try
                    {
                        const bafx::desktop::BackgroundCaptureExecutionStatus
                            stopStatus =
                                bafx::desktop::executeBackgroundCaptureTransition(
                                    backgroundTransition,
                                    window,
                                    renderer,
                                    bafx::desktop::DisplayTargetIntent{
                                        appliedDisplayTarget,
                                        false},
                                    appliedGeneration,
                                    borderlessAccessAuthority,
                                    backgroundExecution,
                                    logPath);
                        if (stopStatus
                            != bafx::desktop::BackgroundCaptureExecutionStatus::
                                Completed)
                        {
                            throw std::logic_error(
                                "WGC stop transaction unexpectedly became pending");
                        }
                        backgroundCaptureEnabled = false;
                        control.setBackgroundCaptureActive(false);
                        report.setBackgroundCaptureStatus(
                            bafx::desktop::backgroundCaptureStatus(
                                backgroundTransition.effectivePath()));
                        bafx::desktop::appendBackgroundCaptureOutcome(
                            logPath,
                            appliedBackgroundRequest,
                            backgroundTransition,
                            backgroundExecution,
                            renderer);
                    }
                    catch (const std::exception& stopError)
                    {
                        const std::array failureFields{
                            bafx::windows::DiagnosticField{
                                "OriginalHRESULT",
                                resultCode},
                            bafx::windows::DiagnosticField{
                                "RecoveryError",
                                stopError.what()}};
                        bafx::windows::appendDiagnosticEvent(
                            logPath,
                            "Graphics.DeviceRecovery.Failed",
                            failureFields,
                            bafx::windows::DiagnosticLevel::Error);
                        throw;
                    }
                    catch (...)
                    {
                        const std::array failureFields{
                            bafx::windows::DiagnosticField{
                                "OriginalHRESULT",
                                resultCode},
                            bafx::windows::DiagnosticField{
                                "RecoveryError",
                                "WGC stop transaction failed with unknown exception"}};
                        bafx::windows::appendDiagnosticEvent(
                            logPath,
                            "Graphics.DeviceRecovery.Failed",
                            failureFields,
                            bafx::windows::DiagnosticLevel::Error);
                        throw;
                    }
                }
                else
                {
                    backgroundCaptureEnabled = false;
                    control.setBackgroundCaptureActive(false);
                }
                report.setBackgroundCaptureStatus(
                    bafx::desktop::backgroundCaptureStatus(
                        backgroundTransition.effectivePath()));

                const bool backgroundRetryEligible =
                    bafx::desktop::canRetryBackgroundCaptureAfterDeviceRecovery(
                        appliedBackgroundRequest.sensorRequired,
                        backgroundCaptureWasActive,
                        adapterChanged,
                        renderer.deviceInfo().driverType,
                        renderer.backgroundCaptureRestartAllowed());
                if (backgroundRetryEligible
                    && backgroundRetryToken
                        == std::numeric_limits<std::uint64_t>::max())
                {
                    const std::array failureFields{
                        bafx::windows::DiagnosticField{
                            "OriginalHRESULT",
                            resultCode},
                        bafx::windows::DiagnosticField{
                            "RecoveryError",
                            "retry token exhausted"}};
                    bafx::windows::appendDiagnosticEvent(
                        logPath,
                        "Graphics.DeviceRecovery.Failed",
                        failureFields,
                        bafx::windows::DiagnosticLevel::Error);
                    throw;
                }
                if (backgroundRetryEligible)
                {
                    ++backgroundRetryToken;
                }
                backgroundRetryPending = backgroundRetryEligible;
                backgroundParticipationLogged = false;
                backgroundPendingDiagnosticLogged = false;
                std::string adapterState = adapterChanged
                    ? "changed"
                    : "same";
                const std::string retryTokenText = std::to_string(
                    backgroundRetryToken);
                const bafx::windows::DeviceRecoveryDiagnostics diagnostics =
                    renderer.deviceRecoveryDiagnostics();
                const std::string totalMicroseconds = std::to_string(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        diagnostics.total).count());
                const std::string backgroundStopMicroseconds = std::to_string(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        diagnostics.backgroundStop).count());
                const std::array successFields{
                    bafx::windows::DiagnosticField{
                        "RetryToken",
                        retryTokenText},
                    bafx::windows::DiagnosticField{
                        "Adapter",
                        adapterState},
                    bafx::windows::DiagnosticField{
                        "WgcWasActive",
                        backgroundCaptureWasActive ? "true" : "false"},
                    bafx::windows::DiagnosticField{
                        "WgcRetryPending",
                        backgroundRetryPending ? "true" : "false"},
                    bafx::windows::DiagnosticField{
                        "WgcStopUs",
                        backgroundStopMicroseconds},
                    bafx::windows::DiagnosticField{
                        "TotalUs",
                        totalMicroseconds}};
                bafx::windows::appendDiagnosticEvent(
                    logPath,
                    "Graphics.DeviceRecovery.Succeeded",
                    successFields);
                try
                {
                    // Retry the exact CPU snapshot and wall-clock sample once;
                    // no simulation/input event is consumed twice.
                    frameDiagnostics = renderer.renderFrame(
                        snapshot,
                        wallTime,
                        controlState.paused);
                }
                catch (const std::exception& retryError)
                {
                    const std::array failureFields{
                        bafx::windows::DiagnosticField{
                            "OriginalHRESULT",
                            resultCode},
                        bafx::windows::DiagnosticField{
                            "RecoveryError",
                            retryError.what()}};
                    bafx::windows::appendDiagnosticEvent(
                        logPath,
                        "Graphics.DeviceRecovery.Failed",
                        failureFields,
                        bafx::windows::DiagnosticLevel::Error);
                    throw;
                }
                catch (...)
                {
                    const std::array failureFields{
                        bafx::windows::DiagnosticField{
                            "OriginalHRESULT",
                            resultCode},
                        bafx::windows::DiagnosticField{
                            "RecoveryError",
                            "retry render failed with unknown exception"}};
                    bafx::windows::appendDiagnosticEvent(
                        logPath,
                        "Graphics.DeviceRecovery.Failed",
                        failureFields,
                        bafx::windows::DiagnosticLevel::Error);
                    throw;
                }
            }
            if (recoveryProbePending)
            {
                recoveryProbePending = false;
                if (deviceRecoveryConsumed)
                {
                    bafx::windows::appendDiagnosticEvent(
                        logPath,
                        "Graphics.DeviceRecovery.Probe.Suppressed",
                        {},
                        bafx::windows::DiagnosticLevel::Warning);
                }
                else
                {
                    deviceRecoveryConsumed = true;
                    bafx::windows::appendDiagnosticEvent(
                        logPath,
                        "Graphics.DeviceRecovery.Probe.Begin");
                    const bool probeRecovered = renderer.tryRecoverDevice();
                    bafx::desktop::appendBackgroundCaptureStopDiagnostics(
                        logPath,
                        renderer,
                        "device-recovery-probe");
                    if (!probeRecovered)
                    {
                        throw std::runtime_error(
                            "Device recovery probe could not rebuild the renderer: "
                            + std::string(renderer.deviceRecoveryFailure()));
                    }
                    appendDeviceRemovedNotificationStatus(
                        logPath,
                        renderer,
                        "device-recovery-probe");
                    report.setDeviceInfo(renderer.deviceInfo());
                    const bafx::windows::DeviceRecoveryDiagnostics diagnostics =
                        renderer.deviceRecoveryDiagnostics();
                    const std::string totalMicroseconds = std::to_string(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            diagnostics.total).count());
                    const std::string backgroundStopMicroseconds = std::to_string(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            diagnostics.backgroundStop).count());
                    const std::array probeFields{
                        bafx::windows::DiagnosticField{
                            "WgcStopUs",
                            backgroundStopMicroseconds},
                        bafx::windows::DiagnosticField{
                            "TotalUs",
                            totalMicroseconds}};
                    frameDiagnostics = renderer.renderFrame(
                        snapshot,
                        wallTime,
                        controlState.paused);
                    const std::optional<bafx::windows::PixelF> probePixel =
                        renderer.lastCenterPixel();
                    if (!probePixel.has_value()
                        || !std::isfinite(probePixel->red)
                        || !std::isfinite(probePixel->green)
                        || !std::isfinite(probePixel->blue)
                        || !std::isfinite(probePixel->alpha)
                        || probePixel->alpha <= 0.01F)
                    {
                        throw std::runtime_error(
                            "Device recovery probe produced an invalid center pixel");
                    }
                    bafx::windows::appendDiagnosticEvent(
                        logPath,
                        "Graphics.DeviceRecovery.Probe.Succeeded",
                        probeFields);
                }
            }
            const bafx::windows::CompositionFrameDiagnostics&
                completedFrameDiagnostics = *frameDiagnostics;
            displaySession.recordPresentedFrame(
                lastPresentedDrawableContent,
                wallTime,
                fixedFramePacingPeriod(
                    config.performance.framePacing).value_or(
                        bafx::core::MonotonicTime::zero()));
            appendPendingBackgroundSnapshotInvalidation();
            const std::uint64_t producerCallbacks =
                wgcCallbackDeltaTracker.observe(
                    completedFrameDiagnostics.wgcActive,
                    completedFrameDiagnostics.wgc.epoch,
                    completedFrameDiagnostics.wgc.frameArrivedCallbacksTotal);
            performanceWindow.addFrame(framePerformanceSample(
                completedFrameDiagnostics,
                producerCallbacks,
                options.smokeTest));
            for (const PointerLatencyOrigin& origin :
                 pointerConsumption.acceptedDowns)
            {
                if (origin.dispatchQpc > 0
                    && completedFrameDiagnostics.presentReturnedQpc
                        >= origin.dispatchQpc)
                {
                    performanceWindow.addDispatchToPresentReturn(
                        durationMicroseconds(
                            clock.fromCounter(
                                completedFrameDiagnostics.presentReturnedQpc)
                            - clock.fromCounter(origin.dispatchQpc)));
                }
                if (origin.messageTimeValid)
                {
                    performanceWindow.addMessageToPresentReturn(
                        bafx::windows::win32MessageQueueAgeMilliseconds(
                            completedFrameDiagnostics.presentReturnedTickMilliseconds,
                            origin.messageTimeMilliseconds));
                }
            }
            if (completedFrameDiagnostics.backgroundParticipated)
            {
                ++backgroundCompositeFrames;
                if (!backgroundParticipationLogged)
                {
                    // A successful Present is the first point where this
                    // captured snapshot is proven to have reached final output.
                    bafx::desktop::appendBackgroundCompositeParticipation(
                        logPath,
                        appliedGeneration,
                        completedFrameDiagnostics);
                    backgroundParticipationLogged = true;
                }
            }
        }
        else
        {
            // Product pause or another display's earlier frame slot can skip
            // this output. Drain only the sensor-owned sample so the next
            // coordinator batch cannot inherit an accumulated WGC entry.
            const bafx::windows::BackgroundSensorMaintenanceDiagnostics
                maintenance = renderer.serviceBackgroundCapture(wallTime);
            const std::uint64_t producerCallbacks =
                wgcCallbackDeltaTracker.observe(
                    maintenance.wgcActive,
                    maintenance.wgc.epoch,
                    maintenance.wgc.frameArrivedCallbacksTotal);
            performanceWindow.addBackgroundMaintenance(wgcPerformanceSample(
                maintenance.wgc,
                maintenance.wgcDrainInclusiveCpu,
                producerCallbacks,
                maintenance.wgcActive,
                maintenance.wgcDrainAttempted,
                maintenance.wgcIdleDrainAttempted,
                maintenance.wgcIdleDrainSkipped));
        }
        appendPendingBackgroundSnapshotInvalidation();
        bool currentBackgroundCaptureActive = renderer.backgroundCaptureActive();
        if (backgroundExecution.transactionActive
            && backgroundCaptureEnabled
            && !currentBackgroundCaptureActive)
        {
            const bafx::desktop::BackgroundCaptureExecutionStatus canceled =
                bafx::desktop::cancelBackgroundCaptureTransition(
                    backgroundTransition,
                    window,
                    renderer,
                    borderlessAccessAuthority,
                    backgroundExecution,
                    bafx::desktop::BackgroundCaptureCancelResizePolicy::Preserve,
                    "capture-session-stopped",
                    logPath);
            if (canceled
                != bafx::desktop::BackgroundCaptureExecutionStatus::Completed)
            {
                throw std::logic_error(
                    "Stopped WGC session could not cancel pending access");
            }
            finishBackgroundCaptureTransaction(
                "background-pending-stop-recovery");
            currentBackgroundCaptureActive = renderer.backgroundCaptureActive();
        }
        if (!backgroundExecution.transactionActive
            && backgroundCaptureEnabled
            && !currentBackgroundCaptureActive)
        {
            const std::string stoppedReason(renderer.backgroundCaptureFailure());
            if (!backgroundTransition.beginSessionStopped())
            {
                throw std::logic_error(
                    "Background capture stop could not enter cleanup transaction");
            }
            const bafx::desktop::BackgroundCaptureExecutionStatus stopStatus =
                bafx::desktop::executeBackgroundCaptureTransition(
                    backgroundTransition,
                    window,
                    renderer,
                    bafx::desktop::DisplayTargetIntent{
                        appliedDisplayTarget,
                        false},
                    appliedGeneration,
                    borderlessAccessAuthority,
                    backgroundExecution,
                    logPath);
            if (stopStatus
                != bafx::desktop::BackgroundCaptureExecutionStatus::Completed)
            {
                throw std::logic_error(
                    "WGC stop transaction unexpectedly became pending");
            }
            if (backgroundExecution.sensorFailure.empty())
            {
                backgroundExecution.sensorFailure = stoppedReason;
            }
            finishBackgroundCaptureTransaction("background-stop-recovery");
            currentBackgroundCaptureActive = renderer.backgroundCaptureActive();
        }
        else if (const std::optional<bafx::windows::WindowSize> captureSize =
                     renderer.pendingBackgroundFramePoolSize();
                 !backgroundExecution.transactionActive
                     && backgroundCaptureEnabled
                     && captureSize.has_value())
        {
            const bool backgroundCaptureWasActive =
                currentBackgroundCaptureActive;
            if (!backgroundTransition.beginFramePoolRecreate(*captureSize))
            {
                throw std::logic_error(
                    "WGC frame pool resize could not enter its transaction");
            }
            const bafx::desktop::BackgroundCaptureExecutionStatus recreateStatus =
                bafx::desktop::executeBackgroundCaptureTransition(
                    backgroundTransition,
                    window,
                    renderer,
                    bafx::desktop::DisplayTargetIntent{
                        appliedDisplayTarget,
                        false},
                    appliedGeneration,
                    borderlessAccessAuthority,
                    backgroundExecution,
                    logPath);
            if (recreateStatus
                != bafx::desktop::BackgroundCaptureExecutionStatus::Completed)
            {
                throw std::logic_error(
                    "WGC frame pool recreate unexpectedly became pending");
            }
            if (backgroundExecution.deviceRecovered)
            {
                const bool retryEligible =
                    bafx::desktop::canRetryBackgroundCaptureAfterDeviceRecovery(
                        appliedBackgroundRequest.sensorRequired,
                        backgroundCaptureWasActive,
                        backgroundExecution.deviceRecoveryAdapterChanged,
                        renderer.deviceInfo().driverType,
                        renderer.backgroundCaptureRestartAllowed());
                backgroundRetryPending = false;
                if (retryEligible)
                {
                    if (backgroundRetryToken
                        == std::numeric_limits<std::uint64_t>::max())
                    {
                        throw std::runtime_error(
                            "WGC retry token exhausted after frame pool recovery");
                    }
                    ++backgroundRetryToken;
                    backgroundRetryPending = true;
                    const std::string retryTokenText = std::to_string(
                        backgroundRetryToken);
                    const std::array retryFields{
                        bafx::windows::DiagnosticField{
                            "RetryToken",
                            retryTokenText}};
                    bafx::windows::appendDiagnosticEvent(
                        logPath,
                        "Graphics.DeviceRecovery.FramePoolRetryScheduled",
                        retryFields);
                }
            }
            finishBackgroundCaptureTransaction("frame-pool-recovery");
            currentBackgroundCaptureActive = renderer.backgroundCaptureActive();
        }
        if (!backgroundCaptureEnabled && currentBackgroundCaptureActive)
        {
            throw std::logic_error(
                "Background sensor became active outside its transaction");
        }
        control.setBackgroundCaptureActive(currentBackgroundCaptureActive);
        appendPendingBackgroundSnapshotInvalidation();
        if (lastPresentedDrawableContent
            && currentBackgroundCaptureActive
            && !backgroundParticipationLogged
            && !backgroundPendingDiagnosticLogged
            && wallTime - applicationStartedAt >= std::chrono::seconds(1))
        {
            std::string diagnostic =
                "WGC final composite is still pending; reason=";
            diagnostic += backgroundCompositeStatusName(
                renderer.backgroundCompositeStatus());
            bafx::windows::appendDiagnosticLog(logPath, diagnostic);
            backgroundPendingDiagnosticLogged = true;
        }
        if (shouldRender)
        {
            static_cast<void>(renderSecondarySessions(
                displaySessions,
                displaySession,
                readyDisplaySessions,
                config,
                renderTime,
                wallTime,
                !controlState.paused || enteringPause,
                logPath));
        }
        if (renderCoordinatorThisIteration && options.smokeTest)
        {
            const std::optional<bafx::windows::PixelF> pixel =
                renderer.lastCenterPixel();
            if (!pixel.has_value()
                || !std::isfinite(pixel->red)
                || !std::isfinite(pixel->green)
                || !std::isfinite(pixel->blue)
                || !std::isfinite(pixel->alpha)
                || pixel->alpha <= 0.01F
                || std::max({pixel->red, pixel->green, pixel->blue}) <= 0.01F)
            {
                throw std::runtime_error(
                    "Desktop smoke test did not render a finite center FX pixel");
            }
        }
        if (renderCoordinatorThisIteration)
        {
            if (!controlState.paused || enteringPause)
            {
                // Pool cleanup belongs after the boundary presentation.
                // Simulation time freezes during product pause, while the
                // one-second lifetime remains independent of monitor cadence.
                simulation.onFrameRendered(renderTime);
            }
            ++renderedFrames;
            if (options.frameLimit.has_value() && renderedFrames >= *options.frameLimit)
            {
                break;
            }
        }

        const bafx::fx::SimulationTime performanceNow = clock.now();
        if (performanceNow - performanceWindowStartedAt
            >= performanceReportInterval)
        {
            previousPerformanceLogWriteCpu =
                bafx::desktop::appendPerformanceInterval(
                    logPath,
                    performanceWindow.summarize(),
                    config,
                    bafx::desktop::PerformanceLogContext{
                        appliedOutputSize,
                        renderer.backgroundCompositeStatus(),
                        controlState.paused},
                    performanceNow - performanceWindowStartedAt,
                    previousPerformanceLogWriteCpu,
                    false);
            performanceWindow.reset();
            performanceWindowStartedAt = clock.now();
        }

        if (controlState.paused && !renderInvalidationPending)
        {
            // Pause freezes authored simulation state, not the desktop beneath
            // it. A visible retained effect must follow WGC frame events so its
            // source-over payload never keeps an obsolete light background.
            pausedWaitables.clear();
            const auto& ownedSessions = displaySessions.sessions();
            for (std::size_t index = 0U; index < ownedSessions.size(); ++index)
            {
                bafx::desktop::DisplaySession& session = *ownedSessions[index];
                if (&session != &displaySession && session.renderFaulted())
                {
                    continue;
                }
                const HANDLE deviceRemoved =
                    session.renderer().deviceRemovedWaitableObject();
                if (deviceRemoved != nullptr)
                {
                    // Device loss invalidates a complete resource domain, so
                    // keep every device event ahead of all WGC frame events.
                    pausedWaitables.push_back(
                        bafx::desktop::PausedWaitable{
                            deviceRemoved,
                            bafx::desktop::PausedWaitableKind::DeviceRemoved,
                            index});
                }
            }
            if (const HANDLE accessChanged =
                    borderlessAccessMonitor.changeEvent();
                accessChanged != nullptr)
            {
                pausedWaitables.push_back(
                    bafx::desktop::PausedWaitable{
                        accessChanged,
                        bafx::desktop::PausedWaitableKind::ControlChanged,
                        0U});
            }
            for (std::size_t index = 0U; index < ownedSessions.size(); ++index)
            {
                bafx::desktop::DisplaySession& session = *ownedSessions[index];
                if (&session != &displaySession && session.renderFaulted())
                {
                    continue;
                }
                const HANDLE backgroundWaitable = &session == &displaySession
                    ? (lastPresentedDrawableContent
                        ? renderer.backgroundFrameAvailableObject()
                        : nullptr)
                    : (session.lastPresentedDrawableContent()
                        ? session.secondaryBackgroundFrameAvailableObject()
                        : nullptr);
                if (backgroundWaitable != nullptr)
                {
                    pausedWaitables.push_back(
                        bafx::desktop::PausedWaitable{
                            backgroundWaitable,
                            bafx::desktop::PausedWaitableKind::
                                BackgroundFrameReady,
                            index});
                }
            }
            const bafx::desktop::PausedWaitResult pausedWait =
                bafx::desktop::waitForAnyPausedInvalidation(
                    pausedWaitables,
                    pausedControlPollMilliseconds);
            switch (pausedWait.wake)
            {
            case bafx::desktop::PausedWaitWake::DeviceRemoved:
                if (pausedWait.token >= ownedSessions.size())
                {
                    throw std::logic_error(
                        "Paused wait returned an unknown display token");
                }
                if (ownedSessions[pausedWait.token].get() == &displaySession)
                {
                    renderInvalidationPending = true;
                }
                else
                {
                    // Paused mode has no intervening render pass. Recover here
                    // so the next maintenance pass only sees the new device.
                    renderInvalidationPending =
                        recoverSecondaryDisplaySession(
                            logPath,
                            *ownedSessions[pausedWait.token],
                            "paused-device-recovery",
                            "Display.Session.PausedDeviceRecovered",
                            true)
                        || renderInvalidationPending;
                }
                break;
            case bafx::desktop::PausedWaitWake::BackgroundFrameReady:
                if (pausedWait.token >= ownedSessions.size())
                {
                    throw std::logic_error(
                        "Paused WGC wait returned an unknown display token");
                }
                renderInvalidationPending = true;
                break;
            case bafx::desktop::PausedWaitWake::ControlChanged:
                // There is no render below this wait. The next iteration
                // consumes the permission generation before doing GPU work.
                break;
            case bafx::desktop::PausedWaitWake::MessagesPending:
            case bafx::desktop::PausedWaitWake::TimedOut:
                break;
            case bafx::desktop::PausedWaitWake::Failed:
                throw bafx::windows::HResultError(
                    HRESULT_FROM_WIN32(pausedWait.error),
                    "MsgWaitForMultipleObjectsEx(paused Host)");
            }
        }
    }
    if (backgroundExecution.transactionActive)
    {
        const bafx::desktop::BackgroundCaptureExecutionStatus canceled =
            bafx::desktop::cancelBackgroundCaptureTransition(
                backgroundTransition,
                window,
                renderer,
                borderlessAccessAuthority,
                backgroundExecution,
                bafx::desktop::BackgroundCaptureCancelResizePolicy::Discard,
                "shutdown",
                logPath);
        if (canceled
            != bafx::desktop::BackgroundCaptureExecutionStatus::Completed)
        {
            throw std::logic_error(
                "Shutdown could not cancel pending background capture access");
        }
        finishBackgroundCaptureTransaction("shutdown-pending-cancel");
    }
    const bafx::fx::SimulationTime finalPerformanceTime = clock.now();
    if (!performanceWindow.empty())
    {
        static_cast<void>(bafx::desktop::appendPerformanceInterval(
            logPath,
            performanceWindow.summarize(),
            config,
            bafx::desktop::PerformanceLogContext{
                appliedOutputSize,
                renderer.backgroundCompositeStatus(),
                control.snapshot().paused},
            finalPerformanceTime - performanceWindowStartedAt,
            previousPerformanceLogWriteCpu,
            true));
    }
    if (backgroundCompositeFrames > 0U)
    {
        std::string summary = "WGC final composite summary; composite-frames=";
        summary += std::to_string(backgroundCompositeFrames);
        summary += "; rendered-frames=";
        summary += std::to_string(renderedFrames);
        bafx::windows::appendDiagnosticLog(logPath, summary);
    }
    // Destructors are not visible in the support log.  Stop the producer
    // explicitly so the final ledger proves that the process released every
    // WGC resource before handing control back to Win32.
    backgroundShutdown.finalize("shutdown");
    appendPendingBackgroundSnapshotInvalidation();
    control.stop();
    return 0;
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    RunOptions options = parseOptions();
    std::filesystem::path logPath{};
    bafx::windows::SupportReport report(bafx::desktop::version);
    std::optional<bafx::desktop::SingleInstanceGuard> instanceGuard;
    try
    {
        if (options.supportInfoPath.has_value())
        {
            // Keep diagnostics portable even when a caller supplies an
            // absolute path; only its file name is accepted beside the EXE.
            const std::wstring requestedName = options.supportInfoPath->wstring();
            options.supportInfoPath.reset();
            options.supportInfoPath = bafx::windows::executableFilePath(
                requestedName,
                L"ba-click-fx-support.txt");
        }
        logPath = bafx::windows::defaultDiagnosticLogPath();
        report.setLogPath(logPath);
        const std::string_view processMode = options.supportInfoOnly
            ? "support-info"
            : (options.smokeTest ? "smoke-test" : "interactive");
        const std::array startupFields{
            bafx::windows::DiagnosticField{
                "Product.Version",
                bafx::desktop::version},
            bafx::windows::DiagnosticField{
                "Process.Mode",
                processMode}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "Process.Startup",
            startupFields);
        if (!options.supportInfoOnly)
        {
            instanceGuard.emplace(bafx::windows::kHostSingleInstanceMutexName);
            if (!instanceGuard->acquire())
            {
                if (instanceGuard->alreadyRunning())
                {
                    bafx::windows::appendDiagnosticLog(
                        logPath,
                        "Another BAFX Host instance is already running");
                    return 0;
                }
                throw std::runtime_error(
                    "Could not acquire the BAFX Host single-instance mutex (error "
                    + std::to_string(instanceGuard->lastError()) + ")");
            }
        }
        const int result = runApplication(instance, options, report, logPath);
        bafx::windows::appendDiagnosticEvent(logPath, "Process.Exited");
        return result;
    }
    catch (const std::exception& error)
    {
        report.setFailure(error.what());
        if (!logPath.empty())
        {
            bafx::windows::appendDiagnosticLog(logPath, report);
        }
        if (options.supportInfoPath.has_value())
        {
            try
            {
                bafx::windows::writeSupportReport(*options.supportInfoPath, report);
            }
            catch (...)
            {
                // Preserve the original startup failure when the requested path is unavailable.
            }
        }
        if (options.smokeTest)
        {
            OutputDebugStringA(error.what());
            OutputDebugStringA("\n");
        }
        else
        {
            MessageBoxA(
                nullptr,
                error.what(),
                "ba-click-fx-desktop failed",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        }
        return 1;
    }
}
