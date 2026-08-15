#include "bafx/windows/package_identity.hpp"

#include <appmodel.h>

#include <cstddef>
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

struct ParsedPackageId
{
    DWORD error{ERROR_SUCCESS};
    std::wstring name{};
    std::wstring publisher{};
    std::wstring publisherId{};
    PACKAGE_VERSION version{};
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

[[nodiscard]] ParsedPackageId parsePackageId(
    const std::wstring& fullName) noexcept
{
    try
    {
        UINT32 bufferLength = 0U;
        const LONG initialResult = PackageIdFromFullName(
            fullName.c_str(),
            PACKAGE_INFORMATION_BASIC,
            &bufferLength,
            nullptr);
        if (initialResult != ERROR_INSUFFICIENT_BUFFER)
        {
            return ParsedPackageId{static_cast<DWORD>(initialResult)};
        }
        if (bufferLength < sizeof(PACKAGE_ID))
        {
            return ParsedPackageId{ERROR_INVALID_DATA};
        }

        std::vector<std::byte> buffer(bufferLength);
        auto* packageId = reinterpret_cast<PACKAGE_ID*>(buffer.data());
        const LONG result = PackageIdFromFullName(
            fullName.c_str(),
            PACKAGE_INFORMATION_BASIC,
            &bufferLength,
            reinterpret_cast<BYTE*>(buffer.data()));
        if (result != ERROR_SUCCESS)
        {
            return ParsedPackageId{static_cast<DWORD>(result)};
        }
        if (packageId->name == nullptr
            || packageId->publisher == nullptr
            || packageId->publisherId == nullptr)
        {
            return ParsedPackageId{ERROR_INVALID_DATA};
        }
        return ParsedPackageId{
            ERROR_SUCCESS,
            packageId->name,
            packageId->publisher,
            packageId->publisherId,
            packageId->version};
    }
    catch (...)
    {
        return ParsedPackageId{ERROR_NOT_ENOUGH_MEMORY};
    }
}

[[nodiscard]] PackageStringResult queryEffectiveExternalPath() noexcept
{
    using GetCurrentPackagePath2Function = LONG(WINAPI*)(
        UINT32,
        UINT32*,
        PWSTR);

    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32 == nullptr)
    {
        return PackageStringResult{GetLastError(), {}};
    }
    const auto query = reinterpret_cast<GetCurrentPackagePath2Function>(
        GetProcAddress(kernel32, "GetCurrentPackagePath2"));
    if (query == nullptr)
    {
        return PackageStringResult{ERROR_CALL_NOT_IMPLEMENTED, {}};
    }

    // PackagePathType_EffectiveExternal is ABI value 5. Resolve the function
    // dynamically so older Windows builds can still load the complete binary.
    return queryPackageString(
        [query](UINT32* length, PWSTR buffer)
        {
            return query(5U, length, buffer);
        });
}

[[nodiscard]] std::string packageVersionText(
    const PACKAGE_VERSION version)
{
    std::ostringstream stream;
    stream << version.Major << '.' << version.Minor << '.' << version.Build
           << '.' << version.Revision;
    return stream.str();
}

}

PackageIdentityInfo queryCurrentPackageIdentity() noexcept
{
    const PackageStringResult fullName =
        queryPackageString(GetCurrentPackageFullName);
    PackageIdentityInfo identity{};
    identity.fullNameError = fullName.error;
    if (fullName.error == static_cast<DWORD>(APPMODEL_ERROR_NO_PACKAGE))
    {
        identity.packagePathError = fullName.error;
        identity.familyNameError = fullName.error;
        identity.packageIdError = fullName.error;
        identity.applicationUserModelIdError = fullName.error;
        identity.stagedPathError = fullName.error;
        identity.effectiveExternalPathError = fullName.error;
        return identity;
    }

    const PackageStringResult packagePath =
        queryPackageString(GetCurrentPackagePath);
    const PackageStringResult familyName =
        queryPackageString(GetCurrentPackageFamilyName);
    const PackageStringResult applicationUserModelId =
        queryPackageString(GetCurrentApplicationUserModelId);
    const PackageStringResult stagedPath = fullName.error == ERROR_SUCCESS
        ? queryPackageString(
            [&fullName](UINT32* length, PWSTR buffer)
            {
                return GetStagedPackagePathByFullName(
                    fullName.value.c_str(),
                    length,
                    buffer);
            })
        : PackageStringResult{fullName.error, {}};
    const PackageStringResult effectiveExternalPath =
        queryEffectiveExternalPath();
    const ParsedPackageId packageId = fullName.error == ERROR_SUCCESS
        ? parsePackageId(fullName.value)
        : ParsedPackageId{fullName.error};

    identity.present = fullName.error == ERROR_SUCCESS;
    identity.fullName = wideToUtf8(fullName.value);
    identity.packagePath = wideToUtf8(packagePath.value);
    identity.packagePathError = packagePath.error;
    identity.familyName = wideToUtf8(familyName.value);
    identity.familyNameError = familyName.error;
    identity.name = wideToUtf8(packageId.name);
    identity.publisher = wideToUtf8(packageId.publisher);
    identity.publisherId = wideToUtf8(packageId.publisherId);
    identity.packageIdError = packageId.error;
    if (packageId.error == ERROR_SUCCESS)
    {
        identity.version = packageVersionText(packageId.version);
    }
    identity.applicationUserModelId = wideToUtf8(applicationUserModelId.value);
    identity.applicationUserModelIdError = applicationUserModelId.error;
    identity.stagedPath = wideToUtf8(stagedPath.value);
    identity.stagedPathError = stagedPath.error;
    identity.effectiveExternalPath = wideToUtf8(effectiveExternalPath.value);
    identity.effectiveExternalPathError = effectiveExternalPath.error;
    return identity;
}

bool packageIdentityComplete(const PackageIdentityInfo& identity) noexcept
{
    return identity.present
        && identity.fullNameError == ERROR_SUCCESS
        && identity.packagePathError == ERROR_SUCCESS
        && identity.familyNameError == ERROR_SUCCESS
        && identity.packageIdError == ERROR_SUCCESS
        && identity.applicationUserModelIdError == ERROR_SUCCESS
        && identity.stagedPathError == ERROR_SUCCESS
        && identity.effectiveExternalPathError == ERROR_SUCCESS
        && !identity.fullName.empty()
        && !identity.packagePath.empty()
        && !identity.familyName.empty()
        && !identity.name.empty()
        && !identity.publisher.empty()
        && !identity.publisherId.empty()
        && !identity.version.empty()
        && !identity.applicationUserModelId.empty()
        && !identity.stagedPath.empty()
        && !identity.effectiveExternalPath.empty();
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
               << ";PackagePath=" << identity.packagePath
               << ";FamilyName=" << identity.familyName
               << ";Name=" << identity.name
               << ";Publisher=" << identity.publisher
               << ";PublisherId=" << identity.publisherId
               << ";Version=" << identity.version
               << ";AUMID=" << identity.applicationUserModelId
               << ";StagedPath=" << identity.stagedPath
               << ";EffectiveExternalPath="
               << identity.effectiveExternalPath;
    }
    stream << ";FamilyNameError=" << hexError(identity.familyNameError)
           << ";PackageIdError=" << hexError(identity.packageIdError)
           << ";AUMIDError="
           << hexError(identity.applicationUserModelIdError)
           << ";StagedPathError=" << hexError(identity.stagedPathError)
           << ";EffectiveExternalPathError="
           << hexError(identity.effectiveExternalPathError)
           << ";Complete="
           << (packageIdentityComplete(identity) ? "true" : "false");
    return stream.str();
}

}
