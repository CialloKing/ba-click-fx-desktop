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

void addTransition(
    PointerFrameDispatch& dispatch,
    const PointerFrameTransitionKind kind,
    const SimulationTime inputTime,
    const bool acceptDown = false)
{
    mergePointerFrameTransition(
        dispatch.buttons,
        transition(kind, inputTime, acceptDown));
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
    addTransition(
        dispatch,
        PointerFrameTransitionKind::Down,
        10ms,
        true);
    dispatch.buttons.held = true;
    dispatch.position = framePosition(finalPosition, 16ms);
    dispatch.positionUse = PointerFramePositionUse::Held;

    applyPointerFrame(runtime, testViewport, 20ms, dispatch);
    runtime.advance(21ms);
    const FrameSnapshot frame = runtime.snapshot(testViewport, 21ms);

    BAFX_CHECK(runtime.pointerHeld());
    BAFX_CHECK(centerDisk(frame).centerPixels.x == finalPosition.x);
    BAFX_CHECK(centerDisk(frame).centerPixels.y == finalPosition.y);
}

BAFX_TEST(up_only_frame_does_not_apply_its_position_as_a_held_move)
{
    SimulationRuntime runtime;
    runtime.pointerDown(startPosition, testViewport, 0ms);
    runtime.advance(1ms);

    PointerFrameDispatch dispatch{};
    addTransition(
        dispatch,
        PointerFrameTransitionKind::Up,
        20ms);
    dispatch.position = framePosition(finalPosition, 20ms);
    dispatch.positionUse = PointerFramePositionUse::None;
    applyPointerFrame(runtime, testViewport, 20ms, dispatch);

    const FrameSnapshot frame = runtime.snapshot(testViewport, 21ms);
    BAFX_CHECK(!runtime.pointerHeld());
    BAFX_CHECK(!trailContains(frame, finalPosition));
}

BAFX_TEST(down_and_up_in_one_frame_create_then_release_at_final_position)
{
    SimulationRuntime runtime;
    PointerFrameDispatch dispatch{};
    addTransition(
        dispatch,
        PointerFrameTransitionKind::Down,
        10ms,
        true);
    addTransition(dispatch, PointerFrameTransitionKind::Up, 12ms);
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
    dispatch.buttons.held = true;
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
    addTransition(
        dispatch,
        PointerFrameTransitionKind::Up,
        16ms);
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
    addTransition(
        down,
        PointerFrameTransitionKind::Down,
        20ms,
        false);
    down.buttons.held = true;
    down.position = framePosition(finalPosition, 20ms, false);
    down.positionUse = PointerFramePositionUse::None;
    applyPointerFrame(runtime, testViewport, 20ms, down);

    const FrameSnapshot frame = runtime.snapshot(testViewport, 21ms);
    BAFX_CHECK(!runtime.pointerHeld());
    BAFX_CHECK(runtime.instanceCount() == 1U);
    BAFX_CHECK(frame.sprites.empty());
}

BAFX_TEST(mixed_down_ownership_aggregates_to_one_accepted_down)
{
    PointerFrameDispatch dispatch{};
    addTransition(
        dispatch,
        PointerFrameTransitionKind::Down,
        8ms,
        false);
    addTransition(
        dispatch,
        PointerFrameTransitionKind::Down,
        10ms,
        true);
    addTransition(
        dispatch,
        PointerFrameTransitionKind::Down,
        12ms,
        true);

    BAFX_CHECK(dispatch.buttons.down);
    BAFX_CHECK(dispatch.buttons.acceptDown);
    BAFX_CHECK(dispatch.buttons.downInputTime == 10ms);
}

BAFX_TEST(down_up_down_edges_follow_unity_aggregated_frame_order)
{
    SimulationRuntime runtime;
    PointerFrameDispatch dispatch{};
    addTransition(
        dispatch,
        PointerFrameTransitionKind::Down,
        10ms,
        true);
    addTransition(dispatch, PointerFrameTransitionKind::Up, 11ms);
    addTransition(
        dispatch,
        PointerFrameTransitionKind::Down,
        12ms,
        true);
    dispatch.buttons.held = true;
    dispatch.position = framePosition(finalPosition, 12ms);
    dispatch.positionUse = PointerFramePositionUse::Held;

    applyPointerFrame(runtime, testViewport, 16ms, dispatch);
    runtime.advance(17ms);

    // The Player reports Down=true, Held=true, Up=true for this batch. The
    // script therefore creates one effect, positions it, then releases it.
    BAFX_CHECK(!runtime.pointerHeld());
    BAFX_CHECK(runtime.instanceCount() == 1U);
    BAFX_CHECK(
        centerDisk(runtime.snapshot(testViewport, 17ms)).centerPixels.x
        == finalPosition.x);

    PointerFrameDispatch followingHeldFrame{};
    followingHeldFrame.buttons.held = true;
    followingHeldFrame.position = framePosition(startPosition, 20ms);
    followingHeldFrame.positionUse = PointerFramePositionUse::Held;
    applyPointerFrame(runtime, testViewport, 20ms, followingHeldFrame);

    BAFX_CHECK(!runtime.pointerHeld());
    BAFX_CHECK(runtime.instanceCount() == 1U);
    BAFX_CHECK(
        !trailContains(
            runtime.snapshot(testViewport, 20ms),
            startPosition));
}

BAFX_TEST(cancel_is_the_final_hard_boundary_of_an_aggregated_frame)
{
    SimulationRuntime runtime;
    runtime.pointerDown(startPosition, testViewport, 0ms);

    PointerFrameDispatch dispatch{};
    dispatch.buttons.held = true;
    addTransition(
        dispatch,
        PointerFrameTransitionKind::Cancel,
        10ms);
    dispatch.position = framePosition(finalPosition, 10ms);
    dispatch.positionUse = PointerFramePositionUse::Held;

    applyPointerFrame(runtime, testViewport, 10ms, dispatch);

    BAFX_CHECK(!runtime.pointerHeld());
    BAFX_CHECK(runtime.instanceCount() == 1U);
}
