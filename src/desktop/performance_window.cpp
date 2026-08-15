#include "performance_window.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace bafx::desktop
{
namespace
{

[[nodiscard]] std::uint64_t percentile(
    const std::vector<std::uint64_t>& sorted,
    const double fraction) noexcept
{
    if (sorted.empty())
    {
        return 0U;
    }

    const double rank = std::ceil(
        fraction * static_cast<double>(sorted.size()));
    const std::size_t index = static_cast<std::size_t>(
        std::max(1.0, rank) - 1.0);
    return sorted[std::min(index, sorted.size() - 1U)];
}

}

std::uint64_t WgcCallbackDeltaTracker::observe(
    const bool active,
    const std::uint64_t epoch,
    const std::uint64_t callbacksTotal) noexcept
{
    if (!active)
    {
        reset();
        return 0U;
    }

    std::uint64_t delta = callbacksTotal;
    if (active_
        && epoch == epoch_
        && callbacksTotal >= callbacksTotal_)
    {
        delta = callbacksTotal - callbacksTotal_;
    }
    epoch_ = epoch;
    callbacksTotal_ = callbacksTotal;
    active_ = true;
    return delta;
}

void WgcCallbackDeltaTracker::reset() noexcept
{
    epoch_ = 0U;
    callbacksTotal_ = 0U;
    active_ = false;
}

BoundedMetric::BoundedMetric(const std::size_t capacity)
    : capacity_(capacity)
{
    if (capacity_ == 0U)
    {
        throw std::invalid_argument("Metric sample capacity must be positive");
    }
    samples_.reserve(capacity_);
}

void BoundedMetric::add(const std::uint64_t value) noexcept
{
    if (sampleCount_ == 0U)
    {
        minimum_ = value;
        maximum_ = value;
    }
    else
    {
        minimum_ = std::min(minimum_, value);
        maximum_ = std::max(maximum_, value);
    }

    ++sampleCount_;
    total_ += static_cast<long double>(value);
    if (samples_.size() < capacity_)
    {
        samples_.push_back(value);
    }
}

void BoundedMetric::reset() noexcept
{
    samples_.clear();
    sampleCount_ = 0U;
    minimum_ = 0U;
    maximum_ = 0U;
    total_ = 0.0L;
}

MetricSummary BoundedMetric::summarize() const
{
    if (sampleCount_ == 0U)
    {
        return {};
    }

    std::vector<std::uint64_t> sorted(samples_);
    std::sort(sorted.begin(), sorted.end());
    return MetricSummary{
        sampleCount_,
        static_cast<std::uint64_t>(sorted.size()),
        sampleCount_ - static_cast<std::uint64_t>(sorted.size()),
        minimum_,
        percentile(sorted, 0.50),
        percentile(sorted, 0.95),
        percentile(sorted, 0.99),
        maximum_,
        static_cast<double>(total_ / static_cast<long double>(sampleCount_))};
}

bool BoundedMetric::empty() const noexcept
{
    return sampleCount_ == 0U;
}

void RuntimePerformanceWindow::addInput(
    const InputPerformanceSample& sample) noexcept
{
    rawInputMessages_ += sample.rawInputMessages;
    moveEvents_ += sample.moveEvents;
    buttonEdges_ += sample.buttonEdges;
    cancelEvents_ += sample.cancelEvents;
    compactedMoveEvents_ += sample.compactedMoveEvents;
    overflowMoveDrops_ += sample.overflowMoveDrops;
    messageTimeUnavailable_ += sample.messageTimeUnavailable;
    inputMessagesDispatched_ += sample.inputMessagesDispatched;
    otherMessagesDispatched_ += sample.otherMessagesDispatched;
    inputDispatchBudgetExhaustions_ += sample.inputDispatchBudgetExhausted ? 1U : 0U;
    otherDispatchBudgetExhaustions_ += sample.otherDispatchBudgetExhausted ? 1U : 0U;
    if (sample.maximumPendingEvents > 0U)
    {
        maximumPendingEvents_.add(sample.maximumPendingEvents);
    }
    if (sample.rawInputMessages > sample.messageTimeUnavailable)
    {
        maximumWin32QueueAgeMilliseconds_.add(
            sample.maximumWin32QueueAgeMilliseconds);
    }
}

void RuntimePerformanceWindow::addFrame(
    const FramePerformanceSample& sample) noexcept
{
    ++frameCount_;
    roiLastVisualBoundsStatus_ = sample.roiVisualBoundsStatus;
    roiLastPlanStatus_ = sample.roiPlanStatus;
    switch (sample.roiVisualBoundsStatus)
    {
    case bafx::fx::FrameBoundsStatus::Ok:
        ++roiVisualBoundsOkFrames_;
        break;
    case bafx::fx::FrameBoundsStatus::Empty:
        ++roiVisualBoundsEmptyFrames_;
        break;
    case bafx::fx::FrameBoundsStatus::Invalid:
        ++roiVisualBoundsInvalidFrames_;
        break;
    case bafx::fx::FrameBoundsStatus::IntegerOverflow:
        ++roiVisualBoundsOverflowFrames_;
        break;
    }
    if (sample.roiDirtyRectAvailable)
    {
        ++roiDirtyRectFrames_;
        roiLastDirtyRectAvailable_ = true;
        roiLastDirtyRect_ = sample.roiDirtyRect;
    }
    switch (sample.roiPlanStatus)
    {
    case bafx::core::RoiStatus::Ok:
        ++roiPlanFrames_;
        break;
    case bafx::core::RoiStatus::Empty:
        ++roiPlanEmptyFrames_;
        break;
    case bafx::core::RoiStatus::InvalidRect:
        ++roiPlanInvalidRectFrames_;
        break;
    case bafx::core::RoiStatus::InvalidFootprint:
        ++roiPlanInvalidFootprintFrames_;
        break;
    case bafx::core::RoiStatus::IntegerOverflow:
        ++roiPlanOverflowFrames_;
        break;
    }
    if (sample.roiFullScreenPixels > 0U)
    {
        roiFullScreenPixels_.add(sample.roiFullScreenPixels);
    }
    if (sample.roiPlanAvailable)
    {
        roiLastBloomOutputAvailable_ = true;
        roiLastBloomOutput_ = sample.roiBloomOutput;
        roiLastAlignedWorkAvailable_ = true;
        roiLastAlignedWork_ = sample.roiAlignedWork;
        roiBloomOutputPixels_.add(sample.roiBloomOutputPixels);
        roiAlignedWorkPixels_.add(sample.roiAlignedWorkPixels);
        roiGuardX_.add(sample.roiGuardX);
        roiGuardY_.add(sample.roiGuardY);
        roiPhasePeriod_.add(sample.roiPhasePeriod);
    }
    wgcActiveFrames_ += sample.wgcActive ? 1U : 0U;
    addWgc(sample);
    backgroundSnapshotAttempts_ +=
        sample.backgroundSnapshotRefreshAttempted ? 1U : 0U;
    backgroundSnapshotsRefreshed_ +=
        sample.backgroundSnapshotRefreshed ? 1U : 0U;
    backgroundParticipatingFrames_ += sample.backgroundParticipated ? 1U : 0U;
    frameTotalCpuMicroseconds_.add(sample.frameTotalCpuMicroseconds);
    fxTotalSubmitCpuMicroseconds_.add(sample.fxTotalSubmitCpuMicroseconds);
    fxMaterialsSubmitCpuMicroseconds_.add(
        sample.fxMaterialsSubmitCpuMicroseconds);
    bloomAndCompositeSubmitCpuMicroseconds_.add(
        sample.bloomAndCompositeSubmitCpuMicroseconds);
    presentCallCpuMicroseconds_.add(sample.presentCallCpuMicroseconds);
    if (sample.backgroundSnapshotRefreshAttempted)
    {
        backgroundSnapshotSubmitCpuMicroseconds_.add(
            sample.backgroundSnapshotSubmitCpuMicroseconds);
    }
    if (sample.diagnosticReadbackUsed)
    {
        diagnosticReadbackCpuMicroseconds_.add(
            sample.diagnosticReadbackCpuMicroseconds);
    }
    if (sample.backgroundSampleAgeValid)
    {
        backgroundSampleAgeMicroseconds_.add(
            sample.backgroundSampleAgeMicroseconds);
    }
    if (sample.gpuTimestampProfilerObserved)
    {
        gpuTimestampProfilerObserved_ = true;
        gpuTimestampProfilerAvailable_ =
            sample.gpuTimestampProfilerAvailable;
        gpuTimestampInitializationResult_ =
            sample.gpuTimestampInitializationResult;
    }
    gpuFramesStarted_ += sample.gpuFrameStarted ? 1U : 0U;
    gpuFramesSubmitted_ += sample.gpuFrameSubmitted ? 1U : 0U;
    gpuPendingPolls_ += sample.gpuPollPending ? 1U : 0U;
    gpuRingFullSkipped_ += sample.gpuRingFullSkipped ? 1U : 0U;
    gpuSamplesCompleted_ += sample.gpuSampleCompleted ? 1U : 0U;
    gpuCancelledSlotsReclaimed_ +=
        sample.gpuCancelledSlotReclaimed ? 1U : 0U;
    gpuDisjointSamples_ += sample.gpuDisjointSample ? 1U : 0U;
    gpuQueryFailures_ += sample.gpuQueryFailure ? 1U : 0U;
    gpuStateErrors_ += sample.gpuStateError ? 1U : 0U;
    if (sample.gpuTimestampProfilerAvailable)
    {
        gpuTimestampPendingFrames_.add(sample.gpuTimestampPendingFrames);
    }
    if (sample.gpuSampleCompleted)
    {
        gpuRenderCommandSpanMicroseconds_.add(
            sample.gpuRenderCommandSpanMicroseconds);
    }
    if (sample.gpuWgcTimingValid)
    {
        gpuWgcDrainAndCopyMicroseconds_.add(
            sample.gpuWgcDrainAndCopyMicroseconds);
    }
    if (sample.gpuBackgroundSnapshotTimingValid)
    {
        gpuBackgroundSnapshotMicroseconds_.add(
            sample.gpuBackgroundSnapshotMicroseconds);
    }
    if (sample.gpuFxTimingValid)
    {
        gpuFxMaterialsMicroseconds_.add(sample.gpuFxMaterialsMicroseconds);
        gpuBloomAndFinalCompositeMicroseconds_.add(
            sample.gpuBloomAndFinalCompositeMicroseconds);
        gpuTotalFxMicroseconds_.add(sample.gpuTotalFxMicroseconds);
    }
}

void RuntimePerformanceWindow::addBackgroundMaintenance(
    const FramePerformanceSample& sample) noexcept
{
    // Maintenance has no swap-chain submission. Keep frame, ROI, GPU and
    // Present statistics reserved for calls that actually render a frame.
    if (!sample.wgcActive)
    {
        return;
    }
    ++wgcMaintenanceCycles_;
    addWgc(sample);
}

void RuntimePerformanceWindow::addWgc(
    const FramePerformanceSample& sample) noexcept
{
    wgcDrainAttemptedFrames_ += sample.wgcDrainAttempted ? 1U : 0U;
    wgcIdleDrainAttemptedFrames_ +=
        sample.wgcIdleDrainAttempted ? 1U : 0U;
    wgcIdleDrainSkippedFrames_ += sample.wgcIdleDrainSkipped ? 1U : 0U;
    wgcProducerCallbacks_ += sample.wgcProducerCallbacks;
    wgcFramesAcquired_ += sample.wgcFramesAcquired;
    wgcFramesSuperseded_ += sample.wgcFramesSuperseded;
    wgcTimestampRejectedFrames_ += sample.wgcTimestampRejectedFrames;
    wgcOwnedCopiesSubmitted_ += sample.wgcOwnedCopySubmitted ? 1U : 0U;
    wgcSamplesAccepted_ += sample.wgcAccepted ? 1U : 0U;
    if (sample.wgcDrainAttempted)
    {
        wgcDrainCpuMicroseconds_.add(sample.wgcDrainCpuMicroseconds);
    }
    if (sample.wgcOwnedCopySubmitted)
    {
        wgcOwnedCopySubmitCpuMicroseconds_.add(
            sample.wgcOwnedCopySubmitCpuMicroseconds);
    }
}

void RuntimePerformanceWindow::addFramePacingWake(
    const FramePacingWake wake) noexcept
{
    switch (wake)
    {
    case FramePacingWake::FrameReady:
        ++framePacingFrameReadyWakes_;
        break;
    case FramePacingWake::DeviceRemoved:
        ++framePacingDeviceRemovedWakes_;
        break;
    case FramePacingWake::MessagesPending:
        ++framePacingMessageWakes_;
        break;
    case FramePacingWake::TimedOut:
        ++framePacingTimeouts_;
        break;
    case FramePacingWake::Failed:
        ++framePacingFailures_;
        break;
    }
}

void RuntimePerformanceWindow::addCaptureExclusionHealthCheck(
    const bool confirmed) noexcept
{
    ++captureExclusionHealthChecks_;
    captureExclusionHealthFailures_ += confirmed ? 0U : 1U;
}

void RuntimePerformanceWindow::addDispatchToPresentReturn(
    const std::uint64_t microseconds) noexcept
{
    dispatchToPresentReturnMicroseconds_.add(microseconds);
}

void RuntimePerformanceWindow::addMessageToPresentReturn(
    const std::uint64_t milliseconds) noexcept
{
    messageToPresentReturnMilliseconds_.add(milliseconds);
}

void RuntimePerformanceWindow::reset() noexcept
{
    frameCount_ = 0U;
    wgcActiveFrames_ = 0U;
    wgcMaintenanceCycles_ = 0U;
    wgcDrainAttemptedFrames_ = 0U;
    wgcIdleDrainAttemptedFrames_ = 0U;
    wgcIdleDrainSkippedFrames_ = 0U;
    wgcProducerCallbacks_ = 0U;
    wgcFramesAcquired_ = 0U;
    wgcFramesSuperseded_ = 0U;
    wgcTimestampRejectedFrames_ = 0U;
    wgcOwnedCopiesSubmitted_ = 0U;
    wgcSamplesAccepted_ = 0U;
    backgroundSnapshotAttempts_ = 0U;
    backgroundSnapshotsRefreshed_ = 0U;
    backgroundParticipatingFrames_ = 0U;
    captureExclusionHealthChecks_ = 0U;
    captureExclusionHealthFailures_ = 0U;
    rawInputMessages_ = 0U;
    moveEvents_ = 0U;
    buttonEdges_ = 0U;
    cancelEvents_ = 0U;
    compactedMoveEvents_ = 0U;
    overflowMoveDrops_ = 0U;
    messageTimeUnavailable_ = 0U;
    inputMessagesDispatched_ = 0U;
    otherMessagesDispatched_ = 0U;
    inputDispatchBudgetExhaustions_ = 0U;
    otherDispatchBudgetExhaustions_ = 0U;
    roiVisualBoundsOkFrames_ = 0U;
    roiVisualBoundsEmptyFrames_ = 0U;
    roiVisualBoundsInvalidFrames_ = 0U;
    roiVisualBoundsOverflowFrames_ = 0U;
    roiDirtyRectFrames_ = 0U;
    roiPlanFrames_ = 0U;
    roiPlanEmptyFrames_ = 0U;
    roiPlanInvalidRectFrames_ = 0U;
    roiPlanInvalidFootprintFrames_ = 0U;
    roiPlanOverflowFrames_ = 0U;
    roiLastVisualBoundsStatus_ = bafx::fx::FrameBoundsStatus::Empty;
    roiLastPlanStatus_ = bafx::core::RoiStatus::Empty;
    framePacingFrameReadyWakes_ = 0U;
    framePacingDeviceRemovedWakes_ = 0U;
    framePacingMessageWakes_ = 0U;
    framePacingTimeouts_ = 0U;
    framePacingFailures_ = 0U;
    gpuFramesStarted_ = 0U;
    gpuFramesSubmitted_ = 0U;
    gpuPendingPolls_ = 0U;
    gpuRingFullSkipped_ = 0U;
    gpuSamplesCompleted_ = 0U;
    gpuCancelledSlotsReclaimed_ = 0U;
    gpuDisjointSamples_ = 0U;
    gpuQueryFailures_ = 0U;
    gpuStateErrors_ = 0U;
    gpuTimestampInitializationResult_ = 0U;
    gpuTimestampProfilerObserved_ = false;
    gpuTimestampProfilerAvailable_ = false;
    frameTotalCpuMicroseconds_.reset();
    wgcDrainCpuMicroseconds_.reset();
    wgcOwnedCopySubmitCpuMicroseconds_.reset();
    backgroundSnapshotSubmitCpuMicroseconds_.reset();
    fxTotalSubmitCpuMicroseconds_.reset();
    fxMaterialsSubmitCpuMicroseconds_.reset();
    bloomAndCompositeSubmitCpuMicroseconds_.reset();
    diagnosticReadbackCpuMicroseconds_.reset();
    presentCallCpuMicroseconds_.reset();
    backgroundSampleAgeMicroseconds_.reset();
    maximumPendingEvents_.reset();
    maximumWin32QueueAgeMilliseconds_.reset();
    dispatchToPresentReturnMicroseconds_.reset();
    messageToPresentReturnMilliseconds_.reset();
    roiFullScreenPixels_.reset();
    roiBloomOutputPixels_.reset();
    roiAlignedWorkPixels_.reset();
    roiGuardX_.reset();
    roiGuardY_.reset();
    roiPhasePeriod_.reset();
    roiLastDirtyRectAvailable_ = false;
    roiLastDirtyRect_ = bafx::core::RectI{};
    roiLastBloomOutputAvailable_ = false;
    roiLastBloomOutput_ = bafx::core::RectI{};
    roiLastAlignedWorkAvailable_ = false;
    roiLastAlignedWork_ = bafx::core::RectI{};
    gpuTimestampPendingFrames_.reset();
    gpuWgcDrainAndCopyMicroseconds_.reset();
    gpuBackgroundSnapshotMicroseconds_.reset();
    gpuFxMaterialsMicroseconds_.reset();
    gpuBloomAndFinalCompositeMicroseconds_.reset();
    gpuTotalFxMicroseconds_.reset();
    gpuRenderCommandSpanMicroseconds_.reset();
}

RuntimePerformanceSummary RuntimePerformanceWindow::summarize() const
{
    RuntimePerformanceSummary summary{};
    summary.frameCount = frameCount_;
    summary.wgcActiveFrames = wgcActiveFrames_;
    summary.wgcMaintenanceCycles = wgcMaintenanceCycles_;
    summary.wgcDrainAttemptedFrames = wgcDrainAttemptedFrames_;
    summary.wgcIdleDrainAttemptedFrames = wgcIdleDrainAttemptedFrames_;
    summary.wgcIdleDrainSkippedFrames = wgcIdleDrainSkippedFrames_;
    summary.wgcProducerCallbacks = wgcProducerCallbacks_;
    summary.wgcFramesAcquired = wgcFramesAcquired_;
    summary.wgcFramesSuperseded = wgcFramesSuperseded_;
    summary.wgcTimestampRejectedFrames = wgcTimestampRejectedFrames_;
    summary.wgcOwnedCopiesSubmitted = wgcOwnedCopiesSubmitted_;
    summary.wgcSamplesAccepted = wgcSamplesAccepted_;
    summary.backgroundSnapshotAttempts = backgroundSnapshotAttempts_;
    summary.backgroundSnapshotsRefreshed = backgroundSnapshotsRefreshed_;
    summary.backgroundParticipatingFrames = backgroundParticipatingFrames_;
    summary.captureExclusionHealthChecks = captureExclusionHealthChecks_;
    summary.captureExclusionHealthFailures = captureExclusionHealthFailures_;
    summary.rawInputMessages = rawInputMessages_;
    summary.moveEvents = moveEvents_;
    summary.buttonEdges = buttonEdges_;
    summary.cancelEvents = cancelEvents_;
    summary.compactedMoveEvents = compactedMoveEvents_;
    summary.overflowMoveDrops = overflowMoveDrops_;
    summary.messageTimeUnavailable = messageTimeUnavailable_;
    summary.inputMessagesDispatched = inputMessagesDispatched_;
    summary.otherMessagesDispatched = otherMessagesDispatched_;
    summary.inputDispatchBudgetExhaustions = inputDispatchBudgetExhaustions_;
    summary.otherDispatchBudgetExhaustions = otherDispatchBudgetExhaustions_;
    summary.roiVisualBoundsOkFrames = roiVisualBoundsOkFrames_;
    summary.roiVisualBoundsEmptyFrames = roiVisualBoundsEmptyFrames_;
    summary.roiVisualBoundsInvalidFrames = roiVisualBoundsInvalidFrames_;
    summary.roiVisualBoundsOverflowFrames = roiVisualBoundsOverflowFrames_;
    summary.roiDirtyRectFrames = roiDirtyRectFrames_;
    summary.roiPlanFrames = roiPlanFrames_;
    summary.roiPlanEmptyFrames = roiPlanEmptyFrames_;
    summary.roiPlanInvalidRectFrames = roiPlanInvalidRectFrames_;
    summary.roiPlanInvalidFootprintFrames = roiPlanInvalidFootprintFrames_;
    summary.roiPlanOverflowFrames = roiPlanOverflowFrames_;
    summary.roiLastVisualBoundsStatus = roiLastVisualBoundsStatus_;
    summary.roiLastPlanStatus = roiLastPlanStatus_;
    summary.framePacingFrameReadyWakes = framePacingFrameReadyWakes_;
    summary.framePacingDeviceRemovedWakes = framePacingDeviceRemovedWakes_;
    summary.framePacingMessageWakes = framePacingMessageWakes_;
    summary.framePacingTimeouts = framePacingTimeouts_;
    summary.framePacingFailures = framePacingFailures_;
    summary.gpuFramesStarted = gpuFramesStarted_;
    summary.gpuFramesSubmitted = gpuFramesSubmitted_;
    summary.gpuPendingPolls = gpuPendingPolls_;
    summary.gpuRingFullSkipped = gpuRingFullSkipped_;
    summary.gpuSamplesCompleted = gpuSamplesCompleted_;
    summary.gpuCancelledSlotsReclaimed = gpuCancelledSlotsReclaimed_;
    summary.gpuDisjointSamples = gpuDisjointSamples_;
    summary.gpuQueryFailures = gpuQueryFailures_;
    summary.gpuStateErrors = gpuStateErrors_;
    summary.gpuTimestampInitializationResult =
        gpuTimestampInitializationResult_;
    summary.gpuTimestampProfilerObserved = gpuTimestampProfilerObserved_;
    summary.gpuTimestampProfilerAvailable = gpuTimestampProfilerAvailable_;
    summary.frameTotalCpuMicroseconds = frameTotalCpuMicroseconds_.summarize();
    summary.wgcDrainCpuMicroseconds = wgcDrainCpuMicroseconds_.summarize();
    summary.wgcOwnedCopySubmitCpuMicroseconds =
        wgcOwnedCopySubmitCpuMicroseconds_.summarize();
    summary.backgroundSnapshotSubmitCpuMicroseconds =
        backgroundSnapshotSubmitCpuMicroseconds_.summarize();
    summary.fxTotalSubmitCpuMicroseconds =
        fxTotalSubmitCpuMicroseconds_.summarize();
    summary.fxMaterialsSubmitCpuMicroseconds =
        fxMaterialsSubmitCpuMicroseconds_.summarize();
    summary.bloomAndCompositeSubmitCpuMicroseconds =
        bloomAndCompositeSubmitCpuMicroseconds_.summarize();
    summary.diagnosticReadbackCpuMicroseconds =
        diagnosticReadbackCpuMicroseconds_.summarize();
    summary.presentCallCpuMicroseconds = presentCallCpuMicroseconds_.summarize();
    summary.backgroundSampleAgeMicroseconds =
        backgroundSampleAgeMicroseconds_.summarize();
    summary.maximumPendingEvents = maximumPendingEvents_.summarize();
    summary.maximumWin32QueueAgeMilliseconds =
        maximumWin32QueueAgeMilliseconds_.summarize();
    summary.dispatchToPresentReturnMicroseconds =
        dispatchToPresentReturnMicroseconds_.summarize();
    summary.messageToPresentReturnMilliseconds =
        messageToPresentReturnMilliseconds_.summarize();
    summary.roiFullScreenPixels = roiFullScreenPixels_.summarize();
    summary.roiBloomOutputPixels = roiBloomOutputPixels_.summarize();
    summary.roiAlignedWorkPixels = roiAlignedWorkPixels_.summarize();
    summary.roiGuardX = roiGuardX_.summarize();
    summary.roiGuardY = roiGuardY_.summarize();
    summary.roiPhasePeriod = roiPhasePeriod_.summarize();
    summary.roiLastDirtyRectAvailable = roiLastDirtyRectAvailable_;
    summary.roiLastDirtyRect = roiLastDirtyRect_;
    summary.roiLastBloomOutputAvailable = roiLastBloomOutputAvailable_;
    summary.roiLastBloomOutput = roiLastBloomOutput_;
    summary.roiLastAlignedWorkAvailable = roiLastAlignedWorkAvailable_;
    summary.roiLastAlignedWork = roiLastAlignedWork_;
    summary.gpuTimestampPendingFrames =
        gpuTimestampPendingFrames_.summarize();
    summary.gpuWgcDrainAndCopyMicroseconds =
        gpuWgcDrainAndCopyMicroseconds_.summarize();
    summary.gpuBackgroundSnapshotMicroseconds =
        gpuBackgroundSnapshotMicroseconds_.summarize();
    summary.gpuFxMaterialsMicroseconds =
        gpuFxMaterialsMicroseconds_.summarize();
    summary.gpuBloomAndFinalCompositeMicroseconds =
        gpuBloomAndFinalCompositeMicroseconds_.summarize();
    summary.gpuTotalFxMicroseconds = gpuTotalFxMicroseconds_.summarize();
    summary.gpuRenderCommandSpanMicroseconds =
        gpuRenderCommandSpanMicroseconds_.summarize();
    return summary;
}

bool RuntimePerformanceWindow::empty() const noexcept
{
    return frameCount_ == 0U
        && wgcMaintenanceCycles_ == 0U
        && captureExclusionHealthChecks_ == 0U
        && rawInputMessages_ == 0U
        && moveEvents_ == 0U
        && buttonEdges_ == 0U
        && cancelEvents_ == 0U
        && inputMessagesDispatched_ == 0U
        && otherMessagesDispatched_ == 0U
        && framePacingFrameReadyWakes_ == 0U
        && framePacingDeviceRemovedWakes_ == 0U
        && framePacingMessageWakes_ == 0U
        && framePacingTimeouts_ == 0U
        && framePacingFailures_ == 0U;
}

}
