#include "bafx/desktop/version.hpp"
#include "bafx/config/config.hpp"
#include "bafx/fx/simulation_runtime.hpp"
#include "bafx/fx/simulation_timeline.hpp"
#include "bafx/windows/composition_renderer.hpp"
#include "bafx/windows/display_capabilities.hpp"
#include "bafx/windows/error.hpp"
#include "bafx/windows/overlay_window.hpp"
#include "bafx/windows/package_identity.hpp"
#include "bafx/windows/portable_paths.hpp"
#include "bafx/windows/runtime_diagnostics.hpp"
#include "bafx/windows/unique_handle.hpp"
#include "background_capture_runtime.hpp"
#include "frame_pacing.hpp"
#include "host_control.hpp"
#include "performance_logging.hpp"
#include "performance_window.hpp"
#include "pointer_frame_dispatch.hpp"

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
    case bafx::desktop::FramePacingWake::MessagesPending:
        return "messages-pending";
    case bafx::desktop::FramePacingWake::TimedOut:
        return "timed-out";
    case bafx::desktop::FramePacingWake::Failed:
        return "failed";
    }
    return "unknown";
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

struct MonitorSelection
{
    HMONITOR handle{nullptr};
    RECT bounds{};
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

[[nodiscard]] MonitorSelection primaryMonitorBounds()
{
    POINT origin{0, 0};
    const HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO information{};
    information.cbSize = sizeof(information);
    if (!GetMonitorInfoW(monitor, &information))
    {
        bafx::windows::throwLastError("GetMonitorInfoW");
    }
    return MonitorSelection{monitor, information.rcMonitor};
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

[[nodiscard]] bool isInsideClient(
    const POINT position,
    const bafx::windows::WindowSize size) noexcept
{
    return position.x >= 0
        && position.y >= 0
        && static_cast<std::uint32_t>(position.x) < size.width
        && static_cast<std::uint32_t>(position.y) < size.height;
}

[[nodiscard]] POINT clampToClient(
    const POINT position,
    const bafx::windows::WindowSize size) noexcept
{
    const LONG maximumX = static_cast<LONG>(size.width - 1U);
    const LONG maximumY = static_cast<LONG>(size.height - 1U);
    return POINT{
        std::clamp(position.x, 0L, maximumX),
        std::clamp(position.y, 0L, maximumY)};
}

[[nodiscard]] PointerConsumptionDiagnostics consumePointerEvents(
    bafx::windows::OverlayWindow& window,
    bafx::fx::SimulationRuntime& simulation,
    bafx::windows::PointerFrameAdapter& frameAdapter,
    const QpcClock& clock,
    const bafx::fx::SimulationTime frameTime,
    const std::vector<bafx::windows::PointerEvent>& events)
{
    using bafx::desktop::PointerFrameDispatch;
    using bafx::desktop::PointerFramePosition;
    using bafx::desktop::PointerFramePositionUse;
    using bafx::desktop::PointerFrameTransition;
    using bafx::desktop::PointerFrameTransitionKind;
    using bafx::windows::PointerEventKind;

    const bafx::fx::Viewport viewport = toViewport(window.size());
    const bafx::windows::PointerFrameSnapshot frame =
        frameAdapter.consume(events);
    PointerFrameDispatch dispatch{};
    PointerConsumptionDiagnostics diagnostics{};
    diagnostics.acceptedDowns.reserve(frame.edges.size());

    for (const bafx::windows::PointerFrameEdge& edge : frame.edges)
    {
        PointerFrameTransition transition{};
        transition.inputTime = clock.fromCounter(edge.trigger.qpcTimestamp);
        switch (edge.kind)
        {
        case PointerEventKind::LeftButtonDown:
            transition.kind = PointerFrameTransitionKind::Down;
            {
                POINT triggerClientPosition = edge.trigger.screenPosition;
                transition.acceptDown = ScreenToClient(
                    window.handle(),
                    &triggerClientPosition)
                    && isInsideClient(triggerClientPosition, window.size());
            }
            break;

        case PointerEventKind::LeftButtonUp:
            transition.kind = PointerFrameTransitionKind::Up;
            break;

        case PointerEventKind::Cancel:
            transition.kind = PointerFrameTransitionKind::Cancel;
            break;

        case PointerEventKind::Move:
            // The frame adapter never emits Move as a state transition.
            continue;
        }
        bafx::desktop::mergePointerFrameTransition(
            dispatch.buttons,
            transition);
        if (edge.kind == PointerEventKind::LeftButtonDown
            && transition.acceptDown)
        {
            diagnostics.acceptedDowns.push_back(PointerLatencyOrigin{
                edge.trigger.qpcTimestamp,
                edge.trigger.messageTimeMilliseconds,
                edge.trigger.messageTimeValid});
        }
    }

    dispatch.buttons.held = frame.heldAfter;
    const bool downNeedsPosition = dispatch.buttons.down
        && dispatch.buttons.acceptDown;
    if (dispatch.buttons.held
        && (frame.hasFinalHeldMove || downNeedsPosition))
    {
        dispatch.positionUse = PointerFramePositionUse::Held;
    }
    else if (!frame.heldAfter
        && frame.edges.empty()
        && frame.hasFinalFreeMove)
    {
        // A button/cancel frame belongs wholly to the strict press path.
        // Ambient enhancement may restart only from a later input frame, so
        // an Up followed by queued Move cannot create a second same-frame stroke.
        dispatch.positionUse = PointerFramePositionUse::Free;
    }

    if (downNeedsPosition
        || dispatch.positionUse != PointerFramePositionUse::None)
    {
        POINT screenPosition{};
        bool positionAvailable = GetCursorPos(&screenPosition) != FALSE;
        if (!positionAvailable && frame.latestNonCancelSample.has_value())
        {
            // Cancel may contain a default POINT when Win32 sampling fails.
            // Only a previously validated non-Cancel event is a safe fallback.
            screenPosition = frame.latestNonCancelSample->screenPosition;
            positionAvailable = true;
        }

        POINT clientPosition = screenPosition;
        if (positionAvailable
            && ScreenToClient(window.handle(), &clientPosition))
        {
            const bool insideClient = isInsideClient(
                clientPosition,
                window.size());
            clientPosition = clampToClient(clientPosition, window.size());
            const bafx::windows::PointerEvent* inputSample = nullptr;
            if (frame.latestMoveSample.has_value())
            {
                inputSample = &*frame.latestMoveSample;
            }
            else if (frame.latestNonCancelSample.has_value())
            {
                inputSample = &*frame.latestNonCancelSample;
            }
            const bafx::fx::SimulationTime inputTime = inputSample != nullptr
                ? clock.fromCounter(inputSample->qpcTimestamp)
                : frameTime;
            dispatch.position = PointerFramePosition{
                bafx::fx::PointF{
                    static_cast<float>(clientPosition.x),
                    static_cast<float>(clientPosition.y)},
                insideClient,
                inputTime};
        }
    }

    // PointerFrameSnapshot retains raw edge order for diagnostics. The effect
    // path consumes aggregated flags and one frame-boundary cursor position.
    bafx::desktop::applyPointerFrame(
        simulation,
        viewport,
        frameTime,
        dispatch);
    return diagnostics;
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
    else if (loadedConfig.status == bafx::config::ConfigStatus::CreatedDefault
        || loadedConfig.status == bafx::config::ConfigStatus::Migrated)
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
    const MonitorSelection primaryMonitor = primaryMonitorBounds();
    report.setPrimaryMonitor(primaryMonitor.bounds);
    bafx::windows::OverlayWindow window(
        instance,
        primaryMonitor.bounds,
        L"ba-click-fx-desktop",
        options.disableRawInput
            ? bafx::windows::RawMouseRegistration::Disabled
            : bafx::windows::RawMouseRegistration::Enabled);
    report.setPrimaryDpi(window.effectiveDpi());
    if (const auto refreshRate =
            bafx::windows::queryPrimaryCompositionRefreshRate();
        refreshRate.has_value())
    {
        report.setPrimaryRefreshRate(*refreshRate);
    }
    if (const auto displayColor =
            bafx::windows::queryDisplayColorCapabilities(primaryMonitor.handle);
        displayColor.has_value())
    {
        report.setPrimaryDisplayColorCapabilities(*displayColor);
    }
    bafx::windows::CompositionRenderer renderer(
        window.handle(),
        window.size(),
        makeBloomSettings(config.effects));
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
    report.setExitUiStatus(window.exitUiStatus());
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
                primaryMonitor.handle,
                control.snapshot().generation,
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
    bafx::fx::SimulationRuntime simulation(makeRuntimeSeed());
    bafx::fx::SimulationTimeline simulationTimeline;
    bafx::windows::PointerFrameAdapter pointerFrameAdapter;
    simulation.setTrailLengthMultiplier(config.effects.trailLength);
    simulation.setInputSamplingRateHz(config.input.samplingRateHz);
    simulation.setAlwaysOnTrailEnabled(
        config.effects.enabled
            && config.effects.trailEnabled
            && !config.input.trailOnlyWhilePressed,
        bafx::fx::SimulationTime{});
    window.show();

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
    MessageDispatchDiagnostics pendingMessageDispatch{};
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
        if (backgroundExecution.resizedOutputSize.has_value())
        {
            appliedOutputSize = *backgroundExecution.resizedOutputSize;
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
    while (!quit && !window.closeRequested())
    {
        accumulateMessageDispatch(pendingMessageDispatch, dispatchMessages(quit));
        window.pollExitShortcut();
        window.pollPointerState();
        bafx::desktop::HostStateSnapshot controlState = control.snapshot();
        if (controlState.shutdownRequested || quit || window.closeRequested())
        {
            break;
        }
        if (runtimeDeadlineReached(clock.now()))
        {
            break;
        }

        bool renderInvalidated = renderInvalidationPending;
        renderInvalidationPending = false;
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
            bafx::desktop::BackgroundCaptureExecutionStatus executionStatus =
                bafx::desktop::BackgroundCaptureExecutionStatus::Pending;
            if (configChanged
                || pendingOutputResize.has_value()
                || backgroundRetryPending)
            {
                const std::string_view cancellationReason = configChanged
                    ? "control-generation"
                    : (pendingOutputResize.has_value()
                        ? "output-resize"
                        : "background-retry");
                executionStatus =
                    bafx::desktop::cancelBackgroundCaptureTransition(
                        backgroundTransition,
                        window,
                        renderer,
                        primaryMonitor.handle,
                        backgroundExecution,
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
                        primaryMonitor.handle,
                        backgroundExecution.controlGeneration,
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
        if (configChanged
            || pendingOutputResize.has_value()
            || backgroundRetryPending)
        {
            renderInvalidated = true;
            if (configChanged)
            {
                config = controlState.config;
                // Host owns the render thread, so applying the immutable control
                // snapshot here makes input, length and Bloom changes take effect
                // on the next frame without cross-thread renderer mutation.
                simulation.setTrailLengthMultiplier(config.effects.trailLength);
                simulation.setInputSamplingRateHz(config.input.samplingRateHz);
                simulation.setAlwaysOnTrailEnabled(
                    config.effects.enabled
                        && config.effects.trailEnabled
                        && !config.input.trailOnlyWhilePressed,
                    simulationTimeline.fromWallTime(clock.now()));
                const bafx::windows::GraphicsDeviceInfo previousDeviceInfo =
                    renderer.deviceInfo();
                const bool bloomDeviceRecovered = renderer.setBloomSettings(
                    makeBloomSettings(config.effects));
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
                        config.background.mode
                            == bafx::config::RenderMode::BackgroundAware
                        && !adapterChanged
                        && renderer.deviceInfo().driverType
                            == bafx::windows::GraphicsDriverType::Hardware;
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
                            "WgcRetry",
                            retryEligible ? "scheduled" : "blocked"}};
                    bafx::windows::appendDiagnosticEvent(
                        logPath,
                        "Graphics.DeviceRecovery.BloomSettingsSucceeded",
                        recoveryFields);
                }
            }
            const bafx::windows::BackgroundCaptureRequest nextBackgroundRequest =
                bafx::desktop::backgroundCaptureRequest(
                    config,
                    backgroundRetryToken);
            const bafx::windows::BackgroundCaptureRequestResult requestResult =
                backgroundTransition.beginIntent(
                    nextBackgroundRequest,
                    pendingOutputResize);
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
                        primaryMonitor.handle,
                        controlState.generation,
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
                ? (pendingOutputResize.has_value()
                    ? "control-and-output-resize"
                    : "control-generation")
                : (pendingOutputResize.has_value()
                    ? "output-resize"
                    : "background-retry");
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

        const bool enteringPause = controlState.paused
            && !simulationTimeline.paused();
        const bool shouldRender = !controlState.paused
            || enteringPause
            || renderInvalidated;
        std::optional<HRESULT> framePacingDeviceLoss;
        if (shouldRender)
        {
            const bafx::desktop::FramePacingWaitResult pacingWait =
                bafx::desktop::waitForFrameOpportunity(
                    options.framePacingStallProbe
                        ? framePacingStallHandle.get()
                        : renderer.frameLatencyWaitableObject(),
                    renderer.deviceRemovedWaitableObject(),
                    activeControlPollMilliseconds);
            performanceWindow.addFramePacingWake(pacingWait.wake);
            switch (pacingWait.wake)
            {
            case bafx::desktop::FramePacingWake::FrameReady:
                lastFrameReadyAt = clock.now();
                break;
            case bafx::desktop::FramePacingWake::DeviceRemoved:
            {
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
                // until a real swap-chain slot is available.
                renderInvalidationPending = renderInvalidated;
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
        }

        const MessageDispatchDiagnostics messageDispatch = pendingMessageDispatch;
        pendingMessageDispatch = MessageDispatchDiagnostics{};

        const bafx::fx::SimulationTime wallTime = clock.now();
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
            window.takePointerEvents();
        const std::size_t pointerEventsBeforeHostCompaction =
            pointerEvents.size();
        pointerEvents = bafx::windows::coalescePointerMoves(
            std::move(pointerEvents));
        bafx::windows::PointerQueueDiagnostics pointerQueue =
            window.takePointerQueueDiagnostics();
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
            static_cast<void>(pointerFrameAdapter.consume(
                pointerEvents));
            if (enteringPause || !config.effects.enabled)
            {
                // Disabling can drain the physical Up event. Cancel
                // idempotently so re-enabling cannot retain a phantom press.
                simulation.pointerCancel(renderTime);
            }
        }
        else
        {
            pointerConsumption = consumePointerEvents(
                window,
                simulation,
                pointerFrameAdapter,
                clock,
                renderTime,
                pointerEvents);
        }
        if (runtimeDeadlineReached(wallTime))
        {
            break;
        }
        if ((!controlState.paused || enteringPause) && config.effects.enabled)
        {
            simulation.advance(renderTime);
        }
        if (shouldRender)
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
                                primaryMonitor.handle,
                                backgroundExecution,
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
                                    primaryMonitor.handle,
                                    appliedGeneration,
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
                    appliedBackgroundRequest.sensorRequired
                    && !adapterChanged
                    && renderer.deviceInfo().driverType
                        == bafx::windows::GraphicsDriverType::Hardware;
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
                    appliedBackgroundRequest.retryToken = backgroundRetryToken;
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
            // Product pause freezes simulation and presentation, not the WGC
            // producer. Drain only the sensor-owned sample so the next visible
            // batch cannot begin from a FramePool entry accumulated while idle.
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
                    primaryMonitor.handle,
                    backgroundExecution,
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
                    primaryMonitor.handle,
                    appliedGeneration,
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
                    primaryMonitor.handle,
                    appliedGeneration,
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
                    appliedBackgroundRequest.sensorRequired
                    && !backgroundExecution.deviceRecoveryAdapterChanged
                    && renderer.deviceInfo().driverType
                        == bafx::windows::GraphicsDriverType::Hardware;
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
                    appliedBackgroundRequest.retryToken = backgroundRetryToken;
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
        if (shouldRender && options.smokeTest)
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
        if (shouldRender)
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

        if (controlState.paused)
        {
            // Pause freezes authored simulation state, not the desktop beneath
            // it. A visible retained effect must follow WGC frame events so its
            // source-over payload never keeps an obsolete light background.
            const HANDLE backgroundWaitable = lastPresentedDrawableContent
                ? renderer.backgroundFrameAvailableObject()
                : nullptr;
            const bafx::desktop::PausedWaitResult pausedWait =
                bafx::desktop::waitForPausedInvalidation(
                    renderer.deviceRemovedWaitableObject(),
                    backgroundWaitable,
                    pausedControlPollMilliseconds);
            switch (pausedWait.wake)
            {
            case bafx::desktop::PausedWaitWake::DeviceRemoved:
            case bafx::desktop::PausedWaitWake::BackgroundFrameReady:
                renderInvalidationPending = true;
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
                primaryMonitor.handle,
                backgroundExecution,
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
