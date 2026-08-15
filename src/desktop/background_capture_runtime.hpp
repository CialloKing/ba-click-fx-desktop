#pragma once

#include "bafx/config/config.hpp"
#include "bafx/windows/background_capture_transition.hpp"
#include "bafx/windows/runtime_diagnostics.hpp"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace bafx::desktop
{

struct BackgroundCaptureExecutionResult
{
    std::string sensorFailure{};
    std::optional<bafx::windows::WindowSize> recreatedFramePoolSize{};
    bool deviceRecovered{false};
    bool deviceRecoveryAdapterChanged{false};
};

[[nodiscard]] bafx::windows::BackgroundCaptureRequest backgroundCaptureRequest(
    const bafx::config::Config& config,
    std::uint64_t retryToken = 0U) noexcept;

[[nodiscard]] BackgroundCaptureExecutionResult executeBackgroundCaptureTransition(
    bafx::windows::BackgroundCaptureTransition& transition,
    bafx::windows::OverlayWindow& window,
    bafx::windows::CompositionRenderer& renderer,
    HMONITOR monitor,
    const std::filesystem::path& logPath);

void appendBackgroundCaptureResourceLedger(
    const std::filesystem::path& logPath,
    const bafx::windows::CompositionRenderer& renderer,
    std::string_view phase) noexcept;

bafx::windows::WgcBackgroundStopDiagnostics
appendBackgroundCaptureStopDiagnostics(
    const std::filesystem::path& logPath,
    bafx::windows::CompositionRenderer& renderer,
    std::string_view phase) noexcept;

[[nodiscard]] bafx::windows::BackgroundCaptureStatus backgroundCaptureStatus(
    bafx::windows::EffectiveBackgroundCapturePath path) noexcept;

void appendBackgroundCaptureOutcome(
    const std::filesystem::path& logPath,
    const bafx::windows::BackgroundCaptureRequest& request,
    const bafx::windows::BackgroundCaptureTransition& transition,
    const BackgroundCaptureExecutionResult& execution,
    const bafx::windows::CompositionRenderer& renderer);

void appendBackgroundSnapshotInvalidation(
    const std::filesystem::path& logPath,
    std::uint64_t controlGeneration,
    const bafx::windows::BackgroundSnapshotInvalidation& invalidation) noexcept;

void appendBackgroundCompositeParticipation(
    const std::filesystem::path& logPath,
    std::uint64_t controlGeneration,
    const bafx::windows::CompositionFrameDiagnostics& diagnostics) noexcept;

}
