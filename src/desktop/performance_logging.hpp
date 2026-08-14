#pragma once

#include "bafx/config/config.hpp"
#include "bafx/windows/composition_renderer.hpp"
#include "performance_window.hpp"

#include <chrono>
#include <filesystem>
#include <string_view>

namespace bafx::desktop
{

struct PerformanceLogContext
{
    bafx::windows::WindowSize outputSize{};
    bafx::windows::BackgroundCompositeStatus backgroundStatus{
        bafx::windows::BackgroundCompositeStatus::Inactive};
    bool paused{false};
};

void appendAppliedConfiguration(
    const std::filesystem::path& logPath,
    const bafx::config::Config& config,
    bafx::windows::WindowSize outputSize,
    std::string_view reason) noexcept;

[[nodiscard]] std::chrono::nanoseconds appendPerformanceInterval(
    const std::filesystem::path& logPath,
    const RuntimePerformanceSummary& summary,
    const bafx::config::Config& config,
    const PerformanceLogContext& context,
    std::chrono::nanoseconds intervalDuration,
    std::chrono::nanoseconds previousLogWriteCpu,
    bool finalInterval) noexcept;

}
