#include "display_session.hpp"

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
          requestedAdapter(target_)),
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

bafx::windows::PointerFrameAdapter& DisplaySession::pointerFrameAdapter() noexcept
{
    return pointerFrameAdapter_;
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
    DisplaySessionRetargetResult result{};
    result.adapter = renderer_.retargetOutputAdapter(
        requestedAdapter(target));
    window_.setBounds(target.bounds);
    result.output = renderer_.resizeOutput(window_.size());
    acceptAppliedTarget(std::move(target), wakeWindow);
    return result;
}

void DisplaySession::refreshColorCapabilities() noexcept
{
    colorCapabilities_ = bafx::windows::queryDisplayColorCapabilities(
        target_.monitor);
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
