#pragma once

#include "display_capture_size_tracker.hpp"
#include "display_target.hpp"

#include "bafx/core/background_freshness.hpp"
#include "bafx/fx/simulation_runtime.hpp"
#include "bafx/fx/simulation_timeline.hpp"
#include "bafx/windows/background_capture_transition.hpp"
#include "bafx/windows/borderless_capture_access.hpp"
#include "bafx/windows/composition_renderer.hpp"
#include "bafx/windows/display_capabilities.hpp"
#include "bafx/windows/display_color_monitor.hpp"
#include "bafx/windows/overlay_window.hpp"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace bafx::desktop
{

inline constexpr std::uint32_t maximumOutputRenegotiationAttempts = 3U;

struct DisplaySessionOptions final
{
    HINSTANCE instance{nullptr};
    HWND wakeWindow{nullptr};
    bafx::windows::BorderlessCaptureAccessAuthority*
        borderlessAccessAuthority{nullptr};
    DisplayTarget target{};
    std::wstring_view title{};
    bafx::windows::FxBloomSettings bloomSettings{};
    bafx::windows::WgcBackgroundStopObserver backgroundStopObserver{};
    bafx::windows::CompositionOutputPreference outputPreference{
        bafx::windows::CompositionOutputPreference::ConservativeSdr};
    std::uint64_t simulationSeed{0U};
};

struct DisplaySessionRetargetResult final
{
    bafx::windows::OutputAdapterRetargetStatus adapter{
        bafx::windows::OutputAdapterRetargetStatus::Unchanged};
    bafx::windows::OutputResizeStatus output{
        bafx::windows::OutputResizeStatus::Unchanged};
    bool pending{false};
};

enum class DisplaySessionBackgroundRecoveryStatus : std::uint8_t
{
    NotRequired,
    Queued,
    Blocked
};

enum class DisplaySessionColorRefreshStatus : std::uint8_t
{
    Refreshed,
    RetainedTransactionSnapshot,
    RetainedLastKnownSnapshot,
    Unavailable
};

enum class DisplaySessionColorRefreshRequest : std::uint8_t
{
    Observation,
    Retry
};

struct DisplaySessionDeviceRecoveryResult final
{
    bool recovered{false};
    bool adapterChanged{false};
    bool backgroundWasActive{false};
    DisplaySessionBackgroundRecoveryStatus background{
        DisplaySessionBackgroundRecoveryStatus::NotRequired};
};

struct DisplaySessionBackgroundCaptureServiceResult final
{
    bool renderInvalidated{false};
    bool deviceRecovered{false};
    bool active{false};
    bool outputRenegotiationDiscarded{false};
    std::optional<bafx::windows::OutputRenegotiationResult>
        outputRenegotiation{};
    std::optional<DisplayTarget> outputRenegotiationTarget{};
    bafx::windows::CompositionOutputPolicy outputRenegotiationPolicy{};
    std::string outputRenegotiationReason{};
    std::string outputRenegotiationFailure{};
    bool outputRenegotiationRetryPending{false};
    std::uint32_t outputRenegotiationRetriesRemaining{0U};
};

struct DisplaySessionBackgroundCaptureState;

// Owns the window, graphics device and authored state for one display. Host
// input, tray and process lifetime stay outside so additional sessions cannot
// duplicate process-global registrations.
class DisplaySession final
{
public:
    explicit DisplaySession(DisplaySessionOptions options);
    ~DisplaySession();

    DisplaySession(const DisplaySession&) = delete;
    DisplaySession& operator=(const DisplaySession&) = delete;
    DisplaySession(DisplaySession&&) = delete;
    DisplaySession& operator=(DisplaySession&&) = delete;

    [[nodiscard]] const DisplayTarget& target() const noexcept;
    // A secondary WGC transaction keeps the old applied target alive until
    // commit. Topology reconciliation must compare against its pending intent
    // so a periodic poll cannot cancel and restart the same permission wait.
    [[nodiscard]] const DisplayTarget& reconciliationTarget() const noexcept;
    [[nodiscard]] bool retargetPendingFor(
        const DisplayTarget& target) const noexcept;
    // A pending permission transaction owns stable geometry and adapter
    // identity, but DPI/DRR metadata may advance while the user responds.
    // Merge that metadata without canceling the bounded transaction.
    [[nodiscard]] bool updatePendingTargetMetadata(
        DisplayTarget target) noexcept;
    [[nodiscard]] bafx::windows::OverlayWindow& window() noexcept;
    [[nodiscard]] const bafx::windows::OverlayWindow& window() const noexcept;
    [[nodiscard]] bafx::windows::CompositionRenderer& renderer() noexcept;
    [[nodiscard]] const bafx::windows::CompositionRenderer& renderer() const noexcept;
    [[nodiscard]] bafx::fx::SimulationRuntime& simulation() noexcept;
    [[nodiscard]] const bafx::fx::SimulationRuntime& simulation() const noexcept;
    [[nodiscard]] bafx::fx::SimulationTimeline& timeline() noexcept;
    [[nodiscard]] bafx::windows::DisplayColorMonitor& colorMonitor() noexcept;
    [[nodiscard]] const std::optional<bafx::windows::DisplayColorCapabilities>&
        colorCapabilities() const noexcept;
    [[nodiscard]] bafx::windows::CompositionOutputPreference
        requestedOutputPreference() const noexcept;
    void setRequestedOutputPreference(
        bafx::windows::CompositionOutputPreference preference) noexcept;
    [[nodiscard]] const bafx::windows::DisplayColorMonitorResult&
        colorMonitorResult() const noexcept;
    [[nodiscard]] bool renderFaulted() const noexcept;
    [[nodiscard]] bool lastPresentedDrawableContent() const noexcept;
    [[nodiscard]] bool resourceDomainReadyForTarget(
        const DisplayTarget& target) const noexcept;
    [[nodiscard]] bool framePacingDue(
        bafx::core::MonotonicTime now) const noexcept;
    [[nodiscard]] std::optional<bafx::core::MonotonicTime>
        nextFramePacingDeadline() const noexcept;

    // Call only after the owner has transactionally moved the HWND and
    // renderer resource domain. Monitoring is rebound first; the owner then
    // samples color state so it can compare the old and new target modes.
    void acceptAppliedTarget(DisplayTarget target, HWND wakeWindow) noexcept;
    // Refreshes DPI, cadence and primary-role metadata when the stable source
    // and render geometry have not changed.
    [[nodiscard]] bafx::windows::BackgroundCadenceRefreshResult
        updateTargetMetadata(DisplayTarget target) noexcept;
    // Secondary sessions currently retarget their FX-only resource domain
    // without entering the coordinator's WGC transaction.
    [[nodiscard]] DisplaySessionRetargetResult retargetFxOnly(
        DisplayTarget target,
        HWND wakeWindow);
    [[nodiscard]] DisplaySessionRetargetResult retargetSecondary(
        DisplayTarget target,
        HWND wakeWindow);
    [[nodiscard]] DisplaySessionDeviceRecoveryResult tryRecoverDevice() noexcept;
    [[nodiscard]] DisplaySessionDeviceRecoveryResult setBloomSettings(
        bafx::windows::FxBloomSettings settings);
    void initializeSecondaryBackgroundCapture(
        bafx::windows::BackgroundCaptureRequest request,
        std::uint64_t controlGeneration,
        const std::filesystem::path& logPath,
        bool powerUnavailable);
    void updateSecondaryBackgroundCaptureRequest(
        bafx::windows::BackgroundCaptureRequest request,
        std::uint64_t controlGeneration);
    void requestSecondaryOutputRenegotiation(
        bafx::windows::CompositionOutputPolicy policy,
        std::string_view reason,
        std::optional<DisplayTarget> target = std::nullopt);
    [[nodiscard]] DisplaySessionBackgroundCaptureServiceResult
        serviceSecondaryBackgroundCapture(bafx::core::MonotonicTime now);
    [[nodiscard]] DisplaySessionBackgroundCaptureServiceResult
        handleSecondaryBorderlessAccessLost(bafx::core::MonotonicTime now);
    [[nodiscard]] bool retrySecondaryBorderlessAccess(
        std::uint64_t controlGeneration);
    [[nodiscard]] DisplaySessionBackgroundCaptureServiceResult
        suspendSecondaryBackgroundCaptureForPower(
            bafx::core::MonotonicTime now);
    [[nodiscard]] bool requestSecondaryPowerRecovery(
        std::uint64_t controlGeneration) noexcept;
    [[nodiscard]] bool secondaryBackgroundCaptureInitialized() const noexcept;
    [[nodiscard]] bool secondaryBackgroundCaptureActive() const noexcept;
    [[nodiscard]] HANDLE secondaryBackgroundFrameAvailableObject() const noexcept;
    [[nodiscard]] bool takeTopologyRefreshRequest() noexcept;
    void observeSecondaryCaptureTopology(
        const DisplayTargetSnapshot& topology) noexcept;
    void shutdownSecondaryBackgroundCapture() noexcept;
    [[nodiscard]] bool colorRefreshRetryPending() const noexcept;
    [[nodiscard]] std::uint32_t colorRefreshRetriesRemaining() const noexcept;
    [[nodiscard]] DisplaySessionColorRefreshStatus refreshColorCapabilities(
        const std::optional<bafx::windows::DisplayColorCapabilities>& fallback =
            std::nullopt,
        DisplaySessionColorRefreshRequest request =
            DisplaySessionColorRefreshRequest::Observation) noexcept;
    void recordPresentedFrame(
        bool drawable,
        bafx::core::MonotonicTime startedAt,
        bafx::core::MonotonicTime minimumPeriod) noexcept;
    void resetFramePacing() noexcept;
    void markRenderFaulted() noexcept;
    void clearRenderFault();
    void show();

private:
    [[nodiscard]] DisplaySessionDeviceRecoveryResult finishDeviceRecovery(
        const bafx::windows::GraphicsDeviceInfo& previousDeviceInfo,
        bool backgroundWasActive) noexcept;
    void acceptPendingSecondaryTargetIfApplied(
        DisplaySessionBackgroundCaptureState& state);
    [[nodiscard]] static std::optional<LUID> requestedAdapter(
        const DisplayTarget& target) noexcept;

    bafx::windows::BorderlessCaptureAccessAuthority*
        borderlessAccessAuthority_{nullptr};
    DisplayTarget target_{};
    bafx::windows::OverlayWindow window_;
    bafx::windows::CompositionOutputPreference requestedOutputPreference_{
        bafx::windows::CompositionOutputPreference::ConservativeSdr};
    std::optional<bafx::windows::DisplayColorCapabilities> colorCapabilities_{};
    bafx::windows::CompositionRenderer renderer_;
    bafx::fx::SimulationRuntime simulation_;
    bafx::fx::SimulationTimeline timeline_{};
    bafx::windows::DisplayColorMonitor colorMonitor_{};
    std::optional<bafx::core::MonotonicTime> nextFramePacingDeadline_{};
    bool lastPresentedDrawableContent_{false};
    bool renderFaulted_{false};
    std::uint32_t colorRefreshRetriesRemaining_{0U};
    std::unique_ptr<DisplaySessionBackgroundCaptureState>
        secondaryBackgroundCapture_{};
};

}
