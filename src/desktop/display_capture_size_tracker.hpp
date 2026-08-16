#pragma once

#include "bafx/windows/overlay_window.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace bafx::desktop
{

struct DisplayCaptureSizeMismatch final
{
    bafx::windows::WindowSize captureSize{};
    bafx::windows::WindowSize outputSize{};
};

[[nodiscard]] inline std::string formatDisplayCaptureSizeMismatch(
    const DisplayCaptureSizeMismatch& mismatch)
{
    std::string reason = "WGC content size ";
    reason += std::to_string(mismatch.captureSize.width);
    reason += "x";
    reason += std::to_string(mismatch.captureSize.height);
    reason += " disagrees with confirmed display output ";
    reason += std::to_string(mismatch.outputSize.width);
    reason += "x";
    reason += std::to_string(mismatch.outputSize.height);
    return reason;
}

class DisplayCaptureSizeTracker final
{
public:
    [[nodiscard]] bool observeCaptureSize(
        const bafx::windows::WindowSize captureSize,
        const bafx::windows::WindowSize outputSize) noexcept
    {
        if (sameSize(captureSize, outputSize))
        {
            reset();
            return false;
        }

        if (!pendingCaptureSize_.has_value()
            || !sameSize(*pendingCaptureSize_, captureSize))
        {
            pendingCaptureSize_ = captureSize;
            candidateOutputSize_.reset();
            confirmedOutputSize_.reset();
            conflictingTopologyObservations_ = 0U;
            topologyRefreshRequested_ = true;
        }
        return true;
    }

    [[nodiscard]] bool takeTopologyRefreshRequest() noexcept
    {
        return std::exchange(topologyRefreshRequested_, false);
    }

    void confirmOutputSize(
        const bafx::windows::WindowSize outputSize) noexcept
    {
        if (!pendingCaptureSize_.has_value())
        {
            return;
        }
        if (sameSize(*pendingCaptureSize_, outputSize))
        {
            // WGC can lead the shell during rotation. The matching topology
            // proves that the output owner should resize instead of failing.
            reset();
            return;
        }

        if (!candidateOutputSize_.has_value()
            || !sameSize(*candidateOutputSize_, outputSize))
        {
            // A complete DisplayConfig query can still precede the shell's
            // rotation notification. Require one later stable observation so
            // the immediate WGC-led poll cannot reject a valid transition.
            candidateOutputSize_ = outputSize;
            confirmedOutputSize_.reset();
            conflictingTopologyObservations_ = 1U;
            return;
        }

        if (conflictingTopologyObservations_ < 2U)
        {
            ++conflictingTopologyObservations_;
        }
        if (conflictingTopologyObservations_ >= 2U)
        {
            confirmedOutputSize_ = outputSize;
        }
    }

    [[nodiscard]] std::optional<DisplayCaptureSizeMismatch>
        takeConfirmedMismatch() noexcept
    {
        if (!pendingCaptureSize_.has_value()
            || !confirmedOutputSize_.has_value())
        {
            return std::nullopt;
        }

        const DisplayCaptureSizeMismatch mismatch{
            *pendingCaptureSize_,
            *confirmedOutputSize_};
        reset();
        return mismatch;
    }

    void reset() noexcept
    {
        pendingCaptureSize_.reset();
        candidateOutputSize_.reset();
        confirmedOutputSize_.reset();
        conflictingTopologyObservations_ = 0U;
        topologyRefreshRequested_ = false;
    }

private:
    [[nodiscard]] static bool sameSize(
        const bafx::windows::WindowSize left,
        const bafx::windows::WindowSize right) noexcept
    {
        return left.width == right.width && left.height == right.height;
    }

    std::optional<bafx::windows::WindowSize> pendingCaptureSize_{};
    std::optional<bafx::windows::WindowSize> candidateOutputSize_{};
    std::optional<bafx::windows::WindowSize> confirmedOutputSize_{};
    std::uint32_t conflictingTopologyObservations_{0U};
    bool topologyRefreshRequested_{false};
};

}
