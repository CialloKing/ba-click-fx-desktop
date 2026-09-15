#include "test_support.hpp"

#include "bafx/windows/detail/package_identity_query.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>

namespace
{

struct QueryFixture
{
    std::array<UINT32, 2> idFlags{};
    std::size_t idCalls{0U};
    LONG idError{ERROR_SUCCESS};
    bool kernel32Exports{false};
    bool kernelBaseExports{true};
    std::size_t resolverCalls{0U};
    LONG pathError{ERROR_SUCCESS};
};

QueryFixture fixture{};

LONG WINAPI fakePackageId(
    PCWSTR, const UINT32 flags, UINT32* length, BYTE* buffer)
{
    if (fixture.idCalls < fixture.idFlags.size())
    {
        fixture.idFlags[fixture.idCalls] = flags;
    }
    ++fixture.idCalls;
    if (fixture.idError != ERROR_SUCCESS)
    {
        return fixture.idError;
    }
    if (buffer == nullptr)
    {
        *length = sizeof(PACKAGE_ID);
        return ERROR_INSUFFICIENT_BUFFER;
    }
    if (*length < sizeof(PACKAGE_ID))
    {
        return ERROR_INSUFFICIENT_BUFFER;
    }
    static wchar_t name[] = L"BafxTest";
    static wchar_t publisher[] = L"CN=BafxTest";
    static wchar_t publisherId[] = L"publisher-id";
    PACKAGE_ID id{};
    id.name = name;
    id.publisher = flags == PACKAGE_INFORMATION_FULL ? publisher : nullptr;
    id.publisherId = publisherId;
    id.version.Major = 1U;
    std::memcpy(buffer, &id, sizeof(id));
    return ERROR_SUCCESS;
}

LONG WINAPI fakePackagePath(const UINT32 type, UINT32* length, PWSTR buffer)
{
    if (type != 5U)
    {
        return ERROR_INVALID_PARAMETER;
    }
    if (fixture.pathError != ERROR_SUCCESS)
    {
        return fixture.pathError;
    }
    constexpr wchar_t path[] = L"C:\\Program Files\\BAFX";
    constexpr UINT32 count = sizeof(path) / sizeof(wchar_t);
    if (buffer == nullptr || *length < count)
    {
        *length = count;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    std::copy_n(path, count, buffer);
    *length = count;
    return ERROR_SUCCESS;
}

FARPROC fakeResolver(const wchar_t* module, const char* name)
{
    ++fixture.resolverCalls;
    if (std::strcmp(name, "GetCurrentPackagePath2") != 0)
    {
        return nullptr;
    }
    const bool available = std::wcscmp(module, L"kernel32.dll") == 0
        ? fixture.kernel32Exports : fixture.kernelBaseExports;
    return available ? reinterpret_cast<FARPROC>(fakePackagePath) : nullptr;
}

}

BAFX_TEST(package_identity_queries_full_information_on_both_calls)
{
    fixture = {};
    const auto id = bafx::windows::detail::queryFullPackageId(L"test", fakePackageId);
    BAFX_CHECK(id.error == ERROR_SUCCESS);
    BAFX_CHECK(id.name == L"BafxTest");
    BAFX_CHECK(id.publisher == L"CN=BafxTest");
    BAFX_CHECK(id.publisherId == L"publisher-id");
    BAFX_CHECK(id.version.Major == 1U);
    BAFX_CHECK(fixture.idCalls == 2U);
    BAFX_CHECK(fixture.idFlags[0] == PACKAGE_INFORMATION_FULL);
    BAFX_CHECK(fixture.idFlags[1] == PACKAGE_INFORMATION_FULL);
}

BAFX_TEST(package_identity_preserves_the_full_information_query_error)
{
    fixture = {};
    fixture.idError = ERROR_NOT_FOUND;
    const auto id = bafx::windows::detail::queryFullPackageId(L"test", fakePackageId);
    BAFX_CHECK(id.error == ERROR_NOT_FOUND);
    BAFX_CHECK(id.publisher.empty());
    BAFX_CHECK(fixture.idCalls == 1U);
}

BAFX_TEST(package_external_path_uses_kernelbase_when_kernel32_has_no_export)
{
    fixture = {};
    const auto path = bafx::windows::detail::queryEffectiveExternalPath(fakeResolver);
    BAFX_CHECK(path.status == bafx::windows::PackagePathQueryStatus::Succeeded);
    BAFX_CHECK(path.error == ERROR_SUCCESS);
    BAFX_CHECK(path.value == L"C:\\Program Files\\BAFX");
    BAFX_CHECK(path.apiModule == "kernelbase.dll");
    BAFX_CHECK(fixture.resolverCalls == 2U);
}

BAFX_TEST(package_external_path_separates_missing_api_from_query_failure)
{
    using namespace bafx::windows;
    fixture = {};
    fixture.kernelBaseExports = false;
    const auto missing = detail::queryEffectiveExternalPath(fakeResolver);
    BAFX_CHECK(missing.status == PackagePathQueryStatus::ApiUnavailable);
    BAFX_CHECK(missing.error == ERROR_CALL_NOT_IMPLEMENTED);
    BAFX_CHECK(missing.apiModule.empty());

    fixture = {};
    fixture.kernel32Exports = true;
    fixture.pathError = ERROR_CALL_NOT_IMPLEMENTED;
    const auto failed = detail::queryEffectiveExternalPath(fakeResolver);
    BAFX_CHECK(failed.status == PackagePathQueryStatus::QueryFailed);
    BAFX_CHECK(failed.error == ERROR_CALL_NOT_IMPLEMENTED);
    BAFX_CHECK(failed.value.empty());
    BAFX_CHECK(failed.apiModule == "kernel32.dll");
    BAFX_CHECK(fixture.resolverCalls == 1U);

    PackageIdentityInfo identity{};
    identity.effectiveExternalPathError = failed.error;
    identity.effectiveExternalPathQueryStatus = failed.status;
    identity.effectiveExternalPathApiModule = failed.apiModule;
    const std::string text = packageIdentityDiagnostic(identity);
    BAFX_CHECK(text.find("EffectiveExternalPath.Query=query-failed") != std::string::npos);
    BAFX_CHECK(text.find("EffectiveExternalPath.ApiModule=kernel32.dll") != std::string::npos);
    BAFX_CHECK(text.find("EffectiveExternalPathError=0x00000078") != std::string::npos);
    BAFX_CHECK(!packageIdentityComplete(identity));
}
