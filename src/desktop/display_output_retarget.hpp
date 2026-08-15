#pragma once

#include "bafx/windows/composition_renderer.hpp"
#include "bafx/windows/overlay_window.hpp"

#include <windows.h>

#include <optional>
#include <stdexcept>

namespace bafx::desktop
{

struct DisplayOutputRetargetIntent final
{
    std::optional<RECT> windowBounds{};
    std::optional<LUID> requestedAdapterLuid{};
    bafx::windows::WindowSize outputSize{};
};

struct DisplayOutputRetargetResult final
{
    bafx::windows::OutputAdapterRetargetStatus adapter{
        bafx::windows::OutputAdapterRetargetStatus::Unchanged};
    bafx::windows::OutputResizeStatus output{
        bafx::windows::OutputResizeStatus::Unchanged};
    bafx::windows::GraphicsDeviceInfo deviceBeforeResize{};
};

class DisplayOutputRollbackError final : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// The HWND, adapter and swap-chain size form one display resource domain.
// Either all requested changes commit, or every previous component is restored.
[[nodiscard]] DisplayOutputRetargetResult retargetDisplayOutput(
    bafx::windows::OverlayWindow& window,
    bafx::windows::CompositionRenderer& renderer,
    const DisplayOutputRetargetIntent& intent);

}
