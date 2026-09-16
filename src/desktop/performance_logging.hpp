#pragma once

#include "bafx/config/config.hpp"
#include "bafx/windows/composition_renderer.hpp"
#include "performance_window.hpp"

#include <chrono>
#include <filesystem>
#include <string_view>

namespace bafx::desktop
{

struct PerformanceLogTiming
{
    std::chrono::nanoseconds summary{};
    std::chrono::nanoseconds fields{};
    std::chrono::nanoseconds lockAndPrepare{};
    std::chrono::nanoseconds format{};
    std::chrono::nanoseconds fileOperations{};
    std::chrono::nanoseconds logWrite{};
    std::chrono::nanoseconds total{};
};

struct PerformanceLogContext
{
    bafx::windows::WindowSize outputSize{};
    bafx::windows::BackgroundCompositeStatus backgroundStatus{
        bafx::windows::BackgroundCompositeStatus::Inactive};
    bool paused{false};
    PerformanceLogTiming previousTiming{};
};

void appendAppliedConfiguration(
    const std::filesystem::path& logPath,
    const bafx::config::Config& config,
    bafx::windows::WindowSize outputSize,
    std::string_view reason,
    std::uint64_t generation = 0U) noexcept;

[[nodiscard]] std::chrono::nanoseconds appendPerformanceInterval(
    const std::filesystem::path& logPath,
    const RuntimePerformanceSummary& summary,
    const bafx::config::Config& config,
    const PerformanceLogContext& context,
    std::chrono::nanoseconds intervalDuration,
    std::chrono::nanoseconds previousLogWriteCpu,
    bool finalInterval,
    PerformanceLogTiming* timing = nullptr) noexcept;

// Include aggregation in the measured/reporting boundary so copying and
// sorting samples cannot disappear from diagnostics or escape into rendering.
[[nodiscard]] PerformanceLogTiming appendPerformanceWindow(
    const std::filesystem::path& logPath,
    const RuntimePerformanceWindow& window,
    const bafx::config::Config& config,
    const PerformanceLogContext& context,
    std::chrono::nanoseconds intervalDuration,
    bool finalInterval) noexcept;

}
