#pragma once

#include "bafx/windows/unique_handle.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace bafx::windows::detail
{

class WgcFrameNotification final
{
public:
    WgcFrameNotification();

    WgcFrameNotification(const WgcFrameNotification&) = delete;
    WgcFrameNotification& operator=(const WgcFrameNotification&) = delete;

    void notifyFrame() noexcept;
    void notifyItemClosed() noexcept;
    void beginStop() noexcept;

    [[nodiscard]] HANDLE eventObject() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] bool itemClosed() const noexcept;

    // The owner calls this after draining its bounded frame budget. A callback
    // racing with ResetEvent is recovered through the generation comparison.
    void resetAfterDrain(
        std::uint64_t observedGeneration,
        bool queuedFramesMayRemain);

private:
    UniqueHandle event_{};
    std::atomic<std::uint64_t> generation_{0U};
    std::atomic_bool itemClosed_{false};
    std::atomic_bool stopping_{false};
};

}
