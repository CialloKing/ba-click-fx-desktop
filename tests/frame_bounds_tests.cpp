#include "test_support.hpp"

#include "bafx/fx/frame_bounds.hpp"

#include <cmath>
#include <optional>

namespace
{

using bafx::core::RectI;
using bafx::fx::FrameBoundsStatus;
using bafx::fx::FrameSnapshot;
using bafx::fx::PointF;
using bafx::fx::Sprite;
using bafx::fx::SpriteKind;
using bafx::fx::TrailPoint;
using bafx::fx::visualBounds;

void checkRect(const RectI actual, const RectI expected)
{
    BAFX_CHECK(actual.left == expected.left);
    BAFX_CHECK(actual.top == expected.top);
    BAFX_CHECK(actual.right == expected.right);
    BAFX_CHECK(actual.bottom == expected.bottom);
}

}

BAFX_TEST(frame_bounds_cover_rotated_sprites_conservatively)
{
    FrameSnapshot snapshot{};
    snapshot.sprites.push_back(Sprite{
        SpriteKind::Triangle,
        PointF{100.0F, 50.0F},
        20.0F,
        0.7853981634F,
        {},
        1.0F,
        0.0F,
        0U,
        4550,
        true});

    const auto result = visualBounds(snapshot);
    BAFX_CHECK(result.status == FrameBoundsStatus::Ok);
    checkRect(result.bounds, RectI{85, 35, 115, 65});
}

BAFX_TEST(frame_bounds_cover_trail_miter_and_prefer_strokes)
{
    FrameSnapshot snapshot{};
    snapshot.trail = {
        TrailPoint{PointF{1000.0F, 1000.0F}, 0.0F},
        TrailPoint{PointF{1100.0F, 1100.0F}, 0.0F}};
    snapshot.trailWidthPixels = 1000.0F;
    snapshot.trailStrokes.push_back({
        {TrailPoint{PointF{10.0F, 20.0F}, 0.0F},
         TrailPoint{PointF{30.0F, 40.0F}, 0.0F}},
        10.0F});

    const auto result = visualBounds(snapshot);
    BAFX_CHECK(result.status == FrameBoundsStatus::Ok);
    checkRect(result.bounds, RectI{-30, -20, 70, 80});
}

BAFX_TEST(frame_bounds_distinguish_empty_invalid_and_overflow)
{
    BAFX_CHECK(visualBounds(FrameSnapshot{}).status == FrameBoundsStatus::Empty);

    FrameSnapshot invalid{};
    invalid.sprites.push_back(Sprite{
        SpriteKind::CenterDisk,
        PointF{NAN, 0.0F},
        10.0F});
    BAFX_CHECK(visualBounds(invalid).status == FrameBoundsStatus::Invalid);

    FrameSnapshot overflow{};
    overflow.sprites.push_back(Sprite{
        SpriteKind::CenterDisk,
        PointF{1.0e20F, 0.0F},
        10.0F});
    BAFX_CHECK(
        visualBounds(overflow).status == FrameBoundsStatus::IntegerOverflow);
}

BAFX_TEST(frame_bounds_union_preserves_missing_frames)
{
    const std::optional<RectI> previous = RectI{10, 20, 30, 40};
    const std::optional<RectI> current = RectI{-5, 25, 35, 45};
    const auto combined = bafx::fx::uniteVisualBounds(previous, current);
    BAFX_CHECK(combined.has_value());
    checkRect(*combined, RectI{-5, 20, 35, 45});
    BAFX_CHECK(
        bafx::fx::uniteVisualBounds(std::nullopt, current).has_value());
    BAFX_CHECK(
        bafx::fx::uniteVisualBounds(previous, std::nullopt).has_value());
}
