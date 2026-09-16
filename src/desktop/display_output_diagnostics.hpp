#pragma once

#include "display_output_retarget.hpp"

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace bafx::desktop
{

class DisplaySession;
struct DisplayTarget;

// These helpers report an already observed output transition. Retry budgets,
// capture teardown and renderer mutations remain with the render owner.
[[nodiscard]] std::string_view outputPreferenceName(
    const bafx::windows::CompositionOutputPreference preference) noexcept;

[[nodiscard]] std::string_view outputTransferName(
    const bafx::windows::CompositionOutputTransfer transfer) noexcept;

[[nodiscard]] std::string_view outputFallbackName(
    const bafx::windows::CompositionOutputFallback fallback) noexcept;

void appendOutputRenegotiation(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const std::string_view reason,
    const bafx::windows::OutputRenegotiationResult& result) noexcept;

void appendOutputRenegotiationFailure(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const bafx::windows::CompositionOutputPolicy policy,
    const std::string_view reason,
    const std::string_view message,
    const bool deviceRecovered = false) noexcept;

void appendOutputRenegotiationRetryScheduled(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const bafx::windows::CompositionOutputPolicy policy,
    const std::string_view reason,
    const std::uint32_t retriesRemaining,
    const std::string_view cadence) noexcept;

void appendOutputRenegotiationExhausted(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const bafx::windows::CompositionOutputPolicy policy,
    const std::string_view reason,
    const bafx::desktop::DisplayOutputExhaustionDisposition disposition)
    noexcept;

void appendOutputRenegotiationDiscarded(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const bafx::desktop::DisplayTarget& queuedTarget,
    const bafx::windows::CompositionOutputPolicy policy,
    const std::string_view reason) noexcept;

}
