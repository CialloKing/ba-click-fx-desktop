#include "bafx/core/intensity.hpp"

#include <algorithm>
#include <cmath>

namespace bafx::core
{
namespace
{

constexpr float scRgbNitsPerUnit = 80.0F;

[[nodiscard]] bool isFinitePositive(float value) noexcept
{
    return std::isfinite(value) && value > 0.0F;
}

}

ResolvedIntensity resolveIntensity(
    const AuthoredIntensity intensity,
    const OutputPolicy& policy) noexcept
{
    ResolvedIntensity result{};

    if (!std::isfinite(intensity.value) || intensity.value < 0.0F)
    {
        result.status = ResolveStatus::InvalidInput;
        return result;
    }

    if (!isFinitePositive(policy.maxCanonicalIntensity))
    {
        result.status = ResolveStatus::InvalidPolicy;
        return result;
    }

    float canonicalValue = 0.0F;
    switch (intensity.semantics)
    {
    case IntensitySemantics::ArtisticRelative:
        if (!std::isfinite(policy.artisticToCanonical) || policy.artisticToCanonical < 0.0F)
        {
            result.status = ResolveStatus::InvalidPolicy;
            return result;
        }
        canonicalValue = intensity.value * policy.artisticToCanonical;
        break;

    case IntensitySemantics::ReferenceWhiteRelative:
        if (policy.encoding == OutputEncoding::HdrSceneReferredScRgb)
        {
            if (!isFinitePositive(policy.hdrSdrWhiteNits))
            {
                result.status = ResolveStatus::InvalidPolicy;
                return result;
            }
            canonicalValue = intensity.value * policy.hdrSdrWhiteNits / scRgbNitsPerUnit;
        }
        else
        {
            canonicalValue = intensity.value;
        }
        break;

    case IntensitySemantics::AbsoluteNits:
        if (policy.encoding == OutputEncoding::HdrSceneReferredScRgb)
        {
            canonicalValue = intensity.value / scRgbNitsPerUnit;
        }
        else if (policy.absoluteNitsOnSdr == AbsoluteNitsOnSdr::UseCalibratedPeak)
        {
            if (!isFinitePositive(policy.calibratedSdrPeakNits))
            {
                result.status = ResolveStatus::InvalidPolicy;
                return result;
            }
            canonicalValue = intensity.value / policy.calibratedSdrPeakNits;
            result.approximate = true;
        }
        else
        {
            result.status = ResolveStatus::UnsupportedOnOutput;
            return result;
        }
        break;
    }

    if (!std::isfinite(canonicalValue))
    {
        result.status = ResolveStatus::InvalidInput;
        return result;
    }

    result.canonicalValue = std::min(canonicalValue, policy.maxCanonicalIntensity);
    result.clamped = canonicalValue > policy.maxCanonicalIntensity;
    return result;
}

}

