#include "display_policy.hpp"

#include <chrono>
#include <cstdint>

namespace bafx::desktop
{
namespace
{

[[nodiscard]] std::optional<bafx::core::MonotonicTime> framePeriod(
    const std::uint32_t numerator,
    const std::uint32_t denominator) noexcept
{
    if (numerator == 0U || denominator == 0U)
    {
        return std::nullopt;
    }

    const std::uint64_t scaledDenominator =
        static_cast<std::uint64_t>(denominator);
    const std::uint64_t scaledNumerator =
        static_cast<std::uint64_t>(numerator);
    if (scaledNumerator < scaledDenominator
        || scaledNumerator > scaledDenominator * 1'000U)
    {
        // Match the topology contract so malformed in-memory refresh evidence
        // cannot produce an impractically slow or busy render loop.
        return std::nullopt;
    }

    const std::uint64_t second = static_cast<std::uint64_t>(
        std::chrono::duration_cast<bafx::core::MonotonicTime>(
            std::chrono::seconds(1)).count());
    const std::uint64_t scaledSecond = second * scaledDenominator;
    // Round upward so the limiter never schedules a frame before the display
    // period represented by the exact DisplayConfig rational.
    const std::uint64_t period =
        (scaledSecond + scaledNumerator - 1U) / scaledNumerator;
    return bafx::core::MonotonicTime{
        static_cast<bafx::core::MonotonicTime::rep>(period)};
}

}

std::optional<bafx::core::MonotonicTime> minimumFramePacingPeriod(
    const bafx::config::FramePacing pacing,
    const std::optional<bafx::windows::DisplayRefreshRate>& refreshRate)
    noexcept
{
    switch (pacing)
    {
    case bafx::config::FramePacing::MatchDisplay:
        if (refreshRate.has_value())
        {
            if (const auto period = framePeriod(
                    refreshRate->numerator,
                    refreshRate->denominator);
                period.has_value())
            {
                return period;
            }
        }
        // Missing display evidence must stay bounded. Unlimited rendering is
        // available only through its explicit user-facing option.
        return framePeriod(60U, 1U);
    case bafx::config::FramePacing::Fixed60:
        return framePeriod(60U, 1U);
    case bafx::config::FramePacing::Fixed120:
        return framePeriod(120U, 1U);
    case bafx::config::FramePacing::Fixed144:
        return framePeriod(144U, 1U);
    case bafx::config::FramePacing::Unlimited:
        return std::nullopt;
    }
    // Invalid in-memory enum values retain the fail-closed 60 FPS ceiling.
    return framePeriod(60U, 1U);
}

ResolvedDisplaySessionPolicy resolveDisplaySessionPolicy(
    const bafx::config::Config& config,
    const DisplayTarget& target) noexcept
{
    ResolvedDisplaySessionPolicy result{};
    result.displayKey = displayTargetPersistentKey(target);
    const bafx::config::ResolvedDisplayPolicy resolved =
        bafx::config::resolveDisplayPolicy(
            config,
            result.displayKey.has_value()
                ? std::string_view(*result.displayKey)
                : std::string_view{});
    result.enabled = resolved.enabled;
    const bool coreMode = config.performance.effectsMode
        == bafx::config::EffectsMode::Core;
    result.hdrEnabled = coreMode ? false : resolved.hdrEnabled;
    result.framePacing = coreMode
        ? bafx::config::FramePacing::Fixed60
        : resolved.framePacing;
    result.overridden = resolved.overridden;
    result.outputPreference = !coreMode && resolved.hdrEnabled
        ? bafx::windows::CompositionOutputPreference::PreferLinearScRgb
        : bafx::windows::CompositionOutputPreference::ConservativeSdr;
    result.minimumFramePeriod = minimumFramePacingPeriod(
        result.framePacing,
        target.refreshRate);
    return result;
}

}
