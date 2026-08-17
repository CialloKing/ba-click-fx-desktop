#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace bafx::windows
{

enum class StartupRegistrationStatus
{
    Unchanged,
    Updated,
    Removed,
    Failed
};

enum class StartupRegistrationOperation
{
    ValidateCommand,
    ValidateTarget,
    OpenKey,
    QueryValue,
    CreateKey,
    SetValue,
    DeleteValue
};

struct StartupRegistrationResult final
{
    StartupRegistrationStatus status{StartupRegistrationStatus::Failed};
    StartupRegistrationOperation operation{
        StartupRegistrationOperation::ValidateCommand};
    DWORD error{ERROR_SUCCESS};
    std::wstring commandLine{};

    [[nodiscard]] bool succeeded() const noexcept
    {
        return status != StartupRegistrationStatus::Failed;
    }
};

[[nodiscard]] std::string_view startupRegistrationOperationName(
    StartupRegistrationOperation operation) noexcept;

[[nodiscard]] std::wstring controlCenterStartupCommandLine(
    const std::filesystem::path& executable,
    bool startMinimized);

[[nodiscard]] StartupRegistrationResult applyControlCenterStartupRegistration(
    const std::filesystem::path& executable,
    bool enabled,
    bool startMinimized) noexcept;

}
