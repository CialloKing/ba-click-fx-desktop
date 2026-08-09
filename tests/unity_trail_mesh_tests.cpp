#include "test_support.hpp"

#include "bafx/core/unity_trail_mesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

using namespace bafx::core;

namespace
{

[[nodiscard]] float signedDoubleArea(
    const UnityTrailVertex& a,
    const UnityTrailVertex& b,
    const UnityTrailVertex& c) noexcept
{
    return (b.x - a.x) * (c.y - a.y)
        - (b.y - a.y) * (c.x - a.x);
}

}

BAFX_TEST(unity_trail_straight_segment_has_triangle_caps_and_exact_bounds)
{
    constexpr std::array points{
        UnityTrailPoint{0.0F, 0.0F},
        UnityTrailPoint{10.0F, 0.0F}};
    const UnityTrailMesh mesh = makeUnityTrailMesh(points, 2.0F);

    BAFX_CHECK(mesh.vertices.size() == 12U);
    float minimumX = mesh.vertices[0].x;
    float maximumX = mesh.vertices[0].x;
    float minimumY = mesh.vertices[0].y;
    float maximumY = mesh.vertices[0].y;
    for (const UnityTrailVertex& vertex : mesh.vertices)
    {
        minimumX = std::min(minimumX, vertex.x);
        maximumX = std::max(maximumX, vertex.x);
        minimumY = std::min(minimumY, vertex.y);
        maximumY = std::max(maximumY, vertex.y);
    }
    BAFX_CHECK_NEAR(minimumX, -1.0F, 1.0e-6F);
    BAFX_CHECK_NEAR(maximumX, 11.0F, 1.0e-6F);
    BAFX_CHECK_NEAR(minimumY, -1.0F, 1.0e-6F);
    BAFX_CHECK_NEAR(maximumY, 1.0F, 1.0e-6F);
}

BAFX_TEST(unity_trail_uses_four_inserted_corner_vertices)
{
    constexpr std::array points{
        UnityTrailPoint{0.0F, 0.0F},
        UnityTrailPoint{10.0F, 0.0F},
        UnityTrailPoint{10.0F, 10.0F}};
    const UnityTrailMesh mesh = makeUnityTrailMesh(points, 2.0F);

    // Two quads, a five-triangle corner fan, and two triangle caps.
    BAFX_CHECK(mesh.vertices.size() == 33U);
    for (std::size_t index = 0U; index < mesh.vertices.size(); index += 3U)
    {
        BAFX_CHECK(signedDoubleArea(
            mesh.vertices[index],
            mesh.vertices[index + 1U],
            mesh.vertices[index + 2U]) >= 0.0F);
    }
}

BAFX_TEST(unity_trail_progress_uses_accumulated_distance)
{
    constexpr std::array points{
        UnityTrailPoint{0.0F, 0.0F},
        UnityTrailPoint{3.0F, 0.0F},
        UnityTrailPoint{7.0F, 0.0F}};
    const UnityTrailMesh mesh = makeUnityTrailMesh(points, 2.0F);

    bool foundJoint = false;
    for (const UnityTrailVertex& vertex : mesh.vertices)
    {
        if (std::abs(vertex.x - 3.0F) <= 1.0e-6F)
        {
            BAFX_CHECK_NEAR(vertex.progress, 3.0F / 7.0F, 1.0e-6F);
            foundJoint = true;
        }
    }
    BAFX_CHECK(foundJoint);
}

BAFX_TEST(unity_trail_gradient_matches_reversed_serialized_keys)
{
    auto color = evaluateUnityTrailColor(0.0F);
    BAFX_CHECK_NEAR(color.r, 0.0F, 0.0F);
    BAFX_CHECK_NEAR(color.g, 0.0F, 0.0F);
    BAFX_CHECK_NEAR(color.b, 0.0F, 0.0F);

    color = evaluateUnityTrailColor(0.5794155795F);
    BAFX_CHECK_NEAR(color.g, 0.09486991F, 1.0e-7F);
    BAFX_CHECK_NEAR(color.b, 0.28235295F, 1.0e-7F);

    color = evaluateUnityTrailColor(0.9794155795F);
    BAFX_CHECK_NEAR(color.g, 0.39058137F, 1.0e-7F);
    BAFX_CHECK_NEAR(color.b, 1.0F, 1.0e-7F);
}

BAFX_TEST(unity_trail_rejects_empty_degenerate_and_nonfinite_widths)
{
    BAFX_CHECK(makeUnityTrailMesh({}, 2.0F).vertices.empty());
    constexpr std::array repeated{
        UnityTrailPoint{1.0F, 1.0F},
        UnityTrailPoint{1.0F, 1.0F}};
    BAFX_CHECK(makeUnityTrailMesh(repeated, 2.0F).vertices.empty());
    constexpr std::array line{
        UnityTrailPoint{0.0F, 0.0F},
        UnityTrailPoint{1.0F, 0.0F}};
    BAFX_CHECK(makeUnityTrailMesh(line, 0.0F).vertices.empty());
    BAFX_CHECK(makeUnityTrailMesh(
        line,
        std::numeric_limits<float>::infinity()).vertices.empty());
}
