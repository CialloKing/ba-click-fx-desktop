#pragma once

#include "bafx/fx/simulation_runtime.hpp"

#include <cstdint>
#include <optional>

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

struct PointerFrameButtons final
{
    bool down{false};
    bool held{false};
    bool up{false};
    bool cancel{false};
    bool acceptDown{false};
    bafx::fx::SimulationTime downInputTime{};
};

enum class PointerFramePositionUse : std::uint8_t
{
    None,
    Held,
    Free
};

struct PointerFrameDispatch final
{
    PointerFrameButtons buttons{};
    std::optional<PointerFramePosition> position{};
    PointerFramePositionUse positionUse{PointerFramePositionUse::None};
};

// Reduces lossless native transitions to Unity Legacy Input's per-frame flags.
void mergePointerFrameTransition(
    PointerFrameButtons& buttons,
    const PointerFrameTransition& transition) noexcept;

// Applies one Unity-style input frame in the script's Down -> Held -> Up order.
void applyPointerFrame(
    bafx::fx::SimulationRuntime& runtime,
    bafx::fx::Viewport viewport,
    bafx::fx::SimulationTime frameTime,
    const PointerFrameDispatch& dispatch);

}
