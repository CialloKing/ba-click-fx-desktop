#include "bafx/fx/frame_bounds.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

namespace bafx::fx
{
namespace
{

constexpr float trailMaximumInnerMiterRatio = 8.0F;

[[nodiscard]] bool finite(const PointF point) noexcept
{
    return std::isfinite(point.x) && std::isfinite(point.y);
}

[[nodiscard]] bool fitsInt32(const double value) noexcept
{
    return value >= static_cast<double>(std::numeric_limits<std::int32_t>::min())
        && value <= static_cast<double>(std::numeric_limits<std::int32_t>::max());
}

}

FrameVisualBoundsResult visualBounds(
    const FrameSnapshot& snapshot) noexcept
{
    FrameVisualBoundsResult result{};
    double minimumX = std::numeric_limits<double>::infinity();
    double minimumY = std::numeric_limits<double>::infinity();
    double maximumX = -std::numeric_limits<double>::infinity();
    double maximumY = -std::numeric_limits<double>::infinity();
    bool found = false;
    bool invalid = false;

    const auto includeBounds = [&](
                                   const double left,
                                   const double top,
                                   const double right,
                                   const double bottom)
    {
        if (!std::isfinite(left)
            || !std::isfinite(top)
            || !std::isfinite(right)
            || !std::isfinite(bottom))
        {
            invalid = true;
            return;
        }
        minimumX = std::min(minimumX, left);
        minimumY = std::min(minimumY, top);
        maximumX = std::max(maximumX, right);
        maximumY = std::max(maximumY, bottom);
        found = true;
    };

    for (const Sprite& sprite : snapshot.sprites)
    {
        if (!finite(sprite.centerPixels)
            || !std::isfinite(sprite.sizePixels)
            || !std::isfinite(sprite.rotationRadians)
            || sprite.sizePixels < 0.0F)
        {
            invalid = true;
            continue;
        }
        if (sprite.sizePixels == 0.0F)
        {
            continue;
        }

        const double halfSize = static_cast<double>(sprite.sizePixels) * 0.5;
        const double radius = halfSize * (
            std::abs(std::cos(static_cast<double>(sprite.rotationRadians)))
            + std::abs(std::sin(static_cast<double>(sprite.rotationRadians))));
        includeBounds(
            static_cast<double>(sprite.centerPixels.x) - radius,
            static_cast<double>(sprite.centerPixels.y) - radius,
            static_cast<double>(sprite.centerPixels.x) + radius,
            static_cast<double>(sprite.centerPixels.y) + radius);
    }

    const auto includeTrail = [&includeBounds, &invalid] (
                                  const std::span<const TrailPoint> points,
                                  const float width)
    {
        if (points.size() < 2U)
        {
            return;
        }
        if (!std::isfinite(width) || width <= 0.0F)
        {
            invalid = true;
            return;
        }

        // makeUnityTrailMesh limits an inner miter to 8 * halfWidth. A
        // point-wise radius of 4 * width also covers the endpoint caps and
        // every segment between two finite points without allocating a mesh.
        const double radius = static_cast<double>(width)
            * static_cast<double>(trailMaximumInnerMiterRatio) * 0.5;
        for (const TrailPoint& point : points)
        {
            if (!finite(point.positionPixels))
            {
                invalid = true;
                continue;
            }
            includeBounds(
                static_cast<double>(point.positionPixels.x) - radius,
                static_cast<double>(point.positionPixels.y) - radius,
                static_cast<double>(point.positionPixels.x) + radius,
                static_cast<double>(point.positionPixels.y) + radius);
        }
    };

    if (snapshot.trailStrokes.empty())
    {
        includeTrail(snapshot.trail, snapshot.trailWidthPixels);
    }
    else
    {
        for (const TrailStroke& stroke : snapshot.trailStrokes)
        {
            includeTrail(stroke.points, stroke.widthPixels);
        }
    }

    if (invalid)
    {
        result.status = FrameBoundsStatus::Invalid;
        return result;
    }
    if (!found)
    {
        result.status = FrameBoundsStatus::Empty;
        return result;
    }

    const double left = std::floor(minimumX);
    const double top = std::floor(minimumY);
    const double right = std::ceil(maximumX);
    const double bottom = std::ceil(maximumY);
    if (!fitsInt32(left)
        || !fitsInt32(top)
        || !fitsInt32(right)
        || !fitsInt32(bottom))
    {
        result.status = FrameBoundsStatus::IntegerOverflow;
        return result;
    }

    result.bounds = bafx::core::RectI{
        static_cast<std::int32_t>(left),
        static_cast<std::int32_t>(top),
        static_cast<std::int32_t>(right),
        static_cast<std::int32_t>(bottom)};
    result.status = FrameBoundsStatus::Ok;
    return result;
}

std::optional<bafx::core::RectI> uniteVisualBounds(
    const std::optional<bafx::core::RectI> previous,
    const std::optional<bafx::core::RectI> current) noexcept
{
    if (!previous.has_value())
    {
        return current;
    }
    if (!current.has_value())
    {
        return previous;
    }
    return bafx::core::RectI{
        std::min(previous->left, current->left),
        std::min(previous->top, current->top),
        std::max(previous->right, current->right),
        std::max(previous->bottom, current->bottom)};
}

}
