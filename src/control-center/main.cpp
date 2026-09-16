#include "control_center_window.hpp"

#include "bafx/windows/portable_paths.hpp"

#include <windows.h>
#include <commctrl.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

using namespace bafx::control_center;

namespace
{

constexpr std::wstring_view controlCenterMutexName = L"Local\\BAFX.ControlCenter.v1";

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
    logControlCenterMessage("Process.Startup.Failed", message, bafx::windows::DiagnosticLevel::Error);
    logControlCenterLifecycle("Process.Exited", "startup-failed", 1);
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

[[nodiscard]] UiMessage describeWin32Failure(const DWORD error)
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
        return UiMessage(TextId::Win32Failure, {std::to_wstring(error), std::wstring{}});
    }

    std::wstring message(buffer.data(), count);
    while (!message.empty()
        && (message.back() == L'\r' || message.back() == L'\n'))
    {
        message.pop_back();
    }
    return UiMessage(TextId::Win32Failure, {std::to_wstring(error), message});
}

[[nodiscard]] bool activateExistingControlCenter() noexcept
{
    // The caption contains the product version and is intentionally mutable.
    // The fixed class name is the stable single-instance activation contract.
    const HWND existing = FindWindowW(
        bafx::control_center::controlCenterWindowClassName.data(),
        nullptr);
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
    logControlCenterLifecycle("Process.Startup", launchOptions().startup ? "windows-startup" : "interactive");

    try
    {
        setUiLanguage(loadLanguagePreference(languagePreferencePath(bafx::windows::executableDirectory())));
    }
    catch (...)
    {
        setUiLanguage(UiLanguage::System);
    }

    // Native common controls are part of Windows and need no app-local runtime.
    const INITCOMMONCONTROLSEX commonControls{
        sizeof(INITCOMMONCONTROLSEX),
        ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    if (InitCommonControlsEx(&commonControls) == FALSE)
    {
        const DWORD error = GetLastError();
        const UiMessage message = describeWin32Failure(error);
        recordStartupFailure(message.render(UiLanguage::SimplifiedChinese));
        localizedMessageBox(nullptr, message, TextId::StartupFailed, MB_OK | MB_ICONERROR);
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
            const UiMessage message = describeWin32Failure(window.lastError());
            recordStartupFailure(message.render(UiLanguage::SimplifiedChinese));
            localizedMessageBox(nullptr, message, TextId::StartupFailed, MB_OK | MB_ICONERROR);
            return 1;
        }
        const int result = window.runMessageLoop();
        logControlCenterLifecycle("Process.Exited", "message-loop-ended", result);
        return result;
    }
    catch (...)
    {
        recordStartupFailure(translatedText(TextId::InitializationFailed, UiLanguage::SimplifiedChinese));
        localizedMessageBox(nullptr, TextId::InitializationFailed, TextId::StartupFailed, MB_OK | MB_ICONERROR);
        return 1;
    }
}
