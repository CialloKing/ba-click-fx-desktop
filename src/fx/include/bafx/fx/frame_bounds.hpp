#pragma once

#include "bafx/core/types.hpp"
#include "bafx/fx/simulation.hpp"

#include <cstdint>
#include <optional>

namespace bafx::fx
{

enum class FrameBoundsStatus : std::uint8_t
{
    Ok,
    Empty,
    Invalid,
    IntegerOverflow
};

struct FrameVisualBoundsResult
{
    bafx::core::RectI bounds{};
    FrameBoundsStatus status{FrameBoundsStatus::Empty};
};

// Bounds are in the snapshot's local top-left pixel space. The result is
// conservative: it includes rotated sprite quads and the renderer's maximum
// supported TrailRenderer miter extension.
[[nodiscard]] FrameVisualBoundsResult visualBounds(
    const FrameSnapshot& snapshot) noexcept;

[[nodiscard]] std::optional<bafx::core::RectI> uniteVisualBounds(
    std::optional<bafx::core::RectI> previous,
    std::optional<bafx::core::RectI> current) noexcept;

}
