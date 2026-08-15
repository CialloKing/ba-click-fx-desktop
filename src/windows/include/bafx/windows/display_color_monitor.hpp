#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace bafx::windows
{

enum class DisplayColorMonitorStatus : std::uint8_t
{
    Active,
    InvalidTarget,
    Unsupported,
    Failed
};

struct DisplayColorMonitorResult
{
    DisplayColorMonitorStatus status{DisplayColorMonitorStatus::Failed};
    HRESULT error{E_UNEXPECTED};
    std::uint64_t generation{0U};
};

class DisplayColorMonitor final
{
public:
    DisplayColorMonitor() noexcept = default;
    ~DisplayColorMonitor() noexcept;

    DisplayColorMonitor(const DisplayColorMonitor&) = delete;
    DisplayColorMonitor& operator=(const DisplayColorMonitor&) = delete;
    DisplayColorMonitor(DisplayColorMonitor&&) = delete;
    DisplayColorMonitor& operator=(DisplayColorMonitor&&) = delete;

    [[nodiscard]] DisplayColorMonitorResult start(
        HMONITOR monitor,
        HWND wakeWindow) noexcept;
    [[nodiscard]] bool notificationPending() const noexcept;
    [[nodiscard]] std::uint64_t consumeNotification() noexcept;
    [[nodiscard]] bool active() const noexcept;
    void stop() noexcept;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_{};
};

[[nodiscard]] std::string_view displayColorMonitorStatusName(
    DisplayColorMonitorStatus status) noexcept;

[[nodiscard]] std::string displayColorMonitorDiagnostic(
    const DisplayColorMonitorResult& result);

}
