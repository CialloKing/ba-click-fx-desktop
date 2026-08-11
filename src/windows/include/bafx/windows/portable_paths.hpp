#pragma once

#include <filesystem>
#include <string_view>

namespace bafx::windows
{

// Runtime-owned files are kept beside the image so a portable bundle does not
// create a second user-data profile under an OS-managed directory.
[[nodiscard]] std::filesystem::path executableDirectory();

// Keep caller-selected report names useful while discarding directory escapes.
[[nodiscard]] std::filesystem::path executableFilePath(
    std::wstring_view requestedName,
    std::wstring_view fallbackName);

}
