#include "bafx/windows/composition_renderer.hpp"
#include "bafx/windows/error.hpp"
#include "bafx/windows/overlay_window.hpp"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string_view>

namespace
{

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

struct RunOptions
{
    std::optional<std::uint32_t> frameLimit{};
    bool smokeTest{false};
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
            options.frameLimit = 3U;
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
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            quit = true;
            return;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
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
    bafx::windows::OverlayWindow window(
        instance,
        primaryMonitorBounds(),
        L"ba-click-fx-desktop");
    bafx::windows::CompositionRenderer renderer(window.handle(), window.size());
    window.show();

    bool quit = false;
    std::uint32_t renderedFrames = 0;
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

        renderer.renderTransparentFrame();
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
