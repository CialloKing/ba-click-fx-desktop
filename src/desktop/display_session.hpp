#pragma once

#include "display_target.hpp"

#include "bafx/fx/simulation_runtime.hpp"
#include "bafx/fx/simulation_timeline.hpp"
#include "bafx/windows/composition_renderer.hpp"
#include "bafx/windows/display_capabilities.hpp"
#include "bafx/windows/display_color_monitor.hpp"
#include "bafx/windows/overlay_window.hpp"

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace bafx::desktop
{

struct DisplaySessionOptions final
{
    HINSTANCE instance{nullptr};
    HWND wakeWindow{nullptr};
    DisplayTarget target{};
    std::wstring_view title{};
    bafx::windows::FxBloomSettings bloomSettings{};
    bafx::windows::WgcBackgroundStopObserver backgroundStopObserver{};
    std::uint64_t simulationSeed{0U};
};

// Owns the window, graphics device and authored state for one display. Host
// input, tray and process lifetime stay outside so additional sessions cannot
// duplicate process-global registrations.
class DisplaySession final
{
public:
    explicit DisplaySession(DisplaySessionOptions options);

    DisplaySession(const DisplaySession&) = delete;
    DisplaySession& operator=(const DisplaySession&) = delete;
    DisplaySession(DisplaySession&&) = delete;
    DisplaySession& operator=(DisplaySession&&) = delete;

    [[nodiscard]] const DisplayTarget& target() const noexcept;
    [[nodiscard]] bafx::windows::OverlayWindow& window() noexcept;
    [[nodiscard]] const bafx::windows::OverlayWindow& window() const noexcept;
    [[nodiscard]] bafx::windows::CompositionRenderer& renderer() noexcept;
    [[nodiscard]] const bafx::windows::CompositionRenderer& renderer() const noexcept;
    [[nodiscard]] bafx::fx::SimulationRuntime& simulation() noexcept;
    [[nodiscard]] const bafx::fx::SimulationRuntime& simulation() const noexcept;
    [[nodiscard]] bafx::fx::SimulationTimeline& timeline() noexcept;
    [[nodiscard]] bafx::windows::PointerFrameAdapter& pointerFrameAdapter() noexcept;
    [[nodiscard]] bafx::windows::DisplayColorMonitor& colorMonitor() noexcept;
    [[nodiscard]] const std::optional<bafx::windows::DisplayColorCapabilities>&
        colorCapabilities() const noexcept;
    [[nodiscard]] const bafx::windows::DisplayColorMonitorResult&
        colorMonitorStartResult() const noexcept;

    // Call only after the owner has transactionally moved the HWND and
    // renderer resource domain. Monitoring is then rebound to the applied
    // target before its current color state is sampled.
    void acceptAppliedTarget(DisplayTarget target, HWND wakeWindow) noexcept;
    void refreshColorCapabilities() noexcept;
    void show();

private:
    [[nodiscard]] static std::optional<LUID> requestedAdapter(
        const DisplayTarget& target) noexcept;

    DisplayTarget target_{};
    bafx::windows::OverlayWindow window_;
    bafx::windows::CompositionRenderer renderer_;
    bafx::fx::SimulationRuntime simulation_;
    bafx::fx::SimulationTimeline timeline_{};
    bafx::windows::PointerFrameAdapter pointerFrameAdapter_{};
    bafx::windows::DisplayColorMonitor colorMonitor_{};
    std::optional<bafx::windows::DisplayColorCapabilities> colorCapabilities_{};
    bafx::windows::DisplayColorMonitorResult colorMonitorStartResult_{};
};

}
