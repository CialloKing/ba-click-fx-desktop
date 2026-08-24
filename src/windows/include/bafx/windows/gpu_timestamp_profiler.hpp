#pragma once

#include <d3d11.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace bafx::windows
{

namespace detail
{

inline constexpr std::size_t gpuTimestampRequiredCheckpointCount = 3U;
inline constexpr std::size_t gpuTimestampOptionalCheckpointCount = 6U;
inline constexpr std::size_t gpuTimestampCheckpointCount =
    gpuTimestampRequiredCheckpointCount + gpuTimestampOptionalCheckpointCount;
inline constexpr std::size_t gpuTimestampBoundaryCount =
    gpuTimestampCheckpointCount + 2U;

enum class GpuTimestampQueryReadStatus : std::uint8_t
{
    Ready,
    Pending,
    Failure
};

struct GpuTimestampDisjointQueryResult
{
    std::uint64_t frequency{0U};
    bool disjoint{false};
};

// This narrow interface keeps the ring state machine deterministic in tests.
// The production backend maps it directly to D3D11 timestamp queries.
class GpuTimestampQueryBackend
{
public:
    virtual ~GpuTimestampQueryBackend() = default;

    [[nodiscard]] virtual bool available() const noexcept = 0;
    [[nodiscard]] virtual HRESULT initializationResult() const noexcept = 0;
    virtual void beginDisjoint(std::size_t slot) noexcept = 0;
    virtual void writeTimestamp(
        std::size_t slot,
        std::size_t boundary) noexcept = 0;
    virtual void endDisjoint(std::size_t slot) noexcept = 0;
    [[nodiscard]] virtual GpuTimestampQueryReadStatus readDisjoint(
        std::size_t slot,
        GpuTimestampDisjointQueryResult& result) noexcept = 0;
    [[nodiscard]] virtual GpuTimestampQueryReadStatus readTimestamp(
        std::size_t slot,
        std::size_t boundary,
        std::uint64_t& timestamp) noexcept = 0;
};

}

enum class GpuTimestampCheckpoint : std::uint8_t
{
    WgcDrainAndCopyComplete = 0U,
    BackgroundSnapshotComplete = 1U,
    FxMaterialsComplete = 2U,
    PrimaryPrefilterComplete = 3U,
    PrimaryPyramidComplete = 4U,
    PrimaryFinalCompositeComplete = 5U,
    RecordingRebuildPrefilterComplete = 6U,
    RecordingRebuildPyramidComplete = 7U,
    RecordingRebuildFinalCompositeComplete = 8U
};

enum class GpuTimestampBeginStatus : std::uint8_t
{
    Started,
    Unavailable,
    AlreadyActive,
    RingFullSkipped
};

enum class GpuTimestampCheckpointStatus : std::uint8_t
{
    Recorded,
    Skipped,
    NoActiveFrame,
    OutOfOrder,
    NotSkippable
};

enum class GpuTimestampEndStatus : std::uint8_t
{
    Submitted,
    SubmittedWithAutoSkippedStages,
    NoActiveFrame,
    IncompleteCancelled
};

enum class GpuTimestampCancelStatus : std::uint8_t
{
    Cancelled,
    NoActiveFrame
};

enum class GpuTimestampPollStatus : std::uint8_t
{
    NoPendingFrame,
    Pending,
    Completed,
    Cancelled,
    Disjoint,
    QueryFailure,
    Unavailable,
    ActiveFrame,
    AlreadyPolled
};

struct GpuTimestampFxPathUsage
{
    bool prefilterExecuted{false};
    bool pyramidExecuted{false};
    bool finalCompositeExecuted{false};
};

struct GpuTimestampFrameUsage
{
    bool wgcDrainAttempted{false};
    bool backgroundSnapshotAttempted{false};
    bool visualContent{false};
    GpuTimestampFxPathUsage primary{};
    GpuTimestampFxPathUsage recordingRebuild{};
};

struct GpuTimestampFxPathSample
{
    std::chrono::nanoseconds prefilter{};
    std::chrono::nanoseconds pyramid{};
    std::chrono::nanoseconds finalComposite{};
};

struct GpuTimestampSample
{
    std::uint64_t frameId{0U};
    std::chrono::nanoseconds wgcDrainAndCopy{};
    std::chrono::nanoseconds backgroundSnapshot{};
    std::chrono::nanoseconds fxMaterials{};
    GpuTimestampFxPathSample primary{};
    GpuTimestampFxPathSample recordingRebuild{};
    // This legacy interval deliberately spans both paths and any tail work.
    std::chrono::nanoseconds bloomAndFinalComposite{};
    std::chrono::nanoseconds totalFx{};
    std::chrono::nanoseconds totalFrame{};
    GpuTimestampFrameUsage usage{};
};

struct GpuTimestampPollResult
{
    GpuTimestampPollStatus status{GpuTimestampPollStatus::NoPendingFrame};
    std::optional<GpuTimestampSample> sample{};
};

struct GpuTimestampProfilerCounters
{
    std::uint64_t framesStarted{0U};
    std::uint64_t framesSubmitted{0U};
    std::uint64_t framesCompleted{0U};
    std::uint64_t framesCancelled{0U};
    std::uint64_t pendingPolls{0U};
    std::uint64_t ringFullSkipped{0U};
    std::uint64_t autoSkippedStageFrames{0U};
    std::uint64_t disjointFrames{0U};
    std::uint64_t queryFailures{0U};
};

class GpuTimestampProfiler final
{
public:
    static constexpr std::size_t slotCount = 8U;

    GpuTimestampProfiler(
        ID3D11Device* device,
        ID3D11DeviceContext* context);
    // Alternate backends are useful for deterministic state-machine tests.
    explicit GpuTimestampProfiler(
        std::unique_ptr<detail::GpuTimestampQueryBackend> backend);
    ~GpuTimestampProfiler();

    GpuTimestampProfiler(const GpuTimestampProfiler&) = delete;
    GpuTimestampProfiler& operator=(const GpuTimestampProfiler&) = delete;
    GpuTimestampProfiler(GpuTimestampProfiler&&) = delete;
    GpuTimestampProfiler& operator=(GpuTimestampProfiler&&) = delete;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] HRESULT initializationResult() const noexcept;
    [[nodiscard]] GpuTimestampBeginStatus beginFrame(
        std::uint64_t frameId) noexcept;
    [[nodiscard]] GpuTimestampCheckpointStatus checkpoint(
        GpuTimestampCheckpoint checkpoint) noexcept;
    // Optional stages still write a timestamp when skipped so every query slot
    // keeps one fixed layout and polling never needs a variable query plan.
    [[nodiscard]] GpuTimestampCheckpointStatus skipCheckpoint(
        GpuTimestampCheckpoint checkpoint) noexcept;
    [[nodiscard]] GpuTimestampEndStatus endFrame(
        GpuTimestampFrameUsage usage = {}) noexcept;
    [[nodiscard]] GpuTimestampCancelStatus cancelFrame() noexcept;

    // A frame token may be polled only once. Every backend GetData call uses
    // D3D11_ASYNC_GETDATA_DONOTFLUSH and no retry loop is performed.
    [[nodiscard]] GpuTimestampPollResult poll(
        std::uint64_t frameToken) noexcept;

    [[nodiscard]] std::size_t pendingFrameCount() const noexcept;
    [[nodiscard]] const GpuTimestampProfilerCounters& counters() const noexcept;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}
