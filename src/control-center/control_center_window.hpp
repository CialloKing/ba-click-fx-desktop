#pragma once

#include "host_state.hpp"

#include "bafx/config/config.hpp"
#include "bafx/windows/ipc_client.hpp"
#include "bafx/windows/unique_handle.hpp"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace bafx::control_center
{

class ControlCenterWindow final
{
public:
    explicit ControlCenterWindow(HINSTANCE instance) noexcept;
    ~ControlCenterWindow();

    ControlCenterWindow(const ControlCenterWindow&) = delete;
    ControlCenterWindow& operator=(const ControlCenterWindow&) = delete;

    [[nodiscard]] bool create(int showCommand);
    [[nodiscard]] int runMessageLoop() const noexcept;
    [[nodiscard]] DWORD lastError() const noexcept;

private:
    enum class ControlId : int
    {
        Pause = 100,
        EffectsEnabled,
        ClickEnabled,
        TrailEnabled,
        TrailAlwaysOn,
        GlobalScale,
        TrailLength,
        TrailWidth,
        InputSamplingRate,
        BloomIntensity,
        BloomQuality,
        BackgroundMode,
        CursorExcluded,
        AllowSystemBorder,
        Refresh,
        HostLifecycle,
        ResetDefaults
    };

    struct SliderControl final
    {
        HWND label{nullptr};
        HWND trackbar{nullptr};
        HWND valueText{nullptr};
        double minimum{0.0};
        double maximum{1.0};
        double step{0.05};
        std::string path{};
    };

    struct PendingPatch final
    {
        std::string path{};
        std::string valueJson{};
    };

    static LRESULT CALLBACK windowProcedure(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam) noexcept;

    [[nodiscard]] LRESULT handleMessage(
        UINT message,
        WPARAM wParam,
        LPARAM lParam);
    [[nodiscard]] bool registerWindowClass() noexcept;
    [[nodiscard]] bool createControls();
    [[nodiscard]] HWND createChild(
        const wchar_t* className,
        const wchar_t* text,
        DWORD style,
        ControlId id = static_cast<ControlId>(0)) const noexcept;
    [[nodiscard]] bool createSlider(
        SliderControl& slider,
        const wchar_t* label,
        double minimum,
        double maximum,
        double step,
        std::string path,
        ControlId id);

    void createFonts();
    void destroyFonts() noexcept;
    void applyFonts() const noexcept;
    void applyDpiMetrics() const noexcept;
    void adaptLayoutToMonitor(HMONITOR monitor, bool force);
    void layoutControls(int clientWidth, int clientHeight) const noexcept;
    void layoutSlider(
        const SliderControl& slider,
        int x,
        int y,
        int width,
        int height) const noexcept;
    [[nodiscard]] int scale(int logicalPixels) const noexcept;

    void onCommand(int id, int notificationCode);
    void onSliderChanged(HWND trackbar);
    void queueNumberPatch(const SliderControl& slider);
    void commitPendingPatch();
    void onTimer(UINT_PTR timerId);

    [[nodiscard]] bool refreshFromHost();
    void updateControls(
        const HostState& state,
        const bafx::config::Config& config);
    void applyPatch(std::string_view path, std::string_view valueJson);
    void sendCommand(std::string_view command);
    void resetDefaults();
    void startHostFromBundle();
    void stopHost();
    [[nodiscard]] bool hostMutexPresent() const noexcept;
    void scheduleHostRefreshRetry(bool startPending = false) noexcept;
    void scheduleHostShutdownPoll() noexcept;
    void finishHostShutdown() noexcept;
    void recoverHostShutdown(std::wstring_view message);
    void updateHostLifecycleButton() const noexcept;

    void setConnected(bool connected) noexcept;
    void setInfo(std::wstring_view title, std::wstring_view message);
    void setError(std::wstring_view message);
    void clearInfo() noexcept;

    [[nodiscard]] bool isChecked(HWND control) const noexcept;
    void setChecked(HWND control, bool checked) const noexcept;
    [[nodiscard]] double sliderValue(const SliderControl& slider) const noexcept;
    void setSliderValue(SliderControl& slider, double value) const noexcept;
    void updateSliderValueText(const SliderControl& slider) const noexcept;

    [[nodiscard]] static std::wstring utf8ToWide(std::string_view value);
    [[nodiscard]] static std::wstring describeResponse(
        const bafx::windows::IpcClientResponse& response);
    [[nodiscard]] static std::string numberJson(double value);
    [[nodiscard]] static std::string patchRequest(
        std::uint64_t generation,
        std::string_view path,
        std::string_view valueJson);
    [[nodiscard]] static std::wstring numberText(double value);
    [[nodiscard]] static std::filesystem::path executableDirectory();

    HINSTANCE instance_{nullptr};
    HWND window_{nullptr};
    HFONT normalFont_{nullptr};
    HFONT titleFont_{nullptr};
    HFONT sectionFont_{nullptr};
    UINT dpi_{96U};
    UINT layoutDpi_{96U};
    HMONITOR layoutMonitor_{nullptr};
    DWORD lastError_{ERROR_SUCCESS};

    HWND titleText_{nullptr};
    HWND statusText_{nullptr};
    HWND messageText_{nullptr};
    HWND effectsHeading_{nullptr};
    HWND effectsEnabled_{nullptr};
    HWND clickEnabled_{nullptr};
    HWND trailEnabled_{nullptr};
    HWND trailAlwaysOn_{nullptr};
    SliderControl globalScale_{};
    SliderControl trailLength_{};
    SliderControl trailWidth_{};
    SliderControl inputSamplingRate_{};
    SliderControl bloomIntensity_{};
    HWND bloomQualityLabel_{nullptr};
    HWND bloomQuality_{nullptr};
    HWND backgroundHeading_{nullptr};
    HWND backgroundModeLabel_{nullptr};
    HWND backgroundMode_{nullptr};
    HWND cursorExcluded_{nullptr};
    HWND allowSystemBorder_{nullptr};
    HWND pauseButton_{nullptr};
    HWND refreshButton_{nullptr};
    HWND hostLifecycleButton_{nullptr};
    HWND resetDefaultsButton_{nullptr};

    bafx::windows::NamedPipeIpcClient client_{};
    bafx::windows::UniqueHandle hostLifetimeMutex_{};
    std::optional<PendingPatch> pendingPatch_{};
    std::uint64_t generation_{0U};
    std::uint32_t hostRetryAttempts_{0U};
    ULONGLONG hostShutdownDeadlineTicks_{0U};
    bool connected_{false};
    // IPC can be unavailable while the Host is still initializing. Keep this
    // process-level state separate so the lifecycle button can still request
    // an orderly shutdown during that window.
    bool hostRunning_{false};
    bool paused_{false};
    bool hostStartPending_{false};
    bool hostShutdownPending_{false};
    bool hostShutdownCommandAcknowledged_{false};
    bool updatingControls_{false};
    bool interactiveMoveResize_{false};
};

}
