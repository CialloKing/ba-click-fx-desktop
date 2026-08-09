#include "test_support.hpp"

#include "bafx/fx/simulation.hpp"

#include <algorithm>
#include <chrono>

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

    const auto frame = simulation.snapshot(goldenViewport, 120ms);
    const auto ring = std::find_if(
        frame.sprites.begin(),
        frame.sprites.end(),
        [](const Sprite& sprite)
        {
            return sprite.kind == SpriteKind::DissolveRing;
        });
    BAFX_CHECK(ring != frame.sprites.end());
    BAFX_CHECK_NEAR(ring->dissolveThreshold, 0.0F, 1.0e-6F);
    BAFX_CHECK(ring->contributesBloom);
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
    BAFX_CHECK(frame.trailWidthPixels > 2.0F);
    BAFX_CHECK(frame.trailWidthPixels < 3.0F);
}

BAFX_TEST(release_waits_sixty_rendered_frames_before_clearing)
{
    Simulation simulation;
    simulation.pointerDown(goldenCenter, goldenViewport, 0ns);
    simulation.pointerUp(10ms);
    for (std::uint32_t frame = 0; frame < 59U; ++frame)
    {
        simulation.advance(10ms + std::chrono::milliseconds(frame + 1U));
    }
    BAFX_CHECK(simulation.active());

    simulation.advance(70ms);
    BAFX_CHECK(!simulation.active());
}

