#pragma once

#include "bafx/windows/external_host_trust.hpp"
#include "bafx/windows/wgc_runtime_capabilities.hpp"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace bafx::windows
{

struct PackageIdentityInfo;

inline constexpr std::uint32_t borderlessCaptureAccessPromptTimeoutMilliseconds =
    120000U;

enum class BorderlessCaptureAccessAsyncStatus : std::uint8_t
{
    NotStarted,
    Started,
    Completed,
    Canceled,
    Error
};

enum class BorderlessCaptureAccessStatus : std::uint8_t
{
    NotPackaged,
    Allowed,
    DeniedBySystem,
    NotDeclaredByApp,
    DeniedByUser,
    UserPromptRequired,
    TimedOut,
    Canceled,
    Unsupported,
    IdentityUntrusted,
    Failed
};

struct BorderlessCaptureAccessResult
{
    BorderlessCaptureAccessStatus status{
        BorderlessCaptureAccessStatus::NotPackaged};
    HRESULT error{S_OK};
    BorderlessCaptureAccessAsyncStatus asyncStatus{
        BorderlessCaptureAccessAsyncStatus::NotStarted};
    std::uint32_t elapsedMilliseconds{0U};
    bool cancelRequested{false};
    std::optional<BorderlessCaptureCapabilityResult> capability{};
    std::optional<ExternalHostTrustResult> externalHostTrust{};
};

class BorderlessCaptureAccessOperation
{
public:
    virtual ~BorderlessCaptureAccessOperation() = default;

    [[nodiscard]] virtual BorderlessCaptureAccessAsyncStatus status()
        const noexcept = 0;
    [[nodiscard]] virtual BorderlessCaptureAccessResult getResults() noexcept = 0;
    [[nodiscard]] virtual HRESULT error() const noexcept = 0;
    virtual void cancel() noexcept = 0;
};

struct BorderlessCaptureAccessPollResult
{
    bool pending{false};
    std::optional<BorderlessCaptureAccessResult> result{};
};

struct BorderlessCaptureAccessHealthResult
{
    BorderlessCaptureAccessStatus status{
        BorderlessCaptureAccessStatus::Failed};
    HRESULT error{E_UNEXPECTED};
    std::uint64_t generation{0U};
};

class BorderlessCaptureAccessMonitor final
{
public:
    BorderlessCaptureAccessMonitor() noexcept;
    ~BorderlessCaptureAccessMonitor() noexcept;

    BorderlessCaptureAccessMonitor(const BorderlessCaptureAccessMonitor&) = delete;
    BorderlessCaptureAccessMonitor& operator=(
        const BorderlessCaptureAccessMonitor&) = delete;
    BorderlessCaptureAccessMonitor(BorderlessCaptureAccessMonitor&&) = delete;
    BorderlessCaptureAccessMonitor& operator=(
        BorderlessCaptureAccessMonitor&&) = delete;

    [[nodiscard]] BorderlessCaptureAccessHealthResult start() noexcept;
    [[nodiscard]] BorderlessCaptureAccessHealthResult observe() noexcept;
    [[nodiscard]] bool notificationPending() const noexcept;
    [[nodiscard]] HANDLE changeEvent() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    void stop() noexcept;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_{};
};

class BorderlessCaptureAccessRequest final
{
public:
    using Clock = std::chrono::steady_clock;

    explicit BorderlessCaptureAccessRequest(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(
            borderlessCaptureAccessPromptTimeoutMilliseconds)) noexcept;
    ~BorderlessCaptureAccessRequest() noexcept;

    BorderlessCaptureAccessRequest(const BorderlessCaptureAccessRequest&) = delete;
    BorderlessCaptureAccessRequest& operator=(
        const BorderlessCaptureAccessRequest&) = delete;
    BorderlessCaptureAccessRequest(BorderlessCaptureAccessRequest&&) = delete;
    BorderlessCaptureAccessRequest& operator=(
        BorderlessCaptureAccessRequest&&) = delete;

    void begin(
        const PackageIdentityInfo& identity,
        Clock::time_point now = Clock::now()) noexcept;
    // Operation injection keeps broker timing tests deterministic without
    // granting a test process package identity or showing a system prompt.
    void begin(
        std::unique_ptr<BorderlessCaptureAccessOperation> operation,
        Clock::time_point now) noexcept;
    [[nodiscard]] BorderlessCaptureAccessPollResult poll(
        Clock::time_point now = Clock::now()) noexcept;
    void cancel(Clock::time_point now = Clock::now()) noexcept;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool pending() const noexcept;

private:
    std::unique_ptr<BorderlessCaptureAccessOperation> operation_{};
    std::optional<BorderlessCaptureAccessResult> readyResult_{};
    std::optional<BorderlessCaptureCapabilityResult> capability_{};
    std::optional<ExternalHostTrustResult> externalHostTrust_{};
    Clock::time_point startedAt_{};
    std::chrono::milliseconds timeout_{};
    bool cancelRequested_{false};
};

[[nodiscard]] bool borderlessCaptureAccessAllowed(
    const BorderlessCaptureAccessResult& result) noexcept;

[[nodiscard]] std::string_view borderlessCaptureAccessStatusName(
    BorderlessCaptureAccessStatus status) noexcept;

[[nodiscard]] std::string_view borderlessCaptureAccessAsyncStatusName(
    BorderlessCaptureAccessAsyncStatus status) noexcept;

[[nodiscard]] std::string borderlessCaptureAccessDiagnostic(
    const BorderlessCaptureAccessResult& result);

[[nodiscard]] std::string borderlessCaptureAccessHealthDiagnostic(
    const BorderlessCaptureAccessHealthResult& result);

}
