#pragma once

#include "bafx/windows/package_identity.hpp"

#include <appmodel.h>

namespace bafx::windows::detail
{

struct ParsedPackageId
{
    DWORD error{ERROR_SUCCESS};
    std::wstring name{};
    std::wstring publisher{};
    std::wstring publisherId{};
    PACKAGE_VERSION version{};
};

struct PackagePathQueryResult
{
    DWORD error{ERROR_SUCCESS};
    std::wstring value{};
    PackagePathQueryStatus status{PackagePathQueryStatus::NotRun};
    std::string_view apiModule{};
};

// Small query seams let tests exercise the native two-call contract and DLL
// fallback without registering a package or changing the user's installation.
using PackageApiResolver = FARPROC (*)(const wchar_t* module, const char* name);

[[nodiscard]] ParsedPackageId queryFullPackageId(
    const std::wstring& fullName,
    decltype(&PackageIdFromFullName) query) noexcept;

[[nodiscard]] PackagePathQueryResult queryEffectiveExternalPath(
    PackageApiResolver resolve) noexcept;

}
