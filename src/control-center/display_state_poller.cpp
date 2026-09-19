#include "display_state_poller.hpp"
#include "control_center_diagnostics.hpp"

#include <utility>

namespace bafx::control_center
{

DisplayStatePoller::DisplayStatePoller(bafx::windows::IpcClientOptions options)
    : DisplayStatePoller([client = DiagnosticIpcClient(std::move(options))]()
    {
        return client.transact("GetDisplayState");
    })
{
}

DisplayStatePoller::DisplayStatePoller(Read read)
    : SnapshotPoller([read = std::move(read)]
    {
        DisplayStatePollResult result;
        result.response = read();
        if (result.response.succeeded())
        {
            result.parsed = parseDisplayState(result.response.payload);
        }
        return result;
    })
{
}

}
