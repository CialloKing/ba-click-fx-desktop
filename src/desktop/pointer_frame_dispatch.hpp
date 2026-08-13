#pragma once

#include "bafx/fx/simulation_runtime.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace bafx::desktop
{

enum class PointerFrameTransitionKind : std::uint8_t
{
    Down,
    Up,
    Cancel
};

struct PointerFrameTransition final
{
    PointerFrameTransitionKind kind{PointerFrameTransitionKind::Cancel};
    // The Windows adapter decides whether a Down owns this overlay. Keeping
    // that policy outside the dispatcher makes this module deterministic.
    bool acceptDown{false};
    bafx::fx::SimulationTime inputTime{};
};

struct PointerFramePosition final
{
    // This position is already mapped and clamped by the Windows boundary.
    bafx::fx::PointF clientPosition{};
    bool insideClient{false};
    bafx::fx::SimulationTime inputTime{};
};

enum class PointerFramePositionUse : std::uint8_t
{
    None,
    Held,
    Free
};

struct PointerFrameDispatch final
{
    std::vector<PointerFrameTransition> transitions{};
    std::optional<PointerFramePosition> position{};
    PointerFramePositionUse positionUse{PointerFramePositionUse::None};
};

// Applies one Unity-style input frame. Edge order is retained for rapid input,
// while all simulation changes share the presented frame's time boundary.
void applyPointerFrame(
    bafx::fx::SimulationRuntime& runtime,
    bafx::fx::Viewport viewport,
    bafx::fx::SimulationTime frameTime,
    const PointerFrameDispatch& dispatch);

}
