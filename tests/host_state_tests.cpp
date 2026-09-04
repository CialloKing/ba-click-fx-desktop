#include "test_support.hpp"

#include "host_state.hpp"

#include "product/version.hpp"

#include <string>
#include <string_view>

using namespace bafx::control_center;

namespace
{

[[nodiscard]] HostStateParseResult parseWithHotkeyFields(
    const std::string_view fields)
{
    return parseHostState(
        R"json({"generation":1,"paused":false,"backgroundCapture":"active","spout2Enabled":false,"spout2Sender":"ba-click-fx-desktop","spout2Status":"disabled","spout2Error":"","spout2OutputContract":"v6","fxProfileCatalog":"B:Unity 原版","activeFxProfile":"Unity 原版","fxProfileWarning":"",)json"
        + std::string(fields) + "}");
}

}

BAFX_TEST(host_state_requires_spout2_runtime_group)
{
    const HostStateParseResult result = parseHostState(
        R"json({"generation":3,"paused":false,"backgroundCapture":"active"})json");

    BAFX_CHECK(!result.succeeded());
    BAFX_CHECK(result.error.find("required Spout2") != std::string::npos);
}

BAFX_TEST(host_state_parses_complete_spout2_runtime_group)
{
    const std::string json = "{\"productVersion\":\""
        + std::string(bafx::product::version)
        + R"json(","generation":4,"paused":true,"backgroundCapture":"fallback-fx-only","spout2Enabled":true,"spout2Sender":"ba-click-fx-desktop","spout2Status":"failed","spout2Error":"receiver said \"no\" \\ retry","spout2OutputContract":"bgra8-srgb-extended-premultiplied-fx-only-v6","fxProfileCatalog":"B:Unity 原版|B:轻量|C:我的预设","activeFxProfile":"我的预设","fxProfileWarning":""})json";
    const HostStateParseResult result = parseHostState(json);

    BAFX_CHECK(result.succeeded());
    BAFX_CHECK(result.state->settingsCompatible());
    BAFX_CHECK(
        result.state->productVersionStatus
        == HostProductVersionStatus::Match);
    BAFX_CHECK(result.state->spout2Enabled);
    BAFX_CHECK(result.state->spout2Sender == "ba-click-fx-desktop");
    BAFX_CHECK(result.state->spout2Status == "failed");
    BAFX_CHECK(result.state->spout2Error == "receiver said \"no\" \\ retry");
    BAFX_CHECK(
        result.state->spout2OutputContract
        == "bgra8-srgb-extended-premultiplied-fx-only-v6");
    BAFX_CHECK(result.state->fxProfiles.size() == 3U);
    BAFX_CHECK(result.state->fxProfiles[0].builtIn);
    BAFX_CHECK(!result.state->fxProfiles[2].builtIn);
    BAFX_CHECK(result.state->fxProfiles[2].name == "我的预设");
    BAFX_CHECK(result.state->activeFxProfile == "我的预设");

    const HostStateParseResult normalizedCase = parseHostState(
        R"json({"generation":5,"paused":false,"backgroundCapture":"active","spout2Enabled":false,"spout2Sender":"ba-click-fx-desktop","spout2Status":"disabled","spout2Error":"","spout2OutputContract":"v6","fxProfileCatalog":"B:Unity 原版|C:Foo","activeFxProfile":"fOO","fxProfileWarning":""})json");
    BAFX_CHECK(normalizedCase.succeeded());
    BAFX_CHECK(
        normalizedCase.state->productVersionStatus
        == HostProductVersionStatus::Missing);
    BAFX_CHECK(!normalizedCase.state->settingsCompatible());
    BAFX_CHECK(normalizedCase.state->activeFxProfile == "Foo");
}

BAFX_TEST(host_state_classifies_invalid_and_mismatched_product_versions)
{
    const HostStateParseResult malformed = parseHostState(
        R"json({"productVersion":"0.2","generation":1,"paused":false,"backgroundCapture":"active","spout2Enabled":false,"spout2Sender":"ba-click-fx-desktop","spout2Status":"disabled","spout2Error":"","spout2OutputContract":"v6","fxProfileCatalog":"B:Unity 原版","activeFxProfile":"Unity 原版","fxProfileWarning":""})json");
    BAFX_CHECK(malformed.succeeded());
    BAFX_CHECK(
        malformed.state->productVersionStatus
        == HostProductVersionStatus::Invalid);
    BAFX_CHECK(!malformed.state->settingsCompatible());

    const HostStateParseResult wrongType = parseHostState(
        R"json({"productVersion":205,"generation":1,"paused":false,"backgroundCapture":"active","spout2Enabled":false,"spout2Sender":"ba-click-fx-desktop","spout2Status":"disabled","spout2Error":"","spout2OutputContract":"v6","fxProfileCatalog":"B:Unity 原版","activeFxProfile":"Unity 原版","fxProfileWarning":""})json");
    BAFX_CHECK(wrongType.succeeded());
    BAFX_CHECK(
        wrongType.state->productVersionStatus
        == HostProductVersionStatus::Invalid);

    const HostStateParseResult mismatch = parseHostState(
        R"json({"productVersion":"65535.65535.65535","generation":1,"paused":false,"backgroundCapture":"active","spout2Enabled":false,"spout2Sender":"ba-click-fx-desktop","spout2Status":"disabled","spout2Error":"","spout2OutputContract":"v6","fxProfileCatalog":"B:Unity 原版","activeFxProfile":"Unity 原版","fxProfileWarning":""})json");
    BAFX_CHECK(mismatch.succeeded());
    BAFX_CHECK(
        mismatch.state->productVersionStatus
        == HostProductVersionStatus::Mismatch);
    BAFX_CHECK(!mismatch.state->settingsCompatible());
}

BAFX_TEST(host_state_rejects_truncated_product_version_without_throwing)
{
    const HostStateParseResult result = parseHostState(
        R"json({"productVersion":)json");

    BAFX_CHECK(!result.succeeded());
    BAFX_CHECK(!result.error.empty());
}

BAFX_TEST(host_state_rejects_partial_or_duplicate_spout2_groups)
{
    const HostStateParseResult partial = parseHostState(
        R"json({"generation":1,"paused":false,"backgroundCapture":"active","spout2Enabled":true})json");
    BAFX_CHECK(!partial.succeeded());
    BAFX_CHECK(
        partial.error.find("required Spout2") != std::string::npos);

    const HostStateParseResult duplicate = parseHostState(
        R"json({"generation":1,"paused":false,"backgroundCapture":"active","spout2Enabled":true,"spout2Enabled":false,"spout2Sender":"x","spout2Status":"sent","spout2Error":"","spout2OutputContract":"v1"})json");
    BAFX_CHECK(!duplicate.succeeded());
    BAFX_CHECK(
        duplicate.error.find("spout2Enabled") != std::string::npos);
}

BAFX_TEST(host_state_rejects_invalid_or_missing_fx_profile_group)
{
    const HostStateParseResult missing = parseHostState(
        R"json({"generation":1,"paused":false,"backgroundCapture":"active","spout2Enabled":false,"spout2Sender":"ba-click-fx-desktop","spout2Status":"disabled","spout2Error":"","spout2OutputContract":"v6"})json");
    BAFX_CHECK(!missing.succeeded());
    BAFX_CHECK(missing.error.find("FX profile") != std::string::npos);

    const HostStateParseResult duplicateProfile = parseHostState(
        R"json({"generation":1,"paused":false,"backgroundCapture":"active","spout2Enabled":false,"spout2Sender":"ba-click-fx-desktop","spout2Status":"disabled","spout2Error":"","spout2OutputContract":"v6","fxProfileCatalog":"B:轻量|C:轻量","activeFxProfile":"轻量","fxProfileWarning":""})json");
    BAFX_CHECK(!duplicateProfile.succeeded());
    BAFX_CHECK(duplicateProfile.error.find("fxProfileCatalog")
        != std::string::npos);

    const HostStateParseResult caseDuplicate = parseHostState(
        R"json({"generation":1,"paused":false,"backgroundCapture":"active","spout2Enabled":false,"spout2Sender":"ba-click-fx-desktop","spout2Status":"disabled","spout2Error":"","spout2OutputContract":"v6","fxProfileCatalog":"C:Foo|C:foo","activeFxProfile":"Foo","fxProfileWarning":""})json");
    BAFX_CHECK(!caseDuplicate.succeeded());
    BAFX_CHECK(caseDuplicate.error.find("fxProfileCatalog")
        != std::string::npos);

    const HostStateParseResult trailingDelimiter = parseHostState(
        R"json({"generation":1,"paused":false,"backgroundCapture":"active","spout2Enabled":false,"spout2Sender":"ba-click-fx-desktop","spout2Status":"disabled","spout2Error":"","spout2OutputContract":"v6","fxProfileCatalog":"B:Unity 原版|","activeFxProfile":"Unity 原版","fxProfileWarning":""})json");
    BAFX_CHECK(!trailingDelimiter.succeeded());
    BAFX_CHECK(trailingDelimiter.error.find("fxProfileCatalog")
        != std::string::npos);

    const HostStateParseResult unknownActive = parseHostState(
        R"json({"generation":1,"paused":false,"backgroundCapture":"active","spout2Enabled":false,"spout2Sender":"ba-click-fx-desktop","spout2Status":"disabled","spout2Error":"","spout2OutputContract":"v6","fxProfileCatalog":"B:Unity 原版|C:我的预设","activeFxProfile":"不存在","fxProfileWarning":""})json");
    BAFX_CHECK(!unknownActive.succeeded());
    BAFX_CHECK(unknownActive.error.find("activeFxProfile")
        != std::string::npos);
}

BAFX_TEST(host_state_parses_complete_hotkey_state_group)
{
    const HostStateParseResult result = parseWithHotkeyFields(
        R"json("hotkeysJson":"{\"togglePause\":null,\"toggleAlwaysOnTrail\":null,\"nextFxProfile\":null,\"shutdown\":null}","hotkeyRegisteredMask":5,"hotkeyCleanupError":0,"hotkeyCaptureToken":42,"hotkeyCaptureKey":75,"hotkeyCaptureModifiers":3,"hotkeyError0":0,"hotkeyError1":1409,"hotkeyError2":0,"hotkeyError3":0,"hotkeyActionError":"")json");

    BAFX_CHECK(result.succeeded());
    BAFX_CHECK(result.state->hotkeysJson.has_value());
    BAFX_CHECK(result.state->hotkeyRegisteredMask == 5U);
    BAFX_CHECK(result.state->hotkeyCaptureToken == 42U);
    BAFX_CHECK(result.state->hotkeyCaptureKey == 75U);
    BAFX_CHECK(result.state->hotkeyCaptureModifiers == 3U);
    BAFX_CHECK(result.state->hotkeyErrors[1] == 1409U);
    BAFX_CHECK(result.state->hotkeyActionError.empty());
}

BAFX_TEST(host_state_rejects_incomplete_duplicate_or_out_of_range_hotkey_state)
{
    constexpr std::string_view prefix =
        R"json("hotkeysJson":"{}","hotkeyRegisteredMask":0,"hotkeyCleanupError":0,"hotkeyCaptureToken":0,"hotkeyCaptureKey":0,"hotkeyCaptureModifiers":0,)json";
    constexpr std::string_view suffix =
        R"json("hotkeyError0":0,"hotkeyError1":0,"hotkeyError2":0,"hotkeyError3":0,"hotkeyActionError":"")json";

    const HostStateParseResult incomplete = parseWithHotkeyFields(
        std::string(prefix) +
        R"json("hotkeyError0":0,"hotkeyError1":0,"hotkeyError2":0,"hotkeyError3":0)json");
    BAFX_CHECK(!incomplete.succeeded());
    BAFX_CHECK(incomplete.error.find("hotkey state group") != std::string::npos);

    const HostStateParseResult duplicate = parseWithHotkeyFields(
        std::string(prefix) + R"json("hotkeyError0":0,)json" + std::string(suffix));
    BAFX_CHECK(!duplicate.succeeded());
    BAFX_CHECK(duplicate.error.find("duplicate hotkey") != std::string::npos);

    for (const std::string_view outOfRange : {
             R"json("hotkeysJson":"{}","hotkeyRegisteredMask":16,"hotkeyCleanupError":0,"hotkeyCaptureToken":0,"hotkeyCaptureKey":0,"hotkeyCaptureModifiers":0,)json",
             R"json("hotkeysJson":"{}","hotkeyRegisteredMask":0,"hotkeyCleanupError":0,"hotkeyCaptureToken":0,"hotkeyCaptureKey":255,"hotkeyCaptureModifiers":0,)json",
             R"json("hotkeysJson":"{}","hotkeyRegisteredMask":0,"hotkeyCleanupError":0,"hotkeyCaptureToken":0,"hotkeyCaptureKey":0,"hotkeyCaptureModifiers":16,)json"})
    {
        const HostStateParseResult result = parseWithHotkeyFields(
            std::string(outOfRange) + std::string(suffix));
        BAFX_CHECK(!result.succeeded());
        BAFX_CHECK(result.error.find("hotkey state group") != std::string::npos);
    }
}
