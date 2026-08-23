#include "test_support.hpp"

#include "host_state.hpp"

#include <string>

using namespace bafx::control_center;

BAFX_TEST(host_state_requires_spout2_runtime_group)
{
    const HostStateParseResult result = parseHostState(
        R"json({"generation":3,"paused":false,"backgroundCapture":"active"})json");

    BAFX_CHECK(!result.succeeded());
    BAFX_CHECK(result.error.find("required Spout2") != std::string::npos);
}

BAFX_TEST(host_state_parses_complete_spout2_runtime_group)
{
    const HostStateParseResult result = parseHostState(
        R"json({"generation":4,"paused":true,"backgroundCapture":"fallback-fx-only","spout2Enabled":true,"spout2Sender":"ba-click-fx-desktop","spout2Status":"failed","spout2Error":"receiver said \"no\" \\ retry","spout2OutputContract":"bgra8-sdr-rolloff-extended-premultiplied-fx-only-v5","fxProfileCatalog":"B:Unity 原版|B:轻量|C:我的预设","activeFxProfile":"我的预设","fxProfileWarning":""})json");

    BAFX_CHECK(result.succeeded());
    BAFX_CHECK(result.state->spout2Enabled);
    BAFX_CHECK(result.state->spout2Sender == "ba-click-fx-desktop");
    BAFX_CHECK(result.state->spout2Status == "failed");
    BAFX_CHECK(result.state->spout2Error == "receiver said \"no\" \\ retry");
    BAFX_CHECK(
        result.state->spout2OutputContract
        == "bgra8-sdr-rolloff-extended-premultiplied-fx-only-v5");
    BAFX_CHECK(result.state->fxProfiles.size() == 3U);
    BAFX_CHECK(result.state->fxProfiles[0].builtIn);
    BAFX_CHECK(!result.state->fxProfiles[2].builtIn);
    BAFX_CHECK(result.state->fxProfiles[2].name == "我的预设");
    BAFX_CHECK(result.state->activeFxProfile == "我的预设");

    const HostStateParseResult normalizedCase = parseHostState(
        R"json({"generation":5,"paused":false,"backgroundCapture":"active","spout2Enabled":false,"spout2Sender":"ba-click-fx-desktop","spout2Status":"disabled","spout2Error":"","spout2OutputContract":"v5","fxProfileCatalog":"B:Unity 原版|C:Foo","activeFxProfile":"fOO","fxProfileWarning":""})json");
    BAFX_CHECK(normalizedCase.succeeded());
    BAFX_CHECK(normalizedCase.state->activeFxProfile == "Foo");
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
        R"json({"generation":1,"paused":false,"backgroundCapture":"active","spout2Enabled":false,"spout2Sender":"ba-click-fx-desktop","spout2Status":"disabled","spout2Error":"","spout2OutputContract":"v5"})json");
    BAFX_CHECK(!missing.succeeded());
    BAFX_CHECK(missing.error.find("FX profile") != std::string::npos);

    const HostStateParseResult duplicateProfile = parseHostState(
        R"json({"generation":1,"paused":false,"backgroundCapture":"active","spout2Enabled":false,"spout2Sender":"ba-click-fx-desktop","spout2Status":"disabled","spout2Error":"","spout2OutputContract":"v5","fxProfileCatalog":"B:轻量|C:轻量","activeFxProfile":"轻量","fxProfileWarning":""})json");
    BAFX_CHECK(!duplicateProfile.succeeded());
    BAFX_CHECK(duplicateProfile.error.find("fxProfileCatalog")
        != std::string::npos);

    const HostStateParseResult caseDuplicate = parseHostState(
        R"json({"generation":1,"paused":false,"backgroundCapture":"active","spout2Enabled":false,"spout2Sender":"ba-click-fx-desktop","spout2Status":"disabled","spout2Error":"","spout2OutputContract":"v5","fxProfileCatalog":"C:Foo|C:foo","activeFxProfile":"Foo","fxProfileWarning":""})json");
    BAFX_CHECK(!caseDuplicate.succeeded());
    BAFX_CHECK(caseDuplicate.error.find("fxProfileCatalog")
        != std::string::npos);

    const HostStateParseResult trailingDelimiter = parseHostState(
        R"json({"generation":1,"paused":false,"backgroundCapture":"active","spout2Enabled":false,"spout2Sender":"ba-click-fx-desktop","spout2Status":"disabled","spout2Error":"","spout2OutputContract":"v5","fxProfileCatalog":"B:Unity 原版|","activeFxProfile":"Unity 原版","fxProfileWarning":""})json");
    BAFX_CHECK(!trailingDelimiter.succeeded());
    BAFX_CHECK(trailingDelimiter.error.find("fxProfileCatalog")
        != std::string::npos);

    const HostStateParseResult unknownActive = parseHostState(
        R"json({"generation":1,"paused":false,"backgroundCapture":"active","spout2Enabled":false,"spout2Sender":"ba-click-fx-desktop","spout2Status":"disabled","spout2Error":"","spout2OutputContract":"v5","fxProfileCatalog":"B:Unity 原版|C:我的预设","activeFxProfile":"不存在","fxProfileWarning":""})json");
    BAFX_CHECK(!unknownActive.succeeded());
    BAFX_CHECK(unknownActive.error.find("activeFxProfile")
        != std::string::npos);
}
