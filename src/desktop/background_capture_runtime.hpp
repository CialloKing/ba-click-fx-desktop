#pragma once

#include "background_capture_stop_watchdog.hpp"
#include "display_target.hpp"

#include "bafx/config/config.hpp"
#include "bafx/windows/background_capture_transition.hpp"
#include "bafx/windows/borderless_capture_access.hpp"
#include "bafx/windows/runtime_diagnostics.hpp"

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace bafx::desktop
{

inline constexpr auto backgroundCaptureExclusionHealthInterval =
    std::chrono::seconds(1);

class CaptureExclusionHealthPoller final
{
public:
    [[nodiscard]] bool shouldQuery(
        bool captureActive,
        std::chrono::nanoseconds now) noexcept;

private:
    std::optional<std::chrono::nanoseconds> lastObservedAt_{};
};

enum class BackgroundCaptureExecutionStatus : std::uint8_t
{
    Pending,
    Completed
};

struct BackgroundCaptureExecutionResult
{
    std::string sensorFailure{};
    std::optional<bafx::windows::WindowSize> resizedOutputSize{};
    std::optional<bafx::windows::WindowSize> recreatedFramePoolSize{};
    bool deviceRecovered{false};
    bool deviceRecoveryAdapterChanged{false};
    bool transactionActive{false};
    bool pending{false};
    bool sensorRestartAllowed{true};
    bool borderlessAccessConfirmed{false};
    DisplayTargetIntent targetIntent{};
    std::uint64_t controlGeneration{0U};
    std::size_t actionIndex{0U};
    std::size_t executedActionCount{0U};
    std::chrono::steady_clock::time_point transactionStartedAt{};
    std::chrono::steady_clock::time_point actionStartedAt{};
    std::optional<bafx::windows::BackgroundCaptureAction> activeAction{};
    std::unique_ptr<bafx::windows::BorderlessCaptureAccessRequest>
        borderlessAccessRequest{};
};

class BackgroundCaptureStopMonitor final
{
public:
    explicit BackgroundCaptureStopMonitor(
        const std::filesystem::path& logPath,
        std::chrono::milliseconds timeout =
            backgroundCaptureStopWatchdogTimeout,
        BackgroundCaptureStopTimeoutHandler timeoutHandler = nullptr,
        const void* timeoutContext = nullptr);

    BackgroundCaptureStopMonitor(const BackgroundCaptureStopMonitor&) = delete;
    BackgroundCaptureStopMonitor& operator=(
        const BackgroundCaptureStopMonitor&) = delete;

    [[nodiscard]] bafx::windows::WgcBackgroundStopObserver observer() noexcept;

private:
    static void observe(
        const void* context,
        const bafx::windows::WgcBackgroundStopProgress& progress) noexcept;
    void record(
        const bafx::windows::WgcBackgroundStopProgress& progress) noexcept;

    const std::filesystem::path& logPath_;
    BackgroundCaptureStopWatchdog watchdog_;
};

[[nodiscard]] bafx::windows::BackgroundCaptureRequest backgroundCaptureRequest(
    const bafx::config::Config& config,
    std::uint64_t retryToken = 0U) noexcept;

[[nodiscard]] bool canRetryBackgroundCaptureAfterDeviceRecovery(
    bool captureRequested,
    bool sensorWasActive,
    bool adapterChanged,
    bafx::windows::GraphicsDriverType driverType,
    bool rendererRestartAllowed) noexcept;

[[nodiscard]] BackgroundCaptureExecutionStatus executeBackgroundCaptureTransition(
    bafx::windows::BackgroundCaptureTransition& transition,
    bafx::windows::OverlayWindow& window,
    bafx::windows::CompositionRenderer& renderer,
    const DisplayTargetIntent& targetIntent,
    std::uint64_t controlGeneration,
    BackgroundCaptureExecutionResult& execution,
    const std::filesystem::path& logPath);

[[nodiscard]] BackgroundCaptureExecutionStatus cancelBackgroundCaptureTransition(
    bafx::windows::BackgroundCaptureTransition& transition,
    bafx::windows::OverlayWindow& window,
    bafx::windows::CompositionRenderer& renderer,
    BackgroundCaptureExecutionResult& execution,
    std::string_view reason,
    const std::filesystem::path& logPath);

void appendBorderlessCaptureAccessCheck(
    const std::filesystem::path& logPath,
    std::uint64_t controlGeneration,
    std::size_t actionIndex,
    const bafx::windows::BorderlessCaptureAccessResult& result) noexcept;

void appendBackgroundCaptureResourceLedger(
    const std::filesystem::path& logPath,
    const bafx::windows::CompositionRenderer& renderer,
    std::string_view phase) noexcept;

bafx::windows::WgcBackgroundStopDiagnostics
appendBackgroundCaptureStopDiagnostics(
    const std::filesystem::path& logPath,
    bafx::windows::CompositionRenderer& renderer,
    std::string_view phase) noexcept;

[[nodiscard]] bafx::windows::BackgroundCaptureStatus backgroundCaptureStatus(
    bafx::windows::EffectiveBackgroundCapturePath path) noexcept;

void appendBackgroundCaptureOutcome(
    const std::filesystem::path& logPath,
    const bafx::windows::BackgroundCaptureRequest& request,
    const bafx::windows::BackgroundCaptureTransition& transition,
    const BackgroundCaptureExecutionResult& execution,
    const bafx::windows::CompositionRenderer& renderer);

void appendBackgroundSnapshotInvalidation(
    const std::filesystem::path& logPath,
    std::uint64_t controlGeneration,
    const bafx::windows::BackgroundSnapshotInvalidation& invalidation) noexcept;

void appendBackgroundCompositeParticipation(
    const std::filesystem::path& logPath,
    std::uint64_t controlGeneration,
    const bafx::windows::CompositionFrameDiagnostics& diagnostics) noexcept;

void appendCaptureExclusionHealthFailure(
    const std::filesystem::path& logPath,
    std::uint64_t controlGeneration,
    bool transactionPending,
    const bafx::windows::CaptureExclusionQueryStatus& status) noexcept;

}
