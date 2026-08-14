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
        .presentCallCpuMicroseconds = 2'000U,
        .wgcActive = true,
        .wgcDrainAttempted = true,
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
        .gpuSampleCompleted = true,
        .gpuFxTimingValid = true});
    window.addDispatchToPresentReturn(25'000U);
    window.addFramePacingWake(bafx::desktop::FramePacingWake::FrameReady);
    window.addFramePacingWake(bafx::desktop::FramePacingWake::MessagesPending);
    window.addFramePacingWake(bafx::desktop::FramePacingWake::TimedOut);
    const bafx::config::Config config = bafx::config::defaultConfig();

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
    BAFX_CHECK(text.find("Cpu.FrameTotal.P95=20000\n") != std::string::npos);
    BAFX_CHECK(text.find("WGC.DrainPolicy=active-fx-only\n")
        != std::string::npos);
    BAFX_CHECK(text.find("WGC.DrainAttemptedFrames=1\n")
        != std::string::npos);
    BAFX_CHECK(text.find("WGC.IdleDrainSkippedFrames=0\n")
        != std::string::npos);
    BAFX_CHECK(text.find("GPU.TimestampProfiler.Available=true\n")
        != std::string::npos);
    BAFX_CHECK(text.find("GPU.TimestampProfiler.InitializationHresult=0x00000000\n")
        != std::string::npos);
    BAFX_CHECK(text.find("GPU.SamplesCompleted=1\n") != std::string::npos);
    BAFX_CHECK(text.find("GPU.WgcDrainAndCopy.Available=false\n")
        != std::string::npos);
    BAFX_CHECK(text.find("GPU.FxTotal.P95=3400\n") != std::string::npos);
    BAFX_CHECK(text.find("GPU.RenderCommandSpan.Max=3500\n")
        != std::string::npos);
    BAFX_CHECK(text.find("Input.DispatchToPresentReturn.Max=25000\n")
        != std::string::npos);
    BAFX_CHECK(text.find("FramePacing.FrameReadyWakes=1\n")
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

BAFX_TEST(configuration_log_records_the_reason_and_reproduction_context)
{
    const TemporaryPerformanceLog log;
    bafx::config::Config config = bafx::config::defaultConfig();
    config.input.samplingRateHz = 240U;
    config.effects.bloomIntensity = 0.5F;

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
    BAFX_CHECK(text.find("Effects.BloomIntensity=0.500\n")
        != std::string::npos);
    BAFX_CHECK(text.find("Input.SamplingRateHz=240\n") != std::string::npos);
    BAFX_CHECK(text.find("Output.Width=3840\n") != std::string::npos);
}
