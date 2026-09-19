#include "host_snapshot_poller.hpp"
#include "control_center_diagnostics.hpp"

namespace bafx::control_center
{

HostSnapshotResult readHostSnapshot(const HostSnapshotRead& read)
{
    for (unsigned attempt = 0U; attempt < 2U; ++attempt)
    {
        HostSnapshotResult result;
        result.response = read("GetState");
        if (!result.response.succeeded())
        {
            return result;
        }
        auto state = parseHostState(result.response.payload);
        if (!state.succeeded())
        {
            result.status = HostSnapshotStatus::StateInvalid;
            result.error = std::move(state.error);
            return result;
        }
        result.state = std::move(state.state);
        if (!result.state->settingsCompatible())
        {
            result.status = HostSnapshotStatus::Incompatible;
            return result;
        }
        const auto generation = result.state->generation;
        result.response = read("GetConfig");
        if (!result.response.succeeded())
        {
            result.status = HostSnapshotStatus::ConfigReadFailed;
            return result;
        }
        auto config = bafx::config::parseJson(result.response.payload);
        if (!config.succeeded())
        {
            result.status = HostSnapshotStatus::ConfigInvalid;
            result.error = std::move(config.message);
            return result;
        }
        result.config = std::move(config.config);
        result.response = read("GetState");
        if (!result.response.succeeded())
        {
            result.status = HostSnapshotStatus::RecheckReadFailed;
            return result;
        }
        state = parseHostState(result.response.payload);
        if (!state.succeeded())
        {
            result.status = HostSnapshotStatus::RecheckInvalid;
            result.error = std::move(state.error);
            return result;
        }
        result.state = std::move(state.state);
        if (!result.state->settingsCompatible())
        {
            result.status = HostSnapshotStatus::Incompatible;
            return result;
        }
        if (result.state->generation == generation)
        {
            result.status = HostSnapshotStatus::Succeeded;
            return result;
        }
        if (attempt == 1U)
        {
            result.status = HostSnapshotStatus::GenerationChanged;
            return result;
        }
    }
    return {};
}

HostSnapshotPoller::HostSnapshotPoller(bafx::windows::IpcClientOptions options)
    : HostSnapshotPoller([client = DiagnosticIpcClient(std::move(options))](const std::string_view command)
    {
        return client.transact(command);
    })
{
}

HostSnapshotPoller::HostSnapshotPoller(HostSnapshotRead read)
    : SnapshotPoller([read = std::move(read)]
    {
        return readHostSnapshot(read);
    })
{
}

}
