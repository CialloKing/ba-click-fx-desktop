#include "bafx/core/differential_bloom.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace bafx::core
{
namespace
{

[[nodiscard]] bool isFinite(const Float3 value) noexcept
{
    return std::isfinite(value.r) && std::isfinite(value.g) && std::isfinite(value.b);
}

[[nodiscard]] bool isNonNegative(const Float3 value) noexcept
{
    return value.r >= 0.0F && value.g >= 0.0F && value.b >= 0.0F;
}

[[nodiscard]] bool validLimits(const DifferentialLimits& limits) noexcept
{
    return std::isfinite(limits.maxBackgroundMagnitude)
        && limits.maxBackgroundMagnitude > 0.0F
        && std::isfinite(limits.maxBloomSeed)
        && limits.maxBloomSeed > 0.0F
        && std::isfinite(limits.maxPrefilterInput)
        && limits.maxPrefilterInput > 0.0F
        && std::isfinite(limits.maxDifferentialSeed)
        && limits.maxDifferentialSeed > 0.0F
        && std::isfinite(limits.negativeRoundoffTolerance)
        && limits.negativeRoundoffTolerance >= 0.0F;
}

[[nodiscard]] Float3 clampBackground(
    const Float3 value,
    const DifferentialLimits& limits) noexcept
{
    return Float3{
        std::clamp(value.r, -limits.maxBackgroundMagnitude, limits.maxBackgroundMagnitude),
        std::clamp(value.g, -limits.maxBackgroundMagnitude, limits.maxBackgroundMagnitude),
        std::clamp(value.b, -limits.maxBackgroundMagnitude, limits.maxBackgroundMagnitude)};
}

[[nodiscard]] Float3 clampEmission(
    const Float3 value,
    const DifferentialLimits& limits) noexcept
{
    return Float3{
        std::min(value.r, limits.maxBloomSeed),
        std::min(value.g, limits.maxBloomSeed),
        std::min(value.b, limits.maxBloomSeed)};
}

}

MathContractStatus validateMathContract(const DifferentialBloomContract& contract) noexcept
{
    const bool valid = contract.inputIsLinearScRgb
        && contract.prefilterIsPointwise
        && contract.prefilterIsIsotoneOnNonNegativeDomain
        && contract.prefilterOutputIsNonNegative
        && contract.pyramidIsLinear
        && contract.pyramidHasZeroBias
        && contract.pyramidWeightsAreNonNegative
        && contract.pyramidHasFiniteSupport
        && contract.pyramidUsesFixedMipPhase
        && contract.pyramidUsesZeroBorder;
    return valid ? MathContractStatus::Valid : MathContractStatus::Invalid;
}

bool hasOnlyFiniteNonNegativeWeights(const std::span<const float> weights) noexcept
{
    if (weights.empty())
    {
        return false;
    }

    for (const float weight : weights)
    {
        if (!std::isfinite(weight) || weight < 0.0F)
        {
            return false;
        }
    }
    return true;
}

DifferentialInputsResult prepareDifferentialInputs(
    const Float3 background,
    const Float3 bloomSeed,
    const DifferentialLimits& limits) noexcept
{
    DifferentialInputsResult result{};
    if (!validLimits(limits))
    {
        result.status = DifferentialStatus::InvalidLimits;
        return result;
    }

    if (!isFinite(background))
    {
        result.status = DifferentialStatus::InvalidBackground;
        return result;
    }

    if (!isFinite(bloomSeed) || !isNonNegative(bloomSeed))
    {
        result.status = DifferentialStatus::InvalidEmission;
        return result;
    }

    const Float3 boundedBackground = clampBackground(background, limits);
    const Float3 boundedEmission = clampEmission(bloomSeed, limits);
    const bool clamped = boundedBackground.r != background.r
        || boundedBackground.g != background.g
        || boundedBackground.b != background.b
        || boundedEmission.r != bloomSeed.r
        || boundedEmission.g != bloomSeed.g
        || boundedEmission.b != bloomSeed.b;

    const auto prepareChannel = [&limits](const float base, const float emission) noexcept
    {
        const float nonNegativeBase = std::max(base, 0.0F);
        const float nonNegativeWithFx = std::max(base + emission, 0.0F);
        return std::array<float, 2>{
            std::min(nonNegativeBase, limits.maxPrefilterInput),
            std::min(nonNegativeWithFx, limits.maxPrefilterInput)};
    };

    const auto red = prepareChannel(boundedBackground.r, boundedEmission.r);
    const auto green = prepareChannel(boundedBackground.g, boundedEmission.g);
    const auto blue = prepareChannel(boundedBackground.b, boundedEmission.b);
    result.inputs.base = Float3{red[0], green[0], blue[0]};
    result.inputs.withFx = Float3{red[1], green[1], blue[1]};

    const bool prefilterClamped = std::max(boundedBackground.r, 0.0F)
            > limits.maxPrefilterInput
        || std::max(boundedBackground.r + boundedEmission.r, 0.0F)
            > limits.maxPrefilterInput
        || std::max(boundedBackground.g, 0.0F)
            > limits.maxPrefilterInput
        || std::max(boundedBackground.g + boundedEmission.g, 0.0F)
            > limits.maxPrefilterInput
        || std::max(boundedBackground.b, 0.0F)
            > limits.maxPrefilterInput
        || std::max(boundedBackground.b + boundedEmission.b, 0.0F)
            > limits.maxPrefilterInput;
    result.status = (clamped || prefilterClamped)
        ? DifferentialStatus::Clamped
        : DifferentialStatus::Ok;
    return result;
}

DifferentialSeedResult finishDifferentialPrefilter(
    const Float3 hBase,
    const Float3 hWithFx,
    const DifferentialLimits& limits) noexcept
{
    DifferentialSeedResult result{};
    if (!validLimits(limits))
    {
        result.status = DifferentialStatus::InvalidLimits;
        return result;
    }

    if (!isFinite(hBase) || !isFinite(hWithFx) || !isNonNegative(hBase) || !isNonNegative(hWithFx))
    {
        result.status = DifferentialStatus::InvalidPrefilterOutput;
        return result;
    }

    const std::array<float, 3> raw{
        hWithFx.r - hBase.r,
        hWithFx.g - hBase.g,
        hWithFx.b - hBase.b};
    for (const float value : raw)
    {
        if (value < -limits.negativeRoundoffTolerance)
        {
            // A material negative delta means H violated the required isotonic contract.
            result.status = DifferentialStatus::ContractViolation;
            return result;
        }
    }

    bool roundoffClamped = false;
    bool upperClamped = false;
    std::array<float, 3> sanitized{};
    for (std::size_t index = 0; index < raw.size(); ++index)
    {
        if (raw[index] < 0.0F)
        {
            sanitized[index] = 0.0F;
            roundoffClamped = true;
        }
        else if (raw[index] > limits.maxDifferentialSeed)
        {
            sanitized[index] = limits.maxDifferentialSeed;
            upperClamped = true;
        }
        else
        {
            sanitized[index] = raw[index];
        }
    }

    result.seed = Float3{sanitized[0], sanitized[1], sanitized[2]};
    if (upperClamped)
    {
        result.status = DifferentialStatus::Clamped;
    }
    else if (roundoffClamped)
    {
        result.status = DifferentialStatus::RoundoffClamped;
    }
    return result;
}

}
