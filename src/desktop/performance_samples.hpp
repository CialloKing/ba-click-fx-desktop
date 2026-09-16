#pragma once

#include "performance_window.hpp"

#include <chrono>
#include <cstdint>

namespace bafx::windows
{
struct CompositionFrameDiagnostics;
struct WgcBackgroundDrainDiagnostics;
}

namespace bafx::desktop
{

[[nodiscard]] std::uint64_t durationMicroseconds(std::chrono::nanoseconds duration) noexcept;

// Convert renderer observations at one boundary; aggregation and log I/O stay
// in their existing modules, so formatting cannot enter the per-frame path.
[[nodiscard]] FramePerformanceSample wgcPerformanceSample(
    const bafx::windows::WgcBackgroundDrainDiagnostics& wgc,
    std::chrono::nanoseconds drainInclusiveCpu,
    std::uint64_t producerCallbacks,
    bool active,
    bool drainAttempted,
    bool idleDrainAttempted,
    bool idleDrainSkipped) noexcept;

[[nodiscard]] FramePerformanceSample framePerformanceSample(
    const bafx::windows::CompositionFrameDiagnostics& frame,
    std::uint64_t wgcProducerCallbacks,
    bool diagnosticReadbackUsed) noexcept;

}
