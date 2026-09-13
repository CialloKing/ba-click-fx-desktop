#pragma once

#include "localization.hpp"

#include <filesystem>

namespace bafx::control_center
{

[[nodiscard]] std::filesystem::path languagePreferencePath(const std::filesystem::path& executableDirectory);
[[nodiscard]] UiLanguage loadLanguagePreference(const std::filesystem::path& path) noexcept;
// Return a Win32 error; the caller publishes the new UI language only on success.
[[nodiscard]] DWORD saveLanguagePreference(const std::filesystem::path& path, UiLanguage language) noexcept;

}
