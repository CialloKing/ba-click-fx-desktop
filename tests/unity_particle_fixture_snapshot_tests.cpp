#include "test_support.hpp"

#include "bafx/reference/unity_particle_fixture.hpp"

#include <array>
#include <string_view>
#include <vector>

namespace
{

using bafx::fx::FrameSnapshot;
using bafx::fx::Sprite;
using bafx::fx::SpriteKind;

[[nodiscard]] std::vector<const Sprite*> spritesOfKind(
    const FrameSnapshot& snapshot,
    const SpriteKind kind)
{
    std::vector<const Sprite*> matches;
    for (const Sprite& sprite : snapshot.sprites)
    {
        if (sprite.kind == kind)
        {
            matches.push_back(&sprite);
        }
    }
    return matches;
}

}

BAFX_TEST(unity_particle_fixture_v2_descriptor_locks_source_observation)
{
    const bafx::reference::UnityParticleFixtureDescriptor descriptor =
        bafx::reference::unityParticleFixtureV2Descriptor();

    BAFX_CHECK(descriptor.schema == 2U);
    BAFX_CHECK(descriptor.fixture == "UnityParticleStateV2");
    BAFX_CHECK(
        descriptor.sourceSha256
        == "1BECBED5019111D8C0F1D9D9A3808B72DF9586E55B960A1DF30DBE4438BECCCD");
    BAFX_CHECK(descriptor.viewport.width == 1950U);
    BAFX_CHECK(descriptor.viewport.height == 1097U);
    BAFX_CHECK(descriptor.ageMilliseconds == 50U);
    BAFX_CHECK(descriptor.particleCount == 7U);
}

BAFX_TEST(unity_particle_fixture_v2_maps_counts_and_materials)
{
    const FrameSnapshot snapshot =
        bafx::reference::makeUnityParticleFixtureV2Snapshot();
    const auto disks = spritesOfKind(snapshot, SpriteKind::CenterDisk);
    const auto rings = spritesOfKind(snapshot, SpriteKind::DissolveRing);
    const auto triangles = spritesOfKind(snapshot, SpriteKind::Triangle);

    BAFX_CHECK(snapshot.active);
    BAFX_CHECK(snapshot.pointerHeld);
    BAFX_CHECK(snapshot.sprites.size() == 7U);
    BAFX_CHECK(disks.size() == 1U);
    BAFX_CHECK(rings.size() == 2U);
    BAFX_CHECK(triangles.size() == 4U);

    BAFX_CHECK(disks[0]->renderQueue == 4499);
    BAFX_CHECK_NEAR(disks[0]->artisticIntensity, 2.0F, 1.0e-6F);
    BAFX_CHECK(disks[0]->contributesBloom);
    for (const Sprite* const ring : rings)
    {
        BAFX_CHECK(ring->renderQueue == 4499);
        BAFX_CHECK_NEAR(ring->artisticIntensity, 5.992157F, 1.0e-6F);
        BAFX_CHECK_NEAR(ring->dissolveThreshold, 1.0F, 1.0e-6F);
        BAFX_CHECK(ring->contributesBloom);
    }
    for (const Sprite* const triangle : triangles)
    {
        BAFX_CHECK(triangle->renderQueue == 4550);
        BAFX_CHECK_NEAR(triangle->artisticIntensity, 5.992157F, 1.0e-6F);
        BAFX_CHECK(triangle->contributesBloom);
        BAFX_CHECK(triangle->atlasFrame == 0U);
    }
}

BAFX_TEST(unity_particle_fixture_v2_maps_projection_size_rotation_and_color)
{
    const FrameSnapshot snapshot =
        bafx::reference::makeUnityParticleFixtureV2Snapshot();
    const auto disks = spritesOfKind(snapshot, SpriteKind::CenterDisk);
    const auto rings = spritesOfKind(snapshot, SpriteKind::DissolveRing);
    const auto triangles = spritesOfKind(snapshot, SpriteKind::Triangle);

    BAFX_CHECK_NEAR(disks[0]->centerPixels.x, 974.999939F, 1.0e-4F);
    BAFX_CHECK_NEAR(disks[0]->centerPixels.y, 548.5F, 1.0e-4F);
    BAFX_CHECK_NEAR(disks[0]->sizePixels, 78.187665F, 1.0e-4F);
    BAFX_CHECK_NEAR(disks[0]->rotationRadians, 3.06379056F, 1.0e-5F);
    BAFX_CHECK_NEAR(disks[0]->color.r, 0.046665087F, 1.0e-6F);
    BAFX_CHECK_NEAR(disks[0]->color.g, 0.127437685F, 1.0e-6F);
    BAFX_CHECK_NEAR(disks[0]->color.b, 1.0F, 1.0e-6F);
    BAFX_CHECK_NEAR(disks[0]->color.a, 0.980392158F, 1.0e-6F);

    constexpr std::array ringSizes{76.850912F, 76.984872F};
    constexpr std::array ringRotations{2.66802382F, 2.06584190F};
    for (std::size_t index = 0U; index < rings.size(); ++index)
    {
        BAFX_CHECK_NEAR(rings[index]->centerPixels.x, 974.999939F, 1.0e-4F);
        BAFX_CHECK_NEAR(rings[index]->centerPixels.y, 548.5F, 1.0e-4F);
        BAFX_CHECK_NEAR(rings[index]->sizePixels, ringSizes[index], 1.0e-4F);
        BAFX_CHECK_NEAR(
            rings[index]->rotationRadians,
            ringRotations[index],
            1.0e-5F);
    }

    constexpr std::array triangleCenters{
        bafx::fx::PointF{981.6985F, 600.1938F},
        bafx::fx::PointF{931.261963F, 576.887146F},
        bafx::fx::PointF{1027.1825F, 545.366638F},
        bafx::fx::PointF{976.392334F, 496.3735F}};
    constexpr std::array triangleSizes{
        5.0314569F,
        3.2104833F,
        3.4153263F,
        4.9113955F};
    for (std::size_t index = 0U; index < triangles.size(); ++index)
    {
        BAFX_CHECK_NEAR(
            triangles[index]->centerPixels.x,
            triangleCenters[index].x,
            1.0e-4F);
        BAFX_CHECK_NEAR(
            triangles[index]->centerPixels.y,
            triangleCenters[index].y,
            1.0e-4F);
        BAFX_CHECK_NEAR(
            triangles[index]->sizePixels,
            triangleSizes[index],
            1.0e-4F);
        BAFX_CHECK_NEAR(triangles[index]->rotationRadians, 0.0F, 1.0e-7F);
        BAFX_CHECK_NEAR(triangles[index]->color.r, 0.25015828F, 1.0e-6F);
        BAFX_CHECK_NEAR(triangles[index]->color.g, 0.25015828F, 1.0e-6F);
        BAFX_CHECK_NEAR(triangles[index]->color.b, 0.25015828F, 1.0e-6F);
        BAFX_CHECK_NEAR(triangles[index]->color.a, 1.0F, 1.0e-6F);
    }
}
