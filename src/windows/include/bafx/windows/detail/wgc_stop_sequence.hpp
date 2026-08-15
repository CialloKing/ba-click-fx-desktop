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
        const std::uint8_t order = stageOrder(stage);
        if (order == 0U || order <= lastStageOrder_)
        {
            notify(stage, WgcBackgroundStopStageState::Failed);
            return WgcBackgroundStopOperationResult{};
        }
        lastStageOrder_ = order;
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
    [[nodiscard]] static std::uint8_t stageOrder(
        const WgcBackgroundStopStage stage) noexcept
    {
        switch (stage)
        {
        case WgcBackgroundStopStage::FrameArrivedUnregister:
            return 1U;
        case WgcBackgroundStopStage::ItemClosedUnregister:
            return 2U;
        case WgcBackgroundStopStage::SessionClose:
            return 3U;
        case WgcBackgroundStopStage::FramePoolClose:
            return 4U;
        case WgcBackgroundStopStage::Stop:
            return 0U;
        }
        return 0U;
    }

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
    std::uint8_t lastStageOrder_{0U};
};

}
