#pragma once

#include "bafx/core/types.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace bafx::core
{

inline constexpr std::size_t unityTrailCornerVertexCount = 4U;
inline constexpr std::size_t unityTrailCapVertexCount = 1U;

struct UnityTrailPoint
{
    float x{0.0F};
    float y{0.0F};
};

struct UnityTrailVertex
{
    float x{0.0F};
    float y{0.0F};
    float progress{0.0F};
    float transverse{0.0F};
};

struct UnityTrailMesh
{
    std::vector<UnityTrailVertex> vertices{};
};

[[nodiscard]] UnityTrailMesh makeUnityTrailMesh(
    std::span<const UnityTrailPoint> points,
    float width) noexcept;

[[nodiscard]] Float3 evaluateUnityTrailColor(float progress) noexcept;

}
