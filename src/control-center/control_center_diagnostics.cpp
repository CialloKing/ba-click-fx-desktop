#include "control_center_diagnostics.hpp"

#include "startup_config.hpp"
#include "bafx/windows/portable_paths.hpp"
#include "product/version.hpp"

#include <array>
#include <chrono>
#include <utility>

namespace bafx::control_center
{

bafx::windows::IpcClientOptions controlCenterIpcOptions()
{
    bafx::windows::IpcClientOptions options{};
    // Bound UI command acknowledgements and background-reader shutdown alike.
    options.timeoutMilliseconds = 100U;
    return options;
}

namespace
{
const ULONGLONG processStartedAt = GetTickCount64();

std::string toUtf8(const std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(count), '\0');
    if (count > 0)
    {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
            result.data(), count, nullptr, nullptr);
    }
    return result;
}

std::string_view statusName(const bafx::windows::IpcClientStatus status) noexcept
{
    using enum bafx::windows::IpcClientStatus;
    switch (status)
    {
    case Ok: return "ok";
    case InvalidOptions: return "invalid-options";
    case InvalidRequest: return "invalid-request";
    case ConnectFailed: return "connect-failed";
    case Timeout: return "timeout";
    case WriteFailed: return "write-failed";
    case ReadFailed: return "read-failed";
    case ResponseTooLarge: return "response-too-large";
    case InvalidResponse: return "invalid-response";
    case InternalError: return "internal-error";
    }
    return "unknown";
}
}

const std::filesystem::path& controlCenterLogPath()
{
    // Control Center has no Host package identity; resolve the installed data
    // directory through the same installation state as its startup settings.
    static const auto path = startupConfigPath(bafx::windows::executableDirectory()).parent_path()
        / L"BAFX.ControlCenter.log";
    return path;
}

void logControlCenterEvent(const std::string_view event,
    const std::initializer_list<bafx::windows::DiagnosticField> fields,
    const bafx::windows::DiagnosticLevel level) noexcept
{
    try
    {
        bafx::windows::appendDiagnosticEvent(controlCenterLogPath(), event,
            std::span(fields.begin(), fields.size()), level);
    }
    catch (...)
    {
        OutputDebugStringW(L"BAFX: Control Center diagnostic path is unavailable.\n");
    }
}

void logControlCenterMessage(const std::string_view event, const std::wstring_view message,
    const bafx::windows::DiagnosticLevel level) noexcept
{
    try
    {
        const auto text = toUtf8(message);
        logControlCenterEvent(event, {{"Message", text}}, level);
    }
    catch (...)
    {
    }
}

void logControlCenterLifecycle(const std::string_view event, const std::string_view reason,
    const int exitCode) noexcept
{
    try
    {
        const auto executable = toUtf8(bafx::windows::executableDirectory().native());
        const auto directory = toUtf8(controlCenterLogPath().parent_path().native());
        const auto code = std::to_string(exitCode);
        const auto uptime = std::to_string(GetTickCount64() - processStartedAt);
#if defined(BAFX_ENABLE_SPOUT2)
        constexpr std::string_view build = "full";
#else
        constexpr std::string_view build = "slim";
#endif
        const std::array fields{
            bafx::windows::DiagnosticField{"Process.Component", "control-center"},
            bafx::windows::DiagnosticField{"Product.Version", bafx::product::version},
            bafx::windows::DiagnosticField{"Product.Build", build},
            bafx::windows::DiagnosticField{"Process.Reason", reason},
            bafx::windows::DiagnosticField{"Process.ExitCode", code},
            bafx::windows::DiagnosticField{"Process.UptimeMs", uptime},
            bafx::windows::DiagnosticField{"Process.ExecutableDirectory", executable},
            bafx::windows::DiagnosticField{"Process.DataDirectory", directory}};
        bafx::windows::appendDiagnosticRecord(controlCenterLogPath(), event, fields,
            bafx::windows::diagnosticLogHealthReport(), exitCode == 0
                ? bafx::windows::DiagnosticLevel::Info : bafx::windows::DiagnosticLevel::Error);
    }
    catch (...)
    {
    }
}

DiagnosticIpcClient::DiagnosticIpcClient(bafx::windows::IpcClientOptions options)
    : client_(options), timeoutMilliseconds_(options.timeoutMilliseconds)
{
}

bafx::windows::IpcClientResponse DiagnosticIpcClient::transact(const std::string_view request) const noexcept
{
    const auto requestSequence = ++requestSequence_;
    try
    {
        const auto command = request.substr(0U, request.find(' '));
        if (!command.starts_with("Get"))
        {
            const auto sequence = std::to_string(requestSequence);
            logControlCenterEvent("IPC.Requested", {{"IPC.Command", command}, {"IPC.RequestSequence", sequence}});
        }
    }
    catch (...)
    {
    }
    const auto started = std::chrono::steady_clock::now();
    auto response = client_.transact(request);
    try
    {
        const std::string command(request.substr(0U, request.find(' ')));
        const auto sequence = std::to_string(requestSequence);
        const auto elapsed = std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count());
        const bool polling = command == "GetState" || command == "GetDisplayState"
            || command == "GetConfig" || command == "GetFxConfig" || command == "GetHotkeyState";
        const ULONGLONG now = GetTickCount64();
        std::uint64_t suppressed = 0U;
        ULONGLONG failureDuration = 0U;
        bool recovered = false;
        if (polling)
        {
            auto previous = failures_.find(command);
            if (response.succeeded())
            {
                if (previous == failures_.end())
                {
                    return response;
                }
                recovered = true;
                suppressed = previous->second.suppressed;
                failureDuration = now - previous->second.firstFailureAt;
                failures_.erase(previous);
            }
            else
            {
                const std::string signature = std::string(statusName(response.status)) + ":"
                    + std::to_string(response.win32Error) + ":" + response.errorCode;
                if (previous != failures_.end())
                {
                    auto& state = previous->second;
                    if (state.signature == signature && now - state.lastReportedAt < 30'000U)
                    {
                        ++state.suppressed;
                        return response;
                    }
                    suppressed = state.suppressed;
                    failureDuration = now - state.firstFailureAt;
                }
                const auto first = previous == failures_.end() ? now : previous->second.firstFailureAt;
                failures_.insert_or_assign(command, FailureState{signature, first, now, 0U});
            }
        }
        const auto error = std::to_string(response.win32Error);
        const auto timeout = std::to_string(timeoutMilliseconds_);
        const auto skipped = std::to_string(suppressed);
        const auto duration = std::to_string(failureDuration);
        logControlCenterEvent(recovered ? "IPC.Recovered" : "IPC.Completed", {
            {"IPC.Command", command}, {"IPC.RequestSequence", sequence},
            {"IPC.TransportStatus", statusName(response.status)},
            {"IPC.CommandSucceeded", response.commandSucceeded ? "true" : "false"},
            {"IPC.ElapsedUs", elapsed}, {"IPC.TimeoutMs", timeout},
            {"IPC.SuppressedFailures", skipped}, {"IPC.FailureDurationMs", duration},
            {"Error.Win32", error}, {"Error.Code", response.errorCode}, {"Error.Message", response.errorMessage}},
            response.succeeded() ? bafx::windows::DiagnosticLevel::Info : bafx::windows::DiagnosticLevel::Warning);
    }
    catch (...)
    {
        // Diagnostic allocation/formatting must never alter an IPC result.
    }
    return response;
}

}
