#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace bafx::control_center
{

// Result carries the UI generation that requested it. Epochs invalidate an
// in-flight read without blocking the UI or sharing its controls with a worker.
template<class Result>
class SnapshotPoller
{
public:
    using Read = std::function<Result()>;

    explicit SnapshotPoller(Read read)
        : read_(std::move(read)), worker_([this](const std::stop_token stop)
        {
            run(stop);
        })
    {
    }

    ~SnapshotPoller()
    {
        worker_.request_stop();
        wake_.notify_all();
    }

    SnapshotPoller(const SnapshotPoller&) = delete;
    SnapshotPoller& operator=(const SnapshotPoller&) = delete;

    [[nodiscard]] bool request(const std::uint64_t generation)
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

    void invalidate()
    {
        const std::lock_guard lock(mutex_);
        ++epoch_;
        requested_.reset();
        completed_.reset();
    }

    [[nodiscard]] std::optional<Result> takeResult()
    {
        const std::lock_guard lock(mutex_);
        return std::exchange(completed_, std::nullopt);
    }

    [[nodiscard]] bool busy() const
    {
        const std::lock_guard lock(mutex_);
        // Retain completion polling until the UI consumes the value.
        return requested_.has_value() || runningEpoch_.has_value() || completed_.has_value();
    }

private:
    struct Request final
    {
        std::uint64_t epoch;
        std::uint64_t generation;
    };

    void run(const std::stop_token stop)
    {
        for (;;)
        {
            Request request{};
            {
                std::unique_lock lock(mutex_);
                if (!wake_.wait(lock, stop, [this]
                    {
                        return requested_.has_value();
                    }) || stop.stop_requested())
                {
                    return;
                }
                request = *requested_;
                requested_.reset();
                runningEpoch_ = request.epoch;
            }

            Result result{};
            try
            {
                result = read_();
            }
            catch (...)
            {
                // Each result type defaults to an observable read failure.
            }
            result.generation = request.generation;
            const std::lock_guard lock(mutex_);
            runningEpoch_.reset();
            if (request.epoch == epoch_ && !stop.stop_requested())
            {
                completed_ = std::move(result);
            }
        }
    }

    Read read_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::uint64_t epoch_{0U};
    std::optional<Request> requested_;
    std::optional<std::uint64_t> runningEpoch_;
    std::optional<Result> completed_;
    // Join before destroying any state the worker accesses.
    std::jthread worker_;
};

}
