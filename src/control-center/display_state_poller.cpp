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
    : read_(std::move(read)), worker_([this](const std::stop_token stop)
    {
        run(stop);
    })
{
}

DisplayStatePoller::~DisplayStatePoller()
{
    worker_.request_stop();
    wake_.notify_all();
}

bool DisplayStatePoller::request(const std::uint64_t generation)
{
    const std::lock_guard lock(mutex_);
    if (requested_.has_value() || completed_.has_value() || runningEpoch_ == epoch_)
    {
        return false;
    }
    requested_ = Request{epoch_, generation};
    wake_.notify_one();
    return true;
}

void DisplayStatePoller::invalidate()
{
    const std::lock_guard lock(mutex_);
    ++epoch_;
    requested_.reset();
    completed_.reset();
}

std::optional<DisplayStatePollResult> DisplayStatePoller::takeResult()
{
    const std::lock_guard lock(mutex_);
    return std::exchange(completed_, std::nullopt);
}

bool DisplayStatePoller::busy() const
{
    const std::lock_guard lock(mutex_);
    // Keep completion polling alive until the UI has consumed the value, even
    // if the worker finishes between takeResult() and this check.
    return requested_.has_value() || runningEpoch_.has_value() || completed_.has_value();
}

void DisplayStatePoller::run(const std::stop_token stop)
{
    for (;;)
    {
        Request request{};
        {
            std::unique_lock lock(mutex_);
            if (!wake_.wait(lock, stop, [this]
                {
                    return requested_.has_value();
                })
                || stop.stop_requested())
            {
                return;
            }
            request = *requested_;
            requested_.reset();
            runningEpoch_ = request.epoch;
        }

        DisplayStatePollResult result;
        result.generation = request.generation;
        try
        {
            result.response = read_();
            if (result.response.succeeded())
            {
                result.parsed = parseDisplayState(result.response.payload);
            }
        }
        catch (...)
        {
            result.response = {};
        }

        const std::lock_guard lock(mutex_);
        runningEpoch_.reset();
        // A response from before a disconnect, page change or mutation must
        // never replace the snapshot requested for the new UI state.
        if (request.epoch == epoch_ && !stop.stop_requested())
        {
            completed_ = std::move(result);
        }
    }
}

}
