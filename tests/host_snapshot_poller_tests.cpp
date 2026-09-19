#include "test_support.hpp"
#include "host_snapshot_poller.hpp"
#include "product/version.hpp"

#include <vector>

namespace
{
using namespace bafx::control_center;
using bafx::windows::IpcClientResponse;

IpcClientResponse response(std::string payload)
{
    IpcClientResponse result;
    result.status = bafx::windows::IpcClientStatus::Ok;
    result.commandSucceeded = true;
    result.payload = std::move(payload);
    return result;
}

IpcClientResponse state(const unsigned generation,
    const std::string_view version = bafx::product::version)
{
    return response("{\"productVersion\":\"" + std::string(version)
        + "\",\"generation\":" + std::to_string(generation)
        + R"(,"paused":false,"backgroundCapture":"active","spout2Enabled":false,"spout2Sender":"BAFX","spout2Status":"disabled","spout2Error":"","spout2OutputContract":"v6","fxProfileCatalog":"B:Unity 原版","activeFxProfile":"Unity 原版","fxProfileWarning":""})");
}

HostSnapshotResult readSequence(const std::vector<IpcClientResponse>& responses)
{
    std::size_t index = 0U;
    const auto result = readHostSnapshot([&](const std::string_view command)
    {
        BAFX_CHECK(command == (index % 3U == 1U ? "GetConfig" : "GetState"));
        BAFX_CHECK(index < responses.size());
        return responses[index++];
    });
    BAFX_CHECK(index == responses.size());
    return result;
}
}

BAFX_TEST(host_snapshot_retries_torn_config_and_publishes_only_confirmed_values)
{
    bafx::config::Config oldConfig;
    auto newConfig = oldConfig;
    newConfig.effects.opacity = 0.5F;
    const auto result = readSequence({state(1U), response(bafx::config::toJson(oldConfig)), state(2U),
        state(2U), response(bafx::config::toJson(newConfig)), state(2U)});
    BAFX_CHECK(result.status == HostSnapshotStatus::Succeeded);
    BAFX_CHECK(result.state->generation == 2U);
    BAFX_CHECK(result.config.effects.opacity == 0.5F);

    const auto changing = readSequence({state(1U), response(bafx::config::toJson(oldConfig)), state(2U),
        state(2U), response(bafx::config::toJson(newConfig)), state(3U)});
    BAFX_CHECK(changing.status == HostSnapshotStatus::GenerationChanged);
}

BAFX_TEST(host_snapshot_gates_config_reads_and_rechecks_host_compatibility)
{
    BAFX_CHECK(readSequence({state(1U, "65535.0.0")}).status == HostSnapshotStatus::Incompatible);
    BAFX_CHECK(readSequence({state(1U), response(bafx::config::toJson(bafx::config::Config{})),
        state(1U, "65535.0.0")}).status == HostSnapshotStatus::Incompatible);
}

BAFX_TEST(host_snapshot_preserves_read_failure_stage)
{
    const auto config = response(bafx::config::toJson(bafx::config::Config{}));
    BAFX_CHECK(readSequence({{}}).status == HostSnapshotStatus::StateReadFailed);
    BAFX_CHECK(readSequence({response("{}")}).status == HostSnapshotStatus::StateInvalid);
    BAFX_CHECK(readSequence({state(1U), {}}).status == HostSnapshotStatus::ConfigReadFailed);
    BAFX_CHECK(readSequence({state(1U), response("invalid")}).status == HostSnapshotStatus::ConfigInvalid);
    BAFX_CHECK(readSequence({state(1U), config, {}}).status == HostSnapshotStatus::RecheckReadFailed);
    BAFX_CHECK(readSequence({state(1U), config, response("{}")}).status == HostSnapshotStatus::RecheckInvalid);
}
