#pragma once

#include "bafx/config/config.hpp"
#include "bafx/windows/background_capture_transition.hpp"
#include "bafx/windows/runtime_diagnostics.hpp"

#include <windows.h>

#include <filesystem>
#include <string>

namespace bafx::desktop
{

struct BackgroundCaptureExecutionResult
{
    std::string sensorFailure{};
};

[[nodiscard]] bafx::windows::BackgroundCaptureRequest backgroundCaptureRequest(
    const bafx::config::Config& config) noexcept;

[[nodiscard]] BackgroundCaptureExecutionResult executeBackgroundCaptureTransition(
    bafx::windows::BackgroundCaptureTransition& transition,
    bafx::windows::OverlayWindow& window,
    bafx::windows::CompositionRenderer& renderer,
    HMONITOR monitor,
    const std::filesystem::path& logPath);

[[nodiscard]] bafx::windows::BackgroundCaptureStatus backgroundCaptureStatus(
    bafx::windows::EffectiveBackgroundCapturePath path) noexcept;

void appendBackgroundCaptureOutcome(
    const std::filesystem::path& logPath,
    const bafx::windows::BackgroundCaptureRequest& request,
    const bafx::windows::BackgroundCaptureTransition& transition,
    const BackgroundCaptureExecutionResult& execution,
    const bafx::windows::CompositionRenderer& renderer);

}
