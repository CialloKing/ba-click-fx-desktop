#include "test_support.hpp"

#include "bafx/core/unity_ring_mesh.hpp"

#include <algorithm>
#include <cmath>

using namespace bafx::core;

namespace
{

[[nodiscard]] float radius(const UnityRingVertex& vertex) noexcept
{
    return std::sqrt(vertex.x * vertex.x + vertex.y * vertex.y);
}

[[nodiscard]] float signedDoubleArea(
    const UnityRingVertex& a,
    const UnityRingVertex& b,
    const UnityRingVertex& c) noexcept
{
    return (b.x - a.x) * (c.y - a.y)
        - (b.y - a.y) * (c.x - a.x);
}

}

BAFX_TEST(unity_ring_mesh_matches_cylinder002_golden_geometry)
{
    const UnityRingMesh mesh = makeUnityRingMesh();
    BAFX_CHECK(mesh.vertices.size() == 130U);
    BAFX_CHECK(mesh.indices.size() == 384U);

    const UnityRingVertex& firstInner = mesh.vertices[0];
    const UnityRingVertex& firstOuter = mesh.vertices[1];
    BAFX_CHECK_NEAR(firstInner.x, -0.09801714F, 4.0e-6F);
    BAFX_CHECK_NEAR(firstInner.y, 0.99518473F, 4.0e-6F);
    BAFX_CHECK_NEAR(firstOuter.x, -0.10425763F, 4.0e-6F);
    BAFX_CHECK_NEAR(firstOuter.y, 1.0585467F, 4.0e-6F);
    BAFX_CHECK_NEAR(radius(firstInner), unityRingInnerRadius, 1.0e-6F);
    BAFX_CHECK_NEAR(radius(firstOuter), unityRingOuterRadius, 1.0e-6F);
    BAFX_CHECK_NEAR(firstInner.u, 0.9995F, 1.0e-7F);
    BAFX_CHECK_NEAR(firstInner.v, 0.0005F, 1.0e-7F);
    BAFX_CHECK_NEAR(firstOuter.v, 0.9995F, 1.0e-7F);
}

BAFX_TEST(unity_ring_mesh_closes_only_position_at_the_uv_seam)
{
    const UnityRingMesh mesh = makeUnityRingMesh();
    const UnityRingVertex& firstInner = mesh.vertices[0];
    const UnityRingVertex& lastInner = mesh.vertices[unityRingVertexCount - 2U];
    const UnityRingVertex& firstOuter = mesh.vertices[1];
    const UnityRingVertex& lastOuter = mesh.vertices[unityRingVertexCount - 1U];

    BAFX_CHECK_NEAR(firstInner.x, lastInner.x, 1.0e-6F);
    BAFX_CHECK_NEAR(firstInner.y, lastInner.y, 1.0e-6F);
    BAFX_CHECK_NEAR(firstOuter.x, lastOuter.x, 1.0e-6F);
    BAFX_CHECK_NEAR(firstOuter.y, lastOuter.y, 1.0e-6F);
    BAFX_CHECK_NEAR(firstInner.u, 0.9995F, 1.0e-7F);
    BAFX_CHECK_NEAR(lastInner.u, 0.0005F, 1.0e-7F);
}

BAFX_TEST(unity_ring_mesh_preserves_clockwise_asset_winding)
{
    const UnityRingMesh mesh = makeUnityRingMesh();
    for (std::size_t index = 0U; index < mesh.indices.size(); index += 3U)
    {
        const UnityRingVertex& a = mesh.vertices[mesh.indices[index]];
        const UnityRingVertex& b = mesh.vertices[mesh.indices[index + 1U]];
        const UnityRingVertex& c = mesh.vertices[mesh.indices[index + 2U]];
        BAFX_CHECK(signedDoubleArea(a, b, c) < 0.0F);
    }
}

BAFX_TEST(unity_ring_mesh_matches_the_original_bounds)
{
    const UnityRingMesh mesh = makeUnityRingMesh();
    float minimumX = mesh.vertices[0].x;
    float maximumX = mesh.vertices[0].x;
    float minimumY = mesh.vertices[0].y;
    float maximumY = mesh.vertices[0].y;
    for (const UnityRingVertex& vertex : mesh.vertices)
    {
        minimumX = std::min(minimumX, vertex.x);
        maximumX = std::max(maximumX, vertex.x);
        minimumY = std::min(minimumY, vertex.y);
        maximumY = std::max(maximumY, vertex.y);
    }

    BAFX_CHECK_NEAR(minimumX, -unityRingOuterRadius, 1.0e-6F);
    BAFX_CHECK_NEAR(maximumX, unityRingOuterRadius, 1.0e-6F);
    BAFX_CHECK_NEAR(minimumY, -unityRingOuterRadius, 1.0e-6F);
    BAFX_CHECK_NEAR(maximumY, unityRingOuterRadius, 1.0e-6F);
}
