#pragma once

#include "bafx/config/config.hpp"
#include "bafx/windows/ipc.hpp"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

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
    void setBackgroundCaptureActive(bool active) noexcept;
    [[nodiscard]] DWORD ipcLastError() const noexcept;

private:
    [[nodiscard]] bafx::windows::IpcResponse handle(
        const bafx::windows::IpcRequest& request) noexcept;
    [[nodiscard]] bafx::windows::IpcResponse handleSetConfig(
        std::string_view payload) noexcept;
    [[nodiscard]] static std::string stateJson(const HostStateSnapshot& state);

    mutable std::mutex mutex_{};
    std::filesystem::path configPath_{};
    bafx::config::Config config_{};
    std::uint64_t generation_{1U};
    bool paused_{false};
    bool backgroundCaptureActive_{false};
    bafx::windows::NamedPipeIpcServer ipc_;
};

[[nodiscard]] std::filesystem::path defaultConfigPath();

}
