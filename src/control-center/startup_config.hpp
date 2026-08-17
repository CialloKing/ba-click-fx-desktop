#pragma once

#include "bafx/config/config.hpp"

#include <filesystem>

namespace bafx::control_center
{

[[nodiscard]] std::filesystem::path startupConfigPath(
    const std::filesystem::path& executableDirectory);

[[nodiscard]] bafx::config::Config loadStartupConfig(
    const std::filesystem::path& executableDirectory);

}
