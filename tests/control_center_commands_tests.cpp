#include "test_support.hpp"

#include "config_commands.hpp"

#include "bafx/config/config.hpp"

#include <string>
#include <string_view>

BAFX_TEST(default_config_request_replaces_the_complete_persisted_config)
{
    constexpr std::string_view prefix = "SetConfig ";
    const std::string request = bafx::control_center::defaultConfigRequest();

    BAFX_CHECK(request.starts_with(prefix));
    BAFX_CHECK(request.find('\r') == std::string::npos);
    BAFX_CHECK(request.find('\n') == std::string::npos);
    BAFX_CHECK(request.find("paused") == std::string::npos);

    const std::string_view payload(
        request.data() + prefix.size(),
        request.size() - prefix.size());
    const bafx::config::Config defaults = bafx::config::defaultConfig();
    const bafx::config::ConfigPatchResult patch =
        bafx::config::applyPatchJson(defaults, payload);
    BAFX_CHECK(!patch.recognized);

    const bafx::config::ConfigLoadResult parsed = bafx::config::parseJson(payload);
    BAFX_CHECK(parsed.succeeded());
    BAFX_CHECK(
        bafx::config::toJson(parsed.config, false)
        == bafx::config::toJson(defaults, false));
}
