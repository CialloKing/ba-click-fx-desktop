#pragma once

namespace bafx::windows
{

struct FxBloomSettings
{
    // A value of one preserves the Unity-derived 1.7 exposure result.
    float intensityMultiplier{1.0F};

    // This is Unity's existing Bloom diffusion control and determines the
    // pyramid depth; it is not an additional desktop-only blur algorithm.
    float diffusion{7.0F};
};

}
