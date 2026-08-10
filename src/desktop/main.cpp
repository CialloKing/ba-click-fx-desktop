#include "bafx/desktop/version.hpp"
#include "bafx/config/config.hpp"
#include "bafx/fx/simulation_runtime.hpp"
#include "bafx/windows/composition_renderer.hpp"
#include "bafx/windows/error.hpp"
#include "bafx/windows/overlay_window.hpp"
#include "bafx/windows/runtime_diagnostics.hpp"
#include "host_control.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace
{

constexpr std::uint32_t maximumMessagesPerFrame = 256U;
constexpr auto smokeTestDeadline = std::chrono::seconds(5);

[[nodiscard]] bool wantsBackgroundCapture(
    const bafx::config::Config& config) noexcept
{
    return config.background.mode == bafx::config::CaptureMode::BackgroundAware;
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

    const float scale = config.effects.globalScale;
    for (bafx::fx::Sprite& sprite : snapshot.sprites)
    {
        sprite.sizePixels *= scale;
    }
    const float trailScale = scale * config.effects.trailWidth;
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
    bool demoClick{false};
};

struct MonitorSelection
{
    HMONITOR handle{nullptr};
    RECT bounds{};
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

void dispatchMessages(bool& quit)
{
    MSG message{};
    std::uint32_t dispatched = 0U;
    while (dispatched < maximumMessagesPerFrame
        && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            quit = true;
            return;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
        ++dispatched;
    }
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

void consumePointerEvents(
    bafx::windows::OverlayWindow& window,
    bafx::fx::SimulationRuntime& simulation,
    const QpcClock& clock)
{
    const bafx::fx::Viewport viewport = toViewport(window.size());
    for (const bafx::windows::PointerEvent& event :
         bafx::windows::coalescePointerMoves(window.takePointerEvents()))
    {
        const bafx::fx::SimulationTime time = clock.fromCounter(event.qpcTimestamp);
        if (event.kind == bafx::windows::PointerEventKind::LeftButtonUp)
        {
            simulation.pointerUp(time);
            continue;
        }
        if (event.kind == bafx::windows::PointerEventKind::Cancel)
        {
            simulation.pointerCancel(time);
            continue;
        }

        POINT clientPosition = event.screenPosition;
        if (!ScreenToClient(window.handle(), &clientPosition))
        {
            continue;
        }

        const bool insideClient = isInsideClient(clientPosition, window.size());
        if (event.kind == bafx::windows::PointerEventKind::LeftButtonDown
            && (!insideClient || simulation.pointerHeld()))
        {
            continue;
        }
        if (event.kind == bafx::windows::PointerEventKind::Move)
        {
            if (!simulation.pointerHeld())
            {
                continue;
            }
            clientPosition = clampToClient(clientPosition, window.size());
        }

        const bafx::fx::PointF position{
            static_cast<float>(clientPosition.x),
            static_cast<float>(clientPosition.y)};
        switch (event.kind)
        {
        case bafx::windows::PointerEventKind::LeftButtonDown:
            simulation.pointerDown(position, viewport, time);
            break;

        case bafx::windows::PointerEventKind::Move:
            simulation.pointerMove(position, viewport, time);
            break;

        case bafx::windows::PointerEventKind::LeftButtonUp:
        case bafx::windows::PointerEventKind::Cancel:
            break;
        }
    }
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

    bafx::desktop::HostControlPlane control(configPath, config);
    report.setConfigurationSchemaVersion(config.schemaVersion);
    const MonitorSelection primaryMonitor = primaryMonitorBounds();
    report.setPrimaryMonitor(primaryMonitor.bounds);
    bafx::windows::OverlayWindow window(
        instance,
        primaryMonitor.bounds,
        L"ba-click-fx-desktop");
    report.setPrimaryDpi(window.effectiveDpi());
    bafx::windows::CompositionRenderer renderer(
        window.handle(),
        window.size(),
        makeBloomSettings(config.effects));
    const bafx::windows::CaptureExclusionStatus captureExclusion =
        window.setCaptureExcluded(wantsBackgroundCapture(config));
    bafx::windows::appendDiagnosticLog(
        logPath,
        bafx::windows::captureExclusionDiagnostic(captureExclusion));
    const bool exclusionConfirmed = captureExclusion.confirmed();
    if (wantsBackgroundCapture(config) && !exclusionConfirmed)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "Capture exclusion was not confirmed; WGC remains disabled");
    }
    report.setDeviceInfo(renderer.deviceInfo());
    report.setExitUiStatus(window.exitUiStatus());
    const bool controlServiceStarted = control.start();
    report.setControlServiceAvailable(controlServiceStarted);
    if (!controlServiceStarted)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            std::string("IPC control service unavailable; continuing without Control Center; error=")
                + std::to_string(control.ipcLastError()));
    }
    else
    {
        bafx::windows::appendDiagnosticLog(logPath, "IPC control service started");
    }
    if (options.supportInfoOnly)
    {
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
    // WGC startup belongs here, after the base renderer exists and only when
    // capture exclusion was confirmed by querying the effective affinity.
    bool backgroundCaptureWanted = wantsBackgroundCapture(config);
    bool backgroundCursorExcluded = config.background.cursorExcluded;
    bool backgroundCaptureEnabled = backgroundCaptureWanted
        && renderer.tryEnableBackgroundCapture(
            primaryMonitor.handle,
            exclusionConfirmed,
            backgroundCursorExcluded);
    if (!backgroundCaptureEnabled)
    {
        report.setBackgroundCaptureStatus(
            bafx::windows::BackgroundCaptureStatus::FallbackFxOnly);
        std::string failure = backgroundCaptureWanted
            ? "WGC background capture unavailable; using FX-only rendering"
            : "WGC background capture disabled by configuration; using FX-only rendering";
        if (backgroundCaptureWanted
            && !renderer.backgroundCaptureFailure().empty())
        {
            failure += "; reason=";
            failure += renderer.backgroundCaptureFailure();
        }
        if (backgroundCaptureWanted)
        {
            // A failed WGC startup has no feedback to protect. Restore normal
            // capture visibility so FX-only fallback remains visible to recorders.
            renderer.disableBackgroundCapture();
            const bafx::windows::CaptureExclusionStatus fallbackExclusion =
                window.setCaptureExcluded(false);
            bafx::windows::appendDiagnosticLog(
                logPath,
                bafx::windows::captureExclusionDiagnostic(fallbackExclusion));
        }
        bafx::windows::appendDiagnosticLog(logPath, failure);
    }
    else
    {
        report.setBackgroundCaptureStatus(
            bafx::windows::BackgroundCaptureStatus::Active);
    }
    bafx::windows::appendDiagnosticLog(logPath, report);
    bafx::fx::SimulationRuntime simulation(makeRuntimeSeed());
    simulation.setTrailLengthMultiplier(config.effects.trailLength);
    window.show();

    std::optional<bafx::fx::SimulationTime> demoStartedAt;
    if (options.demoClick)
    {
        const bafx::fx::Viewport viewport = toViewport(window.size());
        demoStartedAt = clock.now();
        simulation.pointerDown(
            bafx::fx::PointF{
                static_cast<float>(viewport.width) * 0.5F,
                static_cast<float>(viewport.height) * 0.5F},
            viewport,
            *demoStartedAt);
    }

    bool quit = false;
    std::uint32_t renderedFrames = 0;
    std::uint64_t appliedGeneration = control.snapshot().generation;
    std::optional<bafx::fx::SimulationTime> frozenRenderTime;
    const bafx::fx::SimulationTime applicationStartedAt = clock.now();
    while (!quit && !window.closeRequested())
    {
        dispatchMessages(quit);
        window.pollExitShortcut();
        window.pollPointerState();
        bafx::desktop::HostStateSnapshot controlState = control.snapshot();
        if (controlState.shutdownRequested || quit || window.closeRequested())
        {
            break;
        }

        if (controlState.generation != appliedGeneration)
        {
            config = controlState.config;
            // Host owns the render thread, so applying the immutable control
            // snapshot here makes length and Bloom changes take effect on the
            // next rendered frame without cross-thread renderer mutation.
            simulation.setTrailLengthMultiplier(config.effects.trailLength);
            renderer.setBloomSettings(makeBloomSettings(config.effects));
            const bool nextBackgroundCaptureWanted = wantsBackgroundCapture(config);
            const bool nextBackgroundCursorExcluded = config.background.cursorExcluded;
            if (nextBackgroundCaptureWanted != backgroundCaptureWanted
                || nextBackgroundCursorExcluded != backgroundCursorExcluded)
            {
                backgroundCaptureWanted = nextBackgroundCaptureWanted;
                backgroundCursorExcluded = nextBackgroundCursorExcluded;
                if (backgroundCaptureWanted)
                {
                    const bafx::windows::CaptureExclusionStatus exclusion =
                        window.setCaptureExcluded(true);
                    const bool confirmed = exclusion.confirmed();
                    backgroundCaptureEnabled = confirmed
                        && renderer.tryEnableBackgroundCapture(
                            primaryMonitor.handle,
                            confirmed,
                            backgroundCursorExcluded);
                    bafx::windows::appendDiagnosticLog(
                        logPath,
                        bafx::windows::captureExclusionDiagnostic(exclusion));
                    if (!backgroundCaptureEnabled
                        && !renderer.backgroundCaptureFailure().empty())
                    {
                        bafx::windows::appendDiagnosticLog(
                            logPath,
                            std::string("WGC background capture unavailable; using FX-only rendering; reason=")
                                + std::string(renderer.backgroundCaptureFailure()));
                    }
                    if (!backgroundCaptureEnabled)
                    {
                        // WGC did not start, so retaining self-exclusion would
                        // make the FX-only fallback unexpectedly invisible.
                        renderer.disableBackgroundCapture();
                        const bafx::windows::CaptureExclusionStatus fallbackExclusion =
                            window.setCaptureExcluded(false);
                        bafx::windows::appendDiagnosticLog(
                            logPath,
                            bafx::windows::captureExclusionDiagnostic(fallbackExclusion));
                    }
                }
                else
                {
                    renderer.disableBackgroundCapture();
                    backgroundCaptureEnabled = false;
                    const bafx::windows::CaptureExclusionStatus exclusion =
                        window.setCaptureExcluded(false);
                    bafx::windows::appendDiagnosticLog(
                        logPath,
                        bafx::windows::captureExclusionDiagnostic(exclusion));
                }
                report.setBackgroundCaptureStatus(
                    backgroundCaptureEnabled
                    ? bafx::windows::BackgroundCaptureStatus::Active
                    : bafx::windows::BackgroundCaptureStatus::FallbackFxOnly);
                bafx::windows::appendDiagnosticLog(logPath, report);
            }
            appliedGeneration = controlState.generation;
        }

        if (const auto resize = window.takePendingResize(); resize.has_value())
        {
            renderer.resize(*resize);
        }

        if (options.demoClick || !config.effects.enabled || controlState.paused)
        {
            // Do not let disabled/paused input accumulate and replay after resume.
            static_cast<void>(window.takePointerEvents());
        }
        else
        {
            consumePointerEvents(window, simulation, clock);
        }
        const bafx::fx::SimulationTime wallTime = clock.now();
        if (controlState.paused)
        {
            if (!frozenRenderTime.has_value())
            {
                frozenRenderTime = wallTime;
            }
        }
        else
        {
            frozenRenderTime.reset();
        }
        if (options.quitAfterMilliseconds.has_value()
            && wallTime - applicationStartedAt
                >= std::chrono::milliseconds(*options.quitAfterMilliseconds))
        {
            break;
        }
        if (options.smokeTest && wallTime - applicationStartedAt >= smokeTestDeadline)
        {
            // A bounded smoke test must fail instead of hanging under a noisy input source.
            throw std::runtime_error("Desktop smoke test exceeded its five-second deadline");
        }
        const bafx::fx::SimulationTime renderTime =
            options.demoAgeMilliseconds.has_value() && demoStartedAt.has_value()
            ? *demoStartedAt + std::chrono::milliseconds(*options.demoAgeMilliseconds)
            : frozenRenderTime.has_value() ? *frozenRenderTime : wallTime;
        if (!controlState.paused && config.effects.enabled)
        {
            simulation.advance(renderTime);
        }
        bafx::fx::FrameSnapshot snapshot = config.effects.enabled
            ? simulation.snapshot(toViewport(window.size()), renderTime)
            : bafx::fx::FrameSnapshot{};
        applyVisualConfig(snapshot, config);
        renderer.renderFrame(snapshot, wallTime);
        const bool currentBackgroundCaptureActive = renderer.backgroundCaptureActive();
        control.setBackgroundCaptureActive(currentBackgroundCaptureActive);
        if (currentBackgroundCaptureActive != backgroundCaptureEnabled)
        {
            backgroundCaptureEnabled = currentBackgroundCaptureActive;
            if (!backgroundCaptureEnabled && backgroundCaptureWanted)
            {
                // Session close, item close, or a failed resize leaves no
                // valid background sample; return to ordinary FX-only capture.
                renderer.disableBackgroundCapture();
                const bafx::windows::CaptureExclusionStatus fallbackExclusion =
                    window.setCaptureExcluded(false);
                bafx::windows::appendDiagnosticLog(
                    logPath,
                    bafx::windows::captureExclusionDiagnostic(fallbackExclusion));
            }
            report.setBackgroundCaptureStatus(
                backgroundCaptureEnabled
                ? bafx::windows::BackgroundCaptureStatus::Active
                : bafx::windows::BackgroundCaptureStatus::FallbackFxOnly);
            bafx::windows::appendDiagnosticLog(
                logPath,
                backgroundCaptureEnabled
                ? "WGC background capture resumed"
                : "WGC background capture stopped; using FX-only rendering");
            bafx::windows::appendDiagnosticLog(logPath, report);
        }
        if (options.smokeTest)
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
        simulation.onFrameRendered();
        ++renderedFrames;
        if (options.frameLimit.has_value() && renderedFrames >= *options.frameLimit)
        {
            break;
        }

        const HANDLE waitable = renderer.frameLatencyWaitableObject();
        const DWORD waitResult = MsgWaitForMultipleObjectsEx(
            1,
            &waitable,
            1000,
            QS_ALLINPUT,
            MWMO_INPUTAVAILABLE);
        if (waitResult == WAIT_FAILED)
        {
            bafx::windows::throwLastError("MsgWaitForMultipleObjectsEx");
        }
    }
    control.stop();
    return 0;
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    const RunOptions options = parseOptions();
    const std::filesystem::path logPath = bafx::windows::defaultDiagnosticLogPath();
    bafx::windows::SupportReport report(bafx::desktop::version);
    report.setLogPath(logPath);
    bafx::windows::appendDiagnosticLog(logPath, "Startup");
    std::optional<bafx::desktop::SingleInstanceGuard> instanceGuard;
    try
    {
        if (!options.supportInfoOnly)
        {
            instanceGuard.emplace(L"Local\\BAFX.Host.v1");
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
        bafx::windows::appendDiagnosticLog(logPath, "Exited");
        return result;
    }
    catch (const std::exception& error)
    {
        report.setFailure(error.what());
        bafx::windows::appendDiagnosticLog(logPath, report);
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
