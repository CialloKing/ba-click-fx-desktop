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

    // Web Bloom parameter equivalents. Defaults reproduce the extracted
    // shader constants and therefore preserve the existing Golden output.
    float threshold{1.0F};
    float softKnee{0.0F};
    float clampValue{65472.0F};
};

}
