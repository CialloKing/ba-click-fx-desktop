#include "control_center_display.hpp"
#include "control_center_window.hpp"
#include "control_updates.hpp"
#include "config_commands.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <locale>
#include <sstream>
#include <utility>

namespace bafx::control_center
{

[[nodiscard]] std::wstring hresultText(const HRESULT result)
{
    std::wostringstream stream;
    stream << L"0x"
           << std::uppercase
           << std::hex
           << std::setw(8)
           << std::setfill(L'0')
           << static_cast<unsigned long>(result);
    return stream.str();
}

[[nodiscard]] std::optional<bafx::config::FramePacing> selectedFramePacing(
    const HWND comboBox) noexcept
{
    if (comboBox == nullptr)
    {
        return std::nullopt;
    }

    switch (SendMessageW(comboBox, CB_GETCURSEL, 0U, 0))
    {
    case 0:
        return bafx::config::FramePacing::MatchDisplay;
    case 1:
        return bafx::config::FramePacing::Fixed60;
    case 2:
        return bafx::config::FramePacing::Fixed120;
    case 3:
        return bafx::config::FramePacing::Fixed144;
    case 4:
        return bafx::config::FramePacing::Unlimited;
    default:
        return std::nullopt;
    }
}

namespace
{

constexpr std::size_t offlineDisplayItemBase = 1U << 16U;

[[nodiscard]] int framePacingIndex(
    const bafx::config::FramePacing pacing) noexcept
{
    switch (pacing)
    {
    case bafx::config::FramePacing::MatchDisplay:
        return 0;
    case bafx::config::FramePacing::Fixed60:
        return 1;
    case bafx::config::FramePacing::Fixed120:
        return 2;
    case bafx::config::FramePacing::Fixed144:
        return 3;
    case bafx::config::FramePacing::Unlimited:
        return 4;
    }
    return -1;
}

[[nodiscard]] std::wstring framePacingText(
    const bafx::config::FramePacing pacing)
{
    switch (pacing)
    {
    case bafx::config::FramePacing::MatchDisplay:
        return tr(TextId::MatchDisplay);
    case bafx::config::FramePacing::Fixed60:
        return tr(TextId::Fixed60);
    case bafx::config::FramePacing::Fixed120:
        return tr(TextId::Fixed120);
    case bafx::config::FramePacing::Fixed144:
        return tr(TextId::Fixed144);
    case bafx::config::FramePacing::Unlimited:
        return tr(TextId::UnlimitedFps);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::string displaySessionIdentity(
    const DisplaySessionState& session)
{
    if (session.displayKey.has_value())
    {
        return "stable:" + *session.displayKey;
    }

    // A transient identity keeps the same row selected while the Host state
    // refreshes. It must never be used as a persisted configuration key.
    return "transient:" + session.device + '\n' + session.monitor;
}

[[nodiscard]] std::string offlineDisplayIdentity(
    const bafx::config::DisplayOverrideConfig& overrideConfig)
{
    return "stable:" + overrideConfig.displayKey;
}

[[nodiscard]] std::wstring driverStateText(
    const DisplayDriverState driver)
{
    switch (driver)
    {
    case DisplayDriverState::Hardware:
        return tr(TextId::Hardware);
    case DisplayDriverState::Warp:
        return tr(TextId::Warp);
    case DisplayDriverState::Unknown:
        return tr(TextId::Unknown);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring outputStateText(
    const DisplayOutputState output)
{
    switch (output)
    {
    case DisplayOutputState::ConservativeSdr:
        return tr(TextId::ConservativeSdr);
    case DisplayOutputState::LinearScRgb:
        return tr(TextId::LinearScrgb);
    case DisplayOutputState::Unknown:
        return tr(TextId::Unknown);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring colorStateText(const DisplayColorState color)
{
    switch (color)
    {
    case DisplayColorState::Sdr:
        return L"SDR";
    case DisplayColorState::WideColorGamut:
        return tr(TextId::WideGamut);
    case DisplayColorState::Hdr:
        return L"HDR";
    case DisplayColorState::Unknown:
        return tr(TextId::Unknown);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring optionalBooleanText(
    const std::optional<bool> value,
    const std::wstring_view trueText,
    const std::wstring_view falseText)
{
    if (!value.has_value())
    {
        return tr(TextId::Unknown);
    }
    return std::wstring(*value ? trueText : falseText);
}

[[nodiscard]] std::wstring refreshRateText(
    const std::optional<DisplayRefreshState>& refresh)
{
    if (!refresh.has_value()
        || refresh->numerator == 0U
        || refresh->denominator == 0U)
    {
        return tr(TextId::Unknown);
    }

    const double hertz = static_cast<double>(refresh->numerator)
        / static_cast<double>(refresh->denominator);
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(2) << hertz << L" Hz";
    return stream.str();
}

[[nodiscard]] std::wstring topologyStateText(
    const DisplayTopologyState state)
{
    switch (state)
    {
    case DisplayTopologyState::Complete:
        return tr(TextId::Complete);
    case DisplayTopologyState::Incomplete:
        return tr(TextId::Incomplete);
    case DisplayTopologyState::NoActiveDisplays:
        return tr(TextId::NoActiveDisplays);
    case DisplayTopologyState::QueryFailed:
        return tr(TextId::QueryFailed);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring colorMonitorStateText(
    const DisplayColorMonitorState state)
{
    switch (state)
    {
    case DisplayColorMonitorState::Active:
        return tr(TextId::Active);
    case DisplayColorMonitorState::InvalidTarget:
        return tr(TextId::InvalidTarget);
    case DisplayColorMonitorState::Unsupported:
        return tr(TextId::UnsupportedSystem);
    case DisplayColorMonitorState::Failed:
        return tr(TextId::Failed);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring colorSnapshotStateText(
    const DisplayColorSnapshotState state)
{
    switch (state)
    {
    case DisplayColorSnapshotState::Fresh:
        return tr(TextId::LatestCompleteContract);
    case DisplayColorSnapshotState::RetainedTransaction:
        return tr(TextId::RetainedInTransaction);
    case DisplayColorSnapshotState::RetainedLastKnown:
        return tr(TextId::LastCompleteContract);
    case DisplayColorSnapshotState::Unavailable:
        return tr(TextId::Unavailable);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring cadenceFallbackText(
    const DisplayCadenceFallbackState state)
{
    switch (state)
    {
    case DisplayCadenceFallbackState::None:
        return tr(TextId::None);
    case DisplayCadenceFallbackState::NoPhysicalTargets:
        return tr(TextId::NoPhysicalTargets);
    case DisplayCadenceFallbackState::PhysicalTargetUnavailable:
        return tr(TextId::PhysicalTargetUnavailable);
    case DisplayCadenceFallbackState::DrrPhysicalRefreshRateUnavailable:
        return tr(TextId::DrrRateUnavailable);
    case DisplayCadenceFallbackState::InvalidEffectiveRefreshRate:
        return tr(TextId::InvalidEffectiveRate);
    case DisplayCadenceFallbackState::MixedCloneRefreshRates:
        return tr(TextId::CloneRateConflict);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring captureCadenceText(
    const DisplayCaptureCadenceState state)
{
    switch (state)
    {
    case DisplayCaptureCadenceState::Inactive:
        return tr(TextId::Inactive);
    case DisplayCaptureCadenceState::WrongMonitor:
        return tr(TextId::CaptureTargetMismatch);
    case DisplayCaptureCadenceState::TargetRate:
        return tr(TextId::TargetRefreshRate);
    case DisplayCaptureCadenceState::ConservativeFallback:
        return tr(TextId::ConservativeFallback);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring producerCadenceText(
    const DisplayProducerCadenceState state)
{
    switch (state)
    {
    case DisplayProducerCadenceState::NotRequested:
        return tr(TextId::NotRequested);
    case DisplayProducerCadenceState::Applied:
        return tr(TextId::Applied);
    case DisplayProducerCadenceState::InterfaceUnavailable:
        return tr(TextId::InterfaceUnavailable);
    case DisplayProducerCadenceState::Rejected:
        return tr(TextId::SystemDenied);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring outputMappingText(
    const DisplayOutputMappingState state)
{
    switch (state)
    {
    case DisplayOutputMappingState::ConservativeSdr:
        return tr(TextId::ConservativeSdr);
    case DisplayOutputMappingState::AdvancedColorScRgb:
        return L"Advanced Color scRGB";
    case DisplayOutputMappingState::HdrSceneReferredScRgb:
        return L"HDR scene-referred scRGB";
    case DisplayOutputMappingState::Unknown:
        return tr(TextId::Unknown);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring outputFallbackText(
    const DisplayOutputFallbackState state)
{
    switch (state)
    {
    case DisplayOutputFallbackState::None:
        return tr(TextId::None);
    case DisplayOutputFallbackState::ConservativeSdr:
        return tr(TextId::SdrFallback);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring optionalHresultText(
    const std::optional<std::int32_t> result)
{
    if (!result.has_value())
    {
        return tr(TextId::NotQueried);
    }
    return hresultText(static_cast<HRESULT>(*result));
}

[[nodiscard]] std::wstring optionalNitsText(
    const std::optional<float> nits)
{
    if (!nits.has_value())
    {
        return tr(TextId::Unknown);
    }

    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(2) << *nits << L" nits";
    return stream.str();
}

[[nodiscard]] std::wstring activeFxRoiPathText(
    const ActiveFxRoiPathState path)
{
    switch (path)
    {
    case ActiveFxRoiPathState::Disabled:
        return tr(TextId::TurnedOff);
    case ActiveFxRoiPathState::Idle:
        return tr(TextId::Idle);
    case ActiveFxRoiPathState::FullScreen:
        return tr(TextId::FullScreenBloom);
    case ActiveFxRoiPathState::RoiWarmup:
        return tr(TextId::RoiWarmup);
    case ActiveFxRoiPathState::RoiPrefilter:
        return tr(TextId::RoiFirstLevel);
    case ActiveFxRoiPathState::RoiPyramid:
        return tr(TextId::RoiFullPyramid);
    case ActiveFxRoiPathState::Unavailable:
        return tr(TextId::Unavailable);
    }
    return tr(TextId::Unavailable);
}

[[nodiscard]] std::wstring activeFxRoiReasonText(
    const ActiveFxRoiReasonState reason)
{
    switch (reason)
    {
    case ActiveFxRoiReasonState::Disabled:
        return tr(TextId::SwitchOff);
    case ActiveFxRoiReasonState::NoContent:
        return tr(TextId::NoVisibleEffects);
    case ActiveFxRoiReasonState::BloomDisabled:
        return tr(TextId::BloomOff);
    case ActiveFxRoiReasonState::CoreMode:
        return tr(TextId::CoreMode);
    case ActiveFxRoiReasonState::BackgroundDifferentialBloom:
        return tr(TextId::DifferentialBloomFullScreen);
    case ActiveFxRoiReasonState::TouchesBoundary:
        return tr(TextId::EffectTouchesEdge);
    case ActiveFxRoiReasonState::AreaTooLarge:
        return tr(TextId::RoiTooLarge);
    case ActiveFxRoiReasonState::BenefitTooSmall:
        return tr(TextId::SmallExpectedBenefit);
    case ActiveFxRoiReasonState::Context1Unavailable:
        return tr(TextId::Context1Unavailable);
    case ActiveFxRoiReasonState::SharedTargetFullWrite:
        return tr(TextId::SharedTargetFullWrite);
    case ActiveFxRoiReasonState::Applied:
        return tr(TextId::Applied);
    case ActiveFxRoiReasonState::RendererFallback:
        return tr(TextId::RendererFallback);
    case ActiveFxRoiReasonState::Unavailable:
    case ActiveFxRoiReasonState::Count:
        return tr(TextId::DiagnosticsUnavailable);
    }
    return tr(TextId::DiagnosticsUnavailable);
}

[[nodiscard]] std::wstring activeFxRoiRectText(
    const std::optional<ActiveFxRoiRectState>& rect)
{
    if (!rect.has_value())
    {
        return tr(TextId::None);
    }
    std::wostringstream stream;
    stream << L"[" << rect->left << L", " << rect->top
           << L" - " << rect->right << L", " << rect->bottom << L"]";
    return stream.str();
}

[[nodiscard]] std::wstring activeFxRoiGpuValueText(
    const std::optional<double> value)
{
    if (!value.has_value())
    {
        return L"--";
    }
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(2) << *value;
    return stream.str();
}

void appendActiveFxRoiGpuStage(
    std::wostringstream& stream,
    const std::wstring_view label,
    const ActiveFxRoiGpuPercentileState& stage)
{
    stream << label << L" "
           << activeFxRoiGpuValueText(stage.p50Microseconds)
           << L"/"
           << activeFxRoiGpuValueText(stage.p95Microseconds);
}

[[nodiscard]] std::wstring activeFxRoiPixelRatioText(
    const std::uint64_t drawnPixels,
    const std::uint64_t fullPixels)
{
    if (fullPixels == 0U)
    {
        return L"--";
    }
    const double ratio = static_cast<double>(drawnPixels)
        * 100.0
        / static_cast<double>(fullPixels);
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(1) << ratio << L"%";
    return stream.str();
}

void appendActiveFxRoiStageDetails(
    std::wostringstream& details,
    const std::wstring_view label,
    const ActiveFxRoiStageState& stage)
{
    details << L"\r\n  " << label << tr(TextId::RoiPixelColumns)
            << stage.fullPixels << L"/"
            << stage.candidatePixels << L"/"
            << stage.drawnPixels << L"/"
            << stage.clearedPixels
            << tr(TextId::DrawnColumn)
            << activeFxRoiPixelRatioText(
                stage.drawnPixels,
                stage.fullPixels);
}

void appendActiveFxRoiPathDetails(
    std::wostringstream& details,
    const std::wstring_view label,
    const ActiveFxRoiPathRuntimeState& path)
{
    details << L"\r\n[" << label << L"] "
            << activeFxRoiPathText(path.actualPath)
            << L" | " << activeFxRoiReasonText(path.decisionReason)
            << tr(TextId::RoiCurrentColumns)
            << (path.requested ? tr(TextId::BooleanYes) : tr(TextId::BooleanNo)) << L"/"
            << (path.executed ? tr(TextId::BooleanYes) : tr(TextId::BooleanNo)) << L"/"
            << (path.eligible ? tr(TextId::BooleanYes) : tr(TextId::BooleanNo)) << L"/"
            << (path.warmup ? tr(TextId::BooleanYes) : tr(TextId::BooleanNo))
            << tr(TextId::RoiFrameColumns)
            << path.observedFrames << L"/"
            << path.requestedFrames << L"/"
            << path.eligibleFrames << L"/"
            << path.appliedFrames << L"/"
            << path.warmupFrames << L"/"
            << path.fallbackFrames
            << tr(TextId::BloomPixelColumns)
            << path.fullPixels << L"/"
            << path.candidatePixels << L"/"
            << path.drawnPixels << L"/"
            << path.clearedPixels
            << tr(TextId::DrawRatioColumn)
            << activeFxRoiPixelRatioText(
                path.drawnPixels,
                path.fullPixels);
    appendActiveFxRoiStageDetails(
        details,
        L"Prefilter",
        path.stages.prefilter);
    appendActiveFxRoiStageDetails(
        details,
        L"Downsample",
        path.stages.downsample);
    appendActiveFxRoiStageDetails(
        details,
        L"Upsample",
        path.stages.upsample);
    appendActiveFxRoiStageDetails(details, L"Resolve", path.stages.resolve);
    details << L"\r\nDirty " << activeFxRoiRectText(path.dirtyRect)
            << L" | Aligned " << activeFxRoiRectText(path.alignedRect)
            << L"\r\nGuard " << path.guardX << L" x " << path.guardY
            << L" | Phase " << path.phase
            << L"\r\nGPU us p50/p95：";
    appendActiveFxRoiGpuStage(details, L"Prefilter", path.gpu.prefilter);
    details << L" | ";
    appendActiveFxRoiGpuStage(details, L"Pyramid", path.gpu.pyramid);
    details << L" | ";
    appendActiveFxRoiGpuStage(
        details,
        L"Final",
        path.gpu.finalComposite);

    details << tr(TextId::ReasonCountsLabel);
    bool hasReason = false;
    for (std::size_t index = 0U;
         index < path.reasonCounts.size();
         ++index)
    {
        if (path.reasonCounts[index] == 0U)
        {
            continue;
        }
        if (hasReason)
        {
            details << L"；";
        }
        details << activeFxRoiReasonText(
            static_cast<ActiveFxRoiReasonState>(index))
                << L" " << path.reasonCounts[index];
        hasReason = true;
    }
    if (!hasReason)
    {
        details << tr(TextId::None);
    }
}

}

void ControlCenterWindow::updateDisplayControls(
    const bafx::config::Config& config)
{
    setChecked(hdrEnabled_, config.display.hdrEnabled);
    setComboSelection(framePacing_, framePacingIndex(config.performance.framePacing));

    std::vector<ComboItem> items;
    if (!displayStateError_.empty()
        || (displayState_.sessions.empty()
            && displayState_.offlineOverrides.empty()))
    {
        static_cast<void>(updateComboItems(displaySelector_, items));
        updateDisplayPolicyControls();
        updateDisplayDetails();
        return;
    }

    LRESULT selectedIndex = CB_ERR;
    LRESULT primaryIndex = CB_ERR;
    LRESULT coordinatorIndex = CB_ERR;
    for (std::size_t index = 0U;
         index < displayState_.sessions.size();
         ++index)
    {
        const DisplaySessionState& session = displayState_.sessions[index];
        std::wstring label = utf8ToWide(session.device);
        if (!session.monitor.empty())
        {
            label += L" | " + utf8ToWide(session.monitor);
        }
        if (session.primary && session.coordinator)
        {
            label += tr(TextId::PrimaryCoordinatorSuffix);
        }
        else if (session.primary)
        {
            label += tr(TextId::PrimaryDisplaySuffix);
        }
        else if (session.coordinator)
        {
            label += tr(TextId::CoordinatorSuffix);
        }

        const auto comboIndex = static_cast<LRESULT>(items.size());
        items.push_back({std::move(label), static_cast<LPARAM>(index)});

        if (displaySessionIdentity(session) == selectedDisplayIdentity_)
        {
            selectedIndex = comboIndex;
        }
        if (session.primary && primaryIndex == CB_ERR)
        {
            primaryIndex = comboIndex;
        }
        if (session.coordinator && coordinatorIndex == CB_ERR)
        {
            coordinatorIndex = comboIndex;
        }
    }

    for (std::size_t index = 0U;
         index < displayState_.offlineOverrides.size();
         ++index)
    {
        const bafx::config::DisplayOverrideConfig& overrideConfig =
            displayState_.offlineOverrides[index];
        const std::wstring label = tr(TextId::OfflineOverridePrefix)
            + utf8ToWide(overrideConfig.displayKey);
        const auto comboIndex = static_cast<LRESULT>(items.size());
        // Offline policies keep a separate item-data range from active sessions.
        items.push_back({label, static_cast<LPARAM>(offlineDisplayItemBase + index)});

        if (offlineDisplayIdentity(overrideConfig)
            == selectedDisplayIdentity_)
        {
            selectedIndex = comboIndex;
        }
    }

    if (!updateComboItems(displaySelector_, items))
    {
        displayStateError_ = TextId::DisplayAllocationFailed;
        updateDisplayPolicyControls();
        updateDisplayDetails();
        return;
    }
    if (selectedIndex == CB_ERR)
    {
        selectedIndex = primaryIndex != CB_ERR
            ? primaryIndex
            : coordinatorIndex;
    }
    if (selectedIndex == CB_ERR)
    {
        selectedIndex = 0;
    }
    setComboSelection(displaySelector_, selectedIndex);
    updateDisplayPolicyControls();
    updateDisplayDetails();
}

void ControlCenterWindow::updateDisplayPolicyControls() noexcept
{
    const bool wasUpdating = updatingControls_;
    updatingControls_ = true;

    const DisplaySessionState* const session = selectedDisplaySession();
    const bafx::config::DisplayOverrideConfig* const offlineOverride =
        selectedOfflineDisplayOverride();
    const std::string* displayKey = nullptr;
    if (session != nullptr && session->displayKey.has_value())
    {
        displayKey = &*session->displayKey;
    }
    else if (offlineOverride != nullptr)
    {
        displayKey = &offlineOverride->displayKey;
    }

    const bafx::config::DisplayOverrideConfig* overrideConfig = nullptr;
    if (offlineOverride != nullptr)
    {
        overrideConfig = offlineOverride;
    }
    else if (displayKey != nullptr)
    {
        overrideConfig = bafx::config::findDisplayOverride(
            config_.display,
            *displayKey);
    }

    const bool independent = overrideConfig != nullptr;
    bafx::config::ResolvedDisplayPolicy policy = displayKey != nullptr
        ? bafx::config::resolveDisplayPolicy(config_, *displayKey)
        : bafx::config::resolveDisplayPolicy(config_, {});
    if (offlineOverride != nullptr)
    {
        // Offline entries come from the Host's authoritative schema-4 list.
        // Do not replace that runtime fact with a separately fetched config.
        policy.enabled = offlineOverride->enabled;
        policy.hdrEnabled = offlineOverride->hdrEnabled;
        policy.framePacing = offlineOverride->framePacing;
        policy.overridden = true;
    }
    setChecked(displayIndependent_, independent);
    setChecked(displayEffectsEnabled_, policy.enabled);
    setChecked(displayHdrEnabled_, policy.hdrEnabled);
    setComboSelection(displayFramePacing_, framePacingIndex(policy.framePacing));

    const bool canWrite = connected_ && hostSnapshotCurrent_ && displayStateCurrent_
        && displayKey != nullptr;
    setControlEnabled(displayIndependent_, canWrite ? TRUE : FALSE);
    const BOOL policyEnabled = canWrite
            && independent
            && offlineOverride == nullptr
        ? TRUE
        : FALSE;
    setControlEnabled(displayEffectsEnabled_, policyEnabled);
    setControlEnabled(displayHdrEnabled_, policyEnabled);
    setControlEnabled(displayFramePacingLabel_, policyEnabled);
    setControlEnabled(displayFramePacing_, policyEnabled);

    updatingControls_ = wasUpdating;
}

void ControlCenterWindow::updateDisplayDetails()
{
    updateActiveFxRoiDetails();
    if (!displayStateError_.empty())
    {
        setText(displaySummaryText_, TextId::DisplayStatusUnavailable);
        setText(displayDetailsText_, displayStateError_.render().c_str());
        return;
    }

    std::wostringstream summary;
    summary << tr(TextId::TopologyLabel) << topologyStateText(displayState_.topologyStatus)
            << tr(TextId::SessionsColumn) << displayState_.sessions.size()
            << tr(TextId::OfflineColumn) << displayState_.offlineOverrides.size()
            << tr(TextId::GenerationLine) << displayState_.runtimeGeneration
            << L" / " << displayState_.configGeneration
            << L" / " << displayState_.appliedConfigGeneration;
    setText(displaySummaryText_, summary.str().c_str());

    const DisplaySessionState* const selectedSession = selectedDisplaySession();
    const bafx::config::DisplayOverrideConfig* const offlineOverride =
        selectedOfflineDisplayOverride();
    if (selectedSession == nullptr && offlineOverride == nullptr)
    {
        setText(
            displayDetailsText_,
            displayState_.sessions.empty()
                    && displayState_.offlineOverrides.empty()
                ? TextId::NoDisplaySessions
                : TextId::SelectDisplayHint);
        return;
    }

    if (offlineOverride != nullptr)
    {
        selectedDisplayIdentity_ = offlineDisplayIdentity(*offlineOverride);
        std::wostringstream details;
        details << tr(TextId::GlobalTopologyLabel)
                << topologyStateText(displayState_.topologyStatus)
                << tr(TextId::ErrorColumn)
                << hresultText(static_cast<HRESULT>(
                    displayState_.topologyError))
                << tr(TextId::OfflineAuthoritativeColumn)
                << tr(TextId::OfflineDisplayKeyLine)
                << utf8ToWide(offlineOverride->displayKey)
                << tr(TextId::EffectsLine)
                << (offlineOverride->enabled ? tr(TextId::On) : tr(TextId::Off))
                << tr(TextId::HdrRequestColumn)
                << (offlineOverride->hdrEnabled ? tr(TextId::On) : tr(TextId::Off))
                << L" | "
                << framePacingText(offlineOverride->framePacing)
                << tr(TextId::OfflineDisplayHint);
        setText(displayDetailsText_, details.str().c_str());
        return;
    }

    const DisplaySessionState& session = *selectedSession;
    selectedDisplayIdentity_ = displaySessionIdentity(session);

    const std::int64_t width = static_cast<std::int64_t>(session.right)
        - static_cast<std::int64_t>(session.left);
    const std::int64_t height = static_cast<std::int64_t>(session.bottom)
        - static_cast<std::int64_t>(session.top);
    const std::wstring role = session.primary
        ? (session.coordinator ? tr(TextId::PrimaryCoordinatorRole) : tr(TextId::PrimaryDisplayRole))
        : (session.coordinator ? tr(TextId::CoordinatorRole) : tr(TextId::ExtendedDisplayRole));
    const std::wstring captureState = session.backgroundCaptureActive
        ? tr(TextId::Active)
        : tr(TextId::Inactive);
    const std::wstring restartState = session.backgroundCaptureRestartAllowed
        ? tr(TextId::Allowed)
        : tr(TextId::Disallowed);
    const bafx::config::ResolvedDisplayPolicy policy =
        session.displayKey.has_value()
        ? bafx::config::resolveDisplayPolicy(config_, *session.displayKey)
        : bafx::config::resolveDisplayPolicy(config_, {});
    const std::wstring policySource = policy.overridden
        ? tr(TextId::IndependentPolicy)
        : (session.displayKey.has_value() ? tr(TextId::InheritedPolicy) : tr(TextId::InheritedUnstablePolicy));
    const std::wstring sourceId = session.sourceId.has_value()
        ? std::to_wstring(*session.sourceId)
        : tr(TextId::Unknown);

    std::wstring faultState;
    if (!session.renderFaulted && !session.outputContractFaulted)
    {
        faultState = tr(TextId::None);
    }
    else
    {
        if (session.renderFaulted)
        {
            faultState = tr(TextId::RenderingFault);
        }
        if (session.outputContractFaulted)
        {
            if (!faultState.empty())
            {
                faultState += L"、";
            }
            faultState += tr(TextId::OutputContractFault);
        }
    }

    std::wostringstream details;
    details << tr(TextId::GlobalTopologyLabel)
            << topologyStateText(displayState_.topologyStatus)
            << tr(TextId::ErrorColumn)
            << hresultText(static_cast<HRESULT>(
                displayState_.topologyError))
            << tr(TextId::OfflineListColumn)
            << (displayState_.offlineOverridesAuthoritative
                ? tr(TextId::Authoritative)
                : tr(TextId::TopologyRecoveryPending))
            << tr(TextId::DeviceLine) << utf8ToWide(session.device)
            << L" | " << utf8ToWide(session.monitor)
            << tr(TextId::RoleLine) << role
            << tr(TextId::DisplayKeyColumn)
            << (session.displayKey.has_value()
                ? utf8ToWide(*session.displayKey)
                : tr(TextId::Unknown))
            << tr(TextId::DesktopLine) << width << L" x " << height
            << L" @ (" << session.left << L", " << session.top << L")"
            << L" | DPI：" << session.windowDpi
            << L" / " << session.targetDpiX << L" x " << session.targetDpiY
            << tr(TextId::SourceIdentityLine)
            << (session.sourceAdapterResolved ? tr(TextId::Resolved) : tr(TextId::Unresolved))
            << L" | Source "
            << (session.sourceIdentityResolved ? tr(TextId::Resolved) : tr(TextId::Unresolved))
            << L" | ID " << sourceId
            << tr(TextId::PhysicalTargetsColumn) << session.physicalTargetCount
            << L"\r\nGPU：" << utf8ToWide(session.adapter)
            << tr(TextId::DriverColumn) << driverStateText(session.driver)
            << tr(TextId::ConfiguredRequestLine) << policySource
            << tr(TextId::EffectsColumn) << (policy.enabled ? tr(TextId::On) : tr(TextId::Off))
            << L" | HDR " << (policy.hdrEnabled ? tr(TextId::On) : tr(TextId::Off))
            << L" | " << framePacingText(policy.framePacing)
            << tr(TextId::HostAppliedLine)
            << (session.effectsEnabled ? tr(TextId::On) : tr(TextId::Off))
            << L" | HDR " << (session.hdrEnabled ? tr(TextId::On) : tr(TextId::Off))
            << L" | " << framePacingText(session.framePacing)
            << tr(TextId::RefreshRatesLine) << refreshRateText(session.displayRefresh)
            << tr(TextId::CaptureColumn) << refreshRateText(session.captureRefresh)
            << tr(TextId::CapturePolicyColumn)
            << captureCadenceText(session.captureCadenceStatus)
            << tr(TextId::RefreshPolicyLine)
            << refreshRateText(session.producerPolicyRefresh)
            << L" | freshness "
            << refreshRateText(session.freshnessPolicyRefresh)
            << L" / " << session.freshnessPeriodUs << L" us"
            << L"\r\nWGC producer："
            << producerCadenceText(session.producerCadenceStatus)
            << tr(TextId::RequestedColumn) << session.producerRequestedPeriodUs << L" us"
            << tr(TextId::ActualColumn) << session.producerAppliedPeriodUs << L" us"
            << tr(TextId::ResultColumn)
            << hresultText(static_cast<HRESULT>(session.producerResult))
            << tr(TextId::CadenceFallbackLine)
            << cadenceFallbackText(session.cadenceFallbackReason)
            << tr(TextId::OutputRequestedLine) << outputStateText(session.requestedOutput)
            << tr(TextId::ResolvedColumn) << outputStateText(session.resolvedOutput)
            << tr(TextId::ActualColumn) << outputStateText(session.actualOutput)
            << tr(TextId::OutputMappingLine)
            << outputMappingText(session.resolvedOutputMapping)
            << tr(TextId::ActualColumn)
            << outputMappingText(session.actualOutputMapping)
            << tr(TextId::OutputFallbackLine) << outputFallbackText(session.outputFallback)
            << tr(TextId::ResultColumn)
            << hresultText(static_cast<HRESULT>(session.outputFallbackResult))
            << tr(TextId::PolicySatisfiedColumn)
            << (session.outputPolicySatisfied ? tr(TextId::BooleanYes) : tr(TextId::BooleanNo))
            << tr(TextId::SystemColorLine) << colorStateText(session.colorMode)
            << L" | HDR "
            << optionalBooleanText(session.hdrSupported, tr(TextId::Supported), tr(TextId::Unsupported))
            << L" / "
            << optionalBooleanText(session.hdrActive, tr(TextId::Activated), tr(TextId::NotActivated))
            << tr(TextId::UserSwitchColumn)
            << optionalBooleanText(
                session.hdrUserEnabled,
                tr(TextId::On),
                tr(TextId::Off))
            << tr(TextId::PolicyLimitColumn)
            << optionalBooleanText(
                session.advancedColorLimitedByPolicy,
                tr(TextId::BooleanYes),
                tr(TextId::BooleanNo))
            << tr(TextId::ColorMonitorLine)
            << colorMonitorStateText(session.colorMonitorStatus)
            << L" | HRESULT "
            << hresultText(static_cast<HRESULT>(session.colorMonitorHresult))
            << tr(TextId::MonitorGenerationColumn) << session.colorMonitorGeneration
            << tr(TextId::QueryGenerationColumn) << session.colorQueryGeneration
            << tr(TextId::ColorContractLine)
            << colorSnapshotStateText(session.colorSnapshotDisposition)
            << tr(TextId::CompleteColumn)
            << (session.colorSnapshotComplete ? tr(TextId::BooleanYes) : tr(TextId::BooleanNo))
            << tr(TextId::RetriesColumn) << session.colorRefreshRetriesRemaining
            << tr(TextId::AdvancedColorQueryLine)
            << optionalHresultText(session.advancedColorQueryResult)
            << L"\r\nSDR white level："
            << optionalNitsText(session.sdrWhiteLevelNits)
            << tr(TextId::QueryColumn)
            << optionalHresultText(session.sdrWhiteLevelQueryResult)
            << tr(TextId::RetainedColumn)
            << optionalBooleanText(
                session.sdrWhiteLevelRetained,
                tr(TextId::BooleanYes),
                tr(TextId::BooleanNo))
            << tr(TextId::PhysicalTargetsMatchColumn)
            << optionalBooleanText(
                session.sdrWhiteLevelConsistent,
                tr(TextId::BooleanYes),
                tr(TextId::BooleanNo))
            << tr(TextId::BackgroundCaptureLine) << captureState
            << tr(TextId::RestartColumn) << restartState
            << tr(TextId::RuntimeFaultLine) << faultState;

    for (std::size_t index = 0U;
         index < session.physicalCadence.size();
         ++index)
    {
        const DisplayPhysicalCadenceState& physical =
            session.physicalCadence[index];
        details << tr(TextId::PhysicalTargetLine) << index + 1U
                << tr(TextId::VirtualRateColumn) << refreshRateText(physical.virtualRefresh)
                << tr(TextId::PhysicalRateColumn) << refreshRateText(physical.physicalRefresh)
                << tr(TextId::CaptureColumn) << refreshRateText(physical.captureRefresh)
                << L" | DRR boost "
                << (physical.drrBoosted ? tr(TextId::BooleanYes) : tr(TextId::BooleanNo))
                << tr(TextId::AvailableColumn) << (physical.available ? tr(TextId::BooleanYes) : tr(TextId::BooleanNo));
    }
    if (!session.backgroundCaptureFailure.empty())
    {
        details << tr(TextId::CaptureErrorLine)
                << utf8ToWide(session.backgroundCaptureFailure);
    }
    setText(displayDetailsText_, details.str().c_str());
    static_cast<void>(SendMessageW(displayDetailsText_, EM_SETSEL, 0U, 0));
    static_cast<void>(SendMessageW(displayDetailsText_, EM_SCROLLCARET, 0U, 0));
}

void ControlCenterWindow::updateActiveFxRoiDetails()
{
    if (activeFxRoiDetailsText_ == nullptr)
    {
        return;
    }
    if (!displayStateError_.empty())
    {
        setText(activeFxRoiDetailsText_, displayStateError_.render().c_str());
        return;
    }

    const DisplaySessionState* const session = selectedDisplaySession();
    if (session == nullptr)
    {
        setText(
            activeFxRoiDetailsText_,
            selectedOfflineDisplayOverride() != nullptr
                ? TextId::OfflineRoiHint
                : TextId::SelectActiveDisplayForRoi);
        return;
    }

    const ActiveFxRoiRuntimeState& roi = session->activeFxRoi;
    const bool stale = activeFxRoiSampleIsStale(roi);
    std::wostringstream details;
    details << tr(TextId::StatusLabel) << (stale ? tr(TextId::StaleSample) : tr(TextId::FreshSample))
            << tr(TextId::SwitchColumn) << (roi.enabled ? tr(TextId::On) : tr(TextId::Off))
            << tr(TextId::FrameColumn) << roi.lastFrameId
            << tr(TextId::WindowDurationLine) << roi.sampleWindowMs
            << tr(TextId::SampleAgeColumn) << roi.sampleAgeMs << L" ms"
            << tr(TextId::RoiCompositionHint)
            << tr(TextId::RoiSavingsHint);
    if (!displayStateRefreshWarning_.empty())
    {
        details << tr(TextId::RefreshWarningLine) << displayStateRefreshWarning_.render();
    }
    appendActiveFxRoiPathDetails(details, L"Primary", roi.primary);
    appendActiveFxRoiPathDetails(
        details,
        L"Recording rebuild",
        roi.recordingRebuild);

    setText(activeFxRoiDetailsText_, details.str().c_str());
    static_cast<void>(SendMessageW(
        activeFxRoiDetailsText_,
        EM_SETSEL,
        0U,
        0));
    static_cast<void>(SendMessageW(
        activeFxRoiDetailsText_,
        EM_SCROLLCARET,
        0U,
        0));
}

void ControlCenterWindow::setSelectedDisplayOverride()
{
    const DisplaySessionState* const session = selectedDisplaySession();
    if (session == nullptr || !session->displayKey.has_value())
    {
        updateDisplayPolicyControls();
        setInfo(
            TextId::OverrideSaveFailed,
            TextId::StableDisplayKeyMissing);
        return;
    }

    const std::optional<bafx::config::FramePacing> framePacing =
        selectedFramePacing(displayFramePacing_);
    if (!framePacing.has_value())
    {
        updateDisplayPolicyControls();
        setError(TextId::UnknownDisplayFramePacing);
        return;
    }

    // A pending slider commit refreshes every control. Capture this explicit
    // display edit first so that refresh cannot replace it with the old Host
    // values before the override request is assembled.
    const std::string displayKey = *session->displayKey;
    const bool effectsEnabled = isChecked(displayEffectsEnabled_);
    const bool hdrEnabled = isChecked(displayHdrEnabled_);
    if (!commitPendingPatch())
    {
        return;
    }

    bafx::config::DisplayOverrideConfig overrideConfig{};
    overrideConfig.displayKey = displayKey;
    overrideConfig.enabled = effectsEnabled;
    overrideConfig.hdrEnabled = hdrEnabled;
    overrideConfig.framePacing = *framePacing;
    applyDisplayPolicyCommand(setDisplayOverrideRequest(
        generation_,
        overrideConfig));
}

void ControlCenterWindow::removeSelectedDisplayOverride()
{
    if (!commitPendingPatch())
    {
        return;
    }
    const DisplaySessionState* const session = selectedDisplaySession();
    const bafx::config::DisplayOverrideConfig* const offlineOverride =
        selectedOfflineDisplayOverride();
    const std::string* displayKey = nullptr;
    bool overrideExists = false;
    if (offlineOverride != nullptr)
    {
        displayKey = &offlineOverride->displayKey;
        overrideExists = true;
    }
    else if (session != nullptr && session->displayKey.has_value())
    {
        displayKey = &*session->displayKey;
        overrideExists = bafx::config::findDisplayOverride(
            config_.display,
            *displayKey) != nullptr;
    }

    if (displayKey == nullptr)
    {
        updateDisplayPolicyControls();
        setInfo(
            TextId::RestoreGlobalFailed,
            TextId::StableDisplayKeyMissing);
        return;
    }
    if (!overrideExists)
    {
        updateDisplayPolicyControls();
        updateDisplayDetails();
        return;
    }

    applyDisplayPolicyCommand(removeDisplayOverrideRequest(
        generation_,
        *displayKey));
}

void ControlCenterWindow::applyDisplayPolicyCommand(std::string command)
{
    if (!requireCurrentSnapshot() || !displayStateCurrent_)
    {
        return;
    }
    if (!connected_)
    {
        updateDisplayPolicyControls();
        setInfo(TextId::HostDisconnected, TextId::StartHostAndRefresh);
        return;
    }

    invalidateHostRefresh();
    const bafx::windows::IpcClientResponse response = client_.transact(command);
    if (response.succeeded())
    {
        confirmHostMutation();
        requestHostRefresh();
        return;
    }
    if (response.errorCode == "generation_conflict")
    {
        requestHostRefresh();
        setInfo(TextId::ConfigChanged, TextId::ConfigRefreshed);
        return;
    }

    const UiMessage error = describeResponse(response);
    requestHostRefresh();
    setError(error);
}

const DisplaySessionState* ControlCenterWindow::selectedDisplaySession()
    const noexcept
{
    if (displaySelector_ == nullptr)
    {
        return nullptr;
    }
    const LRESULT selected = SendMessageW(
        displaySelector_,
        CB_GETCURSEL,
        0U,
        0);
    if (selected == CB_ERR)
    {
        return nullptr;
    }
    const LRESULT itemData = SendMessageW(
        displaySelector_,
        CB_GETITEMDATA,
        static_cast<WPARAM>(selected),
        0);
    if (itemData == CB_ERR
        || static_cast<std::size_t>(itemData) >= displayState_.sessions.size())
    {
        return nullptr;
    }
    return &displayState_.sessions[static_cast<std::size_t>(itemData)];
}

const bafx::config::DisplayOverrideConfig*
ControlCenterWindow::selectedOfflineDisplayOverride() const noexcept
{
    if (displaySelector_ == nullptr)
    {
        return nullptr;
    }
    const LRESULT selected = SendMessageW(
        displaySelector_,
        CB_GETCURSEL,
        0U,
        0);
    if (selected == CB_ERR)
    {
        return nullptr;
    }
    const LRESULT itemData = SendMessageW(
        displaySelector_,
        CB_GETITEMDATA,
        static_cast<WPARAM>(selected),
        0);
    if (itemData == CB_ERR
        || itemData < static_cast<LRESULT>(offlineDisplayItemBase))
    {
        return nullptr;
    }

    const std::size_t index = static_cast<std::size_t>(itemData)
        - offlineDisplayItemBase;
    if (index >= displayState_.offlineOverrides.size())
    {
        return nullptr;
    }
    return &displayState_.offlineOverrides[index];
}

}
