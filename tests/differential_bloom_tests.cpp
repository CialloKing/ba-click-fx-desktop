#include "test_support.hpp"

#include "bafx/core/differential_bloom.hpp"

#include <algorithm>
#include <array>
#include <limits>

using namespace bafx::core;

namespace
{

Float3 softThreshold(const Float3 value)
{
    return Float3{
        std::max(value.r - 1.0F, 0.0F),
        std::max(value.g - 1.0F, 0.0F),
        std::max(value.b - 1.0F, 0.0F)};
}

}

BAFX_TEST(differential_contract_requires_every_math_property)
{
    DifferentialBloomContract contract{
        true, true, true, true, true, true, true, true, true, true};
    BAFX_CHECK(validateMathContract(contract) == MathContractStatus::Valid);
    contract.pyramidWeightsAreNonNegative = false;
    BAFX_CHECK(validateMathContract(contract) == MathContractStatus::Invalid);

    constexpr std::array positiveKernel{0.25F, 0.5F, 0.25F};
    constexpr std::array negativeLobe{-0.25F, 1.5F, -0.25F};
    BAFX_CHECK(hasOnlyFiniteNonNegativeWeights(positiveKernel));
    BAFX_CHECK(!hasOnlyFiniteNonNegativeWeights(negativeLobe));
}

BAFX_TEST(differential_prefilter_uses_max_of_background_plus_emission)
{
    const auto prepared = prepareDifferentialInputs(
        Float3{-0.25F, 0.5F, 2.0F},
        Float3{0.5F, 0.75F, 0.0F},
        DifferentialLimits{});
    BAFX_CHECK(prepared.status == DifferentialStatus::Ok);
    BAFX_CHECK_NEAR(prepared.inputs.base.r, 0.0F, 0.0F);
    BAFX_CHECK_NEAR(prepared.inputs.withFx.r, 0.25F, 0.0F);
    BAFX_CHECK_NEAR(prepared.inputs.withFx.g, 1.25F, 0.0F);

    const auto delta = finishDifferentialPrefilter(
        softThreshold(prepared.inputs.base),
        softThreshold(prepared.inputs.withFx),
        DifferentialLimits{});
    BAFX_CHECK(delta.status == DifferentialStatus::Ok);
    BAFX_CHECK_NEAR(delta.seed.r, 0.0F, 0.0F);
    BAFX_CHECK_NEAR(delta.seed.g, 0.25F, 1.0e-6F);
    BAFX_CHECK_NEAR(delta.seed.b, 0.0F, 0.0F);
}

BAFX_TEST(differential_prefilter_rejects_contract_violations)
{
    const auto result = finishDifferentialPrefilter(
        Float3{2.0F, 0.0F, 0.0F},
        Float3{1.0F, 0.0F, 0.0F},
        DifferentialLimits{});
    BAFX_CHECK(result.status == DifferentialStatus::ContractViolation);
    BAFX_CHECK_NEAR(result.seed.r, 0.0F, 0.0F);
}

BAFX_TEST(differential_prefilter_sanitizes_roundoff_and_nonfinite_pixels)
{
    DifferentialLimits limits{};
    limits.negativeRoundoffTolerance = 1.0e-4F;
    auto result = finishDifferentialPrefilter(
        Float3{1.0F, 0.0F, 0.0F},
        Float3{0.99995F, 0.0F, 0.0F},
        limits);
    BAFX_CHECK(result.status == DifferentialStatus::RoundoffClamped);

    result = finishDifferentialPrefilter(
        Float3{0.0F, 0.0F, 0.0F},
        Float3{std::numeric_limits<float>::infinity(), 0.0F, 0.0F},
        limits);
    BAFX_CHECK(result.status == DifferentialStatus::InvalidPrefilterOutput);

    const auto prepared = prepareDifferentialInputs(
        Float3{},
        Float3{-0.1F, 1.0F, 1.0F},
        limits);
    BAFX_CHECK(prepared.status == DifferentialStatus::InvalidEmission);
}

BAFX_TEST(soft_threshold_is_isotone_on_sampled_nonnegative_domain)
{
    for (int lower = 0; lower <= 16; ++lower)
    {
        for (int upper = lower; upper <= 16; ++upper)
        {
            const float x = static_cast<float>(lower) * 0.25F;
            const float y = static_cast<float>(upper) * 0.25F;
            const auto hx = softThreshold(Float3{x, x, x});
            const auto hy = softThreshold(Float3{y, y, y});
            BAFX_CHECK(hx.r <= hy.r);
            BAFX_CHECK(hx.g <= hy.g);
            BAFX_CHECK(hx.b <= hy.b);
        }
    }
}

