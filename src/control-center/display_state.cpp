#include "display_state.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>

namespace bafx::control_center
{
namespace
{

constexpr std::size_t maximumDocumentBytes = 256U * 1024U;
constexpr std::size_t maximumStringBytes = 16U * 1024U;
constexpr std::size_t maximumSessions = 64U;
constexpr std::size_t maximumPhysicalTargets = 64U;
constexpr std::size_t maximumOfflineOverrides = 64U;

enum class RootField : std::uint32_t
{
    SchemaVersion,
    RuntimeGeneration,
    ConfigGeneration,
    AppliedConfigGeneration,
    TopologyStatus,
    TopologyError,
    OfflineOverridesAuthoritative,
    OfflineOverrides,
    Sessions,
    Count
};

enum class SessionField : std::uint32_t
{
    Monitor,
    Device,
    DisplayKey,
    Coordinator,
    Primary,
    EffectsEnabled,
    HdrEnabled,
    FramePacing,
    SourceAdapterResolved,
    SourceIdentityResolved,
    SourceId,
    PhysicalTargetCount,
    Left,
    Top,
    Right,
    Bottom,
    TargetDpiX,
    TargetDpiY,
    WindowDpi,
    DisplayRefresh,
    CaptureRefresh,
    CaptureCadenceStatus,
    ProducerPolicyRefresh,
    FreshnessPolicyRefresh,
    FreshnessPeriodUs,
    ProducerCadenceStatus,
    ProducerRequestedPeriodUs,
    ProducerAppliedPeriodUs,
    ProducerResult,
    Adapter,
    Driver,
    RequestedOutput,
    ResolvedOutput,
    ActualOutput,
    OutputPolicySatisfied,
    ResolvedOutputMapping,
    ActualOutputMapping,
    OutputFallbackResult,
    ColorMode,
    HdrSupported,
    HdrActive,
    ColorMonitorStatus,
    ColorMonitorHresult,
    ColorMonitorGeneration,
    ColorQueryGeneration,
    ColorSnapshotDisposition,
    ColorSnapshotComplete,
    AdvancedColorQueryResult,
    AdvancedColorLimitedByPolicy,
    HdrUserEnabled,
    SdrWhiteLevelQueryResult,
    SdrWhiteLevelNits,
    SdrWhiteLevelRetained,
    SdrWhiteLevelConsistent,
    ColorRefreshRetriesRemaining,
    CadenceFallbackReason,
    PhysicalCadence,
    OutputFallback,
    BackgroundCaptureActive,
    BackgroundCaptureRestartAllowed,
    BackgroundCaptureFailure,
    RenderFaulted,
    OutputContractFaulted,
    ActiveFxRoi,
    Count
};

enum class ActiveFxRoiField : std::uint32_t
{
    Enabled,
    SampleWindowMs,
    SampleAgeMs,
    LastFrameId,
    Primary,
    RecordingRebuild,
    Count
};

enum class ActiveFxRoiPathField : std::uint32_t
{
    Requested,
    Executed,
    Eligible,
    Warmup,
    ActualPath,
    DecisionReason,
    ObservedFrames,
    RequestedFrames,
    EligibleFrames,
    AppliedFrames,
    WarmupFrames,
    FullPixels,
    DrawnPixels,
    ClearedPixels,
    GuardX,
    GuardY,
    Phase,
    DirtyRect,
    AlignedRect,
    Gpu,
    ReasonCounts,
    Count
};

enum class ActiveFxRoiRectField : std::uint32_t
{
    Left,
    Top,
    Right,
    Bottom,
    Count
};

enum class ActiveFxRoiGpuField : std::uint32_t
{
    Prefilter,
    Pyramid,
    FinalComposite,
    Count
};

enum class ActiveFxRoiGpuPercentileField : std::uint32_t
{
    P50Us,
    P95Us,
    Count
};

enum class PhysicalCadenceField : std::uint32_t
{
    VirtualRefresh,
    PhysicalRefresh,
    CaptureRefresh,
    DrrBoosted,
    Available,
    Count
};

enum class OfflineOverrideField : std::uint32_t
{
    DisplayKey,
    EffectsEnabled,
    HdrEnabled,
    FramePacing,
    Count
};

template <typename Field>
[[nodiscard]] constexpr std::uint64_t requiredFieldMask() noexcept
{
    constexpr std::uint32_t count = static_cast<std::uint32_t>(Field::Count);
    static_assert(count <= 64U);
    if constexpr (count == 64U)
    {
        return (std::numeric_limits<std::uint64_t>::max)();
    }
    else
    {
        return (1ULL << count) - 1ULL;
    }
}

constexpr std::uint64_t requiredRootFields =
    requiredFieldMask<RootField>();
constexpr std::uint64_t requiredSessionFields =
    requiredFieldMask<SessionField>();
constexpr std::uint64_t requiredPhysicalCadenceFields =
    requiredFieldMask<PhysicalCadenceField>();
constexpr std::uint64_t requiredOfflineOverrideFields =
    requiredFieldMask<OfflineOverrideField>();
constexpr std::uint64_t requiredActiveFxRoiFields =
    requiredFieldMask<ActiveFxRoiField>();
constexpr std::uint64_t requiredActiveFxRoiPathFields =
    requiredFieldMask<ActiveFxRoiPathField>();
constexpr std::uint64_t requiredActiveFxRoiRectFields =
    requiredFieldMask<ActiveFxRoiRectField>();
constexpr std::uint64_t requiredActiveFxRoiGpuFields =
    requiredFieldMask<ActiveFxRoiGpuField>();
constexpr std::uint64_t requiredActiveFxRoiGpuPercentileFields =
    requiredFieldMask<ActiveFxRoiGpuPercentileField>();
constexpr std::uint64_t requiredActiveFxRoiReasonFields =
    requiredFieldMask<ActiveFxRoiReasonState>();

class DisplayStateJsonParser final
{
public:
    explicit DisplayStateJsonParser(const std::string_view input) noexcept
        : input_(input)
    {
    }

    [[nodiscard]] DisplayStateParseResult parse()
    {
        if (input_.size() > maximumDocumentBytes)
        {
            return failure("display state exceeds the protocol size limit");
        }

        DisplayState state{};
        skipWhitespace();
        if (!consume('{'))
        {
            return failure("display state must be an object");
        }

        std::uint64_t seen = 0U;
        skipWhitespace();
        if (consume('}'))
        {
            return failure("display state object is empty");
        }

        while (position_ < input_.size())
        {
            std::string key;
            if (!parseString(key))
            {
                return failure("display state has an invalid property name");
            }
            skipWhitespace();
            if (!consume(':'))
            {
                return failure("display state property is missing ':'");
            }
            skipWhitespace();

            if (key == "schemaVersion")
            {
                if (!markField(seen, RootField::SchemaVersion)
                    || !parseUnsigned(state.schemaVersion)
                    || state.schemaVersion != 3U)
                {
                    return failure(
                        "schemaVersion must be exactly 3");
                }
            }
            else if (key == "runtimeGeneration")
            {
                if (!markField(seen, RootField::RuntimeGeneration)
                    || !parseUnsigned(state.runtimeGeneration))
                {
                    return failure(
                        "runtimeGeneration must be one unsigned integer");
                }
            }
            else if (key == "configGeneration")
            {
                if (!markField(seen, RootField::ConfigGeneration)
                    || !parseUnsigned(state.configGeneration))
                {
                    return failure("configGeneration is invalid");
                }
            }
            else if (key == "appliedConfigGeneration")
            {
                if (!markField(seen, RootField::AppliedConfigGeneration)
                    || !parseUnsigned(state.appliedConfigGeneration))
                {
                    return failure("appliedConfigGeneration is invalid");
                }
            }
            else if (key == "topologyStatus")
            {
                if (!markField(seen, RootField::TopologyStatus)
                    || !parseTopology(state.topologyStatus))
                {
                    return failure("topologyStatus is invalid");
                }
            }
            else if (key == "topologyError")
            {
                if (!markField(seen, RootField::TopologyError)
                    || !parseUnsigned(state.topologyError))
                {
                    return failure("topologyError is invalid");
                }
            }
            else if (key == "offlineOverridesAuthoritative")
            {
                if (!markField(
                        seen,
                        RootField::OfflineOverridesAuthoritative)
                    || !parseBoolean(
                        state.offlineOverridesAuthoritative))
                {
                    return failure(
                        "offlineOverridesAuthoritative is invalid");
                }
            }
            else if (key == "offlineOverrides")
            {
                if (!markField(seen, RootField::OfflineOverrides)
                    || !parseOfflineOverrides(state.offlineOverrides))
                {
                    return failure("offlineOverrides is invalid");
                }
            }
            else if (key == "sessions")
            {
                if (!markField(seen, RootField::Sessions)
                    || !parseSessions(state.sessions))
                {
                    return failure(
                        "sessions must be one valid display array");
                }
            }
            else
            {
                return failure("display state contains an unknown property");
            }

            skipWhitespace();
            if (consume('}'))
            {
                break;
            }
            if (!consume(','))
            {
                return failure(
                    "display state properties must be comma-separated");
            }
            skipWhitespace();
        }

        skipWhitespace();
        if (position_ != input_.size())
        {
            return failure("display state has trailing characters");
        }
        if (seen != requiredRootFields)
        {
            return failure("display state is missing a required property");
        }
        const bool completeTopology = state.topologyStatus
            == DisplayTopologyState::Complete;
        if (state.offlineOverridesAuthoritative != completeTopology
            || (!state.offlineOverridesAuthoritative
                && !state.offlineOverrides.empty()))
        {
            return failure(
                "offline override authority contradicts topology status");
        }

        DisplayStateParseResult result{};
        result.state = std::move(state);
        return result;
    }

private:
    [[nodiscard]] bool parseOfflineOverrides(
        std::vector<bafx::config::DisplayOverrideConfig>& output)
    {
        if (!consume('['))
        {
            return false;
        }
        skipWhitespace();
        if (consume(']'))
        {
            return true;
        }

        while (position_ < input_.size())
        {
            if (output.size() >= maximumOfflineOverrides)
            {
                return false;
            }
            bafx::config::DisplayOverrideConfig value{};
            if (!parseOfflineOverride(value))
            {
                return false;
            }
            if (!output.empty()
                && value.displayKey <= output.back().displayKey)
            {
                // Host serializes the sorted configuration order. Requiring
                // it here also rejects duplicate keys without a second set.
                return false;
            }
            output.push_back(std::move(value));
            skipWhitespace();
            if (consume(']'))
            {
                return true;
            }
            if (!consume(','))
            {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    [[nodiscard]] bool parseOfflineOverride(
        bafx::config::DisplayOverrideConfig& output)
    {
        if (!consume('{'))
        {
            return false;
        }
        skipWhitespace();

        std::uint64_t seen = 0U;
        while (position_ < input_.size())
        {
            std::string key;
            if (!parseString(key))
            {
                return false;
            }
            skipWhitespace();
            if (!consume(':'))
            {
                return false;
            }
            skipWhitespace();

            if (key == "displayKey")
            {
                if (!markField(seen, OfflineOverrideField::DisplayKey)
                    || !parseString(output.displayKey)
                    || output.displayKey.empty())
                {
                    return false;
                }
            }
            else if (key == "effectsEnabled")
            {
                if (!markField(seen, OfflineOverrideField::EffectsEnabled)
                    || !parseBoolean(output.enabled))
                {
                    return false;
                }
            }
            else if (key == "hdrEnabled")
            {
                if (!markField(seen, OfflineOverrideField::HdrEnabled)
                    || !parseBoolean(output.hdrEnabled))
                {
                    return false;
                }
            }
            else if (key == "framePacing")
            {
                if (!markField(seen, OfflineOverrideField::FramePacing)
                    || !parseFramePacing(output.framePacing))
                {
                    return false;
                }
            }
            else
            {
                return false;
            }

            skipWhitespace();
            if (consume('}'))
            {
                return seen == requiredOfflineOverrideFields;
            }
            if (!consume(','))
            {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    [[nodiscard]] bool parseSessions(
        std::vector<DisplaySessionState>& sessions)
    {
        if (!consume('['))
        {
            return false;
        }
        skipWhitespace();
        if (consume(']'))
        {
            return true;
        }

        while (position_ < input_.size())
        {
            if (sessions.size() >= maximumSessions)
            {
                return false;
            }
            DisplaySessionState session{};
            if (!parseSession(session))
            {
                return false;
            }
            sessions.push_back(std::move(session));
            skipWhitespace();
            if (consume(']'))
            {
                return true;
            }
            if (!consume(','))
            {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    [[nodiscard]] bool parseSession(DisplaySessionState& session)
    {
        if (!consume('{'))
        {
            return false;
        }
        skipWhitespace();

        std::uint64_t seen = 0U;
        while (position_ < input_.size())
        {
            std::string key;
            if (!parseString(key))
            {
                return false;
            }
            skipWhitespace();
            if (!consume(':'))
            {
                return false;
            }
            skipWhitespace();

            if (!parseSessionField(key, session, seen))
            {
                return false;
            }
            skipWhitespace();
            if (consume('}'))
            {
                return seen == requiredSessionFields;
            }
            if (!consume(','))
            {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    [[nodiscard]] bool parseSessionField(
        const std::string_view key,
        DisplaySessionState& session,
        std::uint64_t& seen)
    {
        if (key == "monitor")
        {
            return markField(seen, SessionField::Monitor)
                && parseString(session.monitor);
        }
        if (key == "device")
        {
            return markField(seen, SessionField::Device)
                && parseString(session.device);
        }
        if (key == "displayKey")
        {
            return markField(seen, SessionField::DisplayKey)
                && parseOptionalString(session.displayKey);
        }
        if (key == "coordinator")
        {
            return markField(seen, SessionField::Coordinator)
                && parseBoolean(session.coordinator);
        }
        if (key == "primary")
        {
            return markField(seen, SessionField::Primary)
                && parseBoolean(session.primary);
        }
        if (key == "effectsEnabled")
        {
            return markField(seen, SessionField::EffectsEnabled)
                && parseBoolean(session.effectsEnabled);
        }
        if (key == "hdrEnabled")
        {
            return markField(seen, SessionField::HdrEnabled)
                && parseBoolean(session.hdrEnabled);
        }
        if (key == "framePacing")
        {
            return markField(seen, SessionField::FramePacing)
                && parseFramePacing(session.framePacing);
        }
        if (key == "sourceAdapterResolved")
        {
            return markField(seen, SessionField::SourceAdapterResolved)
                && parseBoolean(session.sourceAdapterResolved);
        }
        if (key == "sourceIdentityResolved")
        {
            return markField(seen, SessionField::SourceIdentityResolved)
                && parseBoolean(session.sourceIdentityResolved);
        }
        if (key == "sourceId")
        {
            return markField(seen, SessionField::SourceId)
                && parseOptionalUnsigned(session.sourceId);
        }
        if (key == "physicalTargetCount")
        {
            return markField(seen, SessionField::PhysicalTargetCount)
                && parseUnsigned(session.physicalTargetCount);
        }
        if (key == "left")
        {
            return markField(seen, SessionField::Left)
                && parseSigned(session.left);
        }
        if (key == "top")
        {
            return markField(seen, SessionField::Top)
                && parseSigned(session.top);
        }
        if (key == "right")
        {
            return markField(seen, SessionField::Right)
                && parseSigned(session.right);
        }
        if (key == "bottom")
        {
            return markField(seen, SessionField::Bottom)
                && parseSigned(session.bottom);
        }
        if (key == "targetDpiX")
        {
            return markField(seen, SessionField::TargetDpiX)
                && parseUnsigned(session.targetDpiX);
        }
        if (key == "targetDpiY")
        {
            return markField(seen, SessionField::TargetDpiY)
                && parseUnsigned(session.targetDpiY);
        }
        if (key == "windowDpi")
        {
            return markField(seen, SessionField::WindowDpi)
                && parseUnsigned(session.windowDpi);
        }
        if (key == "displayRefresh")
        {
            return markField(seen, SessionField::DisplayRefresh)
                && parseRefreshRate(session.displayRefresh);
        }
        if (key == "captureRefresh")
        {
            return markField(seen, SessionField::CaptureRefresh)
                && parseRefreshRate(session.captureRefresh);
        }
        if (key == "captureCadenceStatus")
        {
            return markField(seen, SessionField::CaptureCadenceStatus)
                && parseCaptureCadenceStatus(
                    session.captureCadenceStatus);
        }
        if (key == "producerPolicyRefresh")
        {
            return markField(seen, SessionField::ProducerPolicyRefresh)
                && parseRefreshRate(session.producerPolicyRefresh);
        }
        if (key == "freshnessPolicyRefresh")
        {
            return markField(seen, SessionField::FreshnessPolicyRefresh)
                && parseRefreshRate(session.freshnessPolicyRefresh);
        }
        if (key == "freshnessPeriodUs")
        {
            return markField(seen, SessionField::FreshnessPeriodUs)
                && parseUnsigned(session.freshnessPeriodUs);
        }
        if (key == "producerCadenceStatus")
        {
            return markField(seen, SessionField::ProducerCadenceStatus)
                && parseProducerCadenceStatus(
                    session.producerCadenceStatus);
        }
        if (key == "producerRequestedPeriodUs")
        {
            return markField(seen, SessionField::ProducerRequestedPeriodUs)
                && parseUnsigned(session.producerRequestedPeriodUs);
        }
        if (key == "producerAppliedPeriodUs")
        {
            return markField(seen, SessionField::ProducerAppliedPeriodUs)
                && parseUnsigned(session.producerAppliedPeriodUs);
        }
        if (key == "producerResult")
        {
            return markField(seen, SessionField::ProducerResult)
                && parseSigned(session.producerResult);
        }
        if (key == "adapter")
        {
            return markField(seen, SessionField::Adapter)
                && parseString(session.adapter);
        }
        if (key == "driver")
        {
            return markField(seen, SessionField::Driver)
                && parseDriver(session.driver);
        }
        if (key == "requestedOutput")
        {
            return markField(seen, SessionField::RequestedOutput)
                && parseOutput(session.requestedOutput);
        }
        if (key == "resolvedOutput")
        {
            return markField(seen, SessionField::ResolvedOutput)
                && parseOutput(session.resolvedOutput);
        }
        if (key == "actualOutput")
        {
            return markField(seen, SessionField::ActualOutput)
                && parseOutput(session.actualOutput);
        }
        if (key == "outputPolicySatisfied")
        {
            return markField(seen, SessionField::OutputPolicySatisfied)
                && parseBoolean(session.outputPolicySatisfied);
        }
        if (key == "resolvedOutputMapping")
        {
            return markField(seen, SessionField::ResolvedOutputMapping)
                && parseOutputMapping(session.resolvedOutputMapping);
        }
        if (key == "actualOutputMapping")
        {
            return markField(seen, SessionField::ActualOutputMapping)
                && parseOutputMapping(session.actualOutputMapping);
        }
        if (key == "outputFallbackResult")
        {
            return markField(seen, SessionField::OutputFallbackResult)
                && parseSigned(session.outputFallbackResult);
        }
        if (key == "colorMode")
        {
            return markField(seen, SessionField::ColorMode)
                && parseColor(session.colorMode);
        }
        if (key == "hdrSupported")
        {
            return markField(seen, SessionField::HdrSupported)
                && parseOptionalBoolean(session.hdrSupported);
        }
        if (key == "hdrActive")
        {
            return markField(seen, SessionField::HdrActive)
                && parseOptionalBoolean(session.hdrActive);
        }
        if (key == "colorMonitorStatus")
        {
            return markField(seen, SessionField::ColorMonitorStatus)
                && parseColorMonitorStatus(session.colorMonitorStatus);
        }
        if (key == "colorMonitorHresult")
        {
            return markField(seen, SessionField::ColorMonitorHresult)
                && parseSigned(session.colorMonitorHresult);
        }
        if (key == "colorMonitorGeneration")
        {
            return markField(seen, SessionField::ColorMonitorGeneration)
                && parseUnsigned(session.colorMonitorGeneration);
        }
        if (key == "colorQueryGeneration")
        {
            return markField(seen, SessionField::ColorQueryGeneration)
                && parseUnsigned(session.colorQueryGeneration);
        }
        if (key == "colorSnapshotDisposition")
        {
            return markField(seen, SessionField::ColorSnapshotDisposition)
                && parseColorSnapshotDisposition(
                    session.colorSnapshotDisposition);
        }
        if (key == "colorSnapshotComplete")
        {
            return markField(seen, SessionField::ColorSnapshotComplete)
                && parseBoolean(session.colorSnapshotComplete);
        }
        if (key == "advancedColorQueryResult")
        {
            return markField(seen, SessionField::AdvancedColorQueryResult)
                && parseOptionalSigned(session.advancedColorQueryResult);
        }
        if (key == "advancedColorLimitedByPolicy")
        {
            return markField(
                    seen,
                    SessionField::AdvancedColorLimitedByPolicy)
                && parseOptionalBoolean(
                    session.advancedColorLimitedByPolicy);
        }
        if (key == "hdrUserEnabled")
        {
            return markField(seen, SessionField::HdrUserEnabled)
                && parseOptionalBoolean(session.hdrUserEnabled);
        }
        if (key == "sdrWhiteLevelQueryResult")
        {
            return markField(seen, SessionField::SdrWhiteLevelQueryResult)
                && parseOptionalSigned(
                    session.sdrWhiteLevelQueryResult);
        }
        if (key == "sdrWhiteLevelNits")
        {
            return markField(seen, SessionField::SdrWhiteLevelNits)
                && parseOptionalFloat(session.sdrWhiteLevelNits);
        }
        if (key == "sdrWhiteLevelRetained")
        {
            return markField(seen, SessionField::SdrWhiteLevelRetained)
                && parseOptionalBoolean(session.sdrWhiteLevelRetained);
        }
        if (key == "sdrWhiteLevelConsistent")
        {
            return markField(seen, SessionField::SdrWhiteLevelConsistent)
                && parseOptionalBoolean(session.sdrWhiteLevelConsistent);
        }
        if (key == "colorRefreshRetriesRemaining")
        {
            return markField(
                    seen,
                    SessionField::ColorRefreshRetriesRemaining)
                && parseUnsigned(session.colorRefreshRetriesRemaining);
        }
        if (key == "cadenceFallbackReason")
        {
            return markField(seen, SessionField::CadenceFallbackReason)
                && parseCadenceFallback(session.cadenceFallbackReason);
        }
        if (key == "physicalCadence")
        {
            return markField(seen, SessionField::PhysicalCadence)
                && parsePhysicalCadence(session.physicalCadence);
        }
        if (key == "outputFallback")
        {
            return markField(seen, SessionField::OutputFallback)
                && parseOutputFallback(session.outputFallback);
        }
        if (key == "backgroundCaptureActive")
        {
            return markField(seen, SessionField::BackgroundCaptureActive)
                && parseBoolean(session.backgroundCaptureActive);
        }
        if (key == "backgroundCaptureRestartAllowed")
        {
            return markField(
                    seen,
                    SessionField::BackgroundCaptureRestartAllowed)
                && parseBoolean(session.backgroundCaptureRestartAllowed);
        }
        if (key == "backgroundCaptureFailure")
        {
            return markField(seen, SessionField::BackgroundCaptureFailure)
                && parseString(session.backgroundCaptureFailure);
        }
        if (key == "renderFaulted")
        {
            return markField(seen, SessionField::RenderFaulted)
                && parseBoolean(session.renderFaulted);
        }
        if (key == "outputContractFaulted")
        {
            return markField(seen, SessionField::OutputContractFaulted)
                && parseBoolean(session.outputContractFaulted);
        }
        if (key == "activeFxRoi")
        {
            return markField(seen, SessionField::ActiveFxRoi)
                && parseActiveFxRoi(session.activeFxRoi);
        }
        return false;
    }

    [[nodiscard]] bool parseActiveFxRoi(ActiveFxRoiRuntimeState& output)
    {
        if (!consume('{'))
        {
            return false;
        }
        skipWhitespace();

        std::uint64_t seen = 0U;
        while (position_ < input_.size())
        {
            std::string key;
            if (!parseString(key))
            {
                return false;
            }
            skipWhitespace();
            if (!consume(':'))
            {
                return false;
            }
            skipWhitespace();

            bool parsed = false;
            if (key == "enabled")
            {
                parsed = markField(seen, ActiveFxRoiField::Enabled)
                    && parseBoolean(output.enabled);
            }
            else if (key == "sampleWindowMs")
            {
                parsed = markField(seen, ActiveFxRoiField::SampleWindowMs)
                    && parseUnsigned(output.sampleWindowMs)
                    && output.sampleWindowMs > 0U
                    && output.sampleWindowMs <= 60'000U;
            }
            else if (key == "sampleAgeMs")
            {
                parsed = markField(seen, ActiveFxRoiField::SampleAgeMs)
                    && parseUnsigned(output.sampleAgeMs);
            }
            else if (key == "lastFrameId")
            {
                parsed = markField(seen, ActiveFxRoiField::LastFrameId)
                    && parseUnsigned(output.lastFrameId);
            }
            else if (key == "primary")
            {
                parsed = markField(seen, ActiveFxRoiField::Primary)
                    && parseActiveFxRoiPath(output.primary);
            }
            else if (key == "recordingRebuild")
            {
                parsed = markField(
                        seen,
                        ActiveFxRoiField::RecordingRebuild)
                    && parseActiveFxRoiPath(output.recordingRebuild);
            }
            if (!parsed)
            {
                return false;
            }

            skipWhitespace();
            if (consume('}'))
            {
                return seen == requiredActiveFxRoiFields;
            }
            if (!consume(','))
            {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    [[nodiscard]] bool parseActiveFxRoiPath(
        ActiveFxRoiPathRuntimeState& output)
    {
        if (!consume('{'))
        {
            return false;
        }
        skipWhitespace();

        std::uint64_t seen = 0U;
        while (position_ < input_.size())
        {
            std::string key;
            if (!parseString(key))
            {
                return false;
            }
            skipWhitespace();
            if (!consume(':'))
            {
                return false;
            }
            skipWhitespace();

            bool parsed = false;
            if (key == "requested")
            {
                parsed = markField(seen, ActiveFxRoiPathField::Requested)
                    && parseBoolean(output.requested);
            }
            else if (key == "executed")
            {
                parsed = markField(seen, ActiveFxRoiPathField::Executed)
                    && parseBoolean(output.executed);
            }
            else if (key == "eligible")
            {
                parsed = markField(seen, ActiveFxRoiPathField::Eligible)
                    && parseBoolean(output.eligible);
            }
            else if (key == "warmup")
            {
                parsed = markField(seen, ActiveFxRoiPathField::Warmup)
                    && parseBoolean(output.warmup);
            }
            else if (key == "actualPath")
            {
                parsed = markField(seen, ActiveFxRoiPathField::ActualPath)
                    && parseActiveFxRoiActualPath(output.actualPath);
            }
            else if (key == "decisionReason")
            {
                parsed = markField(seen, ActiveFxRoiPathField::DecisionReason)
                    && parseActiveFxRoiReason(output.decisionReason);
            }
            else if (key == "observedFrames")
            {
                parsed = markField(seen, ActiveFxRoiPathField::ObservedFrames)
                    && parseUnsigned(output.observedFrames);
            }
            else if (key == "requestedFrames")
            {
                parsed = markField(seen, ActiveFxRoiPathField::RequestedFrames)
                    && parseUnsigned(output.requestedFrames);
            }
            else if (key == "eligibleFrames")
            {
                parsed = markField(seen, ActiveFxRoiPathField::EligibleFrames)
                    && parseUnsigned(output.eligibleFrames);
            }
            else if (key == "appliedFrames")
            {
                parsed = markField(seen, ActiveFxRoiPathField::AppliedFrames)
                    && parseUnsigned(output.appliedFrames);
            }
            else if (key == "warmupFrames")
            {
                parsed = markField(seen, ActiveFxRoiPathField::WarmupFrames)
                    && parseUnsigned(output.warmupFrames);
            }
            else if (key == "fullPixels")
            {
                parsed = markField(seen, ActiveFxRoiPathField::FullPixels)
                    && parseUnsigned(output.fullPixels);
            }
            else if (key == "drawnPixels")
            {
                parsed = markField(seen, ActiveFxRoiPathField::DrawnPixels)
                    && parseUnsigned(output.drawnPixels);
            }
            else if (key == "clearedPixels")
            {
                parsed = markField(seen, ActiveFxRoiPathField::ClearedPixels)
                    && parseUnsigned(output.clearedPixels);
            }
            else if (key == "guardX")
            {
                parsed = markField(seen, ActiveFxRoiPathField::GuardX)
                    && parseUnsigned(output.guardX);
            }
            else if (key == "guardY")
            {
                parsed = markField(seen, ActiveFxRoiPathField::GuardY)
                    && parseUnsigned(output.guardY);
            }
            else if (key == "phase")
            {
                parsed = markField(seen, ActiveFxRoiPathField::Phase)
                    && parseUnsigned(output.phase);
            }
            else if (key == "dirtyRect")
            {
                parsed = markField(seen, ActiveFxRoiPathField::DirtyRect)
                    && parseActiveFxRoiRect(output.dirtyRect);
            }
            else if (key == "alignedRect")
            {
                parsed = markField(seen, ActiveFxRoiPathField::AlignedRect)
                    && parseActiveFxRoiRect(output.alignedRect);
            }
            else if (key == "gpu")
            {
                parsed = markField(seen, ActiveFxRoiPathField::Gpu)
                    && parseActiveFxRoiGpu(output.gpu);
            }
            else if (key == "reasonCounts")
            {
                parsed = markField(seen, ActiveFxRoiPathField::ReasonCounts)
                    && parseActiveFxRoiReasonCounts(output.reasonCounts);
            }
            if (!parsed)
            {
                return false;
            }

            skipWhitespace();
            if (consume('}'))
            {
                if (seen != requiredActiveFxRoiPathFields)
                {
                    return false;
                }
                return validateActiveFxRoiPath(output);
            }
            if (!consume(','))
            {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    [[nodiscard]] static bool validateActiveFxRoiPath(
        const ActiveFxRoiPathRuntimeState& path) noexcept
    {
        if (path.requestedFrames > path.observedFrames
            || path.eligibleFrames > path.requestedFrames
            || path.appliedFrames > path.eligibleFrames
            || path.warmupFrames > path.eligibleFrames)
        {
            return false;
        }
        std::uint64_t reasonTotal = 0U;
        for (const std::uint64_t count : path.reasonCounts)
        {
            if (count > (std::numeric_limits<std::uint64_t>::max)()
                    - reasonTotal)
            {
                return false;
            }
            reasonTotal += count;
        }
        return reasonTotal == path.observedFrames;
    }

    [[nodiscard]] bool parseActiveFxRoiRect(
        std::optional<ActiveFxRoiRectState>& output)
    {
        if (consumeLiteral("null"))
        {
            output.reset();
            return true;
        }
        if (!consume('{'))
        {
            return false;
        }
        skipWhitespace();

        ActiveFxRoiRectState rect{};
        std::uint64_t seen = 0U;
        while (position_ < input_.size())
        {
            std::string key;
            if (!parseString(key))
            {
                return false;
            }
            skipWhitespace();
            if (!consume(':'))
            {
                return false;
            }
            skipWhitespace();

            bool parsed = false;
            if (key == "left")
            {
                parsed = markField(seen, ActiveFxRoiRectField::Left)
                    && parseSigned(rect.left);
            }
            else if (key == "top")
            {
                parsed = markField(seen, ActiveFxRoiRectField::Top)
                    && parseSigned(rect.top);
            }
            else if (key == "right")
            {
                parsed = markField(seen, ActiveFxRoiRectField::Right)
                    && parseSigned(rect.right);
            }
            else if (key == "bottom")
            {
                parsed = markField(seen, ActiveFxRoiRectField::Bottom)
                    && parseSigned(rect.bottom);
            }
            if (!parsed)
            {
                return false;
            }

            skipWhitespace();
            if (consume('}'))
            {
                if (seen != requiredActiveFxRoiRectFields
                    || rect.right <= rect.left
                    || rect.bottom <= rect.top)
                {
                    return false;
                }
                output = rect;
                return true;
            }
            if (!consume(','))
            {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    [[nodiscard]] bool parseActiveFxRoiGpu(ActiveFxRoiGpuState& output)
    {
        if (!consume('{'))
        {
            return false;
        }
        skipWhitespace();

        std::uint64_t seen = 0U;
        while (position_ < input_.size())
        {
            std::string key;
            if (!parseString(key))
            {
                return false;
            }
            skipWhitespace();
            if (!consume(':'))
            {
                return false;
            }
            skipWhitespace();

            bool parsed = false;
            if (key == "prefilter")
            {
                parsed = markField(seen, ActiveFxRoiGpuField::Prefilter)
                    && parseActiveFxRoiGpuPercentiles(output.prefilter);
            }
            else if (key == "pyramid")
            {
                parsed = markField(seen, ActiveFxRoiGpuField::Pyramid)
                    && parseActiveFxRoiGpuPercentiles(output.pyramid);
            }
            else if (key == "finalComposite")
            {
                parsed = markField(
                        seen,
                        ActiveFxRoiGpuField::FinalComposite)
                    && parseActiveFxRoiGpuPercentiles(
                        output.finalComposite);
            }
            if (!parsed)
            {
                return false;
            }

            skipWhitespace();
            if (consume('}'))
            {
                return seen == requiredActiveFxRoiGpuFields;
            }
            if (!consume(','))
            {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    [[nodiscard]] bool parseActiveFxRoiGpuPercentiles(
        ActiveFxRoiGpuPercentileState& output)
    {
        if (!consume('{'))
        {
            return false;
        }
        skipWhitespace();

        std::uint64_t seen = 0U;
        while (position_ < input_.size())
        {
            std::string key;
            if (!parseString(key))
            {
                return false;
            }
            skipWhitespace();
            if (!consume(':'))
            {
                return false;
            }
            skipWhitespace();

            bool parsed = false;
            if (key == "p50Us")
            {
                parsed = markField(
                        seen,
                        ActiveFxRoiGpuPercentileField::P50Us)
                    && parseOptionalDouble(output.p50Microseconds);
            }
            else if (key == "p95Us")
            {
                parsed = markField(
                        seen,
                        ActiveFxRoiGpuPercentileField::P95Us)
                    && parseOptionalDouble(output.p95Microseconds);
            }
            if (!parsed)
            {
                return false;
            }

            skipWhitespace();
            if (consume('}'))
            {
                if (seen != requiredActiveFxRoiGpuPercentileFields)
                {
                    return false;
                }
                return !output.p50Microseconds.has_value()
                    || !output.p95Microseconds.has_value()
                    || *output.p50Microseconds <= *output.p95Microseconds;
            }
            if (!consume(','))
            {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    [[nodiscard]] bool parseActiveFxRoiReasonCounts(
        std::array<std::uint64_t, activeFxRoiReasonStateCount>& output)
    {
        if (!consume('{'))
        {
            return false;
        }
        skipWhitespace();

        std::uint64_t seen = 0U;
        while (position_ < input_.size())
        {
            std::string key;
            if (!parseString(key))
            {
                return false;
            }
            skipWhitespace();
            if (!consume(':'))
            {
                return false;
            }
            skipWhitespace();

            ActiveFxRoiReasonState reason{};
            if (!activeFxRoiReasonFromToken(key, reason)
                || !markField(seen, reason)
                || !parseUnsigned(
                    output[static_cast<std::size_t>(reason)]))
            {
                return false;
            }

            skipWhitespace();
            if (consume('}'))
            {
                return seen == requiredActiveFxRoiReasonFields;
            }
            if (!consume(','))
            {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    [[nodiscard]] bool parseRefreshRate(
        std::optional<DisplayRefreshState>& output)
    {
        if (consumeLiteral("null"))
        {
            output.reset();
            return true;
        }
        if (!consume('{'))
        {
            return false;
        }
        skipWhitespace();

        DisplayRefreshState refresh{};
        bool numeratorSeen = false;
        bool denominatorSeen = false;
        while (position_ < input_.size())
        {
            std::string key;
            if (!parseString(key))
            {
                return false;
            }
            skipWhitespace();
            if (!consume(':'))
            {
                return false;
            }
            skipWhitespace();

            if (key == "numerator")
            {
                if (numeratorSeen || !parseUnsigned(refresh.numerator))
                {
                    return false;
                }
                numeratorSeen = true;
            }
            else if (key == "denominator")
            {
                if (denominatorSeen || !parseUnsigned(refresh.denominator))
                {
                    return false;
                }
                denominatorSeen = true;
            }
            else
            {
                return false;
            }

            skipWhitespace();
            if (consume('}'))
            {
                if (!numeratorSeen
                    || !denominatorSeen
                    || refresh.numerator == 0U
                    || refresh.denominator == 0U)
                {
                    return false;
                }
                output = refresh;
                return true;
            }
            if (!consume(','))
            {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    [[nodiscard]] bool parsePhysicalCadence(
        std::vector<DisplayPhysicalCadenceState>& output)
    {
        if (!consume('['))
        {
            return false;
        }
        skipWhitespace();
        if (consume(']'))
        {
            return true;
        }

        while (position_ < input_.size())
        {
            if (output.size() >= maximumPhysicalTargets)
            {
                return false;
            }
            DisplayPhysicalCadenceState physical{};
            if (!parsePhysicalCadenceTarget(physical))
            {
                return false;
            }
            output.push_back(std::move(physical));
            skipWhitespace();
            if (consume(']'))
            {
                return true;
            }
            if (!consume(','))
            {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    [[nodiscard]] bool parsePhysicalCadenceTarget(
        DisplayPhysicalCadenceState& output)
    {
        if (!consume('{'))
        {
            return false;
        }
        skipWhitespace();

        std::uint64_t seen = 0U;
        while (position_ < input_.size())
        {
            std::string key;
            if (!parseString(key))
            {
                return false;
            }
            skipWhitespace();
            if (!consume(':'))
            {
                return false;
            }
            skipWhitespace();

            if (key == "virtualRefresh")
            {
                if (!markField(
                        seen,
                        PhysicalCadenceField::VirtualRefresh)
                    || !parseRefreshRate(output.virtualRefresh))
                {
                    return false;
                }
            }
            else if (key == "physicalRefresh")
            {
                if (!markField(
                        seen,
                        PhysicalCadenceField::PhysicalRefresh)
                    || !parseRefreshRate(output.physicalRefresh))
                {
                    return false;
                }
            }
            else if (key == "captureRefresh")
            {
                if (!markField(
                        seen,
                        PhysicalCadenceField::CaptureRefresh)
                    || !parseRefreshRate(output.captureRefresh))
                {
                    return false;
                }
            }
            else if (key == "drrBoosted")
            {
                if (!markField(seen, PhysicalCadenceField::DrrBoosted)
                    || !parseBoolean(output.drrBoosted))
                {
                    return false;
                }
            }
            else if (key == "available")
            {
                if (!markField(seen, PhysicalCadenceField::Available)
                    || !parseBoolean(output.available))
                {
                    return false;
                }
            }
            else
            {
                return false;
            }

            skipWhitespace();
            if (consume('}'))
            {
                return seen == requiredPhysicalCadenceFields;
            }
            if (!consume(','))
            {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    [[nodiscard]] bool parseDriver(DisplayDriverState& output)
    {
        std::string token;
        if (!parseString(token))
        {
            return false;
        }
        if (token == "hardware")
        {
            output = DisplayDriverState::Hardware;
            return true;
        }
        if (token == "warp")
        {
            output = DisplayDriverState::Warp;
            return true;
        }
        if (token == "unknown")
        {
            output = DisplayDriverState::Unknown;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parseActiveFxRoiActualPath(
        ActiveFxRoiPathState& output)
    {
        std::string token;
        if (!parseString(token))
        {
            return false;
        }
        if (token == "disabled")
        {
            output = ActiveFxRoiPathState::Disabled;
            return true;
        }
        if (token == "idle")
        {
            output = ActiveFxRoiPathState::Idle;
            return true;
        }
        if (token == "full-screen")
        {
            output = ActiveFxRoiPathState::FullScreen;
            return true;
        }
        if (token == "roi-warmup")
        {
            output = ActiveFxRoiPathState::RoiWarmup;
            return true;
        }
        if (token == "roi-prefilter")
        {
            output = ActiveFxRoiPathState::RoiPrefilter;
            return true;
        }
        if (token == "unavailable")
        {
            output = ActiveFxRoiPathState::Unavailable;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parseActiveFxRoiReason(
        ActiveFxRoiReasonState& output)
    {
        std::string token;
        return parseString(token)
            && activeFxRoiReasonFromToken(token, output);
    }

    [[nodiscard]] static bool activeFxRoiReasonFromToken(
        const std::string_view token,
        ActiveFxRoiReasonState& output) noexcept
    {
        if (token == "disabled")
        {
            output = ActiveFxRoiReasonState::Disabled;
            return true;
        }
        if (token == "no-content")
        {
            output = ActiveFxRoiReasonState::NoContent;
            return true;
        }
        if (token == "bloom-disabled")
        {
            output = ActiveFxRoiReasonState::BloomDisabled;
            return true;
        }
        if (token == "core-mode")
        {
            output = ActiveFxRoiReasonState::CoreMode;
            return true;
        }
        if (token == "background-differential-bloom")
        {
            output = ActiveFxRoiReasonState::BackgroundDifferentialBloom;
            return true;
        }
        if (token == "touches-boundary")
        {
            output = ActiveFxRoiReasonState::TouchesBoundary;
            return true;
        }
        if (token == "area-too-large")
        {
            output = ActiveFxRoiReasonState::AreaTooLarge;
            return true;
        }
        if (token == "benefit-too-small")
        {
            output = ActiveFxRoiReasonState::BenefitTooSmall;
            return true;
        }
        if (token == "context1-unavailable")
        {
            output = ActiveFxRoiReasonState::Context1Unavailable;
            return true;
        }
        if (token == "shared-target-full-write")
        {
            output = ActiveFxRoiReasonState::SharedTargetFullWrite;
            return true;
        }
        if (token == "applied")
        {
            output = ActiveFxRoiReasonState::Applied;
            return true;
        }
        if (token == "renderer-fallback")
        {
            output = ActiveFxRoiReasonState::RendererFallback;
            return true;
        }
        if (token == "unavailable")
        {
            output = ActiveFxRoiReasonState::Unavailable;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parseOutput(DisplayOutputState& output)
    {
        std::string token;
        if (!parseString(token))
        {
            return false;
        }
        if (token == "conservative-sdr")
        {
            output = DisplayOutputState::ConservativeSdr;
            return true;
        }
        if (token == "linear-scrgb")
        {
            output = DisplayOutputState::LinearScRgb;
            return true;
        }
        if (token == "unknown")
        {
            output = DisplayOutputState::Unknown;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parseCaptureCadenceStatus(
        DisplayCaptureCadenceState& output)
    {
        std::string token;
        if (!parseString(token))
        {
            return false;
        }
        if (token == "inactive")
        {
            output = DisplayCaptureCadenceState::Inactive;
            return true;
        }
        if (token == "wrong-monitor")
        {
            output = DisplayCaptureCadenceState::WrongMonitor;
            return true;
        }
        if (token == "target-rate")
        {
            output = DisplayCaptureCadenceState::TargetRate;
            return true;
        }
        if (token == "conservative-fallback")
        {
            output = DisplayCaptureCadenceState::ConservativeFallback;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parseProducerCadenceStatus(
        DisplayProducerCadenceState& output)
    {
        std::string token;
        if (!parseString(token))
        {
            return false;
        }
        if (token == "not-requested")
        {
            output = DisplayProducerCadenceState::NotRequested;
            return true;
        }
        if (token == "applied")
        {
            output = DisplayProducerCadenceState::Applied;
            return true;
        }
        if (token == "interface-unavailable")
        {
            output = DisplayProducerCadenceState::InterfaceUnavailable;
            return true;
        }
        if (token == "rejected")
        {
            output = DisplayProducerCadenceState::Rejected;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parseOutputMapping(
        DisplayOutputMappingState& output)
    {
        std::string token;
        if (!parseString(token))
        {
            return false;
        }
        if (token == "conservative-sdr")
        {
            output = DisplayOutputMappingState::ConservativeSdr;
            return true;
        }
        if (token == "advanced-color-scrgb")
        {
            output = DisplayOutputMappingState::AdvancedColorScRgb;
            return true;
        }
        if (token == "hdr-scene-referred-scrgb")
        {
            output = DisplayOutputMappingState::HdrSceneReferredScRgb;
            return true;
        }
        if (token == "unknown")
        {
            output = DisplayOutputMappingState::Unknown;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parseFramePacing(
        bafx::config::FramePacing& output)
    {
        std::string token;
        if (!parseString(token))
        {
            return false;
        }
        if (token == "match-display")
        {
            output = bafx::config::FramePacing::MatchDisplay;
            return true;
        }
        if (token == "60")
        {
            output = bafx::config::FramePacing::Fixed60;
            return true;
        }
        if (token == "120")
        {
            output = bafx::config::FramePacing::Fixed120;
            return true;
        }
        if (token == "144")
        {
            output = bafx::config::FramePacing::Fixed144;
            return true;
        }
        if (token == "unlimited")
        {
            output = bafx::config::FramePacing::Unlimited;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parseColor(DisplayColorState& output)
    {
        std::string token;
        if (!parseString(token))
        {
            return false;
        }
        if (token == "sdr")
        {
            output = DisplayColorState::Sdr;
            return true;
        }
        if (token == "wide-color-gamut")
        {
            output = DisplayColorState::WideColorGamut;
            return true;
        }
        if (token == "hdr")
        {
            output = DisplayColorState::Hdr;
            return true;
        }
        if (token == "unknown")
        {
            output = DisplayColorState::Unknown;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parseTopology(DisplayTopologyState& output)
    {
        std::string token;
        if (!parseString(token))
        {
            return false;
        }
        if (token == "complete")
        {
            output = DisplayTopologyState::Complete;
            return true;
        }
        if (token == "incomplete")
        {
            output = DisplayTopologyState::Incomplete;
            return true;
        }
        if (token == "no-active-displays")
        {
            output = DisplayTopologyState::NoActiveDisplays;
            return true;
        }
        if (token == "query-failed")
        {
            output = DisplayTopologyState::QueryFailed;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parseColorMonitorStatus(
        DisplayColorMonitorState& output)
    {
        std::string token;
        if (!parseString(token))
        {
            return false;
        }
        if (token == "active")
        {
            output = DisplayColorMonitorState::Active;
            return true;
        }
        if (token == "invalid-target")
        {
            output = DisplayColorMonitorState::InvalidTarget;
            return true;
        }
        if (token == "unsupported")
        {
            output = DisplayColorMonitorState::Unsupported;
            return true;
        }
        if (token == "failed")
        {
            output = DisplayColorMonitorState::Failed;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parseColorSnapshotDisposition(
        DisplayColorSnapshotState& output)
    {
        std::string token;
        if (!parseString(token))
        {
            return false;
        }
        if (token == "fresh")
        {
            output = DisplayColorSnapshotState::Fresh;
            return true;
        }
        if (token == "retained-transaction")
        {
            output = DisplayColorSnapshotState::RetainedTransaction;
            return true;
        }
        if (token == "retained-last-known")
        {
            output = DisplayColorSnapshotState::RetainedLastKnown;
            return true;
        }
        if (token == "unavailable")
        {
            output = DisplayColorSnapshotState::Unavailable;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parseCadenceFallback(
        DisplayCadenceFallbackState& output)
    {
        std::string token;
        if (!parseString(token))
        {
            return false;
        }
        if (token == "none")
        {
            output = DisplayCadenceFallbackState::None;
            return true;
        }
        if (token == "no-physical-targets")
        {
            output = DisplayCadenceFallbackState::NoPhysicalTargets;
            return true;
        }
        if (token == "physical-target-unavailable")
        {
            output = DisplayCadenceFallbackState::PhysicalTargetUnavailable;
            return true;
        }
        if (token == "drr-physical-refresh-rate-unavailable")
        {
            output = DisplayCadenceFallbackState::
                DrrPhysicalRefreshRateUnavailable;
            return true;
        }
        if (token == "invalid-effective-refresh-rate")
        {
            output = DisplayCadenceFallbackState::InvalidEffectiveRefreshRate;
            return true;
        }
        if (token == "mixed-clone-refresh-rates")
        {
            output = DisplayCadenceFallbackState::MixedCloneRefreshRates;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parseOutputFallback(
        DisplayOutputFallbackState& output)
    {
        std::string token;
        if (!parseString(token))
        {
            return false;
        }
        if (token == "none")
        {
            output = DisplayOutputFallbackState::None;
            return true;
        }
        if (token == "conservative-sdr")
        {
            output = DisplayOutputFallbackState::ConservativeSdr;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool parseOptionalBoolean(std::optional<bool>& output)
    {
        if (consumeLiteral("null"))
        {
            output.reset();
            return true;
        }
        bool value = false;
        if (!parseBoolean(value))
        {
            return false;
        }
        output = value;
        return true;
    }

    [[nodiscard]] bool parseOptionalString(
        std::optional<std::string>& output)
    {
        if (consumeLiteral("null"))
        {
            output.reset();
            return true;
        }
        std::string value;
        if (!parseString(value))
        {
            return false;
        }
        output = std::move(value);
        return true;
    }

    [[nodiscard]] bool parseOptionalSigned(
        std::optional<std::int32_t>& output) noexcept
    {
        if (consumeLiteral("null"))
        {
            output.reset();
            return true;
        }
        std::int32_t value = 0;
        if (!parseSigned(value))
        {
            return false;
        }
        output = value;
        return true;
    }

    [[nodiscard]] bool parseOptionalUnsigned(
        std::optional<std::uint32_t>& output) noexcept
    {
        if (consumeLiteral("null"))
        {
            output.reset();
            return true;
        }
        std::uint32_t value = 0U;
        if (!parseUnsigned(value))
        {
            return false;
        }
        output = value;
        return true;
    }

    [[nodiscard]] bool parseOptionalFloat(
        std::optional<float>& output) noexcept
    {
        if (consumeLiteral("null"))
        {
            output.reset();
            return true;
        }
        const std::size_t begin = position_;
        if (!scanNumber())
        {
            return false;
        }
        float value = 0.0F;
        const auto result = std::from_chars(
            input_.data() + begin,
            input_.data() + position_,
            value,
            std::chars_format::general);
        if (result.ec != std::errc{}
            || result.ptr != input_.data() + position_
            || !std::isfinite(value)
            || value < 0.0F)
        {
            return false;
        }
        output = value;
        return true;
    }

    [[nodiscard]] bool parseOptionalDouble(
        std::optional<double>& output) noexcept
    {
        if (consumeLiteral("null"))
        {
            output.reset();
            return true;
        }
        const std::size_t begin = position_;
        if (!scanNumber())
        {
            return false;
        }
        double value = 0.0;
        const auto result = std::from_chars(
            input_.data() + begin,
            input_.data() + position_,
            value,
            std::chars_format::general);
        if (result.ec != std::errc{}
            || result.ptr != input_.data() + position_
            || !std::isfinite(value)
            || value < 0.0)
        {
            return false;
        }
        output = value;
        return true;
    }

    [[nodiscard]] bool parseBoolean(bool& output) noexcept
    {
        if (consumeLiteral("true"))
        {
            output = true;
            return true;
        }
        if (consumeLiteral("false"))
        {
            output = false;
            return true;
        }
        return false;
    }

    template <typename Integer>
    [[nodiscard]] bool parseUnsigned(Integer& output) noexcept
    {
        const std::size_t begin = position_;
        if (!scanIntegerDigits())
        {
            return false;
        }

        std::uint64_t parsed = 0U;
        const auto result = std::from_chars(
            input_.data() + begin,
            input_.data() + position_,
            parsed);
        if (result.ec != std::errc{}
            || result.ptr != input_.data() + position_
            || parsed > static_cast<std::uint64_t>(
                (std::numeric_limits<Integer>::max)()))
        {
            return false;
        }
        output = static_cast<Integer>(parsed);
        return true;
    }

    [[nodiscard]] bool parseSigned(std::int32_t& output) noexcept
    {
        const std::size_t begin = position_;
        if (position_ < input_.size() && input_[position_] == '-')
        {
            ++position_;
        }
        if (!scanIntegerDigits())
        {
            return false;
        }

        std::int64_t parsed = 0;
        const auto result = std::from_chars(
            input_.data() + begin,
            input_.data() + position_,
            parsed);
        if (result.ec != std::errc{}
            || result.ptr != input_.data() + position_
            || parsed < (std::numeric_limits<std::int32_t>::min)()
            || parsed > (std::numeric_limits<std::int32_t>::max)())
        {
            return false;
        }
        output = static_cast<std::int32_t>(parsed);
        return true;
    }

    [[nodiscard]] bool scanIntegerDigits() noexcept
    {
        const std::size_t begin = position_;
        if (position_ >= input_.size()
            || input_[position_] < '0'
            || input_[position_] > '9')
        {
            return false;
        }
        if (input_[position_] == '0')
        {
            ++position_;
            return position_ >= input_.size()
                || input_[position_] < '0'
                || input_[position_] > '9';
        }
        while (position_ < input_.size()
            && input_[position_] >= '0'
            && input_[position_] <= '9')
        {
            ++position_;
        }
        return position_ > begin;
    }

    [[nodiscard]] bool scanNumber() noexcept
    {
        if (position_ < input_.size() && input_[position_] == '-')
        {
            ++position_;
        }
        if (!scanIntegerDigits())
        {
            return false;
        }
        if (position_ < input_.size() && input_[position_] == '.')
        {
            ++position_;
            const std::size_t fractionalBegin = position_;
            while (position_ < input_.size()
                && input_[position_] >= '0'
                && input_[position_] <= '9')
            {
                ++position_;
            }
            if (position_ == fractionalBegin)
            {
                return false;
            }
        }
        if (position_ < input_.size()
            && (input_[position_] == 'e' || input_[position_] == 'E'))
        {
            ++position_;
            if (position_ < input_.size()
                && (input_[position_] == '+' || input_[position_] == '-'))
            {
                ++position_;
            }
            const std::size_t exponentBegin = position_;
            while (position_ < input_.size()
                && input_[position_] >= '0'
                && input_[position_] <= '9')
            {
                ++position_;
            }
            if (position_ == exponentBegin)
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool parseString(std::string& output)
    {
        if (!consume('"'))
        {
            return false;
        }
        output.clear();
        while (position_ < input_.size())
        {
            const unsigned char character = static_cast<unsigned char>(
                input_[position_++]);
            if (character == '"')
            {
                return output.size() <= maximumStringBytes;
            }
            if (character < 0x20U)
            {
                return false;
            }
            if (character != '\\')
            {
                output.push_back(static_cast<char>(character));
            }
            else if (!parseEscape(output))
            {
                return false;
            }
            if (output.size() > maximumStringBytes)
            {
                return false;
            }
        }
        return false;
    }

    [[nodiscard]] bool parseEscape(std::string& output)
    {
        if (position_ >= input_.size())
        {
            return false;
        }
        const char escape = input_[position_++];
        switch (escape)
        {
        case '"':
        case '\\':
        case '/':
            output.push_back(escape);
            return true;
        case 'b':
            output.push_back('\b');
            return true;
        case 'f':
            output.push_back('\f');
            return true;
        case 'n':
            output.push_back('\n');
            return true;
        case 'r':
            output.push_back('\r');
            return true;
        case 't':
            output.push_back('\t');
            return true;
        case 'u':
            return parseUnicodeEscape(output);
        default:
            return false;
        }
    }

    [[nodiscard]] bool parseUnicodeEscape(std::string& output)
    {
        std::uint32_t codePoint = 0U;
        if (!parseHexQuad(codePoint))
        {
            return false;
        }
        if (codePoint >= 0xD800U && codePoint <= 0xDBFFU)
        {
            if (position_ + 2U > input_.size()
                || input_[position_] != '\\'
                || input_[position_ + 1U] != 'u')
            {
                return false;
            }
            position_ += 2U;
            std::uint32_t lowSurrogate = 0U;
            if (!parseHexQuad(lowSurrogate)
                || lowSurrogate < 0xDC00U
                || lowSurrogate > 0xDFFFU)
            {
                return false;
            }
            codePoint = 0x10000U
                + ((codePoint - 0xD800U) << 10U)
                + (lowSurrogate - 0xDC00U);
        }
        else if (codePoint >= 0xDC00U && codePoint <= 0xDFFFU)
        {
            return false;
        }
        appendUtf8(output, codePoint);
        return true;
    }

    [[nodiscard]] bool parseHexQuad(std::uint32_t& output) noexcept
    {
        if (position_ + 4U > input_.size())
        {
            return false;
        }
        output = 0U;
        for (std::size_t index = 0U; index < 4U; ++index)
        {
            const char character = input_[position_++];
            std::uint32_t nibble = 0U;
            if (character >= '0' && character <= '9')
            {
                nibble = static_cast<std::uint32_t>(character - '0');
            }
            else if (character >= 'a' && character <= 'f')
            {
                nibble = static_cast<std::uint32_t>(character - 'a') + 10U;
            }
            else if (character >= 'A' && character <= 'F')
            {
                nibble = static_cast<std::uint32_t>(character - 'A') + 10U;
            }
            else
            {
                return false;
            }
            output = (output << 4U) | nibble;
        }
        return true;
    }

    static void appendUtf8(std::string& output, const std::uint32_t codePoint)
    {
        if (codePoint <= 0x7FU)
        {
            output.push_back(static_cast<char>(codePoint));
        }
        else if (codePoint <= 0x7FFU)
        {
            output.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
        else if (codePoint <= 0xFFFFU)
        {
            output.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
            output.push_back(static_cast<char>(
                0x80U | ((codePoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
        else
        {
            output.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
            output.push_back(static_cast<char>(
                0x80U | ((codePoint >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(
                0x80U | ((codePoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
    }

    template <typename Field>
    [[nodiscard]] static bool markField(
        std::uint64_t& seen,
        const Field field) noexcept
    {
        const std::uint32_t index = static_cast<std::uint32_t>(field);
        const std::uint64_t mask = 1ULL << index;
        if ((seen & mask) != 0U)
        {
            return false;
        }
        seen |= mask;
        return true;
    }

    void skipWhitespace() noexcept
    {
        while (position_ < input_.size())
        {
            const char character = input_[position_];
            if (character != ' '
                && character != '\t'
                && character != '\r'
                && character != '\n')
            {
                break;
            }
            ++position_;
        }
    }

    [[nodiscard]] bool consume(const char expected) noexcept
    {
        if (position_ >= input_.size() || input_[position_] != expected)
        {
            return false;
        }
        ++position_;
        return true;
    }

    [[nodiscard]] bool consumeLiteral(const std::string_view literal) noexcept
    {
        if (input_.substr(position_, literal.size()) != literal)
        {
            return false;
        }
        position_ += literal.size();
        return true;
    }

    [[nodiscard]] DisplayStateParseResult failure(
        const std::string_view message) const
    {
        DisplayStateParseResult result{};
        result.error = std::string(message)
            + " at byte " + std::to_string(position_);
        return result;
    }

    std::string_view input_{};
    std::size_t position_{0U};
};

}

DisplayStateParseResult parseDisplayState(const std::string_view json) noexcept
{
    try
    {
        return DisplayStateJsonParser(json).parse();
    }
    catch (...)
    {
        DisplayStateParseResult result{};
        result.error = "display state parser could not allocate memory";
        return result;
    }
}

}
