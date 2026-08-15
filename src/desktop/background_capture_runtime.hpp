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

enum class BackgroundCaptureCancelResizePolicy : std::uint8_t
{
    Preserve,
    Discard
};

[[nodiscard]] BackgroundCaptureCancelResizePolicy
backgroundCaptureCancelResizePolicy(
    bool outputResizeSupersedes,
    bool displayTargetSupersedes) noexcept;

struct BackgroundCaptureExecutionResult
{
    std::string sensorFailure{};
    std::optional<bafx::windows::WindowSize> resizedOutputSize{};
    std::optional<bafx::windows::WindowSize> recreatedFramePoolSize{};
    bool deviceRecovered{false};
    bool deviceRecoveryAdapterChanged{false};
    bool outputAdapterRetargeted{false};
    bool outputAdapterWarpFallback{false};
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
};

[[nodiscard]] bool displayTargetBoundsApplied(
    const BackgroundCaptureExecutionResult& execution) noexcept;

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
    bafx::windows::BorderlessCaptureAccessAuthority& borderlessAccessAuthority,
    BackgroundCaptureExecutionResult& execution,
    const std::filesystem::path& logPath);

[[nodiscard]] BackgroundCaptureExecutionStatus cancelBackgroundCaptureTransition(
    bafx::windows::BackgroundCaptureTransition& transition,
    bafx::windows::OverlayWindow& window,
    bafx::windows::CompositionRenderer& renderer,
    bafx::windows::BorderlessCaptureAccessAuthority& borderlessAccessAuthority,
    BackgroundCaptureExecutionResult& execution,
    BackgroundCaptureCancelResizePolicy resizePolicy,
    std::string_view reason,
    const std::filesystem::path& logPath);

void appendDisplayTopologyObserved(
    const std::filesystem::path& logPath,
    std::uint64_t controlGeneration,
    bool transactionActive,
    const DisplayTarget& applied,
    std::uint32_t appliedDpi,
    const DisplayTarget& observed,
    std::uint32_t windowEffectiveDpi) noexcept;

void appendDisplayTopologyApplied(
    const std::filesystem::path& logPath,
    std::uint64_t controlGeneration,
    const DisplayTarget& previous,
    const DisplayTarget& applied,
    std::uint32_t dpi) noexcept;

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
