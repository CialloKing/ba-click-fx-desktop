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
};

[[nodiscard]] PackageIdentityInfo queryCurrentPackageIdentity() noexcept;

[[nodiscard]] std::string packageIdentityDiagnostic(
    const PackageIdentityInfo& identity);

}
