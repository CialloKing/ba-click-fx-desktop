#pragma once

#include "display_state.hpp"
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
        BasicPage,
        AdvancedPage,
        DisplayPage,
        AdvancedTimingSection,
        AdvancedParticlesSection,
        AdvancedRingsSection,
        AdvancedClickShardsSection,
        AdvancedBloomSection,
        EffectsEnabled,
        ClickEnabled,
        TrailEnabled,
        TrailAlwaysOn,
        GlobalScale,
        TrailLength,
        TrailWidth,
        InputSamplingRate,
        BloomIntensity,
        Opacity,
        ClickTimeScale,
        TrailTimeScale,
        TrailLifetimeMs,
        BloomDiffusion,
        BloomThreshold,
        BloomSoftKnee,
        BloomClamp,
        BloomQuality,
        DiskRadius,
        DiskLifetimeMs,
        RingsHdrIntensity,
        RingsCount,
        RingsLifetimeMs,
        RingsRadiusMin,
        RingsRadiusMax,
        RingsAngularVelocityMultiplier,
        RingsRotationDirection,
        ShardsHdrIntensity,
        ShardsClickCount,
        ShardsClickLifetimeMinMs,
        ShardsClickLifetimeMaxMs,
        ShardsClickRadius,
        ShardsClickSpeedMin,
        ShardsClickSpeedMax,
        ShardsSizeMin,
        ShardsSizeMax,
        TrailOpacity,
        BackgroundMode,
        CursorExcluded,
        AllowSystemBorder,
        DisplaySelector,
        HdrEnabled,
        FramePacing,
        DisplayIndependent,
        DisplayEffectsEnabled,
        DisplayHdrEnabled,
        DisplayFramePacing,
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

    enum class Page : std::uint8_t
    {
        Basic,
        Advanced,
        DisplayPerformance
    };

    enum class AdvancedSection : std::uint8_t
    {
        Timing,
        Particles,
        Rings,
        ClickShards,
        Bloom
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
    void selectPage(Page page) noexcept;
    void selectAdvancedSection(AdvancedSection section) noexcept;
    void updatePageVisibility() noexcept;
    void setPageControlVisible(HWND control, bool visible) const noexcept;
    void updateDisplayControls(const bafx::config::Config& config);
    void updateDisplayPolicyControls() noexcept;
    void updateDisplayDetails();
    void redrawWindowTree() const noexcept;
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
    void setSelectedDisplayOverride();
    void removeSelectedDisplayOverride();
    void applyDisplayPolicyCommand(std::string command);
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
    [[nodiscard]] const DisplaySessionState* selectedDisplaySession()
        const noexcept;
    [[nodiscard]] const bafx::config::DisplayOverrideConfig*
        selectedOfflineDisplayOverride() const noexcept;
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
    SliderControl opacity_{};
    SliderControl clickTimeScale_{};
    SliderControl trailTimeScale_{};
    SliderControl trailLifetimeMs_{};
    SliderControl bloomDiffusion_{};
    SliderControl bloomThreshold_{};
    SliderControl bloomSoftKnee_{};
    SliderControl bloomClamp_{};
    SliderControl diskRadius_{};
    SliderControl diskLifetimeMs_{};
    SliderControl ringsHdrIntensity_{};
    SliderControl ringsCount_{};
    SliderControl ringsLifetimeMs_{};
    SliderControl ringsRadiusMin_{};
    SliderControl ringsRadiusMax_{};
    SliderControl ringsAngularVelocityMultiplier_{};
    SliderControl ringsRotationDirection_{};
    SliderControl shardsHdrIntensity_{};
    SliderControl shardsClickCount_{};
    SliderControl shardsClickLifetimeMinMs_{};
    SliderControl shardsClickLifetimeMaxMs_{};
    SliderControl shardsClickRadius_{};
    SliderControl shardsClickSpeedMin_{};
    SliderControl shardsClickSpeedMax_{};
    SliderControl shardsSizeMin_{};
    SliderControl shardsSizeMax_{};
    SliderControl trailOpacity_{};
    HWND basicPageButton_{nullptr};
    HWND advancedPageButton_{nullptr};
    HWND displayPageButton_{nullptr};
    HWND advancedTimingSectionButton_{nullptr};
    HWND advancedParticlesSectionButton_{nullptr};
    HWND advancedRingsSectionButton_{nullptr};
    HWND advancedClickShardsSectionButton_{nullptr};
    HWND advancedBloomSectionButton_{nullptr};
    HWND advancedTimingHeading_{nullptr};
    HWND advancedParticlesHeading_{nullptr};
    HWND advancedRingsHeading_{nullptr};
    HWND advancedClickShardsHeading_{nullptr};
    HWND advancedBloomHeading_{nullptr};
    HWND bloomQualityLabel_{nullptr};
    HWND bloomQuality_{nullptr};
    HWND backgroundHeading_{nullptr};
    HWND backgroundModeLabel_{nullptr};
    HWND backgroundMode_{nullptr};
    HWND cursorExcluded_{nullptr};
    HWND allowSystemBorder_{nullptr};
    HWND displaySettingsHeading_{nullptr};
    HWND displaySelectorLabel_{nullptr};
    HWND displaySelector_{nullptr};
    HWND displaySummaryText_{nullptr};
    HWND hdrEnabled_{nullptr};
    HWND framePacingLabel_{nullptr};
    HWND framePacing_{nullptr};
    HWND displayIndependent_{nullptr};
    HWND displayEffectsEnabled_{nullptr};
    HWND displayHdrEnabled_{nullptr};
    HWND displayFramePacingLabel_{nullptr};
    HWND displayFramePacing_{nullptr};
    HWND displayDetailsHeading_{nullptr};
    HWND displayDetailsText_{nullptr};
    HWND pauseButton_{nullptr};
    HWND refreshButton_{nullptr};
    HWND hostLifecycleButton_{nullptr};
    HWND resetDefaultsButton_{nullptr};

    bafx::windows::NamedPipeIpcClient client_{};
    bafx::windows::UniqueHandle hostLifetimeMutex_{};
    std::optional<PendingPatch> pendingPatch_{};
    bafx::config::Config config_{};
    DisplayState displayState_{};
    std::wstring displayStateError_{};
    std::string selectedDisplayIdentity_{};
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
    Page activePage_{Page::Basic};
    AdvancedSection activeAdvancedSection_{AdvancedSection::Timing};
};

}
