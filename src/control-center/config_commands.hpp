#pragma once

#include "bafx/config/config.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace bafx::control_center
{

[[nodiscard]] std::string defaultConfigRequest();
[[nodiscard]] std::string setDisplayOverrideRequest(
    std::uint64_t generation,
    const bafx::config::DisplayOverrideConfig& overrideConfig);
[[nodiscard]] std::string removeDisplayOverrideRequest(
    std::uint64_t generation,
    std::string_view displayKey);

}
