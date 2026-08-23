#include "test_support.hpp"

#include "host_control.hpp"
#include "display_state.hpp"

#include "bafx/windows/ipc_client.hpp"
#include "bafx/windows/recording_compatibility.hpp"

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace
{

class TemporaryConfigDirectory final
{
public:
    TemporaryConfigDirectory()
    {
        const auto nonce = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = std::filesystem::temp_directory_path()
            / (L"bafx-host-control-tests-"
               + std::to_wstring(GetCurrentProcessId())
               + L"-"
               + std::to_wstring(nonce));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryConfigDirectory()
    {
        // This process owns the PID/clock-scoped directory exclusively.
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryConfigDirectory(const TemporaryConfigDirectory&) = delete;
    TemporaryConfigDirectory& operator=(
        const TemporaryConfigDirectory&) = delete;

    [[nodiscard]] std::filesystem::path configPath() const
    {
        return path_ / L"BAFX.config.json";
    }

    [[nodiscard]] const std::filesystem::path& directoryPath() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_{};
};

struct StartupRegistrationInvocation final
{
    bafx::config::SystemConfig system{};
    bafx::desktop::HostSystemIntegrationPhase phase{
        bafx::desktop::HostSystemIntegrationPhase::Apply};
};

struct FakeSystemIntegration final
{
    std::vector<bafx::windows::StartupRegistrationResult> results{};
    mutable std::vector<StartupRegistrationInvocation> invocations{};
    mutable std::size_t nextResult{0U};

    [[nodiscard]] bafx::desktop::HostSystemIntegration dependency() const noexcept
    {
        return bafx::desktop::HostSystemIntegration{
            this,
            &FakeSystemIntegration::apply};
    }

    [[nodiscard]] static bafx::windows::StartupRegistrationResult apply(
        const void* const context,
        const bafx::config::SystemConfig& system,
        const bafx::desktop::HostSystemIntegrationPhase phase) noexcept
    {
        const auto* const integration =
            static_cast<const FakeSystemIntegration*>(context);
        integration->invocations.push_back(
            StartupRegistrationInvocation{system, phase});
        if (integration->nextResult < integration->results.size())
        {
            return integration->results[integration->nextResult++];
        }
        return bafx::windows::StartupRegistrationResult{
            bafx::windows::StartupRegistrationStatus::Unchanged,
            bafx::windows::StartupRegistrationOperation::QueryValue,
            ERROR_SUCCESS,
            {}};
    }
};

[[nodiscard]] std::wstring testPipeName()
{
    return L"\\\\.\\pipe\\BAFX.HostControlTest."
        + std::to_wstring(GetCurrentProcessId())
        + L"."
        + std::to_wstring(GetTickCount64());
}

void checkEffectsEqual(
    const bafx::config::EffectsConfig& actual,
    const bafx::config::EffectsConfig& expected)
{
    BAFX_CHECK(actual.enabled == expected.enabled);
    BAFX_CHECK(actual.diskLayerEnabled == expected.diskLayerEnabled);
    BAFX_CHECK(actual.ringsLayerEnabled == expected.ringsLayerEnabled);
    BAFX_CHECK(
        actual.clickShardsLayerEnabled
        == expected.clickShardsLayerEnabled);
    BAFX_CHECK(
        actual.trailShardsLayerEnabled
        == expected.trailShardsLayerEnabled);
    BAFX_CHECK(actual.trailLayerEnabled == expected.trailLayerEnabled);
    BAFX_CHECK(actual.bloomLayerEnabled == expected.bloomLayerEnabled);
    BAFX_CHECK_NEAR(actual.globalScale, expected.globalScale, 0.00001F);
    BAFX_CHECK_NEAR(actual.opacity, expected.opacity, 0.00001F);
    BAFX_CHECK(actual.clickEnabled == expected.clickEnabled);
    BAFX_CHECK(actual.trailEnabled == expected.trailEnabled);
    BAFX_CHECK_NEAR(actual.trailLength, expected.trailLength, 0.00001F);
    BAFX_CHECK_NEAR(actual.trailWidth, expected.trailWidth, 0.00001F);
    BAFX_CHECK_NEAR(actual.clickTimeScale, expected.clickTimeScale, 0.00001F);
    BAFX_CHECK_NEAR(actual.trailTimeScale, expected.trailTimeScale, 0.00001F);
    BAFX_CHECK_NEAR(actual.trailLifetimeMs, expected.trailLifetimeMs, 0.00001F);
    BAFX_CHECK_NEAR(actual.diskLifetimeMs, expected.diskLifetimeMs, 0.00001F);
    BAFX_CHECK_NEAR(actual.diskRadius, expected.diskRadius, 0.00001F);
    BAFX_CHECK(actual.ringsCount == expected.ringsCount);
    BAFX_CHECK_NEAR(actual.ringsLifetimeMs, expected.ringsLifetimeMs, 0.00001F);
    BAFX_CHECK_NEAR(actual.ringsRadiusMin, expected.ringsRadiusMin, 0.00001F);
    BAFX_CHECK_NEAR(actual.ringsRadiusMax, expected.ringsRadiusMax, 0.00001F);
    BAFX_CHECK_NEAR(
        actual.ringsAngularVelocityMultiplier,
        expected.ringsAngularVelocityMultiplier,
        0.00001F);
    BAFX_CHECK_NEAR(
        actual.ringsRotationDirection,
        expected.ringsRotationDirection,
        0.00001F);
    BAFX_CHECK_NEAR(
        actual.ringsHdrIntensity,
        expected.ringsHdrIntensity,
        0.00001F);
    BAFX_CHECK_NEAR(
        actual.shardsHdrIntensity,
        expected.shardsHdrIntensity,
        0.00001F);
    BAFX_CHECK_NEAR(actual.trailOpacity, expected.trailOpacity, 0.00001F);
    BAFX_CHECK_NEAR(actual.bloomIntensity, expected.bloomIntensity, 0.00001F);
    BAFX_CHECK_NEAR(actual.bloomDiffusion, expected.bloomDiffusion, 0.00001F);
    BAFX_CHECK_NEAR(actual.bloomThreshold, expected.bloomThreshold, 0.00001F);
    BAFX_CHECK_NEAR(actual.bloomSoftKnee, expected.bloomSoftKnee, 0.00001F);
    BAFX_CHECK_NEAR(actual.bloomClamp, expected.bloomClamp, 0.00001F);
}

}

BAFX_TEST(host_control_start_latches_generation_before_accepting_set_config)
{
    TemporaryConfigDirectory temporary;
    bafx::windows::NamedPipeIpcServer::Options serverOptions{};
    serverOptions.pipeName = testPipeName();
    serverOptions.ioTimeoutMilliseconds = 500U;
    serverOptions.retryDelayMilliseconds = 10U;
    bafx::desktop::HostControlPlane control(
        temporary.configPath(),
        bafx::config::defaultConfig(),
        serverOptions,
        bafx::desktop::HostSystemIntegration{},
        bafx::windows::recordingCompatibleAvailabilityForBuild(28000U));

    const bafx::desktop::HostControlStartResult start = control.start(true);
    BAFX_CHECK(start.serviceStarted);
    BAFX_CHECK(start.appliedGeneration == 1U);

    bafx::windows::IpcClientOptions clientOptions{};
    clientOptions.pipeName = serverOptions.pipeName;
    clientOptions.timeoutMilliseconds = 500U;
    const bafx::windows::NamedPipeIpcClient client(clientOptions);
    const bafx::windows::IpcClientResponse initial = client.transact("GetState");
    BAFX_CHECK(initial.succeeded());
    BAFX_CHECK(initial.payload.find("\"generation\":1") != std::string::npos);
    BAFX_CHECK(
        initial.payload.find("\"backgroundCapture\":\"active\"")
        != std::string::npos);

    const bafx::windows::IpcClientResponse changed = client.transact(
        "SetConfig {\"generation\":1,\"path\":\"background.mode\","
        "\"value\":\"recording-compatible\"}");
    BAFX_CHECK(changed.succeeded());
    const bafx::desktop::HostStateSnapshot current = control.snapshot();
    control.stop();

    BAFX_CHECK(current.generation == 2U);
    BAFX_CHECK(current.generation != start.appliedGeneration);
    BAFX_CHECK(
        current.config.background.mode
        == bafx::config::RenderMode::RecordingCompatible);
}

BAFX_TEST(host_control_publishes_spout2_runtime_without_changing_generation)
{
    TemporaryConfigDirectory temporary;
    bafx::windows::NamedPipeIpcServer::Options serverOptions{};
    serverOptions.pipeName = testPipeName() + L".spout2-runtime";
    serverOptions.ioTimeoutMilliseconds = 500U;
    serverOptions.retryDelayMilliseconds = 10U;
    bafx::desktop::HostControlPlane control(
        temporary.configPath(),
        bafx::config::defaultConfig(),
        serverOptions,
        bafx::desktop::HostSystemIntegration{},
        bafx::windows::recordingCompatibleAvailabilityForBuild(28000U));
    control.setSpout2RuntimeState(
        true,
        "ba-click-fx-desktop",
        bafx::windows::Spout2SenderStatus::Failed,
        "receiver said \"no\" \\ retry");
    BAFX_CHECK(control.start(false).serviceStarted);

    bafx::windows::IpcClientOptions clientOptions{};
    clientOptions.pipeName = serverOptions.pipeName;
    clientOptions.timeoutMilliseconds = 500U;
    const bafx::windows::NamedPipeIpcClient client(clientOptions);
    const bafx::windows::IpcClientResponse state = client.transact("GetState");
    const bafx::desktop::HostStateSnapshot snapshot = control.snapshot();
    control.stop();

    BAFX_CHECK(state.succeeded());
    BAFX_CHECK(snapshot.generation == 1U);
    BAFX_CHECK(snapshot.spout2.enabled);
    BAFX_CHECK(snapshot.spout2.status == "failed");
    BAFX_CHECK(
        snapshot.spout2.outputContract
        == bafx::windows::spout2OutputContract);
    BAFX_CHECK(
        state.payload.find("\"spout2Enabled\":true")
        != std::string::npos);
    BAFX_CHECK(
        state.payload.find(
            "\"spout2Sender\":\"ba-click-fx-desktop\"")
        != std::string::npos);
    BAFX_CHECK(
        state.payload.find("\"spout2Status\":\"failed\"")
        != std::string::npos);
    BAFX_CHECK(
        state.payload.find("receiver said \\\"no\\\" \\\\ retry")
        != std::string::npos);
    BAFX_CHECK(
        state.payload.find(std::string(bafx::windows::spout2OutputContract))
        != std::string::npos);
}

BAFX_TEST(host_control_rejects_recording_mode_below_minimum_build)
{
    TemporaryConfigDirectory temporary;
    bafx::windows::NamedPipeIpcServer::Options serverOptions{};
    serverOptions.pipeName = testPipeName() + L".unsupported-build";
    serverOptions.ioTimeoutMilliseconds = 500U;
    serverOptions.retryDelayMilliseconds = 10U;
    bafx::desktop::HostControlPlane control(
        temporary.configPath(),
        bafx::config::defaultConfig(),
        serverOptions,
        bafx::desktop::HostSystemIntegration{},
        bafx::windows::recordingCompatibleAvailabilityForBuild(26100U));
    BAFX_CHECK(control.start(false).serviceStarted);

    bafx::windows::IpcClientOptions clientOptions{};
    clientOptions.pipeName = serverOptions.pipeName;
    clientOptions.timeoutMilliseconds = 1'000U;
    const bafx::windows::NamedPipeIpcClient client(clientOptions);
    const bafx::windows::IpcClientResponse response = client.transact(
        "SetConfig {\"generation\":1,\"path\":\"background.mode\","
        "\"value\":\"recording-compatible\"}");
    const bafx::desktop::HostStateSnapshot state = control.snapshot();
    control.stop();

    BAFX_CHECK(response.transportSucceeded());
    BAFX_CHECK(!response.succeeded());
    BAFX_CHECK(response.errorCode == "unsupported_os_build");
    BAFX_CHECK(response.errorMessage.find("26100") != std::string::npos);
    BAFX_CHECK(state.generation == 1U);
    BAFX_CHECK(
        state.config.background.mode
        == bafx::config::RenderMode::BackgroundAware);
    BAFX_CHECK(!std::filesystem::exists(temporary.configPath()));
}

BAFX_TEST(host_control_falls_back_and_persists_light_background)
{
    TemporaryConfigDirectory temporary;
    bafx::config::Config initial = bafx::config::defaultConfig();
    initial.background.mode = bafx::config::RenderMode::RecordingCompatible;
    BAFX_CHECK(
        bafx::config::saveConfigAtomic(temporary.configPath(), initial)
            .succeeded());

    bafx::windows::NamedPipeIpcServer::Options serverOptions{};
    serverOptions.pipeName = testPipeName() + L".startup-fallback";
    serverOptions.ioTimeoutMilliseconds = 500U;
    serverOptions.retryDelayMilliseconds = 10U;
    bafx::desktop::HostControlPlane control(
        temporary.configPath(),
        initial,
        serverOptions,
        bafx::desktop::HostSystemIntegration{},
        bafx::windows::recordingCompatibleAvailabilityForBuild(26100U));
    BAFX_CHECK(control.start(false).serviceStarted);
    const bafx::desktop::HostStateSnapshot state = control.snapshot();
    const bafx::config::ConfigLoadResult persisted =
        bafx::config::loadConfig(temporary.configPath());
    control.stop();

    BAFX_CHECK(
        state.config.background.mode
        == bafx::config::RenderMode::LightBackground);
    BAFX_CHECK(persisted.succeeded());
    BAFX_CHECK(
        persisted.config.background.mode
        == bafx::config::RenderMode::LightBackground);
}

BAFX_TEST(host_control_keeps_light_background_when_fallback_save_fails)
{
    TemporaryConfigDirectory temporary;
    bafx::config::Config initial = bafx::config::defaultConfig();
    initial.background.mode = bafx::config::RenderMode::RecordingCompatible;
    bafx::windows::NamedPipeIpcServer::Options serverOptions{};
    serverOptions.pipeName = testPipeName() + L".fallback-save-failure";
    serverOptions.ioTimeoutMilliseconds = 500U;
    serverOptions.retryDelayMilliseconds = 10U;
    bafx::desktop::HostControlPlane control(
        temporary.directoryPath(),
        initial,
        serverOptions,
        bafx::desktop::HostSystemIntegration{},
        bafx::windows::recordingCompatibleAvailabilityForBuild(26100U));
    BAFX_CHECK(control.start(false).serviceStarted);
    const bafx::desktop::HostStateSnapshot state = control.snapshot();
    control.stop();

    BAFX_CHECK(
        state.config.background.mode
        == bafx::config::RenderMode::LightBackground);
}

BAFX_TEST(host_control_rejects_system_patch_when_external_apply_fails)
{
    TemporaryConfigDirectory temporary;
    FakeSystemIntegration integration{};
    integration.results.push_back(
        bafx::windows::StartupRegistrationResult{
            bafx::windows::StartupRegistrationStatus::Failed,
            bafx::windows::StartupRegistrationOperation::SetValue,
            ERROR_ACCESS_DENIED,
            {}});
    bafx::windows::NamedPipeIpcServer::Options serverOptions{};
    serverOptions.pipeName = testPipeName() + L".system-apply-failure";
    serverOptions.ioTimeoutMilliseconds = 500U;
    serverOptions.retryDelayMilliseconds = 10U;
    bafx::desktop::HostControlPlane control(
        temporary.configPath(),
        bafx::config::defaultConfig(),
        serverOptions,
        integration.dependency(),
        bafx::windows::recordingCompatibleAvailabilityForBuild(28000U));
    BAFX_CHECK(control.start(false).serviceStarted);

    bafx::windows::IpcClientOptions clientOptions{};
    clientOptions.pipeName = serverOptions.pipeName;
    clientOptions.timeoutMilliseconds = 1'000U;
    const bafx::windows::NamedPipeIpcClient client(clientOptions);
    const bafx::windows::IpcClientResponse response = client.transact(
        "SetConfig {\"generation\":1,\"path\":\"system.startWithWindows\","
        "\"value\":true}");
    const bafx::desktop::HostStateSnapshot current = control.snapshot();
    control.stop();

    BAFX_CHECK(response.transportSucceeded());
    BAFX_CHECK(!response.succeeded());
    BAFX_CHECK(response.errorCode == "system_integration_failed");
    BAFX_CHECK(response.errorMessage.find("phase=apply") != std::string::npos);
    BAFX_CHECK(
        response.errorMessage.find("operation=set-value")
        != std::string::npos);
    BAFX_CHECK(
        response.errorMessage.find("win32-error=5")
        != std::string::npos);
    BAFX_CHECK(current.generation == 1U);
    BAFX_CHECK(!current.config.system.startWithWindows);
    BAFX_CHECK(!std::filesystem::exists(temporary.configPath()));
    BAFX_CHECK(integration.invocations.size() == 1U);
    BAFX_CHECK(integration.invocations[0].system.startWithWindows);
    BAFX_CHECK(
        integration.invocations[0].phase
        == bafx::desktop::HostSystemIntegrationPhase::Apply);
}

BAFX_TEST(host_control_full_config_applies_system_integration_before_commit)
{
    TemporaryConfigDirectory temporary;
    FakeSystemIntegration integration{};
    integration.results.push_back(
        bafx::windows::StartupRegistrationResult{
            bafx::windows::StartupRegistrationStatus::Updated,
            bafx::windows::StartupRegistrationOperation::SetValue,
            ERROR_SUCCESS,
            {}});
    bafx::windows::NamedPipeIpcServer::Options serverOptions{};
    serverOptions.pipeName = testPipeName() + L".system-full-config";
    serverOptions.ioTimeoutMilliseconds = 500U;
    serverOptions.retryDelayMilliseconds = 10U;
    bafx::desktop::HostControlPlane control(
        temporary.configPath(),
        bafx::config::defaultConfig(),
        serverOptions,
        integration.dependency(),
        bafx::windows::recordingCompatibleAvailabilityForBuild(28000U));
    BAFX_CHECK(control.start(false).serviceStarted);

    bafx::config::Config candidate = bafx::config::defaultConfig();
    candidate.system.startWithWindows = true;
    candidate.system.startMinimized = true;
    candidate.system.closeToTray = false;
    candidate.background.mode =
        bafx::config::RenderMode::RecordingCompatible;
    bafx::windows::IpcClientOptions clientOptions{};
    clientOptions.pipeName = serverOptions.pipeName;
    clientOptions.timeoutMilliseconds = 1'000U;
    const bafx::windows::NamedPipeIpcClient client(clientOptions);
    const bafx::windows::IpcClientResponse response = client.transact(
        "SetConfig " + bafx::config::toJson(candidate, false));
    const bafx::desktop::HostStateSnapshot current = control.snapshot();
    control.stop();

    BAFX_CHECK(response.succeeded());
    BAFX_CHECK(current.generation == 2U);
    BAFX_CHECK(current.config.system.startWithWindows);
    BAFX_CHECK(current.config.system.startMinimized);
    BAFX_CHECK(!current.config.system.closeToTray);
    BAFX_CHECK(integration.invocations.size() == 1U);
    BAFX_CHECK(integration.invocations[0].system.startWithWindows);
    BAFX_CHECK(integration.invocations[0].system.startMinimized);
    const bafx::config::ConfigLoadResult persisted =
        bafx::config::loadConfig(temporary.configPath());
    BAFX_CHECK(persisted.status == bafx::config::ConfigStatus::Ok);
    BAFX_CHECK(persisted.config.system.startWithWindows);
    BAFX_CHECK(persisted.config.system.startMinimized);
}

BAFX_TEST(host_control_compensates_external_state_when_config_save_fails)
{
    TemporaryConfigDirectory temporary;
    FakeSystemIntegration integration{};
    integration.results = {
        bafx::windows::StartupRegistrationResult{
            bafx::windows::StartupRegistrationStatus::Updated,
            bafx::windows::StartupRegistrationOperation::SetValue,
            ERROR_SUCCESS,
            {}},
        bafx::windows::StartupRegistrationResult{
            bafx::windows::StartupRegistrationStatus::Removed,
            bafx::windows::StartupRegistrationOperation::DeleteValue,
            ERROR_SUCCESS,
            {}}};
    bafx::windows::NamedPipeIpcServer::Options serverOptions{};
    serverOptions.pipeName = testPipeName() + L".system-compensation";
    serverOptions.ioTimeoutMilliseconds = 500U;
    serverOptions.retryDelayMilliseconds = 10U;
    bafx::desktop::HostControlPlane control(
        temporary.directoryPath(),
        bafx::config::defaultConfig(),
        serverOptions,
        integration.dependency());
    BAFX_CHECK(control.start(false).serviceStarted);

    bafx::windows::IpcClientOptions clientOptions{};
    clientOptions.pipeName = serverOptions.pipeName;
    clientOptions.timeoutMilliseconds = 1'000U;
    const bafx::windows::NamedPipeIpcClient client(clientOptions);
    const bafx::windows::IpcClientResponse response = client.transact(
        "SetConfig {\"generation\":1,\"path\":\"system.startWithWindows\","
        "\"value\":true}");
    const bafx::desktop::HostStateSnapshot current = control.snapshot();
    control.stop();

    BAFX_CHECK(!response.succeeded());
    BAFX_CHECK(response.errorCode == "config_write_failed");
    BAFX_CHECK(
        response.errorMessage.find("phase=compensate")
        != std::string::npos);
    BAFX_CHECK(
        response.errorMessage.find("operation=delete-value")
        != std::string::npos);
    BAFX_CHECK(
        response.errorMessage.find("win32-error=0")
        != std::string::npos);
    BAFX_CHECK(current.generation == 1U);
    BAFX_CHECK(!current.config.system.startWithWindows);
    BAFX_CHECK(integration.invocations.size() == 2U);
    BAFX_CHECK(integration.invocations[0].system.startWithWindows);
    BAFX_CHECK(!integration.invocations[1].system.startWithWindows);
    BAFX_CHECK(
        integration.invocations[1].phase
        == bafx::desktop::HostSystemIntegrationPhase::Compensate);
}

BAFX_TEST(host_control_reports_compensation_phase_and_win32_failure)
{
    TemporaryConfigDirectory temporary;
    FakeSystemIntegration integration{};
    integration.results = {
        bafx::windows::StartupRegistrationResult{
            bafx::windows::StartupRegistrationStatus::Updated,
            bafx::windows::StartupRegistrationOperation::SetValue,
            ERROR_SUCCESS,
            {}},
        bafx::windows::StartupRegistrationResult{
            bafx::windows::StartupRegistrationStatus::Failed,
            bafx::windows::StartupRegistrationOperation::DeleteValue,
            ERROR_ACCESS_DENIED,
            {}}};
    bafx::windows::NamedPipeIpcServer::Options serverOptions{};
    serverOptions.pipeName = testPipeName() + L".system-compensation-failure";
    serverOptions.ioTimeoutMilliseconds = 500U;
    serverOptions.retryDelayMilliseconds = 10U;
    bafx::desktop::HostControlPlane control(
        temporary.directoryPath(),
        bafx::config::defaultConfig(),
        serverOptions,
        integration.dependency());
    BAFX_CHECK(control.start(false).serviceStarted);

    bafx::windows::IpcClientOptions clientOptions{};
    clientOptions.pipeName = serverOptions.pipeName;
    clientOptions.timeoutMilliseconds = 1'000U;
    const bafx::windows::NamedPipeIpcClient client(clientOptions);
    const bafx::windows::IpcClientResponse response = client.transact(
        "SetConfig {\"generation\":1,\"path\":\"system.startWithWindows\","
        "\"value\":true}");
    const bafx::desktop::HostStateSnapshot current = control.snapshot();
    control.stop();

    BAFX_CHECK(!response.succeeded());
    BAFX_CHECK(response.errorCode == "system_integration_failed");
    BAFX_CHECK(
        response.errorMessage.find("phase=compensate")
        != std::string::npos);
    BAFX_CHECK(
        response.errorMessage.find("operation=delete-value")
        != std::string::npos);
    BAFX_CHECK(
        response.errorMessage.find("win32-error=5")
        != std::string::npos);
    BAFX_CHECK(
        response.errorMessage.find("config-write-error=")
        != std::string::npos);
    BAFX_CHECK(current.generation == 1U);
    BAFX_CHECK(!current.config.system.startWithWindows);
    BAFX_CHECK(integration.invocations.size() == 2U);
}

BAFX_TEST(host_control_serializes_display_override_mutations_by_generation)
{
    TemporaryConfigDirectory temporary;
    bafx::windows::NamedPipeIpcServer::Options serverOptions{};
    serverOptions.pipeName = testPipeName() + L".display-override";
    serverOptions.ioTimeoutMilliseconds = 500U;
    serverOptions.retryDelayMilliseconds = 10U;
    bafx::desktop::HostControlPlane control(
        temporary.configPath(),
        bafx::config::defaultConfig(),
        serverOptions);
    BAFX_CHECK(control.start(false).serviceStarted);

    bafx::windows::IpcClientOptions clientOptions{};
    clientOptions.pipeName = serverOptions.pipeName;
    clientOptions.timeoutMilliseconds = 1'000U;
    const bafx::windows::NamedPipeIpcClient client(clientOptions);
    const std::string key = "displayconfig-v1-sha256:test-panel";
    const bafx::windows::IpcClientResponse added = client.transact(
        "SetDisplayOverride {\"generation\":1,\"displayKey\":\""
        + key
        + "\",\"enabled\":false,\"hdrEnabled\":true,"
          "\"framePacing\":\"120\"}");
    BAFX_CHECK(added.succeeded());

    const bafx::windows::IpcClientResponse stale = client.transact(
        "RemoveDisplayOverride {\"generation\":1,\"displayKey\":\""
        + key + "\"}");
    BAFX_CHECK(!stale.succeeded());
    BAFX_CHECK(stale.errorCode == "generation_conflict");

    const bafx::windows::IpcClientResponse removed = client.transact(
        "RemoveDisplayOverride {\"generation\":2,\"displayKey\":\""
        + key + "\"}");
    BAFX_CHECK(removed.succeeded());
    const bafx::desktop::HostStateSnapshot current = control.snapshot();
    control.stop();

    BAFX_CHECK(current.generation == 3U);
    BAFX_CHECK(current.config.display.overrides.empty());
}

BAFX_TEST(host_control_observes_shutdown_after_the_client_receives_its_ack)
{
    TemporaryConfigDirectory temporary;
    bafx::windows::NamedPipeIpcServer::Options serverOptions{};
    serverOptions.pipeName = testPipeName();
    serverOptions.ioTimeoutMilliseconds = 500U;
    serverOptions.retryDelayMilliseconds = 10U;
    bafx::desktop::HostControlPlane control(
        temporary.configPath(),
        bafx::config::defaultConfig(),
        serverOptions);
    BAFX_CHECK(control.start(false).serviceStarted);

    bafx::windows::IpcClientOptions clientOptions{};
    clientOptions.pipeName = serverOptions.pipeName;
    clientOptions.timeoutMilliseconds = 1'000U;
    const bafx::windows::NamedPipeIpcClient client(clientOptions);
    const bafx::windows::IpcClientResponse response =
        client.transact("Shutdown");
    bafx::desktop::HostStateSnapshot stopped = control.snapshot();
    for (std::size_t attempt = 0U;
         attempt < 200U && !stopped.shutdownRequested;
         ++attempt)
    {
        Sleep(1U);
        stopped = control.snapshot();
    }
    control.stop();

    BAFX_CHECK(response.succeeded());
    BAFX_CHECK(response.payload == "{\"shutdownRequested\":true}");
    BAFX_CHECK(stopped.shutdownRequested);
}

BAFX_TEST(host_control_publishes_one_immutable_display_state_snapshot)
{
    TemporaryConfigDirectory temporary;
    bafx::windows::NamedPipeIpcServer::Options serverOptions{};
    serverOptions.pipeName = testPipeName() + L".display-state";
    serverOptions.ioTimeoutMilliseconds = 500U;
    serverOptions.retryDelayMilliseconds = 10U;
    bafx::config::Config initialConfig = bafx::config::defaultConfig();
    BAFX_CHECK(bafx::config::setDisplayOverride(
        initialConfig,
        bafx::config::DisplayOverrideConfig{
            "displayconfig-v1-sha256:test-display",
            false,
            true,
            bafx::config::FramePacing::Fixed120}));
    BAFX_CHECK(bafx::config::setDisplayOverride(
        initialConfig,
        bafx::config::DisplayOverrideConfig{
            "displayconfig-v1-sha256:offline-display",
            true,
            false,
            bafx::config::FramePacing::Fixed60}));
    bafx::desktop::HostControlPlane control(
        temporary.configPath(),
        initialConfig,
        serverOptions);
    BAFX_CHECK(control.start(false).serviceStarted);

    bafx::windows::DisplaySessionRuntimeSummary session{};
    session.monitor = "monitor-1";
    session.device = R"(\\.\DISPLAY1)";
    session.displayKey = "displayconfig-v1-sha256:test-display";
    session.bounds = RECT{0, 0, 3840, 2160};
    session.targetDpiX = 144U;
    session.targetDpiY = 144U;
    session.windowDpi = 144U;
    session.displayRefreshRate = bafx::windows::DisplayRefreshRate{
        144'000U,
        1'001U,
        bafx::windows::DisplayRefreshRateSource::DisplayConfigPath};
    session.captureRefreshRate = bafx::windows::DisplayRefreshRate{
        60U,
        1U,
        bafx::windows::DisplayRefreshRateSource::DisplayConfigPath};
    session.captureCadenceStatus =
        bafx::windows::BackgroundCadenceRefreshStatus::TargetRate;
    session.captureCadenceFallbackReason =
        bafx::windows::DisplayCaptureCadenceFallbackReason::None;
    session.producerPolicyRefreshRate = session.displayRefreshRate;
    session.freshnessPolicyRefreshRate = session.captureRefreshRate;
    session.freshnessPolicyPeriod = std::chrono::microseconds(16'667);
    session.producerCadence.status =
        bafx::windows::WgcProducerCadenceStatus::Applied;
    session.producerCadence.requested = std::chrono::microseconds(6'951);
    session.producerCadence.applied = std::chrono::microseconds(6'951);
    session.producerCadence.result = S_OK;
    session.physicalCadence.push_back(
        bafx::windows::DisplayPhysicalCadenceRuntimeSummary{
            session.displayRefreshRate,
            session.displayRefreshRate,
            session.displayRefreshRate,
            true,
            true});
    session.sourceAdapterResolved = true;
    session.sourceIdentityResolved = true;
    session.sourceId = 7U;
    session.physicalTargetCount = 1U;
    session.deviceInfo.adapterDescription = L"Test Adapter";
    session.deviceInfo.driverType = bafx::windows::GraphicsDriverType::Hardware;
    session.requestedOutputPreference =
        bafx::windows::CompositionOutputPreference::PreferLinearScRgb;
    session.resolvedOutputPolicy = bafx::windows::compositionOutputPolicyFor(
        bafx::windows::CompositionOutputPreference::PreferLinearScRgb);
    session.deviceInfo.output.transfer =
        bafx::windows::CompositionOutputTransfer::LinearScRgb;
    session.deviceInfo.output.mapping = session.resolvedOutputPolicy.mapping;
    session.outputPolicySatisfied = true;
    session.coordinator = true;
    session.primary = true;
    session.backgroundCaptureActive = true;
    session.backgroundCaptureRestartAllowed = true;
    session.effectsEnabled = false;
    session.hdrEnabled = true;
    session.framePacing = "120";

    bafx::windows::DisplayColorCapabilities color{};
    color.displayPathResolved = true;
    color.advancedColorQueryResult = ERROR_SUCCESS;
    color.advancedColorStateConsistent = true;
    color.activeColorMode = bafx::windows::DisplayColorMode::Hdr;
    color.advancedColorActive = true;
    color.advancedColorInfoV2 = true;
    color.highDynamicRangeSupported = true;
    color.highDynamicRangeUserEnabled = true;
    color.sdrWhiteLevelQueryResult = ERROR_SUCCESS;
    color.sdrWhiteLevelConsistent = true;
    color.sdrWhiteLevelValid = true;
    color.sdrWhiteLevelNits = 203.0F;
    session.colorCapabilities = color;
    session.colorObservation = color;
    session.colorMonitorResult.status =
        bafx::windows::DisplayColorMonitorStatus::Active;
    session.colorMonitorResult.error = S_OK;
    session.colorMonitorResult.generation = 9U;
    session.colorSnapshotDisposition = "fresh";
    session.colorQueryGeneration = 4U;

    bafx::windows::DisplayRuntimeSummary summary{};
    summary.sessionCount = 1U;
    summary.topologyStatus =
        bafx::windows::DisplayTopologyStatus::Complete;
    summary.topologyError = ERROR_SUCCESS;
    summary.sessions.push_back(session);
    control.setDisplayRuntimeSummary(summary, 1U);

    bafx::windows::IpcClientOptions clientOptions{};
    clientOptions.pipeName = serverOptions.pipeName;
    clientOptions.timeoutMilliseconds = 1'000U;
    const bafx::windows::NamedPipeIpcClient client(clientOptions);
    const bafx::windows::IpcClientResponse response =
        client.transact("GetDisplayState");
    const bafx::desktop::DisplayStateSnapshot snapshot =
        control.displaySnapshot();

    color.advancedColorInfoV2 = false;
    session.colorCapabilities = color;
    session.colorObservation = color;
    summary.sessions.clear();
    summary.sessions.push_back(session);
    control.setDisplayRuntimeSummary(summary, 1U);
    const bafx::windows::IpcClientResponse legacyColorResponse =
        client.transact("GetDisplayState");
    control.stop();

    BAFX_CHECK(response.succeeded());
    BAFX_CHECK(response.payload.find("\"schemaVersion\":2")
        != std::string::npos);
    BAFX_CHECK(response.payload.find("\"runtimeGeneration\":1")
        != std::string::npos);
    BAFX_CHECK(response.payload.find("\"configGeneration\":1")
        != std::string::npos);
    BAFX_CHECK(response.payload.find("\"appliedConfigGeneration\":1")
        != std::string::npos);
    BAFX_CHECK(response.payload.find(
        "\"offlineOverridesAuthoritative\":true")
        != std::string::npos);
    BAFX_CHECK(response.payload.find(
        "\"displayKey\":\"displayconfig-v1-sha256:offline-display\"")
        != std::string::npos);
    BAFX_CHECK(response.payload.find("\"device\":\"\\\\\\\\.\\\\DISPLAY1\"")
        != std::string::npos);
    BAFX_CHECK(response.payload.find(
        "\"displayKey\":\"displayconfig-v1-sha256:test-display\"")
        != std::string::npos);
    BAFX_CHECK(response.payload.find("\"displayRefresh\":{\"numerator\":144000")
        != std::string::npos);
    BAFX_CHECK(response.payload.find("\"requestedOutput\":\"linear-scrgb\"")
        != std::string::npos);
    BAFX_CHECK(response.payload.find("\"resolvedOutput\":\"linear-scrgb\"")
        != std::string::npos);
    BAFX_CHECK(response.payload.find("\"actualOutput\":\"linear-scrgb\"")
        != std::string::npos);
    BAFX_CHECK(response.payload.find("\"hdrSupported\":true")
        != std::string::npos);
    BAFX_CHECK(response.payload.find("\"hdrActive\":true")
        != std::string::npos);
    BAFX_CHECK(response.payload.find("\"effectsEnabled\":false")
        != std::string::npos);
    BAFX_CHECK(response.payload.find("\"hdrEnabled\":true")
        != std::string::npos);
    BAFX_CHECK(response.payload.find("\"framePacing\":\"120\"")
        != std::string::npos);
    BAFX_CHECK(response.payload.find(
        "\"captureCadenceStatus\":\"target-rate\"")
        != std::string::npos);
    BAFX_CHECK(response.payload.find("\"producerAppliedPeriodUs\":6951")
        != std::string::npos);
    BAFX_CHECK(response.payload.find("\"hdrUserEnabled\":true")
        != std::string::npos);
    BAFX_CHECK(response.payload.find("\"sdrWhiteLevelNits\":203")
        != std::string::npos);
    BAFX_CHECK(response.payload.find("\"outputFallbackResult\":0")
        != std::string::npos);
    BAFX_CHECK(snapshot.runtimeGeneration == 1U);
    BAFX_CHECK(snapshot.configGeneration == 1U);
    BAFX_CHECK(snapshot.appliedConfigGeneration == 1U);
    BAFX_CHECK(snapshot.offlineOverridesAuthoritative);
    BAFX_CHECK(snapshot.offlineOverrides.size() == 1U);
    BAFX_CHECK(snapshot.offlineOverrides.front().displayKey
        == "displayconfig-v1-sha256:offline-display");
    BAFX_CHECK(snapshot.runtime.sessions.size() == 1U);
    BAFX_CHECK(snapshot.runtime.sessions.front().device == session.device);

    const bafx::control_center::DisplayStateParseResult parsed =
        bafx::control_center::parseDisplayState(response.payload);
    BAFX_CHECK(parsed.succeeded());
    BAFX_CHECK(parsed.state->schemaVersion == 2U);
    BAFX_CHECK(parsed.state->runtimeGeneration == 1U);
    BAFX_CHECK(parsed.state->configGeneration == 1U);
    BAFX_CHECK(parsed.state->offlineOverrides.size() == 1U);
    BAFX_CHECK(parsed.state->offlineOverrides.front().displayKey
        == "displayconfig-v1-sha256:offline-display");
    BAFX_CHECK(parsed.state->sessions.size() == 1U);
    BAFX_CHECK(parsed.state->sessions.front().colorQueryGeneration == 4U);
    BAFX_CHECK(parsed.state->sessions.front().physicalCadence.size() == 1U);

    std::string unknownSessionField = response.payload;
    unknownSessionField.insert(
        unknownSessionField.find("\"monitor\":"),
        "\"futureSessionField\":true,");
    BAFX_CHECK(!bafx::control_center::parseDisplayState(
        unknownSessionField).succeeded());

    std::string duplicateSessionField = response.payload;
    duplicateSessionField.insert(
        duplicateSessionField.find("\"monitor\":"),
        "\"monitor\":\"duplicate\",");
    BAFX_CHECK(!bafx::control_center::parseDisplayState(
        duplicateSessionField).succeeded());

    BAFX_CHECK(legacyColorResponse.succeeded());
    BAFX_CHECK(legacyColorResponse.payload.find("\"hdrSupported\":null")
        != std::string::npos);
    BAFX_CHECK(legacyColorResponse.payload.find("\"hdrUserEnabled\":null")
        != std::string::npos);
}

BAFX_TEST(host_control_fx_config_and_single_param_round_trip_over_ipc)
{
    TemporaryConfigDirectory temporary;
    bafx::config::Config initial = bafx::config::defaultConfig();
    initial.effects.opacity = 0.5F;
    initial.effects.clickTimeScale = 2.0F;
    initial.effects.trailTimeScale = 3.0F;
    initial.effects.trailLifetimeMs = 600.0F;
    initial.effects.trailLength = 2.0F;
    initial.effects.trailWidth = 2.0F;
    initial.effects.diskLifetimeMs = 350.0F;
    initial.effects.ringsCount = 4U;
    initial.effects.ringsLifetimeMs = 900.0F;
    initial.effects.ringsRadiusMin = 45.0F;
    initial.effects.ringsRadiusMax = 95.0F;
    initial.effects.ringsAngularVelocityMultiplier = 14.5F;
    initial.effects.ringsRotationDirection = 0.5F;
    initial.effects.bloomIntensity = 3.5F;
    initial.effects.bloomDiffusion = 8.0F;
    initial.input.trailOnlyWhilePressed = false;
    initial.input.samplingRateHz = 120U;

    bafx::windows::NamedPipeIpcServer::Options serverOptions{};
    serverOptions.pipeName = testPipeName() + L".fx-single";
    serverOptions.ioTimeoutMilliseconds = 500U;
    serverOptions.retryDelayMilliseconds = 10U;
    bafx::desktop::HostControlPlane control(
        temporary.configPath(),
        initial,
        serverOptions,
        bafx::desktop::HostSystemIntegration{},
        bafx::windows::recordingCompatibleAvailabilityForBuild(28000U));
    BAFX_CHECK(control.start(false).serviceStarted);

    bafx::windows::IpcClientOptions clientOptions{};
    clientOptions.pipeName = serverOptions.pipeName;
    clientOptions.timeoutMilliseconds = 1'000U;
    const bafx::windows::NamedPipeIpcClient client(clientOptions);

    const bafx::windows::IpcClientResponse fetched =
        client.transact("GetFxConfig");
    BAFX_CHECK(fetched.succeeded());
    BAFX_CHECK(fetched.payload == bafx::config::getFxConfig(initial, false));
    BAFX_CHECK(fetched.payload.find("\"schemaVersion\"") == std::string::npos);
    BAFX_CHECK(
        fetched.payload.find("\"trailOnlyWhilePressed\"")
        == std::string::npos);
    BAFX_CHECK(
        fetched.payload.find("\"samplingRateHz\"")
        == std::string::npos);
    BAFX_CHECK(fetched.payload.find("\"diskLifetimeMs\":350")
        != std::string::npos);
    BAFX_CHECK(fetched.payload.find("\"ringsCount\":4") != std::string::npos);
    BAFX_CHECK(fetched.payload.find("\"ringsRadiusMin\":45")
        != std::string::npos);
    BAFX_CHECK(fetched.payload.find("\"ringsRadiusMax\":95")
        != std::string::npos);
    BAFX_CHECK(fetched.payload.find("\"ringsAngularVelocityMultiplier\":14.5")
        != std::string::npos);
    BAFX_CHECK(fetched.payload.find("\"ringsRotationDirection\":0.5")
        != std::string::npos);

    const bafx::windows::IpcClientResponse changed = client.transact(
        "SetFxParam {\"generation\":1,\"path\":\"effects.diskLifetimeMs\",\"value\":500}");
    BAFX_CHECK(changed.succeeded());
    const bafx::desktop::HostStateSnapshot applied = control.snapshot();
    BAFX_CHECK(applied.generation == 2U);
    BAFX_CHECK_NEAR(applied.config.effects.diskLifetimeMs, 500.0F, 0.00001F);

    const auto persisted = bafx::config::loadConfig(temporary.configPath());
    BAFX_CHECK(persisted.status == bafx::config::ConfigStatus::Ok);
    BAFX_CHECK_NEAR(
        persisted.config.effects.diskLifetimeMs,
        500.0F,
        0.00001F);

    const bafx::windows::IpcClientResponse rejectedDisplay = client.transact(
        "SetFxParam {\"generation\":2,\"path\":\"display.hdrEnabled\",\"value\":true}");
    const bafx::desktop::HostStateSnapshot afterRejectedDisplay =
        control.snapshot();
    BAFX_CHECK(rejectedDisplay.transportSucceeded());
    BAFX_CHECK(!rejectedDisplay.succeeded());
    BAFX_CHECK(rejectedDisplay.errorCode == "invalid_fx_params");
    BAFX_CHECK(afterRejectedDisplay.generation == 2U);
    BAFX_CHECK(!afterRejectedDisplay.config.display.hdrEnabled);

    const bafx::windows::IpcClientResponse stale = client.transact(
        "SetFxParam {\"generation\":1,\"path\":\"effects.diskLifetimeMs\",\"value\":750}");
    const bafx::desktop::HostStateSnapshot afterStale = control.snapshot();
    control.stop();

    BAFX_CHECK(stale.transportSucceeded());
    BAFX_CHECK(!stale.succeeded());
    BAFX_CHECK(stale.errorCode == "generation_conflict");
    BAFX_CHECK(afterStale.generation == 2U);
    BAFX_CHECK_NEAR(afterStale.config.effects.diskLifetimeMs, 500.0F, 0.00001F);
}

BAFX_TEST(host_control_fx_batch_is_atomic_and_reset_preserves_other_sections)
{
    TemporaryConfigDirectory temporary;
    bafx::config::Config initial = bafx::config::defaultConfig();
    initial.background.mode = bafx::config::RenderMode::RecordingCompatible;
    initial.display.hdrEnabled = true;
    initial.input.trailOnlyWhilePressed = false;
    initial.input.samplingRateHz = 144U;
    initial.system.closeToTray = false;

    bafx::windows::NamedPipeIpcServer::Options serverOptions{};
    serverOptions.pipeName = testPipeName() + L".fx-batch";
    serverOptions.ioTimeoutMilliseconds = 500U;
    serverOptions.retryDelayMilliseconds = 10U;
    bafx::desktop::HostControlPlane control(
        temporary.configPath(),
        initial,
        serverOptions,
        bafx::desktop::HostSystemIntegration{},
        bafx::windows::recordingCompatibleAvailabilityForBuild(28000U));
    BAFX_CHECK(control.start(false).serviceStarted);

    bafx::windows::IpcClientOptions clientOptions{};
    clientOptions.pipeName = serverOptions.pipeName;
    clientOptions.timeoutMilliseconds = 1'000U;
    const bafx::windows::NamedPipeIpcClient client(clientOptions);

    const bafx::windows::IpcClientResponse changed = client.transact(
        "SetFxParams {\"generation\":1,\"patch\":{"
        "\"effects.opacity\":0.25,\"effects.clickTimeScale\":2,"
        "\"effects.trailTimeScale\":3,\"effects.trailLifetimeMs\":600,"
        "\"effects.trailWidth\":2,\"effects.diskLifetimeMs\":350,"
        "\"effects.ringsCount\":4,\"effects.ringsLifetimeMs\":900,"
        "\"effects.ringsRadiusMin\":45,\"effects.ringsRadiusMax\":95,"
        "\"effects.ringsAngularVelocityMultiplier\":14.5,"
        "\"effects.ringsRotationDirection\":0.5,"
        "\"effects.bloomIntensity\":4.2,\"effects.bloomDiffusion\":0,"
        "\"effects.bloomThreshold\":2,\"effects.bloomSoftKnee\":0.5,"
        "\"effects.bloomClamp\":4096}}");
    BAFX_CHECK(changed.succeeded());
    const bafx::desktop::HostStateSnapshot applied = control.snapshot();
    BAFX_CHECK(applied.generation == 2U);
    BAFX_CHECK(changed.payload == bafx::config::getFxConfig(applied.config, false));
    BAFX_CHECK_NEAR(applied.config.effects.opacity, 0.25F, 0.00001F);
    BAFX_CHECK_NEAR(applied.config.effects.clickTimeScale, 2.0F, 0.00001F);
    BAFX_CHECK_NEAR(applied.config.effects.trailTimeScale, 3.0F, 0.00001F);
    BAFX_CHECK_NEAR(applied.config.effects.trailLifetimeMs, 600.0F, 0.00001F);
    BAFX_CHECK_NEAR(applied.config.effects.trailLength, 2.0F, 0.00001F);
    BAFX_CHECK_NEAR(applied.config.effects.trailWidth, 2.0F, 0.00001F);
    BAFX_CHECK_NEAR(applied.config.effects.diskLifetimeMs, 350.0F, 0.00001F);
    BAFX_CHECK(applied.config.effects.ringsCount == 4U);
    BAFX_CHECK_NEAR(applied.config.effects.ringsLifetimeMs, 900.0F, 0.00001F);
    BAFX_CHECK_NEAR(applied.config.effects.ringsRadiusMin, 45.0F, 0.00001F);
    BAFX_CHECK_NEAR(applied.config.effects.ringsRadiusMax, 95.0F, 0.00001F);
    BAFX_CHECK_NEAR(
        applied.config.effects.ringsAngularVelocityMultiplier,
        14.5F,
        0.00001F);
    BAFX_CHECK_NEAR(
        applied.config.effects.ringsRotationDirection,
        0.5F,
        0.00001F);
    BAFX_CHECK_NEAR(applied.config.effects.bloomIntensity, 4.2F, 0.00001F);
    BAFX_CHECK_NEAR(applied.config.effects.bloomDiffusion, 0.0F, 0.00001F);
    BAFX_CHECK_NEAR(applied.config.effects.bloomThreshold, 2.0F, 0.00001F);
    BAFX_CHECK_NEAR(applied.config.effects.bloomSoftKnee, 0.5F, 0.00001F);
    BAFX_CHECK_NEAR(applied.config.effects.bloomClamp, 4096.0F, 0.00001F);

    const auto persistedBatch = bafx::config::loadConfig(temporary.configPath());
    BAFX_CHECK(persistedBatch.status == bafx::config::ConfigStatus::Ok);
    BAFX_CHECK_NEAR(persistedBatch.config.effects.opacity, 0.25F, 0.00001F);
    BAFX_CHECK_NEAR(
        persistedBatch.config.effects.trailLifetimeMs,
        600.0F,
        0.00001F);
    BAFX_CHECK_NEAR(
        persistedBatch.config.effects.diskLifetimeMs,
        350.0F,
        0.00001F);
    BAFX_CHECK(persistedBatch.config.effects.ringsCount == 4U);
    BAFX_CHECK_NEAR(
        persistedBatch.config.effects.ringsLifetimeMs,
        900.0F,
        0.00001F);
    BAFX_CHECK_NEAR(
        persistedBatch.config.effects.ringsRadiusMin,
        45.0F,
        0.00001F);
    BAFX_CHECK_NEAR(
        persistedBatch.config.effects.ringsRadiusMax,
        95.0F,
        0.00001F);
    BAFX_CHECK_NEAR(
        persistedBatch.config.effects.ringsAngularVelocityMultiplier,
        14.5F,
        0.00001F);
    BAFX_CHECK_NEAR(
        persistedBatch.config.effects.ringsRotationDirection,
        0.5F,
        0.00001F);
    BAFX_CHECK_NEAR(
        persistedBatch.config.effects.bloomIntensity,
        4.2F,
        0.00001F);

    const bafx::windows::IpcClientResponse rejected = client.transact(
        "SetFxParams {\"generation\":2,\"patch\":{"
        "\"effects.opacity\":0.75,\"effects.bloomSoftKnee\":2}}");
    const bafx::desktop::HostStateSnapshot afterRejected = control.snapshot();
    BAFX_CHECK(rejected.transportSucceeded());
    BAFX_CHECK(!rejected.succeeded());
    BAFX_CHECK(rejected.errorCode == "invalid_fx_params");
    BAFX_CHECK(afterRejected.generation == 2U);
    BAFX_CHECK_NEAR(afterRejected.config.effects.opacity, 0.25F, 0.00001F);
    BAFX_CHECK_NEAR(afterRejected.config.effects.bloomSoftKnee, 0.5F, 0.00001F);
    const auto persistedAfterRejected = bafx::config::loadConfig(
        temporary.configPath());
    BAFX_CHECK(
        persistedAfterRejected.status == bafx::config::ConfigStatus::Ok);
    BAFX_CHECK_NEAR(
        persistedAfterRejected.config.effects.opacity,
        0.25F,
        0.00001F);
    BAFX_CHECK_NEAR(
        persistedAfterRejected.config.effects.bloomSoftKnee,
        0.5F,
        0.00001F);

    const bafx::windows::IpcClientResponse rejectedProductPath = client.transact(
        "SetFxParams {\"generation\":2,\"patch\":{"
        "\"effects.opacity\":0.75,\"input.samplingRateHz\":30}}");
    const bafx::desktop::HostStateSnapshot afterRejectedProductPath =
        control.snapshot();
    BAFX_CHECK(rejectedProductPath.transportSucceeded());
    BAFX_CHECK(!rejectedProductPath.succeeded());
    BAFX_CHECK(rejectedProductPath.errorCode == "invalid_fx_params");
    BAFX_CHECK(afterRejectedProductPath.generation == 2U);
    BAFX_CHECK_NEAR(
        afterRejectedProductPath.config.effects.opacity,
        0.25F,
        0.00001F);
    BAFX_CHECK(afterRejectedProductPath.config.input.samplingRateHz == 144U);

    const bafx::windows::IpcClientResponse reset =
        client.transact("ResetFxConfig");
    const bafx::desktop::HostStateSnapshot afterReset = control.snapshot();
    control.stop();

    BAFX_CHECK(reset.succeeded());
    BAFX_CHECK(afterReset.generation == 3U);
    BAFX_CHECK(reset.payload == bafx::config::getFxConfig(afterReset.config, false));
    const bafx::config::Config defaults = bafx::config::defaultConfig();
    checkEffectsEqual(afterReset.config.effects, defaults.effects);

    BAFX_CHECK(
        afterReset.config.background.mode
        == bafx::config::RenderMode::RecordingCompatible);
    BAFX_CHECK(afterReset.config.display.hdrEnabled);
    BAFX_CHECK(!afterReset.config.input.trailOnlyWhilePressed);
    BAFX_CHECK(afterReset.config.input.samplingRateHz == 144U);
    BAFX_CHECK(!afterReset.config.system.closeToTray);

    const auto persisted = bafx::config::loadConfig(temporary.configPath());
    BAFX_CHECK(persisted.status == bafx::config::ConfigStatus::Ok);
    checkEffectsEqual(persisted.config.effects, defaults.effects);
    BAFX_CHECK(
        persisted.config.background.mode
        == bafx::config::RenderMode::RecordingCompatible);
    BAFX_CHECK(persisted.config.display.hdrEnabled);
    BAFX_CHECK(!persisted.config.input.trailOnlyWhilePressed);
    BAFX_CHECK(persisted.config.input.samplingRateHz == 144U);
    BAFX_CHECK(!persisted.config.system.closeToTray);
}
