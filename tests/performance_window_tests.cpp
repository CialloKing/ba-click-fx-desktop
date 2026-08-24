#include "test_support.hpp"

#include "performance_window.hpp"

#include <cstdint>

BAFX_TEST(performance_metric_reports_exact_nearest_rank_percentiles)
{
    bafx::desktop::BoundedMetric metric;
    for (std::uint64_t value = 1U; value <= 100U; ++value)
    {
        metric.add(value);
    }

    const bafx::desktop::MetricSummary summary = metric.summarize();
    BAFX_CHECK(summary.sampleCount == 100U);
    BAFX_CHECK(summary.recordedSampleCount == 100U);
    BAFX_CHECK(summary.droppedSampleCount == 0U);
    BAFX_CHECK(summary.minimum == 1U);
    BAFX_CHECK(summary.p50 == 50U);
    BAFX_CHECK(summary.p95 == 95U);
    BAFX_CHECK(summary.p99 == 99U);
    BAFX_CHECK(summary.maximum == 100U);
    BAFX_CHECK(summary.average == 50.5);
}

BAFX_TEST(performance_metric_exposes_capacity_loss_without_losing_extrema)
{
    bafx::desktop::BoundedMetric metric(3U);
    metric.add(10U);
    metric.add(20U);
    metric.add(30U);
    metric.add(1000U);

    const bafx::desktop::MetricSummary summary = metric.summarize();
    BAFX_CHECK(summary.sampleCount == 4U);
    BAFX_CHECK(summary.recordedSampleCount == 3U);
    BAFX_CHECK(summary.droppedSampleCount == 1U);
    BAFX_CHECK(summary.minimum == 10U);
    BAFX_CHECK(summary.p50 == 20U);
    BAFX_CHECK(summary.p95 == 30U);
    BAFX_CHECK(summary.maximum == 1000U);
    BAFX_CHECK(summary.average == 265.0);
}

BAFX_TEST(performance_metric_reset_starts_a_fresh_window)
{
    bafx::desktop::BoundedMetric metric;
    metric.add(42U);
    metric.reset();

    BAFX_CHECK(metric.empty());
    const bafx::desktop::MetricSummary empty = metric.summarize();
    BAFX_CHECK(empty.sampleCount == 0U);
    BAFX_CHECK(empty.average == 0.0);

    metric.add(7U);
    const bafx::desktop::MetricSummary fresh = metric.summarize();
    BAFX_CHECK(fresh.sampleCount == 1U);
    BAFX_CHECK(fresh.minimum == 7U);
    BAFX_CHECK(fresh.maximum == 7U);
}

BAFX_TEST(wgc_callback_delta_stays_continuous_across_idle_skip_frames)
{
    bafx::desktop::WgcCallbackDeltaTracker tracker;

    BAFX_CHECK(tracker.observe(true, 7U, 100U) == 100U);
    // The idle frame still reports the transport total without draining.
    BAFX_CHECK(tracker.observe(true, 7U, 110U) == 10U);
    BAFX_CHECK(tracker.observe(true, 7U, 120U) == 10U);

    BAFX_CHECK(tracker.observe(true, 8U, 4U) == 4U);
    // A same-epoch regression is treated as a new baseline, not underflow.
    BAFX_CHECK(tracker.observe(true, 8U, 2U) == 2U);
    BAFX_CHECK(tracker.observe(false, 0U, 0U) == 0U);
    BAFX_CHECK(tracker.observe(true, 8U, 5U) == 5U);
}

BAFX_TEST(runtime_performance_window_aggregates_input_and_render_contracts)
{
    bafx::desktop::RuntimePerformanceWindow window;
    window.addInput(bafx::desktop::InputPerformanceSample{
        7U,
        5U,
        2U,
        0U,
        4U,
        1U,
        0U,
        9U,
        3U,
        12U,
        35U,
        true,
        false});
    window.addFrame(bafx::desktop::FramePerformanceSample{
        .frameTotalCpuMicroseconds = 10'000U,
        .wgcDrainCpuMicroseconds = 2'000U,
        .wgcOwnedCopySubmitCpuMicroseconds = 100U,
        .backgroundSnapshotSubmitCpuMicroseconds = 300U,
        .fxTotalSubmitCpuMicroseconds = 5'000U,
        .fxMaterialsSubmitCpuMicroseconds = 2'000U,
        .bloomAndCompositeSubmitCpuMicroseconds = 3'000U,
        .presentCallCpuMicroseconds = 1'500U,
        .backgroundSampleAgeMicroseconds = 20'000U,
        .wgcProducerCallbacks = 6U,
        .wgcFramesAcquired = 2U,
        .wgcFramesSuperseded = 1U,
        .wgcActive = true,
        .wgcDrainAttempted = true,
        .wgcOwnedCopySubmitted = true,
        .wgcAccepted = true,
        .backgroundSnapshotRefreshAttempted = true,
        .backgroundSnapshotRefreshed = true,
        .backgroundParticipated = true,
        .backgroundSampleAgeValid = true,
        .roiVisualBoundsStatus = bafx::fx::FrameBoundsStatus::Ok,
        .roiPlanStatus = bafx::core::RoiStatus::Ok,
        .roiDirtyRectAvailable = true,
        .roiPlanAvailable = true,
        .roiFullScreenPixels = 1'920U * 1'080U,
        .roiBloomOutputPixels = 40'000U,
        .roiAlignedWorkPixels = 50'000U,
        .roiGuardX = 378U,
        .roiGuardY = 378U,
        .roiPhasePeriod = 64U,
        .roiDirtyRect = bafx::core::RectI{10, 20, 30, 40},
        .roiBloomOutput = bafx::core::RectI{0, 0, 100, 100},
        .roiAlignedWork = bafx::core::RectI{0, 0, 128, 128},
        .roiRequested = true,
        .roiApplied = true,
        .roiPrefilterPixels = 12'500U,
        .roiActiveStatus =
            bafx::core::ActiveFxRoiStatus::AppliedPrefilter});
    window.addDispatchToPresentReturn(12'000U);
    window.addMessageToPresentReturn(47U);
    window.addFramePacingWake(bafx::desktop::FramePacingWake::FrameReady);
    window.addFramePacingWake(bafx::desktop::FramePacingWake::DeviceRemoved);
    window.addFramePacingWake(bafx::desktop::FramePacingWake::MessagesPending);
    window.addFramePacingWake(bafx::desktop::FramePacingWake::TimedOut);
    window.addFramePacingWake(bafx::desktop::FramePacingWake::Failed);
    window.addCaptureExclusionHealthCheck(true);
    window.addCaptureExclusionHealthCheck(false);

    const bafx::desktop::RuntimePerformanceSummary summary = window.summarize();
    BAFX_CHECK(summary.frameCount == 1U);
    BAFX_CHECK(summary.wgcActiveFrames == 1U);
    BAFX_CHECK(summary.wgcMaintenanceCycles == 0U);
    BAFX_CHECK(summary.wgcDrainAttemptedFrames == 1U);
    BAFX_CHECK(summary.wgcIdleDrainAttemptedFrames == 0U);
    BAFX_CHECK(summary.wgcIdleDrainSkippedFrames == 0U);
    BAFX_CHECK(summary.wgcProducerCallbacks == 6U);
    BAFX_CHECK(summary.wgcFramesAcquired == 2U);
    BAFX_CHECK(summary.wgcFramesSuperseded == 1U);
    BAFX_CHECK(summary.wgcSamplesAccepted == 1U);
    BAFX_CHECK(summary.backgroundSnapshotsRefreshed == 1U);
    BAFX_CHECK(summary.rawInputMessages == 7U);
    BAFX_CHECK(summary.compactedMoveEvents == 4U);
    BAFX_CHECK(summary.inputDispatchBudgetExhaustions == 1U);
    BAFX_CHECK(summary.frameTotalCpuMicroseconds.p95 == 10'000U);
    BAFX_CHECK(summary.wgcDrainCpuMicroseconds.p95 == 2'000U);
    BAFX_CHECK(summary.presentCallCpuMicroseconds.maximum == 1'500U);
    BAFX_CHECK(summary.maximumWin32QueueAgeMilliseconds.maximum == 35U);
    BAFX_CHECK(
        summary.dispatchToPresentReturnMicroseconds.maximum == 12'000U);
    BAFX_CHECK(summary.messageToPresentReturnMilliseconds.maximum == 47U);
    BAFX_CHECK(summary.roiVisualBoundsOkFrames == 1U);
    BAFX_CHECK(summary.roiDirtyRectFrames == 1U);
    BAFX_CHECK(summary.roiPlanFrames == 1U);
    BAFX_CHECK(summary.roiRequestedFrames == 1U);
    BAFX_CHECK(summary.roiAppliedFrames == 1U);
    BAFX_CHECK(
        summary.roiActiveStatusFrames[static_cast<std::size_t>(
            bafx::core::ActiveFxRoiStatus::AppliedPrefilter)]
        == 1U);
    BAFX_CHECK(
        summary.roiLastActiveStatus
        == bafx::core::ActiveFxRoiStatus::AppliedPrefilter);
    BAFX_CHECK(summary.roiFullScreenPixels.maximum == 1'920U * 1'080U);
    BAFX_CHECK(summary.roiAlignedWorkPixels.maximum == 50'000U);
    BAFX_CHECK(summary.roiGuardX.maximum == 378U);
    BAFX_CHECK(summary.roiPhasePeriod.maximum == 64U);
    BAFX_CHECK(summary.roiPrefilterPixels.maximum == 12'500U);
    BAFX_CHECK(summary.roiLastDirtyRectAvailable);
    BAFX_CHECK(summary.roiLastDirtyRect.left == 10);
    BAFX_CHECK(summary.roiLastAlignedWork.right == 128);
    BAFX_CHECK(summary.framePacingFrameReadyWakes == 1U);
    BAFX_CHECK(summary.framePacingDeviceRemovedWakes == 1U);
    BAFX_CHECK(summary.framePacingMessageWakes == 1U);
    BAFX_CHECK(summary.framePacingTimeouts == 1U);
    BAFX_CHECK(summary.framePacingFailures == 1U);
    BAFX_CHECK(summary.captureExclusionHealthChecks == 2U);
    BAFX_CHECK(summary.captureExclusionHealthFailures == 1U);

    window.reset();
    const bafx::desktop::RuntimePerformanceSummary reset = window.summarize();
    BAFX_CHECK(reset.framePacingDeviceRemovedWakes == 0U);
    BAFX_CHECK(reset.captureExclusionHealthChecks == 0U);
    BAFX_CHECK(window.empty());
}

BAFX_TEST(runtime_performance_window_records_idle_wgc_drain_attempts)
{
    bafx::desktop::RuntimePerformanceWindow window;
    window.addFrame(bafx::desktop::FramePerformanceSample{
        .frameTotalCpuMicroseconds = 100U,
        .wgcDrainCpuMicroseconds = 25U,
        .wgcActive = true,
        .wgcDrainAttempted = true,
        .wgcIdleDrainAttempted = true});

    const bafx::desktop::RuntimePerformanceSummary summary = window.summarize();
    BAFX_CHECK(summary.wgcDrainAttemptedFrames == 1U);
    BAFX_CHECK(summary.wgcIdleDrainAttemptedFrames == 1U);
    BAFX_CHECK(summary.wgcIdleDrainSkippedFrames == 0U);
    BAFX_CHECK(summary.wgcDrainCpuMicroseconds.sampleCount == 1U);
    BAFX_CHECK(summary.wgcDrainCpuMicroseconds.maximum == 25U);
}

BAFX_TEST(runtime_performance_window_keeps_wgc_maintenance_out_of_frame_metrics)
{
    bafx::desktop::RuntimePerformanceWindow window;
    window.addBackgroundMaintenance(bafx::desktop::FramePerformanceSample{
        .frameTotalCpuMicroseconds = 900U,
        .wgcDrainCpuMicroseconds = 25U,
        .wgcOwnedCopySubmitCpuMicroseconds = 7U,
        .presentCallCpuMicroseconds = 300U,
        .wgcProducerCallbacks = 3U,
        .wgcFramesAcquired = 2U,
        .wgcFramesSuperseded = 1U,
        .wgcActive = true,
        .wgcDrainAttempted = true,
        .wgcIdleDrainAttempted = true,
        .wgcOwnedCopySubmitted = true,
        .wgcAccepted = true,
        .roiVisualBoundsStatus = bafx::fx::FrameBoundsStatus::Ok,
        .roiPlanStatus = bafx::core::RoiStatus::Ok,
        .roiDirtyRectAvailable = true,
        .roiPlanAvailable = true,
        .roiFullScreenPixels = 100U,
        .roiBloomOutputPixels = 50U,
        .roiAlignedWorkPixels = 64U,
        .gpuRenderCommandSpanMicroseconds = 400U,
        .gpuTimestampProfilerObserved = true,
        .gpuFrameStarted = true,
        .gpuFrameSubmitted = true,
        .gpuSampleCompleted = true});

    const bafx::desktop::RuntimePerformanceSummary summary = window.summarize();
    BAFX_CHECK(summary.frameCount == 0U);
    BAFX_CHECK(summary.wgcActiveFrames == 0U);
    BAFX_CHECK(summary.wgcMaintenanceCycles == 1U);
    BAFX_CHECK(summary.wgcDrainAttemptedFrames == 1U);
    BAFX_CHECK(summary.wgcIdleDrainAttemptedFrames == 1U);
    BAFX_CHECK(summary.wgcProducerCallbacks == 3U);
    BAFX_CHECK(summary.wgcFramesAcquired == 2U);
    BAFX_CHECK(summary.wgcFramesSuperseded == 1U);
    BAFX_CHECK(summary.wgcOwnedCopiesSubmitted == 1U);
    BAFX_CHECK(summary.wgcSamplesAccepted == 1U);
    BAFX_CHECK(summary.wgcDrainCpuMicroseconds.sampleCount == 1U);
    BAFX_CHECK(summary.wgcOwnedCopySubmitCpuMicroseconds.sampleCount == 1U);
    BAFX_CHECK(summary.frameTotalCpuMicroseconds.sampleCount == 0U);
    BAFX_CHECK(summary.presentCallCpuMicroseconds.sampleCount == 0U);
    BAFX_CHECK(summary.roiVisualBoundsOkFrames == 0U);
    BAFX_CHECK(summary.roiPlanFrames == 0U);
    BAFX_CHECK(summary.roiFullScreenPixels.sampleCount == 0U);
    BAFX_CHECK(summary.gpuFramesStarted == 0U);
    BAFX_CHECK(summary.gpuFramesSubmitted == 0U);
    BAFX_CHECK(summary.gpuSamplesCompleted == 0U);
    BAFX_CHECK(summary.gpuRenderCommandSpanMicroseconds.sampleCount == 0U);
    BAFX_CHECK(!window.empty());

    window.reset();
    window.addBackgroundMaintenance(bafx::desktop::FramePerformanceSample{});
    BAFX_CHECK(window.empty());
    BAFX_CHECK(window.summarize().wgcMaintenanceCycles == 0U);
}

BAFX_TEST(runtime_performance_window_excludes_idle_wgc_skips_from_drain_timing)
{
    bafx::desktop::RuntimePerformanceWindow window;
    window.addFrame(bafx::desktop::FramePerformanceSample{
        .frameTotalCpuMicroseconds = 100U,
        .wgcDrainCpuMicroseconds = 0U,
        .wgcProducerCallbacks = 10U,
        .wgcActive = true,
        .wgcIdleDrainSkipped = true});

    const bafx::desktop::RuntimePerformanceSummary summary = window.summarize();
    BAFX_CHECK(summary.wgcActiveFrames == 1U);
    BAFX_CHECK(summary.wgcDrainAttemptedFrames == 0U);
    BAFX_CHECK(summary.wgcIdleDrainAttemptedFrames == 0U);
    BAFX_CHECK(summary.wgcIdleDrainSkippedFrames == 1U);
    BAFX_CHECK(
        summary.wgcActiveFrames
        == summary.wgcDrainAttemptedFrames
            + summary.wgcIdleDrainSkippedFrames);
    BAFX_CHECK(summary.wgcProducerCallbacks == 10U);
    BAFX_CHECK(summary.wgcDrainCpuMicroseconds.sampleCount == 0U);

    window.reset();
    const bafx::desktop::RuntimePerformanceSummary reset = window.summarize();
    BAFX_CHECK(reset.wgcMaintenanceCycles == 0U);
    BAFX_CHECK(reset.wgcDrainAttemptedFrames == 0U);
    BAFX_CHECK(reset.wgcIdleDrainAttemptedFrames == 0U);
    BAFX_CHECK(reset.wgcIdleDrainSkippedFrames == 0U);
}

BAFX_TEST(runtime_performance_window_omits_unavailable_optional_timings)
{
    bafx::desktop::RuntimePerformanceWindow window;
    window.addFrame(bafx::desktop::FramePerformanceSample{
        .frameTotalCpuMicroseconds = 100U,
        .fxTotalSubmitCpuMicroseconds = 50U,
        .fxMaterialsSubmitCpuMicroseconds = 20U,
        .bloomAndCompositeSubmitCpuMicroseconds = 30U,
        .presentCallCpuMicroseconds = 10U});

    const bafx::desktop::RuntimePerformanceSummary summary = window.summarize();
    BAFX_CHECK(summary.wgcDrainCpuMicroseconds.sampleCount == 0U);
    BAFX_CHECK(summary.wgcOwnedCopySubmitCpuMicroseconds.sampleCount == 0U);
    BAFX_CHECK(
        summary.backgroundSnapshotSubmitCpuMicroseconds.sampleCount == 0U);
    BAFX_CHECK(summary.backgroundSampleAgeMicroseconds.sampleCount == 0U);
    BAFX_CHECK(summary.diagnosticReadbackCpuMicroseconds.sampleCount == 0U);

    window.reset();
    BAFX_CHECK(window.empty());
}

BAFX_TEST(runtime_performance_window_filters_gpu_stages_by_original_frame_usage)
{
    bafx::desktop::RuntimePerformanceWindow window;
    window.addFrame(bafx::desktop::FramePerformanceSample{
        .gpuWgcDrainAndCopyMicroseconds = 100U,
        .gpuBackgroundSnapshotMicroseconds = 200U,
        .gpuFxMaterialsMicroseconds = 300U,
        .gpuBloomAndFinalCompositeMicroseconds = 400U,
        .gpuTotalFxMicroseconds = 700U,
        .gpuRenderCommandSpanMicroseconds = 1'000U,
        .gpuTimestampInitializationResult = 0U,
        .gpuTimestampPendingFrames = 2U,
        .gpuTimestampProfilerObserved = true,
        .gpuTimestampProfilerAvailable = true,
        .gpuFrameStarted = true,
        .gpuFrameSubmitted = true,
        .gpuAutoSkippedStages = true,
        .gpuPollPending = true,
        .gpuSampleCompleted = true,
        .gpuWgcTimingValid = true,
        .gpuBackgroundSnapshotTimingValid = true,
        .gpuFxTimingValid = true});
    window.addFrame(bafx::desktop::FramePerformanceSample{
        .gpuWgcDrainAndCopyMicroseconds = 1U,
        .gpuBackgroundSnapshotMicroseconds = 2U,
        .gpuFxMaterialsMicroseconds = 3U,
        .gpuBloomAndFinalCompositeMicroseconds = 4U,
        .gpuTotalFxMicroseconds = 7U,
        .gpuRenderCommandSpanMicroseconds = 10U,
        .gpuTimestampInitializationResult = 0U,
        .gpuTimestampPendingFrames = 1U,
        .gpuTimestampProfilerObserved = true,
        .gpuTimestampProfilerAvailable = true,
        .gpuFrameStarted = true,
        .gpuFrameSubmitted = true,
        .gpuSampleCompleted = true});

    const bafx::desktop::RuntimePerformanceSummary summary = window.summarize();
    BAFX_CHECK(summary.gpuTimestampProfilerObserved);
    BAFX_CHECK(summary.gpuTimestampProfilerAvailable);
    BAFX_CHECK(summary.gpuFramesStarted == 2U);
    BAFX_CHECK(summary.gpuFramesSubmitted == 2U);
    BAFX_CHECK(summary.gpuSamplesCompleted == 2U);
    BAFX_CHECK(summary.gpuAutoSkippedStageFrames == 1U);
    BAFX_CHECK(summary.gpuPendingPolls == 1U);
    BAFX_CHECK(summary.gpuTimestampPendingFrames.maximum == 2U);
    BAFX_CHECK(summary.gpuRenderCommandSpanMicroseconds.sampleCount == 2U);
    BAFX_CHECK(summary.gpuWgcDrainAndCopyMicroseconds.sampleCount == 1U);
    BAFX_CHECK(summary.gpuWgcDrainAndCopyMicroseconds.maximum == 100U);
    BAFX_CHECK(summary.gpuBackgroundSnapshotMicroseconds.sampleCount == 1U);
    BAFX_CHECK(summary.gpuFxMaterialsMicroseconds.sampleCount == 1U);
    BAFX_CHECK(summary.gpuBloomAndFinalCompositeMicroseconds.maximum == 400U);
    BAFX_CHECK(summary.gpuTotalFxMicroseconds.maximum == 700U);
}

BAFX_TEST(runtime_performance_window_aggregates_roi_paths_without_claiming_savings)
{
    bafx::desktop::RuntimePerformanceWindow window;
    window.addFrame(bafx::desktop::FramePerformanceSample{
        .roiPrimary = bafx::desktop::ActiveFxRoiPathPerformanceSample{
            true,
            bafx::desktop::ActiveFxRoiPassDiagnostics{
                true,
                true,
                true,
                false,
                bafx::desktop::ActiveFxRoiActualPath::RoiPrefilter,
                bafx::desktop::ActiveFxRoiDecisionReason::Applied,
                100U,
                40U,
                20U}},
        .roiRecordingRebuild =
            bafx::desktop::ActiveFxRoiPathPerformanceSample{
                true,
                bafx::desktop::ActiveFxRoiPassDiagnostics{
                    false,
                    false,
                    false,
                    false,
                    bafx::desktop::ActiveFxRoiActualPath::Idle,
                    bafx::desktop::ActiveFxRoiDecisionReason::NoContent,
                    100U,
                    0U,
                    0U}}});
    window.addFrame(bafx::desktop::FramePerformanceSample{
        .roiPrimary = bafx::desktop::ActiveFxRoiPathPerformanceSample{
            true,
            bafx::desktop::ActiveFxRoiPassDiagnostics{
                true,
                true,
                true,
                true,
                bafx::desktop::ActiveFxRoiActualPath::RoiWarmup,
                bafx::desktop::ActiveFxRoiDecisionReason::Applied,
                100U,
                45U,
                100U}}});

    const bafx::desktop::RuntimePerformanceSummary summary = window.summarize();
    BAFX_CHECK(summary.roiPrimary.observedFrames == 2U);
    BAFX_CHECK(summary.roiPrimary.requestedFrames == 2U);
    BAFX_CHECK(summary.roiPrimary.eligibleFrames == 2U);
    BAFX_CHECK(summary.roiPrimary.executedFrames == 2U);
    BAFX_CHECK(summary.roiPrimary.appliedFrames == 2U);
    BAFX_CHECK(summary.roiPrimary.warmupFrames == 1U);
    BAFX_CHECK(summary.roiPrimary.fallbackFrames == 0U);
    BAFX_CHECK(summary.roiPrimary.fullPixelsTotal == 200U);
    BAFX_CHECK(summary.roiPrimary.candidatePixelsTotal == 0U);
    BAFX_CHECK(summary.roiPrimary.drawnPixelsTotal == 85U);
    BAFX_CHECK(summary.roiPrimary.clearedPixelsTotal == 120U);
    BAFX_CHECK(summary.roiPrimary.stages.prefilter.fullPixels == 200U);
    BAFX_CHECK(summary.roiPrimary.stages.downsample.fullPixels == 0U);
    BAFX_CHECK(
        summary.roiPrimary.lastActualPath
        == bafx::desktop::ActiveFxRoiActualPath::RoiWarmup);
    const std::size_t appliedReason = static_cast<std::size_t>(
        bafx::desktop::ActiveFxRoiDecisionReason::Applied);
    BAFX_CHECK(
        summary.roiPrimary.decisionReasonFrames[appliedReason] == 2U);
    std::uint64_t reasonFrames = 0U;
    for (const std::uint64_t count : summary.roiPrimary.decisionReasonFrames)
    {
        reasonFrames += count;
    }
    BAFX_CHECK(reasonFrames == summary.roiPrimary.observedFrames);
    BAFX_CHECK(summary.roiRecordingRebuild.observedFrames == 1U);
    BAFX_CHECK(summary.roiRecordingRebuild.executedFrames == 0U);

    window.reset();
    BAFX_CHECK(window.summarize().roiPrimary.observedFrames == 0U);
}

BAFX_TEST(runtime_performance_window_aggregates_explicit_roi_pyramid_stages)
{
    bafx::desktop::ActiveFxRoiPassDiagnostics pyramid{};
    pyramid.requested = true;
    pyramid.eligible = true;
    pyramid.executed = true;
    pyramid.actualPath = bafx::desktop::ActiveFxRoiActualPath::RoiPyramid;
    pyramid.decisionReason = bafx::desktop::ActiveFxRoiDecisionReason::Applied;
    pyramid.fullPixels = 999U;
    pyramid.drawnPixels = 999U;
    pyramid.stages.prefilter = {100U, 30U, 30U, 5U};
    pyramid.stages.downsample = {50U, 20U, 20U, 4U};
    pyramid.stages.upsample = {40U, 15U, 15U, 3U};
    pyramid.stages.resolve = {200U, 35U, 200U, 0U};

    bafx::desktop::ActiveFxRoiPassDiagnostics fallback{};
    fallback.requested = true;
    fallback.executed = true;
    fallback.actualPath = bafx::desktop::ActiveFxRoiActualPath::FullScreen;
    fallback.decisionReason =
        bafx::desktop::ActiveFxRoiDecisionReason::RendererFallback;
    fallback.stages.prefilter = {100U, 30U, 100U, 0U};

    bafx::desktop::RuntimePerformanceWindow window;
    window.addFrame(bafx::desktop::FramePerformanceSample{
        .roiPrimary = {true, pyramid}});
    window.addFrame(bafx::desktop::FramePerformanceSample{
        .roiPrimary = {true, fallback}});

    const bafx::desktop::ActiveFxRoiPathPerformanceSummary summary =
        window.summarize().roiPrimary;
    BAFX_CHECK(summary.observedFrames == 2U);
    BAFX_CHECK(summary.appliedFrames == 1U);
    BAFX_CHECK(summary.fallbackFrames == 1U);
    BAFX_CHECK(summary.fullPixelsTotal == 490U);
    BAFX_CHECK(summary.candidatePixelsTotal == 130U);
    BAFX_CHECK(summary.drawnPixelsTotal == 365U);
    BAFX_CHECK(summary.clearedPixelsTotal == 12U);
    BAFX_CHECK(summary.stages.prefilter.fullPixels == 200U);
    BAFX_CHECK(summary.stages.resolve.drawnPixels == 200U);
    BAFX_CHECK(
        summary.lastActualPath
        == bafx::desktop::ActiveFxRoiActualPath::FullScreen);
}

BAFX_TEST(runtime_performance_window_filters_each_gpu_fx_path_stage)
{
    bafx::desktop::RuntimePerformanceWindow window;
    window.addFrame(bafx::desktop::FramePerformanceSample{
        .gpuPrimary = bafx::desktop::GpuFxPathPerformanceSample{
            10U,
            20U,
            30U,
            true,
            true,
            false},
        .gpuRecordingRebuild = bafx::desktop::GpuFxPathPerformanceSample{
            40U,
            50U,
            60U,
            false,
            false,
            true}});
    window.addFrame(bafx::desktop::FramePerformanceSample{
        .gpuPrimary = bafx::desktop::GpuFxPathPerformanceSample{
            1U,
            2U,
            3U,
            false,
            false,
            false}});

    const bafx::desktop::RuntimePerformanceSummary summary = window.summarize();
    BAFX_CHECK(summary.gpuPrimary.prefilterMicroseconds.sampleCount == 1U);
    BAFX_CHECK(summary.gpuPrimary.prefilterMicroseconds.maximum == 10U);
    BAFX_CHECK(summary.gpuPrimary.pyramidMicroseconds.maximum == 20U);
    BAFX_CHECK(summary.gpuPrimary.finalCompositeMicroseconds.sampleCount == 0U);
    BAFX_CHECK(
        summary.gpuRecordingRebuild.prefilterMicroseconds.sampleCount == 0U);
    BAFX_CHECK(
        summary.gpuRecordingRebuild.finalCompositeMicroseconds.maximum == 60U);
}
