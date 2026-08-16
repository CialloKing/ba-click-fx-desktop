#pragma once

#include "bafx/windows/composition_renderer.hpp"
#include "bafx/windows/display_capabilities.hpp"
#include "bafx/windows/display_color_monitor.hpp"

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

struct DisplaySessionRuntimeSummary final
{
    std::string monitor{};
    std::string device{};
    RECT bounds{};
    std::uint32_t targetDpiX{0U};
    std::uint32_t targetDpiY{0U};
    std::uint32_t windowDpi{0U};
    std::optional<DisplayRefreshRate> displayRefreshRate{};
    std::optional<DisplayRefreshRate> captureRefreshRate{};
    LUID sourceAdapterLuid{};
    std::uint32_t sourceId{0U};
    std::size_t physicalTargetCount{0U};
    GraphicsDeviceInfo deviceInfo{};
    CompositionOutputPreference requestedOutputPreference{
        CompositionOutputPreference::ConservativeSdr};
    CompositionOutputPolicy resolvedOutputPolicy{};
    std::optional<DisplayColorCapabilities> colorCapabilities{};
    DisplayColorMonitorResult colorMonitorResult{};
    std::string backgroundCaptureFailure{};
    bool coordinator{false};
    bool primary{false};
    bool sourceAdapterResolved{false};
    bool sourceIdentityResolved{false};
    bool outputPolicySatisfied{false};
    bool backgroundCaptureActive{false};
    bool backgroundCaptureRestartAllowed{false};
    bool renderFaulted{false};
    bool outputContractFaulted{false};
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
