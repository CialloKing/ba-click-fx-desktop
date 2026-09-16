#include "performance_samples.hpp"
#include "bafx/windows/composition_renderer.hpp"

namespace bafx::desktop
{
namespace
{

[[nodiscard]] bafx::desktop::ActiveFxRoiActualPath activeFxRoiActualPath(
    const bafx::windows::FxActiveRoiActualPath path) noexcept
{
    switch (path)
    {
    case bafx::windows::FxActiveRoiActualPath::Disabled:
        return bafx::desktop::ActiveFxRoiActualPath::Disabled;
    case bafx::windows::FxActiveRoiActualPath::Idle:
        return bafx::desktop::ActiveFxRoiActualPath::Idle;
    case bafx::windows::FxActiveRoiActualPath::FullScreen:
        return bafx::desktop::ActiveFxRoiActualPath::FullScreen;
    case bafx::windows::FxActiveRoiActualPath::RoiWarmup:
        return bafx::desktop::ActiveFxRoiActualPath::RoiWarmup;
    case bafx::windows::FxActiveRoiActualPath::RoiPrefilter:
        return bafx::desktop::ActiveFxRoiActualPath::RoiPrefilter;
    case bafx::windows::FxActiveRoiActualPath::RoiPyramid:
        return bafx::desktop::ActiveFxRoiActualPath::RoiPyramid;
    case bafx::windows::FxActiveRoiActualPath::Unavailable:
        return bafx::desktop::ActiveFxRoiActualPath::Unavailable;
    }
    return bafx::desktop::ActiveFxRoiActualPath::Unavailable;
}

[[nodiscard]] bafx::desktop::ActiveFxRoiStagePixelDiagnostics
activeFxRoiStageDiagnostics(
    const bafx::windows::FxActiveRoiStageDiagnostics& diagnostics) noexcept
{
    return bafx::desktop::ActiveFxRoiStagePixelDiagnostics{
        diagnostics.fullPixels,
        diagnostics.candidatePixels,
        diagnostics.drawnPixels,
        diagnostics.clearedPixels};
}

[[nodiscard]] bafx::desktop::ActiveFxRoiStagesDiagnostics
activeFxRoiStagesDiagnostics(
    const bafx::windows::FxActiveRoiStagesDiagnostics& diagnostics) noexcept
{
    return bafx::desktop::ActiveFxRoiStagesDiagnostics{
        activeFxRoiStageDiagnostics(diagnostics.prefilter),
        activeFxRoiStageDiagnostics(diagnostics.downsample),
        activeFxRoiStageDiagnostics(diagnostics.upsample),
        activeFxRoiStageDiagnostics(diagnostics.resolve)};
}

[[nodiscard]] bafx::desktop::ActiveFxRoiDecisionReason
activeFxRoiDecisionReason(
    const bafx::windows::FxActiveRoiDecisionReason reason) noexcept
{
    switch (reason)
    {
    case bafx::windows::FxActiveRoiDecisionReason::Disabled:
        return bafx::desktop::ActiveFxRoiDecisionReason::Disabled;
    case bafx::windows::FxActiveRoiDecisionReason::NoContent:
        return bafx::desktop::ActiveFxRoiDecisionReason::NoContent;
    case bafx::windows::FxActiveRoiDecisionReason::
        BackgroundDifferentialBloom:
        return bafx::desktop::ActiveFxRoiDecisionReason::
            BackgroundDifferentialBloom;
    case bafx::windows::FxActiveRoiDecisionReason::Context1Unavailable:
        return bafx::desktop::ActiveFxRoiDecisionReason::Context1Unavailable;
    case bafx::windows::FxActiveRoiDecisionReason::SharedTargetFullWrite:
        return bafx::desktop::ActiveFxRoiDecisionReason::SharedTargetFullWrite;
    case bafx::windows::FxActiveRoiDecisionReason::AreaTooLarge:
        return bafx::desktop::ActiveFxRoiDecisionReason::AreaTooLarge;
    case bafx::windows::FxActiveRoiDecisionReason::BenefitTooSmall:
        return bafx::desktop::ActiveFxRoiDecisionReason::BenefitTooSmall;
    case bafx::windows::FxActiveRoiDecisionReason::Applied:
        return bafx::desktop::ActiveFxRoiDecisionReason::Applied;
    case bafx::windows::FxActiveRoiDecisionReason::RendererFallback:
        return bafx::desktop::ActiveFxRoiDecisionReason::RendererFallback;
    }
    return bafx::desktop::ActiveFxRoiDecisionReason::RendererFallback;
}

[[nodiscard]] bafx::desktop::ActiveFxRoiPathPerformanceSample
activeFxRoiPathPerformanceSample(
    const bafx::windows::FxActiveRoiPassDiagnostics& diagnostics) noexcept
{
    bafx::desktop::ActiveFxRoiPassDiagnostics mapped{};
    mapped.requested = diagnostics.requested;
    mapped.eligible = diagnostics.eligible;
    mapped.executed = diagnostics.executed;
    mapped.warmup = diagnostics.warmup;
    mapped.actualPath = activeFxRoiActualPath(diagnostics.actualPath);
    mapped.decisionReason = activeFxRoiDecisionReason(
        diagnostics.decisionReason);
    mapped.fullPixels = diagnostics.fullPixels;
    mapped.candidatePixels = diagnostics.candidatePixels;
    mapped.drawnPixels = diagnostics.drawnPixels;
    mapped.clearedPixels = diagnostics.clearedPixels;
    mapped.stages = activeFxRoiStagesDiagnostics(diagnostics.stages);
    return bafx::desktop::ActiveFxRoiPathPerformanceSample{true, mapped};
}

[[nodiscard]] bafx::desktop::GpuFxPathPerformanceSample
gpuFxPathPerformanceSample(
    const bafx::windows::GpuTimestampFxPathSample& timings,
    const bafx::windows::GpuTimestampFxPathUsage& usage) noexcept
{
    return bafx::desktop::GpuFxPathPerformanceSample{
        durationMicroseconds(timings.prefilter),
        durationMicroseconds(timings.pyramid),
        durationMicroseconds(timings.finalComposite),
        usage.prefilterExecuted,
        usage.pyramidExecuted,
        usage.finalCompositeExecuted};
}

}

[[nodiscard]] std::uint64_t durationMicroseconds(
    const std::chrono::nanoseconds duration) noexcept
{
    if (duration <= std::chrono::nanoseconds::zero())
    {
        return 0U;
    }
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
}

[[nodiscard]] bafx::desktop::FramePerformanceSample wgcPerformanceSample(
    const bafx::windows::WgcBackgroundDrainDiagnostics& wgc,
    const std::chrono::nanoseconds drainInclusiveCpu,
    const std::uint64_t producerCallbacks,
    const bool active,
    const bool drainAttempted,
    const bool idleDrainAttempted,
    const bool idleDrainSkipped) noexcept
{
    bafx::desktop::FramePerformanceSample sample{};
    sample.wgcDrainCpuMicroseconds = durationMicroseconds(drainInclusiveCpu);
    sample.wgcOwnedCopySubmitCpuMicroseconds =
        durationMicroseconds(wgc.ownedCopySubmitCpu);
    sample.wgcProducerCallbacks = producerCallbacks;
    sample.wgcFramesAcquired = wgc.framesAcquired;
    sample.wgcFramesSuperseded = wgc.framesSuperseded;
    sample.wgcTimestampRejectedFrames = wgc.timestampRejectedFrames;
    sample.wgcActive = active;
    sample.wgcDrainAttempted = drainAttempted;
    sample.wgcIdleDrainAttempted = idleDrainAttempted;
    sample.wgcIdleDrainSkipped = idleDrainSkipped;
    sample.wgcOwnedCopySubmitted = wgc.ownedCopySubmitted;
    sample.wgcAccepted = wgc.accepted;
    return sample;
}

[[nodiscard]] bafx::desktop::FramePerformanceSample framePerformanceSample(
    const bafx::windows::CompositionFrameDiagnostics& frame,
    const std::uint64_t wgcProducerCallbacks,
    const bool diagnosticReadbackUsed) noexcept
{
    bafx::desktop::FramePerformanceSample sample = wgcPerformanceSample(
        frame.wgc,
        frame.wgcDrainInclusiveCpu,
        wgcProducerCallbacks,
        frame.wgcActive,
        frame.wgcDrainAttempted,
        frame.wgcIdleDrainAttempted,
        frame.wgcIdleDrainSkipped);
    sample.frameTotalCpuMicroseconds = durationMicroseconds(frame.frameTotalCpu);
    sample.backgroundSnapshotSubmitCpuMicroseconds =
        durationMicroseconds(frame.backgroundSnapshotSubmitCpu);
    sample.fxTotalSubmitCpuMicroseconds =
        durationMicroseconds(frame.fx.totalSubmit);
    sample.fxMaterialsSubmitCpuMicroseconds =
        durationMicroseconds(frame.fx.materialsSubmit);
    sample.bloomAndCompositeSubmitCpuMicroseconds =
        durationMicroseconds(frame.fx.bloomAndCompositeSubmit);
    sample.diagnosticReadbackCpuMicroseconds =
        durationMicroseconds(frame.diagnosticReadbackCpu);
    sample.prePresentCpuMicroseconds =
        durationMicroseconds(frame.prePresentCpu);
    sample.presentCallCpuMicroseconds =
        durationMicroseconds(frame.presentCallCpu);
    sample.backgroundSampleAgeMicroseconds =
        durationMicroseconds(frame.backgroundSampleAge);
    sample.roiVisualBoundsStatus = frame.roi.visualBoundsStatus;
    sample.roiPlanStatus = frame.roi.planStatus;
    sample.roiDirtyRectAvailable = frame.roi.dirtyRectAvailable;
    sample.roiPlanAvailable = frame.roi.planAvailable;
    sample.roiPresentDirtyRectApplied = frame.roi.presentDirtyRectApplied;
    sample.roiFullScreenPixels = frame.roi.fullScreenPixels;
    sample.roiBloomOutputPixels = frame.roi.bloomOutputPixels;
    sample.roiAlignedWorkPixels = frame.roi.alignedWorkPixels;
    sample.roiPresentDirtyPixels = frame.roi.presentDirtyPixels;
    sample.roiGuardX = frame.roi.guardX;
    sample.roiGuardY = frame.roi.guardY;
    sample.roiPhasePeriod = frame.roi.phasePeriod;
    sample.roiDirtyRect = frame.roi.dirtyRect;
    sample.roiBloomOutput = frame.roi.bloomOutput;
    sample.roiAlignedWork = frame.roi.alignedWork;
    sample.roiRequested = frame.roi.requested;
    sample.roiApplied = frame.roi.prefilterApplied;
    sample.roiPrefilterPixels = frame.roi.prefilterPixels;
    sample.roiActiveStatus = frame.roi.activeStatus;
    sample.roiPrimary = activeFxRoiPathPerformanceSample(frame.roi.primary);
    sample.roiRecordingRebuild = activeFxRoiPathPerformanceSample(
        frame.roi.recordingRebuild);
    sample.backgroundSnapshotRefreshAttempted =
        frame.backgroundSnapshotRefreshAttempted;
    sample.backgroundSnapshotRefreshed = frame.backgroundSnapshotRefreshed;
    sample.backgroundParticipated = frame.backgroundParticipated;
    sample.backgroundSampleAgeValid = frame.backgroundSampleAgeValid;
    sample.diagnosticReadbackUsed = diagnosticReadbackUsed;

    sample.gpuTimestampProfilerObserved = true;
    sample.gpuTimestampProfilerAvailable =
        frame.gpuTimestampProfilerAvailable;
    sample.gpuTimestampInitializationResult = static_cast<std::uint32_t>(
        frame.gpuTimestampInitializationResult);
    sample.gpuTimestampPendingFrames = static_cast<std::uint32_t>(
        frame.gpuTimestampPendingFrames);
    sample.gpuStateError = frame.gpuTimestampCheckpointFailure;
    switch (frame.gpuTimestampBegin)
    {
    case bafx::windows::GpuTimestampBeginStatus::Started:
        sample.gpuFrameStarted = true;
        break;
    case bafx::windows::GpuTimestampBeginStatus::Unavailable:
        break;
    case bafx::windows::GpuTimestampBeginStatus::AlreadyActive:
        sample.gpuStateError = true;
        break;
    case bafx::windows::GpuTimestampBeginStatus::RingFullSkipped:
        sample.gpuRingFullSkipped = true;
        break;
    }
    switch (frame.gpuTimestampEnd)
    {
    case bafx::windows::GpuTimestampEndStatus::Submitted:
        sample.gpuFrameSubmitted = true;
        break;
    case bafx::windows::GpuTimestampEndStatus::SubmittedWithAutoSkippedStages:
        // An auto-skipped tail remains a submitted sample, but it also means
        // the renderer failed to emit the complete v0.2.7 stage contract.
        sample.gpuFrameSubmitted = true;
        sample.gpuAutoSkippedStages = true;
        sample.gpuStateError = true;
        break;
    case bafx::windows::GpuTimestampEndStatus::NoActiveFrame:
        break;
    case bafx::windows::GpuTimestampEndStatus::IncompleteCancelled:
        sample.gpuStateError = true;
        break;
    }
    switch (frame.gpuTimestampPoll.status)
    {
    case bafx::windows::GpuTimestampPollStatus::NoPendingFrame:
    case bafx::windows::GpuTimestampPollStatus::Completed:
    case bafx::windows::GpuTimestampPollStatus::Unavailable:
        break;
    case bafx::windows::GpuTimestampPollStatus::Pending:
        sample.gpuPollPending = true;
        break;
    case bafx::windows::GpuTimestampPollStatus::Cancelled:
        sample.gpuCancelledSlotReclaimed = true;
        break;
    case bafx::windows::GpuTimestampPollStatus::Disjoint:
        sample.gpuDisjointSample = true;
        break;
    case bafx::windows::GpuTimestampPollStatus::QueryFailure:
        sample.gpuQueryFailure = true;
        break;
    case bafx::windows::GpuTimestampPollStatus::ActiveFrame:
    case bafx::windows::GpuTimestampPollStatus::AlreadyPolled:
        sample.gpuStateError = true;
        break;
    }

    if (frame.gpuTimestampPoll.sample.has_value())
    {
        const bafx::windows::GpuTimestampSample& gpu =
            *frame.gpuTimestampPoll.sample;
        sample.gpuSampleCompleted = true;
        sample.gpuWgcDrainAndCopyMicroseconds =
            durationMicroseconds(gpu.wgcDrainAndCopy);
        sample.gpuBackgroundSnapshotMicroseconds =
            durationMicroseconds(gpu.backgroundSnapshot);
        sample.gpuFxMaterialsMicroseconds =
            durationMicroseconds(gpu.fxMaterials);
        sample.gpuBloomAndFinalCompositeMicroseconds =
            durationMicroseconds(gpu.bloomAndFinalComposite);
        sample.gpuTotalFxMicroseconds = durationMicroseconds(gpu.totalFx);
        sample.gpuRenderCommandSpanMicroseconds =
            durationMicroseconds(gpu.totalFrame);
        sample.gpuWgcTimingValid = gpu.usage.wgcDrainAttempted;
        sample.gpuBackgroundSnapshotTimingValid =
            gpu.usage.backgroundSnapshotAttempted;
        sample.gpuFxTimingValid = gpu.usage.visualContent;
        sample.gpuPrimary = gpuFxPathPerformanceSample(
            gpu.primary,
            gpu.usage.primary);
        sample.gpuRecordingRebuild = gpuFxPathPerformanceSample(
            gpu.recordingRebuild,
            gpu.usage.recordingRebuild);
    }
    return sample;
}

}
