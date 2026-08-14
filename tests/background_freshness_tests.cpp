#include "test_support.hpp"

#include "bafx/core/background_freshness.hpp"

#include <chrono>
#include <optional>

using namespace bafx::core;
using namespace std::chrono_literals;

namespace
{

BackgroundFrameStamp frameAt(const MonotonicTime time)
{
    return BackgroundFrameStamp{time, 7, true, true};
}

BackgroundUsagePolicy usagePolicy()
{
    return BackgroundUsagePolicy{100ms, 48ms, 7};
}

}

BAFX_TEST(background_usage_accepts_the_entire_bounded_window)
{
    auto decision = evaluateBackgroundUsage(frameAt(0ms), 0ms, usagePolicy());
    BAFX_CHECK(decision.status == BackgroundUsageStatus::Usable);
    BAFX_CHECK(decision.age == 0ms);
    BAFX_CHECK(decision.enabled);

    decision = evaluateBackgroundUsage(frameAt(-100ms + 1ns), 0ms, usagePolicy());
    BAFX_CHECK(decision.status == BackgroundUsageStatus::Usable);
    BAFX_CHECK(decision.age == 100ms - 1ns);
    BAFX_CHECK(decision.enabled);

    decision = evaluateBackgroundUsage(frameAt(48ms), 0ms, usagePolicy());
    BAFX_CHECK(decision.status == BackgroundUsageStatus::Usable);
    BAFX_CHECK(decision.age == -48ms);
    BAFX_CHECK(decision.enabled);
}

BAFX_TEST(background_usage_window_has_explicit_closed_and_open_boundaries)
{
    auto decision = evaluateBackgroundUsage(frameAt(-100ms), 0ms, usagePolicy());
    BAFX_CHECK(decision.status == BackgroundUsageStatus::Stale);
    BAFX_CHECK(decision.age == 100ms);
    BAFX_CHECK(!decision.enabled);

    decision = evaluateBackgroundUsage(frameAt(48ms + 1ns), 0ms, usagePolicy());
    BAFX_CHECK(decision.status == BackgroundUsageStatus::FutureTimestamp);
    BAFX_CHECK(decision.age == -48ms - 1ns);
    BAFX_CHECK(!decision.enabled);
}

BAFX_TEST(background_usage_rejects_epoch_and_contract_errors_immediately)
{
    auto wrongEpoch = frameAt(100ms);
    wrongEpoch.epoch = 8;
    auto decision = evaluateBackgroundUsage(wrongEpoch, 100ms, usagePolicy());
    BAFX_CHECK(decision.status == BackgroundUsageStatus::WrongEpoch);
    BAFX_CHECK(!decision.enabled);

    auto badContract = frameAt(100ms);
    badContract.excludesOwnOverlay = false;
    decision = evaluateBackgroundUsage(badContract, 100ms, usagePolicy());
    BAFX_CHECK(decision.status == BackgroundUsageStatus::InvalidContract);
    BAFX_CHECK(!decision.enabled);
}

BAFX_TEST(background_usage_validates_missing_samples_and_policy)
{
    auto decision = evaluateBackgroundUsage(std::nullopt, 100ms, usagePolicy());
    BAFX_CHECK(decision.status == BackgroundUsageStatus::Missing);
    BAFX_CHECK(!decision.enabled);

    auto invalidPolicy = usagePolicy();
    invalidPolicy.maxAge = MonotonicTime::zero();
    decision = evaluateBackgroundUsage(frameAt(100ms), 100ms, invalidPolicy);
    BAFX_CHECK(decision.status == BackgroundUsageStatus::InvalidPolicy);
    BAFX_CHECK(!decision.enabled);

    invalidPolicy = usagePolicy();
    invalidPolicy.maxFutureSkew = -1ns;
    decision = evaluateBackgroundUsage(frameAt(100ms), 100ms, invalidPolicy);
    BAFX_CHECK(decision.status == BackgroundUsageStatus::InvalidPolicy);
    BAFX_CHECK(!decision.enabled);
}

BAFX_TEST(background_usage_saturates_extreme_timestamp_differences)
{
    auto decision = evaluateBackgroundUsage(
        frameAt(MonotonicTime::min()),
        MonotonicTime::max(),
        usagePolicy());
    BAFX_CHECK(decision.status == BackgroundUsageStatus::Stale);
    BAFX_CHECK(decision.age == MonotonicTime::max());
    BAFX_CHECK(!decision.enabled);

    decision = evaluateBackgroundUsage(
        frameAt(MonotonicTime::max()),
        MonotonicTime::min(),
        usagePolicy());
    BAFX_CHECK(decision.status == BackgroundUsageStatus::FutureTimestamp);
    BAFX_CHECK(decision.age == MonotonicTime::min());
    BAFX_CHECK(!decision.enabled);
}

BAFX_TEST(background_path_latch_holds_across_the_acquire_boundary)
{
    BackgroundPathLatch latch;

    BAFX_CHECK(latch.select(true, true, true)
        == BackgroundRenderPath::BackgroundAware);
    BAFX_CHECK(latch.select(true, false, true)
        == BackgroundRenderPath::BackgroundAware);
    BAFX_CHECK(latch.select(true, true, true)
        == BackgroundRenderPath::BackgroundAware);
}

BAFX_TEST(background_path_latch_downgrades_once_at_the_retain_boundary)
{
    BackgroundPathLatch latch;

    BAFX_CHECK(latch.select(true, true, true)
        == BackgroundRenderPath::BackgroundAware);
    BAFX_CHECK(latch.select(true, false, false)
        == BackgroundRenderPath::FxOnly);
    BAFX_CHECK(latch.select(true, true, true)
        == BackgroundRenderPath::FxOnly);

    BAFX_CHECK(latch.select(false, false, false)
        == BackgroundRenderPath::FxOnly);
    BAFX_CHECK(latch.select(true, true, true)
        == BackgroundRenderPath::BackgroundAware);
}

BAFX_TEST(background_path_latch_keeps_a_captured_snapshot_after_retain_expiry)
{
    BackgroundPathLatch latch;

    BAFX_CHECK(latch.select(true, true, true)
        == BackgroundRenderPath::BackgroundAware);
    // The renderer replaces the live retain decision with true once the
    // immutable snapshot has been copied for this visible batch.
    BAFX_CHECK(latch.select(true, false, true)
        == BackgroundRenderPath::BackgroundAware);
    BAFX_CHECK(latch.select(true, false, true)
        == BackgroundRenderPath::BackgroundAware);
}

BAFX_TEST(background_path_latch_never_upgrades_a_visible_fx_only_batch)
{
    BackgroundPathLatch latch;

    BAFX_CHECK(latch.select(true, false, true)
        == BackgroundRenderPath::FxOnly);
    BAFX_CHECK(latch.select(true, true, true)
        == BackgroundRenderPath::FxOnly);
}

BAFX_TEST(background_path_latch_snapshot_failure_forces_current_batch_fx_only)
{
    BackgroundPathLatch latch;

    BAFX_CHECK(latch.select(true, true, true)
        == BackgroundRenderPath::BackgroundAware);
    latch.forceFxOnly();
    BAFX_CHECK(latch.select(true, true, true)
        == BackgroundRenderPath::FxOnly);

    BAFX_CHECK(latch.select(false, false, false)
        == BackgroundRenderPath::FxOnly);
    BAFX_CHECK(latch.select(true, true, true)
        == BackgroundRenderPath::BackgroundAware);
}

BAFX_TEST(background_path_latch_reset_starts_a_new_capture_session)
{
    BackgroundPathLatch latch;

    // The old session was forced to FX-only after its background sample aged
    // out. A new capture session must be allowed to acquire independently.
    BAFX_CHECK(latch.select(true, false, false)
        == BackgroundRenderPath::FxOnly);
    BAFX_CHECK(latch.select(true, true, true)
        == BackgroundRenderPath::FxOnly);

    latch.reset();

    BAFX_CHECK(latch.select(true, true, true)
        == BackgroundRenderPath::BackgroundAware);
}

BAFX_TEST(background_path_latch_uses_strict_pause_results_without_reupgrade)
{
    BackgroundPathLatch latch;

    BAFX_CHECK(latch.select(true, true, true)
        == BackgroundRenderPath::BackgroundAware);
    // Paused redraws pass the same strict decision as acquire and retain.
    BAFX_CHECK(latch.select(true, false, false)
        == BackgroundRenderPath::FxOnly);
    BAFX_CHECK(latch.select(true, true, true)
        == BackgroundRenderPath::FxOnly);
}
