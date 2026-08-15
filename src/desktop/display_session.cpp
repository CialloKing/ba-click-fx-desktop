#include "display_session.hpp"

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace bafx::desktop
{
namespace
{

[[nodiscard]] std::string describeException(
    const std::exception_ptr& failure)
{
    try
    {
        std::rethrow_exception(failure);
    }
    catch (const std::exception& error)
    {
        return error.what();
    }
    catch (...)
    {
        return "unknown exception";
    }
}

void appendRollbackFailure(
    std::string& failures,
    const std::string_view operation,
    const std::exception_ptr& failure)
{
    if (!failures.empty())
    {
        failures += "; ";
    }
    failures += operation;
    failures += ": ";
    failures += describeException(failure);
}

}

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
    const DisplayTarget previousTarget = target_;
    const bafx::windows::WindowSize previousSize = window_.size();
    DisplaySessionRetargetResult result{};
    try
    {
        // Move first so a rejected HWND geometry cannot unnecessarily replace
        // an otherwise healthy D3D resource domain.
        window_.setBounds(target.bounds);
        result.adapter = renderer_.retargetOutputAdapter(
            requestedAdapter(target));
        result.output = renderer_.resizeOutput(window_.size());
        acceptAppliedTarget(std::move(target), wakeWindow);
        clearRenderFault();
        return result;
    }
    catch (...)
    {
        const std::exception_ptr retargetFailure = std::current_exception();
        std::string rollbackFailures;
        const auto attemptRollback =
            [&](const std::string_view operation, auto&& rollback)
        {
            try
            {
                rollback();
            }
            catch (...)
            {
                appendRollbackFailure(
                    rollbackFailures,
                    operation,
                    std::current_exception());
            }
        };

        // Every step is attempted independently. A failed window restore must
        // not prevent the old adapter domain from being recovered as well.
        attemptRollback("window", [&]()
        {
            window_.setBounds(previousTarget.bounds);
        });
        attemptRollback("adapter", [&]()
        {
            static_cast<void>(renderer_.retargetOutputAdapter(
                requestedAdapter(previousTarget)));
        });
        attemptRollback("output", [&]()
        {
            static_cast<void>(renderer_.resizeOutput(previousSize));
        });

        if (rollbackFailures.empty())
        {
            std::rethrow_exception(retargetFailure);
        }

        markRenderFaulted();
        throw std::runtime_error(
            "Display retarget failed: " + describeException(retargetFailure)
            + "; rollback failed: " + rollbackFailures);
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
