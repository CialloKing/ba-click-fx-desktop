#include "bafx/windows/package_identity.hpp"

#include <appmodel.h>

#include <iomanip>
#include <sstream>
#include <vector>

namespace bafx::windows
{
namespace
{

struct PackageStringResult
{
    DWORD error{ERROR_SUCCESS};
    std::wstring value{};
};

template <typename Query>
[[nodiscard]] PackageStringResult queryPackageString(Query query) noexcept
{
    try
    {
        UINT32 length = 0U;
        const LONG initialResult = query(&length, nullptr);
        if (initialResult != ERROR_INSUFFICIENT_BUFFER)
        {
            return PackageStringResult{
                static_cast<DWORD>(initialResult),
                {}};
        }
        if (length == 0U)
        {
            return PackageStringResult{ERROR_INVALID_DATA, {}};
        }

        std::vector<wchar_t> buffer(length, L'\0');
        const LONG result = query(&length, buffer.data());
        if (result != ERROR_SUCCESS)
        {
            return PackageStringResult{static_cast<DWORD>(result), {}};
        }
        if (length > 0U && buffer[length - 1U] == L'\0')
        {
            --length;
        }
        return PackageStringResult{
            ERROR_SUCCESS,
            std::wstring(buffer.data(), length)};
    }
    catch (...)
    {
        return PackageStringResult{ERROR_NOT_ENOUGH_MEMORY, {}};
    }
}

[[nodiscard]] std::string wideToUtf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0)
    {
        return "<invalid-utf8>";
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        required,
        nullptr,
        nullptr);
    if (written != required)
    {
        return "<invalid-utf8>";
    }
    return result;
}

[[nodiscard]] std::string hexError(const DWORD error)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << error;
    return stream.str();
}

}

PackageIdentityInfo queryCurrentPackageIdentity() noexcept
{
    const PackageStringResult fullName =
        queryPackageString(GetCurrentPackageFullName);
    if (fullName.error == static_cast<DWORD>(APPMODEL_ERROR_NO_PACKAGE))
    {
        return PackageIdentityInfo{
            false,
            {},
            {},
            fullName.error,
            static_cast<DWORD>(APPMODEL_ERROR_NO_PACKAGE)};
    }

    const PackageStringResult packagePath =
        queryPackageString(GetCurrentPackagePath);
    if (fullName.error != ERROR_SUCCESS)
    {
        return PackageIdentityInfo{
            false,
            {},
            {},
            fullName.error,
            packagePath.error};
    }
    return PackageIdentityInfo{
        true,
        wideToUtf8(fullName.value),
        wideToUtf8(packagePath.value),
        fullName.error,
        packagePath.error};
}

std::string packageIdentityDiagnostic(const PackageIdentityInfo& identity)
{
    std::ostringstream stream;
    stream << "Package.Identity=" << (identity.present ? "present" : "absent")
           << ";FullNameError=" << hexError(identity.fullNameError)
           << ";PathError=" << hexError(identity.packagePathError);
    if (identity.present)
    {
        stream << ";FullName=" << identity.fullName
               << ";PackagePath=" << identity.packagePath;
    }
    return stream.str();
}

}
