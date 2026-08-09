#include "test_support.hpp"

#include "bafx/fx/simulation.hpp"
#include "bafx/fx/simulation_runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <vector>

using namespace bafx::fx;
using namespace std::chrono_literals;

namespace
{

constexpr Viewport goldenViewport{1950, 1097};
constexpr PointF goldenCenter{975.0F, 548.5F};

[[nodiscard]] std::size_t countKind(
    const FrameSnapshot& frame,
    const SpriteKind kind)
{
    return static_cast<std::size_t>(std::count_if(
        frame.sprites.begin(),
        frame.sprites.end(),
        [kind](const Sprite& sprite)
        {
            return sprite.kind == kind;
        }));
}

[[nodiscard]] const Sprite& firstKind(
    const FrameSnapshot& frame,
    const SpriteKind kind)
{
    const auto sprite = std::find_if(
        frame.sprites.begin(),
        frame.sprites.end(),
        [kind](const Sprite& candidate)
        {
            return candidate.kind == kind;
        });
    BAFX_CHECK(sprite != frame.sprites.end());
    return *sprite;
}

[[nodiscard]] std::vector<const Sprite*> spritesOfKind(
    const FrameSnapshot& frame,
    const SpriteKind kind)
{
    std::vector<const Sprite*> matches;
    for (const Sprite& sprite : frame.sprites)
    {
        if (sprite.kind == kind)
        {
            matches.push_back(&sprite);
        }
    }
    return matches;
}

}

BAFX_TEST(unity_click_timeline_has_expected_system_counts)
{
    Simulation simulation;
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);

    auto frame = simulation.snapshot(goldenViewport, 50ms);
    BAFX_CHECK(countKind(frame, SpriteKind::CenterDisk) == 1U);
    BAFX_CHECK(countKind(frame, SpriteKind::DissolveRing) == 2U);
    BAFX_CHECK(countKind(frame, SpriteKind::Triangle) == 4U);

    frame = simulation.snapshot(goldenViewport, 250ms);
    BAFX_CHECK(countKind(frame, SpriteKind::CenterDisk) == 0U);
    BAFX_CHECK(countKind(frame, SpriteKind::DissolveRing) == 2U);
    BAFX_CHECK(countKind(frame, SpriteKind::Triangle) == 4U);

    frame = simulation.snapshot(goldenViewport, 650ms);
    BAFX_CHECK(countKind(frame, SpriteKind::CenterDisk) == 0U);
    BAFX_CHECK(countKind(frame, SpriteKind::DissolveRing) == 0U);
}

BAFX_TEST(click_triangle_atlas_frames_are_sampled_per_particle)
{
    bool foundNonAlternatingFrames = false;
    for (std::uint64_t seed = 1U; seed <= 16U; ++seed)
    {
        Simulation simulation(seed);
        simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
        const FrameSnapshot frame = simulation.snapshot(goldenViewport, 50ms);
        const auto triangles = spritesOfKind(
            frame,
            SpriteKind::Triangle);
        BAFX_CHECK(triangles.size() == 4U);

        const bool fixedAlternation = triangles[0]->atlasFrame == 0U
            && triangles[1]->atlasFrame == 1U
            && triangles[2]->atlasFrame == 0U
            && triangles[3]->atlasFrame == 1U;
        foundNonAlternatingFrames = foundNonAlternatingFrames || !fixedAlternation;
        for (const Sprite* triangle : triangles)
        {
            BAFX_CHECK(triangle->atlasFrame <= 1U);
        }
    }
    BAFX_CHECK(foundNonAlternatingFrames);
}

BAFX_TEST(dissolve_ring_custom_data_follows_the_unity_particle_update_phase)
{
    Simulation simulation;
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);

    constexpr std::array sampleTimes{
        50ms,
        100ms,
        110ms,
        120ms,
        130ms,
        180ms,
        250ms,
        450ms};
    constexpr std::array expectedThresholds{
        1.0F,
        0.62384260F,
        0.50000006F,
        0.37615746F,
        0.25925934F,
        0.03993884F,
        0.29346901F,
        0.78884792F};

    for (std::size_t index = 0U; index < sampleTimes.size(); ++index)
    {
        const auto frame = simulation.snapshot(goldenViewport, sampleTimes[index]);
        const Sprite& ring = firstKind(frame, SpriteKind::DissolveRing);
        BAFX_CHECK_NEAR(ring.dissolveThreshold, expectedThresholds[index], 1.0e-6F);
        BAFX_CHECK(ring.contributesBloom);
    }
}

BAFX_TEST(unity_hermite_size_curves_match_serialized_samples)
{
    Simulation simulation;
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);

    const auto diskFrame = simulation.snapshot(goldenViewport, 100ms);
    BAFX_CHECK_NEAR(
        firstKind(diskFrame, SpriteKind::CenterDisk).sizePixels,
        110.53641F,
        1.0e-3F);

    constexpr std::array expectedRingSizesAt250ms{135.67345F, 120.57119F};
    constexpr std::array expectedRingSizesAt450ms{157.33611F, 139.82249F};
    const auto ringFrameAt250ms = simulation.snapshot(goldenViewport, 250ms);
    const auto ringsAt250ms = spritesOfKind(ringFrameAt250ms, SpriteKind::DissolveRing);
    const auto ringFrameAt450ms = simulation.snapshot(goldenViewport, 450ms);
    const auto ringsAt450ms = spritesOfKind(ringFrameAt450ms, SpriteKind::DissolveRing);
    BAFX_CHECK(ringsAt250ms.size() == expectedRingSizesAt250ms.size());
    BAFX_CHECK(ringsAt450ms.size() == expectedRingSizesAt450ms.size());
    for (std::size_t index = 0U; index < expectedRingSizesAt250ms.size(); ++index)
    {
        BAFX_CHECK_NEAR(
            ringsAt250ms[index]->sizePixels,
            expectedRingSizesAt250ms[index],
            1.0e-3F);
        BAFX_CHECK_NEAR(
            ringsAt450ms[index]->sizePixels,
            expectedRingSizesAt450ms[index],
            1.0e-3F);
    }

    constexpr std::array expectedTriangleSizesAt250ms{
        24.46440F,
        22.49270F,
        26.69940F,
        28.54280F};
    constexpr std::array expectedTriangleSizesAt450ms{
        16.89060F,
        16.18470F,
        17.29080F,
        20.57250F};
    const auto trianglesAt250ms = spritesOfKind(ringFrameAt250ms, SpriteKind::Triangle);
    const auto trianglesAt450ms = spritesOfKind(ringFrameAt450ms, SpriteKind::Triangle);
    BAFX_CHECK(trianglesAt250ms.size() == expectedTriangleSizesAt250ms.size());
    BAFX_CHECK(trianglesAt450ms.size() == expectedTriangleSizesAt450ms.size());
    for (std::size_t index = 0U; index < expectedTriangleSizesAt250ms.size(); ++index)
    {
        BAFX_CHECK_NEAR(
            trianglesAt250ms[index]->sizePixels,
            expectedTriangleSizesAt250ms[index],
            1.0e-3F);
        BAFX_CHECK_NEAR(
            trianglesAt450ms[index]->sizePixels,
            expectedTriangleSizesAt450ms[index],
            1.0e-3F);
    }
}

BAFX_TEST(unity_ring_rotation_integrates_the_serialized_two_curves)
{
    Simulation simulation;
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);

    const auto initialFrame = simulation.snapshot(goldenViewport, 1ns);
    const auto frameAt300ms = simulation.snapshot(goldenViewport, 300ms);
    const auto frameAt600ms = simulation.snapshot(goldenViewport, 600ms);
    const auto initialRings = spritesOfKind(initialFrame, SpriteKind::DissolveRing);
    const auto ringsAt300ms = spritesOfKind(frameAt300ms, SpriteKind::DissolveRing);
    const auto ringsAt600ms = spritesOfKind(frameAt600ms, SpriteKind::DissolveRing);
    BAFX_CHECK(initialRings.size() == 2U);
    BAFX_CHECK(ringsAt300ms.size() == initialRings.size());
    BAFX_CHECK(ringsAt600ms.size() == initialRings.size());

    constexpr std::array expectedAt300ms{2.8539987F, 2.5977697F};
    constexpr std::array expectedAt600ms{4.7268543F, 3.9370995F};
    for (std::size_t index = 0U; index < initialRings.size(); ++index)
    {
        BAFX_CHECK_NEAR(
            ringsAt300ms[index]->rotationRadians
                - initialRings[index]->rotationRadians,
            expectedAt300ms[index],
            2.0e-5F);
        BAFX_CHECK_NEAR(
            ringsAt600ms[index]->rotationRadians
                - initialRings[index]->rotationRadians,
            expectedAt600ms[index],
            2.0e-5F);
    }
}

BAFX_TEST(unity_active_color_space_is_applied_after_serialized_color_curves)
{
    Simulation simulation;
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);

    const auto diskFrame = simulation.snapshot(goldenViewport, 10ms);
    const Sprite& disk = firstKind(diskFrame, SpriteKind::CenterDisk);
    BAFX_CHECK_NEAR(disk.color.r, 1.0F, 1.0e-6F);
    BAFX_CHECK_NEAR(disk.color.g, 1.0F, 1.0e-6F);
    BAFX_CHECK_NEAR(disk.color.b, 1.0F, 1.0e-6F);
    BAFX_CHECK_NEAR(disk.color.a, 1.0F, 1.0e-6F);

    const auto ringFrame = simulation.snapshot(goldenViewport, 250ms);
    const Sprite& ring = firstKind(ringFrame, SpriteKind::DissolveRing);
    BAFX_CHECK_NEAR(ring.color.r, 0.23641479F, 1.0e-6F);
    BAFX_CHECK_NEAR(ring.color.g, 0.54607397F, 1.0e-6F);
    BAFX_CHECK_NEAR(ring.color.b, 1.0F, 1.0e-6F);

    constexpr std::array expectedColors{
        ColorF{0.03321951F, 0.14411548F, 0.25064608F, 0.38006002F},
        ColorF{0.03322063F, 0.14413354F, 0.25064608F, 0.54815209F},
        ColorF{0.03321799F, 0.14409056F, 0.25064608F, 0.14858568F},
        ColorF{0.03322067F, 0.14413428F, 0.25064608F, 0.55533624F}};
    const auto triangles = spritesOfKind(ringFrame, SpriteKind::Triangle);
    BAFX_CHECK(triangles.size() == expectedColors.size());
    for (std::size_t index = 0U; index < expectedColors.size(); ++index)
    {
        BAFX_CHECK_NEAR(triangles[index]->color.r, expectedColors[index].r, 1.0e-6F);
        BAFX_CHECK_NEAR(triangles[index]->color.g, expectedColors[index].g, 1.0e-6F);
        BAFX_CHECK_NEAR(triangles[index]->color.b, expectedColors[index].b, 1.0e-6F);
        BAFX_CHECK_NEAR(triangles[index]->color.a, expectedColors[index].a, 1.0e-6F);
    }
}

BAFX_TEST(click_triangles_use_independent_random_angles_and_zero_rotation)
{
    Simulation simulation;
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
    const auto frame = simulation.snapshot(goldenViewport, 50ms);
    const auto triangles = spritesOfKind(frame, SpriteKind::Triangle);
    // These coordinates lock the native deterministic stream only. Unity's
    // engine-specific random stream is compared with layout-insensitive metrics.
    constexpr std::array expectedCenters{
        PointF{1018.42676F, 519.33691F},
        PointF{986.81482F, 497.72739F},
        PointF{998.98273F, 595.01849F},
        PointF{1001.46710F, 593.26154F}};

    BAFX_CHECK(triangles.size() == expectedCenters.size());
    for (std::size_t index = 0U; index < expectedCenters.size(); ++index)
    {
        BAFX_CHECK_NEAR(triangles[index]->centerPixels.x, expectedCenters[index].x, 2.0e-3F);
        BAFX_CHECK_NEAR(triangles[index]->centerPixels.y, expectedCenters[index].y, 2.0e-3F);
        BAFX_CHECK_NEAR(triangles[index]->rotationRadians, 0.0F, 1.0e-7F);
    }
}

BAFX_TEST(triangles_preserve_crisp_hdr_but_never_seed_bloom)
{
    Simulation simulation;
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
    const auto frame = simulation.snapshot(goldenViewport, 130ms);

    for (const Sprite& sprite : frame.sprites)
    {
        if (sprite.kind == SpriteKind::Triangle)
        {
            BAFX_CHECK_NEAR(sprite.artisticIntensity, 5.992157F, 1.0e-6F);
            BAFX_CHECK(!sprite.contributesBloom);
            BAFX_CHECK(sprite.renderQueue == 4550);
        }
    }
}

BAFX_TEST(fixed_ui_projection_round_trips_the_center)
{
    Simulation simulation;
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
    const auto frame = simulation.snapshot(goldenViewport, 50ms);
    const auto disk = std::find_if(
        frame.sprites.begin(),
        frame.sprites.end(),
        [](const Sprite& sprite)
        {
            return sprite.kind == SpriteKind::CenterDisk;
        });
    BAFX_CHECK(disk != frame.sprites.end());
    BAFX_CHECK_NEAR(disk->centerPixels.x, goldenCenter.x, 1.0e-4F);
    BAFX_CHECK_NEAR(disk->centerPixels.y, goldenCenter.y, 1.0e-4F);
}

BAFX_TEST(drag_uses_world_distance_for_trail_and_particles)
{
    Simulation simulation;
    simulation.pointerDown(PointF{100.0F, 100.0F}, goldenViewport, 0ns);
    simulation.pointerMove(PointF{600.0F, 100.0F}, goldenViewport, 50ms);
    simulation.advance(50ms);
    const auto frame = simulation.snapshot(goldenViewport, 50ms);

    BAFX_CHECK(frame.trail.size() >= 2U);
    BAFX_CHECK(countKind(frame, SpriteKind::Triangle) > 4U);
    BAFX_CHECK_NEAR(frame.trailWidthPixels, 2.7425F, 1.0e-4F);
}

BAFX_TEST(delayed_pointer_move_is_not_dropped_after_simulation_advance)
{
    Simulation simulation;
    simulation.pointerDown(PointF{100.0F, 100.0F}, goldenViewport, 0ns);
    simulation.advance(100ms);

    simulation.pointerMove(PointF{600.0F, 100.0F}, goldenViewport, 50ms);
    const auto frame = simulation.snapshot(goldenViewport, 100ms);

    BAFX_CHECK(frame.trail.size() >= 2U);
    BAFX_CHECK(countKind(frame, SpriteKind::Triangle) == 8U);
}

BAFX_TEST(pointer_move_time_rollback_clamps_to_the_previous_sample)
{
    Simulation monotonic;
    Simulation rolledBack;
    constexpr PointF start{100.0F, 100.0F};
    constexpr PointF smallMove{101.0F, 100.0F};
    constexpr PointF longMove{600.0F, 100.0F};

    monotonic.pointerDown(start, goldenViewport, 0ns);
    rolledBack.pointerDown(start, goldenViewport, 0ns);
    monotonic.pointerMove(smallMove, goldenViewport, 100ms);
    rolledBack.pointerMove(smallMove, goldenViewport, 100ms);
    monotonic.pointerMove(longMove, goldenViewport, 100ms);
    rolledBack.pointerMove(longMove, goldenViewport, 50ms);

    const auto monotonicFrame = monotonic.snapshot(goldenViewport, 100ms);
    const auto rolledBackFrame = rolledBack.snapshot(goldenViewport, 100ms);
    const auto monotonicTriangles = spritesOfKind(
        monotonicFrame,
        SpriteKind::Triangle);
    const auto rolledBackTriangles = spritesOfKind(
        rolledBackFrame,
        SpriteKind::Triangle);
    BAFX_CHECK(monotonicTriangles.size() == rolledBackTriangles.size());
    for (std::size_t index = 0U; index < monotonicTriangles.size(); ++index)
    {
        BAFX_CHECK_NEAR(
            monotonicTriangles[index]->centerPixels.x,
            rolledBackTriangles[index]->centerPixels.x,
            1.0e-5F);
        BAFX_CHECK_NEAR(
            monotonicTriangles[index]->centerPixels.y,
            rolledBackTriangles[index]->centerPixels.y,
            1.0e-5F);
        BAFX_CHECK_NEAR(
            monotonicTriangles[index]->sizePixels,
            rolledBackTriangles[index]->sizePixels,
            1.0e-5F);
    }
}

BAFX_TEST(drag_particle_birth_times_are_interpolated_along_the_input_segment)
{
    Simulation simulation;
    simulation.pointerDown(PointF{100.0F, 100.0F}, goldenViewport, 0ns);
    simulation.pointerMove(PointF{600.0F, 100.0F}, goldenViewport, 1000ms);

    const auto frame = simulation.snapshot(goldenViewport, 1000ms);
    const std::size_t visibleTriangles = countKind(frame, SpriteKind::Triangle);
    BAFX_CHECK(visibleTriangles >= 1U);
    BAFX_CHECK(visibleTriangles <= 2U);
}

BAFX_TEST(pointer_cancel_keeps_the_current_trail_until_it_naturally_expires)
{
    Simulation simulation;
    simulation.pointerDown(PointF{100.0F, 100.0F}, goldenViewport, 0ns);
    simulation.pointerMove(PointF{600.0F, 100.0F}, goldenViewport, 50ms);
    BAFX_CHECK(simulation.pointerHeld());
    BAFX_CHECK(!simulation.snapshot(goldenViewport, 50ms).trail.empty());

    simulation.pointerCancel(60ms);
    const auto frame = simulation.snapshot(goldenViewport, 60ms);
    BAFX_CHECK(!simulation.pointerHeld());
    BAFX_CHECK(!frame.trail.empty());
    BAFX_CHECK(countKind(frame, SpriteKind::CenterDisk) == 1U);
    BAFX_CHECK(countKind(frame, SpriteKind::DissolveRing) == 2U);
    BAFX_CHECK(countKind(frame, SpriteKind::Triangle) >= 4U);

    simulation.advance(351ms);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 351ms).trail.empty());
}

BAFX_TEST(trail_length_multiplier_changes_the_simulated_retention_window)
{
    Simulation simulation;
    simulation.setTrailLengthMultiplier(2.0F);
    simulation.pointerDown(PointF{100.0F, 100.0F}, goldenViewport, 0ns);
    simulation.pointerMove(PointF{600.0F, 100.0F}, goldenViewport, 50ms);
    simulation.pointerUp(60ms);

    simulation.advance(400ms);
    BAFX_CHECK(!simulation.snapshot(goldenViewport, 400ms).trail.empty());

    simulation.advance(700ms);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 700ms).trail.empty());

    simulation.setTrailLengthMultiplier(0.0F);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 700ms).trail.empty());
}

BAFX_TEST(pointer_cancel_enters_the_same_rendered_frame_cleanup_as_release)
{
    Simulation simulation;
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
    simulation.pointerCancel(10ms);

    for (std::uint32_t frame = 0U; frame < 60U; ++frame)
    {
        simulation.onFrameRendered();
    }
    BAFX_CHECK(!simulation.active());
}

BAFX_TEST(release_waits_sixty_rendered_frames_before_clearing)
{
    Simulation simulation;
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
    simulation.pointerUp(10ms);

    // Simulation updates alone do not prove that the compositor presented a frame.
    for (std::uint32_t update = 0U; update < 100U; ++update)
    {
        simulation.advance(10ms + std::chrono::milliseconds(update + 1U));
    }
    BAFX_CHECK(simulation.active());

    for (std::uint32_t frame = 0U; frame < 59U; ++frame)
    {
        BAFX_CHECK(simulation.snapshot(goldenViewport, 110ms).active);
        simulation.onFrameRendered();
    }
    BAFX_CHECK(simulation.active());

    // The 60th frame is still drawable; cleanup happens only after it succeeds.
    BAFX_CHECK(simulation.snapshot(goldenViewport, 110ms).active);
    simulation.onFrameRendered();
    BAFX_CHECK(!simulation.active());
    BAFX_CHECK(!simulation.snapshot(goldenViewport, 110ms).active);
}

BAFX_TEST(rapid_clicks_keep_released_effects_alive)
{
    SimulationRuntime runtime;
    constexpr PointF firstClick{300.0F, 400.0F};
    constexpr PointF secondClick{900.0F, 600.0F};

    runtime.pointerDown(firstClick, goldenViewport, 0ns);
    runtime.pointerUp(10ms);
    runtime.pointerDown(secondClick, goldenViewport, 50ms);

    const auto frame = runtime.snapshot(goldenViewport, 100ms);
    BAFX_CHECK(runtime.instanceCount() == 2U);
    BAFX_CHECK(runtime.pointerHeld());
    BAFX_CHECK(countKind(frame, SpriteKind::CenterDisk) == 2U);
    BAFX_CHECK(countKind(frame, SpriteKind::DissolveRing) == 4U);
    BAFX_CHECK(countKind(frame, SpriteKind::Triangle) == 8U);

    const auto disks = spritesOfKind(frame, SpriteKind::CenterDisk);
    BAFX_CHECK_NEAR(disks[0]->centerPixels.x, firstClick.x, 1.0e-3F);
    BAFX_CHECK_NEAR(disks[1]->centerPixels.x, secondClick.x, 1.0e-3F);
}

BAFX_TEST(overlapping_drag_strokes_remain_independent)
{
    SimulationRuntime runtime;

    runtime.pointerDown(PointF{100.0F, 100.0F}, goldenViewport, 0ns);
    runtime.pointerMove(PointF{400.0F, 100.0F}, goldenViewport, 40ms);
    runtime.pointerUp(50ms);
    runtime.pointerDown(PointF{700.0F, 300.0F}, goldenViewport, 60ms);
    runtime.pointerMove(PointF{1000.0F, 300.0F}, goldenViewport, 100ms);

    const auto frame = runtime.snapshot(goldenViewport, 100ms);
    BAFX_CHECK(frame.trailStrokes.size() == 2U);
    BAFX_CHECK(frame.trailStrokes[0].points.size() >= 2U);
    BAFX_CHECK(frame.trailStrokes[1].points.size() >= 2U);
    BAFX_CHECK_NEAR(
        frame.trailStrokes[0].points.back().positionPixels.x,
        400.0F,
        1.0e-3F);
    BAFX_CHECK_NEAR(
        frame.trailStrokes[1].points.front().positionPixels.x,
        700.0F,
        1.0e-3F);
    BAFX_CHECK_NEAR(frame.trail.front().positionPixels.x, 700.0F, 1.0e-3F);
}

BAFX_TEST(released_effect_instances_expire_independently)
{
    SimulationRuntime runtime;
    runtime.pointerDown(PointF{300.0F, 400.0F}, goldenViewport, 0ns);
    runtime.pointerUp(10ms);

    for (std::uint32_t frame = 0U; frame < 30U; ++frame)
    {
        runtime.onFrameRendered();
    }

    runtime.pointerDown(PointF{900.0F, 600.0F}, goldenViewport, 50ms);
    runtime.pointerUp(60ms);
    for (std::uint32_t frame = 0U; frame < 29U; ++frame)
    {
        runtime.onFrameRendered();
    }
    BAFX_CHECK(runtime.instanceCount() == 2U);

    runtime.onFrameRendered();
    BAFX_CHECK(runtime.instanceCount() == 1U);
    BAFX_CHECK(runtime.active());

    for (std::uint32_t frame = 0U; frame < 30U; ++frame)
    {
        runtime.onFrameRendered();
    }
    BAFX_CHECK(runtime.instanceCount() == 0U);
    BAFX_CHECK(!runtime.active());
}

BAFX_TEST(duplicate_down_does_not_restart_the_held_effect)
{
    SimulationRuntime runtime;
    runtime.pointerDown(PointF{300.0F, 400.0F}, goldenViewport, 0ns);
    runtime.pointerDown(PointF{900.0F, 600.0F}, goldenViewport, 50ms);

    const auto frame = runtime.snapshot(goldenViewport, 100ms);
    BAFX_CHECK(runtime.instanceCount() == 1U);
    BAFX_CHECK(countKind(frame, SpriteKind::CenterDisk) == 1U);
    BAFX_CHECK_NEAR(
        firstKind(frame, SpriteKind::CenterDisk).centerPixels.x,
        300.0F,
        1.0e-3F);
}

BAFX_TEST(explicit_runtime_seed_replays_the_same_particle_state)
{
    SimulationRuntime first(20260716U);
    SimulationRuntime second(20260716U);
    first.pointerDown(goldenCenter, goldenViewport, 0ns);
    second.pointerDown(goldenCenter, goldenViewport, 0ns);

    const FrameSnapshot firstFrame = first.snapshot(goldenViewport, 130ms);
    const FrameSnapshot secondFrame = second.snapshot(goldenViewport, 130ms);
    BAFX_CHECK(firstFrame.sprites.size() == secondFrame.sprites.size());
    for (std::size_t index = 0U; index < firstFrame.sprites.size(); ++index)
    {
        const Sprite& firstSprite = firstFrame.sprites[index];
        const Sprite& secondSprite = secondFrame.sprites[index];
        BAFX_CHECK(firstSprite.kind == secondSprite.kind);
        BAFX_CHECK_NEAR(
            firstSprite.centerPixels.x,
            secondSprite.centerPixels.x,
            1.0e-7F);
        BAFX_CHECK_NEAR(
            firstSprite.centerPixels.y,
            secondSprite.centerPixels.y,
            1.0e-7F);
        BAFX_CHECK(firstSprite.atlasFrame == secondSprite.atlasFrame);
    }
}
