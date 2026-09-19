#include "input_health_diagnostics.hpp"
#include "diagnostic_fields.hpp"

namespace bafx::desktop
{

void PointerRoutingDiagnostics::service(const std::filesystem::path& logPath,
    const PointerRouteHealth& snapshot, const std::uint64_t nowTickMs, const bool final) noexcept
{
    const bool changed = snapshot.events != previous_.events
        || snapshot.ownerResets != previous_.ownerResets
        || snapshot.cursorFailures != previous_.cursorFailures;
    const std::uint64_t elapsed = reported_ ? nowTickMs - lastReportTickMs_ : 0U;
    if (reported_ && !final && elapsed < (changed ? 1'000U : 10'000U))
    {
        return;
    }
    lastReportTickMs_ = nowTickMs;
    reported_ = true;
    try
    {
        constexpr std::array names{"NoMove", "EdgeFrame", "Cancelled", "HeldForwarded",
            "FreeForwarded", "AmbientDisabled", "NoPosition", "NoTargetOrOutside",
            "MappingFailed", "HeldWithoutStroke", "Discarded"};
        static_assert(names.size() == static_cast<std::size_t>(PointerRouteOutcome::Count));
        DiagnosticFields fields;
        fields.add("Observation.TickMs", nowTickMs);
        fields.add("Observation.IntervalMs", elapsed);
        fields.add("Observation.Final", final);
        fields.add("Routing.Events.Total", snapshot.events);
        fields.add("Routing.Events.Delta", snapshot.events - previous_.events);
        fields.add("Routing.RawHeld", snapshot.rawHeld);
        fields.add("Routing.PressedSessionActive", snapshot.pressedSessionActive);
        fields.add("Routing.LastOutcome", names[static_cast<std::size_t>(snapshot.lastOutcome)]);
        for (std::size_t index = 0U; index < names.size(); ++index)
        {
            fields.add(std::string("Routing.") + names[index] + ".Frames",
                snapshot.outcomes[index] - previous_.outcomes[index]);
        }
        fields.add("Routing.CursorFailures.Total", snapshot.cursorFailures);
        fields.add("Routing.CursorFailures.Delta", snapshot.cursorFailures - previous_.cursorFailures);
        fields.add("Routing.Cursor.LastWin32Error", snapshot.lastCursorError);
        fields.add("Routing.CursorFallbacks.Delta", snapshot.cursorFallbacks - previous_.cursorFallbacks);
        fields.add("Routing.MappingFailures.Total", snapshot.mappingFailures);
        fields.add("Routing.MappingFailures.Delta", snapshot.mappingFailures - previous_.mappingFailures);
        fields.add("Routing.Mapping.LastWin32Error", snapshot.lastMappingError);
        fields.add("Routing.InvalidViewports.Total", snapshot.invalidViewports);
        fields.add("Routing.ResetCalls.Total", snapshot.ownerResets);
        fields.add("Routing.Semantic", "input-frame-forwarding-not-generated-geometry-or-visible-pixels");
        const bool failed = snapshot.cursorFailures != previous_.cursorFailures
            || snapshot.mappingFailures != previous_.mappingFailures
            || snapshot.invalidViewports != previous_.invalidViewports;
        fields.append(logPath, "Input.Routing", failed
            ? bafx::windows::DiagnosticLevel::Warning : bafx::windows::DiagnosticLevel::Info);
        previous_ = snapshot;
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticEvent(logPath, "Input.Routing.ReportFailed", {},
            bafx::windows::DiagnosticLevel::Warning);
    }
}

void InputHealthDiagnostics::service(const std::filesystem::path& logPath,
    const bafx::windows::PointerHealthSnapshot& snapshot,
    const std::uint64_t nowTickMs, const bool final) noexcept
{
    const bool changed = snapshot.receivedMessages != previous_.receivedMessages
        || snapshot.cancellations != previous_.cancellations
        || snapshot.registered != previous_.registered
        || snapshot.held != previous_.held;
    // A one-second active window separates short focus transitions. Idle
    // heartbeats prove the message loop is alive without continuously writing.
    const std::uint64_t elapsed = reported_ ? nowTickMs - lastReportTickMs_ : 0U;
    if (reported_ && !final && elapsed < (changed ? 1'000U : 10'000U))
    {
        return;
    }
    try
    {
        DiagnosticFields fields;
        fields.add("Observation.TickMs", nowTickMs);
        fields.add("Observation.IntervalMs", elapsed);
        fields.add("Observation.Final", final);
        fields.add("Input.Registered", snapshot.registered);
        fields.add("Input.LogicalHeld", snapshot.held);
        fields.add("Input.Received.Total", snapshot.receivedMessages);
        fields.add("Input.Received.Delta", snapshot.receivedMessages - previous_.receivedMessages);
        fields.add("Input.Accepted.Total", snapshot.acceptedMouseMessages);
        fields.add("Input.Accepted.Delta", snapshot.acceptedMouseMessages - previous_.acceptedMouseMessages);
        fields.add("Input.Move.Delta", snapshot.moves - previous_.moves);
        fields.add("Input.Down.Delta", snapshot.downs - previous_.downs);
        fields.add("Input.Up.Delta", snapshot.ups - previous_.ups);
        fields.add("Input.Cancel.Delta", snapshot.cancellations - previous_.cancellations);
        fields.add("Input.DataReadFailures.Total", snapshot.dataReadFailures);
        fields.add("Input.DataReadFailures.Delta", snapshot.dataReadFailures - previous_.dataReadFailures);
        fields.add("Input.DataRead.LastWin32Error", static_cast<std::uint32_t>(snapshot.lastDataReadError));
        fields.add("Input.InvalidPackets.Total", snapshot.invalidPackets);
        fields.add("Input.NonMousePackets.Total", snapshot.nonMousePackets);
        fields.add("Input.CursorQueryFailures.Total", snapshot.cursorQueryFailures);
        fields.add("Input.CursorQueryFailures.Delta", snapshot.cursorQueryFailures - previous_.cursorQueryFailures);
        fields.add("Input.CursorQuery.LastWin32Error", static_cast<std::uint32_t>(snapshot.lastCursorQueryError));
        fields.add("Input.ClockQueryFailures.Total", snapshot.clockQueryFailures);
        fields.add("Input.LastReceived.Available", snapshot.receivedMessages != 0U);
        fields.add("Input.LastAccepted.Available", snapshot.acceptedMouseMessages != 0U);
        if (snapshot.receivedMessages != 0U)
        {
            fields.add("Input.LastReceived.AgeMs", nowTickMs - snapshot.lastReceivedTickMs);
        }
        if (snapshot.acceptedMouseMessages != 0U)
        {
            fields.add("Input.LastAccepted.AgeMs", nowTickMs - snapshot.lastAcceptedTickMs);
        }
        const bool failed = snapshot.dataReadFailures != previous_.dataReadFailures
            || snapshot.invalidPackets != previous_.invalidPackets
            || snapshot.cursorQueryFailures != previous_.cursorQueryFailures
            || snapshot.clockQueryFailures != previous_.clockQueryFailures;
        fields.append(logPath, "Input.Health", failed
            ? bafx::windows::DiagnosticLevel::Warning
            : bafx::windows::DiagnosticLevel::Info);
        previous_ = snapshot;
        lastReportTickMs_ = nowTickMs;
        reported_ = true;
    }
    catch (...)
    {
        // A formatting failure must not interrupt input or retry at frame rate.
        lastReportTickMs_ = nowTickMs;
        reported_ = true;
        bafx::windows::appendDiagnosticEvent(logPath, "Input.Health.ReportFailed", {},
            bafx::windows::DiagnosticLevel::Warning);
    }
}

}
