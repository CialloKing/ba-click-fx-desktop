#include "test_support.hpp"

#include "config_commands.hpp"

#include "bafx/config/config.hpp"

#include <string>
#include <string_view>

BAFX_TEST(default_config_request_resets_settings_and_preserves_hotkeys)
{
    constexpr std::string_view prefix = "SetConfig ";
    bafx::config::HotkeysConfig hotkeys{};
    hotkeys.bindings[0] = bafx::config::HotkeyBinding{2U, 65U};
    hotkeys.bindings[3] = bafx::config::HotkeyBinding{3U, 81U};
    const std::string request = bafx::control_center::defaultConfigRequest(hotkeys);

    BAFX_CHECK(request.starts_with(prefix));
    BAFX_CHECK(request.find('\r') == std::string::npos);
    BAFX_CHECK(request.find('\n') == std::string::npos);
    BAFX_CHECK(request.find("paused") == std::string::npos);

    const std::string_view payload(
        request.data() + prefix.size(),
        request.size() - prefix.size());
    bafx::config::Config defaults = bafx::config::defaultConfig();
    defaults.hotkeys = hotkeys;
    const bafx::config::ConfigPatchResult patch =
        bafx::config::applyPatchJson(defaults, payload);
    BAFX_CHECK(!patch.recognized);

    const bafx::config::ConfigLoadResult parsed = bafx::config::parseJson(payload);
    BAFX_CHECK(parsed.succeeded());
    BAFX_CHECK(
        bafx::config::toJson(parsed.config, false)
        == bafx::config::toJson(defaults, false));
}

BAFX_TEST(display_override_requests_preserve_atomic_policy_fields)
{
    const bafx::config::DisplayOverrideConfig overrideConfig{
        "displayconfig-v1-sha256:key\\\"quoted",
        false,
        true,
        bafx::config::FramePacing::Fixed120};
    constexpr std::string_view setPrefix = "SetDisplayOverride ";
    const std::string setRequest =
        bafx::control_center::setDisplayOverrideRequest(17U, overrideConfig);
    BAFX_CHECK(setRequest.starts_with(setPrefix));

    const bafx::config::Config defaults = bafx::config::defaultConfig();
    const bafx::config::ConfigPatchResult setResult =
        bafx::config::applyDisplayOverrideJson(
            defaults,
            std::string_view(setRequest).substr(setPrefix.size()));
    BAFX_CHECK(setResult.succeeded());
    BAFX_CHECK(setResult.expectedGeneration == 17U);
    const bafx::config::DisplayOverrideConfig* stored =
        bafx::config::findDisplayOverride(
            setResult.config.display,
            overrideConfig.displayKey);
    BAFX_CHECK(stored != nullptr);
    BAFX_CHECK(!stored->enabled);
    BAFX_CHECK(stored->hdrEnabled);
    BAFX_CHECK(stored->framePacing == bafx::config::FramePacing::Fixed120);

    constexpr std::string_view removePrefix = "RemoveDisplayOverride ";
    const std::string removeRequest =
        bafx::control_center::removeDisplayOverrideRequest(
            18U,
            overrideConfig.displayKey);
    BAFX_CHECK(removeRequest.starts_with(removePrefix));
    const bafx::config::ConfigPatchResult removeResult =
        bafx::config::removeDisplayOverrideJson(
            setResult.config,
            std::string_view(removeRequest).substr(removePrefix.size()));
    BAFX_CHECK(removeResult.succeeded());
    BAFX_CHECK(removeResult.expectedGeneration == 18U);
    BAFX_CHECK(
        bafx::config::findDisplayOverride(
            removeResult.config.display,
            overrideConfig.displayKey)
        == nullptr);
}
