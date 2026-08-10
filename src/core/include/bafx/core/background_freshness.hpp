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

class BackgroundPathLatch final
{
public:
    [[nodiscard]] BackgroundRenderPath select(
        bool hasVisibleContent,
        bool acquireAllowed,
        bool retainAllowed) noexcept;
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
