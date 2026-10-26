#include "bafx/windows/installer_maintenance.hpp"
#include "bafx/windows/unique_handle.hpp"

#include <shlobj.h>
#include <stdexcept>

namespace bafx::windows
{

std::filesystem::path officialInstallationDirectory()
{
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &value)))
    {
        throw std::runtime_error("Cannot resolve the protected installation directory");
    }
    const std::filesystem::path root(value);
    CoTaskMemFree(value);
    return root / L"ba-click-fx-desktop";
}

InstallerMaintenanceState queryMaintenanceEvent(const wchar_t* const name) noexcept
{
    const UniqueHandle event(OpenEventW(SYNCHRONIZE, FALSE, name));
    if (event.get() != nullptr)
    {
        return InstallerMaintenanceState::Active;
    }
    // Access denied or an object-type collision must not be interpreted as idle.
    return GetLastError() == ERROR_FILE_NOT_FOUND
        ? InstallerMaintenanceState::Idle : InstallerMaintenanceState::Unavailable;
}

InstallerMaintenanceState queryInstallerMaintenance(
    const std::filesystem::path& executableDirectory) noexcept
{
    try
    {
        const auto actual = std::filesystem::absolute(executableDirectory).lexically_normal().wstring();
        const auto expected = officialInstallationDirectory().lexically_normal().wstring();
        if (CompareStringOrdinal(actual.c_str(), -1, expected.c_str(), -1, TRUE) != CSTR_EQUAL)
        {
            return InstallerMaintenanceState::Idle;
        }
        return queryMaintenanceEvent();
    }
    catch (...)
    {
        return InstallerMaintenanceState::Unavailable;
    }
}

bool isElevatedProcess() noexcept
{
    HANDLE rawToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken) == FALSE)
    {
        return false;
    }
    const UniqueHandle token(rawToken);
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    return GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &bytes)
        != FALSE && elevation.TokenIsElevated != 0;
}

}
