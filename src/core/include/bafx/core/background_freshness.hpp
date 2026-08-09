#pragma once

#include "bafx/core/intensity.hpp"

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

struct BackgroundFreshnessPolicy
{
    MonotonicTime fullWeightAge{};
    MonotonicTime staleAge{};
    MonotonicTime maxFutureSkew{};
    std::uint64_t expectedEpoch{0};
};

enum class BackgroundFreshness : std::uint8_t
{
    Fresh,
    Fading,
    Missing,
    Stale,
    FutureTimestamp,
    WrongEpoch,
    InvalidContract,
    InvalidPolicy
};

struct BackgroundFreshnessResult
{
    BackgroundFreshness freshness{BackgroundFreshness::Missing};
    float weight{0.0F};
    MonotonicTime age{};
};

[[nodiscard]] BackgroundFreshnessResult evaluateBackgroundFreshness(
    const std::optional<BackgroundFrameStamp>& frame,
    MonotonicTime renderAt,
    const BackgroundFreshnessPolicy& policy) noexcept;

enum class BloomInputMode : std::uint8_t
{
    BackgroundAware,
    FxOnly,
    Disabled
};

[[nodiscard]] BloomInputMode selectBloomInputMode(
    const BackgroundFreshnessResult& freshness,
    BackgroundFallback fallback) noexcept;

}

