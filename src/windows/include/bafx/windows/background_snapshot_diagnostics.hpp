#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace bafx::windows
{

enum class BackgroundSnapshotInvalidationReason : std::uint8_t
{
    VisibleBatchEnded,
    FxOnlyPathSelected,
    WgcDrainFailed,
    WgcSessionStopped,
    FramePoolReconfigureRequired,
    OutputResize,
    DeviceResourcesReleased,
    CaptureSessionReplaced,
    CaptureDisabled,
    SensorStartFailed,
    ReferenceWhiteUnavailable,
    SnapshotResourcesRecreated
};

struct BackgroundSnapshotInvalidation
{
    BackgroundSnapshotInvalidationReason reason{
        BackgroundSnapshotInvalidationReason::VisibleBatchEnded};
    std::uint64_t frameId{0U};
    std::uint64_t wgcEpoch{0U};
    std::uint64_t wgcGeneration{0U};
    std::uint64_t snapshotEpoch{0U};
    std::uint64_t snapshotGeneration{0U};
};

[[nodiscard]] std::string_view backgroundSnapshotInvalidationReasonName(
    BackgroundSnapshotInvalidationReason reason) noexcept;

namespace detail
{

// Renderer state has one owner thread. A single slot preserves the first
// causal invalidation until the Host logger consumes it without allocating.
class BackgroundSnapshotInvalidationMailbox final
{
public:
    void record(BackgroundSnapshotInvalidation invalidation) noexcept;
    [[nodiscard]] std::optional<BackgroundSnapshotInvalidation> take() noexcept;

private:
    std::optional<BackgroundSnapshotInvalidation> pending_{};
};

}

}
