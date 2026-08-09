#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace bafx::core
{

inline constexpr std::size_t unityBloomMaxMipCount = 16U;

struct BloomExtent
{
    std::int32_t width{0};
    std::int32_t height{0};
};

struct UnityBloomSettings
{
    float diffusion{7.0F};
    float anamorphicRatio{0.0F};

    // This is Unity's artistic exposure control, not a physical luminance unit.
    float intensity{1.7F};
};

enum class UnityBloomStatus : std::uint8_t
{
    Ok,
    InvalidSourceExtent,
    InvalidDiffusion,
    InvalidAnamorphicRatio,
    InvalidIntensity
};

struct UnityBloomPlan
{
    std::array<BloomExtent, unityBloomMaxMipCount> mipChain{};
    std::uint8_t mipCount{0};
    float sampleScale{0.0F};
    float exposureGain{0.0F};
};

struct UnityBloomPlanResult
{
    UnityBloomPlan plan{};
    UnityBloomStatus status{UnityBloomStatus::Ok};
};

[[nodiscard]] UnityBloomPlanResult planUnityBloom(
    BloomExtent sourceExtent,
    const UnityBloomSettings& settings = UnityBloomSettings{}) noexcept;

}
