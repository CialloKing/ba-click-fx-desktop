#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace bafx::core
{

inline constexpr std::size_t unityRingSegmentCount = 64U;
inline constexpr std::size_t unityRingVertexCount =
    (unityRingSegmentCount + 1U) * 2U;
inline constexpr std::size_t unityRingIndexCount = unityRingSegmentCount * 6U;
inline constexpr float unityRingInnerRadius = 1.0F;
inline constexpr float unityRingOuterRadius = 1.0636685F;

struct UnityRingVertex
{
    float x{0.0F};
    float y{0.0F};
    float u{0.0F};
    float v{0.0F};
};

struct UnityRingMesh
{
    std::array<UnityRingVertex, unityRingVertexCount> vertices{};
    std::array<std::uint16_t, unityRingIndexCount> indices{};
};

[[nodiscard]] UnityRingMesh makeUnityRingMesh() noexcept;

}
