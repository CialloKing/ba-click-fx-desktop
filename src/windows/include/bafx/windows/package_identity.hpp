#pragma once

#include <windows.h>

#include <string>

namespace bafx::windows
{

struct PackageIdentityInfo
{
    bool present{false};
    std::string fullName{};
    std::string packagePath{};
    DWORD fullNameError{ERROR_SUCCESS};
    DWORD packagePathError{ERROR_SUCCESS};
    std::string familyName{};
    std::string name{};
    std::string publisher{};
    std::string publisherId{};
    std::string version{};
    std::string applicationUserModelId{};
    std::string stagedPath{};
    std::string effectiveExternalPath{};
    DWORD familyNameError{ERROR_SUCCESS};
    DWORD packageIdError{ERROR_SUCCESS};
    DWORD applicationUserModelIdError{ERROR_SUCCESS};
    DWORD stagedPathError{ERROR_SUCCESS};
    DWORD effectiveExternalPathError{ERROR_SUCCESS};
};

[[nodiscard]] PackageIdentityInfo queryCurrentPackageIdentity() noexcept;

[[nodiscard]] bool packageIdentityComplete(
    const PackageIdentityInfo& identity) noexcept;

[[nodiscard]] std::string packageIdentityDiagnostic(
    const PackageIdentityInfo& identity);

}
