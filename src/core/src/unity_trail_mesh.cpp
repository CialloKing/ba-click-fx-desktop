#include "bafx/core/unity_trail_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace bafx::core
{
namespace
{

constexpr float minimumSegmentLength = 1.0e-5F;
constexpr float maximumInnerMiterRatio = 8.0F;
constexpr float turnEpsilon = 1.0e-6F;
constexpr float darkKeyTime = 0.5794155795F;
constexpr float brightKeyTime = 0.9794155795F;
// Unity serializes Gradient keys in the active linear color space. Texture
// samples still use an sRGB SRV, but decoding these keys again skews the trail.
constexpr Float3 darkKey{0.0F, 0.09486991F, 0.28235295F};
constexpr Float3 brightKey{0.0F, 0.39058137F, 1.0F};

struct Vec2
{
    float x{0.0F};
    float y{0.0F};
};

struct Segment
{
    std::size_t pointIndex{0U};
    bool valid{false};
    Vec2 from{};
    Vec2 to{};
    Vec2 tangent{};
    Vec2 normal{};
    Vec2 fromLeft{};
    Vec2 fromRight{};
    Vec2 toLeft{};
    Vec2 toRight{};
};

struct Join
{
    std::size_t previousSegment{0U};
    Vec2 inner{};
    float innerTransverse{0.5F};
    float outerTransverse{0.5F};
    std::vector<Vec2> outerArc{};
};

[[nodiscard]] Vec2 add(const Vec2 a, const Vec2 b) noexcept
{
    return Vec2{a.x + b.x, a.y + b.y};
}

[[nodiscard]] Vec2 subtract(const Vec2 a, const Vec2 b) noexcept
{
    return Vec2{a.x - b.x, a.y - b.y};
}

[[nodiscard]] Vec2 multiply(const Vec2 value, const float scale) noexcept
{
    return Vec2{value.x * scale, value.y * scale};
}

[[nodiscard]] float cross(const Vec2 a, const Vec2 b) noexcept
{
    return a.x * b.y - a.y * b.x;
}

[[nodiscard]] float dot(const Vec2 a, const Vec2 b) noexcept
{
    return a.x * b.x + a.y * b.y;
}

[[nodiscard]] float length(const Vec2 value) noexcept
{
    return std::sqrt(dot(value, value));
}

[[nodiscard]] bool finite(const Vec2 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

[[nodiscard]] UnityTrailVertex makeVertex(
    const Vec2 position,
    const float progress,
    const float transverse) noexcept
{
    return UnityTrailVertex{position.x, position.y, progress, transverse};
}

void appendClockwiseTriangle(
    std::vector<UnityTrailVertex>& output,
    const UnityTrailVertex a,
    UnityTrailVertex b,
    UnityTrailVertex c)
{
    const Vec2 ab{b.x - a.x, b.y - a.y};
    const Vec2 ac{c.x - a.x, c.y - a.y};
    if (cross(ab, ac) < 0.0F)
    {
        std::swap(b, c);
    }
    output.push_back(a);
    output.push_back(b);
    output.push_back(c);
}

[[nodiscard]] Float3 lerp(
    const Float3 from,
    const Float3 to,
    const float amount) noexcept
{
    return Float3{
        from.r + (to.r - from.r) * amount,
        from.g + (to.g - from.g) * amount,
        from.b + (to.b - from.b) * amount};
}

}

UnityTrailMesh makeUnityTrailMesh(
    const std::span<const UnityTrailPoint> points,
    const float width) noexcept
{
    UnityTrailMesh mesh{};
    if (points.size() < 2U || !std::isfinite(width) || width <= 0.0F)
    {
        return mesh;
    }

    const float halfWidth = width * 0.5F;
    std::vector<float> distances(points.size(), 0.0F);
    std::vector<Segment> segments(points.size());
    for (std::size_t index = 1U; index < points.size(); ++index)
    {
        const Vec2 from{points[index - 1U].x, points[index - 1U].y};
        const Vec2 to{points[index].x, points[index].y};
        const Vec2 delta = subtract(to, from);
        const float segmentLength = finite(delta) ? length(delta) : 0.0F;
        distances[index] = distances[index - 1U] + segmentLength;
        if (segmentLength <= minimumSegmentLength)
        {
            continue;
        }

        const Vec2 tangent = multiply(delta, 1.0F / segmentLength);
        const Vec2 normal{-tangent.y, tangent.x};
        const Vec2 offset = multiply(normal, halfWidth);
        segments[index] = Segment{
            index,
            true,
            from,
            to,
            tangent,
            normal,
            add(from, offset),
            subtract(from, offset),
            add(to, offset),
            subtract(to, offset)};
    }

    const float totalLength = distances.back();
    if (!std::isfinite(totalLength) || totalLength <= minimumSegmentLength)
    {
        return mesh;
    }

    std::vector<std::optional<Join>> joins(points.size());
    for (std::size_t pointIndex = 1U;
         pointIndex + 1U < points.size();
         ++pointIndex)
    {
        Segment& previous = segments[pointIndex];
        Segment& next = segments[pointIndex + 1U];
        if (!previous.valid || !next.valid)
        {
            continue;
        }

        const float turn = cross(previous.tangent, next.tangent);
        if (std::abs(turn) <= turnEpsilon)
        {
            continue;
        }
        const float directionDot = dot(previous.tangent, next.tangent);
        const Vec2 point{points[pointIndex].x, points[pointIndex].y};
        const float innerSign = turn > 0.0F ? 1.0F : -1.0F;
        const float outerSign = -innerSign;
        const Vec2 previousInner = add(
            point,
            multiply(previous.normal, halfWidth * innerSign));
        const Vec2 nextInner = add(
            point,
            multiply(next.normal, halfWidth * innerSign));
        const float innerScale = cross(
            subtract(nextInner, previousInner),
            next.tangent) / turn;
        const Vec2 inner = add(
            previousInner,
            multiply(previous.tangent, innerScale));
        const float innerDistance = length(subtract(inner, point));
        const float previousProjection = dot(
            subtract(inner, point),
            previous.tangent);
        const float nextProjection = dot(
            subtract(inner, point),
            next.tangent);
        const float previousLength = length(subtract(previous.to, previous.from));
        const float nextLength = length(subtract(next.to, next.from));
        if (!finite(inner)
            || innerDistance > halfWidth * maximumInnerMiterRatio
            || previousProjection < -previousLength - turnEpsilon
            || previousProjection > turnEpsilon
            || nextProjection < -turnEpsilon
            || nextProjection > nextLength + turnEpsilon)
        {
            continue;
        }

        const float turnAngle = std::atan2(turn, directionDot);
        const float outerStartAngle = std::atan2(
            previous.normal.y * outerSign,
            previous.normal.x * outerSign);
        Join join{};
        join.previousSegment = pointIndex;
        join.inner = inner;
        join.innerTransverse = innerSign > 0.0F ? 1.0F : 0.0F;
        join.outerTransverse = innerSign > 0.0F ? 0.0F : 1.0F;
        join.outerArc.reserve(unityTrailCornerVertexCount + 2U);
        const std::size_t arcSteps = unityTrailCornerVertexCount + 1U;
        for (std::size_t step = 0U; step <= arcSteps; ++step)
        {
            const float angle = outerStartAngle
                + turnAngle * static_cast<float>(step)
                    / static_cast<float>(arcSteps);
            join.outerArc.push_back(Vec2{
                point.x + std::cos(angle) * halfWidth,
                point.y + std::sin(angle) * halfWidth});
        }

        if (innerSign > 0.0F)
        {
            previous.toLeft = inner;
            next.fromLeft = inner;
            previous.toRight = join.outerArc.front();
            next.fromRight = join.outerArc.back();
        }
        else
        {
            previous.toRight = inner;
            next.fromRight = inner;
            previous.toLeft = join.outerArc.front();
            next.fromLeft = join.outerArc.back();
        }
        joins[pointIndex] = std::move(join);
    }

    mesh.vertices.reserve(
        (points.size() - 1U) * 6U
        + points.size() * (unityTrailCornerVertexCount + 1U) * 3U
        + unityTrailCapVertexCount * 6U);
    for (std::size_t index = 1U; index < segments.size(); ++index)
    {
        const Segment& segment = segments[index];
        if (!segment.valid)
        {
            continue;
        }
        const float fromProgress = distances[index - 1U] / totalLength;
        const float toProgress = distances[index] / totalLength;
        appendClockwiseTriangle(
            mesh.vertices,
            makeVertex(segment.fromLeft, fromProgress, 1.0F),
            makeVertex(segment.toRight, toProgress, 0.0F),
            makeVertex(segment.toLeft, toProgress, 1.0F));
        appendClockwiseTriangle(
            mesh.vertices,
            makeVertex(segment.fromLeft, fromProgress, 1.0F),
            makeVertex(segment.fromRight, fromProgress, 0.0F),
            makeVertex(segment.toRight, toProgress, 0.0F));
    }

    for (std::size_t pointIndex = 1U;
         pointIndex + 1U < joins.size();
         ++pointIndex)
    {
        if (!joins[pointIndex].has_value())
        {
            continue;
        }
        const Join& join = *joins[pointIndex];
        const float progress = distances[pointIndex] / totalLength;
        for (std::size_t arc = 1U; arc < join.outerArc.size(); ++arc)
        {
            appendClockwiseTriangle(
                mesh.vertices,
                makeVertex(join.inner, progress, join.innerTransverse),
                makeVertex(
                    join.outerArc[arc - 1U],
                    progress,
                    join.outerTransverse),
                makeVertex(join.outerArc[arc], progress, join.outerTransverse));
        }
    }

    const auto first = std::find_if(
        segments.begin(),
        segments.end(),
        [](const Segment& segment)
        {
            return segment.valid;
        });
    const auto last = std::find_if(
        segments.rbegin(),
        segments.rend(),
        [](const Segment& segment)
        {
            return segment.valid;
        });
    if (first != segments.end() && last != segments.rend())
    {
        const Vec2 startTip = subtract(first->from, multiply(first->tangent, halfWidth));
        appendClockwiseTriangle(
            mesh.vertices,
            makeVertex(first->fromLeft, 0.0F, 1.0F),
            makeVertex(first->fromRight, 0.0F, 0.0F),
            makeVertex(startTip, 0.0F, 0.5F));

        const Vec2 endTip = add(last->to, multiply(last->tangent, halfWidth));
        appendClockwiseTriangle(
            mesh.vertices,
            makeVertex(last->toLeft, 1.0F, 1.0F),
            makeVertex(endTip, 1.0F, 0.5F),
            makeVertex(last->toRight, 1.0F, 0.0F));
    }
    return mesh;
}

Float3 evaluateUnityTrailColor(const float progress) noexcept
{
    const float value = std::clamp(progress, 0.0F, 1.0F);
    if (value <= darkKeyTime)
    {
        return lerp(Float3{}, darkKey, value / darkKeyTime);
    }
    if (value <= brightKeyTime)
    {
        return lerp(
            darkKey,
            brightKey,
            (value - darkKeyTime) / (brightKeyTime - darkKeyTime));
    }
    return brightKey;
}

}
