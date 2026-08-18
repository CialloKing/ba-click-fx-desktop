#include "test_support.hpp"

#include "bafx/fx/simulation.hpp"
#include "bafx/fx/simulation_runtime.hpp"
#include "bafx/fx/simulation_timeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <limits>
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

[[nodiscard]] bool trailContainsPoint(
    const FrameSnapshot& frame,
    const PointF point,
    const float tolerance = 1.0e-3F)
{
    return std::any_of(
        frame.trail.begin(),
        frame.trail.end(),
        [point, tolerance](const TrailPoint& candidate)
        {
            return std::abs(candidate.positionPixels.x - point.x) <= tolerance
                && std::abs(candidate.positionPixels.y - point.y) <= tolerance;
        });
}

void checkSpriteEqual(const Sprite& actual, const Sprite& expected)
{
    BAFX_CHECK(actual.kind == expected.kind);
    BAFX_CHECK(actual.centerPixels.x == expected.centerPixels.x);
    BAFX_CHECK(actual.centerPixels.y == expected.centerPixels.y);
    BAFX_CHECK(actual.sizePixels == expected.sizePixels);
    BAFX_CHECK(actual.rotationRadians == expected.rotationRadians);
    BAFX_CHECK(actual.color.r == expected.color.r);
    BAFX_CHECK(actual.color.g == expected.color.g);
    BAFX_CHECK(actual.color.b == expected.color.b);
    BAFX_CHECK(actual.color.a == expected.color.a);
    BAFX_CHECK(actual.artisticIntensity == expected.artisticIntensity);
    BAFX_CHECK(actual.dissolveThreshold == expected.dissolveThreshold);
    BAFX_CHECK(actual.atlasFrame == expected.atlasFrame);
    BAFX_CHECK(actual.renderQueue == expected.renderQueue);
    BAFX_CHECK(actual.contributesBloom == expected.contributesBloom);
    BAFX_CHECK(
        actual.globalScalePivotPixels.x == expected.globalScalePivotPixels.x);
    BAFX_CHECK(
        actual.globalScalePivotPixels.y == expected.globalScalePivotPixels.y);
    BAFX_CHECK(
        actual.scaleCenterWithGlobalScale == expected.scaleCenterWithGlobalScale);
}

}

BAFX_TEST(simulation_timeline_freezes_and_excludes_the_paused_interval)
{
    SimulationTimeline timeline;

    BAFX_CHECK(timeline.fromWallTime(100ms) == 100ms);
    timeline.setPaused(true, 125ms);
    BAFX_CHECK(timeline.paused());
    BAFX_CHECK(timeline.fromWallTime(125ms) == 125ms);
    BAFX_CHECK(timeline.fromWallTime(5s) == 125ms);

    // Repeated control snapshots must not move the original pause boundary.
    timeline.setPaused(true, 2s);
    BAFX_CHECK(timeline.fromWallTime(5s) == 125ms);

    timeline.setPaused(false, 5s);
    BAFX_CHECK(!timeline.paused());
    BAFX_CHECK(timeline.fromWallTime(5s) == 125ms);
    BAFX_CHECK(timeline.fromWallTime(5016ms) == 141ms);
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

BAFX_TEST(core_effects_mode_keeps_non_bloom_effects)
{
    Simulation simulation;
    simulation.setEffectsMode(SimulationEffectsMode::Core);
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
    // snapshot() only projects a future frame; advance the authored first
    // update before treating the next move as a drag segment.
    simulation.advance(50ms);

    const FrameSnapshot frame = simulation.snapshot(goldenViewport, 50ms);
    BAFX_CHECK(simulation.effectsMode() == SimulationEffectsMode::Core);
    BAFX_CHECK(countKind(frame, SpriteKind::CenterDisk) == 1U);
    BAFX_CHECK(countKind(frame, SpriteKind::DissolveRing) == 2U);
    BAFX_CHECK(countKind(frame, SpriteKind::Triangle) == 4U);
    BAFX_CHECK(frame.trail.size() == 1U);

    simulation.pointerMove(PointF{1200.0F, 600.0F}, goldenViewport, 100ms);
    const FrameSnapshot moved = simulation.snapshot(goldenViewport, 150ms);
    BAFX_CHECK(countKind(moved, SpriteKind::Triangle) > 4U);
    BAFX_CHECK(moved.trail.size() >= 2U);
}

BAFX_TEST(core_effects_mode_switch_discards_existing_geometry)
{
    Simulation simulation;
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 50ms).hasDrawableContent());

    simulation.setEffectsMode(SimulationEffectsMode::Core);
    BAFX_CHECK(!simulation.active());
    BAFX_CHECK(!simulation.snapshot(goldenViewport, 50ms).hasDrawableContent());

    simulation.pointerDown(goldenCenter, goldenViewport, 100ms);
    const FrameSnapshot coreFrame = simulation.snapshot(goldenViewport, 150ms);
    BAFX_CHECK(countKind(coreFrame, SpriteKind::DissolveRing) == 2U);
    BAFX_CHECK(countKind(coreFrame, SpriteKind::Triangle) == 4U);
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

BAFX_TEST(click_shard_settings_use_web_reference_pixel_units)
{
    Simulation simulation(20260716U);
    ShardParticleSettings settings{};
    settings.clickCount = 3U;
    settings.clickLifetimeMinMs = 100.0F;
    settings.clickLifetimeMaxMs = 100.0F;
    settings.clickRadius = 0.0F;
    settings.clickSpeedMin = 0.0F;
    settings.clickSpeedMax = 0.0F;
    settings.sizeMin = 54.0F;
    settings.sizeMax = 54.0F;
    simulation.setShardParticleSettings(settings);
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);

    const FrameSnapshot visibleFrame = simulation.snapshot(
        goldenViewport,
        50ms);
    const auto visible = spritesOfKind(
        visibleFrame,
        SpriteKind::Triangle);
    BAFX_CHECK(visible.size() == 3U);
    for (const Sprite* const shard : visible)
    {
        BAFX_CHECK_NEAR(shard->centerPixels.x, goldenCenter.x, 1.0e-4F);
        BAFX_CHECK_NEAR(shard->centerPixels.y, goldenCenter.y, 1.0e-4F);
        BAFX_CHECK(shard->sizePixels > 0.0F);
        BAFX_CHECK_NEAR(
            shard->sizePixels,
            visible.front()->sizePixels,
            1.0e-5F);
    }

    BAFX_CHECK(
        countKind(
            simulation.snapshot(goldenViewport, 150ms),
            SpriteKind::Triangle)
        == 0U);
}

BAFX_TEST(dissolve_ring_custom_data_follows_the_unity_particle_update_phase)
{
    constexpr std::array sampleTimes{
        50ms,
        100ms,
        110ms,
        120ms,
        130ms,
        140ms,
        150ms,
        180ms,
        250ms,
        450ms};
    constexpr std::array expectedThresholds{
        1.0F,
        0.62384260F,
        0.56235534F,
        0.50000006F,
        0.28175008F,
        0.21600008F,
        0.07407415F,
        0.03429154F,
        0.27497348F,
        0.77141333F};

    for (std::size_t index = 0U; index < sampleTimes.size(); ++index)
    {
        // Golden capture creates a fresh prefab and calls Simulate(total) once.
        Simulation advanced;
        advanced.pointerDown(goldenCenter, goldenViewport, 0ns);
        advanced.advance(sampleTimes[index]);
        const auto advancedFrame = advanced.snapshot(
            goldenViewport,
            sampleTimes[index]);

        // Read-only capture callers must observe the same pending simulation
        // without mutating the live state or first calling advance().
        Simulation queried;
        queried.pointerDown(goldenCenter, goldenViewport, 0ns);
        const auto queriedFrame = queried.snapshot(
            goldenViewport,
            sampleTimes[index]);

        const Sprite& advancedRing = firstKind(
            advancedFrame,
            SpriteKind::DissolveRing);
        const Sprite& queriedRing = firstKind(
            queriedFrame,
            SpriteKind::DissolveRing);
        BAFX_CHECK_NEAR(
            advancedRing.dissolveThreshold,
            expectedThresholds[index],
            1.0e-6F);
        BAFX_CHECK_NEAR(
            queriedRing.dissolveThreshold,
            advancedRing.dissolveThreshold,
            0.0F);
        BAFX_CHECK(advancedRing.contributesBloom);
        BAFX_CHECK(queriedRing.contributesBloom);
    }
}

BAFX_TEST(dissolve_ring_read_only_future_snapshot_does_not_advance_live_state)
{
    Simulation simulation;
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);

    const FrameSnapshot futureFrame = simulation.snapshot(
        goldenViewport,
        250ms);
    const FrameSnapshot earlierFrame = simulation.snapshot(
        goldenViewport,
        50ms);
    const Sprite& futureRing = firstKind(
        futureFrame,
        SpriteKind::DissolveRing);
    const Sprite& earlierRing = firstKind(
        earlierFrame,
        SpriteKind::DissolveRing);

    BAFX_CHECK_NEAR(futureRing.dissolveThreshold, 0.27497348F, 1.0e-6F);
    BAFX_CHECK_NEAR(earlierRing.dissolveThreshold, 1.0F, 0.0F);
}

BAFX_TEST(dissolve_ring_substep_count_preserves_unity_float32_boundaries)
{
    constexpr SimulationTime belowBoundary{179999999};
    constexpr SimulationTime atBoundary = 180ms;
    Simulation below;
    Simulation at;
    below.pointerDown(goldenCenter, goldenViewport, 0ns);
    at.pointerDown(goldenCenter, goldenViewport, 0ns);

    const FrameSnapshot belowFrame = below.snapshot(
        goldenViewport,
        belowBoundary);
    const FrameSnapshot atFrame = at.snapshot(
        goldenViewport,
        atBoundary);
    const Sprite& belowRing = firstKind(
        belowFrame,
        SpriteKind::DissolveRing);
    const Sprite& atRing = firstKind(
        atFrame,
        SpriteKind::DissolveRing);

    // Both nanosecond inputs round to the same float32 elapsed value. Unity
    // therefore takes seven 0.03 s-bounded steps at this boundary.
    BAFX_CHECK_NEAR(belowRing.dissolveThreshold, 0.03429154F, 1.0e-6F);
    BAFX_CHECK_NEAR(
        atRing.dissolveThreshold,
        belowRing.dissolveThreshold,
        0.0F);
}

BAFX_TEST(dissolve_ring_custom_data_matches_unity_at_common_refresh_rates)
{
    struct RefreshSample
    {
        std::uint32_t rateHz;
        std::uint32_t frame;
        float expectedThreshold;
    };
    constexpr std::array samples{
        RefreshSample{60U, 3U, 0.9474879F},
        RefreshSample{60U, 15U, 0.3473206F},
        RefreshSample{120U, 6U, 0.8113854F},
        RefreshSample{120U, 30U, 0.3987512F},
        RefreshSample{240U, 12U, 0.7220346F},
        RefreshSample{240U, 60U, 0.4235710F}};

    for (const RefreshSample& sample : samples)
    {
        Simulation simulation;
        simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
        for (std::uint32_t frame = 1U; frame <= sample.frame; ++frame)
        {
            const auto frameTime = std::chrono::duration_cast<SimulationTime>(
                std::chrono::duration<float>(
                    static_cast<float>(frame)
                    / static_cast<float>(sample.rateHz)));
            simulation.advance(frameTime);
        }

        const auto sampleTime = std::chrono::duration_cast<SimulationTime>(
            std::chrono::duration<float>(
                static_cast<float>(sample.frame)
                / static_cast<float>(sample.rateHz)));
        const FrameSnapshot frame = simulation.snapshot(
            goldenViewport,
            sampleTime);
        const Sprite& ring = firstKind(frame, SpriteKind::DissolveRing);
        BAFX_CHECK_NEAR(
            ring.dissolveThreshold,
            sample.expectedThreshold,
            2.0e-6F);
    }
}

BAFX_TEST(dissolve_ring_lifetime_curves_use_the_same_unity_particle_age)
{
    struct RefreshSample
    {
        std::uint32_t rateHz;
        std::uint32_t frame;
        float expectedParticleAgeSeconds;
    };
    constexpr std::array samples{
        RefreshSample{60U, 3U, 0.03333336F},
        RefreshSample{120U, 6U, 0.04166669F},
        RefreshSample{240U, 12U, 0.04583335F}};
    std::array<float, samples.size()> ringSizes{};

    for (std::size_t index = 0U; index < samples.size(); ++index)
    {
        const RefreshSample& sample = samples[index];
        Simulation simulation;
        simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
        for (std::uint32_t frame = 1U; frame <= sample.frame; ++frame)
        {
            const auto frameTime = std::chrono::duration_cast<SimulationTime>(
                std::chrono::duration<float>(
                    static_cast<float>(frame)
                    / static_cast<float>(sample.rateHz)));
            simulation.advance(frameTime);
        }

        Simulation expected;
        expected.pointerDown(goldenCenter, goldenViewport, 0ns);
        expected.advance(1ns);
        const SimulationTime expectedAge = 1ns
            + std::chrono::duration_cast<SimulationTime>(
                std::chrono::duration<float>(
                    sample.expectedParticleAgeSeconds));
        expected.advance(expectedAge);
        const FrameSnapshot expectedFrame = expected.snapshot(
            goldenViewport,
            expectedAge);
        const auto sampleTime = std::chrono::duration_cast<SimulationTime>(
            std::chrono::duration<float>(
                static_cast<float>(sample.frame)
                / static_cast<float>(sample.rateHz)));
        const FrameSnapshot actualFrame = simulation.snapshot(
            goldenViewport,
            sampleTime);
        const Sprite& expectedRing = firstKind(
            expectedFrame,
            SpriteKind::DissolveRing);
        const Sprite& actualRing = firstKind(
            actualFrame,
            SpriteKind::DissolveRing);

        BAFX_CHECK_NEAR(actualRing.sizePixels, expectedRing.sizePixels, 2.0e-5F);
        BAFX_CHECK_NEAR(
            actualRing.rotationRadians,
            expectedRing.rotationRadians,
            2.0e-6F);
        BAFX_CHECK_NEAR(actualRing.color.r, expectedRing.color.r, 2.0e-6F);
        BAFX_CHECK_NEAR(actualRing.color.g, expectedRing.color.g, 2.0e-6F);
        BAFX_CHECK_NEAR(actualRing.color.b, expectedRing.color.b, 2.0e-6F);
        ringSizes[index] = actualRing.sizePixels;
    }

    BAFX_CHECK(ringSizes[0] < ringSizes[1]);
    BAFX_CHECK(ringSizes[1] < ringSizes[2]);
}

BAFX_TEST(click_burst_systems_follow_unity_particle_age_at_common_refresh_rates)
{
    struct RefreshSample
    {
        std::uint32_t rateHz;
        std::uint32_t frame;
        float expectedDiskSize;
        float expectedTriangleSize;
        PointF expectedTriangleCenter;
    };
    constexpr std::array samples{
        RefreshSample{
            60U,
            3U,
            86.96708F,
            6.30301F,
            PointF{1018.88287F, 519.03064F}},
        RefreshSample{
            120U,
            6U,
            93.55600F,
            9.17558F,
            PointF{1019.33899F, 518.72430F}},
        RefreshSample{
            240U,
            12U,
            96.05126F,
            10.69537F,
            PointF{1019.56702F, 518.57117F}}};
    std::array<float, samples.size()> diskSizes{};
    std::array<float, samples.size()> triangleSizes{};

    for (std::size_t index = 0U; index < samples.size(); ++index)
    {
        const RefreshSample& sample = samples[index];
        Simulation simulation;
        simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
        for (std::uint32_t frame = 1U; frame <= sample.frame; ++frame)
        {
            const auto frameTime = std::chrono::duration_cast<SimulationTime>(
                std::chrono::duration<float>(
                    static_cast<float>(frame)
                    / static_cast<float>(sample.rateHz)));
            simulation.advance(frameTime);
        }

        const auto sampleTime = std::chrono::duration_cast<SimulationTime>(
            std::chrono::duration<float>(
                static_cast<float>(sample.frame)
                / static_cast<float>(sample.rateHz)));
        const FrameSnapshot frame = simulation.snapshot(
            goldenViewport,
            sampleTime);

        BAFX_CHECK(countKind(frame, SpriteKind::CenterDisk) == 1U);
        BAFX_CHECK(countKind(frame, SpriteKind::DissolveRing) == 2U);
        BAFX_CHECK(countKind(frame, SpriteKind::Triangle) == 4U);
        const Sprite& disk = firstKind(frame, SpriteKind::CenterDisk);
        const Sprite& triangle = firstKind(frame, SpriteKind::Triangle);
        // These public Sprite values come from the Unity-observed particle
        // ages, so a shared regression cannot update both sides of the check.
        BAFX_CHECK_NEAR(disk.sizePixels, sample.expectedDiskSize, 2.0e-4F);
        BAFX_CHECK_NEAR(
            triangle.centerPixels.x,
            sample.expectedTriangleCenter.x,
            2.0e-4F);
        BAFX_CHECK_NEAR(
            triangle.centerPixels.y,
            sample.expectedTriangleCenter.y,
            2.0e-4F);
        BAFX_CHECK_NEAR(
            triangle.sizePixels,
            sample.expectedTriangleSize,
            2.0e-4F);
        BAFX_CHECK_NEAR(triangle.color.r, 0.25064608F, 1.0e-6F);
        BAFX_CHECK_NEAR(triangle.color.g, 0.25064608F, 1.0e-6F);
        BAFX_CHECK_NEAR(triangle.color.b, 0.25064608F, 1.0e-6F);
        BAFX_CHECK_NEAR(triangle.color.a, 1.0F, 0.0F);
        diskSizes[index] = disk.sizePixels;
        triangleSizes[index] = triangle.sizePixels;
    }

    // All samples end near 50 ms. Their differing sizes come from Unity's
    // first-frame birth boundary, not an absolute 25 ms render delay.
    BAFX_CHECK(diskSizes[0] < diskSizes[1]);
    BAFX_CHECK(diskSizes[1] < diskSizes[2]);
    BAFX_CHECK(triangleSizes[0] < triangleSizes[1]);
    BAFX_CHECK(triangleSizes[1] < triangleSizes[2]);
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

    constexpr std::array expectedRingSizesAt250ms{135.19762F, 120.14832F};
    constexpr std::array expectedRingSizesAt450ms{157.05330F, 139.57118F};
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
        24.524269F,
        22.541700F,
        26.775435F,
        28.604612F};
    constexpr std::array expectedTriangleSizesAt450ms{
        17.153708F,
        16.405443F,
        17.614582F,
        20.851566F};
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

BAFX_TEST(click_burst_future_snapshot_keeps_the_live_particle_age_unchanged)
{
    Simulation queried;
    queried.pointerDown(goldenCenter, goldenViewport, 0ns);
    const FrameSnapshot futureFrame = queried.snapshot(
        goldenViewport,
        250ms);
    const FrameSnapshot earlierFrame = queried.snapshot(
        goldenViewport,
        50ms);

    Simulation expected;
    expected.pointerDown(goldenCenter, goldenViewport, 0ns);
    const FrameSnapshot expectedFrame = expected.snapshot(
        goldenViewport,
        50ms);
    BAFX_CHECK(countKind(futureFrame, SpriteKind::CenterDisk) == 0U);
    BAFX_CHECK(countKind(futureFrame, SpriteKind::DissolveRing) == 2U);
    BAFX_CHECK(countKind(futureFrame, SpriteKind::Triangle) == 4U);
    BAFX_CHECK(countKind(earlierFrame, SpriteKind::CenterDisk) == 1U);
    BAFX_CHECK(countKind(earlierFrame, SpriteKind::DissolveRing) == 2U);
    BAFX_CHECK(countKind(earlierFrame, SpriteKind::Triangle) == 4U);

    const Sprite& earlierDisk = firstKind(
        earlierFrame,
        SpriteKind::CenterDisk);
    const Sprite& expectedDisk = firstKind(
        expectedFrame,
        SpriteKind::CenterDisk);
    BAFX_CHECK_NEAR(earlierDisk.sizePixels, 78.18764F, 2.0e-4F);
    BAFX_CHECK_NEAR(earlierDisk.color.a, 0.9818524F, 1.0e-6F);
    BAFX_CHECK_NEAR(earlierDisk.sizePixels, expectedDisk.sizePixels, 0.0F);
    BAFX_CHECK_NEAR(earlierDisk.color.a, expectedDisk.color.a, 0.0F);

    const auto earlierTriangles = spritesOfKind(
        earlierFrame,
        SpriteKind::Triangle);
    const auto expectedTriangles = spritesOfKind(
        expectedFrame,
        SpriteKind::Triangle);
    BAFX_CHECK(earlierTriangles.size() == expectedTriangles.size());
    BAFX_CHECK_NEAR(
        earlierTriangles.front()->centerPixels.x,
        1018.42676F,
        2.0e-4F);
    BAFX_CHECK_NEAR(
        earlierTriangles.front()->centerPixels.y,
        519.33691F,
        2.0e-4F);
    BAFX_CHECK_NEAR(
        earlierTriangles.front()->sizePixels,
        3.787674F,
        2.0e-4F);
    BAFX_CHECK_NEAR(earlierTriangles.front()->color.a, 1.0F, 0.0F);
    for (std::size_t index = 0U; index < earlierTriangles.size(); ++index)
    {
        BAFX_CHECK_NEAR(
            earlierTriangles[index]->centerPixels.x,
            expectedTriangles[index]->centerPixels.x,
            0.0F);
        BAFX_CHECK_NEAR(
            earlierTriangles[index]->centerPixels.y,
            expectedTriangles[index]->centerPixels.y,
            0.0F);
        BAFX_CHECK_NEAR(
            earlierTriangles[index]->sizePixels,
            expectedTriangles[index]->sizePixels,
            0.0F);
        BAFX_CHECK_NEAR(
            earlierTriangles[index]->color.a,
            expectedTriangles[index]->color.a,
            0.0F);
    }
}

BAFX_TEST(global_scale_keeps_click_shards_synchronized_with_the_click_effect)
{
    Simulation simulation(20260716U);
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);

    const FrameSnapshot original = simulation.snapshot(goldenViewport, 100ms);
    const Sprite& originalRing = firstKind(original, SpriteKind::DissolveRing);
    const auto originalTriangles = spritesOfKind(original, SpriteKind::Triangle);
    BAFX_CHECK(!originalTriangles.empty());
    constexpr std::array scales{0.5F, 2.0F};
    for (const float scale : scales)
    {
        FrameSnapshot scaled = original;
        applyGlobalScale(scaled, scale);
        const Sprite& scaledRing = firstKind(scaled, SpriteKind::DissolveRing);
        BAFX_CHECK_NEAR(scaledRing.centerPixels.x, originalRing.centerPixels.x, 0.0F);
        BAFX_CHECK_NEAR(scaledRing.centerPixels.y, originalRing.centerPixels.y, 0.0F);
        BAFX_CHECK_NEAR(
            scaledRing.sizePixels,
            originalRing.sizePixels * scale,
            1.0e-4F);

        const auto scaledTriangles = spritesOfKind(scaled, SpriteKind::Triangle);
        BAFX_CHECK(originalTriangles.size() == scaledTriangles.size());
        for (std::size_t index = 0U; index < originalTriangles.size(); ++index)
        {
            const PointF pivot = originalTriangles[index]->globalScalePivotPixels;
            BAFX_CHECK(originalTriangles[index]->scaleCenterWithGlobalScale);
            BAFX_CHECK_NEAR(
                scaledTriangles[index]->centerPixels.x - pivot.x,
                (originalTriangles[index]->centerPixels.x - pivot.x) * scale,
                1.0e-4F);
            BAFX_CHECK_NEAR(
                scaledTriangles[index]->centerPixels.y - pivot.y,
                (originalTriangles[index]->centerPixels.y - pivot.y) * scale,
                1.0e-4F);
            BAFX_CHECK_NEAR(
                scaledTriangles[index]->sizePixels,
                originalTriangles[index]->sizePixels * scale,
                1.0e-4F);
        }
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

    constexpr std::array expectedAt300ms{2.8337526F, 2.5802860F};
    constexpr std::array expectedAt600ms{4.7114286F, 3.9294958F};
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
    BAFX_CHECK_NEAR(ring.color.r, 0.24471897F, 1.0e-6F);
    BAFX_CHECK_NEAR(ring.color.g, 0.55270594F, 1.0e-6F);
    BAFX_CHECK_NEAR(ring.color.b, 1.0F, 1.0e-6F);

    constexpr std::array expectedColors{
        ColorF{0.03321987F, 0.14412127F, 0.25064608F, 0.43425328F},
        ColorF{0.03322097F, 0.14413916F, 0.25064608F, 0.60027027F},
        ColorF{0.03321836F, 0.14409673F, 0.25064608F, 0.20563626F},
        ColorF{0.03322102F, 0.14413990F, 0.25064608F, 0.60736549F}};
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

BAFX_TEST(triangles_preserve_crisp_hdr_and_seed_bloom)
{
    Simulation simulation;
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
    const auto frame = simulation.snapshot(goldenViewport, 130ms);

    for (const Sprite& sprite : frame.sprites)
    {
        if (sprite.kind == SpriteKind::Triangle)
        {
            BAFX_CHECK_NEAR(sprite.artisticIntensity, 5.992157F, 1.0e-6F);
            BAFX_CHECK(sprite.contributesBloom);
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
    simulation.advance(1ns);
    simulation.pointerMove(PointF{600.0F, 100.0F}, goldenViewport, 50ms);
    simulation.advance(50ms);
    const auto frame = simulation.snapshot(goldenViewport, 50ms);

    BAFX_CHECK(frame.trail.size() >= 2U);
    BAFX_CHECK(countKind(frame, SpriteKind::Triangle) > 4U);
    BAFX_CHECK_NEAR(frame.trailWidthPixels, 2.7425F, 1.0e-4F);
}

BAFX_TEST(pressed_runtime_survives_empty_frames_before_trail_move)
{
    constexpr PointF start{100.0F, 100.0F};
    constexpr PointF end{600.0F, 100.0F};
    SimulationRuntime runtime;

    runtime.pointerDown(start, goldenViewport, 0ms);
    runtime.advance(1ms);
    // A held mouse can remain stationary for any number of render frames.
    runtime.advance(10ms);

    BAFX_CHECK(runtime.pointerHeld());
    BAFX_CHECK(!runtime.alwaysOnTrailEnabled());

    runtime.pointerMove(end, goldenViewport, 20ms);
    const FrameSnapshot frame = runtime.snapshot(goldenViewport, 20ms);

    BAFX_CHECK(runtime.pointerHeld());
    BAFX_CHECK(frame.trailStrokes.size() == 1U);
    BAFX_CHECK(frame.trail.size() >= 2U);
    BAFX_CHECK_NEAR(frame.trail.front().positionPixels.x, start.x, 1.0e-3F);
    BAFX_CHECK_NEAR(frame.trail.front().positionPixels.y, start.y, 1.0e-3F);
    BAFX_CHECK_NEAR(frame.trail.back().positionPixels.x, end.x, 1.0e-3F);
    BAFX_CHECK_NEAR(frame.trail.back().positionPixels.y, end.y, 1.0e-3F);
}

BAFX_TEST(pointer_moves_before_first_advance_match_unitys_final_update_position)
{
    Simulation simulation;
    constexpr PointF pressed{100.0F, 100.0F};
    constexpr PointF firstFramePosition{600.0F, 100.0F};
    constexpr PointF secondFramePosition{900.0F, 100.0F};

    simulation.pointerDown(pressed, goldenViewport, 0ns);
    simulation.pointerMove(firstFramePosition, goldenViewport, 50ms);
    simulation.advance(50ms);
    FrameSnapshot frame = simulation.snapshot(goldenViewport, 50ms);

    // CreateEffect and SetDragPosition see one Input.mousePosition in the
    // first Unity Update, so the press coordinate never becomes geometry.
    BAFX_CHECK(frame.trail.size() == 1U);
    BAFX_CHECK(!trailContainsPoint(frame, pressed));
    BAFX_CHECK(trailContainsPoint(frame, firstFramePosition));
    BAFX_CHECK(countKind(frame, SpriteKind::Triangle) == 4U);
    BAFX_CHECK_NEAR(
        firstKind(frame, SpriteKind::CenterDisk).centerPixels.x,
        firstFramePosition.x,
        1.0e-4F);
    for (const Sprite* const ring : spritesOfKind(frame, SpriteKind::DissolveRing))
    {
        BAFX_CHECK_NEAR(ring->centerPixels.x, firstFramePosition.x, 1.0e-4F);
    }

    simulation.pointerMove(secondFramePosition, goldenViewport, 100ms);
    frame = simulation.snapshot(goldenViewport, 100ms);
    BAFX_CHECK(frame.trail.size() == 2U);
    BAFX_CHECK(trailContainsPoint(frame, secondFramePosition));
    BAFX_CHECK(countKind(frame, SpriteKind::Triangle) > 4U);
}

BAFX_TEST(trail_vertex_distance_filters_samples_without_tessellating_frame_jumps)
{
    Simulation simulation;
    constexpr PointF start{100.0F, 100.0F};
    constexpr PointF end{600.0F, 100.0F};
    simulation.pointerDown(start, goldenViewport, 0ns);
    simulation.advance(1ns);
    simulation.pointerMove(PointF{104.0F, 100.0F}, goldenViewport, 50ms);
    simulation.pointerMove(end, goldenViewport, 100ms);

    const auto frame = simulation.snapshot(goldenViewport, 100ms);
    BAFX_CHECK(frame.trail.size() == 2U);
    BAFX_CHECK_NEAR(frame.trail.front().positionPixels.x, start.x, 1.0e-4F);
    BAFX_CHECK_NEAR(frame.trail.back().positionPixels.x, end.x, 1.0e-4F);
}

BAFX_TEST(delayed_pointer_move_is_not_dropped_after_simulation_advance)
{
    Simulation simulation;
    simulation.pointerDown(PointF{100.0F, 100.0F}, goldenViewport, 0ns);
    simulation.advance(100ms);

    simulation.pointerMove(PointF{600.0F, 100.0F}, goldenViewport, 50ms);
    const auto boundaryFrame = simulation.snapshot(goldenViewport, 100ms);
    const auto visibleFrame = simulation.snapshot(goldenViewport, 100ms + 1ns);

    BAFX_CHECK(boundaryFrame.trail.size() >= 2U);
    BAFX_CHECK_NEAR(
        boundaryFrame.trail.back().positionPixels.x,
        600.0F,
        1.0e-4F);
    BAFX_CHECK(countKind(boundaryFrame, SpriteKind::Triangle) == 4U);
    BAFX_CHECK(countKind(visibleFrame, SpriteKind::Triangle) == 8U);
}

BAFX_TEST(stationary_updates_bound_the_next_drag_emission_interval)
{
    Simulation advanced;
    Simulation explicitSample;
    constexpr PointF start{100.0F, 100.0F};
    constexpr PointF end{600.0F, 100.0F};

    for (Simulation* const simulation : {&advanced, &explicitSample})
    {
        simulation->pointerDown(start, goldenViewport, 0ns);
        simulation->advance(1ns);
        simulation->advance(100ms);
    }
    explicitSample.pointerMove(start, goldenViewport, 100ms);
    advanced.pointerMove(end, goldenViewport, 116ms);
    explicitSample.pointerMove(end, goldenViewport, 116ms);

    const FrameSnapshot advancedFrame = advanced.snapshot(goldenViewport, 116ms);
    const FrameSnapshot explicitFrame = explicitSample.snapshot(goldenViewport, 116ms);
    const auto advancedTriangles = spritesOfKind(
        advancedFrame,
        SpriteKind::Triangle);
    const auto explicitTriangles = spritesOfKind(
        explicitFrame,
        SpriteKind::Triangle);
    BAFX_CHECK(advancedTriangles.size() > 4U);
    BAFX_CHECK(advancedTriangles.size() == explicitTriangles.size());
    for (std::size_t index = 0U; index < advancedTriangles.size(); ++index)
    {
        BAFX_CHECK_NEAR(
            advancedTriangles[index]->centerPixels.x,
            explicitTriangles[index]->centerPixels.x,
            0.0F);
        BAFX_CHECK_NEAR(
            advancedTriangles[index]->centerPixels.y,
            explicitTriangles[index]->centerPixels.y,
            0.0F);
        BAFX_CHECK_NEAR(
            advancedTriangles[index]->sizePixels,
            explicitTriangles[index]->sizePixels,
            0.0F);
        BAFX_CHECK_NEAR(
            advancedTriangles[index]->color.a,
            explicitTriangles[index]->color.a,
            0.0F);
    }
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
    monotonic.advance(1ns);
    rolledBack.advance(1ns);
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
    simulation.advance(1ns);
    simulation.pointerMove(PointF{600.0F, 100.0F}, goldenViewport, 1000ms);

    const auto frame = simulation.snapshot(goldenViewport, 1000ms);
    const std::size_t visibleTriangles = countKind(frame, SpriteKind::Triangle);
    BAFX_CHECK(visibleTriangles >= 1U);
    BAFX_CHECK(visibleTriangles <= 2U);
}

BAFX_TEST(drag_particle_age_starts_at_the_distance_interpolated_birth_time)
{
    Simulation simulation;
    constexpr PointF start{100.0F, 100.0F};
    constexpr PointF end{220.0F, 100.0F};
    simulation.pointerDown(start, goldenViewport, 0ns);
    simulation.advance(1ns);
    simulation.pointerMove(end, goldenViewport, 30ms);

    const auto frame = simulation.snapshot(goldenViewport, 30ms);
    const auto triangles = spritesOfKind(frame, SpriteKind::Triangle);
    BAFX_CHECK(triangles.size() == 5U);

    // Ring (4) crossed 0.2 world within this post-first-frame movement and
    // owns the remaining fraction of the input interval immediately.
    constexpr float dragEmissionStepWorld = 0.2F;
    const float segmentWorld = 2.0F
        * static_cast<float>(end.x - start.x)
        / static_cast<float>(goldenViewport.height);
    const float interpolation = dragEmissionStepWorld / segmentWorld;
    const Sprite& dragTriangle = *triangles.back();
    constexpr SimulationTime movementStartedAt = 1ns;
    const SimulationTime movementDuration =
        std::chrono::duration_cast<SimulationTime>(30ms) - movementStartedAt;
    const auto bornAtCount = static_cast<SimulationTime::rep>(
        static_cast<double>(movementDuration.count())
        * static_cast<double>(interpolation));
    const SimulationTime expectedAge = movementDuration
        - SimulationTime{bornAtCount};

    Simulation reference;
    reference.pointerDown(start, goldenViewport, 0ns);
    reference.advance(movementStartedAt);
    reference.pointerMove(end, goldenViewport, movementStartedAt);
    const FrameSnapshot referenceFrame = reference.snapshot(
        goldenViewport,
        movementStartedAt + expectedAge);
    const auto referenceTriangles = spritesOfKind(
        referenceFrame,
        SpriteKind::Triangle);
    BAFX_CHECK(referenceTriangles.size() == 5U);
    const Sprite& expectedDragTriangle = *referenceTriangles.back();

    // Moving the same seeded particle at time zero isolates its expected age.
    // The output must then match the particle born at the distance crossing.
    BAFX_CHECK_NEAR(
        dragTriangle.centerPixels.x,
        expectedDragTriangle.centerPixels.x,
        0.0F);
    BAFX_CHECK_NEAR(
        dragTriangle.centerPixels.y,
        expectedDragTriangle.centerPixels.y,
        0.0F);
    BAFX_CHECK_NEAR(
        dragTriangle.sizePixels,
        expectedDragTriangle.sizePixels,
        0.0F);
    BAFX_CHECK_NEAR(
        dragTriangle.color.a,
        expectedDragTriangle.color.a,
        0.0F);
}

BAFX_TEST(runtime_click_time_scale_change_preserves_elapsed_history)
{
    SimulationRuntime runtime;
    runtime.pointerDown(goldenCenter, goldenViewport, 0ns);

    const FrameSnapshot beforeChange = runtime.snapshot(goldenViewport, 100ms);
    runtime.setClickTimeScale(2.0F, 100ms);
    const FrameSnapshot afterChange = runtime.snapshot(goldenViewport, 100ms);

    // The setter boundary is not a simulation step. Existing particles must
    // therefore be bit-identical until source time advances past that boundary.
    BAFX_CHECK(beforeChange.sprites.size() == afterChange.sprites.size());
    for (std::size_t index = 0U; index < beforeChange.sprites.size(); ++index)
    {
        checkSpriteEqual(afterChange.sprites[index], beforeChange.sprites[index]);
    }

    const FrameSnapshot advanced = runtime.snapshot(goldenViewport, 125ms);
    BAFX_CHECK(countKind(advanced, SpriteKind::CenterDisk) == 1U);
    BAFX_CHECK(
        std::abs(
            firstKind(advanced, SpriteKind::CenterDisk).sizePixels
            - firstKind(afterChange, SpriteKind::CenterDisk).sizePixels)
        > 1.0e-3F);
}

BAFX_TEST(click_particle_spawn_settings_only_apply_to_new_activations)
{
    SimulationRuntime runtime;
    ClickParticleSettings initial{};
    initial.ringsCount = 3U;
    initial.ringsRadiusMin = 50.0F;
    initial.ringsRadiusMax = 50.0F;
    runtime.setClickParticleSettings(initial);
    runtime.pointerDown(goldenCenter, goldenViewport, 0ns);

    const FrameSnapshot beforeChange = runtime.snapshot(goldenViewport, 1ns);
    const auto originalRings = spritesOfKind(
        beforeChange,
        SpriteKind::DissolveRing);
    BAFX_CHECK(originalRings.size() == 3U);

    ClickParticleSettings replacement = initial;
    replacement.ringsCount = 1U;
    replacement.ringsRadiusMin = 100.0F;
    replacement.ringsRadiusMax = 100.0F;
    runtime.setClickParticleSettings(replacement, 1ns);
    const FrameSnapshot retainedFrame = runtime.snapshot(
        goldenViewport,
        1ns);
    const auto retainedRings = spritesOfKind(
        retainedFrame,
        SpriteKind::DissolveRing);
    BAFX_CHECK(retainedRings.size() == originalRings.size());
    for (std::size_t index = 0U; index < originalRings.size(); ++index)
    {
        BAFX_CHECK_NEAR(
            retainedRings[index]->sizePixels,
            originalRings[index]->sizePixels,
            0.0F);
    }

    runtime.pointerUp(10ms);
    runtime.onFrameRendered(1010ms);
    BAFX_CHECK(runtime.instanceCount() == 0U);
    runtime.pointerDown(goldenCenter, goldenViewport, 1100ms);
    const FrameSnapshot replacementFrame = runtime.snapshot(
        goldenViewport,
        1100ms + 1ns);
    const auto replacementRings = spritesOfKind(
        replacementFrame,
        SpriteKind::DissolveRing);
    BAFX_CHECK(replacementRings.size() == 1U);
    BAFX_CHECK_NEAR(
        replacementRings.front()->sizePixels,
        originalRings.front()->sizePixels * 2.0F,
        1.0e-4F);
}

BAFX_TEST(shard_spawn_settings_only_affect_new_particles)
{
    SimulationRuntime runtime(20260716U);
    ShardParticleSettings initial{};
    initial.clickCount = 1U;
    runtime.setShardParticleSettings(initial);
    runtime.pointerDown(goldenCenter, goldenViewport, 0ns);
    runtime.advance(1ns);

    ShardParticleSettings replacement = initial;
    replacement.clickCount = 3U;
    replacement.sizeMin = 540.0F;
    replacement.sizeMax = 540.0F;
    runtime.setShardParticleSettings(replacement);
    runtime.pointerMove(
        PointF{goldenCenter.x + 200.0F, goldenCenter.y},
        goldenViewport,
        30ms);

    const auto activeShards = spritesOfKind(
        runtime.snapshot(goldenViewport, 100ms),
        SpriteKind::Triangle);
    BAFX_CHECK(activeShards.size() == 2U);
    // The click shard retained its sampled size, while the drag shard emitted
    // after the update used the new shared Web size range.
    BAFX_CHECK(
        activeShards.back()->sizePixels
        > activeShards.front()->sizePixels * 5.0F);

    runtime.pointerUp(100ms);
    runtime.onFrameRendered(1100ms);
    BAFX_CHECK(runtime.instanceCount() == 0U);
    runtime.pointerDown(goldenCenter, goldenViewport, 1200ms);
    BAFX_CHECK(
        countKind(
            runtime.snapshot(goldenViewport, 1200ms + 1ns),
            SpriteKind::Triangle)
        == 3U);
}

BAFX_TEST(click_particle_lifetimes_apply_to_an_existing_activation)
{
    Simulation simulation;
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);

    ClickParticleSettings shortLived{};
    shortLived.diskLifetimeMs = 50.0F;
    shortLived.ringsLifetimeMs = 50.0F;
    simulation.setClickParticleSettings(shortLived, 100ms);
    const FrameSnapshot shortened = simulation.snapshot(goldenViewport, 100ms);
    BAFX_CHECK(countKind(shortened, SpriteKind::CenterDisk) == 0U);
    BAFX_CHECK(countKind(shortened, SpriteKind::DissolveRing) == 0U);

    ClickParticleSettings extended = shortLived;
    extended.diskLifetimeMs = 500.0F;
    extended.ringsLifetimeMs = 800.0F;
    simulation.setClickParticleSettings(extended, 100ms);
    const FrameSnapshot lengthened = simulation.snapshot(goldenViewport, 100ms);
    BAFX_CHECK(countKind(lengthened, SpriteKind::CenterDisk) == 1U);
    BAFX_CHECK(countKind(lengthened, SpriteKind::DissolveRing) == 2U);
}

BAFX_TEST(ring_motion_hot_update_is_continuous_and_maps_web_direction)
{
    Simulation simulation;
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
    const FrameSnapshot beforeFrame = simulation.snapshot(
        goldenViewport,
        300ms);
    const auto before = spritesOfKind(
        beforeFrame,
        SpriteKind::DissolveRing);

    ClickParticleSettings clockwiseWeb{};
    clockwiseWeb.ringsRotationDirection = 1.0F;
    clockwiseWeb.ringsAngularVelocityMultiplier *= 2.0F;
    simulation.setClickParticleSettings(clockwiseWeb, 300ms);
    const FrameSnapshot boundaryFrame = simulation.snapshot(
        goldenViewport,
        300ms);
    const auto boundary = spritesOfKind(
        boundaryFrame,
        SpriteKind::DissolveRing);
    BAFX_CHECK(boundary.size() == before.size());
    for (std::size_t index = 0U; index < before.size(); ++index)
    {
        BAFX_CHECK_NEAR(
            boundary[index]->rotationRadians,
            before[index]->rotationRadians,
            1.0e-6F);
    }

    const FrameSnapshot reversedFrame = simulation.snapshot(
        goldenViewport,
        350ms);
    const auto reversed = spritesOfKind(
        reversedFrame,
        SpriteKind::DissolveRing);
    for (std::size_t index = 0U; index < boundary.size(); ++index)
    {
        // Positive Canvas rotation is clockwise, so the native world-space
        // angle must decrease to produce the same screen-space direction.
        BAFX_CHECK(
            reversed[index]->rotationRadians
            < boundary[index]->rotationRadians);
    }

    ClickParticleSettings stopped = clockwiseWeb;
    stopped.ringsRotationDirection = 0.0F;
    simulation.setClickParticleSettings(stopped, 350ms);
    const FrameSnapshot stoppedLaterFrame = simulation.snapshot(
        goldenViewport,
        400ms);
    const auto stoppedLater = spritesOfKind(
        stoppedLaterFrame,
        SpriteKind::DissolveRing);
    for (std::size_t index = 0U; index < reversed.size(); ++index)
    {
        BAFX_CHECK_NEAR(
            stoppedLater[index]->rotationRadians,
            reversed[index]->rotationRadians,
            1.0e-6F);
    }
}

BAFX_TEST(trail_time_scale_change_preserves_existing_visual_age)
{
    SimulationRuntime runtime;
    constexpr PointF start{100.0F, 100.0F};
    constexpr PointF end{600.0F, 100.0F};
    runtime.continuePointerStroke(start, goldenViewport, 0ns, 0ns);
    runtime.advance(1ns);
    runtime.pointerMove(end, goldenViewport, 100ms);

    const FrameSnapshot beforeChange = runtime.snapshot(goldenViewport, 100ms);
    runtime.setTrailTimeScale(4.0F, 100ms);
    const FrameSnapshot afterChange = runtime.snapshot(goldenViewport, 100ms);

    BAFX_CHECK(beforeChange.trail.size() == 2U);
    BAFX_CHECK(beforeChange.sprites.size() == afterChange.sprites.size());
    BAFX_CHECK(beforeChange.trail.size() == afterChange.trail.size());
    for (std::size_t index = 0U; index < beforeChange.sprites.size(); ++index)
    {
        checkSpriteEqual(afterChange.sprites[index], beforeChange.sprites[index]);
    }
    for (std::size_t index = 0U; index < beforeChange.trail.size(); ++index)
    {
        BAFX_CHECK(
            afterChange.trail[index].normalizedAge
            == beforeChange.trail[index].normalizedAge);
    }

    // After the boundary, 25 ms of source time at 4x advances the shared
    // trail clock by 100 ms without rewriting either point's prior 100 ms.
    const FrameSnapshot advanced = runtime.snapshot(goldenViewport, 125ms);
    BAFX_CHECK(advanced.trail.size() == 2U);
    BAFX_CHECK_NEAR(advanced.trail.front().normalizedAge, 2.0F / 3.0F, 1.0e-6F);
    BAFX_CHECK_NEAR(advanced.trail.back().normalizedAge, 1.0F / 3.0F, 1.0e-6F);
}

BAFX_TEST(pointer_cancel_keeps_the_current_trail_until_it_naturally_expires)
{
    Simulation simulation;
    simulation.pointerDown(PointF{100.0F, 100.0F}, goldenViewport, 0ns);
    simulation.advance(1ns);
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

BAFX_TEST(trail_point_expires_at_the_authored_lifetime_boundary)
{
    Simulation simulation;
    simulation.startTrail(PointF{100.0F, 100.0F}, goldenViewport, 0ns);

    simulation.advance(299ms);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 299ms).trail.size() == 1U);

    // Web and Unity both retire a vertex once its 300 ms lifetime is reached.
    simulation.advance(300ms);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 300ms).trail.empty());
}

BAFX_TEST(trail_length_multiplier_changes_the_simulated_retention_window)
{
    Simulation simulation;
    simulation.setTrailLengthMultiplier(2.0F);
    simulation.pointerDown(PointF{100.0F, 100.0F}, goldenViewport, 0ns);
    simulation.advance(1ns);
    simulation.pointerMove(PointF{600.0F, 100.0F}, goldenViewport, 50ms);
    simulation.pointerUp(60ms);

    simulation.advance(400ms);
    BAFX_CHECK(!simulation.snapshot(goldenViewport, 400ms).trail.empty());

    simulation.advance(700ms);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 700ms).trail.empty());

    simulation.setTrailLengthMultiplier(0.0F);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 700ms).trail.empty());
}

BAFX_TEST(game_trail_parking_removes_one_cached_head_per_update)
{
    Simulation simulation;
    simulation.startTrail(PointF{100.0F, 100.0F}, goldenViewport, 0ns);
    simulation.pointerMove(PointF{200.0F, 100.0F}, goldenViewport, 10ms);
    simulation.pointerMove(PointF{300.0F, 100.0F}, goldenViewport, 20ms);
    simulation.pointerMove(PointF{400.0F, 100.0F}, goldenViewport, 30ms);
    simulation.pointerMove(PointF{500.0F, 100.0F}, goldenViewport, 40ms);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 40ms).trail.size() == 5U);

    simulation.updateUnityTrailTimeScale(0.19F);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 40ms).trail.size() == 5U);

    simulation.updateUnityTrailTimeScale(0.19F);
    FrameSnapshot frame = simulation.snapshot(goldenViewport, 40ms);
    BAFX_CHECK(frame.trail.size() == 4U);
    BAFX_CHECK_NEAR(frame.trail.front().positionPixels.x, 200.0F, 1.0e-3F);

    simulation.updateUnityTrailTimeScale(0.19F);
    frame = simulation.snapshot(goldenViewport, 40ms);
    BAFX_CHECK(frame.trail.size() == 3U);
    BAFX_CHECK_NEAR(frame.trail.front().positionPixels.x, 300.0F, 1.0e-3F);

    simulation.updateUnityTrailTimeScale(0.19F);
    frame = simulation.snapshot(goldenViewport, 40ms);
    BAFX_CHECK(frame.trail.size() == 2U);
    BAFX_CHECK_NEAR(frame.trail.front().positionPixels.x, 400.0F, 1.0e-3F);

    simulation.updateUnityTrailTimeScale(0.19F);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 40ms).trail.empty());
}

BAFX_TEST(game_trail_parking_matches_the_n_zero_through_four_sequences)
{
    constexpr std::array<std::array<std::size_t, 4>, 5> expectedSizes{
        std::array<std::size_t, 4>{0U, 0U, 0U, 0U},
        std::array<std::size_t, 4>{1U, 0U, 0U, 0U},
        std::array<std::size_t, 4>{2U, 0U, 0U, 0U},
        std::array<std::size_t, 4>{3U, 2U, 0U, 0U},
        std::array<std::size_t, 4>{4U, 3U, 2U, 0U}};

    for (std::size_t pointCount = 0U; pointCount < expectedSizes.size(); ++pointCount)
    {
        Simulation simulation;
        if (pointCount == 0U)
        {
            simulation.setTrailLengthMultiplier(0.0F);
        }
        simulation.startTrail(PointF{100.0F, 100.0F}, goldenViewport, 0ns);
        for (std::size_t index = 1U; index < pointCount; ++index)
        {
            simulation.pointerMove(
                PointF{100.0F + static_cast<float>(index) * 100.0F, 100.0F},
                goldenViewport,
                std::chrono::milliseconds(index * 10U));
        }

        for (const std::size_t expectedSize : expectedSizes[pointCount])
        {
            simulation.updateUnityTrailTimeScale(0.19F);
            BAFX_CHECK(
                simulation.snapshot(goldenViewport, 40ms).trail.size()
                == expectedSize);
        }
    }
}

BAFX_TEST(game_trail_parking_uses_an_inclusive_threshold_and_one_point_finish_frame)
{
    Simulation simulation;
    simulation.startTrail(PointF{100.0F, 100.0F}, goldenViewport, 0ns);

    simulation.updateUnityTrailTimeScale(0.190001F);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 1ms).trail.size() == 1U);

    simulation.updateUnityTrailTimeScale(0.19F);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 2ms).trail.size() == 1U);

    simulation.updateUnityTrailTimeScale(0.19F);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 3ms).trail.empty());

    // Once FinishParkingSequence disables the renderer, later low-scale
    // Updates are intentionally idempotent rather than re-running cleanup.
    simulation.updateUnityTrailTimeScale(0.19F);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 4ms).trail.empty());
}

BAFX_TEST(game_trail_parking_empty_trail_finishes_on_the_transition_update)
{
    Simulation simulation;
    simulation.setTrailLengthMultiplier(0.0F);
    simulation.startTrail(PointF{100.0F, 100.0F}, goldenViewport, 0ns);

    BAFX_CHECK(simulation.snapshot(goldenViewport, 0ns).trail.empty());
    simulation.updateUnityTrailTimeScale(0.19F);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 1ms).trail.empty());

    simulation.setTrailLengthMultiplier(1.0F);
    simulation.pointerMove(PointF{200.0F, 100.0F}, goldenViewport, 2ms);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 2ms).trail.empty());

    simulation.updateUnityTrailTimeScale(1.0F);
    simulation.pointerMove(PointF{300.0F, 100.0F}, goldenViewport, 3ms);
    const FrameSnapshot frame = simulation.snapshot(goldenViewport, 3ms);
    BAFX_CHECK(frame.trail.size() == 1U);
    BAFX_CHECK_NEAR(frame.trail.front().positionPixels.x, 300.0F, 1.0e-3F);
}

BAFX_TEST(game_trail_parking_nan_uses_the_low_scale_branch)
{
    Simulation simulation;
    simulation.startTrail(PointF{100.0F, 100.0F}, goldenViewport, 0ns);

    simulation.updateUnityTrailTimeScale(std::numeric_limits<float>::quiet_NaN());
    BAFX_CHECK(simulation.snapshot(goldenViewport, 1ms).trail.size() == 1U);

    simulation.updateUnityTrailTimeScale(std::numeric_limits<float>::quiet_NaN());
    BAFX_CHECK(simulation.snapshot(goldenViewport, 2ms).trail.empty());
}

BAFX_TEST(game_trail_parking_resume_preserves_the_current_suffix)
{
    Simulation simulation;
    simulation.startTrail(PointF{100.0F, 100.0F}, goldenViewport, 0ns);
    simulation.pointerMove(PointF{200.0F, 100.0F}, goldenViewport, 10ms);
    simulation.pointerMove(PointF{300.0F, 100.0F}, goldenViewport, 20ms);
    simulation.pointerMove(PointF{400.0F, 100.0F}, goldenViewport, 30ms);

    simulation.updateUnityTrailTimeScale(0.19F);
    simulation.updateUnityTrailTimeScale(0.19F);
    FrameSnapshot frame = simulation.snapshot(goldenViewport, 30ms);
    BAFX_CHECK(frame.trail.size() == 3U);
    BAFX_CHECK_NEAR(frame.trail.front().positionPixels.x, 200.0F, 1.0e-3F);

    simulation.updateUnityTrailTimeScale(1.0F);
    frame = simulation.snapshot(goldenViewport, 31ms);
    BAFX_CHECK(frame.trail.size() == 3U);
    BAFX_CHECK_NEAR(frame.trail.front().positionPixels.x, 200.0F, 1.0e-3F);

    simulation.pointerMove(PointF{500.0F, 100.0F}, goldenViewport, 40ms);
    frame = simulation.snapshot(goldenViewport, 40ms);
    BAFX_CHECK(frame.trail.size() == 4U);
    BAFX_CHECK_NEAR(frame.trail.back().positionPixels.x, 500.0F, 1.0e-3F);
}

BAFX_TEST(game_trail_parking_state_persists_across_pooled_reactivation)
{
    Simulation simulation;
    simulation.startTrail(PointF{100.0F, 100.0F}, goldenViewport, 0ns);
    simulation.updateUnityTrailTimeScale(0.19F);
    simulation.updateUnityTrailTimeScale(0.19F);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 1ms).trail.empty());

    simulation.startTrail(PointF{700.0F, 100.0F}, goldenViewport, 10ms);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 10ms).trail.empty());

    simulation.pointerMove(PointF{800.0F, 100.0F}, goldenViewport, 20ms);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 20ms).trail.empty());

    // FXTouch.Stop clears the Trail but does not reset FxTrailTimeScale.
    // The next normal-scale Update owns re-enabling the pooled renderer.
    simulation.updateUnityTrailTimeScale(1.0F);
    simulation.pointerMove(PointF{900.0F, 100.0F}, goldenViewport, 30ms);
    const FrameSnapshot frame = simulation.snapshot(goldenViewport, 30ms);
    BAFX_CHECK(frame.trail.size() == 1U);
    BAFX_CHECK_NEAR(frame.trail.front().positionPixels.x, 900.0F, 1.0e-3F);
}

BAFX_TEST(game_trail_parking_keeps_an_unfinished_pool_cache)
{
    Simulation simulation;
    simulation.startTrail(PointF{100.0F, 100.0F}, goldenViewport, 0ns);
    simulation.pointerMove(PointF{200.0F, 100.0F}, goldenViewport, 10ms);
    simulation.pointerMove(PointF{300.0F, 100.0F}, goldenViewport, 20ms);
    simulation.pointerMove(PointF{400.0F, 100.0F}, goldenViewport, 30ms);
    simulation.pointerMove(PointF{500.0F, 100.0F}, goldenViewport, 40ms);

    simulation.updateUnityTrailTimeScale(0.19F);
    simulation.updateUnityTrailTimeScale(0.19F);
    simulation.pointerUp(50ms);
    simulation.onFrameRendered(1050ms);
    BAFX_CHECK(!simulation.active());

    // FXTouch.Stop clears TrailRenderer geometry, but the sibling
    // FxTrailTimeScale component retains an unfinished parkingPosList.
    simulation.startTrail(PointF{700.0F, 100.0F}, goldenViewport, 1100ms);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 1100ms).trail.empty());

    simulation.updateUnityTrailTimeScale(0.19F);
    const FrameSnapshot frame = simulation.snapshot(goldenViewport, 1101ms);
    BAFX_CHECK(frame.trail.size() == 3U);
    BAFX_CHECK_NEAR(frame.trail.front().positionPixels.x, 300.0F, 1.0e-3F);
    BAFX_CHECK_NEAR(frame.trail.back().positionPixels.x, 500.0F, 1.0e-3F);
}

BAFX_TEST(game_trail_parking_disables_emission_until_normal_mode_resumes)
{
    Simulation simulation;
    simulation.startTrail(PointF{100.0F, 100.0F}, goldenViewport, 0ns);
    simulation.pointerMove(PointF{200.0F, 100.0F}, goldenViewport, 10ms);

    simulation.updateUnityTrailTimeScale(0.0F);
    simulation.updateUnityTrailTimeScale(0.0F);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 10ms).trail.empty());

    simulation.pointerMove(PointF{800.0F, 500.0F}, goldenViewport, 20ms);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 20ms).trail.empty());

    simulation.updateUnityTrailTimeScale(1.0F);
    FrameSnapshot frame = simulation.snapshot(goldenViewport, 30ms);
    BAFX_CHECK(frame.trail.empty());

    simulation.pointerMove(PointF{900.0F, 500.0F}, goldenViewport, 40ms);
    frame = simulation.snapshot(goldenViewport, 40ms);
    BAFX_CHECK(frame.trail.size() == 1U);
    BAFX_CHECK_NEAR(frame.trail.front().positionPixels.x, 900.0F, 1.0e-3F);
}

BAFX_TEST(desktop_pause_does_not_implicitly_enter_game_trail_parking)
{
    SimulationTimeline timeline;
    Simulation simulation;
    simulation.startTrail(PointF{100.0F, 100.0F}, goldenViewport, 0ns);
    simulation.pointerMove(PointF{300.0F, 100.0F}, goldenViewport, 20ms);

    timeline.setPaused(true, 20ms);
    constexpr std::array<SimulationTime, 3> pausedWallTimes{
        30ms,
        100ms,
        1s};
    for (const SimulationTime wallTime : pausedWallTimes)
    {
        simulation.advance(timeline.fromWallTime(wallTime));
        BAFX_CHECK(simulation.snapshot(goldenViewport, 20ms).trail.size() == 2U);
    }
}

BAFX_TEST(pointer_cancel_uses_the_same_authored_cleanup_deadline_as_release)
{
    Simulation simulation;
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
    simulation.pointerCancel(10ms);

    simulation.onFrameRendered(1009ms);
    BAFX_CHECK(simulation.active());
    simulation.onFrameRendered(1010ms);
    BAFX_CHECK(!simulation.active());
}

BAFX_TEST(release_cleanup_is_independent_of_monitor_refresh_rate)
{
    constexpr std::array refreshRates{60U, 120U, 144U, 240U};
    for (const std::uint32_t refreshRate : refreshRates)
    {
        Simulation simulation;
        simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
        simulation.pointerUp(10ms);

        // A 144/240 Hz monitor submits well over 60 frames before the longest
        // click shards finish. Presentation cadence must not own their lifetime.
        for (std::uint32_t frame = 1U; frame < refreshRate; ++frame)
        {
            const auto elapsed = std::chrono::duration_cast<SimulationTime>(
                std::chrono::duration<double>(
                    static_cast<double>(frame) / refreshRate));
            const SimulationTime frameTime = 10ms + elapsed;
            simulation.advance(frameTime);
            simulation.onFrameRendered(frameTime);
            BAFX_CHECK(simulation.active());
        }

        // The 1 s boundary was drawable; retirement follows that presentation.
        BAFX_CHECK(simulation.snapshot(goldenViewport, 1010ms).active);
        simulation.onFrameRendered(1010ms);
        BAFX_CHECK(!simulation.active());
    }
}

BAFX_TEST(release_waits_for_web_configured_click_particle_lifetimes)
{
    Simulation simulation;
    ClickParticleSettings clickSettings{};
    clickSettings.diskLifetimeMs = 2500.0F;
    clickSettings.ringsLifetimeMs = 2000.0F;
    simulation.setClickParticleSettings(clickSettings);

    ShardParticleSettings shardSettings{};
    shardSettings.clickCount = 0U;
    simulation.setShardParticleSettings(shardSettings);
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
    simulation.pointerUp(10ms);

    simulation.advance(1010ms);
    const FrameSnapshot extended = simulation.snapshot(goldenViewport, 1010ms);
    BAFX_CHECK(countKind(extended, SpriteKind::CenterDisk) == 1U);
    BAFX_CHECK(countKind(extended, SpriteKind::DissolveRing) == 2U);
    simulation.onFrameRendered(1010ms);
    BAFX_CHECK(simulation.active());

    simulation.advance(2600ms);
    const FrameSnapshot completed = simulation.snapshot(goldenViewport, 2600ms);
    BAFX_CHECK(countKind(completed, SpriteKind::CenterDisk) == 0U);
    BAFX_CHECK(countKind(completed, SpriteKind::DissolveRing) == 0U);
    simulation.onFrameRendered(2600ms);
    BAFX_CHECK(!simulation.active());
}

BAFX_TEST(release_waits_for_each_spawned_click_shard_lifetime)
{
    Simulation simulation;
    ClickParticleSettings clickSettings{};
    clickSettings.diskLifetimeMs = 1.0F;
    clickSettings.ringsCount = 0U;
    simulation.setClickParticleSettings(clickSettings);

    ShardParticleSettings shardSettings{};
    shardSettings.clickCount = 1U;
    shardSettings.clickLifetimeMinMs = 2500.0F;
    shardSettings.clickLifetimeMaxMs = 2500.0F;
    simulation.setShardParticleSettings(shardSettings);
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
    simulation.pointerUp(10ms);

    simulation.advance(1010ms);
    BAFX_CHECK(
        countKind(
            simulation.snapshot(goldenViewport, 1010ms),
            SpriteKind::Triangle)
        == 1U);
    simulation.onFrameRendered(1010ms);
    BAFX_CHECK(simulation.active());

    simulation.advance(2600ms);
    simulation.onFrameRendered(2600ms);
    BAFX_CHECK(!simulation.active());
}

BAFX_TEST(release_waits_for_web_configured_trail_lifetime)
{
    Simulation simulation;
    simulation.setTrailLengthMultiplier(10000.0F / 300.0F);
    simulation.startTrail(goldenCenter, goldenViewport, 0ns);
    simulation.pointerMove(
        PointF{goldenCenter.x + 200.0F, goldenCenter.y},
        goldenViewport,
        10ms);
    simulation.pointerUp(20ms);

    simulation.advance(5020ms);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 5020ms).trail.size() >= 2U);
    simulation.onFrameRendered(5020ms);
    BAFX_CHECK(simulation.active());

    simulation.advance(10020ms);
    BAFX_CHECK(simulation.snapshot(goldenViewport, 10020ms).trail.empty());
    simulation.onFrameRendered(10020ms);
    BAFX_CHECK(!simulation.active());
}

BAFX_TEST(release_uses_virtual_click_time_for_visible_lifetime)
{
    Simulation simulation;
    ClickParticleSettings clickSettings{};
    clickSettings.diskLifetimeMs = 200.0F;
    clickSettings.ringsCount = 0U;
    simulation.setClickParticleSettings(clickSettings);

    ShardParticleSettings shardSettings{};
    shardSettings.clickCount = 0U;
    simulation.setShardParticleSettings(shardSettings);
    simulation.setClickTimeScale(0.1F);
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
    simulation.pointerUp(10ms);

    simulation.advance(1010ms);
    BAFX_CHECK(
        countKind(
            simulation.snapshot(goldenViewport, 1010ms),
            SpriteKind::CenterDisk)
        == 1U);
    simulation.onFrameRendered(1010ms);
    BAFX_CHECK(simulation.active());

    simulation.advance(3010ms);
    simulation.onFrameRendered(3010ms);
    BAFX_CHECK(!simulation.active());
}

BAFX_TEST(trail_parking_keeps_the_unity_root_release_deadline)
{
    Simulation simulation;
    simulation.setTrailLengthMultiplier(2000.0F / 300.0F);
    simulation.startTrail(goldenCenter, goldenViewport, 0ns);
    simulation.pointerMove(
        PointF{goldenCenter.x + 200.0F, goldenCenter.y},
        goldenViewport,
        10ms);
    simulation.updateUnityTrailTimeScale(0.19F);
    simulation.pointerUp(20ms);

    simulation.advance(1019ms);
    simulation.onFrameRendered(1019ms);
    BAFX_CHECK(simulation.active());
    BAFX_CHECK(simulation.snapshot(goldenViewport, 1019ms).trail.size() >= 2U);

    simulation.advance(1020ms);
    simulation.onFrameRendered(1020ms);
    BAFX_CHECK(!simulation.active());
}

BAFX_TEST(trail_parking_clears_at_one_second_while_a_long_click_child_survives)
{
    Simulation simulation;
    ClickParticleSettings clickSettings{};
    clickSettings.diskLifetimeMs = 2500.0F;
    clickSettings.ringsCount = 0U;
    simulation.setClickParticleSettings(clickSettings);

    ShardParticleSettings shardSettings{};
    shardSettings.clickCount = 0U;
    simulation.setShardParticleSettings(shardSettings);
    simulation.setTrailLengthMultiplier(2000.0F / 300.0F);
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
    simulation.advance(1ns);
    simulation.pointerMove(
        PointF{goldenCenter.x + 200.0F, goldenCenter.y},
        goldenViewport,
        10ms);
    simulation.updateUnityTrailTimeScale(0.19F);
    simulation.pointerUp(20ms);

    simulation.advance(1020ms);
    const FrameSnapshot boundary = simulation.snapshot(goldenViewport, 1020ms);
    BAFX_CHECK(boundary.trail.size() >= 2U);
    BAFX_CHECK(countKind(boundary, SpriteKind::CenterDisk) == 1U);
    simulation.onFrameRendered(1020ms);

    const FrameSnapshot retainedClick = simulation.snapshot(
        goldenViewport,
        1020ms);
    BAFX_CHECK(simulation.active());
    BAFX_CHECK(retainedClick.trail.empty());
    BAFX_CHECK(countKind(retainedClick, SpriteKind::CenterDisk) == 1U);

    simulation.advance(2600ms);
    simulation.onFrameRendered(2600ms);
    BAFX_CHECK(!simulation.active());
}

BAFX_TEST(lifetime_hot_updates_extend_and_then_shorten_release_retention)
{
    Simulation simulation;
    ClickParticleSettings clickSettings{};
    clickSettings.diskLifetimeMs = 500.0F;
    clickSettings.ringsCount = 0U;
    simulation.setClickParticleSettings(clickSettings);

    ShardParticleSettings shardSettings{};
    shardSettings.clickCount = 0U;
    simulation.setShardParticleSettings(shardSettings);
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
    simulation.pointerUp(10ms);

    ClickParticleSettings extended = clickSettings;
    extended.diskLifetimeMs = 2500.0F;
    simulation.setClickParticleSettings(extended, 900ms);
    simulation.advance(1010ms);
    simulation.onFrameRendered(1010ms);
    BAFX_CHECK(simulation.active());

    ClickParticleSettings shortened = extended;
    shortened.diskLifetimeMs = 100.0F;
    simulation.setClickParticleSettings(shortened, 1200ms);
    simulation.advance(1200ms);
    BAFX_CHECK(
        countKind(
            simulation.snapshot(goldenViewport, 1200ms),
            SpriteKind::CenterDisk)
        == 0U);
    simulation.onFrameRendered(1200ms);
    BAFX_CHECK(!simulation.active());
}

BAFX_TEST(runtime_returns_extended_release_to_the_pool_after_children_complete)
{
    SimulationRuntime runtime;
    ClickParticleSettings clickSettings{};
    clickSettings.diskLifetimeMs = 2500.0F;
    clickSettings.ringsCount = 0U;
    runtime.setClickParticleSettings(clickSettings);

    ShardParticleSettings shardSettings{};
    shardSettings.clickCount = 0U;
    runtime.setShardParticleSettings(shardSettings);
    runtime.pointerDown(goldenCenter, goldenViewport, 0ns);
    runtime.pointerUp(10ms);

    runtime.advance(1010ms);
    runtime.onFrameRendered(1010ms);
    BAFX_CHECK(runtime.instanceCount() == 1U);
    BAFX_CHECK(runtime.pooledInstanceCount() == 0U);

    runtime.advance(2600ms);
    runtime.onFrameRendered(2600ms);
    BAFX_CHECK(runtime.instanceCount() == 0U);
    BAFX_CHECK(runtime.pooledInstanceCount() == 1U);
}

BAFX_TEST(validated_slowest_click_lifetime_has_a_finite_release_bound)
{
    Simulation simulation;
    ClickParticleSettings clickSettings{};
    clickSettings.diskLifetimeMs = 10000.0F;
    clickSettings.ringsCount = 0U;
    simulation.setClickParticleSettings(clickSettings);

    ShardParticleSettings shardSettings{};
    shardSettings.clickCount = 0U;
    simulation.setShardParticleSettings(shardSettings);
    simulation.setClickTimeScale(0.01F);
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
    simulation.pointerUp(10ms);

    // onFrameRendered may be called directly by lifecycle tests. It must use
    // the read-only virtual-time projection rather than stale advance state.
    simulation.onFrameRendered(1000s);
    BAFX_CHECK(simulation.active());
    simulation.onFrameRendered(1004s);
    BAFX_CHECK(!simulation.active());
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

BAFX_TEST(discard_active_effects_removes_all_drawable_state_immediately)
{
    SimulationRuntime runtime;
    runtime.setAlwaysOnTrailEnabled(true, 0ns);
    runtime.pointerMove(PointF{100.0F, 100.0F}, goldenViewport, 10ms);
    runtime.pointerMove(PointF{400.0F, 100.0F}, goldenViewport, 30ms);
    runtime.pointerDown(PointF{600.0F, 300.0F}, goldenViewport, 40ms);
    runtime.pointerUp(50ms);
    runtime.pointerDown(PointF{900.0F, 500.0F}, goldenViewport, 60ms);

    BAFX_CHECK(runtime.active());
    BAFX_CHECK(runtime.pointerHeld());
    BAFX_CHECK(runtime.instanceCount() == 3U);

    runtime.discardActiveEffects();
    const FrameSnapshot discarded = runtime.snapshot(goldenViewport, 60ms);
    BAFX_CHECK(!runtime.active());
    BAFX_CHECK(!runtime.pointerHeld());
    BAFX_CHECK(runtime.instanceCount() == 0U);
    BAFX_CHECK(runtime.pooledInstanceCount() == 0U);
    BAFX_CHECK(!discarded.active);
    BAFX_CHECK(!discarded.pointerHeld);
    BAFX_CHECK(discarded.sprites.empty());
    BAFX_CHECK(discarded.trail.empty());
    BAFX_CHECK(discarded.trailStrokes.empty());

    runtime.advance(10s);
    runtime.onFrameRendered(10s);
    BAFX_CHECK(!runtime.snapshot(goldenViewport, 10s).hasDrawableContent());
}

BAFX_TEST(discard_active_effects_preserves_configuration_and_resets_sampling_phase)
{
    SimulationRuntime runtime;
    ClickParticleSettings clickSettings{};
    clickSettings.ringsCount = 3U;
    runtime.setClickParticleSettings(clickSettings);
    ShardParticleSettings shardSettings{};
    shardSettings.clickCount = 2U;
    runtime.setShardParticleSettings(shardSettings);
    runtime.setClickTimeScale(0.5F);
    runtime.setTrailTimeScale(0.75F);
    runtime.setTrailLengthMultiplier(2.0F);
    runtime.setInputSamplingRateHz(10U);
    runtime.setAlwaysOnTrailEnabled(true, 0ns);

    runtime.pointerMove(PointF{100.0F, 100.0F}, goldenViewport, 0ms);
    runtime.pointerMove(PointF{300.0F, 100.0F}, goldenViewport, 50ms);
    runtime.discardActiveEffects();

    BAFX_CHECK(runtime.alwaysOnTrailEnabled());
    // A hard reset starts a new sampling epoch, so this sub-interval Move must
    // become the next ambient stroke's anchor instead of being rate-limited.
    runtime.pointerMove(PointF{700.0F, 400.0F}, goldenViewport, 60ms);
    runtime.pointerMove(PointF{900.0F, 400.0F}, goldenViewport, 160ms);
    const FrameSnapshot ambient = runtime.snapshot(goldenViewport, 160ms);
    BAFX_CHECK(ambient.trail.size() >= 2U);
    BAFX_CHECK_NEAR(
        ambient.trail.front().positionPixels.x,
        700.0F,
        1.0e-3F);

    runtime.discardActiveEffects();
    runtime.pointerDown(goldenCenter, goldenViewport, 200ms);
    const FrameSnapshot click = runtime.snapshot(goldenViewport, 250ms);
    BAFX_CHECK(countKind(click, SpriteKind::DissolveRing) == 3U);
    BAFX_CHECK(countKind(click, SpriteKind::Triangle) == 2U);
}

BAFX_TEST(discard_active_effects_preserves_the_unity_random_stream_position)
{
    constexpr std::uint64_t seed = 0x12345678U;
    SimulationRuntime recycled(seed);
    SimulationRuntime discarded(seed);

    recycled.pointerDown(goldenCenter, goldenViewport, 0ns);
    discarded.pointerDown(goldenCenter, goldenViewport, 0ns);
    recycled.pointerUp(10ms);
    discarded.pointerUp(10ms);
    recycled.advance(1010ms);
    recycled.onFrameRendered(1010ms);
    discarded.discardActiveEffects();

    BAFX_CHECK(recycled.pooledInstanceCount() == 1U);
    BAFX_CHECK(discarded.pooledInstanceCount() == 0U);
    recycled.pointerDown(goldenCenter, goldenViewport, 1100ms);
    discarded.pointerDown(goldenCenter, goldenViewport, 1100ms);
    const FrameSnapshot recycledFrame = recycled.snapshot(
        goldenViewport,
        1150ms);
    const FrameSnapshot discardedFrame = discarded.snapshot(
        goldenViewport,
        1150ms);

    BAFX_CHECK(recycledFrame.sprites.size() == discardedFrame.sprites.size());
    for (std::size_t index = 0U; index < recycledFrame.sprites.size(); ++index)
    {
        checkSpriteEqual(
            recycledFrame.sprites[index],
            discardedFrame.sprites[index]);
    }
}

BAFX_TEST(runtime_forwards_game_trail_parking_to_all_live_instances)
{
    SimulationRuntime runtime;
    runtime.pointerDown(PointF{100.0F, 100.0F}, goldenViewport, 0ns);
    runtime.advance(1ns);
    runtime.pointerMove(PointF{300.0F, 100.0F}, goldenViewport, 10ms);
    runtime.pointerUp(20ms);

    runtime.pointerDown(PointF{600.0F, 300.0F}, goldenViewport, 30ms);
    runtime.advance(31ms);
    runtime.pointerMove(PointF{800.0F, 300.0F}, goldenViewport, 40ms);
    BAFX_CHECK(runtime.snapshot(goldenViewport, 40ms).trailStrokes.size() == 2U);

    runtime.updateUnityTrailTimeScale(0.19F);
    runtime.updateUnityTrailTimeScale(0.19F);
    const FrameSnapshot frame = runtime.snapshot(goldenViewport, 40ms);
    BAFX_CHECK(frame.trailStrokes.empty());
    BAFX_CHECK(countKind(frame, SpriteKind::CenterDisk) == 2U);
    BAFX_CHECK(countKind(frame, SpriteKind::DissolveRing) == 4U);
}

BAFX_TEST(always_on_trail_is_opt_in_and_ignores_free_moves_by_default)
{
    SimulationRuntime runtime;
    runtime.pointerMove(PointF{100.0F, 100.0F}, goldenViewport, 10ms);
    runtime.pointerMove(PointF{600.0F, 100.0F}, goldenViewport, 50ms);

    BAFX_CHECK(!runtime.alwaysOnTrailEnabled());
    BAFX_CHECK(runtime.instanceCount() == 0U);
    BAFX_CHECK(!runtime.snapshot(goldenViewport, 50ms).hasDrawableContent());
}

BAFX_TEST(unlimited_input_sampling_preserves_each_pointer_turn)
{
    SimulationRuntime runtime;
    runtime.pointerDown(PointF{100.0F, 100.0F}, goldenViewport, 0ms);
    runtime.advance(1ns);
    runtime.pointerMove(PointF{300.0F, 100.0F}, goldenViewport, 100ms);
    runtime.pointerMove(PointF{300.0F, 300.0F}, goldenViewport, 120ms);
    runtime.pointerMove(PointF{500.0F, 300.0F}, goldenViewport, 200ms);

    const FrameSnapshot frame = runtime.snapshot(goldenViewport, 200ms);
    BAFX_CHECK(trailContainsPoint(frame, PointF{300.0F, 100.0F}));
    BAFX_CHECK(trailContainsPoint(frame, PointF{300.0F, 300.0F}));
    BAFX_CHECK(trailContainsPoint(frame, PointF{500.0F, 300.0F}));
}

BAFX_TEST(limited_input_sampling_drops_intermediate_turns)
{
    SimulationRuntime runtime;
    runtime.setInputSamplingRateHz(10U);
    runtime.pointerDown(PointF{100.0F, 100.0F}, goldenViewport, 0ms);
    runtime.advance(1ns);
    runtime.pointerMove(PointF{300.0F, 100.0F}, goldenViewport, 100ms);
    runtime.pointerMove(PointF{300.0F, 300.0F}, goldenViewport, 120ms);
    runtime.pointerMove(PointF{500.0F, 300.0F}, goldenViewport, 200ms);

    const FrameSnapshot frame = runtime.snapshot(goldenViewport, 200ms);
    BAFX_CHECK(trailContainsPoint(frame, PointF{300.0F, 100.0F}));
    BAFX_CHECK(!trailContainsPoint(frame, PointF{300.0F, 300.0F}));
    BAFX_CHECK(trailContainsPoint(frame, PointF{500.0F, 300.0F}));
}

BAFX_TEST(limited_input_sampling_keeps_the_final_pre_first_frame_move)
{
    SimulationRuntime runtime;
    runtime.setInputSamplingRateHz(10U);
    runtime.pointerDown(
        PointF{100.0F, 100.0F},
        goldenViewport,
        0ms,
        0ms);
    runtime.pointerMove(
        PointF{300.0F, 100.0F},
        goldenViewport,
        1ms,
        1ms);
    runtime.pointerMove(
        PointF{500.0F, 100.0F},
        goldenViewport,
        2ms,
        2ms);
    runtime.advance(50ms);

    const FrameSnapshot frame = runtime.snapshot(goldenViewport, 50ms);
    BAFX_CHECK(frame.trail.size() == 1U);
    BAFX_CHECK(trailContainsPoint(frame, PointF{500.0F, 100.0F}));
    BAFX_CHECK(!trailContainsPoint(frame, PointF{100.0F, 100.0F}));
    BAFX_CHECK(countKind(frame, SpriteKind::Triangle) == 4U);

    runtime.pointerMove(
        PointF{700.0F, 100.0F},
        goldenViewport,
        60ms,
        60ms);
    BAFX_CHECK(
        runtime.snapshot(goldenViewport, 60ms).trail.size() == 1U);
}

BAFX_TEST(input_sampling_uses_wall_time_instead_of_simulation_time)
{
    SimulationRuntime runtime;
    runtime.setInputSamplingRateHz(10U);
    runtime.pointerDown(
        PointF{100.0F, 100.0F},
        goldenViewport,
        0ms,
        0ms);
    runtime.advance(1ns);
    runtime.pointerMove(
        PointF{300.0F, 100.0F},
        goldenViewport,
        10ms,
        100ms);
    runtime.pointerMove(
        PointF{300.0F, 300.0F},
        goldenViewport,
        20ms,
        120ms);
    runtime.pointerMove(
        PointF{500.0F, 300.0F},
        goldenViewport,
        30ms,
        200ms);

    const FrameSnapshot frame = runtime.snapshot(goldenViewport, 30ms);
    BAFX_CHECK(trailContainsPoint(frame, PointF{300.0F, 100.0F}));
    BAFX_CHECK(!trailContainsPoint(frame, PointF{300.0F, 300.0F}));
    BAFX_CHECK_NEAR(frame.trail.back().positionPixels.x, 500.0F, 1.0e-3F);
}

BAFX_TEST(changing_input_sampling_rate_resets_the_sampling_phase)
{
    SimulationRuntime runtime;
    runtime.setInputSamplingRateHz(10U);
    runtime.pointerDown(PointF{100.0F, 100.0F}, goldenViewport, 0ms);
    runtime.advance(1ns);
    runtime.pointerMove(PointF{300.0F, 100.0F}, goldenViewport, 100ms);
    runtime.setInputSamplingRateHz(30U);
    runtime.pointerMove(PointF{300.0F, 300.0F}, goldenViewport, 110ms);

    const FrameSnapshot frame = runtime.snapshot(goldenViewport, 110ms);
    BAFX_CHECK(trailContainsPoint(frame, PointF{300.0F, 100.0F}));
    BAFX_CHECK(trailContainsPoint(frame, PointF{300.0F, 300.0F}));
    BAFX_CHECK_NEAR(frame.trail.back().positionPixels.x, 300.0F, 1.0e-3F);
    BAFX_CHECK_NEAR(frame.trail.back().positionPixels.y, 300.0F, 1.0e-3F);
}

BAFX_TEST(always_on_trail_uses_the_same_input_sampling_limit)
{
    SimulationRuntime runtime;
    runtime.setInputSamplingRateHz(10U);
    runtime.setAlwaysOnTrailEnabled(true, 0ms);
    runtime.pointerMove(PointF{100.0F, 100.0F}, goldenViewport, 0ms);
    runtime.pointerMove(PointF{300.0F, 300.0F}, goldenViewport, 50ms);
    runtime.pointerMove(PointF{500.0F, 100.0F}, goldenViewport, 100ms);

    const FrameSnapshot frame = runtime.snapshot(goldenViewport, 100ms);
    BAFX_CHECK(!trailContainsPoint(frame, PointF{300.0F, 300.0F}));
    BAFX_CHECK_NEAR(frame.trail.front().positionPixels.x, 100.0F, 1.0e-3F);
    BAFX_CHECK_NEAR(frame.trail.back().positionPixels.x, 500.0F, 1.0e-3F);
}

BAFX_TEST(pointer_edges_are_never_blocked_by_input_sampling)
{
    SimulationRuntime runtime;
    runtime.setInputSamplingRateHz(1U);
    runtime.pointerDown(PointF{100.0F, 100.0F}, goldenViewport, 0ms);
    runtime.pointerUp(10ms);
    runtime.pointerDown(PointF{500.0F, 300.0F}, goldenViewport, 20ms);

    const FrameSnapshot frame = runtime.snapshot(goldenViewport, 50ms);
    BAFX_CHECK(countKind(frame, SpriteKind::CenterDisk) == 2U);
    BAFX_CHECK(runtime.pointerHeld());
}

BAFX_TEST(always_on_trail_uses_free_moves_without_fabricating_a_click)
{
    SimulationRuntime runtime;
    runtime.setAlwaysOnTrailEnabled(true, 0ns);
    runtime.pointerMove(PointF{100.0F, 100.0F}, goldenViewport, 10ms);

    const FrameSnapshot anchored = runtime.snapshot(goldenViewport, 10ms);
    BAFX_CHECK(runtime.instanceCount() == 1U);
    BAFX_CHECK(anchored.trail.size() == 1U);
    BAFX_CHECK(countKind(anchored, SpriteKind::CenterDisk) == 0U);
    BAFX_CHECK(countKind(anchored, SpriteKind::DissolveRing) == 0U);
    BAFX_CHECK(countKind(anchored, SpriteKind::Triangle) == 0U);

    runtime.pointerMove(PointF{600.0F, 100.0F}, goldenViewport, 50ms);
    runtime.advance(50ms);
    const FrameSnapshot moving = runtime.snapshot(goldenViewport, 50ms);
    BAFX_CHECK(moving.trail.size() >= 2U);
    BAFX_CHECK(countKind(moving, SpriteKind::CenterDisk) == 0U);
    BAFX_CHECK(countKind(moving, SpriteKind::DissolveRing) == 0U);
    BAFX_CHECK(countKind(moving, SpriteKind::Triangle) > 0U);
}

BAFX_TEST(always_on_and_pressed_trails_use_independent_strokes_without_a_bridge)
{
    SimulationRuntime runtime;
    runtime.setAlwaysOnTrailEnabled(true, 0ns);
    runtime.pointerMove(PointF{100.0F, 100.0F}, goldenViewport, 10ms);
    runtime.pointerMove(PointF{400.0F, 100.0F}, goldenViewport, 40ms);

    runtime.pointerDown(PointF{600.0F, 300.0F}, goldenViewport, 60ms);
    runtime.advance(61ms);
    runtime.pointerMove(PointF{900.0F, 300.0F}, goldenViewport, 100ms);
    FrameSnapshot frame = runtime.snapshot(goldenViewport, 100ms);
    BAFX_CHECK(runtime.pointerHeld());
    BAFX_CHECK(frame.trailStrokes.size() == 2U);
    BAFX_CHECK_NEAR(
        frame.trailStrokes[0].points.back().positionPixels.x,
        400.0F,
        1.0e-3F);
    BAFX_CHECK_NEAR(
        frame.trailStrokes[1].points.front().positionPixels.x,
        600.0F,
        1.0e-3F);
    BAFX_CHECK(countKind(frame, SpriteKind::CenterDisk) == 1U);
    BAFX_CHECK(countKind(frame, SpriteKind::DissolveRing) == 2U);

    runtime.pointerUp(110ms);
    runtime.pointerMove(PointF{1000.0F, 500.0F}, goldenViewport, 120ms);
    runtime.pointerMove(PointF{1200.0F, 500.0F}, goldenViewport, 150ms);
    frame = runtime.snapshot(goldenViewport, 150ms);
    BAFX_CHECK(frame.trailStrokes.size() == 3U);
    BAFX_CHECK_NEAR(
        frame.trailStrokes.back().points.front().positionPixels.x,
        1000.0F,
        1.0e-3F);
}

BAFX_TEST(ending_always_on_trail_makes_reentry_start_from_a_fresh_anchor)
{
    SimulationRuntime runtime;
    runtime.setAlwaysOnTrailEnabled(true, 0ns);
    runtime.pointerMove(PointF{100.0F, 100.0F}, goldenViewport, 10ms);
    runtime.pointerMove(PointF{300.0F, 100.0F}, goldenViewport, 30ms);
    runtime.endAlwaysOnTrail(40ms);

    runtime.pointerMove(PointF{900.0F, 500.0F}, goldenViewport, 50ms);
    runtime.pointerMove(PointF{1100.0F, 500.0F}, goldenViewport, 70ms);
    const FrameSnapshot frame = runtime.snapshot(goldenViewport, 70ms);
    BAFX_CHECK(frame.trailStrokes.size() == 2U);
    BAFX_CHECK_NEAR(
        frame.trailStrokes[0].points.back().positionPixels.x,
        300.0F,
        1.0e-3F);
    BAFX_CHECK_NEAR(
        frame.trailStrokes[1].points.front().positionPixels.x,
        900.0F,
        1.0e-3F);
}

BAFX_TEST(disabling_always_on_trail_stops_new_free_move_geometry)
{
    SimulationRuntime runtime;
    runtime.setAlwaysOnTrailEnabled(true, 0ns);
    runtime.pointerMove(PointF{100.0F, 100.0F}, goldenViewport, 10ms);
    runtime.pointerMove(PointF{400.0F, 100.0F}, goldenViewport, 40ms);
    runtime.setAlwaysOnTrailEnabled(false, 50ms);

    runtime.pointerMove(PointF{1000.0F, 500.0F}, goldenViewport, 60ms);
    runtime.pointerMove(PointF{1200.0F, 500.0F}, goldenViewport, 80ms);
    const FrameSnapshot frame = runtime.snapshot(goldenViewport, 80ms);
    BAFX_CHECK(!runtime.alwaysOnTrailEnabled());
    BAFX_CHECK(frame.trailStrokes.size() == 1U);
    BAFX_CHECK_NEAR(
        frame.trailStrokes.front().points.back().positionPixels.x,
        400.0F,
        1.0e-3F);

    runtime.advance(1050ms);
    runtime.onFrameRendered(1050ms);
    BAFX_CHECK(!runtime.active());
}

BAFX_TEST(ambient_trail_stops_requiring_frames_after_geometry_expires)
{
    SimulationRuntime runtime;
    runtime.setAlwaysOnTrailEnabled(true, 0ns);
    runtime.pointerMove(PointF{100.0F, 100.0F}, goldenViewport, 10ms);
    runtime.pointerMove(PointF{400.0F, 100.0F}, goldenViewport, 40ms);
    runtime.advance(40ms);
    BAFX_CHECK(runtime.renderingRequired(40ms));

    runtime.advance(2s);
    BAFX_CHECK(runtime.active());
    BAFX_CHECK(!runtime.renderingRequired(2s));

    runtime.pointerMove(PointF{800.0F, 100.0F}, goldenViewport, 2010ms);
    BAFX_CHECK(runtime.renderingRequired(2010ms));
}

BAFX_TEST(canceling_a_held_pointer_allows_always_on_trail_to_restart)
{
    SimulationRuntime runtime;
    runtime.setAlwaysOnTrailEnabled(true, 0ns);
    runtime.pointerDown(PointF{100.0F, 100.0F}, goldenViewport, 10ms);
    runtime.advance(11ms);
    runtime.pointerMove(PointF{300.0F, 100.0F}, goldenViewport, 30ms);
    runtime.pointerCancel(40ms);
    BAFX_CHECK(!runtime.pointerHeld());

    runtime.pointerMove(PointF{900.0F, 500.0F}, goldenViewport, 50ms);
    runtime.pointerMove(PointF{1100.0F, 500.0F}, goldenViewport, 70ms);
    const FrameSnapshot frame = runtime.snapshot(goldenViewport, 70ms);
    BAFX_CHECK(frame.trailStrokes.size() == 2U);
    BAFX_CHECK_NEAR(
        frame.trailStrokes.back().points.front().positionPixels.x,
        900.0F,
        1.0e-3F);
}

BAFX_TEST(overlapping_drag_strokes_remain_independent)
{
    SimulationRuntime runtime;

    runtime.pointerDown(PointF{100.0F, 100.0F}, goldenViewport, 0ns);
    runtime.advance(1ns);
    runtime.pointerMove(PointF{400.0F, 100.0F}, goldenViewport, 40ms);
    runtime.pointerUp(50ms);
    runtime.pointerDown(PointF{700.0F, 300.0F}, goldenViewport, 60ms);
    runtime.advance(61ms);
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

    runtime.pointerDown(PointF{900.0F, 600.0F}, goldenViewport, 500ms);
    runtime.pointerUp(510ms);
    runtime.advance(1009ms);
    runtime.onFrameRendered(1009ms);
    BAFX_CHECK(runtime.instanceCount() == 2U);

    runtime.advance(1010ms);
    runtime.onFrameRendered(1010ms);
    BAFX_CHECK(runtime.instanceCount() == 1U);
    BAFX_CHECK(runtime.active());

    runtime.advance(1510ms);
    runtime.onFrameRendered(1510ms);
    BAFX_CHECK(runtime.instanceCount() == 0U);
    BAFX_CHECK(!runtime.active());
}

BAFX_TEST(released_fx_touch_returns_to_the_unity_fifo_pool)
{
    SimulationRuntime runtime;
    BAFX_CHECK(runtime.pooledInstanceCount() == 1U);

    runtime.pointerDown(goldenCenter, goldenViewport, 0ns);
    BAFX_CHECK(runtime.pooledInstanceCount() == 0U);
    runtime.pointerUp(10ms);
    runtime.advance(1010ms);
    runtime.onFrameRendered(1010ms);

    BAFX_CHECK(runtime.instanceCount() == 0U);
    BAFX_CHECK(runtime.pooledInstanceCount() == 1U);
    BAFX_CHECK(!runtime.active());
}

BAFX_TEST(unity_fifo_pool_reuses_the_oldest_fx_touch_component_state)
{
    SimulationRuntime runtime;

    runtime.pointerDown(PointF{100.0F, 100.0F}, goldenViewport, 0ns);
    runtime.advance(1ns);
    runtime.pointerMove(PointF{300.0F, 100.0F}, goldenViewport, 10ms);
    runtime.updateUnityTrailTimeScale(0.19F);
    runtime.updateUnityTrailTimeScale(0.19F);
    BAFX_CHECK(runtime.snapshot(goldenViewport, 10ms).trail.empty());
    runtime.pointerUp(20ms);

    // The first effect is still awaiting its one-second restore, so this
    // activation creates a second object with the default component state.
    runtime.pointerDown(PointF{600.0F, 300.0F}, goldenViewport, 30ms);
    BAFX_CHECK(runtime.snapshot(goldenViewport, 30ms).trail.size() == 1U);
    runtime.pointerUp(40ms);

    runtime.advance(1020ms);
    runtime.onFrameRendered(1020ms);
    BAFX_CHECK(runtime.pooledInstanceCount() == 1U);
    runtime.advance(1040ms);
    runtime.onFrameRendered(1040ms);
    BAFX_CHECK(runtime.pooledInstanceCount() == 2U);

    // Inactive pooled GameObjects do not receive Update. A live-object update
    // would restore the first object's parked renderer before it is acquired.
    runtime.updateUnityTrailTimeScale(1.0F);

    // FIFO returns the first object, whose disabled parking renderer survived
    // FXTouch.Stop. A LIFO pool would return the normal second object instead.
    runtime.pointerDown(PointF{900.0F, 500.0F}, goldenViewport, 1100ms);
    BAFX_CHECK(runtime.snapshot(goldenViewport, 1100ms).trail.empty());
    runtime.pointerUp(1110ms);
    runtime.advance(2110ms);
    runtime.onFrameRendered(2110ms);

    runtime.pointerDown(PointF{1200.0F, 700.0F}, goldenViewport, 2200ms);
    BAFX_CHECK(runtime.snapshot(goldenViewport, 2200ms).trail.size() == 1U);
}

BAFX_TEST(always_on_trail_never_enters_the_unity_fx_touch_pool)
{
    SimulationRuntime runtime;
    runtime.setAlwaysOnTrailEnabled(true, 0ns);
    runtime.pointerMove(PointF{100.0F, 100.0F}, goldenViewport, 10ms);
    runtime.pointerMove(PointF{300.0F, 100.0F}, goldenViewport, 20ms);
    runtime.endAlwaysOnTrail(30ms);
    runtime.advance(1030ms);
    runtime.onFrameRendered(1030ms);

    BAFX_CHECK(runtime.instanceCount() == 0U);
    BAFX_CHECK(runtime.pooledInstanceCount() == 1U);
}

BAFX_TEST(always_on_trail_random_stream_does_not_perturb_unity_clicks)
{
    constexpr std::uint64_t seed = 0x12345678U;
    SimulationRuntime strictRuntime(seed);
    SimulationRuntime enhancedRuntime(seed);

    enhancedRuntime.setAlwaysOnTrailEnabled(true, 0ns);
    enhancedRuntime.pointerMove(
        PointF{100.0F, 100.0F},
        goldenViewport,
        10ms);
    enhancedRuntime.pointerMove(
        PointF{300.0F, 100.0F},
        goldenViewport,
        20ms);
    enhancedRuntime.endAlwaysOnTrail(30ms);
    enhancedRuntime.advance(1030ms);
    enhancedRuntime.onFrameRendered(1030ms);
    BAFX_CHECK(enhancedRuntime.instanceCount() == 0U);

    strictRuntime.pointerDown(goldenCenter, goldenViewport, 1100ms);
    enhancedRuntime.pointerDown(goldenCenter, goldenViewport, 1100ms);
    const FrameSnapshot strictFrame = strictRuntime.snapshot(
        goldenViewport,
        1150ms);
    const FrameSnapshot enhancedFrame = enhancedRuntime.snapshot(
        goldenViewport,
        1150ms);

    BAFX_CHECK(strictFrame.sprites.size() == enhancedFrame.sprites.size());
    for (std::size_t index = 0U; index < strictFrame.sprites.size(); ++index)
    {
        checkSpriteEqual(
            strictFrame.sprites[index],
            enhancedFrame.sprites[index]);
    }
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
        checkSpriteEqual(
            firstFrame.sprites[index],
            secondFrame.sprites[index]);
    }
}
