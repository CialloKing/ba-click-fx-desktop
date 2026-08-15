#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace bafx::desktop
{

inline constexpr auto backgroundCaptureStopWatchdogTimeout =
    std::chrono::seconds(10);
inline constexpr std::uint32_t backgroundCaptureStopTimeoutExitCode = 124U;

using BackgroundCaptureStopTimeoutHandler = void (*)(
    const void* context) noexcept;

// WGC Close calls cannot be canceled. This guard gives the Host a hard
// process boundary without moving WinRT or D3D ownership to a detached thread.
class BackgroundCaptureStopWatchdog final
{
public:
    explicit BackgroundCaptureStopWatchdog(
        std::chrono::milliseconds timeout =
            backgroundCaptureStopWatchdogTimeout,
        BackgroundCaptureStopTimeoutHandler timeoutHandler = nullptr,
        const void* timeoutContext = nullptr);
    ~BackgroundCaptureStopWatchdog();

    BackgroundCaptureStopWatchdog(
        const BackgroundCaptureStopWatchdog&) = delete;
    BackgroundCaptureStopWatchdog& operator=(
        const BackgroundCaptureStopWatchdog&) = delete;

    [[nodiscard]] bool arm() noexcept;
    void disarm() noexcept;
    [[nodiscard]] bool armed() const noexcept;
    [[nodiscard]] std::chrono::milliseconds timeout() const noexcept;

private:
    static void terminateCurrentProcess(const void* context) noexcept;
    void run() noexcept;

    const std::chrono::milliseconds timeout_{};
    const BackgroundCaptureStopTimeoutHandler timeoutHandler_{nullptr};
    const void* const timeoutContext_{nullptr};
    mutable std::mutex mutex_{};
    std::condition_variable condition_{};
    std::uint64_t generation_{0U};
    bool armed_{false};
    bool stopping_{false};
    // Start the worker last so it cannot observe partially initialized state.
    std::thread worker_{};
};

}
