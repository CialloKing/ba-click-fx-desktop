#include "bafx/windows/startup_registration.hpp"

#include <utility>
#include <vector>

namespace bafx::windows
{
namespace
{

constexpr wchar_t startupKeyPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t startupValueName[] = L"BAFX Control Center";

class RegistryKey final
{
public:
    RegistryKey() = default;

    ~RegistryKey()
    {
        if (key_ != nullptr)
        {
            RegCloseKey(key_);
        }
    }

    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    [[nodiscard]] HKEY* receive() noexcept
    {
        return &key_;
    }

    [[nodiscard]] HKEY get() const noexcept
    {
        return key_;
    }

private:
    HKEY key_{nullptr};
};

[[nodiscard]] StartupRegistrationResult failure(
    const DWORD error,
    std::wstring commandLine = {}) noexcept
{
    return StartupRegistrationResult{
        StartupRegistrationStatus::Failed,
        error,
        std::move(commandLine)};
}

[[nodiscard]] StartupRegistrationResult removeRegistration() noexcept
{
    RegistryKey key;
    const LSTATUS openStatus = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        startupKeyPath,
        0U,
        KEY_SET_VALUE,
        key.receive());
    if (openStatus == ERROR_FILE_NOT_FOUND)
    {
        return StartupRegistrationResult{
            StartupRegistrationStatus::Unchanged};
    }
    if (openStatus != ERROR_SUCCESS)
    {
        return failure(static_cast<DWORD>(openStatus));
    }

    const LSTATUS deleteStatus = RegDeleteValueW(key.get(), startupValueName);
    if (deleteStatus == ERROR_FILE_NOT_FOUND)
    {
        return StartupRegistrationResult{
            StartupRegistrationStatus::Unchanged};
    }
    if (deleteStatus != ERROR_SUCCESS)
    {
        return failure(static_cast<DWORD>(deleteStatus));
    }
    return StartupRegistrationResult{
        StartupRegistrationStatus::Removed};
}

[[nodiscard]] LSTATUS queryRegistration(
    const HKEY key,
    std::wstring& value)
{
    DWORD type = REG_NONE;
    DWORD byteCount = 0U;
    LSTATUS status = RegQueryValueExW(
        key,
        startupValueName,
        nullptr,
        &type,
        nullptr,
        &byteCount);
    if (status != ERROR_SUCCESS)
    {
        return status;
    }
    if (type != REG_SZ || byteCount < sizeof(wchar_t))
    {
        return ERROR_DATATYPE_MISMATCH;
    }

    std::vector<wchar_t> buffer(
        static_cast<std::size_t>(byteCount / sizeof(wchar_t)) + 1U,
        L'\0');
    status = RegQueryValueExW(
        key,
        startupValueName,
        nullptr,
        &type,
        reinterpret_cast<BYTE*>(buffer.data()),
        &byteCount);
    if (status != ERROR_SUCCESS)
    {
        return status;
    }
    value.assign(buffer.data());
    return ERROR_SUCCESS;
}

}

std::wstring controlCenterStartupCommandLine(
    const std::filesystem::path& executable,
    const bool startMinimized)
{
    const std::wstring path = executable.wstring();
    if (path.empty() || path.find(L'"') != std::wstring::npos)
    {
        return {};
    }

    std::wstring commandLine = L"\"" + path + L"\" --startup";
    if (startMinimized)
    {
        commandLine += L" --minimized";
    }
    return commandLine;
}

StartupRegistrationResult applyControlCenterStartupRegistration(
    const std::filesystem::path& executable,
    const bool enabled,
    const bool startMinimized) noexcept
{
    try
    {
        if (!enabled)
        {
            return removeRegistration();
        }

        std::wstring commandLine = controlCenterStartupCommandLine(
            executable,
            startMinimized);
        if (commandLine.empty())
        {
            return failure(ERROR_INVALID_NAME);
        }
        if (!std::filesystem::is_regular_file(executable))
        {
            return failure(ERROR_FILE_NOT_FOUND, std::move(commandLine));
        }

        RegistryKey key;
        const LSTATUS createStatus = RegCreateKeyExW(
            HKEY_CURRENT_USER,
            startupKeyPath,
            0U,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_QUERY_VALUE | KEY_SET_VALUE,
            nullptr,
            key.receive(),
            nullptr);
        if (createStatus != ERROR_SUCCESS)
        {
            return failure(
                static_cast<DWORD>(createStatus),
                std::move(commandLine));
        }

        std::wstring existing;
        const LSTATUS queryStatus = queryRegistration(key.get(), existing);
        if (queryStatus == ERROR_SUCCESS && existing == commandLine)
        {
            return StartupRegistrationResult{
                StartupRegistrationStatus::Unchanged,
                ERROR_SUCCESS,
                std::move(commandLine)};
        }
        if (queryStatus != ERROR_SUCCESS
            && queryStatus != ERROR_FILE_NOT_FOUND
            && queryStatus != ERROR_DATATYPE_MISMATCH)
        {
            return failure(
                static_cast<DWORD>(queryStatus),
                std::move(commandLine));
        }

        const DWORD byteCount = static_cast<DWORD>(
            (commandLine.size() + 1U) * sizeof(wchar_t));
        const LSTATUS setStatus = RegSetValueExW(
            key.get(),
            startupValueName,
            0U,
            REG_SZ,
            reinterpret_cast<const BYTE*>(commandLine.c_str()),
            byteCount);
        if (setStatus != ERROR_SUCCESS)
        {
            return failure(
                static_cast<DWORD>(setStatus),
                std::move(commandLine));
        }
        return StartupRegistrationResult{
            StartupRegistrationStatus::Updated,
            ERROR_SUCCESS,
            std::move(commandLine)};
    }
    catch (...)
    {
        return failure(ERROR_NOT_ENOUGH_MEMORY);
    }
}

}
