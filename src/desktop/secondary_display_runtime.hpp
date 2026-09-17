#pragma once

#include "display_session_manager.hpp"

#include <filesystem>
#include <span>
#include <string_view>

namespace bafx::desktop
{

// Secondary surfaces keep their own failure and capture domains. The caller
// remains the render owner and supplies the frame opportunities it has acquired.
struct SecondaryRenderSummary final
{
    std::size_t rendered{0U};
    std::size_t notReady{0U};
    std::size_t recovered{0U};
    std::size_t failed{0U};
};


void appendSecondaryRenderFailure(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const std::string_view operation,
    const std::string_view message) noexcept;

void appendSecondaryBackgroundCaptureFailure(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const std::string_view operation,
    const std::string_view message) noexcept;

void applySecondaryBackgroundCaptureRequest(
    bafx::desktop::DisplaySessionManager& sessions,
    bafx::desktop::DisplaySession& coordinator,
    const bafx::windows::BackgroundCaptureRequest& request,
    const std::uint64_t controlGeneration,
    const std::filesystem::path& logPath,
    const bool powerUnavailable) noexcept;

[[nodiscard]] bool handleSecondaryBorderlessAccessLosses(
    bafx::desktop::DisplaySessionManager& sessions,
    bafx::desktop::DisplaySession& coordinator,
    const bafx::core::MonotonicTime now,
    const std::filesystem::path& logPath) noexcept;

[[nodiscard]] bool retrySecondaryBorderlessAccess(
    bafx::desktop::DisplaySessionManager& sessions,
    bafx::desktop::DisplaySession& coordinator,
    const std::uint64_t controlGeneration,
    const bafx::core::MonotonicTime now,
    const std::filesystem::path& logPath) noexcept;

void appendSecondaryBackgroundCaptureServiceResult(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const bafx::desktop::DisplaySessionBackgroundCaptureServiceResult& result)
    noexcept;

[[nodiscard]] bool serviceSecondaryBackgroundCaptures(
    bafx::desktop::DisplaySessionManager& sessions,
    bafx::desktop::DisplaySession& coordinator,
    const bafx::core::MonotonicTime now,
    const std::filesystem::path& logPath) noexcept;

[[nodiscard]] bool maintainSecondaryBackgroundCaptures(
    bafx::desktop::DisplaySessionManager& sessions,
    bafx::desktop::DisplaySession& coordinator,
    const std::span<bafx::desktop::DisplaySession*> readySessions,
    const bafx::core::MonotonicTime now,
    const std::filesystem::path& logPath) noexcept;

void appendSecondaryDeviceRecovery(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const bafx::desktop::DisplaySessionDeviceRecoveryResult& recovery,
    const std::string_view eventName) noexcept;

[[nodiscard]] bool recoverSecondaryDisplaySession(
    const std::filesystem::path& logPath,
    bafx::desktop::DisplaySession& session,
    const std::string_view failureOperation,
    const std::string_view successEvent,
    const bool validateRemovalReason) noexcept;

SecondaryRenderSummary renderSecondarySessions(
    bafx::desktop::DisplaySessionManager& sessions,
    bafx::desktop::DisplaySession& coordinator,
    const std::span<bafx::desktop::DisplaySession*> readySessions,
    const bafx::config::Config& config,
    const bafx::fx::SimulationTime renderTime,
    const bafx::core::MonotonicTime wallTime,
    const bool commitSimulationFrame,
    const bool requireCurrentBackground,
    const std::filesystem::path& logPath);

}
