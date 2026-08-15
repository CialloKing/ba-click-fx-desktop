#pragma once

#include "bafx/windows/composition_renderer.hpp"
#include "bafx/windows/display_capabilities.hpp"
#include "bafx/windows/overlay_window.hpp"

#include <windows.h>

#include <optional>
#include <stdexcept>

namespace bafx::desktop
{

// Keep the user's HDR request independent from the transport that is safe for
// one current monitor. Unknown or contradictory display state must not promote
// the final swap chain to scRGB.
[[nodiscard]] bafx::windows::CompositionOutputPreference
resolveDisplayOutputPreference(
    bafx::windows::CompositionOutputPreference requested,
    const std::optional<bafx::windows::DisplayColorCapabilities>& capabilities)
    noexcept;

struct DisplayOutputRetargetIntent final
{
    std::optional<RECT> windowBounds{};
    std::optional<LUID> requestedAdapterLuid{};
    bafx::windows::WindowSize outputSize{};
    // Omitted callers preserve their current transport. FX-only retargets can
    // provide the new monitor's resolved policy and commit it atomically.
    std::optional<bafx::windows::CompositionOutputPreference>
        outputPreference{};
};

struct DisplayOutputRetargetResult final
{
    bafx::windows::OutputAdapterRetargetStatus adapter{
        bafx::windows::OutputAdapterRetargetStatus::Unchanged};
    bafx::windows::OutputResizeStatus output{
        bafx::windows::OutputResizeStatus::Unchanged};
    std::optional<bafx::windows::OutputRenegotiationResult>
        outputRenegotiation{};
    bafx::windows::GraphicsDeviceInfo deviceBeforeResize{};
    bafx::windows::GraphicsDeviceInfo deviceBeforeOutputRenegotiation{};
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
