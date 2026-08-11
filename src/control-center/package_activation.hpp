#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace bafx::control_center
{

struct PackageActivationIdentity final
{
    std::wstring appUserModelId{};
};

struct PackageActivationIdentityResult final
{
    bool installStatePresent{false};
    std::optional<PackageActivationIdentity> identity{};
    std::wstring error{};

    [[nodiscard]] bool succeeded() const noexcept
    {
        return identity.has_value();
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
