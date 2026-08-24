#pragma once

#include "bafx/core/roi.hpp"
#include "bafx/fx/frame_bounds.hpp"
#include "frame_pacing.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace bafx::desktop
{

struct MetricSummary
{
    std::uint64_t sampleCount{0U};
    std::uint64_t recordedSampleCount{0U};
    std::uint64_t droppedSampleCount{0U};
    std::uint64_t minimum{0U};
    std::uint64_t p50{0U};
    std::uint64_t p95{0U};
    std::uint64_t p99{0U};
    std::uint64_t maximum{0U};
    double average{0.0};
};

struct InputPerformanceSample
{
    std::uint64_t rawInputMessages{0U};
    std::uint64_t moveEvents{0U};
    std::uint64_t buttonEdges{0U};
    std::uint64_t cancelEvents{0U};
    std::uint64_t compactedMoveEvents{0U};
    std::uint64_t overflowMoveDrops{0U};
    std::uint64_t messageTimeUnavailable{0U};
    std::uint64_t inputMessagesDispatched{0U};
    std::uint64_t otherMessagesDispatched{0U};
    std::uint32_t maximumPendingEvents{0U};
    std::uint32_t maximumWin32QueueAgeMilliseconds{0U};
    bool inputDispatchBudgetExhausted{false};
    bool otherDispatchBudgetExhausted{false};
};

enum class ActiveFxRoiActualPath : std::uint8_t
{
    Disabled,
    Idle,
    FullScreen,
    RoiWarmup,
    RoiPrefilter,
    Unavailable
};

[[nodiscard]] constexpr std::string_view activeFxRoiActualPathName(
    const ActiveFxRoiActualPath path) noexcept
{
    switch (path)
    {
    case ActiveFxRoiActualPath::Disabled:
        return "disabled";
    case ActiveFxRoiActualPath::Idle:
        return "idle";
    case ActiveFxRoiActualPath::FullScreen:
        return "full-screen";
    case ActiveFxRoiActualPath::RoiWarmup:
        return "roi-warmup";
    case ActiveFxRoiActualPath::RoiPrefilter:
        return "roi-prefilter";
    case ActiveFxRoiActualPath::Unavailable:
        return "unavailable";
    }
    return "unavailable";
}

enum class ActiveFxRoiDecisionReason : std::uint8_t
{
    Disabled,
    NoContent,
    BackgroundDifferentialBloom,
    Context1Unavailable,
    SharedTargetFullWrite,
    AreaTooLarge,
    BenefitTooSmall,
    Applied,
    RendererFallback
};

[[nodiscard]] constexpr std::string_view activeFxRoiDecisionReasonName(
    const ActiveFxRoiDecisionReason reason) noexcept
{
    switch (reason)
    {
    case ActiveFxRoiDecisionReason::Disabled:
        return "disabled";
    case ActiveFxRoiDecisionReason::NoContent:
        return "no-content";
    case ActiveFxRoiDecisionReason::BackgroundDifferentialBloom:
        return "background-differential-bloom";
    case ActiveFxRoiDecisionReason::Context1Unavailable:
        return "context1-unavailable";
    case ActiveFxRoiDecisionReason::SharedTargetFullWrite:
        return "shared-target-full-write";
    case ActiveFxRoiDecisionReason::AreaTooLarge:
        return "area-too-large";
    case ActiveFxRoiDecisionReason::BenefitTooSmall:
        return "benefit-too-small";
    case ActiveFxRoiDecisionReason::Applied:
        return "applied";
    case ActiveFxRoiDecisionReason::RendererFallback:
        return "renderer-fallback";
    }
    return "renderer-fallback";
}

struct ActiveFxRoiPassDiagnostics
{
    bool requested{false};
    bool eligible{false};
    bool executed{false};
    bool warmup{false};
    ActiveFxRoiActualPath actualPath{ActiveFxRoiActualPath::Disabled};
    ActiveFxRoiDecisionReason decisionReason{
        ActiveFxRoiDecisionReason::Disabled};
    std::uint64_t fullPixels{0U};
    std::uint64_t drawnPixels{0U};
    std::uint64_t clearedPixels{0U};
};

inline constexpr std::size_t activeFxRoiDecisionReasonCount =
    static_cast<std::size_t>(ActiveFxRoiDecisionReason::RendererFallback)
    + 1U;
inline constexpr std::size_t activeFxRoiStatusCount =
    static_cast<std::size_t>(bafx::core::ActiveFxRoiStatus::RendererFallback)
    + 1U;

struct ActiveFxRoiPathPerformanceSample
{
    bool observed{false};
    ActiveFxRoiPassDiagnostics diagnostics{};
};

struct ActiveFxRoiPathPerformanceSummary
{
    std::uint64_t observedFrames{0U};
    std::uint64_t requestedFrames{0U};
    std::uint64_t eligibleFrames{0U};
    std::uint64_t executedFrames{0U};
    std::uint64_t appliedFrames{0U};
    std::uint64_t warmupFrames{0U};
    std::uint64_t fullPixelsTotal{0U};
    std::uint64_t drawnPixelsTotal{0U};
    std::uint64_t clearedPixelsTotal{0U};
    bool lastExecuted{false};
    ActiveFxRoiActualPath lastActualPath{ActiveFxRoiActualPath::Disabled};
    ActiveFxRoiDecisionReason lastDecisionReason{
        ActiveFxRoiDecisionReason::Disabled};
    std::array<
        std::uint64_t,
        activeFxRoiDecisionReasonCount> decisionReasonFrames{};
};

struct GpuFxPathPerformanceSample
{
    std::uint64_t prefilterMicroseconds{0U};
    std::uint64_t pyramidMicroseconds{0U};
    std::uint64_t finalCompositeMicroseconds{0U};
    bool prefilterTimingValid{false};
    bool pyramidTimingValid{false};
    bool finalCompositeTimingValid{false};
};

struct GpuFxPathPerformanceSummary
{
    MetricSummary prefilterMicroseconds{};
    MetricSummary pyramidMicroseconds{};
    MetricSummary finalCompositeMicroseconds{};
};

struct FramePerformanceSample
{
    std::uint64_t frameTotalCpuMicroseconds{0U};
    std::uint64_t wgcDrainCpuMicroseconds{0U};
    std::uint64_t wgcOwnedCopySubmitCpuMicroseconds{0U};
    std::uint64_t backgroundSnapshotSubmitCpuMicroseconds{0U};
    std::uint64_t fxTotalSubmitCpuMicroseconds{0U};
    std::uint64_t fxMaterialsSubmitCpuMicroseconds{0U};
    std::uint64_t bloomAndCompositeSubmitCpuMicroseconds{0U};
    std::uint64_t diagnosticReadbackCpuMicroseconds{0U};
    std::uint64_t presentCallCpuMicroseconds{0U};
    std::uint64_t backgroundSampleAgeMicroseconds{0U};
    std::uint64_t wgcProducerCallbacks{0U};
    std::uint32_t wgcFramesAcquired{0U};
    std::uint32_t wgcFramesSuperseded{0U};
    std::uint32_t wgcTimestampRejectedFrames{0U};
    bool wgcActive{false};
    bool wgcDrainAttempted{false};
    bool wgcIdleDrainAttempted{false};
    bool wgcIdleDrainSkipped{false};
    bool wgcOwnedCopySubmitted{false};
    bool wgcAccepted{false};
    bool backgroundSnapshotRefreshAttempted{false};
    bool backgroundSnapshotRefreshed{false};
    bool backgroundParticipated{false};
    bool backgroundSampleAgeValid{false};
    bool diagnosticReadbackUsed{false};
    bafx::fx::FrameBoundsStatus roiVisualBoundsStatus{
        bafx::fx::FrameBoundsStatus::Empty};
    bafx::core::RoiStatus roiPlanStatus{bafx::core::RoiStatus::Empty};
    bool roiDirtyRectAvailable{false};
    bool roiPlanAvailable{false};
    std::uint64_t roiFullScreenPixels{0U};
    std::uint64_t roiBloomOutputPixels{0U};
    std::uint64_t roiAlignedWorkPixels{0U};
    std::uint32_t roiGuardX{0U};
    std::uint32_t roiGuardY{0U};
    std::uint32_t roiPhasePeriod{0U};
    bafx::core::RectI roiDirtyRect{};
    bafx::core::RectI roiBloomOutput{};
    bafx::core::RectI roiAlignedWork{};
    bool roiRequested{false};
    bool roiApplied{false};
    std::uint64_t roiPrefilterPixels{0U};
    bafx::core::ActiveFxRoiStatus roiActiveStatus{
        bafx::core::ActiveFxRoiStatus::Disabled};
    ActiveFxRoiPathPerformanceSample roiPrimary{};
    ActiveFxRoiPathPerformanceSample roiRecordingRebuild{};
    std::uint64_t gpuWgcDrainAndCopyMicroseconds{0U};
    std::uint64_t gpuBackgroundSnapshotMicroseconds{0U};
    std::uint64_t gpuFxMaterialsMicroseconds{0U};
    std::uint64_t gpuBloomAndFinalCompositeMicroseconds{0U};
    std::uint64_t gpuTotalFxMicroseconds{0U};
    std::uint64_t gpuRenderCommandSpanMicroseconds{0U};
    std::uint32_t gpuTimestampInitializationResult{0U};
    std::uint32_t gpuTimestampPendingFrames{0U};
    bool gpuTimestampProfilerObserved{false};
    bool gpuTimestampProfilerAvailable{false};
    bool gpuFrameStarted{false};
    bool gpuFrameSubmitted{false};
    bool gpuAutoSkippedStages{false};
    bool gpuPollPending{false};
    bool gpuRingFullSkipped{false};
    bool gpuSampleCompleted{false};
    bool gpuCancelledSlotReclaimed{false};
    bool gpuDisjointSample{false};
    bool gpuQueryFailure{false};
    bool gpuStateError{false};
    bool gpuWgcTimingValid{false};
    bool gpuBackgroundSnapshotTimingValid{false};
    bool gpuFxTimingValid{false};
    GpuFxPathPerformanceSample gpuPrimary{};
    GpuFxPathPerformanceSample gpuRecordingRebuild{};
};

struct RuntimePerformanceSummary
{
    std::uint64_t frameCount{0U};
    std::uint64_t wgcActiveFrames{0U};
    std::uint64_t wgcMaintenanceCycles{0U};
    std::uint64_t wgcDrainAttemptedFrames{0U};
    std::uint64_t wgcIdleDrainAttemptedFrames{0U};
    std::uint64_t wgcIdleDrainSkippedFrames{0U};
    std::uint64_t wgcProducerCallbacks{0U};
    std::uint64_t wgcFramesAcquired{0U};
    std::uint64_t wgcFramesSuperseded{0U};
    std::uint64_t wgcTimestampRejectedFrames{0U};
    std::uint64_t wgcOwnedCopiesSubmitted{0U};
    std::uint64_t wgcSamplesAccepted{0U};
    std::uint64_t backgroundSnapshotAttempts{0U};
    std::uint64_t backgroundSnapshotsRefreshed{0U};
    std::uint64_t backgroundParticipatingFrames{0U};
    std::uint64_t captureExclusionHealthChecks{0U};
    std::uint64_t captureExclusionHealthFailures{0U};
    std::uint64_t rawInputMessages{0U};
    std::uint64_t moveEvents{0U};
    std::uint64_t buttonEdges{0U};
    std::uint64_t cancelEvents{0U};
    std::uint64_t compactedMoveEvents{0U};
    std::uint64_t overflowMoveDrops{0U};
    std::uint64_t messageTimeUnavailable{0U};
    std::uint64_t inputMessagesDispatched{0U};
    std::uint64_t otherMessagesDispatched{0U};
    std::uint64_t inputDispatchBudgetExhaustions{0U};
    std::uint64_t otherDispatchBudgetExhaustions{0U};
    std::uint64_t roiVisualBoundsOkFrames{0U};
    std::uint64_t roiVisualBoundsEmptyFrames{0U};
    std::uint64_t roiVisualBoundsInvalidFrames{0U};
    std::uint64_t roiVisualBoundsOverflowFrames{0U};
    std::uint64_t roiDirtyRectFrames{0U};
    std::uint64_t roiPlanFrames{0U};
    std::uint64_t roiPlanEmptyFrames{0U};
    std::uint64_t roiPlanInvalidRectFrames{0U};
    std::uint64_t roiPlanInvalidFootprintFrames{0U};
    std::uint64_t roiPlanOverflowFrames{0U};
    std::uint64_t roiRequestedFrames{0U};
    std::uint64_t roiAppliedFrames{0U};
    std::array<std::uint64_t, activeFxRoiStatusCount> roiActiveStatusFrames{};
    bafx::core::ActiveFxRoiStatus roiLastActiveStatus{
        bafx::core::ActiveFxRoiStatus::Disabled};
    bafx::fx::FrameBoundsStatus roiLastVisualBoundsStatus{
        bafx::fx::FrameBoundsStatus::Empty};
    bafx::core::RoiStatus roiLastPlanStatus{bafx::core::RoiStatus::Empty};
    std::uint64_t framePacingFrameReadyWakes{0U};
    std::uint64_t framePacingDeviceRemovedWakes{0U};
    std::uint64_t framePacingCadenceWakes{0U};
    std::uint64_t framePacingMessageWakes{0U};
    std::uint64_t framePacingTimeouts{0U};
    std::uint64_t framePacingFailures{0U};
    std::uint64_t gpuFramesStarted{0U};
    std::uint64_t gpuFramesSubmitted{0U};
    std::uint64_t gpuPendingPolls{0U};
    std::uint64_t gpuRingFullSkipped{0U};
    std::uint64_t gpuSamplesCompleted{0U};
    std::uint64_t gpuAutoSkippedStageFrames{0U};
    std::uint64_t gpuCancelledSlotsReclaimed{0U};
    std::uint64_t gpuDisjointSamples{0U};
    std::uint64_t gpuQueryFailures{0U};
    std::uint64_t gpuStateErrors{0U};
    std::uint32_t gpuTimestampInitializationResult{0U};
    bool gpuTimestampProfilerObserved{false};
    bool gpuTimestampProfilerAvailable{false};
    MetricSummary frameTotalCpuMicroseconds{};
    MetricSummary wgcDrainCpuMicroseconds{};
    MetricSummary wgcOwnedCopySubmitCpuMicroseconds{};
    MetricSummary backgroundSnapshotSubmitCpuMicroseconds{};
    MetricSummary fxTotalSubmitCpuMicroseconds{};
    MetricSummary fxMaterialsSubmitCpuMicroseconds{};
    MetricSummary bloomAndCompositeSubmitCpuMicroseconds{};
    MetricSummary diagnosticReadbackCpuMicroseconds{};
    MetricSummary presentCallCpuMicroseconds{};
    MetricSummary backgroundSampleAgeMicroseconds{};
    MetricSummary maximumPendingEvents{};
    MetricSummary maximumWin32QueueAgeMilliseconds{};
    MetricSummary dispatchToPresentReturnMicroseconds{};
    MetricSummary messageToPresentReturnMilliseconds{};
    MetricSummary roiFullScreenPixels{};
    MetricSummary roiBloomOutputPixels{};
    MetricSummary roiAlignedWorkPixels{};
    MetricSummary roiGuardX{};
    MetricSummary roiGuardY{};
    MetricSummary roiPhasePeriod{};
    MetricSummary roiPrefilterPixels{};
    ActiveFxRoiPathPerformanceSummary roiPrimary{};
    ActiveFxRoiPathPerformanceSummary roiRecordingRebuild{};
    bool roiLastDirtyRectAvailable{false};
    bafx::core::RectI roiLastDirtyRect{};
    bool roiLastBloomOutputAvailable{false};
    bafx::core::RectI roiLastBloomOutput{};
    bool roiLastAlignedWorkAvailable{false};
    bafx::core::RectI roiLastAlignedWork{};
    MetricSummary gpuTimestampPendingFrames{};
    MetricSummary gpuWgcDrainAndCopyMicroseconds{};
    MetricSummary gpuBackgroundSnapshotMicroseconds{};
    MetricSummary gpuFxMaterialsMicroseconds{};
    MetricSummary gpuBloomAndFinalCompositeMicroseconds{};
    GpuFxPathPerformanceSummary gpuPrimary{};
    GpuFxPathPerformanceSummary gpuRecordingRebuild{};
    MetricSummary gpuTotalFxMicroseconds{};
    MetricSummary gpuRenderCommandSpanMicroseconds{};
};

class WgcCallbackDeltaTracker final
{
public:
    [[nodiscard]] std::uint64_t observe(
        bool active,
        std::uint64_t epoch,
        std::uint64_t callbacksTotal) noexcept;
    void reset() noexcept;

private:
    std::uint64_t epoch_{0U};
    std::uint64_t callbacksTotal_{0U};
    bool active_{false};
};

// Interactive diagnostics keep exact samples for one bounded reporting window.
// The all-sample average and extrema remain valid if an extreme cadence fills
// the buffer; the report exposes droppedSampleCount instead of hiding bias.
class BoundedMetric final
{
public:
    explicit BoundedMetric(std::size_t capacity = 4096U);

    void add(std::uint64_t value) noexcept;
    void reset() noexcept;

    [[nodiscard]] MetricSummary summarize() const;
    [[nodiscard]] bool empty() const noexcept;

private:
    std::vector<std::uint64_t> samples_{};
    std::size_t capacity_{0U};
    std::uint64_t sampleCount_{0U};
    std::uint64_t minimum_{0U};
    std::uint64_t maximum_{0U};
    long double total_{0.0L};
};

class ActiveFxRoiPathPerformanceWindow final
{
public:
    void add(const ActiveFxRoiPathPerformanceSample& sample) noexcept;
    void reset() noexcept;

    [[nodiscard]] ActiveFxRoiPathPerformanceSummary summarize() const noexcept;

private:
    ActiveFxRoiPathPerformanceSummary summary_{};
};

class GpuFxPathPerformanceWindow final
{
public:
    void add(const GpuFxPathPerformanceSample& sample) noexcept;
    void reset() noexcept;

    [[nodiscard]] GpuFxPathPerformanceSummary summarize() const;

private:
    BoundedMetric prefilterMicroseconds_{};
    BoundedMetric pyramidMicroseconds_{};
    BoundedMetric finalCompositeMicroseconds_{};
};

class RuntimePerformanceWindow final
{
public:
    void addInput(const InputPerformanceSample& sample) noexcept;
    void addFrame(const FramePerformanceSample& sample) noexcept;
    // A paused Host still services WGC, but that work is not a rendered frame.
    // Only transport diagnostics are consumed by this narrow entry point.
    void addBackgroundMaintenance(
        const FramePerformanceSample& sample) noexcept;
    void addFramePacingWake(FramePacingWake wake) noexcept;
    void addCaptureExclusionHealthCheck(bool confirmed) noexcept;
    void addDispatchToPresentReturn(std::uint64_t microseconds) noexcept;
    void addMessageToPresentReturn(std::uint64_t milliseconds) noexcept;
    void reset() noexcept;

    [[nodiscard]] RuntimePerformanceSummary summarize() const;
    [[nodiscard]] bool empty() const noexcept;

private:
    void addWgc(const FramePerformanceSample& sample) noexcept;

    std::uint64_t frameCount_{0U};
    std::uint64_t wgcActiveFrames_{0U};
    std::uint64_t wgcMaintenanceCycles_{0U};
    std::uint64_t wgcDrainAttemptedFrames_{0U};
    std::uint64_t wgcIdleDrainAttemptedFrames_{0U};
    std::uint64_t wgcIdleDrainSkippedFrames_{0U};
    std::uint64_t wgcProducerCallbacks_{0U};
    std::uint64_t wgcFramesAcquired_{0U};
    std::uint64_t wgcFramesSuperseded_{0U};
    std::uint64_t wgcTimestampRejectedFrames_{0U};
    std::uint64_t wgcOwnedCopiesSubmitted_{0U};
    std::uint64_t wgcSamplesAccepted_{0U};
    std::uint64_t backgroundSnapshotAttempts_{0U};
    std::uint64_t backgroundSnapshotsRefreshed_{0U};
    std::uint64_t backgroundParticipatingFrames_{0U};
    std::uint64_t captureExclusionHealthChecks_{0U};
    std::uint64_t captureExclusionHealthFailures_{0U};
    std::uint64_t rawInputMessages_{0U};
    std::uint64_t moveEvents_{0U};
    std::uint64_t buttonEdges_{0U};
    std::uint64_t cancelEvents_{0U};
    std::uint64_t compactedMoveEvents_{0U};
    std::uint64_t overflowMoveDrops_{0U};
    std::uint64_t messageTimeUnavailable_{0U};
    std::uint64_t inputMessagesDispatched_{0U};
    std::uint64_t otherMessagesDispatched_{0U};
    std::uint64_t inputDispatchBudgetExhaustions_{0U};
    std::uint64_t otherDispatchBudgetExhaustions_{0U};
    std::uint64_t roiVisualBoundsOkFrames_{0U};
    std::uint64_t roiVisualBoundsEmptyFrames_{0U};
    std::uint64_t roiVisualBoundsInvalidFrames_{0U};
    std::uint64_t roiVisualBoundsOverflowFrames_{0U};
    std::uint64_t roiDirtyRectFrames_{0U};
    std::uint64_t roiPlanFrames_{0U};
    std::uint64_t roiPlanEmptyFrames_{0U};
    std::uint64_t roiPlanInvalidRectFrames_{0U};
    std::uint64_t roiPlanInvalidFootprintFrames_{0U};
    std::uint64_t roiPlanOverflowFrames_{0U};
    std::uint64_t roiRequestedFrames_{0U};
    std::uint64_t roiAppliedFrames_{0U};
    std::array<std::uint64_t, activeFxRoiStatusCount> roiActiveStatusFrames_{};
    bafx::core::ActiveFxRoiStatus roiLastActiveStatus_{
        bafx::core::ActiveFxRoiStatus::Disabled};
    bafx::fx::FrameBoundsStatus roiLastVisualBoundsStatus_{
        bafx::fx::FrameBoundsStatus::Empty};
    bafx::core::RoiStatus roiLastPlanStatus_{bafx::core::RoiStatus::Empty};
    std::uint64_t framePacingFrameReadyWakes_{0U};
    std::uint64_t framePacingDeviceRemovedWakes_{0U};
    std::uint64_t framePacingCadenceWakes_{0U};
    std::uint64_t framePacingMessageWakes_{0U};
    std::uint64_t framePacingTimeouts_{0U};
    std::uint64_t framePacingFailures_{0U};
    std::uint64_t gpuFramesStarted_{0U};
    std::uint64_t gpuFramesSubmitted_{0U};
    std::uint64_t gpuPendingPolls_{0U};
    std::uint64_t gpuRingFullSkipped_{0U};
    std::uint64_t gpuSamplesCompleted_{0U};
    std::uint64_t gpuAutoSkippedStageFrames_{0U};
    std::uint64_t gpuCancelledSlotsReclaimed_{0U};
    std::uint64_t gpuDisjointSamples_{0U};
    std::uint64_t gpuQueryFailures_{0U};
    std::uint64_t gpuStateErrors_{0U};
    std::uint32_t gpuTimestampInitializationResult_{0U};
    bool gpuTimestampProfilerObserved_{false};
    bool gpuTimestampProfilerAvailable_{false};
    BoundedMetric frameTotalCpuMicroseconds_{};
    BoundedMetric wgcDrainCpuMicroseconds_{};
    BoundedMetric wgcOwnedCopySubmitCpuMicroseconds_{};
    BoundedMetric backgroundSnapshotSubmitCpuMicroseconds_{};
    BoundedMetric fxTotalSubmitCpuMicroseconds_{};
    BoundedMetric fxMaterialsSubmitCpuMicroseconds_{};
    BoundedMetric bloomAndCompositeSubmitCpuMicroseconds_{};
    BoundedMetric diagnosticReadbackCpuMicroseconds_{};
    BoundedMetric presentCallCpuMicroseconds_{};
    BoundedMetric backgroundSampleAgeMicroseconds_{};
    BoundedMetric maximumPendingEvents_{};
    BoundedMetric maximumWin32QueueAgeMilliseconds_{};
    BoundedMetric dispatchToPresentReturnMicroseconds_{};
    BoundedMetric messageToPresentReturnMilliseconds_{};
    BoundedMetric roiFullScreenPixels_{};
    BoundedMetric roiBloomOutputPixels_{};
    BoundedMetric roiAlignedWorkPixels_{};
    BoundedMetric roiGuardX_{};
    BoundedMetric roiGuardY_{};
    BoundedMetric roiPhasePeriod_{};
    BoundedMetric roiPrefilterPixels_{};
    ActiveFxRoiPathPerformanceWindow roiPrimary_{};
    ActiveFxRoiPathPerformanceWindow roiRecordingRebuild_{};
    bool roiLastDirtyRectAvailable_{false};
    bafx::core::RectI roiLastDirtyRect_{};
    bool roiLastBloomOutputAvailable_{false};
    bafx::core::RectI roiLastBloomOutput_{};
    bool roiLastAlignedWorkAvailable_{false};
    bafx::core::RectI roiLastAlignedWork_{};
    BoundedMetric gpuTimestampPendingFrames_{};
    BoundedMetric gpuWgcDrainAndCopyMicroseconds_{};
    BoundedMetric gpuBackgroundSnapshotMicroseconds_{};
    BoundedMetric gpuFxMaterialsMicroseconds_{};
    BoundedMetric gpuBloomAndFinalCompositeMicroseconds_{};
    GpuFxPathPerformanceWindow gpuPrimary_{};
    GpuFxPathPerformanceWindow gpuRecordingRebuild_{};
    BoundedMetric gpuTotalFxMicroseconds_{};
    BoundedMetric gpuRenderCommandSpanMicroseconds_{};
};

}
