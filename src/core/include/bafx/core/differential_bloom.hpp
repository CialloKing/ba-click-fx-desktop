#pragma once

#include "bafx/core/types.hpp"

#include <cstdint>
#include <span>

namespace bafx::core
{

struct DifferentialBloomContract
{
    bool inputIsLinearScRgb{false};
    bool prefilterIsPointwise{false};
    bool prefilterIsIsotoneOnNonNegativeDomain{false};
    bool prefilterOutputIsNonNegative{false};
    bool pyramidIsLinear{false};
    bool pyramidHasZeroBias{false};
    bool pyramidWeightsAreNonNegative{false};
    bool pyramidHasFiniteSupport{false};
    bool pyramidUsesFixedMipPhase{false};
    bool pyramidUsesZeroBorder{false};
};

enum class MathContractStatus : std::uint8_t
{
    Valid,
    Invalid
};

[[nodiscard]] MathContractStatus validateMathContract(
    const DifferentialBloomContract& contract) noexcept;

[[nodiscard]] bool hasOnlyFiniteNonNegativeWeights(
    std::span<const float> weights) noexcept;

struct DifferentialLimits
{
    float maxBackgroundMagnitude{16.0F};
    float maxBloomSeed{16.0F};
    float maxPrefilterInput{32.0F};
    float maxDifferentialSeed{16.0F};
    float negativeRoundoffTolerance{1.0e-6F};
};

enum class DifferentialStatus : std::uint8_t
{
    Ok,
    Clamped,
    RoundoffClamped,
    ContractViolation,
    InvalidBackground,
    InvalidEmission,
    InvalidPrefilterOutput,
    InvalidLimits
};

struct DifferentialInputs
{
    Float3 base{};
    Float3 withFx{};
};

struct DifferentialInputsResult
{
    DifferentialInputs inputs{};
    DifferentialStatus status{DifferentialStatus::Ok};
};

[[nodiscard]] DifferentialInputsResult prepareDifferentialInputs(
    Float3 background,
    Float3 bloomSeed,
    const DifferentialLimits& limits) noexcept;

struct DifferentialSeedResult
{
    Float3 seed{};
    DifferentialStatus status{DifferentialStatus::Ok};
};

[[nodiscard]] DifferentialSeedResult finishDifferentialPrefilter(
    Float3 hBase,
    Float3 hWithFx,
    const DifferentialLimits& limits) noexcept;

}

