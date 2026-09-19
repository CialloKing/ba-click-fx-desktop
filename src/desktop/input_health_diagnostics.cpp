#include "input_health_diagnostics.hpp"
#include "diagnostic_fields.hpp"

namespace bafx::desktop
{

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
