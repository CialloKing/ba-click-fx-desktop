#include "test_support.hpp"

#include "display_policy.hpp"

#include <chrono>
#include <string>

namespace
{

[[nodiscard]] bafx::desktop::DisplayTarget targetWithPath(
    std::wstring path)
{
    bafx::desktop::DisplayTarget target{};
    target.physicalTargetCount = 1U;
    target.physicalTargetIdentities = {
        bafx::desktop::DisplayPhysicalTargetIdentity{
            {},
            0U,
            std::move(path)}};
    return target;
}

}

BAFX_TEST(display_policy_inherits_global_defaults_without_a_stable_key)
{
    bafx::config::Config config = bafx::config::defaultConfig();
    config.display.hdrEnabled = true;
    config.performance.framePacing = bafx::config::FramePacing::Fixed60;

    const bafx::desktop::ResolvedDisplaySessionPolicy policy =
        bafx::desktop::resolveDisplaySessionPolicy(
            config,
            bafx::desktop::DisplayTarget{});
    BAFX_CHECK(!policy.displayKey.has_value());
    BAFX_CHECK(policy.enabled);
    BAFX_CHECK(policy.hdrEnabled);
    BAFX_CHECK(!policy.overridden);
    BAFX_CHECK(
        policy.outputPreference
        == bafx::windows::CompositionOutputPreference::PreferLinearScRgb);
    BAFX_CHECK(policy.minimumFramePeriod.has_value());
    BAFX_CHECK(
        *policy.minimumFramePeriod
        == bafx::core::MonotonicTime{16'666'667LL});
}

BAFX_TEST(display_policy_resolves_one_complete_target_override)
{
    const bafx::desktop::DisplayTarget target = targetWithPath(
        L"\\\\?\\DISPLAY#PANEL-A#1#{GUID}");
    const std::optional<std::string> displayKey =
        bafx::desktop::displayTargetPersistentKey(target);
    BAFX_CHECK(displayKey.has_value());

    bafx::config::Config config = bafx::config::defaultConfig();
    std::string error;
    BAFX_CHECK(bafx::config::setDisplayOverride(
        config,
        bafx::config::DisplayOverrideConfig{
            *displayKey,
            false,
            true,
            bafx::config::FramePacing::Fixed120},
        &error));

    const bafx::desktop::ResolvedDisplaySessionPolicy policy =
        bafx::desktop::resolveDisplaySessionPolicy(config, target);
    BAFX_CHECK(policy.displayKey == displayKey);
    BAFX_CHECK(!policy.enabled);
    BAFX_CHECK(policy.hdrEnabled);
    BAFX_CHECK(policy.overridden);
    BAFX_CHECK(
        policy.framePacing == bafx::config::FramePacing::Fixed120);
    BAFX_CHECK(
        policy.outputPreference
        == bafx::windows::CompositionOutputPreference::PreferLinearScRgb);
    BAFX_CHECK(policy.minimumFramePeriod.has_value());
    BAFX_CHECK(
        *policy.minimumFramePeriod
        == bafx::core::MonotonicTime{8'333'334LL});
}

BAFX_TEST(display_policy_period_uses_exact_non_early_deadlines)
{
    BAFX_CHECK(
        bafx::desktop::minimumFramePacingPeriod(
            bafx::config::FramePacing::MatchDisplay,
            bafx::windows::DisplayRefreshRate{170U, 1U})
        == bafx::core::MonotonicTime{5'882'353LL});
    BAFX_CHECK(
        bafx::desktop::minimumFramePacingPeriod(
            bafx::config::FramePacing::MatchDisplay,
            bafx::windows::DisplayRefreshRate{60'000U, 1'001U})
        == bafx::core::MonotonicTime{16'683'334LL});
    BAFX_CHECK(
        bafx::desktop::minimumFramePacingPeriod(
            bafx::config::FramePacing::MatchDisplay,
            std::nullopt)
        == bafx::core::MonotonicTime{16'666'667LL});
    BAFX_CHECK(
        bafx::desktop::minimumFramePacingPeriod(
            bafx::config::FramePacing::MatchDisplay,
            bafx::windows::DisplayRefreshRate{0U, 1U})
        == bafx::core::MonotonicTime{16'666'667LL});
    BAFX_CHECK(
        bafx::desktop::minimumFramePacingPeriod(
            bafx::config::FramePacing::MatchDisplay,
            bafx::windows::DisplayRefreshRate{1'001U, 1U})
        == bafx::core::MonotonicTime{16'666'667LL});
    BAFX_CHECK(
        bafx::desktop::minimumFramePacingPeriod(
            bafx::config::FramePacing::Fixed144,
            std::nullopt)
        == bafx::core::MonotonicTime{6'944'445LL});
    BAFX_CHECK(!bafx::desktop::minimumFramePacingPeriod(
        bafx::config::FramePacing::Unlimited,
        bafx::windows::DisplayRefreshRate{170U, 1U}).has_value());
}

BAFX_TEST(display_policy_match_display_uses_the_target_refresh_rate)
{
    bafx::desktop::DisplayTarget target{};
    target.refreshRate = bafx::windows::DisplayRefreshRate{170U, 1U};

    const bafx::desktop::ResolvedDisplaySessionPolicy policy =
        bafx::desktop::resolveDisplaySessionPolicy(
            bafx::config::defaultConfig(),
            target);
    BAFX_CHECK(
        policy.framePacing == bafx::config::FramePacing::MatchDisplay);
    BAFX_CHECK(
        policy.minimumFramePeriod
        == bafx::core::MonotonicTime{5'882'353LL});
}

BAFX_TEST(display_policy_core_mode_forces_conservative_fixed_sixty)
{
    bafx::config::Config config = bafx::config::defaultConfig();
    config.display.hdrEnabled = true;
    config.performance.framePacing = bafx::config::FramePacing::Fixed144;
    config.performance.effectsMode = bafx::config::EffectsMode::Core;

    const bafx::desktop::ResolvedDisplaySessionPolicy policy =
        bafx::desktop::resolveDisplaySessionPolicy(
            config,
            bafx::desktop::DisplayTarget{});
    BAFX_CHECK(!policy.hdrEnabled);
    BAFX_CHECK(
        policy.outputPreference
        == bafx::windows::CompositionOutputPreference::ConservativeSdr);
    BAFX_CHECK(policy.framePacing == bafx::config::FramePacing::Fixed60);
    BAFX_CHECK(
        policy.minimumFramePeriod
        == bafx::core::MonotonicTime{16'666'667LL});
}
