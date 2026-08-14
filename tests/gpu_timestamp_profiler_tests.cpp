#include "test_support.hpp"

#include "bafx/windows/gpu_timestamp_profiler.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

using namespace std::chrono_literals;
using namespace bafx::windows;

namespace
{

struct WarpDevice
{
    Microsoft::WRL::ComPtr<ID3D11Device> device{};
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context{};
};

[[nodiscard]] WarpDevice createWarpDevice()
{
    constexpr std::array featureLevels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    WarpDevice graphics{};
    D3D_FEATURE_LEVEL selectedLevel{};
    const HRESULT result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_WARP,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels.data(),
        static_cast<UINT>(featureLevels.size()),
        D3D11_SDK_VERSION,
        &graphics.device,
        &selectedLevel,
        &graphics.context);
    BAFX_CHECK(SUCCEEDED(result));
    BAFX_CHECK(selectedLevel >= D3D_FEATURE_LEVEL_11_0);
    return graphics;
}

class FakeTimestampQueryBackend final
    : public detail::GpuTimestampQueryBackend
{
public:
    struct Slot
    {
        detail::GpuTimestampQueryReadStatus disjointStatus{
            detail::GpuTimestampQueryReadStatus::Ready};
        detail::GpuTimestampDisjointQueryResult disjoint{3U, false};
        std::array<
            detail::GpuTimestampQueryReadStatus,
            detail::gpuTimestampBoundaryCount> timestampStatuses{};
        std::array<std::uint64_t, detail::gpuTimestampBoundaryCount> timestamps{
            0U,
            3U,
            9U,
            15U,
            30U};
        std::size_t beginCalls{0U};
        std::size_t endCalls{0U};
        std::size_t disjointReadCalls{0U};
        std::array<std::size_t, detail::gpuTimestampBoundaryCount>
            timestampWriteCalls{};
        std::array<std::size_t, detail::gpuTimestampBoundaryCount>
            timestampReadCalls{};

        Slot()
        {
            timestampStatuses.fill(
                detail::GpuTimestampQueryReadStatus::Ready);
        }
    };

    [[nodiscard]] bool available() const noexcept override
    {
        return isAvailable;
    }

    [[nodiscard]] HRESULT initializationResult() const noexcept override
    {
        return initializationError;
    }

    void beginDisjoint(const std::size_t slot) noexcept override
    {
        ++slots[slot].beginCalls;
    }

    void writeTimestamp(
        const std::size_t slot,
        const std::size_t boundary) noexcept override
    {
        ++slots[slot].timestampWriteCalls[boundary];
    }

    void endDisjoint(const std::size_t slot) noexcept override
    {
        ++slots[slot].endCalls;
    }

    [[nodiscard]] detail::GpuTimestampQueryReadStatus readDisjoint(
        const std::size_t slot,
        detail::GpuTimestampDisjointQueryResult& result) noexcept override
    {
        ++slots[slot].disjointReadCalls;
        result = slots[slot].disjoint;
        return slots[slot].disjointStatus;
    }

    [[nodiscard]] detail::GpuTimestampQueryReadStatus readTimestamp(
        const std::size_t slot,
        const std::size_t boundary,
        std::uint64_t& timestamp) noexcept override
    {
        ++slots[slot].timestampReadCalls[boundary];
        timestamp = slots[slot].timestamps[boundary];
        return slots[slot].timestampStatuses[boundary];
    }

    bool isAvailable{true};
    HRESULT initializationError{S_OK};
    std::array<Slot, GpuTimestampProfiler::slotCount> slots{};
};

struct ProfilerFixture
{
    ProfilerFixture()
    {
        auto ownedBackend = std::make_unique<FakeTimestampQueryBackend>();
        backend = ownedBackend.get();
        profiler = std::make_unique<GpuTimestampProfiler>(
            std::move(ownedBackend));
    }

    FakeTimestampQueryBackend* backend{nullptr};
    std::unique_ptr<GpuTimestampProfiler> profiler{};
};

void submitFrame(
    GpuTimestampProfiler& profiler,
    const std::uint64_t frameId,
    const GpuTimestampFrameUsage usage = {true, true, true})
{
    BAFX_CHECK(
        profiler.beginFrame(frameId) == GpuTimestampBeginStatus::Started);
    BAFX_CHECK(
        profiler.checkpoint(
            GpuTimestampCheckpoint::WgcDrainAndCopyComplete)
        == GpuTimestampCheckpointStatus::Recorded);
    BAFX_CHECK(
        profiler.checkpoint(
            GpuTimestampCheckpoint::BackgroundSnapshotComplete)
        == GpuTimestampCheckpointStatus::Recorded);
    BAFX_CHECK(
        profiler.checkpoint(GpuTimestampCheckpoint::FxMaterialsComplete)
        == GpuTimestampCheckpointStatus::Recorded);
    BAFX_CHECK(profiler.endFrame(usage) == GpuTimestampEndStatus::Submitted);
}

}

BAFX_TEST(gpu_timestamp_profiler_uses_eight_fixed_slots_and_skips_when_full)
{
    ProfilerFixture fixture;
    static_assert(GpuTimestampProfiler::slotCount == 8U);

    for (std::uint64_t frameId = 1U;
         frameId <= GpuTimestampProfiler::slotCount;
         ++frameId)
    {
        submitFrame(*fixture.profiler, frameId);
    }

    BAFX_CHECK(
        fixture.profiler->pendingFrameCount()
        == GpuTimestampProfiler::slotCount);
    BAFX_CHECK(
        fixture.profiler->beginFrame(9U)
        == GpuTimestampBeginStatus::RingFullSkipped);
    BAFX_CHECK(fixture.profiler->counters().ringFullSkipped == 1U);
}

BAFX_TEST(gpu_timestamp_profiler_reports_exact_stage_and_total_durations)
{
    ProfilerFixture fixture;
    submitFrame(*fixture.profiler, 42U);

    const GpuTimestampPollResult result = fixture.profiler->poll(100U);
    BAFX_CHECK(result.status == GpuTimestampPollStatus::Completed);
    BAFX_CHECK(result.sample.has_value());
    BAFX_CHECK(result.sample->frameId == 42U);
    BAFX_CHECK(result.sample->wgcDrainAndCopy == 1s);
    BAFX_CHECK(result.sample->backgroundSnapshot == 2s);
    BAFX_CHECK(result.sample->fxMaterials == 2s);
    BAFX_CHECK(result.sample->bloomAndFinalComposite == 5s);
    BAFX_CHECK(result.sample->totalFx == 7s);
    BAFX_CHECK(result.sample->totalFrame == 10s);
    BAFX_CHECK(result.sample->usage.wgcDrainAttempted);
    BAFX_CHECK(result.sample->usage.backgroundSnapshotAttempted);
    BAFX_CHECK(result.sample->usage.visualContent);
    BAFX_CHECK(fixture.profiler->counters().framesCompleted == 1U);
    BAFX_CHECK(fixture.profiler->pendingFrameCount() == 0U);
}

BAFX_TEST(gpu_timestamp_profiler_polls_once_per_frame_token_without_waiting)
{
    ProfilerFixture fixture;
    submitFrame(*fixture.profiler, 1U);
    fixture.backend->slots[0U].disjointStatus =
        detail::GpuTimestampQueryReadStatus::Pending;

    BAFX_CHECK(
        fixture.profiler->poll(10U).status
        == GpuTimestampPollStatus::Pending);
    BAFX_CHECK(fixture.backend->slots[0U].disjointReadCalls == 1U);
    BAFX_CHECK(
        fixture.profiler->poll(10U).status
        == GpuTimestampPollStatus::AlreadyPolled);
    BAFX_CHECK(fixture.backend->slots[0U].disjointReadCalls == 1U);

    fixture.backend->slots[0U].disjointStatus =
        detail::GpuTimestampQueryReadStatus::Ready;
    fixture.backend->slots[0U].timestampStatuses[2U] =
        detail::GpuTimestampQueryReadStatus::Pending;
    BAFX_CHECK(
        fixture.profiler->poll(11U).status
        == GpuTimestampPollStatus::Pending);
    BAFX_CHECK(fixture.backend->slots[0U].disjointReadCalls == 2U);
    BAFX_CHECK(fixture.backend->slots[0U].timestampReadCalls[0U] == 1U);
    BAFX_CHECK(fixture.backend->slots[0U].timestampReadCalls[1U] == 1U);
    BAFX_CHECK(fixture.backend->slots[0U].timestampReadCalls[2U] == 1U);
    BAFX_CHECK(fixture.backend->slots[0U].timestampReadCalls[3U] == 0U);
    BAFX_CHECK(fixture.profiler->counters().pendingPolls == 2U);
}

BAFX_TEST(gpu_timestamp_profiler_preserves_original_stage_applicability)
{
    ProfilerFixture fixture;
    submitFrame(
        *fixture.profiler,
        8U,
        GpuTimestampFrameUsage{
            false,
            false,
            true});

    const GpuTimestampPollResult result = fixture.profiler->poll(12U);
    BAFX_CHECK(result.status == GpuTimestampPollStatus::Completed);
    BAFX_CHECK(result.sample.has_value());
    BAFX_CHECK(!result.sample->usage.wgcDrainAttempted);
    BAFX_CHECK(!result.sample->usage.backgroundSnapshotAttempted);
    BAFX_CHECK(result.sample->usage.visualContent);
}

BAFX_TEST(gpu_timestamp_profiler_reclaims_cancelled_frames_after_gpu_completion)
{
    ProfilerFixture fixture;
    BAFX_CHECK(
        fixture.profiler->beginFrame(5U)
        == GpuTimestampBeginStatus::Started);
    BAFX_CHECK(
        fixture.profiler->checkpoint(
            GpuTimestampCheckpoint::WgcDrainAndCopyComplete)
        == GpuTimestampCheckpointStatus::Recorded);
    BAFX_CHECK(
        fixture.profiler->cancelFrame()
        == GpuTimestampCancelStatus::Cancelled);
    BAFX_CHECK(fixture.backend->slots[0U].endCalls == 1U);
    BAFX_CHECK(fixture.profiler->pendingFrameCount() == 1U);

    fixture.backend->slots[0U].disjointStatus =
        detail::GpuTimestampQueryReadStatus::Pending;
    BAFX_CHECK(
        fixture.profiler->poll(20U).status
        == GpuTimestampPollStatus::Pending);
    fixture.backend->slots[0U].disjointStatus =
        detail::GpuTimestampQueryReadStatus::Ready;
    BAFX_CHECK(
        fixture.profiler->poll(21U).status
        == GpuTimestampPollStatus::Cancelled);
    BAFX_CHECK(fixture.profiler->pendingFrameCount() == 0U);
    BAFX_CHECK(fixture.profiler->counters().framesCancelled == 1U);
    BAFX_CHECK(
        fixture.profiler->beginFrame(6U)
        == GpuTimestampBeginStatus::Started);
}

BAFX_TEST(gpu_timestamp_profiler_cancels_incomplete_and_rejects_bad_order)
{
    ProfilerFixture fixture;
    BAFX_CHECK(
        fixture.profiler->beginFrame(1U)
        == GpuTimestampBeginStatus::Started);
    BAFX_CHECK(
        fixture.profiler->checkpoint(
            GpuTimestampCheckpoint::BackgroundSnapshotComplete)
        == GpuTimestampCheckpointStatus::OutOfOrder);
    BAFX_CHECK(
        fixture.profiler->endFrame()
        == GpuTimestampEndStatus::IncompleteCancelled);
    BAFX_CHECK(fixture.backend->slots[0U].endCalls == 1U);
    BAFX_CHECK(fixture.profiler->counters().framesCancelled == 1U);
}

BAFX_TEST(gpu_timestamp_profiler_discards_disjoint_samples)
{
    ProfilerFixture fixture;
    submitFrame(*fixture.profiler, 1U);
    fixture.backend->slots[0U].disjoint.disjoint = true;

    const GpuTimestampPollResult result = fixture.profiler->poll(30U);
    BAFX_CHECK(result.status == GpuTimestampPollStatus::Disjoint);
    BAFX_CHECK(!result.sample.has_value());
    BAFX_CHECK(fixture.profiler->counters().disjointFrames == 1U);
    BAFX_CHECK(fixture.backend->slots[0U].timestampReadCalls[0U] == 0U);
}

BAFX_TEST(gpu_timestamp_profiler_reports_query_failures_and_releases_the_slot)
{
    ProfilerFixture fixture;
    submitFrame(*fixture.profiler, 1U);
    fixture.backend->slots[0U].timestampStatuses[3U] =
        detail::GpuTimestampQueryReadStatus::Failure;

    const GpuTimestampPollResult result = fixture.profiler->poll(40U);
    BAFX_CHECK(result.status == GpuTimestampPollStatus::QueryFailure);
    BAFX_CHECK(fixture.profiler->pendingFrameCount() == 0U);
    BAFX_CHECK(fixture.profiler->counters().queryFailures == 1U);
}

BAFX_TEST(gpu_timestamp_profiler_treats_invalid_timestamp_order_as_failure)
{
    ProfilerFixture fixture;
    submitFrame(*fixture.profiler, 1U);
    fixture.backend->slots[0U].timestamps = {0U, 3U, 2U, 15U, 30U};

    BAFX_CHECK(
        fixture.profiler->poll(50U).status
        == GpuTimestampPollStatus::QueryFailure);
    BAFX_CHECK(fixture.profiler->counters().queryFailures == 1U);
}

BAFX_TEST(gpu_timestamp_profiler_surfaces_initialization_failure)
{
    auto backend = std::make_unique<FakeTimestampQueryBackend>();
    backend->isAvailable = false;
    backend->initializationError = E_OUTOFMEMORY;
    GpuTimestampProfiler profiler(std::move(backend));

    BAFX_CHECK(!profiler.available());
    BAFX_CHECK(profiler.initializationResult() == E_OUTOFMEMORY);
    BAFX_CHECK(
        profiler.beginFrame(1U) == GpuTimestampBeginStatus::Unavailable);
    BAFX_CHECK(
        profiler.poll(1U).status == GpuTimestampPollStatus::Unavailable);
    BAFX_CHECK(profiler.counters().queryFailures == 1U);
}

BAFX_TEST(gpu_timestamp_profiler_warp_backend_completes_without_internal_flush)
{
    const WarpDevice graphics = createWarpDevice();
    GpuTimestampProfiler profiler(
        graphics.device.Get(),
        graphics.context.Get());
    BAFX_CHECK(profiler.available());
    submitFrame(profiler, 77U);

    // Flush belongs to the test harness, not the profiler. It makes WARP
    // execute the command list while poll itself remains strictly DONOTFLUSH.
    graphics.context->Flush();
    GpuTimestampPollResult result{};
    constexpr std::uint64_t maximumFramePolls = 4'096U;
    for (std::uint64_t frameToken = 1U;
         frameToken <= maximumFramePolls;
         ++frameToken)
    {
        result = profiler.poll(frameToken);
        if (result.status != GpuTimestampPollStatus::Pending)
        {
            break;
        }
        std::this_thread::yield();
    }

    BAFX_CHECK(result.status == GpuTimestampPollStatus::Completed);
    BAFX_CHECK(result.sample.has_value());
    BAFX_CHECK(result.sample->frameId == 77U);
    BAFX_CHECK(profiler.pendingFrameCount() == 0U);
}
