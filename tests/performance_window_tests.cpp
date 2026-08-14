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
        10'000U,
        2'000U,
        100U,
        300U,
        5'000U,
        2'000U,
        3'000U,
        0U,
        1'500U,
        20'000U,
        6U,
        2U,
        1U,
        0U,
        true,
        true,
        true,
        true,
        true,
        true,
        true,
        false});
    window.addDispatchToPresentReturn(12'000U);
    window.addMessageToPresentReturn(47U);

    const bafx::desktop::RuntimePerformanceSummary summary = window.summarize();
    BAFX_CHECK(summary.frameCount == 1U);
    BAFX_CHECK(summary.wgcProducerCallbacks == 6U);
    BAFX_CHECK(summary.wgcFramesAcquired == 2U);
    BAFX_CHECK(summary.wgcFramesSuperseded == 1U);
    BAFX_CHECK(summary.wgcSamplesAccepted == 1U);
    BAFX_CHECK(summary.backgroundSnapshotsRefreshed == 1U);
    BAFX_CHECK(summary.rawInputMessages == 7U);
    BAFX_CHECK(summary.compactedMoveEvents == 4U);
    BAFX_CHECK(summary.inputDispatchBudgetExhaustions == 1U);
    BAFX_CHECK(summary.frameTotalCpuMicroseconds.p95 == 10'000U);
    BAFX_CHECK(summary.presentCallCpuMicroseconds.maximum == 1'500U);
    BAFX_CHECK(summary.maximumWin32QueueAgeMilliseconds.maximum == 35U);
    BAFX_CHECK(
        summary.dispatchToPresentReturnMicroseconds.maximum == 12'000U);
    BAFX_CHECK(summary.messageToPresentReturnMilliseconds.maximum == 47U);
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
