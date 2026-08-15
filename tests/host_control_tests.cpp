#include "test_support.hpp"

#include "host_control.hpp"

#include "bafx/windows/ipc_client.hpp"

#include <windows.h>

#include <chrono>
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
    const bafx::desktop::HostStateSnapshot stopped = control.snapshot();
    control.stop();

    BAFX_CHECK(response.succeeded());
    BAFX_CHECK(response.payload == "{\"shutdownRequested\":true}");
    BAFX_CHECK(stopped.shutdownRequested);
}
