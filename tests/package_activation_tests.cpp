#include "test_support.hpp"

#include "package_activation.hpp"

#include "product/version.hpp"

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

constexpr std::string_view validInstallStateTemplate =
    R"json({
  "schema": 2,
  "transactionId": "0123456789abcdef0123456789abcdef",
  "packageName": "CialloKing.BaClickFxDesktop",
  "applicationId": "BaClickFxDesktop",
  "publisher": "CN=BaClickFx.Local",
  "productVersion": "@PRODUCT_VERSION@",
  "packageVersion": "@PACKAGE_VERSION@",
  "templateSha256": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
  "packageFullName": "CialloKing.BaClickFxDesktop_0.2.5.0_x64__abc123",
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

[[nodiscard]] std::string makeInstallState(
    const std::string_view productVersion = bafx::product::version,
    const std::string_view packageVersion = {})
{
    std::string state(validInstallStateTemplate);
    const auto replaceToken = [&state](
                                  const std::string_view token,
                                  const std::string_view value)
    {
        const std::size_t position = state.find(token);
        if (position == std::string::npos)
        {
            throw std::runtime_error("Install state test token is missing.");
        }
        state.replace(position, token.size(), value);
    };
    replaceToken("@PRODUCT_VERSION@", productVersion);
    if (packageVersion.empty())
    {
        replaceToken(
            "@PACKAGE_VERSION@",
            std::string(productVersion) + ".0");
    }
    else
    {
        replaceToken("@PACKAGE_VERSION@", packageVersion);
    }
    return state;
}

}

BAFX_TEST(package_activation_state_builds_aumid)
{
    const auto result = bafx::control_center::parsePackageActivationState(
        makeInstallState());

    BAFX_CHECK(result.succeeded());
    BAFX_CHECK(result.installStatePresent);
    BAFX_CHECK(
        result.status
        == bafx::control_center::PackageActivationStateStatus::Valid);
    BAFX_CHECK(result.identity->appUserModelId
        == L"CialloKing.BaClickFxDesktop_abc123!BaClickFxDesktop");
    BAFX_CHECK(result.identity->productVersion == bafx::product::version);
    BAFX_CHECK(
        result.identity->packageVersion
        == std::string(bafx::product::version) + ".0");
}

BAFX_TEST(package_activation_state_rejects_legacy_schema)
{
    const auto result = bafx::control_center::parsePackageActivationState(
        R"json({"schema":1,"packageFamilyName":"CialloKing.BaClickFxDesktop_abc123","applicationId":"BaClickFxDesktop"})json");

    BAFX_CHECK(result.installStatePresent);
    BAFX_CHECK(!result.succeeded());
    BAFX_CHECK(
        result.status
        == bafx::control_center::PackageActivationStateStatus::Corrupt);
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

BAFX_TEST(package_activation_state_requires_strict_coherent_versions)
{
    const auto malformed =
        bafx::control_center::parsePackageActivationState(
            makeInstallState("0.02.5", "0.2.5.0"));
    BAFX_CHECK(!malformed.succeeded());
    BAFX_CHECK(
        malformed.status
        == bafx::control_center::PackageActivationStateStatus::Corrupt);

    const auto mismatch =
        bafx::control_center::parsePackageActivationState(
            makeInstallState(
                bafx::product::version,
                "65535.65535.65535.0"));
    BAFX_CHECK(!mismatch.succeeded());
    BAFX_CHECK(
        mismatch.status
        == bafx::control_center::PackageActivationStateStatus::VersionMismatch);
}

BAFX_TEST(package_activation_state_classifies_partial_upgrade)
{
    const auto result = bafx::control_center::parsePackageActivationState(
        makeInstallState(
            "65535.65535.65535",
            "65535.65535.65535.0"));

    BAFX_CHECK(result.installStatePresent);
    BAFX_CHECK(!result.succeeded());
    BAFX_CHECK(
        result.status
        == bafx::control_center::PackageActivationStateStatus::PartialUpgrade);
    BAFX_CHECK(result.identity.has_value());
}

BAFX_TEST(package_activation_state_reads_complete_file)
{
    TemporaryInstallDirectory directory;
    directory.writeState(makeInstallState());

    const auto result = bafx::control_center::readPackageActivationState(
        directory.path());

    BAFX_CHECK(result.installStatePresent);
    BAFX_CHECK(result.succeeded());
    BAFX_CHECK(
        result.status
        == bafx::control_center::PackageActivationStateStatus::Valid);
    BAFX_CHECK(
        result.source
        == bafx::control_center::PackageActivationStateSource::Primary);
    BAFX_CHECK(result.identity->appUserModelId
        == L"CialloKing.BaClickFxDesktop_abc123!BaClickFxDesktop");
}

BAFX_TEST(package_activation_state_classifies_missing_files)
{
    TemporaryInstallDirectory directory;

    const auto result = bafx::control_center::readPackageActivationState(
        directory.path());

    BAFX_CHECK(!result.installStatePresent);
    BAFX_CHECK(!result.succeeded());
    BAFX_CHECK(
        result.status
        == bafx::control_center::PackageActivationStateStatus::Missing);
    BAFX_CHECK(
        result.source
        == bafx::control_center::PackageActivationStateSource::None);
}

BAFX_TEST(package_activation_state_reads_utf8_bom_file)
{
    TemporaryInstallDirectory directory;
    directory.writeState("\xEF\xBB\xBF" + makeInstallState());

    const auto result = bafx::control_center::readPackageActivationState(
        directory.path());

    BAFX_CHECK(result.installStatePresent);
    BAFX_CHECK(result.succeeded());
}

BAFX_TEST(package_activation_state_uses_backup_when_primary_is_corrupt)
{
    TemporaryInstallDirectory directory;
    directory.writeState("{broken");
    directory.writeState(makeInstallState(), true);

    const auto result = bafx::control_center::readPackageActivationState(
        directory.path());

    BAFX_CHECK(result.installStatePresent);
    BAFX_CHECK(result.succeeded());
    BAFX_CHECK(
        result.status
        == bafx::control_center::PackageActivationStateStatus::BackupRecovered);
    BAFX_CHECK(result.recoveredFromBackup());
    BAFX_CHECK(result.identity->appUserModelId
        == L"CialloKing.BaClickFxDesktop_abc123!BaClickFxDesktop");
}

BAFX_TEST(package_activation_state_does_not_mask_primary_partial_upgrade)
{
    TemporaryInstallDirectory directory;
    directory.writeState(makeInstallState(
        "65535.65535.65535",
        "65535.65535.65535.0"));
    directory.writeState(makeInstallState(), true);

    const auto result = bafx::control_center::readPackageActivationState(
        directory.path());

    BAFX_CHECK(!result.succeeded());
    BAFX_CHECK(
        result.status
        == bafx::control_center::PackageActivationStateStatus::PartialUpgrade);
    BAFX_CHECK(
        result.source
        == bafx::control_center::PackageActivationStateSource::Primary);
}

BAFX_TEST(package_activation_state_classifies_missing_primary_with_backup)
{
    TemporaryInstallDirectory directory;
    directory.writeState(makeInstallState(), true);

    const auto result = bafx::control_center::readPackageActivationState(
        directory.path());

    BAFX_CHECK(result.installStatePresent);
    BAFX_CHECK(!result.succeeded());
    BAFX_CHECK(
        result.status
        == bafx::control_center::PackageActivationStateStatus::PartialUpgrade);
    BAFX_CHECK(
        result.source
        == bafx::control_center::PackageActivationStateSource::Backup);
}

BAFX_TEST(package_activation_state_reports_both_corrupt_files)
{
    TemporaryInstallDirectory directory;
    directory.writeState("{broken");
    directory.writeState("[]", true);

    const auto result = bafx::control_center::readPackageActivationState(
        directory.path());

    BAFX_CHECK(result.installStatePresent);
    BAFX_CHECK(!result.succeeded());
    BAFX_CHECK(
        result.status
        == bafx::control_center::PackageActivationStateStatus::Corrupt);
    BAFX_CHECK(result.error.find(L"Backup state is also invalid")
        != std::wstring::npos);
}
