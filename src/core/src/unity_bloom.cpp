#include "bafx/core/unity_bloom.hpp"

#include <algorithm>
#include <cmath>

namespace bafx::core
{
namespace
{

constexpr float naturalLogarithmOfTwo = 0.6931471805599453F;
constexpr float maximumDiffusion = 10.0F;
constexpr float minimumDiffusion = 1.0F;

[[nodiscard]] std::int32_t makeInitialDimension(
    const std::int32_t sourceDimension,
    const float stretch) noexcept
{
    // The reconstruction uses Math.Floor with a double divisor before integer mips.
    const double divisor = 2.0 - static_cast<double>(stretch);
    const double scaled = static_cast<double>(sourceDimension) / divisor;
    return std::max(1, static_cast<std::int32_t>(std::floor(scaled)));
}

}

UnityBloomPlanResult planUnityBloom(
    const BloomExtent sourceExtent,
    const UnityBloomSettings& settings) noexcept
{
    UnityBloomPlanResult result{};

    if (sourceExtent.width <= 0 || sourceExtent.height <= 0)
    {
        result.status = UnityBloomStatus::InvalidSourceExtent;
        return result;
    }

    if (!std::isfinite(settings.diffusion)
        || settings.diffusion < minimumDiffusion)
    {
        result.status = UnityBloomStatus::InvalidDiffusion;
        return result;
    }

    if (!std::isfinite(settings.anamorphicRatio))
    {
        result.status = UnityBloomStatus::InvalidAnamorphicRatio;
        return result;
    }

    if (!std::isfinite(settings.intensity) || settings.intensity < 0.0F)
    {
        result.status = UnityBloomStatus::InvalidIntensity;
        return result;
    }

    const float intensityExponent =
        settings.intensity / 10.0F * naturalLogarithmOfTwo;
    // Keep Unity's float Exp-minus-one ordering; expm1 would change tiny values.
    const float exposureGain = std::exp(intensityExponent) - 1.0F;
    if (!std::isfinite(exposureGain))
    {
        result.status = UnityBloomStatus::InvalidIntensity;
        return result;
    }

    const float anamorphicRatio = std::clamp(
        settings.anamorphicRatio,
        -1.0F,
        1.0F);
    const float horizontalStretch = std::max(-anamorphicRatio, 0.0F);
    const float verticalStretch = std::max(anamorphicRatio, 0.0F);
    std::int32_t width = makeInitialDimension(
        sourceExtent.width,
        horizontalStretch);
    std::int32_t height = makeInitialDimension(
        sourceExtent.height,
        verticalStretch);

    const float logSize = std::log2(
        static_cast<float>(std::max(width, height)));
    const float logIterations = logSize
        + std::min(settings.diffusion, maximumDiffusion)
        - maximumDiffusion;
    const float iterationFloorValue = std::floor(logIterations);
    const auto iterationFloor = static_cast<std::int32_t>(iterationFloorValue);
    const auto mipCount = static_cast<std::uint8_t>(std::clamp(
        iterationFloor,
        1,
        static_cast<std::int32_t>(unityBloomMaxMipCount)));

    result.plan.mipCount = mipCount;
    result.plan.sampleScale =
        0.5F + logIterations - iterationFloorValue;
    result.plan.exposureGain = exposureGain;
    result.plan.mipChain[0] = BloomExtent{width, height};

    for (std::size_t index = 1U; index < mipCount; ++index)
    {
        // Unity performs truncating positive integer division at every level.
        width = std::max(1, width / 2);
        height = std::max(1, height / 2);
        result.plan.mipChain[index] = BloomExtent{width, height};
    }

    return result;
}

}
