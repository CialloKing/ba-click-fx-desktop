#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

namespace bafx::windows
{

enum class StartupRegistrationStatus
{
    Unchanged,
    Updated,
    Removed,
    Failed
};

struct StartupRegistrationResult final
{
    StartupRegistrationStatus status{StartupRegistrationStatus::Failed};
    DWORD error{ERROR_SUCCESS};
    std::wstring commandLine{};

    [[nodiscard]] bool succeeded() const noexcept
    {
        return status != StartupRegistrationStatus::Failed;
    }
};

[[nodiscard]] std::wstring controlCenterStartupCommandLine(
    const std::filesystem::path& executable,
    bool startMinimized);

[[nodiscard]] StartupRegistrationResult applyControlCenterStartupRegistration(
    const std::filesystem::path& executable,
    bool enabled,
    bool startMinimized) noexcept;

}
