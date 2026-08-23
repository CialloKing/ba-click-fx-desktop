#pragma once

namespace bafx::windows
{

struct FxBloomSettings
{
    // This is the Web/Unity Bloom intensity scalar, not a multiplier around
    // a hidden native constant. The Unity default is 1.7.
    float intensity{1.7F};

    // This is Unity's existing Bloom diffusion control and determines the
    // pyramid depth; it is not an additional desktop-only blur algorithm.
    float diffusion{7.0F};

    // Web Bloom parameter equivalents. Defaults reproduce the extracted
    // shader constants and therefore preserve the existing Golden output.
    float threshold{1.0F};
    float softKnee{0.0F};
    float clampValue{65472.0F};

    // Disabling the layer bypasses the Bloom pyramid while preserving direct
    // materials and every desktop/recording composite contract.
    bool enabled{true};
};

}
