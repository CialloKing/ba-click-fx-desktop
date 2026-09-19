#include "window_visibility_diagnostics.hpp"
#include "diagnostic_fields.hpp"

namespace bafx::desktop
{
namespace
{

void appendIdentity(DiagnosticFields& fields, const std::string& prefix,
    const bafx::windows::WindowIdentity& identity)
{
    fields.add(prefix + ".Handle", static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(identity.handle)));
    fields.add(prefix + ".ProcessId", static_cast<std::uint32_t>(identity.processId));
    fields.add(prefix + ".ProcessQueryError", static_cast<std::uint32_t>(identity.processError));
    fields.add(prefix + ".ClassQueryError", static_cast<std::uint32_t>(identity.classError));
    std::array<char, 512U> text{};
    const int length = WideCharToMultiByte(CP_UTF8, 0, identity.className.data(), -1,
        text.data(), static_cast<int>(text.size()), nullptr, nullptr);
    fields.add(prefix + ".Class", length > 0 ? text.data() : "<unavailable>");
}

}

bool WindowVisibilityDiagnostics::begin(const std::filesystem::path& logPath,
    const std::uint64_t nowTickMs, const bool final) noexcept
{
    if (started_ && !final && nowTickMs - tickMs_ < 250U)
    {
        return false;
    }
    tickMs_ = nowTickMs;
    final_ = final;
    const auto foreground = bafx::windows::observeWindowIdentity(GetForegroundWindow());
    foregroundChanged_ = !started_ || foreground != foreground_;
    foreground_ = foreground;
    started_ = true;
    for (auto& [window, previous] : windows_)
    {
        static_cast<void>(window);
        previous.seen = false;
    }
    if (foregroundChanged_)
    {
        try
        {
            DiagnosticFields fields;
            fields.add("Observation.TickMs", tickMs_);
            fields.add("Observation.Policy", "sampled-at-most-every-250ms-not-all-win-events");
            fields.add("Foreground.Available", foreground_.handle != nullptr);
            appendIdentity(fields, "Foreground", foreground_);
            fields.append(logPath, "Desktop.ForegroundChanged", bafx::windows::DiagnosticLevel::Info);
        }
        catch (...)
        {
            bafx::windows::appendDiagnosticEvent(logPath, "Desktop.Observation.ReportFailed", {},
                bafx::windows::DiagnosticLevel::Warning);
        }
    }
    return true;
}

void WindowVisibilityDiagnostics::observe(const std::filesystem::path& logPath,
    const HWND window, const SurfaceDiagnosticState& state,
    const std::uint64_t configurationGeneration, const std::uint64_t presentedFrames,
    const std::uint64_t lastPresentAgeMs,
    const bafx::fx::SimulationInputDiagnostics& simulation) noexcept
{
    try
    {
        auto& previous = windows_[window];
        previous.seen = true;
        const auto observation = bafx::windows::observeWindow(window);
        const bool changed = !previous.reported || previous.window != observation
            || previous.state != state || foregroundChanged_
            || previous.configurationGeneration != configurationGeneration;
        const bool inputActive = simulation.moveCalls != previous.simulation.moveCalls
            || simulation.ambientEnds != previous.simulation.ambientEnds;
        if (!changed && !final_
            && tickMs_ - previous.lastReportTickMs < (inputActive ? 1'000U : 10'000U))
        {
            return;
        }
        // Advance the cadence even if formatting fails; diagnostics must never
        // repeatedly allocate or write at frame rate after a failure.
        // A replacement session may reuse the same HWND. Its counters start at
        // zero and must never be subtracted from the previous session's totals.
        const bool baselineReset = !previous.reported || previous.state.instanceId != state.instanceId;
        const auto previousSimulation = baselineReset
            ? bafx::fx::SimulationInputDiagnostics{} : previous.simulation;
        const auto intervalMs = baselineReset ? 0U : tickMs_ - previous.lastReportTickMs;
        previous = Previous{observation, state, simulation, configurationGeneration, tickMs_, true, true};
        DiagnosticFields fields;
        fields.add("Observation.TickMs", tickMs_);
        fields.add("Observation.IntervalMs", intervalMs);
        fields.add("Observation.CounterBaselineReset", baselineReset);
        fields.add("Observation.Final", final_);
        fields.add("Observation.Reason", changed ? "state-change" : (inputActive ? "active-input" : "heartbeat"));
        fields.add("Configuration.Generation", configurationGeneration);
        appendIdentity(fields, "Window", observation.identity);
        appendIdentity(fields, "Foreground", foreground_);
        fields.add("Window.Valid", observation.valid);
        fields.add("Window.BoundsQueryError", static_cast<std::uint32_t>(observation.boundsError));
        fields.add("Window.StyleQueryError", static_cast<std::uint32_t>(observation.styleError));
        fields.addHex32("Window.CloakQueryHRESULT", static_cast<std::uint32_t>(observation.cloakResult));
        if (observation.valid)
        {
            fields.add("Window.Visible", observation.visible);
            fields.add("Window.Minimized", observation.minimized);
        }
        if (observation.styleError == ERROR_SUCCESS)
        {
            fields.addHex32("Window.ExtendedStyle", static_cast<std::uint32_t>(observation.extendedStyle));
            fields.add("Window.Topmost", (observation.extendedStyle & WS_EX_TOPMOST) != 0);
        }
        if (observation.boundsError == ERROR_SUCCESS)
        {
            fields.add("Window.Left", static_cast<std::int32_t>(observation.bounds[0]));
            fields.add("Window.Top", static_cast<std::int32_t>(observation.bounds[1]));
            fields.add("Window.Right", static_cast<std::int32_t>(observation.bounds[2]));
            fields.add("Window.Bottom", static_cast<std::int32_t>(observation.bounds[3]));
        }
        if (SUCCEEDED(observation.cloakResult))
        {
            fields.add("Window.Cloaked", static_cast<std::uint32_t>(observation.cloaked));
        }
        fields.add("Window.Owner", static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(observation.owner)));
        fields.add("Window.AboveCandidate.Available", observation.aboveCandidate.handle != nullptr);
        if (observation.aboveCandidate.handle != nullptr)
        {
            appendIdentity(fields, "Window.AboveCandidate", observation.aboveCandidate);
        }
        fields.add("Window.AboveScan.Count", observation.scannedAbove);
        fields.add("Window.AboveScan.Truncated", observation.scanTruncated);
        fields.add("Window.AboveScan.QueryFailures", observation.aboveQueryFailures);
        fields.add("Window.AboveScan.LastWin32Error", static_cast<std::uint32_t>(observation.lastAboveQueryError));
        fields.add("Window.VisibilitySemantic", "window-state-and-overlap-candidate-not-composed-pixels");
        fields.add("Surface.Enabled", state.enabled);
        fields.add("Surface.InstanceId", state.instanceId);
        fields.add("Surface.Paused", state.paused);
        fields.add("Surface.PowerUnavailable", state.powerUnavailable);
        fields.add("Surface.Faulted", state.faulted);
        fields.add("Surface.PointerHeld", state.pointerHeld);
        fields.add("Surface.AmbientEnabled", state.ambientEnabled);
        fields.add("Surface.AmbientActive", state.ambientActive);
        fields.add("Surface.AmbientSemantic", "allocated-stroke-or-anchor-not-visible-geometry");
        fields.add("Surface.DrawableLastFrame", state.drawableLastFrame);
        fields.add("Schedule.Available", state.scheduleReason != "not-evaluated");
        fields.add("Schedule.Reason", state.scheduleReason);
        fields.add("Schedule.Scope", "last-global-policy-decision-before-window-sample");
        if (state.scheduleReason != "not-evaluated")
        {
            fields.add("Schedule.Render", state.renderScheduled);
            fields.add("Schedule.ConfigurationGeneration", state.scheduleGeneration);
        }
        fields.add("Surface.PresentedFrames.Total", presentedFrames);
        fields.add("Surface.LastPresent.Available", presentedFrames != 0U);
        if (presentedFrames != 0U)
        {
            fields.add("Surface.LastPresentedFrameStart.AgeMs", lastPresentAgeMs);
        }
        fields.add("Surface.PresentSemantic", "successful-present-return-count-not-dwm-or-scanout");
        fields.add("Simulation.MoveCalls.Total", simulation.moveCalls);
        fields.add("Simulation.MoveCalls.Delta", simulation.moveCalls - previousSimulation.moveCalls);
        fields.add("Simulation.HeldMoves.Delta", simulation.heldMoves - previousSimulation.heldMoves);
        fields.add("Simulation.AmbientDisabled.Delta", simulation.ambientDisabled - previousSimulation.ambientDisabled);
        fields.add("Simulation.RateLimited.Delta", simulation.rateLimited - previousSimulation.rateLimited);
        fields.add("Simulation.AmbientAnchors.Delta", simulation.ambientAnchors - previousSimulation.ambientAnchors);
        fields.add("Simulation.AmbientMoves.Delta", simulation.ambientMoves - previousSimulation.ambientMoves);
        fields.add("Simulation.AmbientEnds.Delta", simulation.ambientEnds - previousSimulation.ambientEnds);
        const bool unexpectedInvisible = state.enabled && !state.faulted
            && (!observation.valid || !observation.visible || observation.minimized
                || (SUCCEEDED(observation.cloakResult) && observation.cloaked != 0U));
        fields.append(logPath, "Desktop.SurfaceState", unexpectedInvisible
            || observation.boundsError != ERROR_SUCCESS || observation.styleError != ERROR_SUCCESS
            || FAILED(observation.cloakResult)
            ? bafx::windows::DiagnosticLevel::Warning : bafx::windows::DiagnosticLevel::Info);
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticEvent(logPath, "Desktop.SurfaceState.ReportFailed", {},
            bafx::windows::DiagnosticLevel::Warning);
    }
}

void WindowVisibilityDiagnostics::end(const std::filesystem::path& logPath) noexcept
{
    for (auto it = windows_.begin(); it != windows_.end();)
    {
        if (it->second.seen)
        {
            ++it;
            continue;
        }
        try
        {
            DiagnosticFields fields;
            fields.add("Observation.TickMs", tickMs_);
            fields.add("Surface.InstanceId", it->second.state.instanceId);
            appendIdentity(fields, "Window", it->second.window.identity);
            fields.append(logPath, "Desktop.SurfaceRemoved", bafx::windows::DiagnosticLevel::Info);
        }
        catch (...)
        {
            bafx::windows::appendDiagnosticEvent(logPath, "Desktop.SurfaceRemoved.ReportFailed", {},
                bafx::windows::DiagnosticLevel::Warning);
        }
        it = windows_.erase(it);
    }
}

}
