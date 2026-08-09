#include "bafx/desktop/version.hpp"
#include "bafx/fx/simulation_runtime.hpp"
#include "bafx/windows/composition_renderer.hpp"
#include "bafx/windows/error.hpp"
#include "bafx/windows/overlay_window.hpp"
#include "bafx/windows/runtime_diagnostics.hpp"

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

[[nodiscard]] RECT primaryMonitorBounds()
{
    POINT origin{0, 0};
    const HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO information{};
    information.cbSize = sizeof(information);
    if (!GetMonitorInfoW(monitor, &information))
    {
        bafx::windows::throwLastError("GetMonitorInfoW");
    }
    return information.rcMonitor;
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
    const RECT monitorBounds = primaryMonitorBounds();
    report.setPrimaryMonitor(monitorBounds);
    bafx::windows::OverlayWindow window(
        instance,
        monitorBounds,
        L"ba-click-fx-desktop");
    bafx::windows::CompositionRenderer renderer(window.handle(), window.size());
    report.setDeviceInfo(renderer.deviceInfo());
    report.setExitUiStatus(window.exitUiStatus());
    bafx::windows::appendDiagnosticLog(logPath, report);
    if (options.supportInfoOnly)
    {
        bafx::windows::writeSupportReport(*options.supportInfoPath, report);
        return 0;
    }
    if (options.smokeTest && renderer.deviceInfo().adapterDescription.empty())
    {
        throw std::runtime_error("Desktop smoke test could not identify the D3D11 adapter");
    }
    renderer.setReadbackDiagnostics(options.smokeTest);
    bafx::fx::SimulationRuntime simulation(makeRuntimeSeed());
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
    const bafx::fx::SimulationTime applicationStartedAt = clock.now();
    while (!quit && !window.closeRequested())
    {
        dispatchMessages(quit);
        window.pollExitShortcut();
        window.pollPointerState();
        if (quit || window.closeRequested())
        {
            break;
        }

        if (const auto resize = window.takePendingResize(); resize.has_value())
        {
            renderer.resize(*resize);
        }

        consumePointerEvents(window, simulation, clock);
        const bafx::fx::SimulationTime wallTime = clock.now();
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
            : wallTime;
        simulation.advance(renderTime);
        const bafx::fx::FrameSnapshot snapshot = simulation.snapshot(
            toViewport(window.size()),
            renderTime);
        renderer.renderFrame(snapshot);
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
    try
    {
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
