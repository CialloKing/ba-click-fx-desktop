#pragma once

#include "bafx/windows/wgc_background_sensor.hpp"

#include <chrono>

namespace bafx::windows::detail
{

struct WgcBackgroundStopOperationResult
{
    std::chrono::nanoseconds elapsed{};
    bool succeeded{false};
};

// Keep the order and failure isolation independent from WinRT so every stop
// stage can be verified without a live capture session.
class WgcBackgroundStopSequence final
{
public:
    WgcBackgroundStopSequence(
        const WgcBackgroundStopObserver observer,
        const DWORD ownerThreadId,
        const DWORD callerThreadId) noexcept
        : observer_(observer),
          ownerThreadId_(ownerThreadId),
          callerThreadId_(callerThreadId)
    {
        notify(
            WgcBackgroundStopStage::Stop,
            WgcBackgroundStopStageState::Begin);
    }

    template <typename Operation>
    [[nodiscard]] WgcBackgroundStopOperationResult run(
        const WgcBackgroundStopStage stage,
        Operation&& operation) noexcept
    {
        notify(stage, WgcBackgroundStopStageState::Begin);
        const auto startedAt = std::chrono::steady_clock::now();
        bool succeeded = true;
        try
        {
            operation();
        }
        catch (...)
        {
            succeeded = false;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - startedAt);
        notify(
            stage,
            succeeded
                ? WgcBackgroundStopStageState::Succeeded
                : WgcBackgroundStopStageState::Failed);
        return WgcBackgroundStopOperationResult{elapsed, succeeded};
    }

    void complete(const bool succeeded) const noexcept
    {
        notify(
            WgcBackgroundStopStage::Stop,
            succeeded
                ? WgcBackgroundStopStageState::Succeeded
                : WgcBackgroundStopStageState::Failed);
    }

    [[nodiscard]] bool ownerThreadMatched() const noexcept
    {
        return ownerThreadId_ == callerThreadId_;
    }

private:
    void notify(
        const WgcBackgroundStopStage stage,
        const WgcBackgroundStopStageState state) const noexcept
    {
        observer_.notify(WgcBackgroundStopProgress{
            stage,
            state,
            ownerThreadId_,
            callerThreadId_});
    }

    WgcBackgroundStopObserver observer_{};
    DWORD ownerThreadId_{0U};
    DWORD callerThreadId_{0U};
};

}
