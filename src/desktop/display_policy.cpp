#include "display_policy.hpp"

#include <chrono>
#include <cstdint>

namespace bafx::desktop
{

std::optional<bafx::core::MonotonicTime> fixedFramePacingPeriod(
    const bafx::config::FramePacing pacing) noexcept
{
    std::uint32_t framesPerSecond = 0U;
    switch (pacing)
    {
    case bafx::config::FramePacing::MatchDisplay:
        return std::nullopt;
    case bafx::config::FramePacing::Fixed60:
        framesPerSecond = 60U;
        break;
    case bafx::config::FramePacing::Fixed120:
        framesPerSecond = 120U;
        break;
    case bafx::config::FramePacing::Fixed144:
        framesPerSecond = 144U;
        break;
    }
    if (framesPerSecond == 0U)
    {
        // Keep malformed in-memory state fail-closed even if a caller bypasses
        // config validation; division by zero must never reach the Host loop.
        return std::nullopt;
    }

    const std::int64_t second = std::chrono::duration_cast<
        bafx::core::MonotonicTime>(std::chrono::seconds(1)).count();
    return bafx::core::MonotonicTime{
        (second + static_cast<std::int64_t>(framesPerSecond) - 1LL)
        / static_cast<std::int64_t>(framesPerSecond)};
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
    result.fixedFramePeriod = fixedFramePacingPeriod(result.framePacing);
    return result;
}

}
