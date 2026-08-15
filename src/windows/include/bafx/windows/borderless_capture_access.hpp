#pragma once

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

inline constexpr std::uint32_t borderlessCaptureAccessTimeoutMilliseconds = 100U;
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

class BorderlessCaptureAccessRequest final
{
public:
    using Clock = std::chrono::steady_clock;

    explicit BorderlessCaptureAccessRequest(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(
            borderlessCaptureAccessPromptTimeoutMilliseconds)) noexcept;
    ~BorderlessCaptureAccessRequest() = default;

    BorderlessCaptureAccessRequest(const BorderlessCaptureAccessRequest&) = delete;
    BorderlessCaptureAccessRequest& operator=(
        const BorderlessCaptureAccessRequest&) = delete;
    BorderlessCaptureAccessRequest(BorderlessCaptureAccessRequest&&) noexcept = default;
    BorderlessCaptureAccessRequest& operator=(
        BorderlessCaptureAccessRequest&&) noexcept = default;

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
    Clock::time_point startedAt_{};
    std::chrono::milliseconds timeout_{};
    bool cancelRequested_{false};
};

[[nodiscard]] BorderlessCaptureAccessResult requestBorderlessCaptureAccess() noexcept;

// Accept a caller-owned identity snapshot so diagnostic collectors can retain
// exactly the evidence used to decide whether a broker request is legal.
[[nodiscard]] BorderlessCaptureAccessResult requestBorderlessCaptureAccess(
    const PackageIdentityInfo& identity) noexcept;

[[nodiscard]] bool borderlessCaptureAccessAllowed(
    const BorderlessCaptureAccessResult& result) noexcept;

[[nodiscard]] std::string_view borderlessCaptureAccessStatusName(
    BorderlessCaptureAccessStatus status) noexcept;

[[nodiscard]] std::string_view borderlessCaptureAccessAsyncStatusName(
    BorderlessCaptureAccessAsyncStatus status) noexcept;

[[nodiscard]] std::string borderlessCaptureAccessDiagnostic(
    const BorderlessCaptureAccessResult& result);

}
