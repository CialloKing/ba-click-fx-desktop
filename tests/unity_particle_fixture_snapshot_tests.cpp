#include "test_support.hpp"

#include "bafx/reference/unity_particle_fixture.hpp"

#include <array>
#include <stdexcept>
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

[[nodiscard]] bool rejectsUnsupportedAge()
{
    try
    {
        static_cast<void>(
            bafx::reference::makeUnityParticleFixtureV2Snapshot(110U));
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }
    return false;
}

}

BAFX_TEST(unity_particle_fixture_v2_descriptor_locks_source_observation)
{
    const auto descriptors =
        bafx::reference::unityParticleFixtureV2Descriptors();
    constexpr std::array ages{50U, 100U, 120U, 250U, 450U};
    constexpr std::array particleCounts{7U, 7U, 7U, 6U, 6U};
    constexpr std::array<std::string_view, 5U> hashes{
        "1BECBED5019111D8C0F1D9D9A3808B72DF9586E55B960A1DF30DBE4438BECCCD",
        "9DB1712C896871CE46EE49FBB0F9340E42EE4599A81A56545081085814CB31F2",
        "8892417B9272E9E18BC0D8596B408DC4B05EDFE3D66E813910A33828499DF993",
        "3A1D0249D738B5448E60758697BE44B76F247A5FDFA571FE5DA981C3343B6E99",
        "CA067C13E8DFB7BA03052F0C83DBE5F5C09A2AC1709DB30571825D9FEBB53F20"};

    BAFX_CHECK(descriptors.size() == ages.size());
    for (std::size_t index = 0U; index < descriptors.size(); ++index)
    {
        const auto& descriptor = descriptors[index];
        BAFX_CHECK(descriptor.schema == 2U);
        BAFX_CHECK(descriptor.fixture == "UnityParticleStateV2");
        BAFX_CHECK(descriptor.sourceFixture.find("Reference/Diagnostics/") == 0U);
        BAFX_CHECK(descriptor.sourceSha256 == hashes[index]);
        BAFX_CHECK(descriptor.viewport.width == 1950U);
        BAFX_CHECK(descriptor.viewport.height == 1097U);
        BAFX_CHECK(descriptor.ageMilliseconds == ages[index]);
        BAFX_CHECK(descriptor.particleCount == particleCounts[index]);
    }
}

BAFX_TEST(unity_particle_fixture_v2_maps_counts_and_materials)
{
    const FrameSnapshot snapshot =
        bafx::reference::makeUnityParticleFixtureV2Snapshot(50U);
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
        bafx::reference::makeUnityParticleFixtureV2Snapshot(50U);
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

BAFX_TEST(unity_particle_fixture_v2_maps_each_locked_age_range)
{
    constexpr std::array ages{50U, 100U, 120U, 250U, 450U};
    constexpr std::array diskCounts{1U, 1U, 1U, 0U, 0U};
    constexpr std::array totalCounts{7U, 7U, 7U, 6U, 6U};
    for (std::size_t index = 0U; index < ages.size(); ++index)
    {
        const FrameSnapshot snapshot =
            bafx::reference::makeUnityParticleFixtureV2Snapshot(ages[index]);
        BAFX_CHECK(snapshot.sprites.size() == totalCounts[index]);
        BAFX_CHECK(
            spritesOfKind(snapshot, SpriteKind::CenterDisk).size()
            == diskCounts[index]);
        BAFX_CHECK(
            spritesOfKind(snapshot, SpriteKind::DissolveRing).size() == 2U);
        BAFX_CHECK(spritesOfKind(snapshot, SpriteKind::Triangle).size() == 4U);
    }
    BAFX_CHECK(rejectsUnsupportedAge());
}
