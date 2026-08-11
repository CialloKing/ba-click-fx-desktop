#include "test_support.hpp"

#include "package_activation.hpp"

BAFX_TEST(package_activation_state_builds_aumid)
{
    const auto result = bafx::control_center::parsePackageActivationState(
        R"json({"schema":1,"packageFamilyName":"CialloKing.BaClickFxDesktop_abc123","applicationId":"BaClickFxDesktop"})json");

    BAFX_CHECK(result.succeeded());
    BAFX_CHECK(result.installStatePresent);
    BAFX_CHECK(result.identity->appUserModelId
        == L"CialloKing.BaClickFxDesktop_abc123!BaClickFxDesktop");
}

BAFX_TEST(package_activation_state_rejects_wrong_application)
{
    const auto result = bafx::control_center::parsePackageActivationState(
        R"json({"schema":1,"packageFamilyName":"CialloKing.BaClickFxDesktop_abc123","applicationId":"Other"})json");

    BAFX_CHECK(result.installStatePresent);
    BAFX_CHECK(!result.succeeded());
    BAFX_CHECK(!result.error.empty());
}

BAFX_TEST(package_activation_state_rejects_nested_values)
{
    const auto result = bafx::control_center::parsePackageActivationState(
        R"json({"schema":1,"packageFamilyName":{"value":"bad"},"applicationId":"BaClickFxDesktop"})json");

    BAFX_CHECK(result.installStatePresent);
    BAFX_CHECK(!result.succeeded());
}
