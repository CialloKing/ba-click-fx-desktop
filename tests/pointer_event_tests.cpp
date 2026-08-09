#include "test_support.hpp"

#include "bafx/windows/overlay_window.hpp"

#include <vector>

using namespace bafx::windows;

namespace
{

[[nodiscard]] PointerEvent event(
    const PointerEventKind kind,
    const LONG x,
    const std::int64_t timestamp) noexcept
{
    return PointerEvent{kind, POINT{x, 0}, timestamp};
}

}

BAFX_TEST(pointer_moves_are_coalesced_to_the_latest_sample_per_tick)
{
    std::vector<PointerEvent> events{
        event(PointerEventKind::Move, 10, 100),
        event(PointerEventKind::Move, 20, 200),
        event(PointerEventKind::Move, 30, 300)};

    events = coalescePointerMoves(std::move(events));
    BAFX_CHECK(events.size() == 1U);
    BAFX_CHECK(events[0].screenPosition.x == 30);
    BAFX_CHECK(events[0].qpcTimestamp == 300);
}

BAFX_TEST(pointer_button_edges_partition_move_coalescing)
{
    std::vector<PointerEvent> events{
        event(PointerEventKind::Move, 1, 10),
        event(PointerEventKind::Move, 2, 20),
        event(PointerEventKind::LeftButtonDown, 3, 30),
        event(PointerEventKind::Move, 4, 40),
        event(PointerEventKind::Move, 5, 50),
        event(PointerEventKind::LeftButtonUp, 6, 60),
        event(PointerEventKind::Move, 7, 70),
        event(PointerEventKind::Move, 8, 80)};

    events = coalescePointerMoves(std::move(events));
    BAFX_CHECK(events.size() == 5U);
    BAFX_CHECK(events[0].kind == PointerEventKind::Move);
    BAFX_CHECK(events[0].screenPosition.x == 2);
    BAFX_CHECK(events[1].kind == PointerEventKind::LeftButtonDown);
    BAFX_CHECK(events[2].kind == PointerEventKind::Move);
    BAFX_CHECK(events[2].screenPosition.x == 5);
    BAFX_CHECK(events[3].kind == PointerEventKind::LeftButtonUp);
    BAFX_CHECK(events[4].kind == PointerEventKind::Move);
    BAFX_CHECK(events[4].screenPosition.x == 8);
}

BAFX_TEST(pointer_coalescing_accepts_empty_input)
{
    const std::vector<PointerEvent> events = coalescePointerMoves({});
    BAFX_CHECK(events.empty());
}
