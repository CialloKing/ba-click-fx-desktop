#include "test_support.hpp"

#include "bafx/core/background_freshness.hpp"

#include <chrono>
#include <optional>

using namespace bafx::core;
using namespace std::chrono_literals;

namespace
{

BackgroundFreshnessPolicy policy()
{
    return BackgroundFreshnessPolicy{16ms, 48ms, 1ms, 7};
}

BackgroundFrameStamp frameAt(const MonotonicTime time)
{
    return BackgroundFrameStamp{time, 7, true, true};
}

BackgroundUsagePolicy usagePolicy()
{
    return BackgroundUsagePolicy{policy(), 250ms, 48ms};
}

}

BAFX_TEST(background_freshness_has_closed_full_weight_boundary)
{
    const auto result = evaluateBackgroundFreshness(frameAt(84ms), 100ms, policy());
    BAFX_CHECK(result.freshness == BackgroundFreshness::Fresh);
    BAFX_CHECK_NEAR(result.weight, 1.0F, 0.0F);
}

BAFX_TEST(background_freshness_fades_and_then_stops)
{
    auto result = evaluateBackgroundFreshness(frameAt(68ms), 100ms, policy());
    BAFX_CHECK(result.freshness == BackgroundFreshness::Fading);
    BAFX_CHECK_NEAR(result.weight, 0.5F, 1.0e-6F);

    result = evaluateBackgroundFreshness(frameAt(52ms), 100ms, policy());
    BAFX_CHECK(result.freshness == BackgroundFreshness::Stale);
    BAFX_CHECK_NEAR(result.weight, 0.0F, 0.0F);
    BAFX_CHECK(selectBloomInputMode(result, BackgroundFallback::FxOnlyBloom)
        == BloomInputMode::FxOnly);
}

BAFX_TEST(background_freshness_rejects_future_epoch_and_contract_errors)
{
    auto result = evaluateBackgroundFreshness(frameAt(101ms), 100ms, policy());
    BAFX_CHECK(result.freshness == BackgroundFreshness::Fresh);

    result = evaluateBackgroundFreshness(frameAt(101ms + 1ns), 100ms, policy());
    BAFX_CHECK(result.freshness == BackgroundFreshness::FutureTimestamp);

    auto wrongEpoch = frameAt(100ms);
    wrongEpoch.epoch = 8;
    result = evaluateBackgroundFreshness(wrongEpoch, 100ms, policy());
    BAFX_CHECK(result.freshness == BackgroundFreshness::WrongEpoch);

    auto badContract = frameAt(100ms);
    badContract.excludesOwnOverlay = false;
    result = evaluateBackgroundFreshness(badContract, 100ms, policy());
    BAFX_CHECK(result.freshness == BackgroundFreshness::InvalidContract);
}

BAFX_TEST(background_freshness_validates_missing_and_policy)
{
    auto result = evaluateBackgroundFreshness(std::nullopt, 100ms, policy());
    BAFX_CHECK(result.freshness == BackgroundFreshness::Missing);

    auto invalidPolicy = policy();
    invalidPolicy.staleAge = invalidPolicy.fullWeightAge;
    result = evaluateBackgroundFreshness(frameAt(100ms), 100ms, invalidPolicy);
    BAFX_CHECK(result.freshness == BackgroundFreshness::InvalidPolicy);
}

BAFX_TEST(background_usage_keeps_transport_stable_after_bloom_fades)
{
    auto decision = evaluateBackgroundUsage(frameAt(52ms), 100ms, usagePolicy());
    BAFX_CHECK(decision.freshness.freshness == BackgroundFreshness::Stale);
    BAFX_CHECK_NEAR(decision.freshness.weight, 0.0F, 0.0F);
    BAFX_CHECK(decision.transportEnabled);

    decision = evaluateBackgroundUsage(frameAt(32ms), 0ms, usagePolicy());
    BAFX_CHECK(
        decision.freshness.freshness == BackgroundFreshness::FutureTimestamp);
    BAFX_CHECK_NEAR(decision.freshness.weight, 0.0F, 0.0F);
    BAFX_CHECK(decision.transportEnabled);
}

BAFX_TEST(background_usage_transport_window_is_bounded)
{
    auto decision = evaluateBackgroundUsage(frameAt(-250ms), 0ms, usagePolicy());
    BAFX_CHECK(decision.freshness.freshness == BackgroundFreshness::Stale);
    BAFX_CHECK(!decision.transportEnabled);

    decision = evaluateBackgroundUsage(frameAt(48ms + 1ns), 0ms, usagePolicy());
    BAFX_CHECK(
        decision.freshness.freshness == BackgroundFreshness::FutureTimestamp);
    BAFX_CHECK(!decision.transportEnabled);

    auto invalidContract = frameAt(0ms);
    invalidContract.excludesOwnOverlay = false;
    decision = evaluateBackgroundUsage(invalidContract, 0ms, usagePolicy());
    BAFX_CHECK(
        decision.freshness.freshness == BackgroundFreshness::InvalidContract);
    BAFX_CHECK(!decision.transportEnabled);
}
