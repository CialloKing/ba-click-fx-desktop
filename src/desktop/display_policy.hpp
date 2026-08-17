#pragma once

#include "display_target.hpp"

#include "bafx/config/config.hpp"
#include "bafx/core/background_freshness.hpp"
#include "bafx/windows/composition_renderer.hpp"

#include <optional>
#include <string>

namespace bafx::desktop
{

struct ResolvedDisplaySessionPolicy final
{
    std::optional<std::string> displayKey{};
    bool enabled{true};
    bool hdrEnabled{false};
    bafx::config::FramePacing framePacing{
        bafx::config::FramePacing::MatchDisplay};
    bool overridden{false};
    bafx::windows::CompositionOutputPreference outputPreference{
        bafx::windows::CompositionOutputPreference::ConservativeSdr};
    std::optional<bafx::core::MonotonicTime> fixedFramePeriod{};
};

[[nodiscard]] std::optional<bafx::core::MonotonicTime>
fixedFramePacingPeriod(bafx::config::FramePacing pacing) noexcept;

[[nodiscard]] ResolvedDisplaySessionPolicy resolveDisplaySessionPolicy(
    const bafx::config::Config& config,
    const DisplayTarget& target) noexcept;

}
