#include "test_support.hpp"

#include "bafx/windows/overlay_window.hpp"

#include <utility>
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

BAFX_TEST(distinct_pointer_moves_preserve_the_raw_input_path)
{
    std::vector<PointerEvent> events{
        event(PointerEventKind::Move, 10, 100),
        event(PointerEventKind::Move, 20, 200),
        event(PointerEventKind::Move, 30, 300)};

    events = coalescePointerMoves(std::move(events));
    BAFX_CHECK(events.size() == 3U);
    BAFX_CHECK(events[0].screenPosition.x == 10);
    BAFX_CHECK(events[1].screenPosition.x == 20);
    BAFX_CHECK(events[2].screenPosition.x == 30);
}

BAFX_TEST(exact_duplicate_pointer_moves_keep_the_latest_timestamp)
{
    std::vector<PointerEvent> events{
        event(PointerEventKind::Move, 10, 100),
        event(PointerEventKind::Move, 10, 200),
        event(PointerEventKind::Move, 20, 300)};

    events = coalescePointerMoves(std::move(events));
    BAFX_CHECK(events.size() == 2U);
    BAFX_CHECK(events[0].screenPosition.x == 10);
    BAFX_CHECK(events[0].qpcTimestamp == 200);
    BAFX_CHECK(events[1].screenPosition.x == 20);
}

BAFX_TEST(pointer_button_edges_preserve_ordered_move_samples)
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
    BAFX_CHECK(events.size() == 8U);
    BAFX_CHECK(events[0].kind == PointerEventKind::Move);
    BAFX_CHECK(events[1].kind == PointerEventKind::Move);
    BAFX_CHECK(events[2].kind == PointerEventKind::LeftButtonDown);
    BAFX_CHECK(events[3].kind == PointerEventKind::Move);
    BAFX_CHECK(events[4].kind == PointerEventKind::Move);
    BAFX_CHECK(events[5].kind == PointerEventKind::LeftButtonUp);
    BAFX_CHECK(events[6].kind == PointerEventKind::Move);
    BAFX_CHECK(events[7].kind == PointerEventKind::Move);
}

BAFX_TEST(pointer_coalescing_accepts_empty_input)
{
    const std::vector<PointerEvent> events = coalescePointerMoves({});
    BAFX_CHECK(events.empty());
}

BAFX_TEST(backlog_compaction_keeps_edges_and_latest_move_per_run)
{
    std::vector<PointerEvent> events{
        event(PointerEventKind::Move, 10, 100),
        event(PointerEventKind::Move, 20, 200),
        event(PointerEventKind::LeftButtonDown, 30, 300),
        event(PointerEventKind::Move, 40, 400),
        event(PointerEventKind::Move, 50, 500),
        event(PointerEventKind::LeftButtonUp, 60, 600)};

    events = compactPointerEventBacklog(std::move(events));
    BAFX_CHECK(events.size() == 4U);
    BAFX_CHECK(events[0].kind == PointerEventKind::Move);
    BAFX_CHECK(events[0].screenPosition.x == 20);
    BAFX_CHECK(events[1].kind == PointerEventKind::LeftButtonDown);
    BAFX_CHECK(events[2].kind == PointerEventKind::Move);
    BAFX_CHECK(events[2].screenPosition.x == 50);
    BAFX_CHECK(events[3].kind == PointerEventKind::LeftButtonUp);
}

BAFX_TEST(backlog_compaction_preserves_cancel_edges)
{
    std::vector<PointerEvent> events{
        event(PointerEventKind::Move, 10, 100),
        event(PointerEventKind::Move, 20, 200),
        event(PointerEventKind::Cancel, 30, 300),
        event(PointerEventKind::Move, 40, 400),
        event(PointerEventKind::Move, 50, 500)};

    events = compactPointerEventBacklog(std::move(events));
    BAFX_CHECK(events.size() == 3U);
    BAFX_CHECK(events[0].screenPosition.x == 20);
    BAFX_CHECK(events[1].kind == PointerEventKind::Cancel);
    BAFX_CHECK(events[2].screenPosition.x == 50);
}

BAFX_TEST(backlog_compaction_never_discards_button_edges)
{
    std::vector<PointerEvent> events;
    events.reserve(4096U);
    for (std::size_t index = 0U; index < 1024U; ++index)
    {
        events.push_back(event(PointerEventKind::LeftButtonDown, 1, index * 4U));
        events.push_back(event(PointerEventKind::LeftButtonUp, 2, index * 4U + 1U));
        events.push_back(event(PointerEventKind::Cancel, 3, index * 4U + 2U));
        events.push_back(event(PointerEventKind::LeftButtonDown, 4, index * 4U + 3U));
    }

    events = compactPointerEventBacklog(std::move(events));
    BAFX_CHECK(events.size() == 4096U);
    BAFX_CHECK(events.front().kind == PointerEventKind::LeftButtonDown);
    BAFX_CHECK(events[1].kind == PointerEventKind::LeftButtonUp);
    BAFX_CHECK(events[2].kind == PointerEventKind::Cancel);
    BAFX_CHECK(events.back().kind == PointerEventKind::LeftButtonDown);
}
