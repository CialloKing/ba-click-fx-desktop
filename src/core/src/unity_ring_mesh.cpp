#include "bafx/core/unity_ring_mesh.hpp"

#include <cmath>

namespace bafx::core
{
namespace
{

constexpr float pi = 3.14159265358979323846F;
constexpr float firstAngleRadians = 95.625F * pi / 180.0F;
constexpr float angleStepRadians = 5.625F * pi / 180.0F;
constexpr float firstU = 0.9995F;
constexpr float lastU = 0.0005F;
constexpr float innerV = 0.0005F;
constexpr float outerV = 0.9995F;

}

UnityRingMesh makeUnityRingMesh() noexcept
{
    UnityRingMesh mesh{};
    for (std::size_t segment = 0U;
         segment <= unityRingSegmentCount;
         ++segment)
    {
        const float progress = static_cast<float>(segment)
            / static_cast<float>(unityRingSegmentCount);
        const float angle = firstAngleRadians
            + angleStepRadians * static_cast<float>(segment);
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        const float u = firstU + (lastU - firstU) * progress;
        const std::size_t vertex = segment * 2U;
        mesh.vertices[vertex] = UnityRingVertex{
            cosine * unityRingInnerRadius,
            sine * unityRingInnerRadius,
            u,
            innerV};
        mesh.vertices[vertex + 1U] = UnityRingVertex{
            cosine * unityRingOuterRadius,
            sine * unityRingOuterRadius,
            u,
            outerV};
    }

    for (std::size_t segment = 0U;
         segment < unityRingSegmentCount;
         ++segment)
    {
        const std::uint16_t inner = static_cast<std::uint16_t>(segment * 2U);
        const std::uint16_t outer = static_cast<std::uint16_t>(inner + 1U);
        const std::uint16_t nextInner = static_cast<std::uint16_t>(inner + 2U);
        const std::uint16_t nextOuter = static_cast<std::uint16_t>(inner + 3U);
        const std::size_t index = segment * 6U;

        // Cylinder002 faces -Z; preserve its clockwise local winding.
        mesh.indices[index] = nextInner;
        mesh.indices[index + 1U] = outer;
        mesh.indices[index + 2U] = inner;
        mesh.indices[index + 3U] = nextInner;
        mesh.indices[index + 4U] = nextOuter;
        mesh.indices[index + 5U] = outer;
    }
    return mesh;
}

}
