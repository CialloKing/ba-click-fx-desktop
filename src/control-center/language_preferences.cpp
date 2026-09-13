#include "language_preferences.hpp"

#include "startup_config.hpp"

#include <array>
#include <fstream>

namespace bafx::control_center
{

std::filesystem::path languagePreferencePath(const std::filesystem::path& executableDirectory)
{
    return startupConfigPath(executableDirectory).parent_path() / L"BAFX.ControlCenter.language";
}

UiLanguage loadLanguagePreference(const std::filesystem::path& path) noexcept
{
    try
    {
        std::ifstream stream(path, std::ios::binary);
        std::array<char, 64U> buffer{};
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (!stream.eof() || stream.bad())
        {
            return UiLanguage::System;
        }
        return parseUiLanguage(std::string_view(buffer.data(), static_cast<std::size_t>(stream.gcount())));
    }
    catch (...)
    {
        return UiLanguage::System;
    }
}

DWORD saveLanguagePreference(const std::filesystem::path& path, const UiLanguage language) noexcept
{
    try
    {
        if (path.empty())
        {
            return ERROR_INVALID_NAME;
        }
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
        {
            return static_cast<DWORD>(error.value());
        }
        // Control Center is single-instance; a per-process sibling keeps the
        // replacement on the same volume and never truncates the saved choice.
        auto temporary = path;
        temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
        const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0U,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return GetLastError();
        }
        const auto token = uiLanguageToken(language);
        DWORD written = 0U;
        DWORD status = ERROR_SUCCESS;
        if (!WriteFile(file, token.data(), static_cast<DWORD>(token.size()), &written, nullptr)
            || written != token.size() || !FlushFileBuffers(file))
        {
            status = GetLastError();
            if (status == ERROR_SUCCESS)
            {
                status = ERROR_WRITE_FAULT;
            }
        }
        CloseHandle(file);
        if (status == ERROR_SUCCESS && !MoveFileExW(temporary.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            status = GetLastError();
        }
        if (status != ERROR_SUCCESS)
        {
            DeleteFileW(temporary.c_str());
        }
        return status;
    }
    catch (...)
    {
        return ERROR_WRITE_FAULT;
    }
}

}
