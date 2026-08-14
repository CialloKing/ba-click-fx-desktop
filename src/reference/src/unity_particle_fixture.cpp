#include "bafx/reference/unity_particle_fixture.hpp"

#include "bafx/core/color_space.hpp"

#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace bafx::reference
{
namespace
{

constexpr std::uint32_t fixtureSchema = 2U;
constexpr std::string_view fixtureName = "UnityParticleStateV2";
constexpr bafx::fx::Viewport fixtureViewport{1950U, 1097U};
constexpr float unityRingMeshDiameter = 2.127337F;

enum class UnityParticleSystemKind
{
    CenterDisk,
    DissolveRing,
    Triangle
};

struct FixturePoint
{
    float x{0.0F};
    float y{0.0F};
};

struct FixtureRotation
{
    float z{0.0F};
    float w{1.0F};
};

struct FixtureColor
{
    float r{0.0F};
    float g{0.0F};
    float b{0.0F};
    float a{0.0F};
};

struct UnityParticleObservation
{
    UnityParticleSystemKind system{UnityParticleSystemKind::CenterDisk};
    FixturePoint projectedBottomLeftPixels{};
    float systemScale{1.0F};
    float size{0.0F};
    FixtureRotation rotation{};
    FixtureColor gammaColor{};
    float custom1X{0.0F};
    std::uint32_t atlasFrame{0U};
};

#include "unity_particle_fixture_v2.generated.inc"

[[nodiscard]] float planarRotationRadians(
    const FixtureRotation rotation) noexcept
{
    float angle = 2.0F * std::atan2(rotation.z, rotation.w);
    if (angle < 0.0F)
    {
        angle += 2.0F * std::numbers::pi_v<float>;
    }
    return angle;
}

[[nodiscard]] bafx::fx::ColorF linearColor(
    const FixtureColor color) noexcept
{
    const bafx::core::Float3 rgb = bafx::core::srgbToLinear(
        bafx::core::Float3{color.r, color.g, color.b});
    return bafx::fx::ColorF{rgb.r, rgb.g, rgb.b, color.a};
}

[[nodiscard]] bafx::fx::Sprite makeSprite(
    const UnityParticleObservation& observation) noexcept
{
    bafx::fx::Sprite sprite{};
    sprite.centerPixels = bafx::fx::PointF{
        observation.projectedBottomLeftPixels.x,
        static_cast<float>(fixtureViewport.height)
            - observation.projectedBottomLeftPixels.y};
    sprite.sizePixels = observation.size
        * observation.systemScale
        * (static_cast<float>(fixtureViewport.height) * 0.5F);
    sprite.rotationRadians = planarRotationRadians(observation.rotation);
    sprite.color = linearColor(observation.gammaColor);
    sprite.atlasFrame = observation.atlasFrame;
    sprite.globalScalePivotPixels = sprite.centerPixels;

    switch (observation.system)
    {
    case UnityParticleSystemKind::CenterDisk:
    {
        sprite.kind = bafx::fx::SpriteKind::CenterDisk;
        sprite.artisticIntensity = 2.0F;
        sprite.renderQueue = 4499;
        sprite.contributesBloom = true;
        break;
    }

    case UnityParticleSystemKind::DissolveRing:
    {
        sprite.kind = bafx::fx::SpriteKind::DissolveRing;
        sprite.sizePixels *= unityRingMeshDiameter;
        sprite.artisticIntensity = 5.992157F;
        sprite.dissolveThreshold = observation.custom1X;
        sprite.renderQueue = 4499;
        sprite.contributesBloom = true;
        break;
    }

    case UnityParticleSystemKind::Triangle:
    {
        sprite.kind = bafx::fx::SpriteKind::Triangle;
        sprite.artisticIntensity = 5.992157F;
        sprite.renderQueue = 4550;
        sprite.contributesBloom = true;
        break;
    }
    }
    return sprite;
}

}

std::span<const UnityParticleFixtureDescriptor>
unityParticleFixtureV2Descriptors() noexcept
{
    return unityParticleFixtureDescriptors;
}

bafx::fx::FrameSnapshot makeUnityParticleFixtureV2Snapshot(
    const std::uint32_t ageMilliseconds)
{
    std::size_t fixtureIndex = unityParticleFixtureDescriptors.size();
    for (std::size_t index = 0U;
         index < unityParticleFixtureDescriptors.size();
         ++index)
    {
        if (unityParticleFixtureDescriptors[index].ageMilliseconds
            == ageMilliseconds)
        {
            fixtureIndex = index;
            break;
        }
    }
    if (fixtureIndex == unityParticleFixtureDescriptors.size())
    {
        throw std::invalid_argument("Unsupported Unity particle fixture age");
    }

    const std::size_t begin = unityParticleFixtureObservationOffsets[fixtureIndex];
    const std::size_t end = unityParticleFixtureObservationOffsets[fixtureIndex + 1U];
    bafx::fx::FrameSnapshot snapshot{};
    snapshot.active = true;
    snapshot.pointerHeld = true;
    snapshot.sprites.reserve(end - begin);
    for (std::size_t index = begin; index < end; ++index)
    {
        snapshot.sprites.push_back(makeSprite(unityParticleObservations[index]));
    }
    return snapshot;
}

}
