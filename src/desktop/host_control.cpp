#include "host_control.hpp"

#include "bafx/windows/portable_paths.hpp"

#include <sstream>
#include <utility>

namespace bafx::desktop
{
namespace
{

[[nodiscard]] std::string jsonBool(const bool value)
{
    return value ? "true" : "false";
}

[[nodiscard]] std::string jsonEscape(const std::string_view value)
{
    std::string result;
    result.reserve(value.size() + 2U);
    result.push_back('"');
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (character < 0x20U)
            {
                result += "\\u00";
                constexpr char hex[] = "0123456789abcdef";
                result.push_back(hex[character >> 4U]);
                result.push_back(hex[character & 0x0FU]);
            }
            else
            {
                result.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    result.push_back('"');
    return result;
}

[[nodiscard]] std::string statusName(const bool active)
{
    return active ? "active" : "fallback-fx-only";
}

}

SingleInstanceGuard::SingleInstanceGuard(std::wstring name)
    : name_(std::move(name))
{
}

SingleInstanceGuard::~SingleInstanceGuard()
{
    if (mutex_ != nullptr)
    {
        CloseHandle(mutex_);
        mutex_ = nullptr;
    }
}

bool SingleInstanceGuard::acquire() noexcept
{
    if (mutex_ != nullptr)
    {
        return !alreadyRunning_;
    }
    if (name_.empty())
    {
        lastError_ = ERROR_INVALID_NAME;
        return false;
    }
    // CreateMutexW only promises ERROR_ALREADY_EXISTS for an existing named
    // object; clear a stale thread error so a newly created mutex is not
    // misclassified as another Host instance.
    SetLastError(ERROR_SUCCESS);
    mutex_ = CreateMutexW(nullptr, TRUE, name_.c_str());
    if (mutex_ == nullptr)
    {
        lastError_ = GetLastError();
        return false;
    }
    lastError_ = GetLastError();
    alreadyRunning_ = lastError_ == ERROR_ALREADY_EXISTS;
    if (alreadyRunning_)
    {
        CloseHandle(mutex_);
        mutex_ = nullptr;
        return false;
    }
    return true;
}

bool SingleInstanceGuard::alreadyRunning() const noexcept
{
    return alreadyRunning_;
}

DWORD SingleInstanceGuard::lastError() const noexcept
{
    return lastError_;
}

HostControlPlane::HostControlPlane(
    std::filesystem::path configPath,
    bafx::config::Config initialConfig)
    : HostControlPlane(
          std::move(configPath),
          std::move(initialConfig),
          bafx::windows::NamedPipeIpcServer::Options{})
{
}

HostControlPlane::HostControlPlane(
    std::filesystem::path configPath,
    bafx::config::Config initialConfig,
    bafx::windows::NamedPipeIpcServer::Options ipcOptions)
    : configPath_(std::move(configPath))
    , config_(std::move(initialConfig))
    , ipc_([this](const bafx::windows::IpcRequest& request)
           {
               return handle(request);
           },
           std::move(ipcOptions))
{
}

HostControlPlane::~HostControlPlane()
{
    stop();
}

HostControlStartResult HostControlPlane::start(
    const bool backgroundCaptureActive) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    backgroundCaptureActive_ = backgroundCaptureActive;
    const std::uint64_t appliedGeneration = generation_;
    // Keep the mutex until the server thread exists. An immediate SetConfig
    // may advance the generation only after this applied baseline is latched.
    const bool serviceStarted = ipc_.start();
    return HostControlStartResult{appliedGeneration, serviceStarted};
}

void HostControlPlane::stop() noexcept
{
    ipc_.stop();
}

HostStateSnapshot HostControlPlane::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return HostStateSnapshot{
        config_,
        generation_,
        paused_,
        shutdownRequested_.load(std::memory_order_acquire),
        backgroundCaptureActive_};
}

void HostControlPlane::setBackgroundCaptureActive(const bool active) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    backgroundCaptureActive_ = active;
}

DWORD HostControlPlane::ipcLastError() const noexcept
{
    return ipc_.lastError();
}

bafx::windows::IpcResponse HostControlPlane::handle(
    const bafx::windows::IpcRequest& request) noexcept
{
    try
    {
        switch (request.command)
        {
        case bafx::windows::IpcCommand::GetState:
            return bafx::windows::IpcResponse::success(stateJson(snapshot()));

        case bafx::windows::IpcCommand::GetConfig:
        {
            const HostStateSnapshot state = snapshot();
            return bafx::windows::IpcResponse::success(
                bafx::config::toJson(state.config, false));
        }

        case bafx::windows::IpcCommand::SetConfig:
            return handleSetConfig(request.payload);

        case bafx::windows::IpcCommand::Pause:
        {
            std::lock_guard<std::mutex> lock(mutex_);
            paused_ = true;
            ++generation_;
            return bafx::windows::IpcResponse::success(stateJson(HostStateSnapshot{
                config_,
                generation_,
                paused_,
                shutdownRequested_.load(std::memory_order_acquire),
                backgroundCaptureActive_}));
        }

        case bafx::windows::IpcCommand::Resume:
        {
            std::lock_guard<std::mutex> lock(mutex_);
            paused_ = false;
            ++generation_;
            return bafx::windows::IpcResponse::success(stateJson(HostStateSnapshot{
                config_,
                generation_,
                paused_,
                shutdownRequested_.load(std::memory_order_acquire),
                backgroundCaptureActive_}));
        }

        case bafx::windows::IpcCommand::Shutdown:
            shutdownRequested_.store(true, std::memory_order_release);
            return bafx::windows::IpcResponse::success(
                "{\"shutdownRequested\":true}");
        }
    }
    catch (...)
    {
        return bafx::windows::IpcResponse::failure(
            "handler_error",
            "host control handler failed");
    }
    return bafx::windows::IpcResponse::failure(
        "unknown_command",
        "command is not supported");
}

bafx::windows::IpcResponse HostControlPlane::handleSetConfig(
    const std::string_view payload) noexcept
{
    bafx::config::Config baseConfig;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        baseConfig = config_;
    }

    const bafx::config::ConfigPatchResult patch =
        bafx::config::applyPatchJson(baseConfig, payload);
    bafx::config::Config candidate{};
    std::optional<std::uint64_t> expectedGeneration;
    if (patch.recognized)
    {
        if (!patch.succeeded())
        {
            return bafx::windows::IpcResponse::failure(
                "invalid_config",
                patch.message.empty() ? "configuration patch is invalid" : patch.message);
        }
        candidate = patch.config;
        expectedGeneration = patch.expectedGeneration;
    }
    else
    {
        const bafx::config::ConfigLoadResult parsed = bafx::config::parseJson(payload);
        if (!parsed.succeeded())
        {
            return bafx::windows::IpcResponse::failure(
                "invalid_config",
                parsed.message.empty() ? "configuration is invalid" : parsed.message);
        }
        candidate = parsed.config;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (expectedGeneration.has_value() && *expectedGeneration != generation_)
    {
        return bafx::windows::IpcResponse::failure(
            "generation_conflict",
            "configuration generation changed; refresh before retrying");
    }
    const bafx::config::ConfigSaveResult saved =
        bafx::config::saveConfigAtomic(configPath_, candidate);
    if (!saved.succeeded())
    {
        return bafx::windows::IpcResponse::failure(
            "config_write_failed",
            saved.message.empty() ? "configuration could not be saved" : saved.message);
    }
    config_ = candidate;
    ++generation_;
    return bafx::windows::IpcResponse::success(
        bafx::config::toJson(config_, false));
}

std::string HostControlPlane::stateJson(const HostStateSnapshot& state)
{
    std::ostringstream stream;
    stream << "{\"generation\":" << state.generation
           << ",\"paused\":" << jsonBool(state.paused)
           << ",\"shutdownRequested\":" << jsonBool(state.shutdownRequested)
           << ",\"effectsEnabled\":" << jsonBool(state.config.effects.enabled)
           << ",\"backgroundCapture\":"
           << jsonEscape(statusName(state.backgroundCaptureActive))
           << ",\"captureMode\":"
           << jsonEscape(bafx::config::toString(state.config.background.mode))
           << "}";
    return stream.str();
}

std::filesystem::path defaultConfigPath()
{
    return bafx::windows::executableFilePath(
        L"BAFX.config.json",
        L"BAFX.config.json");
}

}
