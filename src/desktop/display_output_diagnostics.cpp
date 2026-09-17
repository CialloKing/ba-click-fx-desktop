#include "display_output_diagnostics.hpp"
#include "display_session.hpp"

#include "bafx/windows/runtime_diagnostics.hpp"

#include <array>
#include <iomanip>
#include <sstream>
#include <string>

namespace bafx::desktop
{

[[nodiscard]] std::string_view colorRefreshStatusName(
    const DisplaySessionColorRefreshStatus status) noexcept
{
    switch (status)
    {
    case bafx::desktop::DisplaySessionColorRefreshStatus::Refreshed:
        return "succeeded";
    case bafx::desktop::DisplaySessionColorRefreshStatus::
        RetainedTransactionSnapshot:
        return "retained-target-snapshot";
    case bafx::desktop::DisplaySessionColorRefreshStatus::
        RetainedLastKnownSnapshot:
        return "retained-last-known-snapshot";
    case bafx::desktop::DisplaySessionColorRefreshStatus::Unavailable:
        return "failed";
    }
    return "failed";
}

void appendDeviceRemovedNotificationStatus(
    const std::filesystem::path& logPath,
    const bafx::windows::CompositionRenderer& renderer,
    const std::string_view phase)
{
    const bool available = renderer.deviceRemovedWaitableObject() != nullptr;
    const std::string resultCode = bafx::desktop::formatHresult(
        renderer.deviceRemovedNotificationResult());
    const std::array fields{
        bafx::windows::DiagnosticField{"Phase", phase},
        bafx::windows::DiagnosticField{
            "Available",
            available ? "true" : "false"},
        bafx::windows::DiagnosticField{
            "RegistrationHRESULT",
            resultCode}};
    bafx::windows::appendDiagnosticEvent(
        logPath,
        "Graphics.DeviceRemovalNotification.Status",
        fields,
        available
            ? bafx::windows::DiagnosticLevel::Info
            : bafx::windows::DiagnosticLevel::Warning);
}

[[nodiscard]] std::string formatHresult(const HRESULT result)
{
    std::ostringstream stream;
    stream << "0x"
           << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0')
           << static_cast<unsigned long>(result);
    return stream.str();
}

namespace
{

[[nodiscard]] std::string_view outputMappingName(
    const bafx::windows::CompositionOutputMappingMode mapping) noexcept
{
    switch (mapping)
    {
    case bafx::windows::CompositionOutputMappingMode::ConservativeSdr:
        return "conservative-sdr";
    case bafx::windows::CompositionOutputMappingMode::AdvancedColorScRgb:
        return "advanced-color-scrgb";
    case bafx::windows::CompositionOutputMappingMode::HdrSceneReferredScRgb:
        return "hdr-scene-referred-scrgb";
    }
    return "unknown";
}

[[nodiscard]] std::string_view intensitySemanticsName(
    const bafx::core::IntensitySemantics semantics) noexcept
{
    switch (semantics)
    {
    case bafx::core::IntensitySemantics::ArtisticRelative:
        return "artistic-relative";
    case bafx::core::IntensitySemantics::ReferenceWhiteRelative:
        return "reference-white-relative";
    case bafx::core::IntensitySemantics::AbsoluteNits:
        return "absolute-nits";
    }
    return "unknown";
}

[[nodiscard]] std::string outputReferenceWhiteNits(
    const bafx::windows::CompositionOutputMapping& mapping)
{
    return mapping.referenceWhiteValid
        ? std::to_string(mapping.referenceWhiteNits)
        : "unknown";
}

[[nodiscard]] std::string_view outputRenegotiationStatusName(
    const bafx::windows::OutputRenegotiationStatus status) noexcept
{
    switch (status)
    {
    case bafx::windows::OutputRenegotiationStatus::RecreatedSameContract:
        return "recreated-same-contract";
    case bafx::windows::OutputRenegotiationStatus::ChangedWithinTransfer:
        return "changed-within-transfer";
    case bafx::windows::OutputRenegotiationStatus::ChangedToLinearScRgb:
        return "changed-to-linear-scrgb";
    case bafx::windows::OutputRenegotiationStatus::ChangedToSdr:
        return "changed-to-sdr";
    }
    return "unknown";
}

}

[[nodiscard]] std::string_view outputPreferenceName(
    const bafx::windows::CompositionOutputPreference preference) noexcept
{
    switch (preference)
    {
    case bafx::windows::CompositionOutputPreference::ConservativeSdr:
        return "conservative-sdr";
    case bafx::windows::CompositionOutputPreference::PreferLinearScRgb:
        return "prefer-linear-scrgb";
    }
    return "unknown";
}

[[nodiscard]] std::string_view outputTransferName(
    const bafx::windows::CompositionOutputTransfer transfer) noexcept
{
    switch (transfer)
    {
    case bafx::windows::CompositionOutputTransfer::Unknown:
        return "unknown";
    case bafx::windows::CompositionOutputTransfer::LinearScRgb:
        return "linear-scrgb";
    case bafx::windows::CompositionOutputTransfer::SdrGamma22:
        return "sdr-gamma22";
    }
    return "unknown";
}

[[nodiscard]] std::string_view outputFallbackName(
    const bafx::windows::CompositionOutputFallback fallback) noexcept
{
    switch (fallback)
    {
    case bafx::windows::CompositionOutputFallback::None:
        return "none";
    case bafx::windows::CompositionOutputFallback::ConservativeSdr:
        return "conservative-sdr";
    }
    return "unknown";
}

void appendOutputRenegotiation(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const std::string_view reason,
    const bafx::windows::OutputRenegotiationResult& result) noexcept
{
    try
    {
        const std::string monitor =
            bafx::desktop::formatDisplayTargetMonitor(session.target());
        const std::string previousFormat = std::to_string(
            static_cast<std::uint32_t>(result.previous.format));
        const std::string currentFormat = std::to_string(
            static_cast<std::uint32_t>(result.current.format));
        const std::string deviceRecovered = result.deviceRecovered
            ? "true"
            : "false";
        const bafx::windows::CompositionOutputPolicy& requestedPolicy =
            session.renderer().outputPolicy();
        const std::string previousReferenceWhite =
            outputReferenceWhiteNits(result.previous.mapping);
        const std::string currentReferenceWhite =
            outputReferenceWhiteNits(result.current.mapping);
        const std::string requestedReferenceWhite =
            outputReferenceWhiteNits(requestedPolicy.mapping);
        const std::string_view preferenceSatisfied =
            bafx::windows::compositionOutputSatisfiesPreference(
                result.current,
                result.currentPreference)
            ? "true"
            : "false";
        const std::string_view policySatisfied =
            bafx::windows::compositionOutputSatisfiesPolicy(
                result.current,
                requestedPolicy)
            ? "true"
            : "false";
        const std::array fields{
            bafx::windows::DiagnosticField{"Reason", reason},
            bafx::windows::DiagnosticField{"Monitor", monitor},
            bafx::windows::DiagnosticField{
                "Status",
                outputRenegotiationStatusName(result.status)},
            bafx::windows::DiagnosticField{
                "PreviousPreference",
                outputPreferenceName(result.previousPreference)},
            bafx::windows::DiagnosticField{
                "CurrentPreference",
                outputPreferenceName(result.currentPreference)},
            bafx::windows::DiagnosticField{"PreviousFormat", previousFormat},
            bafx::windows::DiagnosticField{"CurrentFormat", currentFormat},
            bafx::windows::DiagnosticField{
                "PreviousTransfer",
                outputTransferName(result.previous.transfer)},
            bafx::windows::DiagnosticField{
                "CurrentTransfer",
                outputTransferName(result.current.transfer)},
            bafx::windows::DiagnosticField{
                "PreviousMapping",
                outputMappingName(result.previous.mapping.mode)},
            bafx::windows::DiagnosticField{
                "CurrentMapping",
                outputMappingName(result.current.mapping.mode)},
            bafx::windows::DiagnosticField{
                "RequestedMapping",
                outputMappingName(requestedPolicy.mapping.mode)},
            bafx::windows::DiagnosticField{
                "IntensitySemantics",
                intensitySemanticsName(
                    result.current.mapping.intensitySemantics)},
            bafx::windows::DiagnosticField{
                "PreviousReferenceWhiteNits",
                previousReferenceWhite},
            bafx::windows::DiagnosticField{
                "CurrentReferenceWhiteNits",
                currentReferenceWhite},
            bafx::windows::DiagnosticField{
                "RequestedReferenceWhiteNits",
                requestedReferenceWhite},
            bafx::windows::DiagnosticField{
                "Fallback",
                outputFallbackName(result.current.fallback)},
            bafx::windows::DiagnosticField{
                "PreferenceSatisfied",
                preferenceSatisfied},
            bafx::windows::DiagnosticField{
                "PolicySatisfied",
                policySatisfied},
            bafx::windows::DiagnosticField{
                "DeviceRecovered",
                deviceRecovered}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "Display.Output.Renegotiated",
            fields,
            result.current.fallback ==
                    bafx::windows::CompositionOutputFallback::None
                ? bafx::windows::DiagnosticLevel::Info
                : bafx::windows::DiagnosticLevel::Warning);
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "Display output renegotiation diagnostics could not be formatted");
    }
}

void appendOutputRenegotiationFailure(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const bafx::windows::CompositionOutputPolicy policy,
    const std::string_view reason,
    const std::string_view message,
    const bool deviceRecovered) noexcept
{
    try
    {
        const std::string monitor =
            bafx::desktop::formatDisplayTargetMonitor(session.target());
        const std::string referenceWhite =
            outputReferenceWhiteNits(policy.mapping);
        const std::array fields{
            bafx::windows::DiagnosticField{"Reason", reason},
            bafx::windows::DiagnosticField{"Monitor", monitor},
            bafx::windows::DiagnosticField{
                "RequestedPreference",
                outputPreferenceName(policy.preference)},
            bafx::windows::DiagnosticField{
                "RequestedMapping",
                outputMappingName(policy.mapping.mode)},
            bafx::windows::DiagnosticField{
                "IntensitySemantics",
                intensitySemanticsName(policy.mapping.intensitySemantics)},
            bafx::windows::DiagnosticField{
                "ReferenceWhiteNits",
                referenceWhite},
            bafx::windows::DiagnosticField{"Message", message},
            bafx::windows::DiagnosticField{
                "DeviceRecovered",
                deviceRecovered ? "true" : "false"}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "Display.Output.RenegotiationFailed",
            fields,
            bafx::windows::DiagnosticLevel::Error);
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "Display output renegotiation failure could not be formatted");
    }
}

void appendOutputRenegotiationRetryScheduled(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const bafx::windows::CompositionOutputPolicy policy,
    const std::string_view reason,
    const std::uint32_t retriesRemaining,
    const std::string_view cadence) noexcept
{
    try
    {
        const std::string monitor =
            bafx::desktop::formatDisplayTargetMonitor(session.target());
        const std::string remaining = std::to_string(retriesRemaining);
        const std::string referenceWhite =
            outputReferenceWhiteNits(policy.mapping);
        const std::array fields{
            bafx::windows::DiagnosticField{"Reason", reason},
            bafx::windows::DiagnosticField{"Monitor", monitor},
            bafx::windows::DiagnosticField{
                "RequestedPreference",
                outputPreferenceName(policy.preference)},
            bafx::windows::DiagnosticField{
                "RequestedMapping",
                outputMappingName(policy.mapping.mode)},
            bafx::windows::DiagnosticField{
                "ReferenceWhiteNits",
                referenceWhite},
            bafx::windows::DiagnosticField{"RetriesRemaining", remaining},
            bafx::windows::DiagnosticField{"Cadence", cadence}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "Display.Output.RenegotiationRetryScheduled",
            fields,
            bafx::windows::DiagnosticLevel::Warning);
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "Display output renegotiation retry diagnostics could not be formatted");
    }
}

void appendOutputRenegotiationExhausted(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const bafx::windows::CompositionOutputPolicy policy,
    const std::string_view reason,
    const bafx::desktop::DisplayOutputExhaustionDisposition disposition)
    noexcept
{
    try
    {
        const bafx::windows::CompositionOutputState& output =
            session.renderer().outputState();
        const std::string monitor =
            bafx::desktop::formatDisplayTargetMonitor(session.target());
        const bool failClosed = disposition
            == bafx::desktop::DisplayOutputExhaustionDisposition::FailClosed;
        const std::array fields{
            bafx::windows::DiagnosticField{"Reason", reason},
            bafx::windows::DiagnosticField{"Monitor", monitor},
            bafx::windows::DiagnosticField{
                "RequestedPreference",
                outputPreferenceName(policy.preference)},
            bafx::windows::DiagnosticField{
                "RequestedMapping",
                outputMappingName(policy.mapping.mode)},
            bafx::windows::DiagnosticField{
                "ActualTransfer",
                outputTransferName(output.transfer)},
            bafx::windows::DiagnosticField{
                "ActualMapping",
                outputMappingName(output.mapping.mode)},
            bafx::windows::DiagnosticField{
                "Fallback",
                outputFallbackName(output.fallback)},
            bafx::windows::DiagnosticField{
                "Disposition",
                failClosed ? "fail-closed" : "accept-conservative-fallback"}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "Display.Output.RenegotiationExhausted",
            fields,
            failClosed
                ? bafx::windows::DiagnosticLevel::Error
                : bafx::windows::DiagnosticLevel::Warning);
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "Exhausted output renegotiation diagnostics could not be formatted");
    }
}

void appendOutputRenegotiationDiscarded(
    const std::filesystem::path& logPath,
    const bafx::desktop::DisplaySession& session,
    const bafx::desktop::DisplayTarget& queuedTarget,
    const bafx::windows::CompositionOutputPolicy policy,
    const std::string_view reason) noexcept
{
    try
    {
        const std::string queuedMonitor =
            bafx::desktop::formatDisplayTargetMonitor(queuedTarget);
        const std::string currentMonitor =
            bafx::desktop::formatDisplayTargetMonitor(session.target());
        const std::string queuedDevice =
            bafx::desktop::displayTargetDeviceUtf8(queuedTarget);
        const std::string currentDevice =
            bafx::desktop::displayTargetDeviceUtf8(session.target());
        const std::string referenceWhite =
            outputReferenceWhiteNits(policy.mapping);
        const std::array fields{
            bafx::windows::DiagnosticField{"Reason", reason},
            bafx::windows::DiagnosticField{
                "RequestedPreference",
                outputPreferenceName(policy.preference)},
            bafx::windows::DiagnosticField{
                "RequestedMapping",
                outputMappingName(policy.mapping.mode)},
            bafx::windows::DiagnosticField{
                "ReferenceWhiteNits",
                referenceWhite},
            bafx::windows::DiagnosticField{"Cause", "display-target-changed"},
            bafx::windows::DiagnosticField{"QueuedMonitor", queuedMonitor},
            bafx::windows::DiagnosticField{"CurrentMonitor", currentMonitor},
            bafx::windows::DiagnosticField{"QueuedDevice", queuedDevice},
            bafx::windows::DiagnosticField{"CurrentDevice", currentDevice}};
        bafx::windows::appendDiagnosticEvent(
            logPath,
            "Display.Output.RenegotiationDiscarded",
            fields);
    }
    catch (...)
    {
        bafx::windows::appendDiagnosticLog(
            logPath,
            "Discarded output renegotiation diagnostics could not be formatted");
    }
}

}
