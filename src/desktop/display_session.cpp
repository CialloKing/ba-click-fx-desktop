#include "display_session.hpp"
#include "display_output_retarget.hpp"

#include <utility>

namespace bafx::desktop
{
DisplaySession::DisplaySession(DisplaySessionOptions options)
    : target_(std::move(options.target)),
      window_(
          options.instance,
          target_.bounds,
          options.title,
          bafx::windows::OverlayWindowOptions::renderSurface()),
      renderer_(
          window_.handle(),
          window_.size(),
          options.bloomSettings,
          options.backgroundStopObserver,
          requestedAdapter(target_),
          options.outputPreference),
      simulation_(options.simulationSeed)
{
    colorMonitorStartResult_ = colorMonitor_.start(
        target_.monitor,
        options.wakeWindow);
    refreshColorCapabilities();
}

const DisplayTarget& DisplaySession::target() const noexcept
{
    return target_;
}

bafx::windows::OverlayWindow& DisplaySession::window() noexcept
{
    return window_;
}

const bafx::windows::OverlayWindow& DisplaySession::window() const noexcept
{
    return window_;
}

bafx::windows::CompositionRenderer& DisplaySession::renderer() noexcept
{
    return renderer_;
}

const bafx::windows::CompositionRenderer& DisplaySession::renderer() const noexcept
{
    return renderer_;
}

bafx::fx::SimulationRuntime& DisplaySession::simulation() noexcept
{
    return simulation_;
}

const bafx::fx::SimulationRuntime& DisplaySession::simulation() const noexcept
{
    return simulation_;
}

bafx::fx::SimulationTimeline& DisplaySession::timeline() noexcept
{
    return timeline_;
}

bafx::windows::DisplayColorMonitor& DisplaySession::colorMonitor() noexcept
{
    return colorMonitor_;
}

const std::optional<bafx::windows::DisplayColorCapabilities>&
DisplaySession::colorCapabilities() const noexcept
{
    return colorCapabilities_;
}

const bafx::windows::DisplayColorMonitorResult&
DisplaySession::colorMonitorStartResult() const noexcept
{
    return colorMonitorStartResult_;
}

bool DisplaySession::renderFaulted() const noexcept
{
    return renderFaulted_;
}

void DisplaySession::acceptAppliedTarget(
    DisplayTarget target,
    const HWND wakeWindow) noexcept
{
    target_ = std::move(target);
    colorMonitorStartResult_ = colorMonitor_.start(
        target_.monitor,
        wakeWindow);
}

void DisplaySession::updateTargetMetadata(DisplayTarget target) noexcept
{
    target_ = std::move(target);
}

DisplaySessionRetargetResult DisplaySession::retargetFxOnly(
    DisplayTarget target,
    const HWND wakeWindow)
{
    try
    {
        const DisplayOutputRetargetResult output = retargetDisplayOutput(
            window_,
            renderer_,
            DisplayOutputRetargetIntent{
                target.bounds,
                requestedAdapter(target),
                displayTargetSize(target)});
        acceptAppliedTarget(std::move(target), wakeWindow);
        clearRenderFault();
        return DisplaySessionRetargetResult{
            output.adapter,
            output.output};
    }
    catch (const DisplayOutputRollbackError&)
    {
        // A failed rollback leaves this independent surface untrustworthy;
        // keep it out of frame pacing until topology reconciliation replaces it.
        markRenderFaulted();
        throw;
    }
}

void DisplaySession::refreshColorCapabilities() noexcept
{
    colorCapabilities_ = bafx::windows::queryDisplayColorCapabilities(
        target_.monitor);
}

void DisplaySession::markRenderFaulted() noexcept
{
    renderFaulted_ = true;
}

void DisplaySession::clearRenderFault() noexcept
{
    renderFaulted_ = false;
}

void DisplaySession::show()
{
    window_.show();
}

std::optional<LUID> DisplaySession::requestedAdapter(
    const DisplayTarget& target) noexcept
{
    return target.sourceIdentityResolved
        ? std::optional<LUID>(target.sourceAdapterLuid)
        : std::nullopt;
}

}
