#include "test_support.hpp"

#include "bafx/config/config.hpp"
#include "performance_logging.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{

class TemporaryPerformanceLog final
{
public:
    TemporaryPerformanceLog()
    {
        directory_ = std::filesystem::temp_directory_path()
            / ("bafx-performance-log-"
                + std::to_string(GetCurrentProcessId())
                + "-"
                + std::to_string(GetTickCount64()));
        std::filesystem::create_directories(directory_);
        path_ = directory_ / "support.log";
    }

    ~TemporaryPerformanceLog()
    {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    TemporaryPerformanceLog(const TemporaryPerformanceLog&) = delete;
    TemporaryPerformanceLog& operator=(const TemporaryPerformanceLog&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

    [[nodiscard]] std::string read() const
    {
        std::ifstream input(path_, std::ios::binary);
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    }

private:
    std::filesystem::path directory_{};
    std::filesystem::path path_{};
};

}

BAFX_TEST(performance_log_preserves_metric_and_semantic_fields)
{
    const TemporaryPerformanceLog log;
    bafx::desktop::RuntimePerformanceWindow window;
    window.addFrame(bafx::desktop::FramePerformanceSample{
        .frameTotalCpuMicroseconds = 20'000U,
        .fxTotalSubmitCpuMicroseconds = 4'000U,
        .fxMaterialsSubmitCpuMicroseconds = 1'000U,
        .bloomAndCompositeSubmitCpuMicroseconds = 3'000U,
        .prePresentCpuMicroseconds = 1'500U,
        .presentCallCpuMicroseconds = 2'000U,
        .wgcActive = true,
        .wgcDrainAttempted = true,
        .roiVisualBoundsStatus = bafx::fx::FrameBoundsStatus::Ok,
        .roiPlanStatus = bafx::core::RoiStatus::Ok,
        .roiDirtyRectAvailable = true,
        .roiPlanAvailable = true,
        .roiFullScreenPixels = 1920U * 1080U,
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
            bafx::core::ActiveFxRoiStatus::AppliedPrefilter,
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
                40U,
                20U}},
        .gpuFxMaterialsMicroseconds = 900U,
        .gpuBloomAndFinalCompositeMicroseconds = 2'500U,
        .gpuTotalFxMicroseconds = 3'400U,
        .gpuRenderCommandSpanMicroseconds = 3'500U,
        .gpuTimestampInitializationResult = 0U,
        .gpuTimestampPendingFrames = 1U,
        .gpuTimestampProfilerObserved = true,
        .gpuTimestampProfilerAvailable = true,
        .gpuFrameStarted = true,
        .gpuFrameSubmitted = true,
        .gpuAutoSkippedStages = false,
        .gpuSampleCompleted = true,
        .gpuFxTimingValid = true,
        .gpuPrimary = bafx::desktop::GpuFxPathPerformanceSample{
            500U,
            1'000U,
            700U,
            true,
            true,
            true}});
    window.addDispatchToPresentReturn(25'000U);
    window.addFramePacingWake(bafx::desktop::FramePacingWake::FrameReady);
    window.addFramePacingWake(bafx::desktop::FramePacingWake::DeviceRemoved);
    window.addFramePacingWake(bafx::desktop::FramePacingWake::MessagesPending);
    window.addFramePacingWake(bafx::desktop::FramePacingWake::TimedOut);
    window.addCaptureExclusionHealthCheck(true);
    bafx::config::Config config = bafx::config::defaultConfig();
    config.performance.activeFxRoiEnabled = true;

    static_cast<void>(bafx::desktop::appendPerformanceInterval(
        log.path(),
        window.summarize(),
        config,
        bafx::desktop::PerformanceLogContext{
            bafx::windows::WindowSize{1920U, 1080U},
            bafx::windows::BackgroundCompositeStatus::Inactive,
            false},
        std::chrono::seconds(1),
        std::chrono::microseconds(123),
        true));

    const std::string text = log.read();
    BAFX_CHECK(text.find("Event.Name=Performance.Interval\n") != std::string::npos);
    BAFX_CHECK(text.find("Window.Final=true\n") != std::string::npos);
    BAFX_CHECK(text.find("Window.PresentedFps=1.000\n") != std::string::npos);
    BAFX_CHECK(text.find("Effects.BloomQuality=high\n") != std::string::npos);
    BAFX_CHECK(text.find(
        "Timing.PresentSemantic=Present-call-return-not-dwm-composition-or-scanout\n")
        != std::string::npos);
    BAFX_CHECK(text.find(
        "Timing.PrePresentSemantic=fx-render-return-to-Present-call-entry-including-roi-diagnostics-spout-gpu-query-end-and-readback\n")
        != std::string::npos);
    BAFX_CHECK(text.find(
        "Timing.PresentMode=interval-0-frame-latency-gated\n")
        != std::string::npos);
    BAFX_CHECK(text.find(
        "ROI.ProductionPath=active-fx-pyramid-with-full-screen-fallback\n")
        != std::string::npos);
    BAFX_CHECK(text.find("ROI.FinalCompositePath=full-screen\n")
        != std::string::npos);
    BAFX_CHECK(text.find("ROI.WgcCopyPath=full-screen\n")
        != std::string::npos);
    BAFX_CHECK(text.find("ROI.RequestedFrames=1\n")
        != std::string::npos);
    BAFX_CHECK(text.find("ROI.AppliedPrefilterFrames=1\n")
        != std::string::npos);
    BAFX_CHECK(text.find("ROI.Active.LastStatus=prefilter-roi\n")
        != std::string::npos);
    BAFX_CHECK(text.find(
        "ROI.Active.Reason.prefilter-roi.Frames=1\n")
        != std::string::npos);
    BAFX_CHECK(text.find(
        "ROI.Active.Reason.pyramid-roi.Frames=0\n")
        != std::string::npos);
    BAFX_CHECK(text.find("ROI.PrefilterPixels.Max=12500\n")
        != std::string::npos);
    BAFX_CHECK(text.find("ROI.VisualBounds.OkFrames=1\n")
        != std::string::npos);
    BAFX_CHECK(text.find("ROI.Plan.LastStatus=ok\n")
        != std::string::npos);
    BAFX_CHECK(text.find("WGC.CaptureExclusion.HealthChecks=1\n")
        != std::string::npos);
    BAFX_CHECK(text.find("WGC.CaptureExclusion.HealthFailures=0\n")
        != std::string::npos);
    BAFX_CHECK(text.find(
        "WGC.CaptureExclusion.HealthPolicy=one-hz-query-stop-then-fx-only\n")
        != std::string::npos);
    BAFX_CHECK(text.find("ROI.AlignedWorkPixels.Max=50000\n")
        != std::string::npos);
    BAFX_CHECK(text.find("ROI.LastDirtyRect.Left=10\n")
        != std::string::npos);
    BAFX_CHECK(text.find("ROI.LastAlignedWork.Right=128\n")
        != std::string::npos);
    BAFX_CHECK(text.find("ROI.Primary.ObservedFrames=1\n")
        != std::string::npos);
    BAFX_CHECK(text.find("ROI.Primary.AppliedFrames=1\n")
        != std::string::npos);
    BAFX_CHECK(text.find("ROI.Primary.FallbackFrames=0\n")
        != std::string::npos);
    BAFX_CHECK(text.find("ROI.Primary.CandidatePixels.Total=40\n")
        != std::string::npos);
    BAFX_CHECK(text.find("ROI.Primary.DrawnPixels.Total=40\n")
        != std::string::npos);
    BAFX_CHECK(text.find(
        "ROI.Primary.Stage.Prefilter.FullPixels.Total=100\n")
        != std::string::npos);
    BAFX_CHECK(text.find(
        "ROI.Primary.Stage.Prefilter.CandidatePixels.Total=40\n")
        != std::string::npos);
    BAFX_CHECK(text.find(
        "ROI.Primary.Stage.Prefilter.DrawnPixels.Total=40\n")
        != std::string::npos);
    BAFX_CHECK(text.find(
        "ROI.Primary.Stage.Prefilter.ClearedPixels.Total=20\n")
        != std::string::npos);
    BAFX_CHECK(text.find(
        "ROI.Primary.Stage.Resolve.FullPixels.Total=0\n")
        != std::string::npos);
    BAFX_CHECK(text.find("ROI.Primary.DrawnToFullRatio=0.400\n")
        != std::string::npos);
    BAFX_CHECK(text.find(
        "ROI.Primary.PixelCoverageSemantic=drawn-and-cleared-command-coverage-not-gpu-time-savings\n")
        != std::string::npos);
    BAFX_CHECK(text.find("ROI.Primary.Last.ActualPath=roi-prefilter\n")
        != std::string::npos);
    BAFX_CHECK(text.find("ROI.Primary.Reason.applied.Frames=1\n")
        != std::string::npos);
    BAFX_CHECK(text.find("Cpu.FrameTotal.P95=20000\n") != std::string::npos);
    BAFX_CHECK(text.find(
        "WGC.DrainPolicy=visible-every-frame-idle-sensor-only-max-20hz\n")
        != std::string::npos);
    BAFX_CHECK(text.find("WGC.MaintenanceCycles=0\n")
        != std::string::npos);
    BAFX_CHECK(text.find("WGC.DrainAttemptedFrames=1\n")
        != std::string::npos);
    BAFX_CHECK(text.find("WGC.VisibleDrainAttemptedFrames=1\n")
        != std::string::npos);
    BAFX_CHECK(text.find("WGC.IdleDrainAttemptedFrames=0\n")
        != std::string::npos);
    BAFX_CHECK(text.find("WGC.IdleDrainThrottledFrames=0\n")
        != std::string::npos);
    BAFX_CHECK(text.find("GPU.TimestampProfiler.Available=true\n")
        != std::string::npos);
    BAFX_CHECK(text.find("GPU.TimestampProfiler.InitializationHresult=0x00000000\n")
        != std::string::npos);
    BAFX_CHECK(text.find("GPU.SamplesCompleted=1\n") != std::string::npos);
    BAFX_CHECK(text.find("GPU.AutoSkippedStageFrames=0\n")
        != std::string::npos);
    BAFX_CHECK(text.find("GPU.WgcDrainAndCopy.Available=false\n")
        != std::string::npos);
    BAFX_CHECK(text.find("GPU.FxTotal.P95=3400\n") != std::string::npos);
    BAFX_CHECK(text.find("GPU.Primary.Prefilter.P95=500\n")
        != std::string::npos);
    BAFX_CHECK(text.find("GPU.Primary.Pyramid.P95=1000\n")
        != std::string::npos);
    BAFX_CHECK(text.find("GPU.Primary.FinalComposite.P95=700\n")
        != std::string::npos);
    BAFX_CHECK(text.find("GPU.RecordingRebuild.Prefilter.Available=false\n")
        != std::string::npos);
    BAFX_CHECK(text.find("GPU.RenderCommandSpan.Max=3500\n")
        != std::string::npos);
    BAFX_CHECK(text.find("Cpu.PrePresent.Max=1500\n")
        != std::string::npos);
    BAFX_CHECK(text.find("Cpu.PrePresent.Samples=1\n")
        != std::string::npos);
    BAFX_CHECK(text.find("Cpu.PrePresent.DroppedSamples=0\n")
        != std::string::npos);
    BAFX_CHECK(text.find("Input.DispatchToPresentReturn.Max=25000\n")
        != std::string::npos);
    BAFX_CHECK(text.find("FramePacing.FrameReadyWakes=1\n")
        != std::string::npos);
    BAFX_CHECK(text.find("FramePacing.DeviceRemovedWakes=1\n")
        != std::string::npos);
    BAFX_CHECK(text.find("FramePacing.MessageWakes=1\n")
        != std::string::npos);
    BAFX_CHECK(text.find("FramePacing.Timeouts=1\n")
        != std::string::npos);
    BAFX_CHECK(text.find("FramePacing.Failures=0\n")
        != std::string::npos);
    BAFX_CHECK(text.find("Diagnostics.PreviousLogWriteCpuUs=123\n")
        != std::string::npos);
}

BAFX_TEST(performance_log_distinguishes_idle_wgc_attempts_from_throttling)
{
    const TemporaryPerformanceLog log;
    bafx::desktop::RuntimePerformanceWindow window;
    window.addBackgroundMaintenance(bafx::desktop::FramePerformanceSample{
        .wgcActive = true,
        .wgcDrainAttempted = true,
        .wgcIdleDrainAttempted = true});
    window.addBackgroundMaintenance(bafx::desktop::FramePerformanceSample{
        .wgcActive = true,
        .wgcIdleDrainSkipped = true});

    static_cast<void>(bafx::desktop::appendPerformanceInterval(
        log.path(),
        window.summarize(),
        bafx::config::defaultConfig(),
        bafx::desktop::PerformanceLogContext{
            bafx::windows::WindowSize{1920U, 1080U},
            bafx::windows::BackgroundCompositeStatus::WaitingForFrame,
            false},
        std::chrono::seconds(1),
        std::chrono::microseconds(0),
        false));

    const std::string text = log.read();
    BAFX_CHECK(text.find("WGC.ActiveFrames=0\n") != std::string::npos);
    BAFX_CHECK(text.find("WGC.MaintenanceCycles=2\n")
        != std::string::npos);
    BAFX_CHECK(text.find("WGC.DrainAttemptedFrames=1\n")
        != std::string::npos);
    BAFX_CHECK(text.find("WGC.VisibleDrainAttemptedFrames=0\n")
        != std::string::npos);
    BAFX_CHECK(text.find("WGC.IdleDrainAttemptedFrames=1\n")
        != std::string::npos);
    BAFX_CHECK(text.find("WGC.IdleDrainThrottledFrames=1\n")
        != std::string::npos);
}

BAFX_TEST(performance_log_warns_when_device_removed_wake_is_observed)
{
    const TemporaryPerformanceLog log;
    bafx::desktop::RuntimePerformanceWindow window;
    window.addFramePacingWake(bafx::desktop::FramePacingWake::DeviceRemoved);

    static_cast<void>(bafx::desktop::appendPerformanceInterval(
        log.path(),
        window.summarize(),
        bafx::config::defaultConfig(),
        bafx::desktop::PerformanceLogContext{
            bafx::windows::WindowSize{1920U, 1080U},
            bafx::windows::BackgroundCompositeStatus::Inactive,
            false},
        std::chrono::seconds(1),
        std::chrono::microseconds(0),
        false));

    const std::string text = log.read();
    BAFX_CHECK(text.find("Event.Name=Performance.Interval\n")
        != std::string::npos);
    BAFX_CHECK(text.find("Event.Level=Warning\n") != std::string::npos);
    BAFX_CHECK(text.find("FramePacing.DeviceRemovedWakes=1\n")
        != std::string::npos);
}

BAFX_TEST(performance_log_warns_when_capture_exclusion_health_fails)
{
    const TemporaryPerformanceLog log;
    bafx::desktop::RuntimePerformanceWindow window;
    window.addCaptureExclusionHealthCheck(false);

    static_cast<void>(bafx::desktop::appendPerformanceInterval(
        log.path(),
        window.summarize(),
        bafx::config::defaultConfig(),
        bafx::desktop::PerformanceLogContext{
            bafx::windows::WindowSize{1920U, 1080U},
            bafx::windows::BackgroundCompositeStatus::Inactive,
            false},
        std::chrono::seconds(1),
        std::chrono::microseconds(0),
        false));

    const std::string text = log.read();
    BAFX_CHECK(text.find("Event.Name=Performance.Interval\n")
        != std::string::npos);
    BAFX_CHECK(text.find("Event.Level=Warning\n") != std::string::npos);
    BAFX_CHECK(text.find("WGC.CaptureExclusion.HealthChecks=1\n")
        != std::string::npos);
    BAFX_CHECK(text.find("WGC.CaptureExclusion.HealthFailures=1\n")
        != std::string::npos);
}

BAFX_TEST(performance_log_warns_when_gpu_stage_tail_is_auto_skipped)
{
    const TemporaryPerformanceLog log;
    bafx::desktop::RuntimePerformanceWindow window;
    window.addFrame(bafx::desktop::FramePerformanceSample{
        .gpuAutoSkippedStages = true});

    static_cast<void>(bafx::desktop::appendPerformanceInterval(
        log.path(),
        window.summarize(),
        bafx::config::defaultConfig(),
        bafx::desktop::PerformanceLogContext{
            bafx::windows::WindowSize{1920U, 1080U},
            bafx::windows::BackgroundCompositeStatus::Inactive,
            false},
        std::chrono::seconds(1),
        std::chrono::microseconds(0),
        false));

    const std::string text = log.read();
    BAFX_CHECK(text.find("Event.Level=Warning\n") != std::string::npos);
    BAFX_CHECK(text.find("GPU.AutoSkippedStageFrames=1\n")
        != std::string::npos);
}

BAFX_TEST(configuration_log_records_the_reason_and_reproduction_context)
{
    const TemporaryPerformanceLog log;
    bafx::config::Config config = bafx::config::defaultConfig();
    config.input.samplingRateHz = 240U;
    config.effects.diskLifetimeMs = 350.0F;
    config.effects.ringsCount = 4U;
    config.effects.ringsLifetimeMs = 900.0F;
    config.effects.ringsRadiusMin = 45.0F;
    config.effects.ringsRadiusMax = 95.0F;
    config.effects.ringsAngularVelocityMultiplier = 14.5F;
    config.effects.ringsRotationDirection = 0.5F;
    config.effects.bloomIntensity = 0.5F;
    config.display.hdrEnabled = true;

    bafx::desktop::appendAppliedConfiguration(
        log.path(),
        config,
        bafx::windows::WindowSize{3840U, 2160U},
        "test-change");

    const std::string text = log.read();
    BAFX_CHECK(text.find("Event.Name=Configuration.Applied\n")
        != std::string::npos);
    BAFX_CHECK(text.find("Configuration.Reason=test-change\n")
        != std::string::npos);
    BAFX_CHECK(text.find("Effects.DiskLifetimeMs=350.000\n")
        != std::string::npos);
    BAFX_CHECK(text.find("Effects.RingsCount=4\n") != std::string::npos);
    BAFX_CHECK(text.find("Effects.RingsLifetimeMs=900.000\n")
        != std::string::npos);
    BAFX_CHECK(text.find("Effects.RingsRadiusMin=45.000\n")
        != std::string::npos);
    BAFX_CHECK(text.find("Effects.RingsRadiusMax=95.000\n")
        != std::string::npos);
    BAFX_CHECK(text.find(
        "Effects.RingsAngularVelocityMultiplier=14.500\n")
        != std::string::npos);
    BAFX_CHECK(text.find("Effects.RingsRotationDirection=0.500\n")
        != std::string::npos);
    BAFX_CHECK(text.find("Effects.BloomIntensity=0.500\n")
        != std::string::npos);
    BAFX_CHECK(text.find("Display.HdrEnabled=true\n") != std::string::npos);
    BAFX_CHECK(text.find("Input.SamplingRateHz=240\n") != std::string::npos);
    BAFX_CHECK(text.find("Output.Width=3840\n") != std::string::npos);
}
