#include "bafx/windows/portable_paths.hpp"

#include "bafx/windows/error.hpp"
#include "bafx/windows/package_identity.hpp"

#include <windows.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace bafx::windows
{

std::filesystem::path executableDirectory()
{
    // Grow the buffer instead of accepting a truncated module path; a
    // truncated path could otherwise redirect runtime data to the wrong tree.
    constexpr DWORD maximumPathCharacters = 32'768U;
    std::vector<wchar_t> buffer(512U, L'\0');
    for (;;)
    {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0U)
        {
            throwLastError("GetModuleFileNameW");
        }
        if (length < buffer.size())
        {
            return std::filesystem::path(
                std::wstring(buffer.data(), length)).parent_path();
        }
        if (buffer.size() >= maximumPathCharacters)
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            throwLastError("GetModuleFileNameW");
        }
        buffer.resize(buffer.size() * 2U, L'\0');
    }
}

std::filesystem::path runtimeDataDirectory()
{
    const std::filesystem::path root = executableDirectory();
    if (!queryCurrentPackageIdentity().present)
    {
        return root;
    }
    return root / L"data";
}

std::filesystem::path executableFilePath(
    const std::wstring_view requestedName,
    const std::wstring_view fallbackName)
{
    std::filesystem::path filename = std::filesystem::path(requestedName).filename();
    if (filename.empty() || filename == L"." || filename == L"..")
    {
        filename = std::filesystem::path(fallbackName).filename();
    }
    if (filename.empty() || filename == L"." || filename == L"..")
    {
        throw std::invalid_argument("portable file name is empty");
    }
    return runtimeDataDirectory() / filename;
}

}
