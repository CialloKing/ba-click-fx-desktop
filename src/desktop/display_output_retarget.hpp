#pragma once

#include "bafx/windows/composition_renderer.hpp"
#include "bafx/windows/display_capabilities.hpp"
#include "bafx/windows/overlay_window.hpp"

#include <windows.h>

#include <cstdint>
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

// Resolve one complete final-output policy from the user's global opt-in and
// the current monitor. Unity-authored FP16 values remain ArtisticRelative;
// reference-white metadata is carried only for the final output boundary.
[[nodiscard]] bafx::windows::CompositionOutputPolicy
resolveDisplayOutputPolicy(
    bafx::windows::CompositionOutputPreference requested,
    const std::optional<bafx::windows::DisplayColorCapabilities>& capabilities)
    noexcept;

enum class DisplayOutputExhaustionDisposition : std::uint8_t
{
    AcceptConservativeFallback,
    FailClosed
};

// A finite retry may stop only after the actual transport is conservative SDR.
// Keeping an old scRGB or unknown surface would violate explicit HDR opt-in.
[[nodiscard]] DisplayOutputExhaustionDisposition
resolveDisplayOutputExhaustionDisposition(
    const bafx::windows::CompositionOutputState& output) noexcept;

// A stable scRGB application preference can still cross into a different DWM
// output contract when the monitor's Advanced Color state changes.
[[nodiscard]] bool displayOutputContractChanged(
    bafx::windows::CompositionOutputPreference previousPreference,
    bafx::windows::CompositionOutputPreference currentPreference,
    const std::optional<bafx::windows::DisplayColorCapabilities>& previous,
    const std::optional<bafx::windows::DisplayColorCapabilities>& current)
    noexcept;

struct DisplayOutputRetargetIntent final
{
    std::optional<RECT> windowBounds{};
    std::optional<LUID> requestedAdapterLuid{};
    bafx::windows::WindowSize outputSize{};
    // Omitted callers preserve their current transport. FX-only retargets can
    // provide the new monitor's resolved policy and commit it atomically.
    std::optional<bafx::windows::CompositionOutputPolicy> outputPolicy{};
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
