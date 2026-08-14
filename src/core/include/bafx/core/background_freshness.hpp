#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace bafx::core
{

using MonotonicTime = std::chrono::nanoseconds;

struct BackgroundFrameStamp
{
    MonotonicTime capturedAt{};
    std::uint64_t epoch{0};
    bool canonicalLinearScRgb{false};
    bool excludesOwnOverlay{false};
};

struct BackgroundUsagePolicy
{
    MonotonicTime maxAge{};
    MonotonicTime maxFutureSkew{};
    std::uint64_t expectedEpoch{0};
};

enum class BackgroundUsageStatus : std::uint8_t
{
    Usable,
    Missing,
    Stale,
    FutureTimestamp,
    WrongEpoch,
    InvalidContract,
    InvalidPolicy
};

struct BackgroundUsageDecision
{
    BackgroundUsageStatus status{BackgroundUsageStatus::Missing};
    MonotonicTime age{};
    bool enabled{false};
};

enum class BackgroundRenderPath : std::uint8_t
{
    FxOnly,
    BackgroundAware
};

// A selected path is one complete visual contract. The renderer passes
// retainAllowed as either a live sample decision or the availability of its
// immutable batch snapshot; the latter keeps a visible effect on one path
// through a transient capture-age miss.
class BackgroundPathLatch final
{
public:
    [[nodiscard]] BackgroundRenderPath select(
        bool hasVisibleContent,
        bool acquireAllowed,
        bool retainAllowed) noexcept;
    // A failed first snapshot must keep the current visible batch on one
    // compositing contract; the next idle boundary resets it normally.
    void forceFxOnly() noexcept;
    void reset() noexcept;

private:
    std::optional<BackgroundRenderPath> path_{};
};

// A usable sample selects one complete visual path: Differential Bloom and
// final source-over reconstruction must never disagree about background use.
[[nodiscard]] BackgroundUsageDecision evaluateBackgroundUsage(
    const std::optional<BackgroundFrameStamp>& frame,
    MonotonicTime renderAt,
    const BackgroundUsagePolicy& policy) noexcept;

}
