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
    const std::int64_t timestamp,
    const std::uint32_t messageTimeMilliseconds = 0U) noexcept
{
    return PointerEvent{
        kind,
        POINT{x, 0},
        timestamp,
        messageTimeMilliseconds};
}

}

BAFX_TEST(pointer_moves_keep_the_latest_sample_per_contiguous_run)
{
    std::vector<PointerEvent> events{
        event(PointerEventKind::Move, 10, 100, 1U),
        event(PointerEventKind::Move, 20, 200, 2U),
        event(PointerEventKind::Move, 30, 300, 3U)};

    events = coalescePointerMoves(std::move(events));
    BAFX_CHECK(events.size() == 1U);
    BAFX_CHECK(events[0].screenPosition.x == 30);
    BAFX_CHECK(events[0].qpcTimestamp == 300);
    BAFX_CHECK(events[0].messageTimeMilliseconds == 3U);
}

BAFX_TEST(pointer_move_run_keeps_the_latest_timestamp)
{
    std::vector<PointerEvent> events{
        event(PointerEventKind::Move, 10, 100),
        event(PointerEventKind::Move, 10, 200),
        event(PointerEventKind::Move, 20, 300)};

    events = coalescePointerMoves(std::move(events));
    BAFX_CHECK(events.size() == 1U);
    BAFX_CHECK(events[0].screenPosition.x == 20);
    BAFX_CHECK(events[0].qpcTimestamp == 300);
}

BAFX_TEST(pointer_button_edges_partition_move_runs)
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

BAFX_TEST(pointer_move_runs_preserve_all_state_edges_and_latest_metadata)
{
    std::vector<PointerEvent> events{
        event(PointerEventKind::Move, 10, 100, 1U),
        event(PointerEventKind::Move, 20, 200, 2U),
        event(PointerEventKind::Cancel, 30, 300, 3U),
        event(PointerEventKind::Move, 40, 400, 4U),
        event(PointerEventKind::Move, 50, 500, 5U),
        event(PointerEventKind::LeftButtonDown, 60, 600, 6U),
        event(PointerEventKind::Move, 70, 700, 7U),
        event(PointerEventKind::Move, 80, 800, 8U),
        event(PointerEventKind::LeftButtonUp, 90, 900, 9U),
        event(PointerEventKind::Move, 100, 1000, 10U),
        event(PointerEventKind::Move, 110, 1100, 11U)};

    events = coalescePointerMoves(std::move(events));
    BAFX_CHECK(events.size() == 7U);
    BAFX_CHECK(events[0].kind == PointerEventKind::Move);
    BAFX_CHECK(events[0].screenPosition.x == 20);
    BAFX_CHECK(events[0].qpcTimestamp == 200);
    BAFX_CHECK(events[0].messageTimeMilliseconds == 2U);
    BAFX_CHECK(events[1].kind == PointerEventKind::Cancel);
    BAFX_CHECK(events[2].kind == PointerEventKind::Move);
    BAFX_CHECK(events[2].screenPosition.x == 50);
    BAFX_CHECK(events[3].kind == PointerEventKind::LeftButtonDown);
    BAFX_CHECK(events[4].kind == PointerEventKind::Move);
    BAFX_CHECK(events[4].screenPosition.x == 80);
    BAFX_CHECK(events[5].kind == PointerEventKind::LeftButtonUp);
    BAFX_CHECK(events[6].kind == PointerEventKind::Move);
    BAFX_CHECK(events[6].screenPosition.x == 110);
    BAFX_CHECK(events[6].qpcTimestamp == 1100);
    BAFX_CHECK(events[6].messageTimeMilliseconds == 11U);
}

BAFX_TEST(backlog_and_normal_move_compaction_share_one_contract)
{
    const std::vector<PointerEvent> source{
        event(PointerEventKind::Move, 10, 100, 1U),
        event(PointerEventKind::Move, 20, 200, 2U),
        event(PointerEventKind::Cancel, 30, 300, 3U),
        event(PointerEventKind::Move, 40, 400, 4U),
        event(PointerEventKind::LeftButtonDown, 50, 500, 5U),
        event(PointerEventKind::Move, 60, 600, 6U),
        event(PointerEventKind::LeftButtonUp, 70, 700, 7U)};

    const std::vector<PointerEvent> normal = coalescePointerMoves(source);
    const std::vector<PointerEvent> backlog = compactPointerEventBacklog(source);
    BAFX_CHECK(normal.size() == backlog.size());
    for (std::size_t index = 0U; index < normal.size(); ++index)
    {
        BAFX_CHECK(normal[index].kind == backlog[index].kind);
        BAFX_CHECK(normal[index].screenPosition.x == backlog[index].screenPosition.x);
        BAFX_CHECK(normal[index].screenPosition.y == backlog[index].screenPosition.y);
        BAFX_CHECK(normal[index].qpcTimestamp == backlog[index].qpcTimestamp);
        BAFX_CHECK(
            normal[index].messageTimeMilliseconds
            == backlog[index].messageTimeMilliseconds);
    }
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
