#pragma once

#include "display_state.hpp"
#include "snapshot_poller.hpp"
#include "bafx/windows/ipc_client.hpp"

namespace bafx::control_center
{

struct DisplayStatePollResult final
{
    std::uint64_t generation{0U};
    bafx::windows::IpcClientResponse response{};
    DisplayStateParseResult parsed{};
};

// One worker owns its IPC client and parser. The UI only transfers requests and
// completed values; it never holds this mutex while waiting on the Host.
class DisplayStatePoller final : public SnapshotPoller<DisplayStatePollResult>
{
public:
    using Read = std::function<bafx::windows::IpcClientResponse()>;

    explicit DisplayStatePoller(bafx::windows::IpcClientOptions options);
    explicit DisplayStatePoller(Read read);
};

}
