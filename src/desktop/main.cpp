#include "bafx/fx/simulation.hpp"
#include "bafx/windows/composition_renderer.hpp"
#include "bafx/windows/error.hpp"
#include "bafx/windows/overlay_window.hpp"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace
{

constexpr std::uint32_t maximumMessagesPerFrame = 256U;
constexpr auto smokeTestDeadline = std::chrono::seconds(5);

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
            options.frameLimit = 3U;
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

void consumePointerEvents(
    bafx::windows::OverlayWindow& window,
    bafx::fx::Simulation& simulation,
    const QpcClock& clock)
{
    const bafx::fx::Viewport viewport = toViewport(window.size());
    for (const bafx::windows::PointerEvent& event : window.takePointerEvents())
    {
        POINT clientPosition = event.screenPosition;
        if (!ScreenToClient(window.handle(), &clientPosition))
        {
            continue;
        }

        const bafx::fx::PointF position{
            static_cast<float>(clientPosition.x),
            static_cast<float>(clientPosition.y)};
        const bafx::fx::SimulationTime time = clock.fromCounter(event.qpcTimestamp);
        switch (event.kind)
        {
        case bafx::windows::PointerEventKind::LeftButtonDown:
            simulation.pointerDown(position, viewport, time);
            break;

        case bafx::windows::PointerEventKind::Move:
            simulation.pointerMove(position, viewport, time);
            break;

        case bafx::windows::PointerEventKind::LeftButtonUp:
            simulation.pointerUp(time);
            break;
        }
    }
}

int runApplication(const HINSTANCE instance, const RunOptions options)
{
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
        && GetLastError() != ERROR_ACCESS_DENIED)
    {
        bafx::windows::throwLastError("SetProcessDpiAwarenessContext");
    }

    ComApartment apartment;
    QpcClock clock;
    bafx::windows::OverlayWindow window(
        instance,
        primaryMonitorBounds(),
        L"ba-click-fx-desktop");
    bafx::windows::CompositionRenderer renderer(window.handle(), window.size());
    bafx::fx::Simulation simulation;
    window.show();

    if (options.demoClick)
    {
        const bafx::fx::Viewport viewport = toViewport(window.size());
        simulation.pointerDown(
            bafx::fx::PointF{
                static_cast<float>(viewport.width) * 0.5F,
                static_cast<float>(viewport.height) * 0.5F},
            viewport,
            clock.now());
    }

    bool quit = false;
    std::uint32_t renderedFrames = 0;
    const bafx::fx::SimulationTime smokeStartedAt = clock.now();
    while (!quit && !window.closeRequested())
    {
        dispatchMessages(quit);
        if (quit)
        {
            break;
        }

        if (const auto resize = window.takePendingResize(); resize.has_value())
        {
            renderer.resize(*resize);
        }

        consumePointerEvents(window, simulation, clock);
        const bafx::fx::SimulationTime renderTime = clock.now();
        if (options.smokeTest && renderTime - smokeStartedAt >= smokeTestDeadline)
        {
            // A bounded smoke test must fail instead of hanging under a noisy input source.
            throw std::runtime_error("Desktop smoke test exceeded its five-second deadline");
        }
        simulation.advance(renderTime);
        const bafx::fx::FrameSnapshot snapshot = simulation.snapshot(
            toViewport(window.size()),
            renderTime);
        renderer.renderFrame(snapshot);
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
    try
    {
        return runApplication(instance, options);
    }
    catch (const std::exception& error)
    {
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
