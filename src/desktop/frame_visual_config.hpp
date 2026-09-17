#pragma once

#include "bafx/config/config.hpp"
#include "bafx/fx/simulation.hpp"
#include "bafx/windows/overlay_window.hpp"

namespace bafx::desktop
{

// Coordinator and secondary surfaces apply exactly the same authored FX mapping.
void applyVisualConfig(
    bafx::fx::FrameSnapshot& snapshot,
    const bafx::config::Config& config);

[[nodiscard]] bafx::fx::Viewport toViewport(const bafx::windows::WindowSize size) noexcept;

}
