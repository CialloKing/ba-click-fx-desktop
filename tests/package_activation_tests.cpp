#include "test_support.hpp"

#include "package_activation.hpp"

#include <objbase.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace
{

class TemporaryInstallDirectory final
{
public:
    TemporaryInstallDirectory()
    {
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid)))
        {
            throw std::runtime_error("Could not create a test directory identifier.");
        }
        wchar_t guidText[39]{};
        if (StringFromGUID2(guid, guidText, 39) == 0)
        {
            throw std::runtime_error("Could not format a test directory identifier.");
        }
        path_ = std::filesystem::temp_directory_path()
            / (L"bafx-package-activation-" + std::wstring(guidText));
        std::filesystem::create_directories(path_ / L"Installer");
    }

    ~TemporaryInstallDirectory()
    {
        // Tests own this GUID-scoped directory, so cleanup cannot touch user data.
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryInstallDirectory(const TemporaryInstallDirectory&) = delete;
    TemporaryInstallDirectory& operator=(const TemporaryInstallDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

    void writeState(const std::string& contents, const bool backup = false) const
    {
        std::ofstream stream(
            path_ / L"Installer"
                / (backup ? L"INSTALL-STATE.json.bak" : L"INSTALL-STATE.json"),
            std::ios::binary);
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream)
        {
            throw std::runtime_error("Could not write the test install state.");
        }
    }

private:
    std::filesystem::path path_{};
};

constexpr std::string_view validInstallState =
    R"json({
  "schema": 2,
  "transactionId": "0123456789abcdef0123456789abcdef",
  "packageName": "CialloKing.BaClickFxDesktop",
  "applicationId": "BaClickFxDesktop",
  "publisher": "CN=BaClickFx.Local",
  "productVersion": "0.1.0-alpha.15",
  "packageVersion": "0.1.0.15",
  "templateSha256": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
  "packageFullName": "CialloKing.BaClickFxDesktop_0.1.0.15_x64__abc123",
  "packageFamilyName": "CialloKing.BaClickFxDesktop_abc123",
  "certificateThumbprint": "1111111111111111111111111111111111111111",
  "certificateSha256": "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB",
  "certificateInstalledBySetup": true,
  "externalLocation": "C:\\Program Files\\BAFX",
  "installedUserSid": "S-1-5-21-1",
  "hostFile": "ba-click-fx-desktop.exe",
  "hostSha256": "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC",
  "packageFile": "identity.msix",
  "packageSha256": "DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD",
  "ownedCertificateThumbprints": "1111111111111111111111111111111111111111",
  "ownedPackageFiles": "identity.msix",
  "installedUtc": "2026-08-16T00:00:00.0000000Z"
})json";

}

BAFX_TEST(package_activation_state_builds_aumid)
{
    const auto result = bafx::control_center::parsePackageActivationState(
        validInstallState);

    BAFX_CHECK(result.succeeded());
    BAFX_CHECK(result.installStatePresent);
    BAFX_CHECK(result.identity->appUserModelId
        == L"CialloKing.BaClickFxDesktop_abc123!BaClickFxDesktop");
}

BAFX_TEST(package_activation_state_rejects_legacy_schema)
{
    const auto result = bafx::control_center::parsePackageActivationState(
        R"json({"schema":1,"packageFamilyName":"CialloKing.BaClickFxDesktop_abc123","applicationId":"BaClickFxDesktop"})json");

    BAFX_CHECK(result.installStatePresent);
    BAFX_CHECK(!result.succeeded());
    BAFX_CHECK(result.error.find(L"expected 2, found 1") != std::wstring::npos);
}

BAFX_TEST(package_activation_state_rejects_wrong_application)
{
    const auto result = bafx::control_center::parsePackageActivationState(
        R"json({"schema":2,"packageFamilyName":"CialloKing.BaClickFxDesktop_abc123","applicationId":"Other"})json");

    BAFX_CHECK(result.installStatePresent);
    BAFX_CHECK(!result.succeeded());
    BAFX_CHECK(!result.error.empty());
}

BAFX_TEST(package_activation_state_rejects_nested_values)
{
    const auto result = bafx::control_center::parsePackageActivationState(
        R"json({"schema":2,"packageFamilyName":{"value":"bad"},"applicationId":"BaClickFxDesktop"})json");

    BAFX_CHECK(result.installStatePresent);
    BAFX_CHECK(!result.succeeded());
}

BAFX_TEST(package_activation_state_reads_complete_file)
{
    TemporaryInstallDirectory directory;
    directory.writeState(std::string(validInstallState));

    const auto result = bafx::control_center::readPackageActivationState(
        directory.path());

    BAFX_CHECK(result.installStatePresent);
    BAFX_CHECK(result.succeeded());
    BAFX_CHECK(result.identity->appUserModelId
        == L"CialloKing.BaClickFxDesktop_abc123!BaClickFxDesktop");
}

BAFX_TEST(package_activation_state_reads_utf8_bom_file)
{
    TemporaryInstallDirectory directory;
    directory.writeState("\xEF\xBB\xBF" + std::string(validInstallState));

    const auto result = bafx::control_center::readPackageActivationState(
        directory.path());

    BAFX_CHECK(result.installStatePresent);
    BAFX_CHECK(result.succeeded());
}

BAFX_TEST(package_activation_state_uses_backup_when_primary_is_corrupt)
{
    TemporaryInstallDirectory directory;
    directory.writeState("{broken");
    directory.writeState(std::string(validInstallState), true);

    const auto result = bafx::control_center::readPackageActivationState(
        directory.path());

    BAFX_CHECK(result.installStatePresent);
    BAFX_CHECK(result.succeeded());
    BAFX_CHECK(result.identity->appUserModelId
        == L"CialloKing.BaClickFxDesktop_abc123!BaClickFxDesktop");
}
