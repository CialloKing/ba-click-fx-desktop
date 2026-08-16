#include "host_control.hpp"

#include "bafx/windows/portable_paths.hpp"

#include <limits>
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

[[nodiscard]] std::string wideToUtf8(const std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }
    if (value.size()
        > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return {};
    }

    const int characterCount = static_cast<int>(value.size());
    const int byteCount = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        characterCount,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (byteCount <= 0)
    {
        return {};
    }

    std::string result(static_cast<std::size_t>(byteCount), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        characterCount,
        result.data(),
        byteCount,
        nullptr,
        nullptr);
    if (converted != byteCount)
    {
        return {};
    }
    return result;
}

[[nodiscard]] std::string_view outputPreferenceName(
    const bafx::windows::CompositionOutputPreference preference) noexcept
{
    switch (preference)
    {
    case bafx::windows::CompositionOutputPreference::ConservativeSdr:
        return "conservative-sdr";
    case bafx::windows::CompositionOutputPreference::PreferLinearScRgb:
        return "linear-scrgb";
    }
    return "unknown";
}

[[nodiscard]] std::string_view driverTypeName(
    const bafx::windows::GraphicsDriverType driver) noexcept
{
    switch (driver)
    {
    case bafx::windows::GraphicsDriverType::Hardware:
        return "hardware";
    case bafx::windows::GraphicsDriverType::Warp:
        return "warp";
    }
    return "unknown";
}

void appendOptionalBoolean(
    std::ostringstream& stream,
    const std::optional<bool> value)
{
    if (!value.has_value())
    {
        stream << "null";
        return;
    }
    stream << jsonBool(*value);
}

void appendRefreshRate(
    std::ostringstream& stream,
    const std::optional<bafx::windows::DisplayRefreshRate>& refreshRate)
{
    if (!refreshRate.has_value()
        || refreshRate->numerator == 0U
        || refreshRate->denominator == 0U)
    {
        stream << "null";
        return;
    }
    stream << "{\"numerator\":" << refreshRate->numerator
           << ",\"denominator\":" << refreshRate->denominator
           << "}";
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
        ipc_.stopRequested(),
        backgroundCaptureActive_};
}

DisplayStateSnapshot HostControlPlane::displaySnapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return DisplayStateSnapshot{
        displayRuntimeSummary_,
        displayRuntimeGeneration_};
}

void HostControlPlane::setBackgroundCaptureActive(const bool active) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    backgroundCaptureActive_ = active;
}

void HostControlPlane::setDisplayRuntimeSummary(
    bafx::windows::DisplayRuntimeSummary summary)
{
    std::lock_guard<std::mutex> lock(mutex_);
    displayRuntimeSummary_ = std::move(summary);
    ++displayRuntimeGeneration_;
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

        case bafx::windows::IpcCommand::GetDisplayState:
            return bafx::windows::IpcResponse::success(
                displayStateJson(displaySnapshot()));

        case bafx::windows::IpcCommand::GetConfig:
        {
            const HostStateSnapshot state = snapshot();
            return bafx::windows::IpcResponse::success(
                bafx::config::toJson(state.config, false));
        }

        case bafx::windows::IpcCommand::GetFxConfig:
        {
            const HostStateSnapshot state = snapshot();
            return bafx::windows::IpcResponse::success(
                bafx::config::getFxConfig(state.config, false));
        }

        case bafx::windows::IpcCommand::SetConfig:
            return handleSetConfig(request.payload);

        case bafx::windows::IpcCommand::SetFxParam:
            return handleSetFxParams(request.payload, false);

        case bafx::windows::IpcCommand::SetFxParams:
            return handleSetFxParams(request.payload, true);

        case bafx::windows::IpcCommand::ResetFxConfig:
            return handleResetFxConfig();

        case bafx::windows::IpcCommand::Pause:
        {
            std::lock_guard<std::mutex> lock(mutex_);
            paused_ = true;
            ++generation_;
            return bafx::windows::IpcResponse::success(stateJson(HostStateSnapshot{
                config_,
                generation_,
                paused_,
                ipc_.stopRequested(),
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
                ipc_.stopRequested(),
                backgroundCaptureActive_}));
        }

        case bafx::windows::IpcCommand::Shutdown:
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

bafx::windows::IpcResponse HostControlPlane::handleSetFxParams(
    const std::string_view payload,
    const bool batch) noexcept
{
    bafx::config::Config baseConfig;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        baseConfig = config_;
    }

    bafx::config::Config candidate{};
    std::optional<std::uint64_t> expectedGeneration;
    if (batch)
    {
        const bafx::config::ConfigBatchPatchResult patch =
            bafx::config::setFxParams(baseConfig, payload);
        if (!patch.succeeded())
        {
            return bafx::windows::IpcResponse::failure(
                "invalid_fx_params",
                patch.message.empty()
                    ? "FX parameter patch is invalid"
                    : patch.message);
        }
        candidate = patch.config;
        expectedGeneration = patch.expectedGeneration;
    }
    else
    {
        const bafx::config::ConfigPatchResult patch =
            bafx::config::applyFxPatchJson(baseConfig, payload);
        if (!patch.succeeded())
        {
            return bafx::windows::IpcResponse::failure(
                "invalid_fx_params",
                patch.message.empty()
                    ? "FX parameter patch is invalid"
                    : patch.message);
        }
        candidate = patch.config;
        expectedGeneration = patch.expectedGeneration;
    }
    if (!expectedGeneration.has_value())
    {
        return bafx::windows::IpcResponse::failure(
            "invalid_fx_params",
            "FX parameter patch generation is required");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (*expectedGeneration != generation_)
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
        bafx::config::getFxConfig(config_, false));
}

bafx::windows::IpcResponse HostControlPlane::handleResetFxConfig() noexcept
{
    bafx::config::Config candidate;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        candidate = config_;
    }
    candidate.effects = bafx::config::resetFxConfig().effects;
    std::lock_guard<std::mutex> lock(mutex_);
    const bafx::config::ConfigSaveResult saved =
        bafx::config::saveConfigAtomic(configPath_, candidate);
    if (!saved.succeeded())
    {
        return bafx::windows::IpcResponse::failure(
            "config_write_failed",
            saved.message.empty() ? "FX defaults could not be saved" : saved.message);
    }
    config_ = candidate;
    ++generation_;
    return bafx::windows::IpcResponse::success(
        bafx::config::getFxConfig(config_, false));
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
        if (!patch.expectedGeneration.has_value())
        {
            // A path patch is a read-modify-write transaction. Requiring the
            // observed generation prevents stale clients from overwriting a
            // newer configuration snapshot without detecting the conflict.
            return bafx::windows::IpcResponse::failure(
                "invalid_config",
                "configuration patch generation is required");
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

std::string HostControlPlane::displayStateJson(
    const DisplayStateSnapshot& state)
{
    std::ostringstream stream;
    stream << "{\"generation\":" << state.generation
           << ",\"sessions\":[";
    for (std::size_t index = 0U;
         index < state.runtime.sessions.size();
         ++index)
    {
        if (index != 0U)
        {
            stream << ',';
        }

        const bafx::windows::DisplaySessionRuntimeSummary& session =
            state.runtime.sessions[index];
        const bool colorComplete = session.colorCapabilities.has_value()
            && bafx::windows::displayColorStateComplete(
                *session.colorCapabilities);
        const std::optional<bool> hdrSupported = colorComplete
            ? std::optional<bool>(
                session.colorCapabilities->highDynamicRangeSupported)
            : std::nullopt;
        const std::optional<bool> hdrActive = colorComplete
            ? std::optional<bool>(
                session.colorCapabilities->activeColorMode
                    == bafx::windows::DisplayColorMode::Hdr
                && (!session.colorCapabilities->displayPathResolved
                    || session.colorCapabilities->advancedColorActive))
            : std::nullopt;
        const std::optional<bafx::windows::CompositionOutputPreference>
            actualOutput =
                bafx::windows::effectiveCompositionOutputPreference(
                    session.deviceInfo.output);

        stream << "{\"monitor\":" << jsonEscape(session.monitor)
               << ",\"device\":" << jsonEscape(session.device)
               << ",\"coordinator\":" << jsonBool(session.coordinator)
               << ",\"primary\":" << jsonBool(session.primary)
               << ",\"left\":" << session.bounds.left
               << ",\"top\":" << session.bounds.top
               << ",\"right\":" << session.bounds.right
               << ",\"bottom\":" << session.bounds.bottom
               << ",\"targetDpiX\":" << session.targetDpiX
               << ",\"targetDpiY\":" << session.targetDpiY
               << ",\"windowDpi\":" << session.windowDpi
               << ",\"displayRefresh\":";
        appendRefreshRate(stream, session.displayRefreshRate);
        stream << ",\"captureRefresh\":";
        appendRefreshRate(stream, session.captureRefreshRate);
        stream << ",\"adapter\":"
               << jsonEscape(wideToUtf8(
                    session.deviceInfo.adapterDescription))
               << ",\"driver\":"
               << jsonEscape(driverTypeName(session.deviceInfo.driverType))
               << ",\"requestedOutput\":"
               << jsonEscape(outputPreferenceName(
                    session.requestedOutputPreference))
               << ",\"resolvedOutput\":"
               << jsonEscape(outputPreferenceName(
                    session.resolvedOutputPolicy.preference))
               << ",\"actualOutput\":"
               << jsonEscape(actualOutput.has_value()
                    ? outputPreferenceName(*actualOutput)
                    : std::string_view{"unknown"})
               << ",\"outputPolicySatisfied\":"
               << jsonBool(session.outputPolicySatisfied)
               << ",\"colorMode\":"
               << jsonEscape(colorComplete
                    ? bafx::windows::displayColorModeName(
                        session.colorCapabilities->activeColorMode)
                    : std::string_view{"unknown"})
               << ",\"hdrSupported\":";
        appendOptionalBoolean(stream, hdrSupported);
        stream << ",\"hdrActive\":";
        appendOptionalBoolean(stream, hdrActive);
        stream << ",\"backgroundCaptureActive\":"
               << jsonBool(session.backgroundCaptureActive)
               << ",\"backgroundCaptureRestartAllowed\":"
               << jsonBool(session.backgroundCaptureRestartAllowed)
               << ",\"backgroundCaptureFailure\":"
               << jsonEscape(session.backgroundCaptureFailure)
               << ",\"renderFaulted\":"
               << jsonBool(session.renderFaulted)
               << ",\"outputContractFaulted\":"
               << jsonBool(session.outputContractFaulted)
               << '}';
    }
    stream << "]}";
    return stream.str();
}

std::filesystem::path defaultConfigPath()
{
    return bafx::windows::executableFilePath(
        L"BAFX.config.json",
        L"BAFX.config.json");
}

}
