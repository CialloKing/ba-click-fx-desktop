#include "host_control.hpp"

#include "bafx/windows/portable_paths.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <span>
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

struct FxProfileCommandPayload final
{
    std::optional<std::uint64_t> generation{};
    std::string name{};
    std::string error{};

    [[nodiscard]] bool succeeded() const noexcept
    {
        return generation.has_value() && error.empty();
    }
};

[[nodiscard]] FxProfileCommandPayload parseFxProfileCommandPayload(
    const std::string_view payload) noexcept
{
    FxProfileCommandPayload result{};
    const std::size_t separator = payload.find(' ');
    if (separator == std::string_view::npos
        || separator == 0U
        || separator + 1U >= payload.size())
    {
        result.error = "profile command requires generation and name";
        return result;
    }
    const std::string_view generation = payload.substr(0U, separator);
    std::uint64_t parsedGeneration = 0U;
    const auto parsed = std::from_chars(
        generation.data(),
        generation.data() + generation.size(),
        parsedGeneration);
    if (parsed.ec != std::errc{}
        || parsed.ptr != generation.data() + generation.size())
    {
        result.error = "profile command generation is invalid";
        return result;
    }
    result.generation = parsedGeneration;
    result.name = std::string(payload.substr(separator + 1U));
    return result;
}

[[nodiscard]] std::string fxProfileCatalog(
    const std::span<const FxProfileSummary> profiles)
{
    std::string result;
    for (std::size_t index = 0U; index < profiles.size(); ++index)
    {
        if (index != 0U)
        {
            result.push_back('|');
        }
        result += profiles[index].builtIn ? "B:" : "C:";
        result += profiles[index].name;
    }
    return result;
}

[[nodiscard]] bafx::windows::IpcResponse profileStoreFailure(
    const FxProfileStoreResult& result)
{
    std::string code = "invalid_fx_profile";
    if (result.status == FxProfileStoreStatus::NotFound)
    {
        code = "fx_profile_not_found";
    }
    else if (result.status == FxProfileStoreStatus::TooManyProfiles)
    {
        code = "fx_profile_limit_reached";
    }
    else if (result.status == FxProfileStoreStatus::DuplicateEffects)
    {
        code = "fx_profile_duplicate";
    }
    else if (result.status == FxProfileStoreStatus::IoError)
    {
        code = "fx_profile_store_write_failed";
    }
    return bafx::windows::IpcResponse::failure(
        std::move(code),
        result.message.empty()
            ? "effects profile operation failed"
            : result.message);
}

[[nodiscard]] std::string statusName(const bool active)
{
    return active ? "active" : "fallback-fx-only";
}

[[nodiscard]] bool startupRegistrationSettingsEqual(
    const bafx::config::SystemConfig& left,
    const bafx::config::SystemConfig& right) noexcept
{
    return left.startWithWindows == right.startWithWindows
        && left.startMinimized == right.startMinimized;
}

[[nodiscard]] std::string_view systemIntegrationPhaseName(
    const HostSystemIntegrationPhase phase) noexcept
{
    switch (phase)
    {
    case HostSystemIntegrationPhase::Apply:
        return "apply";
    case HostSystemIntegrationPhase::Compensate:
        return "compensate";
    }
    return "unknown";
}

[[nodiscard]] std::string_view startupRegistrationStatusName(
    const bafx::windows::StartupRegistrationStatus status) noexcept
{
    switch (status)
    {
    case bafx::windows::StartupRegistrationStatus::Unchanged:
        return "unchanged";
    case bafx::windows::StartupRegistrationStatus::Updated:
        return "updated";
    case bafx::windows::StartupRegistrationStatus::Removed:
        return "removed";
    case bafx::windows::StartupRegistrationStatus::Failed:
        return "failed";
    }
    return "unknown";
}

[[nodiscard]] std::string systemIntegrationResultDetails(
    const HostSystemIntegrationPhase phase,
    const bafx::windows::StartupRegistrationResult& result)
{
    std::ostringstream stream;
    stream << "phase=" << systemIntegrationPhaseName(phase)
           << "; operation="
           << bafx::windows::startupRegistrationOperationName(result.operation)
           << "; status=" << startupRegistrationStatusName(result.status)
           << "; win32-error=" << result.error;
    return stream.str();
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

[[nodiscard]] std::string_view topologyStatusName(
    const bafx::windows::DisplayTopologyStatus status) noexcept
{
    switch (status)
    {
    case bafx::windows::DisplayTopologyStatus::Complete:
        return "complete";
    case bafx::windows::DisplayTopologyStatus::Incomplete:
        return "incomplete";
    case bafx::windows::DisplayTopologyStatus::NoActiveDisplays:
        return "no-active-displays";
    case bafx::windows::DisplayTopologyStatus::QueryFailed:
        return "query-failed";
    }
    return "query-failed";
}

[[nodiscard]] std::string_view outputFallbackName(
    const bafx::windows::CompositionOutputFallback fallback) noexcept
{
    switch (fallback)
    {
    case bafx::windows::CompositionOutputFallback::None:
        return "none";
    case bafx::windows::CompositionOutputFallback::ConservativeSdr:
        return "conservative-sdr";
    }
    return "none";
}

[[nodiscard]] std::string_view outputMappingName(
    const bafx::windows::CompositionOutputMappingMode mode) noexcept
{
    switch (mode)
    {
    case bafx::windows::CompositionOutputMappingMode::ConservativeSdr:
        return "conservative-sdr";
    case bafx::windows::CompositionOutputMappingMode::AdvancedColorScRgb:
        return "advanced-color-scrgb";
    case bafx::windows::CompositionOutputMappingMode::HdrSceneReferredScRgb:
        return "hdr-scene-referred-scrgb";
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

void appendOptionalSigned(
    std::ostringstream& stream,
    const std::optional<std::int32_t> value)
{
    if (!value.has_value())
    {
        stream << "null";
        return;
    }
    stream << *value;
}

void appendOptionalUnsigned(
    std::ostringstream& stream,
    const std::optional<std::uint32_t> value)
{
    if (!value.has_value())
    {
        stream << "null";
        return;
    }
    stream << *value;
}

void appendOptionalFloat(
    std::ostringstream& stream,
    const std::optional<float> value)
{
    if (!value.has_value() || !std::isfinite(*value))
    {
        stream << "null";
        return;
    }

    std::array<char, 64U> buffer{};
    const auto converted = std::to_chars(
        buffer.data(),
        buffer.data() + buffer.size(),
        *value,
        std::chars_format::general,
        (std::numeric_limits<float>::max_digits10));
    if (converted.ec != std::errc{})
    {
        stream << "null";
        return;
    }
    stream.write(buffer.data(), converted.ptr - buffer.data());
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

void appendPhysicalCadence(
    std::ostringstream& stream,
    const std::vector<
        bafx::windows::DisplayPhysicalCadenceRuntimeSummary>& targets)
{
    stream << '[';
    for (std::size_t index = 0U; index < targets.size(); ++index)
    {
        if (index != 0U)
        {
            stream << ',';
        }
        const auto& target = targets[index];
        stream << "{\"virtualRefresh\":";
        appendRefreshRate(stream, target.virtualRefreshRate);
        stream << ",\"physicalRefresh\":";
        appendRefreshRate(stream, target.physicalRefreshRate);
        stream << ",\"captureRefresh\":";
        appendRefreshRate(stream, target.captureRefreshRate);
        stream << ",\"drrBoosted\":"
               << jsonBool(target.dynamicRefreshRateBoosted)
               << ",\"available\":" << jsonBool(target.available)
               << '}';
    }
    stream << ']';
}

[[nodiscard]] std::uint64_t nonNegativeMicroseconds(
    const bafx::core::MonotonicTime duration) noexcept
{
    const auto microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    return microseconds > 0
        ? static_cast<std::uint64_t>(microseconds)
        : 0U;
}

void appendOfflineOverrides(
    std::ostringstream& stream,
    const std::vector<bafx::config::DisplayOverrideConfig>& overrides)
{
    stream << '[';
    for (std::size_t index = 0U; index < overrides.size(); ++index)
    {
        if (index != 0U)
        {
            stream << ',';
        }
        const bafx::config::DisplayOverrideConfig& overrideConfig =
            overrides[index];
        stream << "{\"displayKey\":"
               << jsonEscape(overrideConfig.displayKey)
               << ",\"effectsEnabled\":" << jsonBool(overrideConfig.enabled)
               << ",\"hdrEnabled\":" << jsonBool(overrideConfig.hdrEnabled)
               << ",\"framePacing\":"
               << jsonEscape(bafx::config::toString(
                    overrideConfig.framePacing))
               << '}';
    }
    stream << ']';
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
          bafx::windows::NamedPipeIpcServer::Options{},
          HostSystemIntegration{},
          bafx::windows::queryRecordingCompatibleAvailability())
{
}

HostControlPlane::HostControlPlane(
    std::filesystem::path configPath,
    bafx::config::Config initialConfig,
    bafx::windows::NamedPipeIpcServer::Options ipcOptions)
    : HostControlPlane(
          std::move(configPath),
          std::move(initialConfig),
          std::move(ipcOptions),
          HostSystemIntegration{},
          bafx::windows::queryRecordingCompatibleAvailability())
{
}

HostControlPlane::HostControlPlane(
    std::filesystem::path configPath,
    bafx::config::Config initialConfig,
    HostSystemIntegration systemIntegration)
    : HostControlPlane(
          std::move(configPath),
          std::move(initialConfig),
          bafx::windows::NamedPipeIpcServer::Options{},
          systemIntegration,
          bafx::windows::queryRecordingCompatibleAvailability())
{
}

HostControlPlane::HostControlPlane(
    std::filesystem::path configPath,
    bafx::config::Config initialConfig,
    bafx::windows::NamedPipeIpcServer::Options ipcOptions,
    HostSystemIntegration systemIntegration)
    : HostControlPlane(
          std::move(configPath),
          std::move(initialConfig),
          std::move(ipcOptions),
          systemIntegration,
          bafx::windows::queryRecordingCompatibleAvailability())
{
}

HostControlPlane::HostControlPlane(
    std::filesystem::path configPath,
    bafx::config::Config initialConfig,
    bafx::windows::NamedPipeIpcServer::Options ipcOptions,
    HostSystemIntegration systemIntegration,
    bafx::windows::RecordingCompatibleAvailability
        recordingCompatibleAvailability,
    const FxProfileStoreReadProbe fxProfileReadProbe)
    : configPath_(std::move(configPath))
    , fxProfileStore_(
          configPath_.parent_path() / L"fx-profiles",
          fxProfileReadProbe)
    , config_(std::move(initialConfig))
    , recordingCompatibleAvailability_(recordingCompatibleAvailability)
    , systemIntegration_(systemIntegration)
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
    normalizeRecordingCompatibleStartup();
    backgroundCaptureActive_ = backgroundCaptureActive;
    const std::uint64_t appliedGeneration = configGeneration_;
    // Keep the mutex until the server thread exists. An immediate SetConfig
    // may advance the generation only after this applied baseline is latched.
    const bool serviceStarted = ipc_.start();
    return HostControlStartResult{appliedGeneration, serviceStarted};
}

void HostControlPlane::normalizeRecordingCompatibleStartup() noexcept
{
    if (config_.background.mode
            != bafx::config::RenderMode::RecordingCompatible
        || recordingCompatibleAvailability_.supported)
    {
        return;
    }

    const std::string reason =
        bafx::windows::recordingCompatibleAvailabilityReasonName(
            recordingCompatibleAvailability_.reason);
    bafx::config::Config fallback = config_;
    fallback.background.mode = bafx::config::RenderMode::LightBackground;
    const bafx::config::ConfigSaveResult saved =
        bafx::config::saveConfigAtomic(configPath_, fallback);
    config_ = std::move(fallback);
    appendRecordingCompatibleDiagnostic(
        "recording-compatible-test: fallback",
        "recording-compatible",
        "light-background",
        saved.succeeded() ? reason : "fallback-save-failed");
}

void HostControlPlane::appendRecordingCompatibleDiagnostic(
    const std::string_view eventName,
    const std::string_view requestedMode,
    const std::string_view effectiveMode,
    const std::string_view reason) const noexcept
{
    const std::string version =
        bafx::windows::recordingCompatibleVersionString(
            recordingCompatibleAvailability_);
    const std::string build = std::to_string(
        recordingCompatibleAvailability_.build);
    const std::string minimumBuild = std::to_string(
        bafx::windows::minimumRecordingCompatibleBuild);
    const bool recordingCompatible =
        effectiveMode == "recording-compatible";
    const bool lightBackground = effectiveMode == "light-background";
    const std::string_view appliedProfile = recordingCompatible
        ? std::string_view("RecordingCompatible")
        : (lightBackground
            ? std::string_view("LightBackground")
            : std::string_view("BackgroundAware"));
    const std::string_view effectivePath =
        recordingCompatible
        ? std::string_view("session-local-requested")
        : (lightBackground
            ? std::string_view("fx-only")
            : std::string_view("legacy-global-requested"));
    const std::string_view wgc = lightBackground
        ? std::string_view("disabled")
        : std::string_view("runtime-managed");
    const std::string_view alphaLimit = recordingCompatible
        ? std::string_view("0.90")
        : (lightBackground
            ? std::string_view("0.85")
            : std::string_view("profile-dependent"));
    const std::string generation = std::to_string(generation_);
    const std::array fields{
        bafx::windows::DiagnosticField{"OS.Version", version},
        bafx::windows::DiagnosticField{"OS.Build", build},
        bafx::windows::DiagnosticField{"RecordingCompatible.MinimumBuild", minimumBuild},
        bafx::windows::DiagnosticField{
            "RecordingCompatible.Eligibility",
            recordingCompatibleAvailability_.supported ? "available" : "unavailable"},
        bafx::windows::DiagnosticField{"RequestedMode", requestedMode},
        bafx::windows::DiagnosticField{"AppliedProfile", appliedProfile},
        bafx::windows::DiagnosticField{"EffectiveMode", effectiveMode},
        bafx::windows::DiagnosticField{"EffectivePath", effectivePath},
        bafx::windows::DiagnosticField{"WGC", wgc},
        bafx::windows::DiagnosticField{"AlphaLimit", alphaLimit},
        bafx::windows::DiagnosticField{"Generation", generation},
        bafx::windows::DiagnosticField{
            "ApplicationRevision",
            bafx::windows::recordingCompatibleApplicationRevision()},
        bafx::windows::DiagnosticField{"Reason", reason}};
    bafx::windows::appendDiagnosticEvent(
        bafx::windows::defaultDiagnosticLogPath(),
        eventName,
        fields,
        recordingCompatibleAvailability_.supported
            ? bafx::windows::DiagnosticLevel::Info
            : bafx::windows::DiagnosticLevel::Warning);
}

void HostControlPlane::stop() noexcept
{
    ipc_.stop();
}

HostRuntimeSnapshot HostControlPlane::runtimeSnapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return HostRuntimeSnapshot{
        config_,
        configGeneration_,
        paused_,
        ipc_.stopRequested()};
}

HostStateSnapshot HostControlPlane::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshotLocked();
}

HostStateSnapshot HostControlPlane::snapshotLocked() const
{
    return HostStateSnapshot{
        config_,
        generation_,
        configGeneration_,
        paused_,
        ipc_.stopRequested(),
        backgroundCaptureActive_,
        spout2RuntimeState_,
        fxProfileStore_.summaries(),
        fxProfileStore_.activeProfileName(config_.effects),
        fxProfileStore_.loadWarning()};
}

DisplayStateSnapshot HostControlPlane::displaySnapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    DisplayStateSnapshot snapshot{};
    snapshot.runtime = displayRuntimeSummary_;
    snapshot.runtimeGeneration = displayRuntimeGeneration_;
    snapshot.configGeneration = configGeneration_;
    snapshot.appliedConfigGeneration = appliedConfigGeneration_;
    snapshot.offlineOverridesAuthoritative =
        displayRuntimeSummary_.topologyStatus
        == bafx::windows::DisplayTopologyStatus::Complete;
    if (!snapshot.offlineOverridesAuthoritative)
    {
        return snapshot;
    }

    for (const bafx::config::DisplayOverrideConfig& overrideConfig :
         config_.display.overrides)
    {
        const bool connected = std::ranges::any_of(
            displayRuntimeSummary_.sessions,
            [&overrideConfig](
                const bafx::windows::DisplaySessionRuntimeSummary& session)
            {
                return session.displayKey.has_value()
                    && *session.displayKey == overrideConfig.displayKey;
            });
        if (!connected)
        {
            snapshot.offlineOverrides.push_back(overrideConfig);
        }
    }
    return snapshot;
}

void HostControlPlane::setBackgroundCaptureActive(const bool active) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    backgroundCaptureActive_ = active;
}

void HostControlPlane::setSpout2RuntimeState(
    const bool enabled,
    const std::string_view sender,
    const bafx::windows::Spout2SenderStatus status,
    const std::string_view error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    spout2RuntimeState_.enabled = enabled;
    spout2RuntimeState_.sender = sender;
    spout2RuntimeState_.status = bafx::windows::spout2SenderStatusName(status);
    spout2RuntimeState_.error = error;
    spout2RuntimeState_.outputContract = bafx::windows::spout2OutputContract;
}

void HostControlPlane::setDisplayRuntimeSummary(
    bafx::windows::DisplayRuntimeSummary summary,
    const std::uint64_t appliedConfigGeneration)
{
    std::lock_guard<std::mutex> lock(mutex_);
    displayRuntimeSummary_ = std::move(summary);
    appliedConfigGeneration_ = appliedConfigGeneration;
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

        case bafx::windows::IpcCommand::SetDisplayOverride:
            return handleDisplayOverrideMutation(request.payload, false);

        case bafx::windows::IpcCommand::RemoveDisplayOverride:
            return handleDisplayOverrideMutation(request.payload, true);

        case bafx::windows::IpcCommand::SetFxParam:
            return handleSetFxParams(request.payload, false);

        case bafx::windows::IpcCommand::SetFxParams:
            return handleSetFxParams(request.payload, true);

        case bafx::windows::IpcCommand::ResetFxConfig:
            return handleResetFxConfig();

        case bafx::windows::IpcCommand::SaveFxProfile:
        case bafx::windows::IpcCommand::ApplyFxProfile:
        case bafx::windows::IpcCommand::DeleteFxProfile:
            return handleFxProfileMutation(request.payload, request.command);

        case bafx::windows::IpcCommand::Pause:
        {
            std::lock_guard<std::mutex> lock(mutex_);
            paused_ = true;
            ++generation_;
            return bafx::windows::IpcResponse::success(
                stateJson(snapshotLocked()));
        }

        case bafx::windows::IpcCommand::Resume:
        {
            std::lock_guard<std::mutex> lock(mutex_);
            paused_ = false;
            ++generation_;
            return bafx::windows::IpcResponse::success(
                stateJson(snapshotLocked()));
        }

        case bafx::windows::IpcCommand::ClearLogs:
            return handleClearLogs();

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
            "control state generation changed; refresh before retrying");
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
    ++configGeneration_;
    if (candidate.background.mode
        == bafx::config::RenderMode::RecordingCompatible)
    {
        appendRecordingCompatibleDiagnostic(
            "recording-compatible-test: applied",
            "recording-compatible",
            "recording-compatible",
            "available");
    }
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
    ++configGeneration_;
    return bafx::windows::IpcResponse::success(
        bafx::config::getFxConfig(config_, false));
}

bafx::windows::IpcResponse HostControlPlane::handleFxProfileMutation(
    const std::string_view payload,
    const bafx::windows::IpcCommand command) noexcept
{
    const FxProfileCommandPayload request =
        parseFxProfileCommandPayload(payload);
    if (!request.succeeded())
    {
        return bafx::windows::IpcResponse::failure(
            "invalid_fx_profile",
            request.error.empty()
                ? "effects profile command is invalid"
                : request.error);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (*request.generation != generation_)
    {
        return bafx::windows::IpcResponse::failure(
            "generation_conflict",
            "control state generation changed; refresh before retrying");
    }

    if (command == bafx::windows::IpcCommand::SaveFxProfile)
    {
        const FxProfileStoreResult saved = fxProfileStore_.save(
            request.name,
            config_.effects);
        if (!saved.succeeded())
        {
            return profileStoreFailure(saved);
        }
        ++generation_;
        // Saving the catalog advances optimistic control state but leaves the
        // render configuration generation stable.
        return bafx::windows::IpcResponse::success(
            stateJson(snapshotLocked()));
    }

    if (command == bafx::windows::IpcCommand::DeleteFxProfile)
    {
        const FxProfileStoreResult removed = fxProfileStore_.remove(
            request.name);
        if (!removed.succeeded())
        {
            return profileStoreFailure(removed);
        }
        ++generation_;
        // Deleting a catalog entry leaves the currently applied effects
        // untouched, so it must not invalidate render/capture state.
        return bafx::windows::IpcResponse::success(
            stateJson(snapshotLocked()));
    }

    if (command != bafx::windows::IpcCommand::ApplyFxProfile)
    {
        return bafx::windows::IpcResponse::failure(
            "invalid_fx_profile",
            "effects profile command is not supported");
    }

    const FxProfile* profile = fxProfileStore_.find(request.name);
    if (profile == nullptr)
    {
        return bafx::windows::IpcResponse::failure(
            "fx_profile_not_found",
            "effects profile does not exist");
    }
    bafx::config::Config candidate = config_;
    candidate.effects = profile->effects;
    const bafx::config::ConfigSaveResult saved =
        bafx::config::saveConfigAtomic(configPath_, candidate);
    if (!saved.succeeded())
    {
        return bafx::windows::IpcResponse::failure(
            "config_write_failed",
            saved.message.empty()
                ? "effects profile could not be applied"
                : saved.message);
    }
    config_ = std::move(candidate);
    ++generation_;
    ++configGeneration_;
    return bafx::windows::IpcResponse::success(
        stateJson(snapshotLocked()));
}

bafx::windows::IpcResponse HostControlPlane::handleClearLogs() noexcept
{
    const std::filesystem::path logPath =
        bafx::windows::defaultDiagnosticLogPath();
    const bafx::windows::DiagnosticLogCleanupResult cleanup =
        bafx::windows::clearDiagnosticLogs(logPath);
    const std::string removedFiles = std::to_string(cleanup.removedFiles);
    const std::string removedBytes = std::to_string(cleanup.removedBytes);
    const std::string failedFiles = std::to_string(cleanup.failedFiles);
    const std::array fields{
        bafx::windows::DiagnosticField{"Log.Cleanup.RemovedFiles", removedFiles},
        bafx::windows::DiagnosticField{"Log.Cleanup.RemovedBytes", removedBytes},
        bafx::windows::DiagnosticField{"Log.Cleanup.FailedFiles", failedFiles}};
    bafx::windows::appendDiagnosticEvent(
        logPath,
        "Log.Cleanup",
        fields,
        cleanup.failedFiles == 0U
            ? bafx::windows::DiagnosticLevel::Info
            : bafx::windows::DiagnosticLevel::Warning);

    std::ostringstream response;
    response << "{\"removedFiles\":" << cleanup.removedFiles
             << ",\"removedBytes\":" << cleanup.removedBytes
             << ",\"failedFiles\":" << cleanup.failedFiles << '}';
    return bafx::windows::IpcResponse::success(response.str());
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
            "control state generation changed; refresh before retrying");
    }

    if (candidate.background.mode
            == bafx::config::RenderMode::RecordingCompatible
        && !recordingCompatibleAvailability_.supported)
    {
        const bool queryFailed =
            recordingCompatibleAvailability_.reason
            == bafx::windows::RecordingCompatibleAvailabilityReason::VersionQueryFailed;
        const std::string reason =
            bafx::windows::recordingCompatibleAvailabilityReasonName(
                recordingCompatibleAvailability_.reason);
        appendRecordingCompatibleDiagnostic(
            queryFailed
                ? "recording-compatible-test: version-query-failed"
                : "recording-compatible-test: unsupported-build",
            "recording-compatible",
            bafx::config::toString(config_.background.mode),
            reason);
        const std::string message = queryFailed
            ? "OS version could not be queried; recording-compatible requires build 28000 or newer"
            : "recording-compatible requires OS build 28000 or newer; detected build "
                + std::to_string(recordingCompatibleAvailability_.build);
        return bafx::windows::IpcResponse::failure(
            queryFailed ? "os_version_unavailable" : "unsupported_os_build",
            message);
    }

    const bool systemIntegrationChanged =
        !startupRegistrationSettingsEqual(config_.system, candidate.system);
    if (systemIntegrationChanged)
    {
        const bafx::windows::StartupRegistrationResult applied =
            systemIntegration_.applyStartupRegistration == nullptr
            ? bafx::windows::StartupRegistrationResult{
                bafx::windows::StartupRegistrationStatus::Failed,
                bafx::windows::StartupRegistrationOperation::ValidateCommand,
                ERROR_NOT_SUPPORTED,
                {}}
            : systemIntegration_.applyStartupRegistration(
                systemIntegration_.context,
                candidate.system,
                HostSystemIntegrationPhase::Apply);
        if (!applied.succeeded())
        {
            return bafx::windows::IpcResponse::failure(
                "system_integration_failed",
                systemIntegrationResultDetails(
                    HostSystemIntegrationPhase::Apply,
                    applied));
        }
    }

    const bafx::config::ConfigSaveResult saved =
        bafx::config::saveConfigAtomic(configPath_, candidate);
    if (!saved.succeeded())
    {
        if (systemIntegrationChanged)
        {
            // The registry was changed first so a successful IPC response can
            // never expose a saved configuration whose external state differs.
            // Restore the authoritative current config when the file commit
            // fails, and report compensation failure as the higher-risk error.
            const bafx::windows::StartupRegistrationResult compensated =
                systemIntegration_.applyStartupRegistration(
                    systemIntegration_.context,
                    config_.system,
                    HostSystemIntegrationPhase::Compensate);
            const std::string compensation = systemIntegrationResultDetails(
                HostSystemIntegrationPhase::Compensate,
                compensated);
            if (!compensated.succeeded())
            {
                const std::string saveError = saved.message.empty()
                    ? "configuration could not be saved"
                    : saved.message;
                return bafx::windows::IpcResponse::failure(
                    "system_integration_failed",
                    compensation + "; config-write-error=" + saveError);
            }
            return bafx::windows::IpcResponse::failure(
                "config_write_failed",
                (saved.message.empty()
                    ? std::string("configuration could not be saved")
                    : saved.message)
                    + "; " + compensation);
        }
        return bafx::windows::IpcResponse::failure(
            "config_write_failed",
            saved.message.empty() ? "configuration could not be saved" : saved.message);
    }
    config_ = candidate;
    ++generation_;
    ++configGeneration_;
    return bafx::windows::IpcResponse::success(
        bafx::config::toJson(config_, false));
}

bafx::windows::IpcResponse HostControlPlane::handleDisplayOverrideMutation(
    const std::string_view payload,
    const bool remove) noexcept
{
    bafx::config::Config baseConfig;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        baseConfig = config_;
    }

    const bafx::config::ConfigPatchResult mutation = remove
        ? bafx::config::removeDisplayOverrideJson(baseConfig, payload)
        : bafx::config::applyDisplayOverrideJson(baseConfig, payload);
    if (!mutation.succeeded())
    {
        return bafx::windows::IpcResponse::failure(
            "invalid_display_override",
            mutation.message.empty()
                ? "display override mutation is invalid"
                : mutation.message);
    }
    if (!mutation.expectedGeneration.has_value())
    {
        return bafx::windows::IpcResponse::failure(
            "invalid_display_override",
            "display override generation is required");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (*mutation.expectedGeneration != generation_)
    {
        return bafx::windows::IpcResponse::failure(
            "generation_conflict",
            "control state generation changed; refresh before retrying");
    }
    const bafx::config::ConfigSaveResult saved =
        bafx::config::saveConfigAtomic(configPath_, mutation.config);
    if (!saved.succeeded())
    {
        return bafx::windows::IpcResponse::failure(
            "config_write_failed",
            saved.message.empty()
                ? "display override could not be saved"
                : saved.message);
    }
    config_ = mutation.config;
    ++generation_;
    ++configGeneration_;
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
           << ",\"spout2Enabled\":" << jsonBool(state.spout2.enabled)
           << ",\"spout2Sender\":" << jsonEscape(state.spout2.sender)
           << ",\"spout2Status\":" << jsonEscape(state.spout2.status)
           << ",\"spout2Error\":" << jsonEscape(state.spout2.error)
           << ",\"spout2OutputContract\":"
           << jsonEscape(state.spout2.outputContract)
           << ",\"fxProfileCatalog\":"
           << jsonEscape(fxProfileCatalog(state.fxProfiles))
           << ",\"activeFxProfile\":"
           << jsonEscape(state.activeFxProfile)
           << ",\"fxProfileWarning\":"
           << jsonEscape(state.fxProfileWarning)
           << "}";
    return stream.str();
}

std::string HostControlPlane::displayStateJson(
    const DisplayStateSnapshot& state)
{
    std::ostringstream stream;
    stream << "{\"schemaVersion\":2"
           << ",\"runtimeGeneration\":" << state.runtimeGeneration
           << ",\"configGeneration\":" << state.configGeneration
           << ",\"appliedConfigGeneration\":"
           << state.appliedConfigGeneration
           << ",\"topologyStatus\":"
           << jsonEscape(topologyStatusName(state.runtime.topologyStatus))
           << ",\"topologyError\":"
           << static_cast<std::uint32_t>(state.runtime.topologyError)
           << ",\"offlineOverridesAuthoritative\":"
           << jsonBool(state.offlineOverridesAuthoritative)
           << ",\"offlineOverrides\":";
    appendOfflineOverrides(stream, state.offlineOverrides);
    stream << ",\"sessions\":[";
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
        const bool colorV2Credible = colorComplete
            && session.colorCapabilities->advancedColorInfoV2;
        const std::optional<bool> hdrSupported = colorV2Credible
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
        const std::optional<bool> hdrUserEnabled = colorV2Credible
            ? std::optional<bool>(
                session.colorCapabilities->highDynamicRangeUserEnabled)
            : std::nullopt;
        const bool whiteLevelKnown = colorComplete
            && session.colorCapabilities->sdrWhiteLevelValid;
        const std::optional<float> sdrWhiteLevelNits = whiteLevelKnown
            ? std::optional<float>(
                session.colorCapabilities->sdrWhiteLevelNits)
            : std::nullopt;
        const std::optional<bool> sdrWhiteLevelRetained = colorComplete
            ? std::optional<bool>(
                session.colorCapabilities->sdrWhiteLevelRetained)
            : std::nullopt;
        const std::optional<std::int32_t> advancedColorQueryResult =
            session.colorObservation.has_value()
                ? std::optional<std::int32_t>(static_cast<std::int32_t>(
                    session.colorObservation->advancedColorQueryResult))
                : std::nullopt;
        const std::optional<std::int32_t> sdrWhiteLevelQueryResult =
            session.colorObservation.has_value()
                ? std::optional<std::int32_t>(static_cast<std::int32_t>(
                    session.colorObservation->sdrWhiteLevelQueryResult))
                : std::nullopt;
        const std::optional<bool> advancedColorLimitedByPolicy = colorComplete
                ? std::optional<bool>(
                    session.colorCapabilities
                        ->advancedColorLimitedByPolicy)
                : std::nullopt;
        const std::optional<bool> sdrWhiteLevelConsistent = colorComplete
            ? std::optional<bool>(
                session.colorCapabilities->sdrWhiteLevelConsistent)
            : std::nullopt;
        const std::optional<bafx::windows::CompositionOutputPreference>
            actualOutput =
                bafx::windows::effectiveCompositionOutputPreference(
                    session.deviceInfo.output);

        stream << "{\"monitor\":" << jsonEscape(session.monitor)
               << ",\"device\":" << jsonEscape(session.device)
               << ",\"displayKey\":";
        if (session.displayKey.has_value())
        {
            stream << jsonEscape(*session.displayKey);
        }
        else
        {
            stream << "null";
        }
        stream << ",\"coordinator\":" << jsonBool(session.coordinator)
               << ",\"primary\":" << jsonBool(session.primary)
               << ",\"effectsEnabled\":" << jsonBool(session.effectsEnabled)
               << ",\"hdrEnabled\":" << jsonBool(session.hdrEnabled)
               << ",\"framePacing\":" << jsonEscape(session.framePacing)
               << ",\"sourceAdapterResolved\":"
               << jsonBool(session.sourceAdapterResolved)
               << ",\"sourceIdentityResolved\":"
               << jsonBool(session.sourceIdentityResolved)
               << ",\"sourceId\":";
        appendOptionalUnsigned(
            stream,
            session.sourceIdentityResolved
                ? std::optional<std::uint32_t>(session.sourceId)
                : std::nullopt);
        stream << ",\"physicalTargetCount\":"
               << session.physicalTargetCount
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
        stream << ",\"captureCadenceStatus\":"
               << jsonEscape(
                    bafx::windows::backgroundCadenceRefreshStatusName(
                        session.captureCadenceStatus))
               << ",\"cadenceFallbackReason\":"
               << jsonEscape(
                    bafx::windows::displayCaptureCadenceFallbackReasonName(
                        session.captureCadenceFallbackReason))
               << ",\"producerPolicyRefresh\":";
        appendRefreshRate(stream, session.producerPolicyRefreshRate);
        stream << ",\"freshnessPolicyRefresh\":";
        appendRefreshRate(stream, session.freshnessPolicyRefreshRate);
        stream << ",\"freshnessPeriodUs\":"
               << nonNegativeMicroseconds(session.freshnessPolicyPeriod)
               << ",\"producerCadenceStatus\":"
               << jsonEscape(bafx::windows::wgcProducerCadenceStatusName(
                    session.producerCadence.status))
               << ",\"producerRequestedPeriodUs\":"
               << nonNegativeMicroseconds(
                    session.producerCadence.requested)
               << ",\"producerAppliedPeriodUs\":"
               << nonNegativeMicroseconds(session.producerCadence.applied)
               << ",\"producerResult\":"
               << static_cast<std::int32_t>(
                    session.producerCadence.result)
               << ",\"physicalCadence\":";
        appendPhysicalCadence(stream, session.physicalCadence);
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
               << ",\"resolvedOutputMapping\":"
               << jsonEscape(outputMappingName(
                    session.resolvedOutputPolicy.mapping.mode))
               << ",\"actualOutputMapping\":"
               << jsonEscape(outputMappingName(
                    session.deviceInfo.output.mapping.mode))
               << ",\"outputFallback\":"
               << jsonEscape(outputFallbackName(
                    session.deviceInfo.output.fallback))
               << ",\"outputFallbackResult\":"
               << static_cast<std::int32_t>(
                    session.deviceInfo.output.fallbackResult)
               << ",\"colorMode\":"
               << jsonEscape(colorComplete
                    ? bafx::windows::displayColorModeName(
                        session.colorCapabilities->activeColorMode)
                    : std::string_view{"unknown"})
               << ",\"hdrSupported\":";
        appendOptionalBoolean(stream, hdrSupported);
        stream << ",\"hdrActive\":";
        appendOptionalBoolean(stream, hdrActive);
        stream << ",\"colorMonitorStatus\":"
               << jsonEscape(bafx::windows::displayColorMonitorStatusName(
                    session.colorMonitorResult.status))
               << ",\"colorMonitorHresult\":"
               << static_cast<std::int32_t>(
                    session.colorMonitorResult.error)
               << ",\"colorMonitorGeneration\":"
               << session.colorMonitorResult.generation
               << ",\"colorQueryGeneration\":"
               << session.colorQueryGeneration
               << ",\"colorSnapshotDisposition\":"
               << jsonEscape(session.colorSnapshotDisposition)
               << ",\"colorSnapshotComplete\":"
               << jsonBool(colorComplete)
               << ",\"advancedColorQueryResult\":";
        appendOptionalSigned(stream, advancedColorQueryResult);
        stream << ",\"advancedColorLimitedByPolicy\":";
        appendOptionalBoolean(stream, advancedColorLimitedByPolicy);
        stream << ",\"hdrUserEnabled\":";
        appendOptionalBoolean(stream, hdrUserEnabled);
        stream << ",\"sdrWhiteLevelQueryResult\":";
        appendOptionalSigned(stream, sdrWhiteLevelQueryResult);
        stream << ",\"sdrWhiteLevelNits\":";
        appendOptionalFloat(stream, sdrWhiteLevelNits);
        stream << ",\"sdrWhiteLevelRetained\":";
        appendOptionalBoolean(stream, sdrWhiteLevelRetained);
        stream << ",\"sdrWhiteLevelConsistent\":";
        appendOptionalBoolean(stream, sdrWhiteLevelConsistent);
        stream << ",\"colorRefreshRetriesRemaining\":"
               << session.colorRefreshRetriesRemaining;
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
