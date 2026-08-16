#include "test_support.hpp"

#include "bafx/windows/overlay_window.hpp"

#include <initializer_list>
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
        messageTimeMilliseconds,
        messageTimeMilliseconds != 0U};
}

[[nodiscard]] PointerFrameSnapshot consume(
    PointerFrameAdapter& adapter,
    const std::initializer_list<PointerEvent> events)
{
    return adapter.consume(std::span<const PointerEvent>(
        events.begin(),
        events.size()));
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
    BAFX_CHECK(events[0].messageTimeValid);
}

BAFX_TEST(win32_message_queue_age_handles_tick_count_wrap)
{
    BAFX_CHECK(win32MessageQueueAgeMilliseconds(150U, 100U) == 50U);
    BAFX_CHECK(
        win32MessageQueueAgeMilliseconds(25U, 0xFFFFFFF0U)
        == 41U);
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

BAFX_TEST(pointer_frame_adapter_keeps_held_state_across_empty_frames)
{
    PointerFrameAdapter adapter;

    const PointerFrameSnapshot down = consume(adapter, {
        event(PointerEventKind::LeftButtonDown, 10, 100, 1U)});
    BAFX_CHECK(!down.heldBefore);
    BAFX_CHECK(down.heldAfter);
    BAFX_CHECK(down.edges.size() == 1U);
    BAFX_CHECK(down.edges[0].kind == PointerEventKind::LeftButtonDown);
    BAFX_CHECK(adapter.held());

    const PointerFrameSnapshot empty = consume(adapter, {});
    BAFX_CHECK(empty.heldBefore);
    BAFX_CHECK(empty.heldAfter);
    BAFX_CHECK(empty.edges.empty());
    BAFX_CHECK(!empty.latestNonCancelSample.has_value());
    BAFX_CHECK(!empty.latestMoveSample.has_value());
    BAFX_CHECK(adapter.held());
}

BAFX_TEST(pointer_frame_adapter_latches_free_down_move_at_the_final_sample)
{
    PointerFrameAdapter adapter;
    const PointerFrameSnapshot frame = consume(adapter, {
        event(PointerEventKind::LeftButtonDown, 10, 100, 1U),
        event(PointerEventKind::Move, 20, 200, 2U)});

    BAFX_CHECK(!frame.heldBefore);
    BAFX_CHECK(frame.heldAfter);
    BAFX_CHECK(frame.edges.size() == 1U);
    BAFX_CHECK(frame.edges[0].kind == PointerEventKind::LeftButtonDown);
    BAFX_CHECK(!frame.hasFinalFreeMove);
    BAFX_CHECK(frame.hasFinalHeldMove);
    BAFX_CHECK(frame.latestNonCancelSample.has_value());
    BAFX_CHECK(frame.latestNonCancelSample->screenPosition.x == 20);
    BAFX_CHECK(frame.latestNonCancelSample->qpcTimestamp == 200);
    BAFX_CHECK(frame.latestNonCancelSample->messageTimeMilliseconds == 2U);
}

BAFX_TEST(pointer_frame_adapter_release_frame_reports_move_without_remaining_held)
{
    PointerFrameAdapter adapter;
    static_cast<void>(consume(adapter, {
        event(PointerEventKind::LeftButtonDown, 10, 100)}));

    const PointerFrameSnapshot frame = consume(adapter, {
        event(PointerEventKind::Move, 20, 200),
        event(PointerEventKind::LeftButtonUp, 30, 300)});

    BAFX_CHECK(frame.heldBefore);
    BAFX_CHECK(!frame.heldAfter);
    BAFX_CHECK(!frame.hasFinalHeldMove);
    BAFX_CHECK(frame.edges.size() == 1U);
    BAFX_CHECK(frame.edges[0].kind == PointerEventKind::LeftButtonUp);
    BAFX_CHECK(!frame.hasFinalFreeMove);
    BAFX_CHECK(frame.latestNonCancelSample.has_value());
    BAFX_CHECK(frame.latestNonCancelSample->screenPosition.x == 30);
    BAFX_CHECK(!adapter.held());
}

BAFX_TEST(pointer_frame_adapter_down_move_up_is_one_released_frame_state)
{
    PointerFrameAdapter adapter;
    const PointerFrameSnapshot frame = consume(adapter, {
        event(PointerEventKind::LeftButtonDown, 10, 100),
        event(PointerEventKind::Move, 20, 200),
        event(PointerEventKind::LeftButtonUp, 30, 300)});

    BAFX_CHECK(!frame.heldBefore);
    BAFX_CHECK(!frame.heldAfter);
    BAFX_CHECK(frame.edges.size() == 2U);
    BAFX_CHECK(frame.edges[0].kind == PointerEventKind::LeftButtonDown);
    BAFX_CHECK(frame.edges[1].kind == PointerEventKind::LeftButtonUp);
    BAFX_CHECK(!frame.hasFinalHeldMove);
    BAFX_CHECK(!frame.hasFinalFreeMove);
    BAFX_CHECK(frame.latestNonCancelSample.has_value());
    BAFX_CHECK(frame.latestNonCancelSample->screenPosition.x == 30);
}

BAFX_TEST(pointer_frame_adapter_ignores_duplicate_state_edges)
{
    PointerFrameAdapter adapter;
    const PointerFrameSnapshot down = consume(adapter, {
        event(PointerEventKind::LeftButtonDown, 10, 100),
        event(PointerEventKind::LeftButtonDown, 20, 200)});
    BAFX_CHECK(down.edges.size() == 1U);
    BAFX_CHECK(down.edges[0].kind == PointerEventKind::LeftButtonDown);
    BAFX_CHECK(down.heldAfter);
    BAFX_CHECK(down.latestNonCancelSample.has_value());
    BAFX_CHECK(down.latestNonCancelSample->screenPosition.x == 20);

    const PointerFrameSnapshot up = consume(adapter, {
        event(PointerEventKind::LeftButtonUp, 30, 300),
        event(PointerEventKind::LeftButtonUp, 40, 400)});
    BAFX_CHECK(up.edges.size() == 1U);
    BAFX_CHECK(up.edges[0].kind == PointerEventKind::LeftButtonUp);
    BAFX_CHECK(!up.heldAfter);
    BAFX_CHECK(up.latestNonCancelSample.has_value());
    BAFX_CHECK(up.latestNonCancelSample->screenPosition.x == 40);

    const PointerFrameSnapshot strayUp = consume(adapter, {
        event(PointerEventKind::LeftButtonUp, 50, 500)});
    BAFX_CHECK(strayUp.edges.empty());
    BAFX_CHECK(!strayUp.heldAfter);
}

BAFX_TEST(pointer_frame_adapter_cancel_hard_releases_without_replacing_position)
{
    PointerFrameAdapter adapter;
    static_cast<void>(consume(adapter, {
        event(PointerEventKind::LeftButtonDown, 10, 100)}));

    const PointerFrameSnapshot frame = consume(adapter, {
        event(PointerEventKind::Move, 20, 200, 2U),
        event(PointerEventKind::Cancel, 999, 300, 3U)});

    BAFX_CHECK(frame.heldBefore);
    BAFX_CHECK(!frame.heldAfter);
    BAFX_CHECK(frame.edges.size() == 1U);
    BAFX_CHECK(frame.edges[0].kind == PointerEventKind::Cancel);
    BAFX_CHECK(frame.latestNonCancelSample.has_value());
    BAFX_CHECK(frame.latestNonCancelSample->screenPosition.x == 20);
    BAFX_CHECK(frame.latestNonCancelSample->qpcTimestamp == 200);
    BAFX_CHECK(!adapter.held());

    const PointerFrameSnapshot duplicateCancel = consume(adapter, {
        event(PointerEventKind::Cancel, 1000, 400)});
    BAFX_CHECK(duplicateCancel.edges.size() == 1U);
    BAFX_CHECK(duplicateCancel.edges[0].kind == PointerEventKind::Cancel);
    BAFX_CHECK(!duplicateCancel.heldAfter);
    BAFX_CHECK(!duplicateCancel.latestNonCancelSample.has_value());
}

BAFX_TEST(pointer_frame_adapter_classifies_moves_by_the_order_of_edges)
{
    PointerFrameAdapter adapter;
    const PointerFrameSnapshot frame = consume(adapter, {
        event(PointerEventKind::Move, 10, 100),
        event(PointerEventKind::LeftButtonDown, 20, 200),
        event(PointerEventKind::Move, 30, 300),
        event(PointerEventKind::LeftButtonUp, 40, 400),
        event(PointerEventKind::Move, 50, 500)});

    BAFX_CHECK(frame.hasFinalFreeMove);
    BAFX_CHECK(!frame.hasFinalHeldMove);
    BAFX_CHECK(frame.edges.size() == 2U);
    BAFX_CHECK(frame.edges[0].kind == PointerEventKind::LeftButtonDown);
    BAFX_CHECK(frame.edges[1].kind == PointerEventKind::LeftButtonUp);
    BAFX_CHECK(!frame.heldAfter);
    BAFX_CHECK(frame.latestNonCancelSample.has_value());
    BAFX_CHECK(frame.latestNonCancelSample->screenPosition.x == 50);
}

BAFX_TEST(pointer_frame_adapter_preserves_down_up_down_order)
{
    PointerFrameAdapter adapter;
    const PointerFrameSnapshot frame = consume(adapter, {
        event(PointerEventKind::LeftButtonDown, 10, 100),
        event(PointerEventKind::LeftButtonUp, 20, 200),
        event(PointerEventKind::LeftButtonDown, 30, 300)});

    BAFX_CHECK(frame.edges.size() == 3U);
    BAFX_CHECK(frame.edges[0].kind == PointerEventKind::LeftButtonDown);
    BAFX_CHECK(frame.edges[0].trigger.screenPosition.x == 10);
    BAFX_CHECK(frame.edges[1].kind == PointerEventKind::LeftButtonUp);
    BAFX_CHECK(frame.edges[1].trigger.screenPosition.x == 20);
    BAFX_CHECK(frame.edges[2].kind == PointerEventKind::LeftButtonDown);
    BAFX_CHECK(frame.edges[2].trigger.screenPosition.x == 30);
    BAFX_CHECK(frame.heldAfter);
    BAFX_CHECK(!frame.hasFinalFreeMove);
    BAFX_CHECK(!frame.hasFinalHeldMove);
}

BAFX_TEST(pointer_frame_adapter_does_not_promote_a_prior_held_epoch_move)
{
    PointerFrameAdapter adapter;
    const PointerFrameSnapshot frame = consume(adapter, {
        event(PointerEventKind::LeftButtonDown, 10, 100),
        event(PointerEventKind::Move, 20, 200),
        event(PointerEventKind::LeftButtonUp, 30, 300),
        event(PointerEventKind::LeftButtonDown, 40, 400)});

    BAFX_CHECK(frame.edges.size() == 3U);
    BAFX_CHECK(frame.heldAfter);
    BAFX_CHECK(!frame.hasFinalHeldMove);
    BAFX_CHECK(frame.latestMoveSample.has_value());
    BAFX_CHECK(frame.latestMoveSample->screenPosition.x == 20);
}

BAFX_TEST(pointer_frame_adapter_does_not_promote_a_pre_edge_free_move)
{
    PointerFrameAdapter adapter;
    const PointerFrameSnapshot frame = consume(adapter, {
        event(PointerEventKind::Move, 10, 100),
        event(PointerEventKind::LeftButtonDown, 20, 200),
        event(PointerEventKind::LeftButtonUp, 30, 300)});

    BAFX_CHECK(frame.edges.size() == 2U);
    BAFX_CHECK(!frame.heldAfter);
    BAFX_CHECK(!frame.hasFinalFreeMove);
    BAFX_CHECK(frame.latestMoveSample.has_value());
    BAFX_CHECK(frame.latestMoveSample->screenPosition.x == 10);
    BAFX_CHECK(frame.latestNonCancelSample.has_value());
    BAFX_CHECK(frame.latestNonCancelSample->screenPosition.x == 30);
}

BAFX_TEST(pointer_frame_adapter_promotes_a_move_after_up_in_the_final_free_epoch)
{
    PointerFrameAdapter adapter;
    static_cast<void>(consume(adapter, {
        event(PointerEventKind::LeftButtonDown, 10, 100)}));

    const PointerFrameSnapshot frame = consume(adapter, {
        event(PointerEventKind::LeftButtonUp, 20, 200),
        event(PointerEventKind::Move, 30, 300)});

    BAFX_CHECK(frame.edges.size() == 1U);
    BAFX_CHECK(frame.edges[0].kind == PointerEventKind::LeftButtonUp);
    BAFX_CHECK(!frame.heldAfter);
    BAFX_CHECK(frame.hasFinalFreeMove);
    BAFX_CHECK(frame.latestMoveSample.has_value());
    BAFX_CHECK(frame.latestMoveSample->screenPosition.x == 30);
}

BAFX_TEST(pointer_frame_adapter_records_cancel_while_already_free)
{
    PointerFrameAdapter adapter;
    const PointerFrameSnapshot frame = consume(adapter, {
        event(PointerEventKind::Move, 10, 100),
        event(PointerEventKind::Cancel, 999, 200)});

    BAFX_CHECK(frame.edges.size() == 1U);
    BAFX_CHECK(frame.edges[0].kind == PointerEventKind::Cancel);
    BAFX_CHECK(frame.edges[0].trigger.qpcTimestamp == 200);
    BAFX_CHECK(!frame.heldAfter);
    BAFX_CHECK(!frame.hasFinalFreeMove);
    BAFX_CHECK(frame.latestNonCancelSample.has_value());
    BAFX_CHECK(frame.latestNonCancelSample->screenPosition.x == 10);
}

BAFX_TEST(pointer_frame_adapter_matches_raw_and_coalesced_move_runs)
{
    const std::vector<PointerEvent> raw{
        event(PointerEventKind::Move, 10, 100),
        event(PointerEventKind::Move, 20, 200),
        event(PointerEventKind::LeftButtonDown, 30, 300),
        event(PointerEventKind::Move, 40, 400),
        event(PointerEventKind::Move, 50, 500)};
    const std::vector<PointerEvent> coalesced = coalescePointerMoves(raw);
    PointerFrameAdapter rawAdapter;
    PointerFrameAdapter coalescedAdapter;

    const PointerFrameSnapshot rawFrame = rawAdapter.consume(raw);
    const PointerFrameSnapshot coalescedFrame = coalescedAdapter.consume(coalesced);
    BAFX_CHECK(rawFrame.heldBefore == coalescedFrame.heldBefore);
    BAFX_CHECK(rawFrame.heldAfter == coalescedFrame.heldAfter);
    BAFX_CHECK(rawFrame.edges.size() == coalescedFrame.edges.size());
    for (std::size_t index = 0U; index < rawFrame.edges.size(); ++index)
    {
        BAFX_CHECK(rawFrame.edges[index].kind == coalescedFrame.edges[index].kind);
        BAFX_CHECK(
            rawFrame.edges[index].trigger.qpcTimestamp
            == coalescedFrame.edges[index].trigger.qpcTimestamp);
    }
    BAFX_CHECK(rawFrame.hasFinalFreeMove == coalescedFrame.hasFinalFreeMove);
    BAFX_CHECK(rawFrame.hasFinalHeldMove == coalescedFrame.hasFinalHeldMove);
    BAFX_CHECK(rawFrame.latestNonCancelSample.has_value());
    BAFX_CHECK(coalescedFrame.latestNonCancelSample.has_value());
    BAFX_CHECK(
        rawFrame.latestNonCancelSample->screenPosition.x
        == coalescedFrame.latestNonCancelSample->screenPosition.x);
    BAFX_CHECK(
        rawFrame.latestNonCancelSample->qpcTimestamp
        == coalescedFrame.latestNonCancelSample->qpcTimestamp);
    BAFX_CHECK(rawFrame.latestMoveSample.has_value());
    BAFX_CHECK(coalescedFrame.latestMoveSample.has_value());
    BAFX_CHECK(
        rawFrame.latestMoveSample->qpcTimestamp
        == coalescedFrame.latestMoveSample->qpcTimestamp);
}
