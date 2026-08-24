#pragma once

#include "bafx/windows/composition_renderer.hpp"
#include "bafx/windows/display_capabilities.hpp"
#include "bafx/windows/display_color_monitor.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bafx::windows
{

enum class BackgroundCaptureStatus : std::uint8_t
{
    NotProbed,
    Active,
    FallbackFxOnly,
    FallbackFxOnlyCaptureVisibilityUnknown
};

inline constexpr std::uint32_t diagnosticLogSchemaVersion = 2U;

struct DiagnosticField
{
    std::string_view key{};
    std::string_view value{};
};

enum class DiagnosticLevel : std::uint8_t
{
    Debug,
    Info,
    Warning,
    Error
};

struct DiagnosticLogRetention
{
    std::uintmax_t maximumBytes{8U * 1024U * 1024U};
    std::uint32_t backupCount{3U};
};

struct DiagnosticLogCleanupResult
{
    std::uint32_t removedFiles{0U};
    std::uintmax_t removedBytes{0U};
    std::uint32_t failedFiles{0U};
    std::error_code firstError{};
};

struct DisplayPhysicalCadenceRuntimeSummary final
{
    std::optional<DisplayRefreshRate> virtualRefreshRate{};
    std::optional<DisplayRefreshRate> physicalRefreshRate{};
    std::optional<DisplayRefreshRate> captureRefreshRate{};
    bool dynamicRefreshRateBoosted{false};
    bool available{false};
};

enum class ActiveFxRoiRuntimePath : std::uint8_t
{
    Disabled,
    Idle,
    FullScreen,
    RoiWarmup,
    RoiPrefilter,
    Unavailable
};

[[nodiscard]] constexpr std::string_view activeFxRoiRuntimePathName(
    const ActiveFxRoiRuntimePath path) noexcept
{
    switch (path)
    {
    case ActiveFxRoiRuntimePath::Disabled:
        return "disabled";
    case ActiveFxRoiRuntimePath::Idle:
        return "idle";
    case ActiveFxRoiRuntimePath::FullScreen:
        return "full-screen";
    case ActiveFxRoiRuntimePath::RoiWarmup:
        return "roi-warmup";
    case ActiveFxRoiRuntimePath::RoiPrefilter:
        return "roi-prefilter";
    case ActiveFxRoiRuntimePath::Unavailable:
        return "unavailable";
    }
    return "unavailable";
}

enum class ActiveFxRoiRuntimeReason : std::uint8_t
{
    Disabled,
    NoContent,
    BackgroundDifferentialBloom,
    Context1Unavailable,
    SharedTargetFullWrite,
    AreaTooLarge,
    BenefitTooSmall,
    Applied,
    RendererFallback,
    BloomDisabled,
    CoreMode,
    TouchesBoundary,
    Unavailable,
    Count
};

inline constexpr std::size_t activeFxRoiRuntimeReasonCount =
    static_cast<std::size_t>(ActiveFxRoiRuntimeReason::Count);

[[nodiscard]] constexpr std::string_view activeFxRoiRuntimeReasonName(
    const ActiveFxRoiRuntimeReason reason) noexcept
{
    switch (reason)
    {
    case ActiveFxRoiRuntimeReason::Disabled:
        return "disabled";
    case ActiveFxRoiRuntimeReason::NoContent:
        return "no-content";
    case ActiveFxRoiRuntimeReason::BloomDisabled:
        return "bloom-disabled";
    case ActiveFxRoiRuntimeReason::CoreMode:
        return "core-mode";
    case ActiveFxRoiRuntimeReason::BackgroundDifferentialBloom:
        return "background-differential-bloom";
    case ActiveFxRoiRuntimeReason::TouchesBoundary:
        return "touches-boundary";
    case ActiveFxRoiRuntimeReason::AreaTooLarge:
        return "area-too-large";
    case ActiveFxRoiRuntimeReason::BenefitTooSmall:
        return "benefit-too-small";
    case ActiveFxRoiRuntimeReason::Context1Unavailable:
        return "context1-unavailable";
    case ActiveFxRoiRuntimeReason::SharedTargetFullWrite:
        return "shared-target-full-write";
    case ActiveFxRoiRuntimeReason::Applied:
        return "applied";
    case ActiveFxRoiRuntimeReason::RendererFallback:
        return "renderer-fallback";
    case ActiveFxRoiRuntimeReason::Unavailable:
    case ActiveFxRoiRuntimeReason::Count:
        return "unavailable";
    }
    return "unavailable";
}

struct ActiveFxRoiGpuPercentiles final
{
    std::optional<double> p50Microseconds{};
    std::optional<double> p95Microseconds{};
};

struct ActiveFxRoiGpuRuntimeSummary final
{
    ActiveFxRoiGpuPercentiles prefilter{};
    ActiveFxRoiGpuPercentiles pyramid{};
    ActiveFxRoiGpuPercentiles finalComposite{};
};

struct ActiveFxRoiPathRuntimeSummary final
{
    bool requested{false};
    bool executed{false};
    bool eligible{false};
    bool warmup{false};
    ActiveFxRoiRuntimePath actualPath{ActiveFxRoiRuntimePath::Unavailable};
    ActiveFxRoiRuntimeReason decisionReason{
        ActiveFxRoiRuntimeReason::Unavailable};
    std::uint64_t observedFrames{0U};
    std::uint64_t requestedFrames{0U};
    std::uint64_t eligibleFrames{0U};
    std::uint64_t appliedFrames{0U};
    std::uint64_t warmupFrames{0U};
    std::uint64_t fullPixels{0U};
    std::uint64_t drawnPixels{0U};
    std::uint64_t clearedPixels{0U};
    std::uint32_t guardX{0U};
    std::uint32_t guardY{0U};
    std::uint32_t phase{0U};
    std::optional<RECT> dirtyRect{};
    std::optional<RECT> alignedRect{};
    ActiveFxRoiGpuRuntimeSummary gpu{};
    std::array<std::uint64_t, activeFxRoiRuntimeReasonCount> reasonCounts{};
};

struct ActiveFxRoiRuntimeSummary final
{
    bool enabled{false};
    std::uint32_t sampleWindowMs{5'000U};
    std::uint32_t sampleAgeMs{0U};
    std::uint64_t lastFrameId{0U};
    ActiveFxRoiPathRuntimeSummary primary{};
    ActiveFxRoiPathRuntimeSummary recordingRebuild{};
};

struct DisplaySessionRuntimeSummary final
{
    std::string monitor{};
    std::string device{};
    // Null means DisplayConfig did not provide every physical target path, so
    // callers must not persist a policy under a transient fallback identity.
    std::optional<std::string> displayKey{};
    RECT bounds{};
    std::uint32_t targetDpiX{0U};
    std::uint32_t targetDpiY{0U};
    std::uint32_t windowDpi{0U};
    std::optional<DisplayRefreshRate> displayRefreshRate{};
    std::optional<DisplayRefreshRate> captureRefreshRate{};
    DisplayCaptureCadenceFallbackReason captureCadenceFallbackReason{
        DisplayCaptureCadenceFallbackReason::NoPhysicalTargets};
    BackgroundCadenceRefreshStatus captureCadenceStatus{
        BackgroundCadenceRefreshStatus::Inactive};
    std::optional<DisplayRefreshRate> producerPolicyRefreshRate{};
    std::optional<DisplayRefreshRate> freshnessPolicyRefreshRate{};
    std::chrono::nanoseconds freshnessPolicyPeriod{};
    WgcProducerCadenceState producerCadence{};
    std::vector<DisplayPhysicalCadenceRuntimeSummary> physicalCadence{};
    LUID sourceAdapterLuid{};
    std::uint32_t sourceId{0U};
    std::size_t physicalTargetCount{0U};
    GraphicsDeviceInfo deviceInfo{};
    CompositionOutputPreference requestedOutputPreference{
        CompositionOutputPreference::ConservativeSdr};
    CompositionOutputPolicy resolvedOutputPolicy{};
    std::optional<DisplayColorCapabilities> colorCapabilities{};
    std::optional<DisplayColorCapabilities> colorObservation{};
    DisplayColorMonitorResult colorMonitorResult{};
    std::string colorSnapshotDisposition{"unavailable"};
    std::uint64_t colorQueryGeneration{0U};
    std::string backgroundCaptureFailure{};
    std::string framePacing{"match-display"};
    bool effectsEnabled{true};
    bool hdrEnabled{false};
    bool coordinator{false};
    bool primary{false};
    bool sourceAdapterResolved{false};
    bool sourceIdentityResolved{false};
    DisplayTopologyStatus sourceTopologyStatus{
        DisplayTopologyStatus::QueryFailed};
    LONG sourceTopologyError{ERROR_GEN_FAILURE};
    std::uint32_t colorRefreshRetriesRemaining{0U};
    bool outputPolicySatisfied{false};
    bool backgroundCaptureActive{false};
    bool backgroundCaptureRestartAllowed{false};
    bool renderFaulted{false};
    bool outputContractFaulted{false};
    // This is an immutable, bounded publication surface. The render thread
    // fills a detached snapshot; IPC never reaches into live renderer state.
    ActiveFxRoiRuntimeSummary activeFxRoi{};
};

struct DisplayRuntimeSummary final
{
    std::size_t sessionCount{0U};
    CompositionOutputPreference requestedOutputPreference{
        CompositionOutputPreference::ConservativeSdr};
    CompositionOutputPreference resolvedOutputPreference{
        CompositionOutputPreference::ConservativeSdr};
    std::optional<CompositionOutputPreference> actualOutputPreference{};
    bool outputPolicySatisfied{false};
    bool colorSnapshotComplete{false};
    bool hdrCapabilityObserved{false};
    bool hdrActive{false};
    DisplayTopologyStatus topologyStatus{DisplayTopologyStatus::QueryFailed};
    LONG topologyError{ERROR_GEN_FAILURE};
    std::vector<DisplaySessionRuntimeSummary> sessions{};
};

class SupportReport final
{
public:
    explicit SupportReport(std::string_view version);

    void setPrimaryMonitor(RECT bounds);
    void setPrimaryDpi(std::uint32_t dpi) noexcept;
    void setPrimaryRefreshRate(const DisplayRefreshRate& refreshRate) noexcept;
    void setPrimaryDisplayColorCapabilities(
        const DisplayColorCapabilities& capabilities) noexcept;
    void clearPrimaryDisplayColorCapabilities() noexcept;
    void setPrimaryDisplayColorMonitorResult(
        const DisplayColorMonitorResult& result) noexcept;
    void setDeviceInfo(const GraphicsDeviceInfo& info);
    void setExitUiStatus(const ExitUiStatus& status);
    void setBackgroundCaptureStatus(BackgroundCaptureStatus status) noexcept;
    void setDisplayRuntimeSummary(DisplayRuntimeSummary summary);
    void setConfigurationSchemaVersion(std::uint32_t version) noexcept;
    void setControlServiceAvailable(bool available) noexcept;
    void setLogPath(const std::filesystem::path& path);
    void setFailure(std::string_view failure);

    [[nodiscard]] std::string serialize() const;

private:
    std::string version_;
    std::string osVersion_;
    std::string architecture_;
    std::string primaryMonitor_;
    std::optional<std::uint32_t> primaryDpi_{};
    std::optional<DisplayRefreshRate> primaryRefreshRate_{};
    std::optional<DisplayColorCapabilities> primaryDisplayColorCapabilities_{};
    std::optional<DisplayColorMonitorResult>
        primaryDisplayColorMonitorResult_{};
    std::string logPath_;
    std::string failure_;
    GraphicsDeviceInfo deviceInfo_{};
    ExitUiStatus exitUiStatus_{};
    BackgroundCaptureStatus backgroundCaptureStatus_{
        BackgroundCaptureStatus::NotProbed};
    std::optional<DisplayRuntimeSummary> displayRuntimeSummary_{};
    std::optional<std::uint32_t> configurationSchemaVersion_{};
    bool controlServiceAvailable_{false};
    bool hasDeviceInfo_{false};
    bool hasExitUiStatus_{false};
};

[[nodiscard]] std::filesystem::path defaultDiagnosticLogPath();

[[nodiscard]] std::string_view diagnosticSessionId() noexcept;

// Performs one best-effort rotation using the supplied retention. Normal
// appends independently enforce the default retention for long-running hosts.
void rotateDiagnosticLog(
    const std::filesystem::path& path,
    DiagnosticLogRetention retention = {}) noexcept;

[[nodiscard]] DiagnosticLogCleanupResult clearDiagnosticLogs(
    const std::filesystem::path& path) noexcept;

void appendDiagnosticEvent(
    const std::filesystem::path& path,
    std::string_view eventName,
    std::span<const DiagnosticField> fields = {},
    DiagnosticLevel level = DiagnosticLevel::Info) noexcept;

void writeSupportReport(
    const std::filesystem::path& path,
    const SupportReport& report);

void appendDiagnosticLog(
    const std::filesystem::path& path,
    std::string_view event) noexcept;

void appendDiagnosticLog(
    const std::filesystem::path& path,
    const SupportReport& report) noexcept;

[[nodiscard]] std::string captureExclusionDiagnostic(
    const CaptureExclusionStatus& status);

[[nodiscard]] std::string captureExclusionQueryDiagnostic(
    const CaptureExclusionQueryStatus& status);

}
