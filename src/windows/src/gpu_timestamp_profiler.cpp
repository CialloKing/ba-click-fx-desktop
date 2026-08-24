#include "bafx/windows/gpu_timestamp_profiler.hpp"

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace bafx::windows
{
namespace
{

using Microsoft::WRL::ComPtr;

constexpr std::uint64_t nanosecondsPerSecond = 1'000'000'000U;

class D3d11GpuTimestampQueryBackend final
    : public detail::GpuTimestampQueryBackend
{
public:
    D3d11GpuTimestampQueryBackend(
        ID3D11Device* device,
        ID3D11DeviceContext* context) noexcept
        : context_(context)
    {
        if (device == nullptr || context == nullptr)
        {
            initializationResult_ = E_POINTER;
            return;
        }

        D3D11_QUERY_DESC disjointDescription{};
        disjointDescription.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        D3D11_QUERY_DESC timestampDescription{};
        timestampDescription.Query = D3D11_QUERY_TIMESTAMP;
        for (SlotQueries& slot : slots_)
        {
            initializationResult_ = device->CreateQuery(
                &disjointDescription,
                &slot.disjoint);
            if (FAILED(initializationResult_) || slot.disjoint == nullptr)
            {
                normalizeNullQueryFailure();
                releaseQueries();
                return;
            }
            for (ComPtr<ID3D11Query>& timestamp : slot.timestamps)
            {
                initializationResult_ = device->CreateQuery(
                    &timestampDescription,
                    &timestamp);
                if (FAILED(initializationResult_) || timestamp == nullptr)
                {
                    normalizeNullQueryFailure();
                    releaseQueries();
                    return;
                }
            }
        }
        initializationResult_ = S_OK;
    }

    [[nodiscard]] bool available() const noexcept override
    {
        return initializationResult_ == S_OK;
    }

    [[nodiscard]] HRESULT initializationResult() const noexcept override
    {
        return initializationResult_;
    }

    void beginDisjoint(const std::size_t slot) noexcept override
    {
        context_->Begin(slots_[slot].disjoint.Get());
    }

    void writeTimestamp(
        const std::size_t slot,
        const std::size_t boundary) noexcept override
    {
        context_->End(slots_[slot].timestamps[boundary].Get());
    }

    void endDisjoint(const std::size_t slot) noexcept override
    {
        context_->End(slots_[slot].disjoint.Get());
    }

    [[nodiscard]] detail::GpuTimestampQueryReadStatus readDisjoint(
        const std::size_t slot,
        detail::GpuTimestampDisjointQueryResult& result) noexcept override
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
        const HRESULT readResult = context_->GetData(
            slots_[slot].disjoint.Get(),
            &data,
            sizeof(data),
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (readResult == S_FALSE)
        {
            return detail::GpuTimestampQueryReadStatus::Pending;
        }
        if (FAILED(readResult))
        {
            return detail::GpuTimestampQueryReadStatus::Failure;
        }
        result.frequency = data.Frequency;
        result.disjoint = data.Disjoint != FALSE;
        return detail::GpuTimestampQueryReadStatus::Ready;
    }

    [[nodiscard]] detail::GpuTimestampQueryReadStatus readTimestamp(
        const std::size_t slot,
        const std::size_t boundary,
        std::uint64_t& timestamp) noexcept override
    {
        const HRESULT readResult = context_->GetData(
            slots_[slot].timestamps[boundary].Get(),
            &timestamp,
            sizeof(timestamp),
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (readResult == S_FALSE)
        {
            return detail::GpuTimestampQueryReadStatus::Pending;
        }
        if (FAILED(readResult))
        {
            return detail::GpuTimestampQueryReadStatus::Failure;
        }
        return detail::GpuTimestampQueryReadStatus::Ready;
    }

private:
    struct SlotQueries
    {
        ComPtr<ID3D11Query> disjoint{};
        std::array<
            ComPtr<ID3D11Query>,
            detail::gpuTimestampBoundaryCount> timestamps{};
    };

    void normalizeNullQueryFailure() noexcept
    {
        if (SUCCEEDED(initializationResult_))
        {
            initializationResult_ = E_FAIL;
        }
    }

    void releaseQueries() noexcept
    {
        for (SlotQueries& slot : slots_)
        {
            slot.disjoint.Reset();
            for (ComPtr<ID3D11Query>& timestamp : slot.timestamps)
            {
                timestamp.Reset();
            }
        }
        context_.Reset();
    }

    std::array<SlotQueries, GpuTimestampProfiler::slotCount> slots_{};
    ComPtr<ID3D11DeviceContext> context_{};
    HRESULT initializationResult_{E_FAIL};
};

[[nodiscard]] std::optional<std::chrono::nanoseconds> timestampDelta(
    const std::uint64_t start,
    const std::uint64_t end,
    const std::uint64_t frequency) noexcept
{
    if (frequency == 0U || end < start)
    {
        return std::nullopt;
    }

    const std::uint64_t ticks = end - start;
    const std::uint64_t seconds = ticks / frequency;
    const std::uint64_t remainder = ticks % frequency;
    constexpr auto maximumNanoseconds = static_cast<std::uint64_t>(
        std::numeric_limits<std::chrono::nanoseconds::rep>::max());
    if (seconds > maximumNanoseconds / nanosecondsPerSecond)
    {
        return std::nullopt;
    }
    const std::uint64_t wholeNanoseconds = seconds * nanosecondsPerSecond;
    // remainder is smaller than frequency, so reducing before multiplication
    // avoids overflowing while retaining deterministic integer truncation.
    if (remainder
        > std::numeric_limits<std::uint64_t>::max() / nanosecondsPerSecond)
    {
        return std::nullopt;
    }
    const std::uint64_t fractionalNanoseconds =
        remainder * nanosecondsPerSecond / frequency;
    if (fractionalNanoseconds > maximumNanoseconds - wholeNanoseconds)
    {
        return std::nullopt;
    }
    return std::chrono::nanoseconds(
        static_cast<std::chrono::nanoseconds::rep>(
            wholeNanoseconds + fractionalNanoseconds));
}

}

struct GpuTimestampProfiler::Implementation
{
    enum class SlotState : std::uint8_t
    {
        Free,
        Recording,
        Pending,
        PendingCancellation
    };

    struct Slot
    {
        SlotState state{SlotState::Free};
        std::uint64_t frameId{0U};
        std::uint64_t submissionSequence{0U};
        std::size_t nextBoundary{0U};
        GpuTimestampFrameUsage usage{};
    };

    [[nodiscard]] static bool isOptionalCheckpoint(
        const GpuTimestampCheckpoint checkpoint) noexcept
    {
        return static_cast<std::size_t>(checkpoint)
            >= detail::gpuTimestampRequiredCheckpointCount;
    }

    static void markExecuted(
        GpuTimestampFrameUsage& usage,
        const GpuTimestampCheckpoint checkpoint) noexcept
    {
        switch (checkpoint)
        {
        case GpuTimestampCheckpoint::PrimaryPrefilterComplete:
            usage.primary.prefilterExecuted = true;
            break;
        case GpuTimestampCheckpoint::PrimaryPyramidComplete:
            usage.primary.pyramidExecuted = true;
            break;
        case GpuTimestampCheckpoint::PrimaryFinalCompositeComplete:
            usage.primary.finalCompositeExecuted = true;
            break;
        case GpuTimestampCheckpoint::RecordingRebuildPrefilterComplete:
            usage.recordingRebuild.prefilterExecuted = true;
            break;
        case GpuTimestampCheckpoint::RecordingRebuildPyramidComplete:
            usage.recordingRebuild.pyramidExecuted = true;
            break;
        case GpuTimestampCheckpoint::RecordingRebuildFinalCompositeComplete:
            usage.recordingRebuild.finalCompositeExecuted = true;
            break;
        case GpuTimestampCheckpoint::WgcDrainAndCopyComplete:
        case GpuTimestampCheckpoint::BackgroundSnapshotComplete:
        case GpuTimestampCheckpoint::FxMaterialsComplete:
            break;
        }
    }

    explicit Implementation(
        std::unique_ptr<detail::GpuTimestampQueryBackend> queryBackend)
        : backend(std::move(queryBackend))
    {
        if (backend == nullptr)
        {
            backend = std::make_unique<D3d11GpuTimestampQueryBackend>(
                nullptr,
                nullptr);
        }
        if (!backend->available() && FAILED(backend->initializationResult()))
        {
            counters.queryFailures = 1U;
        }
    }

    [[nodiscard]] std::optional<std::size_t> freeSlot() const noexcept
    {
        for (std::size_t offset = 0U; offset < slots.size(); ++offset)
        {
            const std::size_t index = (nextSlot + offset) % slots.size();
            if (slots[index].state == SlotState::Free)
            {
                return index;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::size_t> oldestPendingSlot() const noexcept
    {
        std::optional<std::size_t> oldest;
        for (std::size_t index = 0U; index < slots.size(); ++index)
        {
            const Slot& slot = slots[index];
            if (slot.state != SlotState::Pending
                && slot.state != SlotState::PendingCancellation)
            {
                continue;
            }
            if (!oldest.has_value()
                || slot.submissionSequence
                    < slots[*oldest].submissionSequence)
            {
                oldest = index;
            }
        }
        return oldest;
    }

    void releaseSlot(const std::size_t index) noexcept
    {
        slots[index] = Slot{};
    }

    [[nodiscard]] GpuTimestampPollResult queryFailure(
        const std::size_t slot) noexcept
    {
        releaseSlot(slot);
        ++counters.queryFailures;
        return GpuTimestampPollResult{GpuTimestampPollStatus::QueryFailure, {}};
    }

    [[nodiscard]] GpuTimestampPollResult pending() noexcept
    {
        ++counters.pendingPolls;
        return GpuTimestampPollResult{GpuTimestampPollStatus::Pending, {}};
    }

    [[nodiscard]] GpuTimestampPollResult pollOldest() noexcept
    {
        const std::optional<std::size_t> pendingSlot = oldestPendingSlot();
        if (!pendingSlot.has_value())
        {
            return GpuTimestampPollResult{
                GpuTimestampPollStatus::NoPendingFrame,
                {}};
        }

        Slot& slot = slots[*pendingSlot];
        detail::GpuTimestampDisjointQueryResult disjoint{};
        const detail::GpuTimestampQueryReadStatus disjointStatus =
            backend->readDisjoint(*pendingSlot, disjoint);
        if (disjointStatus == detail::GpuTimestampQueryReadStatus::Pending)
        {
            return pending();
        }
        if (disjointStatus == detail::GpuTimestampQueryReadStatus::Failure)
        {
            return queryFailure(*pendingSlot);
        }
        if (slot.state == SlotState::PendingCancellation)
        {
            releaseSlot(*pendingSlot);
            return GpuTimestampPollResult{
                GpuTimestampPollStatus::Cancelled,
                {}};
        }
        if (disjoint.disjoint || disjoint.frequency == 0U)
        {
            releaseSlot(*pendingSlot);
            ++counters.disjointFrames;
            return GpuTimestampPollResult{
                GpuTimestampPollStatus::Disjoint,
                {}};
        }

        std::array<std::uint64_t, detail::gpuTimestampBoundaryCount> timestamps{};
        for (std::size_t boundary = 0U;
             boundary < timestamps.size();
             ++boundary)
        {
            const detail::GpuTimestampQueryReadStatus timestampStatus =
                backend->readTimestamp(
                    *pendingSlot,
                    boundary,
                    timestamps[boundary]);
            if (timestampStatus == detail::GpuTimestampQueryReadStatus::Pending)
            {
                return pending();
            }
            if (timestampStatus == detail::GpuTimestampQueryReadStatus::Failure)
            {
                return queryFailure(*pendingSlot);
            }
        }

        const auto wgc = timestampDelta(
            timestamps[0U],
            timestamps[1U],
            disjoint.frequency);
        const auto snapshot = timestampDelta(
            timestamps[1U],
            timestamps[2U],
            disjoint.frequency);
        const auto materials = timestampDelta(
            timestamps[2U],
            timestamps[3U],
            disjoint.frequency);
        const auto primaryPrefilter = timestampDelta(
            timestamps[3U],
            timestamps[4U],
            disjoint.frequency);
        const auto primaryPyramid = timestampDelta(
            timestamps[4U],
            timestamps[5U],
            disjoint.frequency);
        const auto primaryFinalComposite = timestampDelta(
            timestamps[5U],
            timestamps[6U],
            disjoint.frequency);
        const auto recordingRebuildPrefilter = timestampDelta(
            timestamps[6U],
            timestamps[7U],
            disjoint.frequency);
        const auto recordingRebuildPyramid = timestampDelta(
            timestamps[7U],
            timestamps[8U],
            disjoint.frequency);
        const auto recordingRebuildFinalComposite = timestampDelta(
            timestamps[8U],
            timestamps[9U],
            disjoint.frequency);
        const auto bloom = timestampDelta(
            timestamps[3U],
            timestamps[10U],
            disjoint.frequency);
        const auto totalFx = timestampDelta(
            timestamps[2U],
            timestamps[10U],
            disjoint.frequency);
        const auto totalFrame = timestampDelta(
            timestamps[0U],
            timestamps[10U],
            disjoint.frequency);
        if (!wgc.has_value()
            || !snapshot.has_value()
            || !materials.has_value()
            || !primaryPrefilter.has_value()
            || !primaryPyramid.has_value()
            || !primaryFinalComposite.has_value()
            || !recordingRebuildPrefilter.has_value()
            || !recordingRebuildPyramid.has_value()
            || !recordingRebuildFinalComposite.has_value()
            || !bloom.has_value()
            || !totalFx.has_value()
            || !totalFrame.has_value())
        {
            return queryFailure(*pendingSlot);
        }

        GpuTimestampSample sample{};
        sample.frameId = slot.frameId;
        sample.wgcDrainAndCopy = *wgc;
        sample.backgroundSnapshot = *snapshot;
        sample.fxMaterials = *materials;
        sample.primary = GpuTimestampFxPathSample{
            *primaryPrefilter,
            *primaryPyramid,
            *primaryFinalComposite};
        sample.recordingRebuild = GpuTimestampFxPathSample{
            *recordingRebuildPrefilter,
            *recordingRebuildPyramid,
            *recordingRebuildFinalComposite};
        sample.bloomAndFinalComposite = *bloom;
        sample.totalFx = *totalFx;
        sample.totalFrame = *totalFrame;
        sample.usage = slot.usage;
        releaseSlot(*pendingSlot);
        ++counters.framesCompleted;
        return GpuTimestampPollResult{
            GpuTimestampPollStatus::Completed,
            sample};
    }

    std::unique_ptr<detail::GpuTimestampQueryBackend> backend;
    std::array<Slot, GpuTimestampProfiler::slotCount> slots{};
    std::optional<std::size_t> activeSlot{};
    std::optional<std::uint64_t> lastPollFrameToken{};
    std::size_t nextSlot{0U};
    std::uint64_t submissionSequence{0U};
    GpuTimestampProfilerCounters counters{};
};

GpuTimestampProfiler::GpuTimestampProfiler(
    ID3D11Device* device,
    ID3D11DeviceContext* context)
    : GpuTimestampProfiler(
        std::make_unique<D3d11GpuTimestampQueryBackend>(device, context))
{
}

GpuTimestampProfiler::GpuTimestampProfiler(
    std::unique_ptr<detail::GpuTimestampQueryBackend> backend)
    : implementation_(std::make_unique<Implementation>(std::move(backend)))
{
}

GpuTimestampProfiler::~GpuTimestampProfiler()
{
    if (implementation_->activeSlot.has_value())
    {
        (void)cancelFrame();
    }
}

bool GpuTimestampProfiler::available() const noexcept
{
    return implementation_->backend->available();
}

HRESULT GpuTimestampProfiler::initializationResult() const noexcept
{
    return implementation_->backend->initializationResult();
}

GpuTimestampBeginStatus GpuTimestampProfiler::beginFrame(
    const std::uint64_t frameId) noexcept
{
    Implementation& state = *implementation_;
    if (!available())
    {
        return GpuTimestampBeginStatus::Unavailable;
    }
    if (state.activeSlot.has_value())
    {
        return GpuTimestampBeginStatus::AlreadyActive;
    }
    const std::optional<std::size_t> freeSlot = state.freeSlot();
    if (!freeSlot.has_value())
    {
        ++state.counters.ringFullSkipped;
        return GpuTimestampBeginStatus::RingFullSkipped;
    }

    Implementation::Slot& slot = state.slots[*freeSlot];
    slot.state = Implementation::SlotState::Recording;
    slot.frameId = frameId;
    slot.nextBoundary = 1U;
    state.activeSlot = freeSlot;
    state.nextSlot = (*freeSlot + 1U) % state.slots.size();
    state.backend->beginDisjoint(*freeSlot);
    state.backend->writeTimestamp(*freeSlot, 0U);
    ++state.counters.framesStarted;
    return GpuTimestampBeginStatus::Started;
}

GpuTimestampCheckpointStatus GpuTimestampProfiler::checkpoint(
    const GpuTimestampCheckpoint checkpointValue) noexcept
{
    Implementation& state = *implementation_;
    if (!state.activeSlot.has_value())
    {
        return GpuTimestampCheckpointStatus::NoActiveFrame;
    }

    const std::size_t expectedBoundary =
        static_cast<std::size_t>(checkpointValue) + 1U;
    Implementation::Slot& slot = state.slots[*state.activeSlot];
    if (slot.nextBoundary != expectedBoundary)
    {
        return GpuTimestampCheckpointStatus::OutOfOrder;
    }
    state.backend->writeTimestamp(*state.activeSlot, expectedBoundary);
    Implementation::markExecuted(slot.usage, checkpointValue);
    ++slot.nextBoundary;
    return GpuTimestampCheckpointStatus::Recorded;
}

GpuTimestampCheckpointStatus GpuTimestampProfiler::skipCheckpoint(
    const GpuTimestampCheckpoint checkpointValue) noexcept
{
    Implementation& state = *implementation_;
    if (!state.activeSlot.has_value())
    {
        return GpuTimestampCheckpointStatus::NoActiveFrame;
    }
    if (!Implementation::isOptionalCheckpoint(checkpointValue))
    {
        return GpuTimestampCheckpointStatus::NotSkippable;
    }

    const std::size_t expectedBoundary =
        static_cast<std::size_t>(checkpointValue) + 1U;
    Implementation::Slot& slot = state.slots[*state.activeSlot];
    if (slot.nextBoundary != expectedBoundary)
    {
        return GpuTimestampCheckpointStatus::OutOfOrder;
    }
    state.backend->writeTimestamp(*state.activeSlot, expectedBoundary);
    ++slot.nextBoundary;
    return GpuTimestampCheckpointStatus::Skipped;
}

GpuTimestampEndStatus GpuTimestampProfiler::endFrame(
    const GpuTimestampFrameUsage usage) noexcept
{
    Implementation& state = *implementation_;
    if (!state.activeSlot.has_value())
    {
        return GpuTimestampEndStatus::NoActiveFrame;
    }

    Implementation::Slot& slot = state.slots[*state.activeSlot];
    if (slot.nextBoundary < detail::gpuTimestampRequiredCheckpointCount + 1U)
    {
        (void)cancelFrame();
        return GpuTimestampEndStatus::IncompleteCancelled;
    }

    const std::size_t completedSlot = *state.activeSlot;
    // Old renderers stop after FxMaterialsComplete. Fill the optional tail at
    // the same command position so an instrumentation upgrade cannot cancel
    // otherwise valid frames or manufacture applicable stage measurements.
    const bool autoSkippedStages =
        slot.nextBoundary < detail::gpuTimestampBoundaryCount - 1U;
    while (slot.nextBoundary < detail::gpuTimestampBoundaryCount - 1U)
    {
        state.backend->writeTimestamp(completedSlot, slot.nextBoundary);
        ++slot.nextBoundary;
    }
    state.backend->writeTimestamp(
        completedSlot,
        detail::gpuTimestampBoundaryCount - 1U);
    state.backend->endDisjoint(completedSlot);
    slot.usage.wgcDrainAttempted = usage.wgcDrainAttempted;
    slot.usage.backgroundSnapshotAttempted =
        usage.backgroundSnapshotAttempted;
    slot.usage.visualContent = usage.visualContent;
    slot.state = Implementation::SlotState::Pending;
    slot.submissionSequence = ++state.submissionSequence;
    state.activeSlot.reset();
    ++state.counters.framesSubmitted;
    state.counters.autoSkippedStageFrames += autoSkippedStages ? 1U : 0U;
    return autoSkippedStages
        ? GpuTimestampEndStatus::SubmittedWithAutoSkippedStages
        : GpuTimestampEndStatus::Submitted;
}

GpuTimestampCancelStatus GpuTimestampProfiler::cancelFrame() noexcept
{
    Implementation& state = *implementation_;
    if (!state.activeSlot.has_value())
    {
        return GpuTimestampCancelStatus::NoActiveFrame;
    }

    const std::size_t cancelledSlot = *state.activeSlot;
    state.backend->endDisjoint(cancelledSlot);
    Implementation::Slot& slot = state.slots[cancelledSlot];
    slot.state = Implementation::SlotState::PendingCancellation;
    slot.submissionSequence = ++state.submissionSequence;
    state.activeSlot.reset();
    ++state.counters.framesCancelled;
    return GpuTimestampCancelStatus::Cancelled;
}

GpuTimestampPollResult GpuTimestampProfiler::poll(
    const std::uint64_t frameToken) noexcept
{
    Implementation& state = *implementation_;
    if (!available())
    {
        return GpuTimestampPollResult{
            GpuTimestampPollStatus::Unavailable,
            {}};
    }
    if (state.activeSlot.has_value())
    {
        return GpuTimestampPollResult{
            GpuTimestampPollStatus::ActiveFrame,
            {}};
    }
    if (state.lastPollFrameToken.has_value()
        && *state.lastPollFrameToken == frameToken)
    {
        return GpuTimestampPollResult{
            GpuTimestampPollStatus::AlreadyPolled,
            {}};
    }
    state.lastPollFrameToken = frameToken;
    return state.pollOldest();
}

std::size_t GpuTimestampProfiler::pendingFrameCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(
        implementation_->slots.begin(),
        implementation_->slots.end(),
        [](const Implementation::Slot& slot)
        {
            return slot.state == Implementation::SlotState::Pending
                || slot.state
                    == Implementation::SlotState::PendingCancellation;
        }));
}

const GpuTimestampProfilerCounters& GpuTimestampProfiler::counters() const noexcept
{
    return implementation_->counters;
}

}
