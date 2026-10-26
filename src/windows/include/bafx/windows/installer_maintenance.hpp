#pragma once

#include <filesystem>
#include <windows.h>

namespace bafx::windows
{

inline constexpr wchar_t kInstallerMaintenanceEvent[] = L"Global\\BAFX.InstallerMaintenance.v1";
inline constexpr wchar_t kPrepareForMaintenanceMessage[] = L"BAFX.PrepareForMaintenance.v1";
inline constexpr int kInstallerMaintenanceExitCode = ERROR_INSTALL_ALREADY_RUNNING;

enum class InstallerMaintenanceState
{
    Idle,
    Active,
    Unavailable
};

[[nodiscard]] std::filesystem::path officialInstallationDirectory();
[[nodiscard]] InstallerMaintenanceState queryMaintenanceEvent(
    const wchar_t* name = kInstallerMaintenanceEvent) noexcept;
[[nodiscard]] InstallerMaintenanceState queryInstallerMaintenance(
    const std::filesystem::path& executableDirectory) noexcept;
[[nodiscard]] bool isElevatedProcess() noexcept;

}
