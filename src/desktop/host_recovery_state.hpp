#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bafx::desktop
{

inline constexpr std::uint32_t maximumOutputRenegotiationAttempts = 3U;

// Copying a pending operation preserves its budget. Only a new recovery edge
// creates a fresh budget; repeated failures never wrap or silently replenish it.
class OutputRenegotiationBudget final
{
public:
    [[nodiscard]] constexpr std::uint32_t remaining() const noexcept
    {
        return remaining_;
    }

    [[nodiscard]] constexpr bool retryAfterFailure() noexcept
    {
        if (remaining_ <= 1U)
        {
            return false;
        }
        --remaining_;
        return true;
    }

private:
    std::uint32_t remaining_{maximumOutputRenegotiationAttempts};
};

// Owned by the Host render thread. Completing a request clears only scheduling
// state: the monotonic token must survive to distinguish later WGC requests.
class BackgroundRetryState final
{
public:
    explicit BackgroundRetryState(const std::uint64_t token) noexcept : token_(token)
    {
    }

    [[nodiscard]] std::uint64_t token() const noexcept
    {
        return token_;
    }

    [[nodiscard]] bool pending() const noexcept
    {
        return pending_;
    }

    [[nodiscard]] bool exhausted() const noexcept
    {
        return token_ == (std::numeric_limits<std::uint64_t>::max)();
    }

    void requireAvailable(const std::string_view message) const
    {
        if (exhausted())
        {
            throw std::runtime_error(std::string(message));
        }
    }

    void advanceToken(const std::string_view message = "WGC retry token exhausted")
    {
        requireAvailable(message);
        ++token_;
    }

    void request(const std::string_view message = "WGC retry token exhausted")
    {
        advanceToken(message);
        pending_ = true;
    }

    void finishRequest() noexcept
    {
        pending_ = false;
    }

private:
    std::uint64_t token_;
    bool pending_{false};
};

struct BackgroundCaptureObservation final
{
    bool participationLogged{false};
    bool pendingLogged{false};

    void reset() noexcept
    {
        *this = {};
    }
};

}
