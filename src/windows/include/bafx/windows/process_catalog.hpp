#pragma once

#include <windows.h>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bafx::windows
{

struct ProcessInfo final
{
    DWORD id{0U};
    std::wstring executableName{};
    std::filesystem::path executablePath{};
    DWORD pathError{ERROR_SUCCESS};
    bool sessionKnown{true};
};

struct ProcessSnapshot final
{
    std::vector<ProcessInfo> processes{};
    DWORD error{ERROR_SUCCESS};
    // An incomplete enumeration is never evidence that a process exited.
    bool complete{false};
};

[[nodiscard]] std::wstring processTextWide(std::string_view text);
[[nodiscard]] std::string processTextUtf8(std::wstring_view text);
[[nodiscard]] std::filesystem::path queryProcessExecutable(
    DWORD processId, DWORD* error = nullptr) noexcept;
// Names filter before opening processes. An empty filter is useful for the
// program picker; the Host passes only enabled rule basenames.
[[nodiscard]] ProcessSnapshot queryProcessSnapshot(
    std::span<const std::wstring> executableNames = {}, bool currentSessionOnly = true) noexcept;

}
