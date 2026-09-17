#pragma once

#include "display_session.hpp"
#include "host_recovery_state.hpp"
#include "bafx/windows/runtime_diagnostics.hpp"

#include <functional>
#include <optional>
#include <string>

namespace bafx::desktop
{

class HostControlPlane;

// All borrowed resources outlive this runtime and remain on the render-owner
// thread. Publishing through the Host keeps diagnostics and IPC on one snapshot.
struct CoordinatorOutputContext final
{
    DisplaySession& displaySession;
    HostControlPlane& control;
    bafx::windows::SupportReport& report;
    BackgroundRetryState& backgroundRetry;
    DisplayCaptureSizeTracker& coordinatorCaptureSizeTracker;
    BackgroundCaptureObservation& backgroundObservation;
    bool& backgroundCaptureEnabled;
    bool& deviceRecoveryConsumed;
    const std::filesystem::path& logPath;
    std::function<void()> updateDisplayRuntimeSummary;
};

// One renderer operation with shared success/failure diagnostics. The caller
// owns capture teardown, retry scheduling and the safe mutation boundary.
[[nodiscard]] std::optional<bafx::windows::OutputRenegotiationResult>
tryRenegotiateOutput(
    const std::filesystem::path& logPath,
    DisplaySession& session,
    bafx::windows::CompositionOutputPolicy policy,
    std::string_view reason) noexcept;

class CoordinatorOutputRuntime final
{
public:
    explicit CoordinatorOutputRuntime(CoordinatorOutputContext context);
    CoordinatorOutputRuntime(const CoordinatorOutputRuntime&) = delete;
    CoordinatorOutputRuntime& operator=(const CoordinatorOutputRuntime&) = delete;

    [[nodiscard]] bool hasPending() const noexcept;
    [[nodiscard]] bool readyForAttempt(bool maintenanceDue) const noexcept;
    void clearPending() noexcept;

    // Call only when capture transactions, target changes and resizes are idle.
    void servicePending(bool backgroundCaptureRequested);
    [[nodiscard]] bool renegotiate(
        bafx::windows::CompositionOutputPolicy policy,
        std::string_view reason,
        bool backgroundCaptureRequested);
    void queueRecoveryIfNeeded(std::string_view reason);
    void refreshColorState(
        std::string_view reason,
        std::uint64_t generation,
        bool outputRebuiltForCurrentTarget,
        const std::optional<bafx::windows::DisplayColorCapabilities>& fallbackCapabilities,
        const DisplayTarget& appliedDisplayTarget,
        DisplaySessionColorRefreshRequest request =
            DisplaySessionColorRefreshRequest::Observation);

private:
    struct PendingOutputRenegotiation final
    {
        bafx::windows::CompositionOutputPolicy policy{};
        std::string reason{};
        OutputRenegotiationBudget budget{};
        bool retryPending{false};
    };

    [[nodiscard]] bool attemptOutput(
        bafx::windows::CompositionOutputPolicy policy,
        std::string_view reason,
        bool backgroundCaptureRequested);
    void retainFailed(PendingOutputRenegotiation pending);

    CoordinatorOutputContext context_;
    std::optional<PendingOutputRenegotiation> pending_{};
};

}
