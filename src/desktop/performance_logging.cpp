#include "performance_logging.hpp"

#include "bafx/windows/runtime_diagnostics.hpp"

#include <array>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace bafx::desktop
{
namespace
{

class DiagnosticFields final
{
public:
    void add(const std::string_view key, const std::string_view value)
    {
        addOwned(std::string(key), std::string(value));
    }

    void add(const std::string_view key, const char* const value)
    {
        add(key, std::string_view(value != nullptr ? value : "<null>"));
    }

    void add(const std::string_view key, const std::uint64_t value)
    {
        add(key, std::to_string(value));
    }

    void add(const std::string_view key, const std::uint32_t value)
    {
        add(key, static_cast<std::uint64_t>(value));
    }

    void add(const std::string_view key, const std::int32_t value)
    {
        addOwned(std::string(key), std::to_string(value));
    }

    void add(const std::string_view key, const bool value)
    {
        add(key, std::string_view(value ? "true" : "false"));
    }

    void addDecimal(const std::string_view key, const double value)
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::fixed << std::setprecision(3) << value;
        add(key, stream.str());
    }

    void addHex32(const std::string_view key, const std::uint32_t value)
    {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << "0x"
               << std::uppercase
               << std::hex
               << std::setw(8)
               << std::setfill('0')
               << value;
        add(key, stream.str());
    }

    void append(
        const std::filesystem::path& path,
        const std::string_view eventName,
        const bafx::windows::DiagnosticLevel level) const
    {
        std::vector<bafx::windows::DiagnosticField> views;
        views.reserve(fields_.size());
        for (const auto& [key, value] : fields_)
        {
            views.push_back(bafx::windows::DiagnosticField{key, value});
        }
        bafx::windows::appendDiagnosticEvent(path, eventName, views, level);
    }

private:
    void addOwned(std::string key, std::string value)
    {
        fields_.emplace_back(std::move(key), std::move(value));
    }

    std::vector<std::pair<std::string, std::string>> fields_{};
};

[[nodiscard]] std::string_view backgroundStatusName(
    const bafx::windows::BackgroundCompositeStatus status) noexcept
{
    using bafx::windows::BackgroundCompositeStatus;
    switch (status)
    {
    case BackgroundCompositeStatus::Inactive:
        return "inactive";
    case BackgroundCompositeStatus::WaitingForFrame:
        return "waiting-for-frame";
    case BackgroundCompositeStatus::SizeMismatch:
        return "size-mismatch";
    case BackgroundCompositeStatus::Stale:
        return "stale";
    case BackgroundCompositeStatus::FutureTimestamp:
        return "future-timestamp";
    case BackgroundCompositeStatus::WrongEpoch:
        return "wrong-epoch";
    case BackgroundCompositeStatus::InvalidContract:
        return "invalid-contract";
    case BackgroundCompositeStatus::InvalidPolicy:
        return "invalid-policy";
    case BackgroundCompositeStatus::CaptureFailed:
        return "capture-failed";
    case BackgroundCompositeStatus::LatchedFxOnly:
        return "latched-fx-only";
    case BackgroundCompositeStatus::Participating:
        return "participating";
    }
    return "unknown";
}

[[nodiscard]] std::string_view frameBoundsStatusName(
    const bafx::fx::FrameBoundsStatus status) noexcept
{
    using bafx::fx::FrameBoundsStatus;
    switch (status)
    {
    case FrameBoundsStatus::Ok:
        return "ok";
    case FrameBoundsStatus::Empty:
        return "empty";
    case FrameBoundsStatus::Invalid:
        return "invalid";
    case FrameBoundsStatus::IntegerOverflow:
        return "integer-overflow";
    }
    return "unknown";
}

[[nodiscard]] std::string_view roiStatusName(
    const bafx::core::RoiStatus status) noexcept
{
    using bafx::core::RoiStatus;
    switch (status)
    {
    case RoiStatus::Ok:
        return "ok";
    case RoiStatus::Empty:
        return "empty";
    case RoiStatus::InvalidRect:
        return "invalid-rect";
    case RoiStatus::InvalidFootprint:
        return "invalid-footprint";
    case RoiStatus::IntegerOverflow:
        return "integer-overflow";
    }
    return "unknown";
}

void appendMetric(
    DiagnosticFields& fields,
    const std::string_view prefix,
    const MetricSummary& metric,
    const std::string_view unit)
{
    const std::string base(prefix);
    fields.add(base + ".Available", metric.sampleCount > 0U);
    fields.add(base + ".Unit", unit);
    fields.add(base + ".Samples", metric.sampleCount);
    fields.add(base + ".RecordedSamples", metric.recordedSampleCount);
    fields.add(base + ".DroppedSamples", metric.droppedSampleCount);
    if (metric.sampleCount == 0U)
    {
        return;
    }
    fields.add(base + ".Min", metric.minimum);
    fields.addDecimal(base + ".Average", metric.average);
    fields.add(base + ".P50", metric.p50);
    fields.add(base + ".P95", metric.p95);
    fields.add(base + ".P99", metric.p99);
    fields.add(base + ".Max", metric.maximum);
}

void appendRect(
    DiagnosticFields& fields,
    const std::string_view prefix,
    const bool available,
    const bafx::core::RectI rect)
{
    fields.add(std::string(prefix) + ".Available", available);
    if (!available)
    {
        return;
    }
    const std::string base(prefix);
    fields.add(base + ".Left", rect.left);
    fields.add(base + ".Top", rect.top);
    fields.add(base + ".Right", rect.right);
    fields.add(base + ".Bottom", rect.bottom);
}

void appendActiveFxRoiPath(
    DiagnosticFields& fields,
    const std::string_view prefix,
    const ActiveFxRoiPathPerformanceSummary& summary)
{
    const auto appendStage = [&fields](
        const std::string& stageBase,
        const ActiveFxRoiStagePixelDiagnostics& stage)
    {
        fields.add(stageBase + ".FullPixels.Total", stage.fullPixels);
        fields.add(
            stageBase + ".CandidatePixels.Total",
            stage.candidatePixels);
        fields.add(stageBase + ".DrawnPixels.Total", stage.drawnPixels);
        fields.add(stageBase + ".ClearedPixels.Total", stage.clearedPixels);
    };
    const std::string base(prefix);
    fields.add(base + ".ObservedFrames", summary.observedFrames);
    fields.add(base + ".RequestedFrames", summary.requestedFrames);
    fields.add(base + ".EligibleFrames", summary.eligibleFrames);
    fields.add(base + ".ExecutedFrames", summary.executedFrames);
    fields.add(base + ".AppliedFrames", summary.appliedFrames);
    fields.add(base + ".WarmupFrames", summary.warmupFrames);
    fields.add(base + ".FallbackFrames", summary.fallbackFrames);
    fields.add(base + ".FullPixels.Total", summary.fullPixelsTotal);
    fields.add(base + ".CandidatePixels.Total", summary.candidatePixelsTotal);
    fields.add(base + ".DrawnPixels.Total", summary.drawnPixelsTotal);
    fields.add(base + ".ClearedPixels.Total", summary.clearedPixelsTotal);
    appendStage(base + ".Stage.Prefilter", summary.stages.prefilter);
    appendStage(base + ".Stage.Downsample", summary.stages.downsample);
    appendStage(base + ".Stage.Upsample", summary.stages.upsample);
    appendStage(base + ".Stage.Resolve", summary.stages.resolve);
    fields.add(
        base + ".PixelCoverageSemantic",
        "drawn-and-cleared-command-coverage-not-gpu-time-savings");
    fields.addDecimal(
        base + ".DrawnToFullRatio",
        summary.fullPixelsTotal > 0U
            ? static_cast<double>(summary.drawnPixelsTotal)
                / static_cast<double>(summary.fullPixelsTotal)
            : 0.0);
    fields.addDecimal(
        base + ".ClearedToFullRatio",
        summary.fullPixelsTotal > 0U
            ? static_cast<double>(summary.clearedPixelsTotal)
                / static_cast<double>(summary.fullPixelsTotal)
            : 0.0);
    fields.add(base + ".Last.Executed", summary.lastExecuted);
    fields.add(
        base + ".Last.ActualPath",
        activeFxRoiActualPathName(summary.lastActualPath));
    fields.add(
        base + ".Last.DecisionReason",
        activeFxRoiDecisionReasonName(summary.lastDecisionReason));

    constexpr std::array reasons{
        ActiveFxRoiDecisionReason::Disabled,
        ActiveFxRoiDecisionReason::NoContent,
        ActiveFxRoiDecisionReason::BackgroundDifferentialBloom,
        ActiveFxRoiDecisionReason::Context1Unavailable,
        ActiveFxRoiDecisionReason::SharedTargetFullWrite,
        ActiveFxRoiDecisionReason::AreaTooLarge,
        ActiveFxRoiDecisionReason::BenefitTooSmall,
        ActiveFxRoiDecisionReason::Applied,
        ActiveFxRoiDecisionReason::RendererFallback};
    for (const ActiveFxRoiDecisionReason reason : reasons)
    {
        const std::size_t index = static_cast<std::size_t>(reason);
        fields.add(
            base + ".Reason."
                + std::string(
                    activeFxRoiDecisionReasonName(reason))
                + ".Frames",
            summary.decisionReasonFrames[index]);
    }
}

void appendGpuFxPath(
    DiagnosticFields& fields,
    const std::string_view prefix,
    const GpuFxPathPerformanceSummary& summary)
{
    appendMetric(
        fields,
        std::string(prefix) + ".Prefilter",
        summary.prefilterMicroseconds,
        "us");
    appendMetric(
        fields,
        std::string(prefix) + ".Pyramid",
        summary.pyramidMicroseconds,
        "us");
    appendMetric(
        fields,
        std::string(prefix) + ".FinalComposite",
        summary.finalCompositeMicroseconds,
        "us");
}

void appendConfigurationFields(
    DiagnosticFields& fields,
    const bafx::config::Config& config,
    const bafx::windows::WindowSize outputSize)
{
    fields.add("Configuration.SchemaVersion", config.schemaVersion);
    fields.add("Effects.Enabled", config.effects.enabled);
    fields.add("Effects.ClickEnabled", config.effects.clickEnabled);
    fields.add("Effects.TrailEnabled", config.effects.trailEnabled);
    fields.add("Effects.DiskLayerEnabled", config.effects.diskLayerEnabled);
    fields.add("Effects.RingsLayerEnabled", config.effects.ringsLayerEnabled);
    fields.add(
        "Effects.ClickShardsLayerEnabled",
        config.effects.clickShardsLayerEnabled);
    fields.add(
        "Effects.TrailShardsLayerEnabled",
        config.effects.trailShardsLayerEnabled);
    fields.add("Effects.TrailLayerEnabled", config.effects.trailLayerEnabled);
    fields.add("Effects.BloomLayerEnabled", config.effects.bloomLayerEnabled);
    fields.addDecimal("Effects.GlobalScale", config.effects.globalScale);
    fields.addDecimal("Effects.Opacity", config.effects.opacity);
    fields.addDecimal("Effects.TrailLength", config.effects.trailLength);
    fields.addDecimal("Effects.TrailWidth", config.effects.trailWidth);
    fields.addDecimal("Effects.ClickTimeScale", config.effects.clickTimeScale);
    fields.addDecimal("Effects.TrailTimeScale", config.effects.trailTimeScale);
    fields.addDecimal("Effects.TrailLifetimeMs", config.effects.trailLifetimeMs);
    fields.addDecimal("Effects.DiskLifetimeMs", config.effects.diskLifetimeMs);
    fields.addDecimal("Effects.DiskRadius", config.effects.diskRadius);
    fields.add("Effects.RingsCount", config.effects.ringsCount);
    fields.addDecimal("Effects.RingsLifetimeMs", config.effects.ringsLifetimeMs);
    fields.addDecimal("Effects.RingsRadiusMin", config.effects.ringsRadiusMin);
    fields.addDecimal("Effects.RingsRadiusMax", config.effects.ringsRadiusMax);
    fields.addDecimal(
        "Effects.RingsAngularVelocityMultiplier",
        config.effects.ringsAngularVelocityMultiplier);
    fields.addDecimal(
        "Effects.RingsRotationDirection",
        config.effects.ringsRotationDirection);
    fields.addDecimal(
        "Effects.RingsHdrIntensity",
        config.effects.ringsHdrIntensity);
    fields.addDecimal(
        "Effects.ShardsHdrIntensity",
        config.effects.shardsHdrIntensity);
    fields.add("Effects.ShardsClickCount", config.effects.shardsClickCount);
    fields.addDecimal(
        "Effects.ShardsClickLifetimeMinMs",
        config.effects.shardsClickLifetimeMinMs);
    fields.addDecimal(
        "Effects.ShardsClickLifetimeMaxMs",
        config.effects.shardsClickLifetimeMaxMs);
    fields.addDecimal(
        "Effects.ShardsClickRadius",
        config.effects.shardsClickRadius);
    fields.addDecimal(
        "Effects.ShardsClickSpeedMin",
        config.effects.shardsClickSpeedMin);
    fields.addDecimal(
        "Effects.ShardsClickSpeedMax",
        config.effects.shardsClickSpeedMax);
    fields.addDecimal("Effects.ShardsSizeMin", config.effects.shardsSizeMin);
    fields.addDecimal("Effects.ShardsSizeMax", config.effects.shardsSizeMax);
    fields.addDecimal("Effects.TrailOpacity", config.effects.trailOpacity);
    fields.addDecimal("Effects.BloomIntensity", config.effects.bloomIntensity);
    fields.add("Effects.BloomQuality", bafx::config::toString(
        bafx::config::bloomQualityForDiffusion(
            config.effects.bloomDiffusion)));
    fields.addDecimal(
        "Effects.BloomDiffusion",
        config.effects.bloomDiffusion);
    fields.addDecimal("Effects.BloomThreshold", config.effects.bloomThreshold);
    fields.addDecimal("Effects.BloomSoftKnee", config.effects.bloomSoftKnee);
    fields.addDecimal("Effects.BloomClamp", config.effects.bloomClamp);
    fields.add("Background.Mode", bafx::config::toString(config.background.mode));
    fields.add("Background.CursorExcluded", config.background.cursorExcluded);
    fields.add("Background.AllowSystemBorder", config.background.allowSystemBorder);
    fields.add("Display.HdrEnabled", config.display.hdrEnabled);
    fields.add("Input.LeftClick", config.input.leftClick);
    fields.add("Input.RightClick", config.input.rightClick);
    fields.add("Input.MiddleClick", config.input.middleClick);
    fields.add("Input.SamplingRateHz", config.input.samplingRateHz);
    fields.add(
        "Input.TrailOnlyWhilePressed",
        config.input.trailOnlyWhilePressed);
    fields.add(
        "Performance.IdleOptimization",
        config.performance.idleOptimization);
    fields.add(
        "Performance.ActiveFxRoiEnabled",
        config.performance.activeFxRoiEnabled);
    fields.add(
        "Performance.FramePacing",
        bafx::config::toString(config.performance.framePacing));
    fields.add("Output.Width", outputSize.width);
    fields.add("Output.Height", outputSize.height);
}

[[nodiscard]] std::uint64_t microseconds(
    const std::chrono::nanoseconds duration) noexcept
{
    if (duration <= std::chrono::nanoseconds::zero())
    {
        return 0U;
    }
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
}

}

void appendAppliedConfiguration(
    const std::filesystem::path& logPath,
    const bafx::config::Config& config,
    const bafx::windows::WindowSize outputSize,
    const std::string_view reason) noexcept
{
    try
    {
        DiagnosticFields fields;
        fields.add("Configuration.Reason", reason);
        appendConfigurationFields(fields, config, outputSize);
        fields.append(
            logPath,
            "Configuration.Applied",
            bafx::windows::DiagnosticLevel::Info);
    }
    catch (...)
    {
        // Configuration diagnostics must not block applying the configuration.
    }
}

std::chrono::nanoseconds appendPerformanceInterval(
    const std::filesystem::path& logPath,
    const RuntimePerformanceSummary& summary,
    const bafx::config::Config& config,
    const PerformanceLogContext& context,
    const std::chrono::nanoseconds intervalDuration,
    const std::chrono::nanoseconds previousLogWriteCpu,
    const bool finalInterval) noexcept
{
    const auto startedAt = std::chrono::steady_clock::now();
    try
    {
        DiagnosticFields fields;
        appendConfigurationFields(fields, config, context.outputSize);
        const std::uint64_t durationUs = microseconds(intervalDuration);
        const std::uint64_t visibleDrainAttemptedFrames =
            summary.wgcDrainAttemptedFrames
                    >= summary.wgcIdleDrainAttemptedFrames
                ? summary.wgcDrainAttemptedFrames
                    - summary.wgcIdleDrainAttemptedFrames
                : 0U;
        fields.add("Window.Final", finalInterval);
        fields.add("Window.DurationUs", durationUs);
        fields.add("Window.FrameCount", summary.frameCount);
        fields.addDecimal(
            "Window.PresentedFps",
            durationUs > 0U
                ? static_cast<double>(summary.frameCount) * 1'000'000.0
                    / static_cast<double>(durationUs)
                : 0.0);
        fields.add("Runtime.Paused", context.paused);
        fields.add(
            "Background.CompositeStatus",
            backgroundStatusName(context.backgroundStatus));
        fields.add(
            "Timing.CpuSemantic",
            "inclusive-api-call-duration-not-gpu-execution");
        fields.add(
            "Timing.PresentSemantic",
            "Present-call-return-not-dwm-composition-or-scanout");
        fields.add(
            "Timing.PrePresentSemantic",
            "fx-render-return-to-Present-call-entry-including-roi-diagnostics-spout-gpu-query-end-and-readback");
        fields.add(
            "Timing.PresentMode",
            "interval-0-frame-latency-gated");
        fields.add(
            "Timing.InputSemantic",
            "input-to-Present-return-not-photon-latency");
        fields.add(
            "Timing.GpuSemantic",
            "asynchronous-D3D11-timestamp-command-execution-not-Present-DWM-or-scanout");
        fields.add(
            "GPU.SampleCompletionSemantic",
            "completed-sample-may-belong-to-an-older-reporting-window");
        fields.add(
            "GPU.StageApplicabilitySemantic",
            "original-frame-usage-with-primary-and-recording-stage-validity");
        fields.add(
            "ROI.ProductionPath",
            config.performance.activeFxRoiEnabled
                ? "active-fx-pyramid-with-full-screen-fallback"
                : "disabled-full-screen");
        fields.add("ROI.FinalCompositePath", "full-screen");
        fields.add("ROI.WgcCopyPath", "full-screen");
        fields.add("ROI.RequestedFrames", summary.roiRequestedFrames);
        fields.add(
            "ROI.AppliedPrefilterFrames",
            summary.roiAppliedFrames);
        fields.add(
            "ROI.Active.LastStatus",
            bafx::core::activeFxRoiStatusName(
                summary.roiLastActiveStatus));
        constexpr std::array activeStatuses{
            bafx::core::ActiveFxRoiStatus::Disabled,
            bafx::core::ActiveFxRoiStatus::AppliedPrefilter,
            bafx::core::ActiveFxRoiStatus::AppliedPyramid,
            bafx::core::ActiveFxRoiStatus::NoVisualPlan,
            bafx::core::ActiveFxRoiStatus::BloomDisabled,
            bafx::core::ActiveFxRoiStatus::CoreMode,
            bafx::core::ActiveFxRoiStatus::BackgroundDifferentialBloom,
            bafx::core::ActiveFxRoiStatus::TouchesBoundary,
            bafx::core::ActiveFxRoiStatus::AreaTooLarge,
            bafx::core::ActiveFxRoiStatus::BenefitTooSmall,
            bafx::core::ActiveFxRoiStatus::Context1Unavailable,
            bafx::core::ActiveFxRoiStatus::SharedTargetFullWrite,
            bafx::core::ActiveFxRoiStatus::RendererFallback};
        for (const bafx::core::ActiveFxRoiStatus status : activeStatuses)
        {
            const std::size_t index = static_cast<std::size_t>(status);
            fields.add(
                "ROI.Active.Reason."
                    + std::string(bafx::core::activeFxRoiStatusName(status))
                    + ".Frames",
                summary.roiActiveStatusFrames[index]);
        }
        fields.add(
            "ROI.VisualBounds.LastStatus",
            frameBoundsStatusName(summary.roiLastVisualBoundsStatus));
        fields.add(
            "ROI.Plan.LastStatus",
            roiStatusName(summary.roiLastPlanStatus));
        fields.add("ROI.VisualBounds.OkFrames", summary.roiVisualBoundsOkFrames);
        fields.add(
            "ROI.VisualBounds.EmptyFrames",
            summary.roiVisualBoundsEmptyFrames);
        fields.add(
            "ROI.VisualBounds.InvalidFrames",
            summary.roiVisualBoundsInvalidFrames);
        fields.add(
            "ROI.VisualBounds.OverflowFrames",
            summary.roiVisualBoundsOverflowFrames);
        fields.add("ROI.DirtyRect.Frames", summary.roiDirtyRectFrames);
        fields.add("ROI.Plan.OkFrames", summary.roiPlanFrames);
        fields.add("ROI.Plan.EmptyFrames", summary.roiPlanEmptyFrames);
        fields.add(
            "ROI.Plan.InvalidRectFrames",
            summary.roiPlanInvalidRectFrames);
        fields.add(
            "ROI.Plan.InvalidFootprintFrames",
            summary.roiPlanInvalidFootprintFrames);
        fields.add("ROI.Plan.OverflowFrames", summary.roiPlanOverflowFrames);
        appendActiveFxRoiPath(fields, "ROI.Primary", summary.roiPrimary);
        appendActiveFxRoiPath(
            fields,
            "ROI.RecordingRebuild",
            summary.roiRecordingRebuild);
        fields.add(
            "WGC.ProducerSemantic",
            "FrameArrived-callback-rate-proxy");
        fields.add(
            "Diagnostics.PreviousLogWriteCpuUs",
            microseconds(previousLogWriteCpu));

        fields.add("WGC.ActiveFrames", summary.wgcActiveFrames);
        fields.add("WGC.MaintenanceCycles", summary.wgcMaintenanceCycles);
        fields.add(
            "WGC.DrainPolicy",
            "visible-every-frame-idle-sensor-only-max-20hz");
        fields.add(
            "WGC.DrainAttemptedFrames",
            summary.wgcDrainAttemptedFrames);
        fields.add(
            "WGC.VisibleDrainAttemptedFrames",
            visibleDrainAttemptedFrames);
        fields.add(
            "WGC.IdleDrainAttemptedFrames",
            summary.wgcIdleDrainAttemptedFrames);
        fields.add(
            "WGC.IdleDrainThrottledFrames",
            summary.wgcIdleDrainSkippedFrames);
        fields.add("WGC.ProducerCallbacks", summary.wgcProducerCallbacks);
        fields.addDecimal(
            "WGC.ProducerCallbackFps",
            durationUs > 0U
                ? static_cast<double>(summary.wgcProducerCallbacks) * 1'000'000.0
                    / static_cast<double>(durationUs)
                : 0.0);
        fields.add("WGC.FramesAcquired", summary.wgcFramesAcquired);
        fields.add("WGC.FramesSuperseded", summary.wgcFramesSuperseded);
        fields.add(
            "WGC.TimestampRejectedFrames",
            summary.wgcTimestampRejectedFrames);
        fields.add(
            "WGC.OwnedCopiesSubmitted",
            summary.wgcOwnedCopiesSubmitted);
        fields.add("WGC.SamplesAccepted", summary.wgcSamplesAccepted);
        fields.addDecimal(
            "WGC.AcceptedFps",
            durationUs > 0U
                ? static_cast<double>(summary.wgcSamplesAccepted) * 1'000'000.0
                    / static_cast<double>(durationUs)
                : 0.0);
        fields.add(
            "Background.SnapshotAttempts",
            summary.backgroundSnapshotAttempts);
        fields.add(
            "Background.SnapshotsRefreshed",
            summary.backgroundSnapshotsRefreshed);
        fields.add(
            "Background.ParticipatingFrames",
            summary.backgroundParticipatingFrames);
        fields.add(
            "WGC.CaptureExclusion.HealthChecks",
            summary.captureExclusionHealthChecks);
        fields.add(
            "WGC.CaptureExclusion.HealthFailures",
            summary.captureExclusionHealthFailures);
        fields.add(
            "WGC.CaptureExclusion.HealthPolicy",
            "one-hz-query-stop-then-fx-only");

        fields.add("Input.RawMessages", summary.rawInputMessages);
        fields.add("Input.MoveEvents", summary.moveEvents);
        fields.add("Input.ButtonEdges", summary.buttonEdges);
        fields.add("Input.CancelEvents", summary.cancelEvents);
        fields.add("Input.CompactedMoveEvents", summary.compactedMoveEvents);
        fields.add("Input.OverflowMoveDrops", summary.overflowMoveDrops);
        fields.add(
            "Input.MessageTimeUnavailable",
            summary.messageTimeUnavailable);
        fields.add(
            "MessagePump.InputDispatched",
            summary.inputMessagesDispatched);
        fields.add(
            "MessagePump.OtherDispatched",
            summary.otherMessagesDispatched);
        fields.add(
            "MessagePump.InputBudgetExhaustions",
            summary.inputDispatchBudgetExhaustions);
        fields.add(
            "MessagePump.OtherBudgetExhaustions",
            summary.otherDispatchBudgetExhaustions);
        fields.add(
            "FramePacing.FrameReadyWakes",
            summary.framePacingFrameReadyWakes);
        fields.add(
            "FramePacing.DeviceRemovedWakes",
            summary.framePacingDeviceRemovedWakes);
        fields.add(
            "FramePacing.CadenceWakes",
            summary.framePacingCadenceWakes);
        fields.add(
            "FramePacing.MessageWakes",
            summary.framePacingMessageWakes);
        fields.add(
            "FramePacing.Timeouts",
            summary.framePacingTimeouts);
        fields.add(
            "FramePacing.Failures",
            summary.framePacingFailures);

        fields.add(
            "GPU.TimestampProfiler.Observed",
            summary.gpuTimestampProfilerObserved);
        if (summary.gpuTimestampProfilerObserved)
        {
            fields.add(
                "GPU.TimestampProfiler.Available",
                summary.gpuTimestampProfilerAvailable);
            fields.addHex32(
                "GPU.TimestampProfiler.InitializationHresult",
                summary.gpuTimestampInitializationResult);
        }
        fields.add("GPU.FramesStarted", summary.gpuFramesStarted);
        fields.add("GPU.FramesSubmitted", summary.gpuFramesSubmitted);
        fields.add("GPU.PendingPolls", summary.gpuPendingPolls);
        fields.add("GPU.RingFullSkipped", summary.gpuRingFullSkipped);
        fields.add("GPU.SamplesCompleted", summary.gpuSamplesCompleted);
        fields.add(
            "GPU.AutoSkippedStageFrames",
            summary.gpuAutoSkippedStageFrames);
        fields.add(
            "GPU.CancelledSlotsReclaimed",
            summary.gpuCancelledSlotsReclaimed);
        fields.add("GPU.DisjointSamples", summary.gpuDisjointSamples);
        fields.add("GPU.QueryFailures", summary.gpuQueryFailures);
        fields.add("GPU.StateErrors", summary.gpuStateErrors);

        appendMetric(
            fields,
            "Cpu.FrameTotal",
            summary.frameTotalCpuMicroseconds,
            "us");
        appendMetric(
            fields,
            "Cpu.WgcDrainInclusive",
            summary.wgcDrainCpuMicroseconds,
            "us");
        appendMetric(
            fields,
            "Cpu.WgcOwnedCopySubmit",
            summary.wgcOwnedCopySubmitCpuMicroseconds,
            "us");
        appendMetric(
            fields,
            "Cpu.BackgroundSnapshotSubmit",
            summary.backgroundSnapshotSubmitCpuMicroseconds,
            "us");
        appendMetric(
            fields,
            "Cpu.FxTotalSubmit",
            summary.fxTotalSubmitCpuMicroseconds,
            "us");
        appendMetric(
            fields,
            "Cpu.FxMaterialsSubmit",
            summary.fxMaterialsSubmitCpuMicroseconds,
            "us");
        appendMetric(
            fields,
            "Cpu.BloomAndCompositeSubmit",
            summary.bloomAndCompositeSubmitCpuMicroseconds,
            "us");
        appendMetric(
            fields,
            "Cpu.DiagnosticReadback",
            summary.diagnosticReadbackCpuMicroseconds,
            "us");
        appendMetric(
            fields,
            "Cpu.PrePresent",
            summary.prePresentCpuMicroseconds,
            "us");
        appendMetric(
            fields,
            "Cpu.PresentCall",
            summary.presentCallCpuMicroseconds,
            "us");
        appendMetric(
            fields,
            "WGC.SampleAge",
            summary.backgroundSampleAgeMicroseconds,
            "us");
        appendMetric(
            fields,
            "Input.PendingEvents",
            summary.maximumPendingEvents,
            "events");
        appendMetric(
            fields,
            "Input.Win32QueueAge",
            summary.maximumWin32QueueAgeMilliseconds,
            "ms");
        appendMetric(
            fields,
            "Input.DispatchToPresentReturn",
            summary.dispatchToPresentReturnMicroseconds,
            "us");
        appendMetric(
            fields,
            "Input.MessageToPresentReturn",
            summary.messageToPresentReturnMilliseconds,
            "ms");
        appendMetric(
            fields,
            "ROI.FullScreenPixels",
            summary.roiFullScreenPixels,
            "pixels");
        appendMetric(
            fields,
            "ROI.BloomOutputPixels",
            summary.roiBloomOutputPixels,
            "pixels");
        appendMetric(
            fields,
            "ROI.AlignedWorkPixels",
            summary.roiAlignedWorkPixels,
            "pixels");
        appendMetric(fields, "ROI.GuardX", summary.roiGuardX, "pixels");
        appendMetric(fields, "ROI.GuardY", summary.roiGuardY, "pixels");
        appendMetric(
            fields,
            "ROI.PhasePeriod",
            summary.roiPhasePeriod,
            "pixels");
        appendMetric(
            fields,
            "ROI.PrefilterPixels",
            summary.roiPrefilterPixels,
            "pixels");
        appendRect(
            fields,
            "ROI.LastDirtyRect",
            summary.roiLastDirtyRectAvailable,
            summary.roiLastDirtyRect);
        appendRect(
            fields,
            "ROI.LastBloomOutput",
            summary.roiLastBloomOutputAvailable,
            summary.roiLastBloomOutput);
        appendRect(
            fields,
            "ROI.LastAlignedWork",
            summary.roiLastAlignedWorkAvailable,
            summary.roiLastAlignedWork);
        appendMetric(
            fields,
            "GPU.PendingFrames",
            summary.gpuTimestampPendingFrames,
            "slots");
        appendMetric(
            fields,
            "GPU.WgcDrainAndCopy",
            summary.gpuWgcDrainAndCopyMicroseconds,
            "us");
        appendMetric(
            fields,
            "GPU.BackgroundSnapshot",
            summary.gpuBackgroundSnapshotMicroseconds,
            "us");
        appendMetric(
            fields,
            "GPU.FxMaterials",
            summary.gpuFxMaterialsMicroseconds,
            "us");
        appendMetric(
            fields,
            "GPU.BloomAndFinalComposite",
            summary.gpuBloomAndFinalCompositeMicroseconds,
            "us");
        appendGpuFxPath(fields, "GPU.Primary", summary.gpuPrimary);
        appendGpuFxPath(
            fields,
            "GPU.RecordingRebuild",
            summary.gpuRecordingRebuild);
        appendMetric(
            fields,
            "GPU.FxTotal",
            summary.gpuTotalFxMicroseconds,
            "us");
        appendMetric(
            fields,
            "GPU.RenderCommandSpan",
            summary.gpuRenderCommandSpanMicroseconds,
            "us");

        const bool warning = summary.overflowMoveDrops > 0U
            || summary.inputDispatchBudgetExhaustions > 0U
            || summary.otherDispatchBudgetExhaustions > 0U
            || summary.framePacingTimeouts > 0U
            || summary.framePacingFailures > 0U
            || summary.gpuRingFullSkipped > 0U
            || summary.gpuAutoSkippedStageFrames > 0U
            || summary.gpuQueryFailures > 0U
            || summary.gpuStateErrors > 0U
            || summary.roiVisualBoundsInvalidFrames > 0U
            || summary.roiVisualBoundsOverflowFrames > 0U
            || summary.roiPlanInvalidRectFrames > 0U
            || summary.roiPlanInvalidFootprintFrames > 0U
            || summary.roiPlanOverflowFrames > 0U
            || summary.framePacingDeviceRemovedWakes > 0U
            || summary.captureExclusionHealthFailures > 0U
            || summary.frameTotalCpuMicroseconds.maximum >= 100'000U
            || summary.presentCallCpuMicroseconds.maximum >= 50'000U;
        fields.append(
            logPath,
            "Performance.Interval",
            warning
                ? bafx::windows::DiagnosticLevel::Warning
                : bafx::windows::DiagnosticLevel::Info);
    }
    catch (...)
    {
        // Aggregation failures must never enter the interactive render path.
    }
    return std::chrono::steady_clock::now() - startedAt;
}

}
