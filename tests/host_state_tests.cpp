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
        R"json({"generation":4,"paused":true,"backgroundCapture":"fallback-fx-only","spout2Enabled":true,"spout2Sender":"ba-click-fx-desktop","spout2Status":"failed","spout2Error":"receiver said \"no\" \\ retry","spout2OutputContract":"bgra8-srgb-premultiplied-fx-only-v1"})json");

    BAFX_CHECK(result.succeeded());
    BAFX_CHECK(result.state->spout2Enabled);
    BAFX_CHECK(result.state->spout2Sender == "ba-click-fx-desktop");
    BAFX_CHECK(result.state->spout2Status == "failed");
    BAFX_CHECK(result.state->spout2Error == "receiver said \"no\" \\ retry");
    BAFX_CHECK(
        result.state->spout2OutputContract
        == "bgra8-srgb-premultiplied-fx-only-v1");
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
