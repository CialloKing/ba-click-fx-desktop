#include "test_support.hpp"

#include "bafx/fx/simulation.hpp"

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

BAFX_TEST(dissolve_ring_is_most_open_at_120ms)
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
        0.62384260F,
        0.07407415F,
        0.01967585F,
        0.0F,
        0.03993887F,
        0.22559442F,
        0.44780129F,
        0.86553848F};

    for (std::size_t index = 0U; index < sampleTimes.size(); ++index)
    {
        const auto frame = simulation.snapshot(goldenViewport, sampleTimes[index]);
        const Sprite& ring = firstKind(frame, SpriteKind::DissolveRing);
        BAFX_CHECK_NEAR(ring.dissolveThreshold, expectedThresholds[index], 1.0e-6F);
        BAFX_CHECK(ring.contributesBloom);
    }
}

BAFX_TEST(unity_hermite_size_curves_match_golden_samples)
{
    Simulation simulation;
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);

    const auto diskFrame = simulation.snapshot(goldenViewport, 100ms);
    BAFX_CHECK_NEAR(
        firstKind(diskFrame, SpriteKind::CenterDisk).sizePixels,
        119.39155F,
        1.0e-3F);

    constexpr std::array expectedRingSizesAt250ms{139.71249F, 124.16064F};
    constexpr std::array expectedRingSizesAt450ms{158.58969F, 140.93652F};
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
        23.86673F,
        22.00195F,
        25.94406F,
        27.92309F};
    constexpr std::array expectedTriangleSizesAt450ms{
        15.52249F,
        15.03560F,
        15.60907F,
        19.12013F};
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

    const auto initialFrame = simulation.snapshot(goldenViewport, 0ns);
    const auto frameAt300ms = simulation.snapshot(goldenViewport, 300ms);
    const auto frameAt600ms = simulation.snapshot(goldenViewport, 600ms);
    const auto initialRings = spritesOfKind(initialFrame, SpriteKind::DissolveRing);
    const auto ringsAt300ms = spritesOfKind(frameAt300ms, SpriteKind::DissolveRing);
    const auto ringsAt600ms = spritesOfKind(frameAt600ms, SpriteKind::DissolveRing);
    BAFX_CHECK(initialRings.size() == 2U);
    BAFX_CHECK(ringsAt300ms.size() == initialRings.size());
    BAFX_CHECK(ringsAt600ms.size() == initialRings.size());

    constexpr std::array expectedAt300ms{3.0705049F, 2.7826068F};
    constexpr std::array expectedAt600ms{4.8338957F, 3.9891858F};
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

BAFX_TEST(unity_color_keys_and_start_color_multiplication_match_golden_samples)
{
    Simulation simulation;
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);

    const auto diskFrame = simulation.snapshot(goldenViewport, 10ms);
    const Sprite& disk = firstKind(diskFrame, SpriteKind::CenterDisk);
    BAFX_CHECK_NEAR(disk.color.r, 0.68512273F, 1.0e-6F);
    BAFX_CHECK_NEAR(disk.color.g, 0.74733746F, 1.0e-6F);
    BAFX_CHECK_NEAR(disk.color.b, 1.0F, 1.0e-6F);
    BAFX_CHECK_NEAR(disk.color.a, 1.0F, 1.0e-6F);

    const auto ringFrame = simulation.snapshot(goldenViewport, 250ms);
    const Sprite& ring = firstKind(ringFrame, SpriteKind::DissolveRing);
    BAFX_CHECK_NEAR(ring.color.r, 0.44804364F, 1.0e-6F);
    BAFX_CHECK_NEAR(ring.color.g, 0.72771418F, 1.0e-6F);
    BAFX_CHECK_NEAR(ring.color.b, 1.0F, 1.0e-6F);

    constexpr std::array expectedColors{
        ColorF{0.20035757F, 0.41559723F, 0.53773582F, 0.07775988F},
        ColorF{0.20036153F, 0.41562453F, 0.53773582F, 0.07909042F},
        ColorF{0.20035212F, 0.41555959F, 0.53773582F, 0.26349252F},
        ColorF{0.20036171F, 0.41562572F, 0.53773582F, 0.08707273F}};
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
    constexpr std::array expectedCenters{
        PointF{1019.79510F, 518.41803F},
        PointF{987.14734F, 496.29846F},
        PointF{999.75006F, 596.50702F},
        PointF{1002.14856F, 594.41406F}};

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
