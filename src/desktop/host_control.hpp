#pragma once

#include "bafx/config/config.hpp"
#include "bafx/windows/ipc.hpp"
#include "bafx/windows/runtime_diagnostics.hpp"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace bafx::desktop
{

struct HostStateSnapshot final
{
    bafx::config::Config config{};
    std::uint64_t generation{0U};
    bool paused{false};
    bool shutdownRequested{false};
    bool backgroundCaptureActive{false};
};

struct HostControlStartResult final
{
    std::uint64_t appliedGeneration{0U};
    bool serviceStarted{false};
};

struct DisplayStateSnapshot final
{
    bafx::windows::DisplayRuntimeSummary runtime{};
    std::vector<bafx::config::DisplayOverrideConfig> offlineOverrides{};
    std::uint64_t runtimeGeneration{0U};
    std::uint64_t configGeneration{0U};
    std::uint64_t appliedConfigGeneration{0U};
    bool offlineOverridesAuthoritative{false};
};

class SingleInstanceGuard final
{
public:
    explicit SingleInstanceGuard(std::wstring name);
    ~SingleInstanceGuard();

    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

    [[nodiscard]] bool acquire() noexcept;
    [[nodiscard]] bool alreadyRunning() const noexcept;
    [[nodiscard]] DWORD lastError() const noexcept;

private:
    std::wstring name_{};
    HANDLE mutex_{nullptr};
    bool alreadyRunning_{false};
    DWORD lastError_{ERROR_SUCCESS};
};

class HostControlPlane final
{
public:
    HostControlPlane(
        std::filesystem::path configPath,
        bafx::config::Config initialConfig);
    HostControlPlane(
        std::filesystem::path configPath,
        bafx::config::Config initialConfig,
        bafx::windows::NamedPipeIpcServer::Options ipcOptions);
    ~HostControlPlane();

    HostControlPlane(const HostControlPlane&) = delete;
    HostControlPlane& operator=(const HostControlPlane&) = delete;

    // Publish the initialized renderer state and latch its configuration
    // generation before the pipe can accept the first control request.
    [[nodiscard]] HostControlStartResult start(
        bool backgroundCaptureActive) noexcept;
    void stop() noexcept;

    [[nodiscard]] HostStateSnapshot snapshot() const;
    [[nodiscard]] DisplayStateSnapshot displaySnapshot() const;
    void setBackgroundCaptureActive(bool active) noexcept;
    // The render owner publishes one immutable cross-display view. Pipe
    // clients never inspect live renderer or WGC objects from the IPC thread.
    void setDisplayRuntimeSummary(
        bafx::windows::DisplayRuntimeSummary summary,
        std::uint64_t appliedConfigGeneration);
    [[nodiscard]] DWORD ipcLastError() const noexcept;

private:
    [[nodiscard]] bafx::windows::IpcResponse handle(
        const bafx::windows::IpcRequest& request) noexcept;
    [[nodiscard]] bafx::windows::IpcResponse handleSetConfig(
        std::string_view payload) noexcept;
    [[nodiscard]] bafx::windows::IpcResponse handleDisplayOverrideMutation(
        std::string_view payload,
        bool remove) noexcept;
    [[nodiscard]] bafx::windows::IpcResponse handleSetFxParams(
        std::string_view payload,
        bool batch) noexcept;
    [[nodiscard]] bafx::windows::IpcResponse handleResetFxConfig() noexcept;
    [[nodiscard]] static std::string stateJson(const HostStateSnapshot& state);
    [[nodiscard]] static std::string displayStateJson(
        const DisplayStateSnapshot& state);

    mutable std::mutex mutex_{};
    std::filesystem::path configPath_{};
    bafx::config::Config config_{};
    std::uint64_t generation_{1U};
    bafx::windows::DisplayRuntimeSummary displayRuntimeSummary_{};
    std::uint64_t displayRuntimeGeneration_{0U};
    std::uint64_t appliedConfigGeneration_{0U};
    bool paused_{false};
    bool backgroundCaptureActive_{false};
    bafx::windows::NamedPipeIpcServer ipc_;
};

[[nodiscard]] std::filesystem::path defaultConfigPath();

}
