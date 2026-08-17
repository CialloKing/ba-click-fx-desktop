#include "control_center_window.hpp"

#include "bafx/windows/portable_paths.hpp"

#include <windows.h>
#include <commctrl.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

namespace
{

constexpr std::wstring_view controlCenterMutexName = L"Local\\BAFX.ControlCenter.v1";
constexpr std::wstring_view controlCenterWindowTitle = L"BAFX Control Center";

struct LaunchOptions final
{
    bool startup{false};
    bool minimized{false};
};

[[nodiscard]] LaunchOptions launchOptions() noexcept
{
    LaunchOptions options{};
    for (int index = 1; index < __argc; ++index)
    {
        const std::wstring_view argument(__wargv[index]);
        if (argument == L"--startup")
        {
            options.startup = true;
        }
        else if (argument == L"--minimized")
        {
            options.minimized = true;
        }
    }
    return options;
}

void recordStartupFailure(const std::wstring_view message) noexcept
{
    const std::wstring line(message);
    OutputDebugStringW(line.c_str());
    OutputDebugStringW(L"\n");

    try
    {
        // Startup can fail before a window exists. A portable bundle therefore
        // keeps this diagnostic beside the executable rather than in UI state.
        std::wofstream stream(bafx::windows::executableFilePath(
            L"BAFX.ControlCenter.startup-error.log",
            L"BAFX.ControlCenter.startup-error.log"));
        stream << line << L'\n';
    }
    catch (...)
    {
        // Diagnostics must never hide the original startup failure.
    }
}

[[nodiscard]] std::wstring describeWin32Failure(const DWORD error)
{
    std::array<wchar_t, 1'024U> buffer{};
    const DWORD count = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0U,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        nullptr);
    if (count == 0U)
    {
        return L"Win32 错误 " + std::to_wstring(error);
    }

    std::wstring message(buffer.data(), count);
    while (!message.empty()
        && (message.back() == L'\r' || message.back() == L'\n'))
    {
        message.pop_back();
    }
    return L"Win32 错误 " + std::to_wstring(error) + L"：" + message;
}

[[nodiscard]] bool activateExistingControlCenter() noexcept
{
    const HWND existing = FindWindowW(nullptr, controlCenterWindowTitle.data());
    if (existing == nullptr)
    {
        return false;
    }
    static_cast<void>(ShowWindow(existing, SW_RESTORE));
    static_cast<void>(SetForegroundWindow(existing));
    return true;
}

class ProcessMutex final
{
public:
    ProcessMutex() = default;

    ~ProcessMutex()
    {
        if (handle_ != nullptr)
        {
            CloseHandle(handle_);
        }
    }

    ProcessMutex(const ProcessMutex&) = delete;
    ProcessMutex& operator=(const ProcessMutex&) = delete;

    [[nodiscard]] bool acquire() noexcept
    {
        // CreateMutexW only defines the last-error value for an existing name.
        // Clear it so an unrelated earlier error cannot look like a duplicate.
        SetLastError(ERROR_SUCCESS);
        handle_ = CreateMutexW(nullptr, TRUE, controlCenterMutexName.data());
        if (handle_ == nullptr)
        {
            return false;
        }
        return GetLastError() != ERROR_ALREADY_EXISTS;
    }

private:
    HANDLE handle_{nullptr};
};

}

int WINAPI wWinMain(
    const HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    const int showCommand)
{
    ProcessMutex instanceGuard;
    if (!instanceGuard.acquire())
    {
        static_cast<void>(activateExistingControlCenter());
        return 0;
    }

    // Native common controls are part of Windows and need no app-local runtime.
    const INITCOMMONCONTROLSEX commonControls{
        sizeof(INITCOMMONCONTROLSEX),
        ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    if (InitCommonControlsEx(&commonControls) == FALSE)
    {
        const DWORD error = GetLastError();
        const std::wstring message = describeWin32Failure(error);
        recordStartupFailure(message);
        MessageBoxW(nullptr, message.c_str(), L"BAFX 启动失败", MB_OK | MB_ICONERROR);
        return 1;
    }

    try
    {
        bafx::control_center::ControlCenterWindow window(instance);
        const LaunchOptions options = launchOptions();
        const int effectiveShowCommand = options.minimized
            ? SW_SHOWMINIMIZED
            : showCommand;
        if (!window.create(effectiveShowCommand, options.startup))
        {
            const std::wstring message = describeWin32Failure(window.lastError());
            recordStartupFailure(message);
            MessageBoxW(nullptr, message.c_str(), L"BAFX 启动失败", MB_OK | MB_ICONERROR);
            return 1;
        }
        return window.runMessageLoop();
    }
    catch (...)
    {
        constexpr std::wstring_view message = L"控制中心初始化时发生内部错误。";
        recordStartupFailure(message);
        MessageBoxW(nullptr, message.data(), L"BAFX 启动失败", MB_OK | MB_ICONERROR);
        return 1;
    }
}
