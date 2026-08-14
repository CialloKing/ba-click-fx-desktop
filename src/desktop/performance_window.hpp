#pragma once

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
    bool wgcOwnedCopySubmitted{false};
    bool wgcAccepted{false};
    bool backgroundSnapshotRefreshAttempted{false};
    bool backgroundSnapshotRefreshed{false};
    bool backgroundParticipated{false};
    bool backgroundSampleAgeValid{false};
    bool diagnosticReadbackUsed{false};
};

struct RuntimePerformanceSummary
{
    std::uint64_t frameCount{0U};
    std::uint64_t wgcActiveFrames{0U};
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
    void addDispatchToPresentReturn(std::uint64_t microseconds) noexcept;
    void addMessageToPresentReturn(std::uint64_t milliseconds) noexcept;
    void reset() noexcept;

    [[nodiscard]] RuntimePerformanceSummary summarize() const;
    [[nodiscard]] bool empty() const noexcept;

private:
    std::uint64_t frameCount_{0U};
    std::uint64_t wgcActiveFrames_{0U};
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
};

}
