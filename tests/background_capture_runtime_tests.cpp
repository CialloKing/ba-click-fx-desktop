#include "test_support.hpp"

#include "background_capture_runtime.hpp"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{

class TemporaryBackgroundCaptureLog final
{
public:
    TemporaryBackgroundCaptureLog()
    {
        directory_ = std::filesystem::temp_directory_path()
            / ("bafx-background-capture-log-"
                + std::to_string(GetCurrentProcessId())
                + "-"
                + std::to_string(GetTickCount64()));
        std::filesystem::create_directories(directory_);
        path_ = directory_ / "support.log";
    }

    ~TemporaryBackgroundCaptureLog()
    {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    TemporaryBackgroundCaptureLog(const TemporaryBackgroundCaptureLog&) = delete;
    TemporaryBackgroundCaptureLog& operator=(
        const TemporaryBackgroundCaptureLog&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

    [[nodiscard]] std::string read() const
    {
        std::ifstream input(path_, std::ios::binary);
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    }

private:
    std::filesystem::path directory_{};
    std::filesystem::path path_{};
};

}

BAFX_TEST(display_target_commit_requires_the_resize_action)
{
    bafx::desktop::BackgroundCaptureExecutionResult execution{};
    execution.targetIntent = bafx::desktop::DisplayTargetIntent{
        bafx::desktop::DisplayTarget{
            reinterpret_cast<HMONITOR>(static_cast<std::uintptr_t>(1U)),
            L"\\\\.\\DISPLAY2",
            RECT{-2560, 0, 0, 1440}},
        true};

    BAFX_CHECK(!bafx::desktop::displayTargetBoundsApplied(execution));
    execution.resizedOutputSize = bafx::windows::WindowSize{1920U, 1080U};
    BAFX_CHECK(!bafx::desktop::displayTargetBoundsApplied(execution));
    execution.resizedOutputSize = bafx::windows::WindowSize{2560U, 1440U};
    BAFX_CHECK(bafx::desktop::displayTargetBoundsApplied(execution));
}

BAFX_TEST(background_cancel_discards_only_superseded_geometry)
{
    using bafx::desktop::BackgroundCaptureCancelResizePolicy;

    BAFX_CHECK(bafx::desktop::backgroundCaptureCancelResizePolicy(false, false)
        == BackgroundCaptureCancelResizePolicy::Preserve);
    BAFX_CHECK(bafx::desktop::backgroundCaptureCancelResizePolicy(true, false)
        == BackgroundCaptureCancelResizePolicy::Discard);
    BAFX_CHECK(bafx::desktop::backgroundCaptureCancelResizePolicy(false, true)
        == BackgroundCaptureCancelResizePolicy::Discard);
    BAFX_CHECK(bafx::desktop::backgroundCaptureCancelResizePolicy(true, true)
        == BackgroundCaptureCancelResizePolicy::Discard);
}

BAFX_TEST(capture_exclusion_health_poller_is_bounded_and_resets)
{
    bafx::desktop::CaptureExclusionHealthPoller poller;
    BAFX_CHECK(!poller.shouldQuery(true, std::chrono::nanoseconds::zero()));
    BAFX_CHECK(!poller.shouldQuery(true, std::chrono::milliseconds(999)));
    BAFX_CHECK(poller.shouldQuery(true, std::chrono::seconds(1)));
    BAFX_CHECK(!poller.shouldQuery(true, std::chrono::milliseconds(1500)));
    BAFX_CHECK(poller.shouldQuery(true, std::chrono::seconds(9)));

    BAFX_CHECK(!poller.shouldQuery(false, std::chrono::seconds(10)));
    BAFX_CHECK(!poller.shouldQuery(true, std::chrono::seconds(20)));
    BAFX_CHECK(poller.shouldQuery(true, std::chrono::seconds(21)));

    BAFX_CHECK(!poller.shouldQuery(true, std::chrono::seconds(2)));
    BAFX_CHECK(poller.shouldQuery(true, std::chrono::seconds(3)));
}

BAFX_TEST(display_topology_capture_recovery_waits_for_session_stop)
{
    bafx::desktop::BackgroundCaptureTopologyRecoveryGate recovery;
    recovery.observeDisplayConfigurationChange(std::chrono::seconds(10));

    BAFX_CHECK(!recovery.takeRetry(
        std::chrono::seconds(11),
        true,
        bafx::windows::BackgroundCaptureFailure::None,
        bafx::windows::GraphicsDriverType::Hardware,
        true,
        false));
    BAFX_CHECK(recovery.takeRetry(
        std::chrono::seconds(12),
        true,
        bafx::windows::BackgroundCaptureFailure::SessionStopped,
        bafx::windows::GraphicsDriverType::Hardware,
        true,
        false));
    BAFX_CHECK(!recovery.takeRetry(
        std::chrono::seconds(12),
        true,
        bafx::windows::BackgroundCaptureFailure::SessionStopped,
        bafx::windows::GraphicsDriverType::Hardware,
        true,
        false));
}

BAFX_TEST(display_topology_capture_recovery_expires_and_honors_safety_gates)
{
    using bafx::windows::BackgroundCaptureFailure;
    using bafx::windows::GraphicsDriverType;

    bafx::desktop::BackgroundCaptureTopologyRecoveryGate recovery;
    recovery.observeDisplayConfigurationChange(std::chrono::seconds(10));
    BAFX_CHECK(!recovery.takeRetry(
        std::chrono::seconds(16),
        true,
        BackgroundCaptureFailure::SessionStopped,
        GraphicsDriverType::Hardware,
        true,
        false));

    recovery.observeDisplayConfigurationChange(std::chrono::seconds(20));
    BAFX_CHECK(!recovery.takeRetry(
        std::chrono::seconds(21),
        false,
        BackgroundCaptureFailure::SessionStopped,
        GraphicsDriverType::Hardware,
        true,
        false));
    BAFX_CHECK(!recovery.takeRetry(
        std::chrono::seconds(21),
        true,
        BackgroundCaptureFailure::SensorStartFailed,
        GraphicsDriverType::Hardware,
        true,
        false));
    BAFX_CHECK(!recovery.takeRetry(
        std::chrono::seconds(21),
        true,
        BackgroundCaptureFailure::SessionStopped,
        GraphicsDriverType::Warp,
        true,
        false));
    BAFX_CHECK(!recovery.takeRetry(
        std::chrono::seconds(21),
        true,
        BackgroundCaptureFailure::SessionStopped,
        GraphicsDriverType::Hardware,
        false,
        false));
    BAFX_CHECK(!recovery.takeRetry(
        std::chrono::seconds(21),
        true,
        BackgroundCaptureFailure::SessionStopped,
        GraphicsDriverType::Hardware,
        true,
        true));
    BAFX_CHECK(recovery.takeRetry(
        std::chrono::seconds(22),
        true,
        BackgroundCaptureFailure::SessionStopped,
        GraphicsDriverType::Hardware,
        true,
        false));
}

BAFX_TEST(device_recovery_retry_requires_an_active_capture)
{
    BAFX_CHECK(bafx::desktop::canRetryBackgroundCaptureAfterDeviceRecovery(
        true,
        true,
        false,
        bafx::windows::GraphicsDriverType::Hardware,
        true));
    BAFX_CHECK(!bafx::desktop::canRetryBackgroundCaptureAfterDeviceRecovery(
        true,
        false,
        false,
        bafx::windows::GraphicsDriverType::Hardware,
        true));
    BAFX_CHECK(!bafx::desktop::canRetryBackgroundCaptureAfterDeviceRecovery(
        false,
        true,
        false,
        bafx::windows::GraphicsDriverType::Hardware,
        true));
}

BAFX_TEST(background_capture_request_selects_session_local_for_recording_mode)
{
    bafx::config::Config config{};
    config.background.mode = bafx::config::RenderMode::RecordingCompatible;
    const bafx::windows::BackgroundCaptureRequest request =
        bafx::desktop::backgroundCaptureRequest(config, 7U);
    BAFX_CHECK(request.sensorRequired);
    BAFX_CHECK(
        request.overlayProfile
        == bafx::windows::FxOverlayProfile::RecordingCompatible);
    BAFX_CHECK(
        request.exclusionMode
        == bafx::windows::BackgroundCaptureRequest::ExclusionMode::SessionLocal);
    BAFX_CHECK(request.retryToken == 7U);
}

BAFX_TEST(background_capture_request_keeps_legacy_and_fx_only_modes_distinct)
{
    bafx::config::Config backgroundAware{};
    backgroundAware.background.mode =
        bafx::config::RenderMode::BackgroundAware;
    const auto legacy = bafx::desktop::backgroundCaptureRequest(backgroundAware);
    BAFX_CHECK(legacy.sensorRequired);
    BAFX_CHECK(
        legacy.exclusionMode
        == bafx::windows::BackgroundCaptureRequest::ExclusionMode::LegacyGlobal);

    bafx::config::Config lightBackground{};
    lightBackground.background.mode =
        bafx::config::RenderMode::LightBackground;
    const auto fxOnly = bafx::desktop::backgroundCaptureRequest(lightBackground);
    BAFX_CHECK(!fxOnly.sensorRequired);
    BAFX_CHECK(
        fxOnly.exclusionMode
        == bafx::windows::BackgroundCaptureRequest::ExclusionMode::LegacyGlobal);
}

BAFX_TEST(core_effects_mode_disables_background_capture)
{
    bafx::config::Config config = bafx::config::defaultConfig();
    config.background.mode = bafx::config::RenderMode::BackgroundAware;
    config.performance.effectsMode = bafx::config::EffectsMode::Core;

    const auto request = bafx::desktop::backgroundCaptureRequest(config);
    BAFX_CHECK(!request.sensorRequired);
    BAFX_CHECK(
        request.overlayProfile == bafx::windows::FxOverlayProfile::Core);
    BAFX_CHECK(
        request.exclusionMode
        == bafx::windows::BackgroundCaptureRequest::ExclusionMode::LegacyGlobal);
}

BAFX_TEST(device_recovery_retry_honors_resource_domain_gates)
{
    BAFX_CHECK(!bafx::desktop::canRetryBackgroundCaptureAfterDeviceRecovery(
        true,
        true,
        true,
        bafx::windows::GraphicsDriverType::Hardware,
        true));
    BAFX_CHECK(!bafx::desktop::canRetryBackgroundCaptureAfterDeviceRecovery(
        true,
        true,
        false,
        bafx::windows::GraphicsDriverType::Warp,
        true));
    BAFX_CHECK(!bafx::desktop::canRetryBackgroundCaptureAfterDeviceRecovery(
        true,
        true,
        false,
        bafx::windows::GraphicsDriverType::Hardware,
        false));
}

BAFX_TEST(display_topology_logs_observed_and_committed_targets)
{
    const TemporaryBackgroundCaptureLog log;
    const bafx::desktop::DisplayTarget first{
        reinterpret_cast<HMONITOR>(static_cast<std::uintptr_t>(1U)),
        L"\\\\.\\DISPLAY1",
        RECT{0, 0, 1920, 1080}};
    const bafx::desktop::DisplayTarget second{
        reinterpret_cast<HMONITOR>(static_cast<std::uintptr_t>(2U)),
        L"\\\\.\\DISPLAY2",
        RECT{-2560, 0, 0, 1440}};

    bafx::desktop::appendDisplayTopologyObserved(
        log.path(),
        17U,
        true,
        first,
        96U,
        second,
        144U);
    bafx::desktop::appendDisplayTopologyApplied(
        log.path(),
        17U,
        first,
        second,
        144U);

    const std::string contents = log.read();
    BAFX_CHECK(contents.find("Event.Name=Display.Topology.Observed")
        != std::string::npos);
    BAFX_CHECK(contents.find("Event.Name=Display.Topology.Applied")
        != std::string::npos);
    BAFX_CHECK(contents.find("Control.Generation=17") != std::string::npos);
    BAFX_CHECK(contents.find("Transaction.Active=true") != std::string::npos);
    BAFX_CHECK(contents.find("Display.Changed=true") != std::string::npos);
    BAFX_CHECK(contents.find("Display.Applied.Dpi=96") != std::string::npos);
    BAFX_CHECK(contents.find("Window.EffectiveDpi=144") != std::string::npos);
    BAFX_CHECK(contents.find("Window.DpiChanged=true") != std::string::npos);
    BAFX_CHECK(contents.find("Display.Applied.Device=\\\\.\\DISPLAY2")
        != std::string::npos);
    BAFX_CHECK(contents.find("Display.Applied.Bounds=2560x1440@-2560,0")
        != std::string::npos);
    BAFX_CHECK(contents.find("Display.Applied.Dpi=144")
        != std::string::npos);
}

BAFX_TEST(capture_exclusion_health_failure_log_preserves_recovery_evidence)
{
    const TemporaryBackgroundCaptureLog log;
    bafx::windows::CaptureExclusionQueryStatus status{};
    status.expectedAffinity = WDA_EXCLUDEFROMCAPTURE;
    status.observedAffinity = WDA_NONE;
    status.queryError = ERROR_INVALID_WINDOW_HANDLE;

    bafx::desktop::appendCaptureExclusionHealthFailure(
        log.path(),
        73U,
        true,
        status);

    const std::string contents = log.read();
    BAFX_CHECK(contents.find("Event.Level=Error") != std::string::npos);
    BAFX_CHECK(
        contents.find("Event.Name=WGC.CaptureExclusion.HealthFailed")
        != std::string::npos);
    BAFX_CHECK(contents.find("Control.Generation=73") != std::string::npos);
    BAFX_CHECK(contents.find("Transaction.Pending=true") != std::string::npos);
    BAFX_CHECK(
        contents.find("Capture.Exclusion.Expected=0x00000011")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("Capture.Exclusion.Observed=0x00000000")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("Capture.Exclusion.Query=failed")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("Capture.Exclusion.QueryError=0x00000578")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("Recovery=stop-wgc-then-fallback-fx-only")
        != std::string::npos);
}

BAFX_TEST(background_snapshot_invalidation_log_preserves_causal_identity)
{
    const TemporaryBackgroundCaptureLog log;
    bafx::desktop::appendBackgroundSnapshotInvalidation(
        log.path(),
        11U,
        bafx::windows::BackgroundSnapshotInvalidation{
            bafx::windows::BackgroundSnapshotInvalidationReason::
                WgcSessionStopped,
            13U,
            17U,
            19U,
            23U,
            29U});

    const std::string contents = log.read();
    BAFX_CHECK(contents.find("Event.Level=Warning") != std::string::npos);
    BAFX_CHECK(
        contents.find("Event.Name=BackgroundSnapshot.Invalidated")
        != std::string::npos);
    BAFX_CHECK(contents.find("Control.Generation=11") != std::string::npos);
    BAFX_CHECK(contents.find("Frame.Id=13") != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Epoch=17") != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Generation=19") != std::string::npos);
    BAFX_CHECK(
        contents.find("BackgroundSnapshot.Epoch=23") != std::string::npos);
    BAFX_CHECK(
        contents.find("BackgroundSnapshot.Generation=29")
        != std::string::npos);
    BAFX_CHECK(
        contents.find(
            "BackgroundSnapshot.InvalidationReason=wgc-session-stopped")
        != std::string::npos);
}

BAFX_TEST(background_stop_observer_persists_the_uncancellable_stage_boundary)
{
    const TemporaryBackgroundCaptureLog log;
    bafx::desktop::BackgroundCaptureStopMonitor monitor(log.path());
    const bafx::windows::WgcBackgroundStopObserver observer =
        monitor.observer();
    observer.notify(bafx::windows::WgcBackgroundStopProgress{
        bafx::windows::WgcBackgroundStopStage::Stop,
        bafx::windows::WgcBackgroundStopStageState::Begin,
        17U,
        17U});
    observer.notify(bafx::windows::WgcBackgroundStopProgress{
        bafx::windows::WgcBackgroundStopStage::SessionClose,
        bafx::windows::WgcBackgroundStopStageState::Begin,
        17U,
        19U});
    observer.notify(bafx::windows::WgcBackgroundStopProgress{
        bafx::windows::WgcBackgroundStopStage::Stop,
        bafx::windows::WgcBackgroundStopStageState::Failed,
        17U,
        17U});

    const std::string contents = log.read();
    BAFX_CHECK(contents.find("Event.Level=Warning") != std::string::npos);
    BAFX_CHECK(
        contents.find("Event.Name=BackgroundCapture.StopProgress")
        != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Stop.Stage=session-close")
        != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Stop.StageState=begin")
        != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Stop.OwnerThreadId=17")
        != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Stop.CallerThreadId=19")
        != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Stop.OwnerThreadMatched=false")
        != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Stop.WatchdogArmed=true")
        != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Stop.WatchdogArmStatus=accepted")
        != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Stop.WatchdogTimeoutMs=10000")
        != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Stop.WatchdogArmed=false")
        != std::string::npos);
}

BAFX_TEST(background_composite_participation_log_keeps_legacy_marker)
{
    const TemporaryBackgroundCaptureLog log;
    bafx::windows::CompositionFrameDiagnostics diagnostics{};
    diagnostics.frameId = 31U;
    diagnostics.wgc.epoch = 37U;
    diagnostics.wgc.acceptedGeneration = 41U;
    diagnostics.wgc.frameConfigurationQueryResult = S_OK;
    diagnostics.wgc.frameConfigurationIterationResult = S_OK;
    diagnostics.wgc.expectedFrameConfigurationIteration = 42U;
    diagnostics.wgc.frameConfigurationIteration = 42U;
    diagnostics.wgc.frameConfigurationIterationConfirmed = true;
    diagnostics.backgroundSnapshotEpoch = 43U;
    diagnostics.backgroundSnapshotGeneration = 47U;
    diagnostics.backgroundParticipated = true;

    bafx::desktop::appendBackgroundCompositeParticipation(
        log.path(),
        53U,
        diagnostics);

    const std::string contents = log.read();
    BAFX_CHECK(contents.find("Event.Level=Info") != std::string::npos);
    BAFX_CHECK(
        contents.find("Event.Name=BackgroundComposite.Participated")
        != std::string::npos);
    BAFX_CHECK(contents.find("Control.Generation=53") != std::string::npos);
    BAFX_CHECK(contents.find("Frame.Id=31") != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Epoch=37") != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Generation=41") != std::string::npos);
    BAFX_CHECK(
        contents.find("BackgroundSnapshot.Epoch=43") != std::string::npos);
    BAFX_CHECK(
        contents.find("BackgroundSnapshot.Generation=47")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.ConfigurationIteration.Frame=42")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.ConfigurationIteration.Expected=42")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.ConfigurationIteration.FrameQi=0x00000000")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.ConfigurationIteration.FrameRead=0x00000000")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.ConfigurationIteration.Confirmed=true")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("Event.Name=Message\nEvent.Message="
                      "WGC background sample entered the final desktop composite")
        != std::string::npos);
}

BAFX_TEST(background_iteration_rejection_is_logged_without_participation)
{
    const TemporaryBackgroundCaptureLog log;
    bafx::windows::CompositionFrameDiagnostics diagnostics{};
    diagnostics.frameId = 67U;
    diagnostics.wgc.frameConfigurationQueryResult = E_NOINTERFACE;
    diagnostics.wgc.frameConfigurationIterationResult = S_FALSE;
    diagnostics.wgc.expectedFrameConfigurationIteration = 6U;
    diagnostics.wgc.frameConfigurationIteration = 5U;
    diagnostics.wgc.configurationIterationRejectedFrames = 1U;
    diagnostics.wgc.configurationIterationRejectedFramesTotal = 9U;
    diagnostics.wgc.configurationIterationConsecutiveRejectedFrames = 2U;

    bafx::desktop::appendBackgroundCompositeParticipation(
        log.path(),
        71U,
        diagnostics);

    const std::string contents = log.read();
    BAFX_CHECK(
        contents.find("Event.Name=WGC.ConfigurationIteration.Rejected")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.ConfigurationIteration.RejectedFrames=1")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.ConfigurationIteration.Frame=5")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.ConfigurationIteration.Expected=6")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.ConfigurationIteration.RejectedFramesTotal=9")
        != std::string::npos);
    BAFX_CHECK(
        contents.find(
            "WGC.ConfigurationIteration.ConsecutiveRejectedFrames=2")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.ConfigurationIteration.FrameQi=0x80004002")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("Event.Name=BackgroundComposite.Participated")
        == std::string::npos);
}

BAFX_TEST(background_composite_participation_log_rejects_unpresented_frame)
{
    const TemporaryBackgroundCaptureLog log;
    bafx::windows::CompositionFrameDiagnostics diagnostics{};
    diagnostics.frameId = 59U;

    bafx::desktop::appendBackgroundCompositeParticipation(
        log.path(),
        61U,
        diagnostics);

    BAFX_CHECK(!std::filesystem::exists(log.path()));
}

BAFX_TEST(borderless_access_log_preserves_permission_decision)
{
    const TemporaryBackgroundCaptureLog log;
    const bafx::windows::BorderlessCaptureAccessResult result{
        bafx::windows::BorderlessCaptureAccessStatus::NotPackaged,
        HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE),
        bafx::windows::BorderlessCaptureAccessAsyncStatus::Canceled,
        4321U,
        true};

    bafx::desktop::appendBorderlessCaptureAccessCheck(
        log.path(),
        67U,
        3U,
        result);

    const std::string contents = log.read();
    BAFX_CHECK(contents.find("Event.Level=Warning") != std::string::npos);
    BAFX_CHECK(
        contents.find("Event.Name=WGC.BorderlessAccess.Checked")
        != std::string::npos);
    BAFX_CHECK(contents.find("Control.Generation=67") != std::string::npos);
    BAFX_CHECK(
        contents.find("Transaction.ActionIndex=3") != std::string::npos);
    BAFX_CHECK(
        contents.find("Background.AllowSystemBorder=false")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.BorderlessAccess.Status=not-packaged")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.BorderlessAccess.HRESULT=0x80073D54")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.BorderlessAccess.AsyncStatus=canceled")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.BorderlessAccess.ElapsedMs=4321")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.BorderlessAccess.CancelRequested=true")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.BorderlessAccess.Allowed=false")
        != std::string::npos);
}
