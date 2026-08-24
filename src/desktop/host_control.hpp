#pragma once

#include "fx_profile_store.hpp"

#include "bafx/config/config.hpp"
#include "bafx/windows/ipc.hpp"
#include "bafx/windows/recording_compatibility.hpp"
#include "bafx/windows/runtime_diagnostics.hpp"
#include "bafx/windows/spout2_sender.hpp"
#include "bafx/windows/startup_registration.hpp"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace bafx::desktop
{

struct Spout2RuntimeState final
{
    bool enabled{false};
    std::string sender{"ba-click-fx-desktop"};
    std::string status{"disabled"};
    std::string error{};
    std::string outputContract{bafx::windows::spout2OutputContract};
};

struct HostRuntimeSnapshot final
{
    bafx::config::Config config{};
    std::uint64_t configGeneration{0U};
    bool paused{false};
    bool shutdownRequested{false};
};

struct HostStateSnapshot final
{
    bafx::config::Config config{};
    std::uint64_t generation{0U};
    // Control generation protects all IPC mutations. Config generation only
    // advances when the immutable render configuration actually changes.
    std::uint64_t configGeneration{0U};
    bool paused{false};
    bool shutdownRequested{false};
    bool backgroundCaptureActive{false};
    Spout2RuntimeState spout2{};
    std::vector<FxProfileSummary> fxProfiles{};
    std::string activeFxProfile{"自定义"};
    std::string fxProfileWarning{};
};

struct HostControlStartResult final
{
    std::uint64_t appliedGeneration{0U};
    bool serviceStarted{false};
};

enum class HostSystemIntegrationPhase
{
    Apply,
    Compensate
};

struct HostSystemIntegration final
{
    using ApplyStartupRegistration =
        bafx::windows::StartupRegistrationResult (*)(
            const void* context,
            const bafx::config::SystemConfig& system,
            HostSystemIntegrationPhase phase) noexcept;

    const void* context{nullptr};
    ApplyStartupRegistration applyStartupRegistration{nullptr};
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
    HostControlPlane(
        std::filesystem::path configPath,
        bafx::config::Config initialConfig,
        HostSystemIntegration systemIntegration);
    HostControlPlane(
        std::filesystem::path configPath,
        bafx::config::Config initialConfig,
        bafx::windows::NamedPipeIpcServer::Options ipcOptions,
        HostSystemIntegration systemIntegration);
    HostControlPlane(
        std::filesystem::path configPath,
        bafx::config::Config initialConfig,
        bafx::windows::NamedPipeIpcServer::Options ipcOptions,
        HostSystemIntegration systemIntegration,
        bafx::windows::RecordingCompatibleAvailability
            recordingCompatibleAvailability);
    ~HostControlPlane();

    HostControlPlane(const HostControlPlane&) = delete;
    HostControlPlane& operator=(const HostControlPlane&) = delete;

    // Publish the initialized renderer state and latch its configuration
    // generation before the pipe can accept the first control request.
    [[nodiscard]] HostControlStartResult start(
        bool backgroundCaptureActive) noexcept;
    void stop() noexcept;

    // The render loop must not materialize IPC-only Profile state or transport
    // strings while it samples immutable runtime control values.
    [[nodiscard]] HostRuntimeSnapshot runtimeSnapshot() const;
    [[nodiscard]] HostStateSnapshot snapshot() const;
    [[nodiscard]] DisplayStateSnapshot displaySnapshot() const;
    void setBackgroundCaptureActive(bool active) noexcept;
    // Runtime transport health is observational state. Publishing it must not
    // invalidate optimistic config generations used by SetConfig clients.
    void setSpout2RuntimeState(
        bool enabled,
        std::string_view sender,
        bafx::windows::Spout2SenderStatus status,
        std::string_view error);
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
    [[nodiscard]] bafx::windows::IpcResponse handleFxProfileMutation(
        std::string_view payload,
        bafx::windows::IpcCommand command) noexcept;
    [[nodiscard]] bafx::windows::IpcResponse handleClearLogs() noexcept;
    [[nodiscard]] HostStateSnapshot snapshotLocked() const;
    [[nodiscard]] static std::string stateJson(const HostStateSnapshot& state);
    [[nodiscard]] static std::string displayStateJson(
        const DisplayStateSnapshot& state);
    void normalizeRecordingCompatibleStartup() noexcept;
    void appendRecordingCompatibleDiagnostic(
        std::string_view eventName,
        std::string_view requestedMode,
        std::string_view effectiveMode,
        std::string_view reason) const noexcept;

    mutable std::mutex mutex_{};
    std::filesystem::path configPath_{};
    FxProfileStore fxProfileStore_;
    bafx::config::Config config_{};
    std::uint64_t generation_{1U};
    std::uint64_t configGeneration_{1U};
    bafx::windows::DisplayRuntimeSummary displayRuntimeSummary_{};
    std::chrono::steady_clock::time_point displayRuntimePublishedAt_{};
    std::uint64_t displayRuntimeGeneration_{0U};
    std::uint64_t appliedConfigGeneration_{0U};
    bool paused_{false};
    bool backgroundCaptureActive_{false};
    Spout2RuntimeState spout2RuntimeState_{};
    bafx::windows::RecordingCompatibleAvailability
        recordingCompatibleAvailability_{};
    HostSystemIntegration systemIntegration_{};
    bafx::windows::NamedPipeIpcServer ipc_;
};

[[nodiscard]] std::filesystem::path defaultConfigPath();

}
