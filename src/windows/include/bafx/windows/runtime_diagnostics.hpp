#pragma once

#include "bafx/windows/composition_renderer.hpp"

#include <filesystem>
#include <cstdint>
#include <string>
#include <string_view>

namespace bafx::windows
{

enum class BackgroundCaptureStatus : std::uint8_t
{
    NotProbed,
    Active,
    FallbackFxOnly
};

class SupportReport final
{
public:
    explicit SupportReport(std::string_view version);

    void setPrimaryMonitor(RECT bounds);
    void setDeviceInfo(const GraphicsDeviceInfo& info);
    void setExitUiStatus(const ExitUiStatus& status);
    void setBackgroundCaptureStatus(BackgroundCaptureStatus status) noexcept;
    void setLogPath(const std::filesystem::path& path);
    void setFailure(std::string_view failure);

    [[nodiscard]] std::string serialize() const;

private:
    std::string version_;
    std::string osVersion_;
    std::string architecture_;
    std::string primaryMonitor_;
    std::string logPath_;
    std::string failure_;
    GraphicsDeviceInfo deviceInfo_{};
    ExitUiStatus exitUiStatus_{};
    BackgroundCaptureStatus backgroundCaptureStatus_{
        BackgroundCaptureStatus::NotProbed};
    bool hasDeviceInfo_{false};
    bool hasExitUiStatus_{false};
};

[[nodiscard]] std::filesystem::path defaultDiagnosticLogPath();

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

}
