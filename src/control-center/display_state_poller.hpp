#pragma once

#include "display_state.hpp"
#include "bafx/windows/ipc_client.hpp"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

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
class DisplayStatePoller final
{
public:
    using Read = std::function<bafx::windows::IpcClientResponse()>;

    explicit DisplayStatePoller(bafx::windows::IpcClientOptions options);
    explicit DisplayStatePoller(Read read);
    ~DisplayStatePoller();

    DisplayStatePoller(const DisplayStatePoller&) = delete;
    DisplayStatePoller& operator=(const DisplayStatePoller&) = delete;

    [[nodiscard]] bool request(std::uint64_t generation);
    [[nodiscard]] std::optional<DisplayStatePollResult> takeResult();
    [[nodiscard]] bool busy() const;
    void invalidate();

private:
    struct Request final
    {
        std::uint64_t epoch;
        std::uint64_t generation;
    };

    void run(std::stop_token stop);

    Read read_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::uint64_t epoch_{0U};
    std::optional<Request> requested_;
    std::optional<std::uint64_t> runningEpoch_;
    std::optional<DisplayStatePollResult> completed_;
    // Destroy/join the worker before destroying anything it can access.
    std::jthread worker_;
};

}
