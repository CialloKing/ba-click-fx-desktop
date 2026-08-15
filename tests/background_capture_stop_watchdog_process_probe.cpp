#include "background_capture_stop_watchdog.hpp"

#include <windows.h>

#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr std::string_view triggerArgument = "--trigger-timeout";
constexpr DWORD childWaitTimeoutMilliseconds = 5'000U;
constexpr DWORD childFallbackSleepMilliseconds = 2'000U;
constexpr UINT probeCleanupExitCode = 125U;
static_assert(
    bafx::desktop::backgroundCaptureStopWatchdogTimeout
    == std::chrono::seconds(10));
static_assert(bafx::desktop::backgroundCaptureStopTimeoutExitCode == 124U);

[[nodiscard]] std::wstring currentExecutablePath()
{
    std::vector<wchar_t> buffer(32'768U, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0U || length >= buffer.size())
    {
        return {};
    }
    return std::wstring(buffer.data(), length);
}

void closeProcessInformation(PROCESS_INFORMATION& processInformation) noexcept
{
    if (processInformation.hThread != nullptr)
    {
        CloseHandle(processInformation.hThread);
        processInformation.hThread = nullptr;
    }
    if (processInformation.hProcess != nullptr)
    {
        CloseHandle(processInformation.hProcess);
        processInformation.hProcess = nullptr;
    }
}

[[nodiscard]] int runTimeoutChild()
{
    bafx::desktop::BackgroundCaptureStopWatchdog watchdog(
        std::chrono::milliseconds(50));
    if (!watchdog.arm())
    {
        return 10;
    }

    // A finite fallback keeps the probe bounded even if the worker never runs.
    Sleep(childFallbackSleepMilliseconds);
    return 11;
}

[[nodiscard]] int runProbeParent()
{
    const std::wstring executablePath = currentExecutablePath();
    if (executablePath.empty())
    {
        std::cerr << "GetModuleFileNameW failed: " << GetLastError() << '\n';
        return 1;
    }

    std::wstring commandLine = L"\"" + executablePath
        + L"\" --trigger-timeout";
    STARTUPINFOW startupInformation{};
    startupInformation.cb = sizeof(startupInformation);
    PROCESS_INFORMATION processInformation{};
    if (!CreateProcessW(
            executablePath.c_str(),
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startupInformation,
            &processInformation))
    {
        std::cerr << "CreateProcessW failed: " << GetLastError() << '\n';
        return 2;
    }

    const DWORD waitResult = WaitForSingleObject(
        processInformation.hProcess,
        childWaitTimeoutMilliseconds);
    if (waitResult != WAIT_OBJECT_0)
    {
        const DWORD waitError = waitResult == WAIT_FAILED
            ? GetLastError()
            : ERROR_SUCCESS;
        // Keep cleanup bounded, but report both operations so a surviving
        // child cannot be mistaken for successful containment.
        const bool terminationRequested = TerminateProcess(
            processInformation.hProcess,
            probeCleanupExitCode) != FALSE;
        const DWORD terminationError = terminationRequested
            ? ERROR_SUCCESS
            : GetLastError();
        const DWORD cleanupWaitResult = WaitForSingleObject(
            processInformation.hProcess,
            1'000U);
        closeProcessInformation(processInformation);
        std::cerr << "Watchdog child did not terminate in time: "
                  << waitResult
                  << "; wait error: " << waitError
                  << "; termination requested: "
                  << (terminationRequested ? "true" : "false")
                  << "; termination error: " << terminationError
                  << "; cleanup wait: " << cleanupWaitResult << '\n';
        return 3;
    }

    DWORD exitCode = 0U;
    if (!GetExitCodeProcess(processInformation.hProcess, &exitCode))
    {
        const DWORD error = GetLastError();
        closeProcessInformation(processInformation);
        std::cerr << "GetExitCodeProcess failed: " << error << '\n';
        return 4;
    }
    closeProcessInformation(processInformation);

    if (exitCode != bafx::desktop::backgroundCaptureStopTimeoutExitCode)
    {
        std::cerr << "Expected watchdog exit code "
                  << bafx::desktop::backgroundCaptureStopTimeoutExitCode
                  << ", got " << exitCode << '\n';
        return 5;
    }
    return 0;
}

}

int main(const int argumentCount, char* const arguments[])
{
    if (argumentCount == 2
        && std::string_view(arguments[1]) == triggerArgument)
    {
        return runTimeoutChild();
    }
    if (argumentCount != 1)
    {
        std::cerr << "Unexpected arguments\n";
        return 6;
    }
    return runProbeParent();
}
