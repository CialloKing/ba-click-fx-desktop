#include "performance_logging.hpp"

#include "bafx/windows/runtime_diagnostics.hpp"

#include <array>
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

void appendConfigurationFields(
    DiagnosticFields& fields,
    const bafx::config::Config& config,
    const bafx::windows::WindowSize outputSize)
{
    fields.add("Configuration.SchemaVersion", config.schemaVersion);
    fields.add("Effects.Enabled", config.effects.enabled);
    fields.add("Effects.ClickEnabled", config.effects.clickEnabled);
    fields.add("Effects.TrailEnabled", config.effects.trailEnabled);
    fields.addDecimal("Effects.GlobalScale", config.effects.globalScale);
    fields.addDecimal("Effects.TrailLength", config.effects.trailLength);
    fields.addDecimal("Effects.TrailWidth", config.effects.trailWidth);
    fields.addDecimal("Effects.BloomIntensity", config.effects.bloomIntensity);
    fields.add("Effects.BloomQuality", bafx::config::toString(
        config.effects.bloomQuality));
    fields.addDecimal(
        "Effects.BloomDiffusion",
        bafx::config::bloomDiffusionForQuality(config.effects.bloomQuality));
    fields.add("Background.Mode", bafx::config::toString(config.background.mode));
    fields.add("Background.CursorExcluded", config.background.cursorExcluded);
    fields.add("Background.AllowSystemBorder", config.background.allowSystemBorder);
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
            "Timing.InputSemantic",
            "input-to-Present-return-not-photon-latency");
        fields.add(
            "WGC.ProducerSemantic",
            "FrameArrived-callback-rate-proxy");
        fields.add(
            "Diagnostics.PreviousLogWriteCpuUs",
            microseconds(previousLogWriteCpu));

        fields.add("WGC.ActiveFrames", summary.wgcActiveFrames);
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

        const bool warning = summary.overflowMoveDrops > 0U
            || summary.inputDispatchBudgetExhaustions > 0U
            || summary.otherDispatchBudgetExhaustions > 0U
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
