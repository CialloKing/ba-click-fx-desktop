#pragma once

#include "host_state.hpp"
#include "snapshot_poller.hpp"
#include "bafx/config/config.hpp"
#include "bafx/windows/ipc_client.hpp"

namespace bafx::control_center
{

enum class HostSnapshotStatus
{
    StateReadFailed,
    StateInvalid,
    Incompatible,
    ConfigReadFailed,
    ConfigInvalid,
    RecheckReadFailed,
    RecheckInvalid,
    GenerationChanged,
    Succeeded
};

struct HostSnapshotResult final
{
    std::uint64_t generation{0U};
    HostSnapshotStatus status{HostSnapshotStatus::StateReadFailed};
    bafx::windows::IpcClientResponse response{};
    std::optional<HostState> state{};
    bafx::config::Config config{};
    std::string error{};
};

using HostSnapshotRead = std::function<bafx::windows::IpcClientResponse(std::string_view)>;

// Read a coherent state/config pair, with one bounded retry for a generation
// change. Compatibility must pass before configuration is read or published.
[[nodiscard]] HostSnapshotResult readHostSnapshot(const HostSnapshotRead& read);

class HostSnapshotPoller final : public SnapshotPoller<HostSnapshotResult>
{
public:
    explicit HostSnapshotPoller(bafx::windows::IpcClientOptions options);
    explicit HostSnapshotPoller(HostSnapshotRead read);
};

}
