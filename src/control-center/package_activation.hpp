#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace bafx::control_center
{

enum class PackageActivationStateStatus
{
    Missing,
    Valid,
    Corrupt,
    VersionMismatch,
    BackupRecovered,
    PartialUpgrade
};

enum class PackageActivationStateSource
{
    None,
    Primary,
    Backup
};

struct PackageActivationIdentity final
{
    std::wstring appUserModelId{};
    std::string productVersion{};
    std::string packageVersion{};
};

struct PackageActivationIdentityResult final
{
    bool installStatePresent{false};
    PackageActivationStateStatus status{
        PackageActivationStateStatus::Missing};
    PackageActivationStateSource source{
        PackageActivationStateSource::None};
    std::optional<PackageActivationIdentity> identity{};
    std::wstring error{};

    [[nodiscard]] bool succeeded() const noexcept
    {
        return identity.has_value()
            && (status == PackageActivationStateStatus::Valid
                || status == PackageActivationStateStatus::BackupRecovered);
    }

    [[nodiscard]] bool recoveredFromBackup() const noexcept
    {
        return status == PackageActivationStateStatus::BackupRecovered
            && source == PackageActivationStateSource::Backup;
    }
};

struct PackageActivationResult final
{
    HRESULT result{E_FAIL};
    DWORD processId{0U};

    [[nodiscard]] bool succeeded() const noexcept
    {
        return SUCCEEDED(result);
    }
};

[[nodiscard]] PackageActivationIdentityResult parsePackageActivationState(
    std::string_view json) noexcept;

[[nodiscard]] PackageActivationIdentityResult readPackageActivationState(
    const std::filesystem::path& executableDirectory) noexcept;

[[nodiscard]] PackageActivationResult activatePackagedHost(
    const std::wstring& appUserModelId) noexcept;

}
