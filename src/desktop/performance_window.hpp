#pragma once

#include "frame_pacing.hpp"

#include <cstddef>
#include <cstdint>
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
    bool wgcIdleDrainSkipped{false};
    bool wgcOwnedCopySubmitted{false};
    bool wgcAccepted{false};
    bool backgroundSnapshotRefreshAttempted{false};
    bool backgroundSnapshotRefreshed{false};
    bool backgroundParticipated{false};
    bool backgroundSampleAgeValid{false};
    bool diagnosticReadbackUsed{false};
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
};

struct RuntimePerformanceSummary
{
    std::uint64_t frameCount{0U};
    std::uint64_t wgcActiveFrames{0U};
    std::uint64_t wgcDrainAttemptedFrames{0U};
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
    std::uint64_t framePacingFrameReadyWakes{0U};
    std::uint64_t framePacingMessageWakes{0U};
    std::uint64_t framePacingTimeouts{0U};
    std::uint64_t framePacingFailures{0U};
    std::uint64_t gpuFramesStarted{0U};
    std::uint64_t gpuFramesSubmitted{0U};
    std::uint64_t gpuPendingPolls{0U};
    std::uint64_t gpuRingFullSkipped{0U};
    std::uint64_t gpuSamplesCompleted{0U};
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
    MetricSummary gpuTimestampPendingFrames{};
    MetricSummary gpuWgcDrainAndCopyMicroseconds{};
    MetricSummary gpuBackgroundSnapshotMicroseconds{};
    MetricSummary gpuFxMaterialsMicroseconds{};
    MetricSummary gpuBloomAndFinalCompositeMicroseconds{};
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

class RuntimePerformanceWindow final
{
public:
    void addInput(const InputPerformanceSample& sample) noexcept;
    void addFrame(const FramePerformanceSample& sample) noexcept;
    void addFramePacingWake(FramePacingWake wake) noexcept;
    void addDispatchToPresentReturn(std::uint64_t microseconds) noexcept;
    void addMessageToPresentReturn(std::uint64_t milliseconds) noexcept;
    void reset() noexcept;

    [[nodiscard]] RuntimePerformanceSummary summarize() const;
    [[nodiscard]] bool empty() const noexcept;

private:
    std::uint64_t frameCount_{0U};
    std::uint64_t wgcActiveFrames_{0U};
    std::uint64_t wgcDrainAttemptedFrames_{0U};
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
    std::uint64_t framePacingFrameReadyWakes_{0U};
    std::uint64_t framePacingMessageWakes_{0U};
    std::uint64_t framePacingTimeouts_{0U};
    std::uint64_t framePacingFailures_{0U};
    std::uint64_t gpuFramesStarted_{0U};
    std::uint64_t gpuFramesSubmitted_{0U};
    std::uint64_t gpuPendingPolls_{0U};
    std::uint64_t gpuRingFullSkipped_{0U};
    std::uint64_t gpuSamplesCompleted_{0U};
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
    BoundedMetric gpuTimestampPendingFrames_{};
    BoundedMetric gpuWgcDrainAndCopyMicroseconds_{};
    BoundedMetric gpuBackgroundSnapshotMicroseconds_{};
    BoundedMetric gpuFxMaterialsMicroseconds_{};
    BoundedMetric gpuBloomAndFinalCompositeMicroseconds_{};
    BoundedMetric gpuTotalFxMicroseconds_{};
    BoundedMetric gpuRenderCommandSpanMicroseconds_{};
};

}
