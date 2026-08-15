#include "bafx/windows/detail/wgc_frame_notification.hpp"

#include "bafx/windows/error.hpp"

namespace bafx::windows::detail
{

WgcFrameNotification::WgcFrameNotification()
    : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
    if (event_.get() == nullptr)
    {
        throwLastError("CreateEventW(WGC frame available)");
    }
}

void WgcFrameNotification::notifyFrame() noexcept
{
    if (stopping_.load(std::memory_order_acquire))
    {
        return;
    }
    generation_.fetch_add(1U, std::memory_order_release);
    SetEvent(event_.get());
}

void WgcFrameNotification::notifyItemClosed() noexcept
{
    itemClosed_.store(true, std::memory_order_release);
    SetEvent(event_.get());
}

void WgcFrameNotification::beginStop() noexcept
{
    stopping_.store(true, std::memory_order_release);
    SetEvent(event_.get());
}

HANDLE WgcFrameNotification::eventObject() const noexcept
{
    return event_.get();
}

std::uint64_t WgcFrameNotification::generation() const noexcept
{
    return generation_.load(std::memory_order_acquire);
}

bool WgcFrameNotification::itemClosed() const noexcept
{
    return itemClosed_.load(std::memory_order_acquire);
}

void WgcFrameNotification::resetAfterDrain(
    const std::uint64_t observedGeneration,
    const bool queuedFramesMayRemain)
{
    if (!ResetEvent(event_.get()))
    {
        throwLastError("ResetEvent(WGC frame available)");
    }
    if (queuedFramesMayRemain
        || itemClosed()
        || stopping_.load(std::memory_order_acquire)
        || generation() != observedGeneration)
    {
        SetEvent(event_.get());
    }
}

}
