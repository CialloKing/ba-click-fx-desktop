#include "test_support.hpp"

#include "identity_signer_options.hpp"

#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace
{

template<typename Function>
[[nodiscard]] bool throwsInvalidArgument(Function&& function)
{
    try
    {
        function();
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }
    return false;
}

class TemporaryMsix final
{
public:
    TemporaryMsix()
    {
        path_ = std::filesystem::temp_directory_path()
            / (L"bafx-identity-signer-options-test-"
                + std::to_wstring(GetCurrentProcessId()) + L".msix");
        std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
        stream << "test";
        if (!stream)
        {
            throw std::runtime_error("Could not create the identity signer test file");
        }
    }

    ~TemporaryMsix()
    {
        // This fixed test-owned file contains no user data.
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    TemporaryMsix(const TemporaryMsix&) = delete;
    TemporaryMsix& operator=(const TemporaryMsix&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_{};
};

}

BAFX_TEST(identity_signer_thumbprint_accepts_mixed_case_hex)
{
    const auto thumbprint = bafx::identity_signer::parseThumbprint(
        L"0123456789abcdefABCDEF0123456789abcdefAB");

    BAFX_CHECK(thumbprint.front() == 0x01U);
    BAFX_CHECK(thumbprint[5U] == 0xABU);
    BAFX_CHECK(thumbprint.back() == 0xABU);
}

BAFX_TEST(identity_signer_thumbprint_rejects_wrong_length_and_separators)
{
    BAFX_CHECK(throwsInvalidArgument([]
    {
        static_cast<void>(bafx::identity_signer::parseThumbprint(
            L"0123456789abcdefABCDEF0123456789abcdefA"));
    }));
    BAFX_CHECK(throwsInvalidArgument([]
    {
        static_cast<void>(bafx::identity_signer::parseThumbprint(
            L"0123456789abcdefABCDEF0123456789abcde-AB"));
    }));
}

BAFX_TEST(identity_signer_arguments_are_order_independent)
{
    constexpr std::array arguments{
        std::wstring_view(L"--thumbprint"),
        std::wstring_view(L"0123456789abcdefABCDEF0123456789abcdefAB"),
        std::wstring_view(L"--store-location"),
        std::wstring_view(L"LocalMachine"),
        std::wstring_view(L"--package"),
        std::wstring_view(L"C:\\release\\identity.msix")};

    const auto options = bafx::identity_signer::parseOptions(arguments);

    BAFX_CHECK(options.packagePath == L"C:\\release\\identity.msix");
    BAFX_CHECK(options.storeLocation
        == bafx::identity_signer::CertificateStoreLocation::LocalMachine);
}

BAFX_TEST(identity_signer_arguments_reject_duplicates_and_unknown_store)
{
    constexpr std::array duplicateArguments{
        std::wstring_view(L"--package"),
        std::wstring_view(L"C:\\release\\identity.msix"),
        std::wstring_view(L"--package"),
        std::wstring_view(L"C:\\release\\other.msix"),
        std::wstring_view(L"--thumbprint"),
        std::wstring_view(L"0123456789abcdefABCDEF0123456789abcdefAB")};
    BAFX_CHECK(throwsInvalidArgument([&]
    {
        static_cast<void>(bafx::identity_signer::parseOptions(duplicateArguments));
    }));

    constexpr std::array unknownStoreArguments{
        std::wstring_view(L"--package"),
        std::wstring_view(L"C:\\release\\identity.msix"),
        std::wstring_view(L"--thumbprint"),
        std::wstring_view(L"0123456789abcdefABCDEF0123456789abcdefAB"),
        std::wstring_view(L"--store-location"),
        std::wstring_view(L"CurrentUser")};
    BAFX_CHECK(throwsInvalidArgument([&]
    {
        static_cast<void>(bafx::identity_signer::parseOptions(unknownStoreArguments));
    }));
}

BAFX_TEST(identity_signer_package_validation_requires_absolute_regular_msix)
{
    TemporaryMsix package;
    bafx::identity_signer::validatePackagePath(package.path());

    BAFX_CHECK(throwsInvalidArgument([]
    {
        bafx::identity_signer::validatePackagePath(L"relative.msix");
    }));
    BAFX_CHECK(throwsInvalidArgument([&]
    {
        std::filesystem::path wrongExtension = package.path();
        wrongExtension.replace_extension(L".appx");
        bafx::identity_signer::validatePackagePath(wrongExtension);
    }));
}
