#include "test_support.hpp"
#include "display_state_poller.hpp"

#include <atomic>
#include <chrono>
#include <future>

namespace
{

using namespace std::chrono_literals;
using bafx::control_center::DisplayStatePoller;
using bafx::control_center::DisplayStatePollResult;
using bafx::windows::IpcClientResponse;
using bafx::windows::IpcClientStatus;

IpcClientResponse validResponse()
{
    IpcClientResponse response;
    response.status = IpcClientStatus::Ok;
    response.commandSucceeded = true;
    response.payload = R"({"schemaVersion":4,"runtimeGeneration":7,"configGeneration":11,"appliedConfigGeneration":10,"topologyStatus":"complete","topologyError":0,"offlineOverridesAuthoritative":true,"offlineOverrides":[],"sessions":[]})";
    return response;
}

DisplayStatePollResult awaitResult(DisplayStatePoller& poller)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (auto result = poller.takeResult(); result.has_value())
        {
            return std::move(*result);
        }
        std::this_thread::sleep_for(1ms);
    }
    throw std::runtime_error("display state polling did not complete");
}

}

BAFX_TEST(display_polling_coalesces_requests_and_discards_invalidated_inflight_results)
{
    std::promise<std::thread::id> started;
    auto startedFuture = started.get_future();
    std::promise<void> release;
    const auto releaseFuture = release.get_future();
    std::atomic<unsigned> calls{0U};
    DisplayStatePoller poller([&]
    {
        if (++calls == 1U)
        {
            started.set_value(std::this_thread::get_id());
            // A failed assertion must not strand the worker during destruction.
            static_cast<void>(releaseFuture.wait_for(2s));
        }
        return validResponse();
    });

    BAFX_CHECK(poller.request(10U));
    BAFX_CHECK(startedFuture.wait_for(2s) == std::future_status::ready);
    BAFX_CHECK(startedFuture.get() != std::this_thread::get_id());
    BAFX_CHECK(poller.busy());
    BAFX_CHECK(!poller.request(10U));
    BAFX_CHECK(!poller.takeResult().has_value());

    poller.invalidate();
    BAFX_CHECK(poller.request(11U));
    BAFX_CHECK(!poller.request(11U));
    release.set_value();
    const auto result = awaitResult(poller);
    BAFX_CHECK(result.generation == 11U);
    BAFX_CHECK(result.parsed.succeeded());
    BAFX_CHECK(result.parsed.state->runtimeGeneration == 7U);
    BAFX_CHECK(calls == 2U);
    BAFX_CHECK(!poller.busy());
    BAFX_CHECK(!poller.takeResult().has_value());
}

BAFX_TEST(display_polling_recovers_after_read_errors_and_reports_invalid_snapshots)
{
    unsigned calls{0U};
    DisplayStatePoller poller([&]
    {
        if (++calls == 1U)
        {
            throw std::runtime_error("injected read failure");
        }
        auto response = validResponse();
        if (calls == 2U)
        {
            response.payload = "{}";
        }
        return response;
    });

    BAFX_CHECK(poller.request(1U));
    BAFX_CHECK(awaitResult(poller).response.status == IpcClientStatus::InternalError);
    BAFX_CHECK(poller.request(2U));
    const auto malformed = awaitResult(poller);
    BAFX_CHECK(malformed.response.succeeded());
    BAFX_CHECK(!malformed.parsed.succeeded());
    BAFX_CHECK(poller.request(3U));
    BAFX_CHECK(awaitResult(poller).parsed.succeeded());
}
