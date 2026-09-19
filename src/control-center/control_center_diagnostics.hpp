#pragma once

#include "bafx/windows/diagnostic_log.hpp"
#include "bafx/windows/ipc_client.hpp"

#include <initializer_list>
#include <map>

namespace bafx::control_center
{

[[nodiscard]] bafx::windows::IpcClientOptions controlCenterIpcOptions();
[[nodiscard]] const std::filesystem::path& controlCenterLogPath();
void logControlCenterEvent(std::string_view event,
    std::initializer_list<bafx::windows::DiagnosticField> fields = {},
    bafx::windows::DiagnosticLevel level = bafx::windows::DiagnosticLevel::Info) noexcept;
void logControlCenterMessage(std::string_view event, std::wstring_view message,
    bafx::windows::DiagnosticLevel level) noexcept;
void logControlCenterLifecycle(std::string_view event, std::string_view reason, int exitCode = 0) noexcept;

// Each instance has one owner thread. UI actions and background polling use
// separate clients; repeated failures are summarized every 30 seconds and on recovery.
class DiagnosticIpcClient final
{
public:
    explicit DiagnosticIpcClient(bafx::windows::IpcClientOptions options = {});
    [[nodiscard]] bafx::windows::IpcClientResponse transact(std::string_view request) const noexcept;

private:
    struct FailureState
    {
        std::string signature;
        ULONGLONG firstFailureAt{0U};
        ULONGLONG lastReportedAt{0U};
        std::uint64_t suppressed{0U};
    };
    bafx::windows::NamedPipeIpcClient client_;
    DWORD timeoutMilliseconds_;
    mutable std::map<std::string, FailureState> failures_;
    mutable std::uint64_t requestSequence_{0U};
};

}
