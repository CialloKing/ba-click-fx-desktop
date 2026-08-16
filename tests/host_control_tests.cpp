#include "test_support.hpp"

#include "host_control.hpp"

#include "bafx/windows/ipc_client.hpp"

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>

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

private:
    std::filesystem::path path_{};
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
    BAFX_CHECK_NEAR(actual.globalScale, expected.globalScale, 0.00001F);
    BAFX_CHECK_NEAR(actual.opacity, expected.opacity, 0.00001F);
    BAFX_CHECK(actual.clickEnabled == expected.clickEnabled);
    BAFX_CHECK(actual.trailEnabled == expected.trailEnabled);
    BAFX_CHECK_NEAR(actual.trailLength, expected.trailLength, 0.00001F);
    BAFX_CHECK_NEAR(actual.trailWidth, expected.trailWidth, 0.00001F);
    BAFX_CHECK_NEAR(actual.clickTimeScale, expected.clickTimeScale, 0.00001F);
    BAFX_CHECK_NEAR(actual.trailTimeScale, expected.trailTimeScale, 0.00001F);
    BAFX_CHECK_NEAR(actual.trailLifetimeMs, expected.trailLifetimeMs, 0.00001F);
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
        serverOptions);

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
        serverOptions);
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
    BAFX_CHECK(fetched.payload.find("\"trailAlways\":true") != std::string::npos);
    BAFX_CHECK(
        fetched.payload.find("\"inputSamplingRate\":120")
        != std::string::npos);

    const bafx::windows::IpcClientResponse changed = client.transact(
        "SetFxParam {\"generation\":1,\"path\":\"opacity\",\"value\":0.25}");
    BAFX_CHECK(changed.succeeded());
    const bafx::desktop::HostStateSnapshot applied = control.snapshot();
    BAFX_CHECK(applied.generation == 2U);
    BAFX_CHECK_NEAR(applied.config.effects.opacity, 0.25F, 0.00001F);

    const auto persisted = bafx::config::loadConfig(temporary.configPath());
    BAFX_CHECK(persisted.status == bafx::config::ConfigStatus::Ok);
    BAFX_CHECK_NEAR(persisted.config.effects.opacity, 0.25F, 0.00001F);

    const bafx::windows::IpcClientResponse stale = client.transact(
        "SetFxParam {\"generation\":1,\"path\":\"opacity\",\"value\":0.75}");
    const bafx::desktop::HostStateSnapshot afterStale = control.snapshot();
    control.stop();

    BAFX_CHECK(stale.transportSucceeded());
    BAFX_CHECK(!stale.succeeded());
    BAFX_CHECK(stale.errorCode == "generation_conflict");
    BAFX_CHECK(afterStale.generation == 2U);
    BAFX_CHECK_NEAR(afterStale.config.effects.opacity, 0.25F, 0.00001F);
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
        serverOptions);
    BAFX_CHECK(control.start(false).serviceStarted);

    bafx::windows::IpcClientOptions clientOptions{};
    clientOptions.pipeName = serverOptions.pipeName;
    clientOptions.timeoutMilliseconds = 1'000U;
    const bafx::windows::NamedPipeIpcClient client(clientOptions);

    const bafx::windows::IpcClientResponse changed = client.transact(
        "SetFxParams {\"generation\":1,\"patch\":{"
        "\"opacity\":0.25,\"clickTimeScale\":2,\"trailTimeScale\":3,"
        "\"trail.lifetimeMs\":600,\"trail.width\":5.4,"
        "\"bloom.intensity\":4.2,\"bloom.diffusion\":0,"
        "\"bloom.threshold\":2,\"bloom.softKnee\":0.5,"
        "\"bloom.clamp\":4096}}");
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
        persistedBatch.config.effects.bloomIntensity,
        4.2F,
        0.00001F);

    const bafx::windows::IpcClientResponse rejected = client.transact(
        "SetFxParams {\"generation\":2,\"patch\":{"
        "\"opacity\":0.75,\"bloom.softKnee\":2}}");
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
