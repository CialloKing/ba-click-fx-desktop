#include "frame_visual_config.hpp"

#include <algorithm>

namespace bafx::desktop
{

void applyVisualConfig(
    bafx::fx::FrameSnapshot& snapshot,
    const bafx::config::Config& config)
{
    // The public Web value is a radius, while Sprite stores the Unity quad's
    // full extent. Scaling against the Web default preserves the authored
    // native geometry at radius 64.8.
    constexpr float unityDiskRadiusAtReferenceHeight = 64.8F;
    constexpr float unityHdrIntensity = 5.992157F;
    if (!config.effects.enabled)
    {
        snapshot = bafx::fx::FrameSnapshot{};
        return;
    }
    // The renderer applies this after Unity material evaluation. Mutating
    // particle Alpha here would change Dissolve geometry and square emission.
    snapshot.globalOpacity = std::clamp(
        config.effects.opacity,
        0.0F,
        1.0F);

    const float diskRadiusScale = config.effects.diskRadius
        / unityDiskRadiusAtReferenceHeight;
    const float ringsHdrScale = config.effects.ringsHdrIntensity
        / unityHdrIntensity;
    const float shardsHdrScale = config.effects.shardsHdrIntensity
        / unityHdrIntensity;
    for (bafx::fx::Sprite& sprite : snapshot.sprites)
    {
        switch (sprite.kind)
        {
        case bafx::fx::SpriteKind::CenterDisk:
            sprite.sizePixels *= diskRadiusScale;
            break;
        case bafx::fx::SpriteKind::DissolveRing:
            sprite.artisticIntensity *= ringsHdrScale;
            break;
        case bafx::fx::SpriteKind::Triangle:
            sprite.artisticIntensity *= shardsHdrScale;
            break;
        }
    }
    snapshot.trailOpacity *= config.effects.trailOpacity;
    for (bafx::fx::TrailStroke& stroke : snapshot.trailStrokes)
    {
        stroke.opacity *= config.effects.trailOpacity;
    }

    bafx::fx::applyGlobalScale(snapshot, config.effects.globalScale);
    const float trailScale = config.effects.trailWidth;
    snapshot.trailWidthPixels *= trailScale;
    for (bafx::fx::TrailStroke& stroke : snapshot.trailStrokes)
    {
        stroke.widthPixels *= trailScale;
    }
}

[[nodiscard]] bafx::fx::Viewport toViewport(const bafx::windows::WindowSize size) noexcept
{
    return bafx::fx::Viewport{size.width, size.height};
}

}
