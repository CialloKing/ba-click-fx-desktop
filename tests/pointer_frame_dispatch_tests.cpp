#include "test_support.hpp"

#include "pointer_frame_dispatch.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>

using namespace bafx::desktop;
using namespace bafx::fx;
using namespace std::chrono_literals;

namespace
{

constexpr Viewport testViewport{1200U, 800U};
constexpr PointF startPosition{100.0F, 100.0F};
constexpr PointF finalPosition{700.0F, 400.0F};

[[nodiscard]] PointerFramePosition framePosition(
    const PointF position,
    const SimulationTime inputTime,
    const bool insideClient = true)
{
    return PointerFramePosition{position, insideClient, inputTime};
}

[[nodiscard]] PointerFrameTransition transition(
    const PointerFrameTransitionKind kind,
    const SimulationTime inputTime,
    const bool acceptDown = false)
{
    return PointerFrameTransition{kind, acceptDown, inputTime};
}

[[nodiscard]] const Sprite& centerDisk(const FrameSnapshot& frame)
{
    const auto found = std::find_if(
        frame.sprites.begin(),
        frame.sprites.end(),
        [](const Sprite& sprite)
        {
            return sprite.kind == SpriteKind::CenterDisk;
        });
    BAFX_CHECK(found != frame.sprites.end());
    return *found;
}

[[nodiscard]] bool trailContains(
    const FrameSnapshot& frame,
    const PointF position)
{
    return std::any_of(
        frame.trail.begin(),
        frame.trail.end(),
        [position](const TrailPoint& point)
        {
            return point.positionPixels.x == position.x
                && point.positionPixels.y == position.y;
        });
}

}

BAFX_TEST(down_uses_the_unified_final_frame_position)
{
    SimulationRuntime runtime;
    PointerFrameDispatch dispatch{};
    dispatch.transitions.push_back(transition(
        PointerFrameTransitionKind::Down,
        10ms,
        true));
    dispatch.position = framePosition(finalPosition, 16ms);
    dispatch.positionUse = PointerFramePositionUse::Held;

    applyPointerFrame(runtime, testViewport, 20ms, dispatch);
    runtime.advance(21ms);
    const FrameSnapshot frame = runtime.snapshot(testViewport, 21ms);

    BAFX_CHECK(runtime.pointerHeld());
    BAFX_CHECK(centerDisk(frame).centerPixels.x == finalPosition.x);
    BAFX_CHECK(centerDisk(frame).centerPixels.y == finalPosition.y);
}

BAFX_TEST(release_frame_does_not_apply_its_position_as_a_held_move)
{
    SimulationRuntime runtime;
    runtime.pointerDown(startPosition, testViewport, 0ms);
    runtime.advance(1ms);

    PointerFrameDispatch dispatch{};
    dispatch.transitions.push_back(transition(
        PointerFrameTransitionKind::Up,
        20ms));
    dispatch.position = framePosition(finalPosition, 20ms);
    dispatch.positionUse = PointerFramePositionUse::Held;
    applyPointerFrame(runtime, testViewport, 20ms, dispatch);

    const FrameSnapshot frame = runtime.snapshot(testViewport, 21ms);
    BAFX_CHECK(!runtime.pointerHeld());
    BAFX_CHECK(!trailContains(frame, finalPosition));
}

BAFX_TEST(down_and_up_in_one_frame_create_then_release_at_final_position)
{
    SimulationRuntime runtime;
    PointerFrameDispatch dispatch{};
    dispatch.transitions = {
        transition(PointerFrameTransitionKind::Down, 10ms, true),
        transition(PointerFrameTransitionKind::Up, 12ms)};
    dispatch.position = framePosition(finalPosition, 12ms);
    dispatch.positionUse = PointerFramePositionUse::Held;

    applyPointerFrame(runtime, testViewport, 16ms, dispatch);
    runtime.advance(17ms);
    const FrameSnapshot frame = runtime.snapshot(testViewport, 17ms);

    BAFX_CHECK(!runtime.pointerHeld());
    BAFX_CHECK(runtime.instanceCount() == 1U);
    BAFX_CHECK(centerDisk(frame).centerPixels.x == finalPosition.x);
    BAFX_CHECK(centerDisk(frame).centerPixels.y == finalPosition.y);
}

BAFX_TEST(held_frame_moves_the_active_pointer_to_the_final_position)
{
    SimulationRuntime runtime;
    runtime.pointerDown(startPosition, testViewport, 0ms);
    runtime.advance(1ms);

    PointerFrameDispatch dispatch{};
    dispatch.position = framePosition(finalPosition, 15ms);
    dispatch.positionUse = PointerFramePositionUse::Held;
    applyPointerFrame(runtime, testViewport, 16ms, dispatch);

    const FrameSnapshot frame = runtime.snapshot(testViewport, 17ms);
    BAFX_CHECK(runtime.pointerHeld());
    BAFX_CHECK(trailContains(frame, finalPosition));
}

BAFX_TEST(up_is_applied_even_when_the_frame_position_is_missing)
{
    SimulationRuntime runtime;
    runtime.pointerDown(startPosition, testViewport, 0ms);

    PointerFrameDispatch dispatch{};
    dispatch.transitions.push_back(transition(
        PointerFrameTransitionKind::Up,
        16ms));
    dispatch.positionUse = PointerFramePositionUse::Held;
    applyPointerFrame(runtime, testViewport, 16ms, dispatch);

    BAFX_CHECK(!runtime.pointerHeld());
}

BAFX_TEST(free_inside_position_drives_the_opt_in_always_on_trail)
{
    SimulationRuntime runtime;
    runtime.setAlwaysOnTrailEnabled(true, 0ms);

    PointerFrameDispatch first{};
    first.position = framePosition(startPosition, 10ms);
    first.positionUse = PointerFramePositionUse::Free;
    applyPointerFrame(runtime, testViewport, 10ms, first);

    PointerFrameDispatch second{};
    second.position = framePosition(finalPosition, 20ms);
    second.positionUse = PointerFramePositionUse::Free;
    applyPointerFrame(runtime, testViewport, 20ms, second);

    const FrameSnapshot frame = runtime.snapshot(testViewport, 21ms);
    BAFX_CHECK(!runtime.pointerHeld());
    BAFX_CHECK(runtime.instanceCount() == 1U);
    BAFX_CHECK(trailContains(frame, finalPosition));
}

BAFX_TEST(free_outside_position_ends_the_ambient_stroke)
{
    SimulationRuntime runtime;
    runtime.setAlwaysOnTrailEnabled(true, 0ms);

    PointerFrameDispatch inside{};
    inside.position = framePosition(startPosition, 10ms);
    inside.positionUse = PointerFramePositionUse::Free;
    applyPointerFrame(runtime, testViewport, 10ms, inside);

    PointerFrameDispatch outside{};
    outside.position = framePosition(finalPosition, 20ms, false);
    outside.positionUse = PointerFramePositionUse::Free;
    applyPointerFrame(runtime, testViewport, 20ms, outside);

    PointerFrameDispatch reentry{};
    reentry.position = framePosition(finalPosition, 30ms);
    reentry.positionUse = PointerFramePositionUse::Free;
    applyPointerFrame(runtime, testViewport, 30ms, reentry);

    // Re-entry starts a second stroke instead of bridging across the edge.
    BAFX_CHECK(runtime.instanceCount() == 2U);
}

BAFX_TEST(rejected_down_retires_ambient_motion_without_creating_a_click)
{
    SimulationRuntime runtime;
    runtime.setAlwaysOnTrailEnabled(true, 0ms);

    PointerFrameDispatch freeMove{};
    freeMove.position = framePosition(startPosition, 10ms);
    freeMove.positionUse = PointerFramePositionUse::Free;
    applyPointerFrame(runtime, testViewport, 10ms, freeMove);

    PointerFrameDispatch down{};
    down.transitions.push_back(transition(
        PointerFrameTransitionKind::Down,
        20ms,
        false));
    down.position = framePosition(finalPosition, 20ms, false);
    down.positionUse = PointerFramePositionUse::None;
    applyPointerFrame(runtime, testViewport, 20ms, down);

    const FrameSnapshot frame = runtime.snapshot(testViewport, 21ms);
    BAFX_CHECK(!runtime.pointerHeld());
    BAFX_CHECK(runtime.instanceCount() == 1U);
    BAFX_CHECK(frame.sprites.empty());
}

BAFX_TEST(ordered_down_up_down_edges_preserve_both_click_instances)
{
    SimulationRuntime runtime;
    PointerFrameDispatch dispatch{};
    dispatch.transitions = {
        transition(PointerFrameTransitionKind::Down, 10ms, true),
        transition(PointerFrameTransitionKind::Up, 11ms),
        transition(PointerFrameTransitionKind::Down, 12ms, true)};
    dispatch.position = framePosition(finalPosition, 12ms);
    dispatch.positionUse = PointerFramePositionUse::Held;

    applyPointerFrame(runtime, testViewport, 16ms, dispatch);

    BAFX_CHECK(runtime.pointerHeld());
    BAFX_CHECK(runtime.instanceCount() == 2U);
}
