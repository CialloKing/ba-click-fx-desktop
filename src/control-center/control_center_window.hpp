#pragma once

#include "display_state.hpp"
#include "host_state.hpp"
#include "obs_spout_plugin_probe.hpp"
#include "update_check.hpp"

#include "bafx/config/config.hpp"
#include "bafx/windows/ipc_client.hpp"
#include "bafx/windows/unique_handle.hpp"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bafx::control_center
{

// This name is a process-level rendezvous contract. Unlike the caption, it
// must remain stable when product versions or localized titles change.
inline constexpr std::wstring_view controlCenterWindowClassName =
    L"BAFX.NativeControlCenter.Window.v1";

class ControlCenterWindow final
{
public:
    explicit ControlCenterWindow(HINSTANCE instance) noexcept;
    ~ControlCenterWindow();

    ControlCenterWindow(const ControlCenterWindow&) = delete;
    ControlCenterWindow& operator=(const ControlCenterWindow&) = delete;

    [[nodiscard]] bool create(int showCommand, bool startHostOnLaunch);
    [[nodiscard]] int runMessageLoop() noexcept;
    [[nodiscard]] DWORD lastError() const noexcept;

private:
    enum class ControlId : int
    {
        Pause = 100,
        BasicPage,
        AdvancedPage,
        DisplayPage,
        HotkeysPage,
        SystemPage,
        AdvancedTimingSection,
        AdvancedParticlesSection,
        AdvancedRingsSection,
        AdvancedClickShardsSection,
        AdvancedBloomSection,
        AdvancedLayersSection,
        EffectsEnabled,
        EffectsMode,
        ClickEnabled,
        TrailEnabled,
        DiskLayerEnabled,
        RingsLayerEnabled,
        ClickShardsLayerEnabled,
        TrailShardsLayerEnabled,
        TrailLayerEnabled,
        BloomLayerEnabled,
        TrailAlwaysOn,
        LeftClickEnabled,
        RightClickEnabled,
        MiddleClickEnabled,
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
        ThemeColorEdit,
        ThemeColorPreview,
        ThemeColorChoose,
        BackgroundMode,
        CursorExcluded,
        AllowSystemBorder,
        IdleOptimization,
        FxProfileSelector,
        FxProfileName,
        ApplyFxProfile,
        SaveFxProfile,
        DeleteFxProfile,
        ActiveFxRoiEnabled,
        StartWithWindows,
        StartMinimized,
        CloseToTray,
        CheckForUpdates,
        OpenRelease,
        OpenRepository,
#if defined(BAFX_ENABLE_SPOUT2)
        Spout2Enabled,
        RefreshObsSpoutPlugin,
        OpenObsSpoutPluginPage,
#endif
        DisplaySelector,
        HdrEnabled,
        FramePacing,
        DisplayIndependent,
        DisplayEffectsEnabled,
        DisplayHdrEnabled,
        DisplayFramePacing,
        Refresh,
        HostLifecycle,
        ClearLogs,
        ResetDefaults
    };
    static_assert(
        static_cast<int>(ControlId::OpenRelease)
            == static_cast<int>(ControlId::CheckForUpdates) + 1,
        "Version update actions must preserve their keyboard order");
    static_assert(
        static_cast<int>(ControlId::OpenRepository)
            == static_cast<int>(ControlId::OpenRelease) + 1,
        "Repository action must follow the version update actions");

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
        std::uint64_t generation{0U};
        std::string path{};
        std::string valueJson{};
    };

    enum class Page : std::uint8_t
    {
        Basic,
        Advanced,
        DisplayPerformance,
        Hotkeys,
        System
    };

    enum class AdvancedSection : std::uint8_t
    {
        Timing,
        Particles,
        Rings,
        ClickShards,
        Bloom,
        Layers
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
    [[nodiscard]] bool createHotkeyControls();
    void layoutHotkeyControls(int width, int height) const noexcept;
    void updateHotkeyControls();
    [[nodiscard]] bool refreshHotkeys(
        std::string_view command = "GetHotkeyState");
    void beginHotkeyCapture(std::size_t index);
    void endHotkeyCapture();
    void clearHotkeyCaptureLocally() noexcept;
    [[nodiscard]] bool captureHotkeyMessage(const MSG& message);
    [[nodiscard]] bool saveHotkeys();
    [[nodiscard]] bool confirmHotkeyDraft();
    [[nodiscard]] bool prepareToClose();
    void closeControlCenter();
    void acceptHotkeyCandidate(bafx::config::HotkeyBinding binding);
    [[nodiscard]] bool onHotkeyCommand(int id);
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
    void updateActiveFxRoiDetails();
    void updateDisplayStatePolling() noexcept;
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
    void commitThemeColor();
    void chooseThemeColor();
    void queueNumberPatch(const SliderControl& slider);
    bool commitPendingPatch();
    bool readyForFxProfileMutation();
    bool applyFxProfileMutationRequest(std::string command);
    bool applyPatchRequest(std::string command);
    void onTimer(UINT_PTR timerId);

    void beginManualUpdateCheck();
    void pollManualUpdateCheck();
    void openOfficialLatestRelease();
    void openOfficialProjectRepository();

    [[nodiscard]] bool refreshFromHost();
    [[nodiscard]] bool refreshDisplayStateFromHost();
    [[nodiscard]] static std::wstring hostVersionDescription(
        const HostState& state);
    void updateHostVersionText(const HostState& state);
    void rejectIncompatibleHostVersion(const HostState& state);
    void updateControls(
        const HostState& state,
        const bafx::config::Config& config);
    void updateFxProfileControls(const HostState& state);
    void updateFxProfileActionState() const noexcept;
    void onFxProfileSelectionChanged();
    void applySelectedFxProfile();
    void saveCurrentFxProfile();
    void deleteSelectedFxProfile();
    bool applyPatch(
        std::string_view path,
        std::string_view valueJson);
    void setSelectedDisplayOverride();
    void removeSelectedDisplayOverride();
    void applyDisplayPolicyCommand(std::string command);
    void sendCommand(std::string_view command);
    void clearDiagnosticLogs();
    void resetDefaults();
#if defined(BAFX_ENABLE_SPOUT2)
    void updateSpout2Status(const HostState& state);
    void refreshObsPluginStatus();
    void openObsPluginPage();
#endif
    void startHostFromBundle();
    void stopHost();
    [[nodiscard]] bool hostMutexPresent() const noexcept;
    void scheduleHostRefreshRetry(bool startPending = false) noexcept;
    void scheduleHostShutdownPoll() noexcept;
    void finishHostShutdown() noexcept;
    void recoverHostShutdown(std::wstring_view message);
    void updateHostLifecycleButton() const noexcept;
    [[nodiscard]] bool ensureTrayIcon() noexcept;
    void removeTrayIcon() noexcept;
    void restoreFromTray() noexcept;
    void showTrayMenu();

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
    [[nodiscard]] static std::string wideToUtf8(std::wstring_view value);
    [[nodiscard]] static std::wstring describeResponse(
        const bafx::windows::IpcClientResponse& response);
    [[nodiscard]] static std::string numberJson(double value);
    [[nodiscard]] static std::string patchRequest(
        std::uint64_t generation,
        std::string_view path,
        std::string_view valueJson);
    [[nodiscard]] static std::string fxPatchRequest(
        std::uint64_t generation,
        std::string_view path,
        std::string_view valueJson);
    [[nodiscard]] static std::string fxProfileRequest(
        std::string_view command,
        std::uint64_t generation,
        std::string_view name);
    [[nodiscard]] static std::wstring numberText(double value);
    [[nodiscard]] static std::filesystem::path executableDirectory();

    [[nodiscard]] const FxProfileState* selectedFxProfile() const noexcept;
    [[nodiscard]] const FxProfileState* findFxProfile(
        std::string_view name) const;
    [[nodiscard]] std::optional<std::string> fxProfileNameFromEdit() const;

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
    HWND effectsModeLabel_{nullptr};
    HWND effectsMode_{nullptr};
    HWND clickEnabled_{nullptr};
    HWND trailEnabled_{nullptr};
    HWND diskLayerEnabled_{nullptr};
    HWND ringsLayerEnabled_{nullptr};
    HWND clickShardsLayerEnabled_{nullptr};
    HWND trailShardsLayerEnabled_{nullptr};
    HWND trailLayerEnabled_{nullptr};
    HWND bloomLayerEnabled_{nullptr};
    HWND trailAlwaysOn_{nullptr};
    HWND leftClickEnabled_{nullptr};
    HWND rightClickEnabled_{nullptr};
    HWND middleClickEnabled_{nullptr};
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
    HWND themeColorLabel_{nullptr};
    HWND themeColorEdit_{nullptr};
    HWND themeColorPreview_{nullptr};
    HWND themeColorChoose_{nullptr};
    HWND basicPageButton_{nullptr};
    HWND advancedPageButton_{nullptr};
    HWND displayPageButton_{nullptr};
    HWND systemPageButton_{nullptr};
    HWND hotkeysPageButton_{nullptr};
    HWND hotkeyHint_{nullptr};
    HWND hotkeySave_{nullptr};
    HWND hotkeyRevert_{nullptr};
    HWND hotkeyRetry_{nullptr};
    HWND hotkeyCancel_{nullptr};
    std::array<HWND, 4U> hotkeyLabels_{};
    std::array<HWND, 4U> hotkeyValues_{};
    std::array<HWND, 4U> hotkeyRecord_{};
    std::array<HWND, 4U> hotkeyClear_{};
    std::array<HWND, 4U> hotkeyStatuses_{};
    std::vector<HWND> hotkeyControls_{};
    bafx::config::HotkeysConfig hotkeyDraft_{};
    bafx::config::HotkeysConfig hotkeyBaseline_{};
    HostState hotkeyState_{};
    std::uint64_t hotkeyDraftGeneration_{0U};
    std::uint64_t hotkeyCaptureToken_{0U};
    std::optional<std::size_t> hotkeyRecording_{};
    std::optional<bafx::config::HotkeyBinding> hotkeyCandidate_{};
    bool hotkeyDraftDirty_{false};
    bool hotkeyDraftConflicted_{false};
    bool hotkeyStateKnown_{false};
    bool hotkeyCaptureInvalid_{false};
    bool hotkeyAwaitRelease_{false};
    std::uint64_t displayedHotkeyCleanupError_{0U};
    std::string displayedHotkeyActionError_{};
    static constexpr UINT_PTR hotkeyTimerId = 6U;
    HWND advancedTimingSectionButton_{nullptr};
    HWND advancedParticlesSectionButton_{nullptr};
    HWND advancedRingsSectionButton_{nullptr};
    HWND advancedClickShardsSectionButton_{nullptr};
    HWND advancedBloomSectionButton_{nullptr};
    HWND advancedLayersSectionButton_{nullptr};
    HWND advancedTimingHeading_{nullptr};
    HWND advancedParticlesHeading_{nullptr};
    HWND advancedRingsHeading_{nullptr};
    HWND advancedClickShardsHeading_{nullptr};
    HWND advancedBloomHeading_{nullptr};
    HWND advancedLayersHeading_{nullptr};
    HWND bloomQualityLabel_{nullptr};
    HWND bloomQuality_{nullptr};
    HWND backgroundHeading_{nullptr};
    HWND backgroundModeLabel_{nullptr};
    HWND backgroundMode_{nullptr};
    HWND cursorExcluded_{nullptr};
    HWND allowSystemBorder_{nullptr};
    HWND idleOptimization_{nullptr};
    HWND fxProfileLabel_{nullptr};
    HWND fxProfileSelector_{nullptr};
    HWND fxProfileNameEdit_{nullptr};
    HWND applyFxProfileButton_{nullptr};
    HWND saveFxProfileButton_{nullptr};
    HWND deleteFxProfileButton_{nullptr};
    HWND systemSettingsHeading_{nullptr};
    HWND startWithWindows_{nullptr};
    HWND startMinimized_{nullptr};
    HWND closeToTray_{nullptr};
    HWND versionUpdateHeading_{nullptr};
    HWND controlCenterVersionText_{nullptr};
    HWND hostVersionText_{nullptr};
    HWND installStateText_{nullptr};
    HWND latestVersionText_{nullptr};
    HWND checkForUpdatesButton_{nullptr};
    HWND openReleaseButton_{nullptr};
    HWND repositoryStarHint_{nullptr};
    HWND openRepositoryButton_{nullptr};
#if defined(BAFX_ENABLE_SPOUT2)
    HWND spout2Enabled_{nullptr};
    HWND spout2SenderStatus_{nullptr};
    HWND obsSpoutPluginStatus_{nullptr};
    HWND spout2ObsHint_{nullptr};
    HWND refreshObsSpoutPluginButton_{nullptr};
    HWND openObsSpoutPluginPageButton_{nullptr};
#endif
    HWND displaySettingsHeading_{nullptr};
    HWND displaySelectorLabel_{nullptr};
    HWND displaySelector_{nullptr};
    HWND displaySummaryText_{nullptr};
    HWND hdrEnabled_{nullptr};
    HWND activeFxRoiEnabled_{nullptr};
    HWND framePacingLabel_{nullptr};
    HWND framePacing_{nullptr};
    HWND displayIndependent_{nullptr};
    HWND displayEffectsEnabled_{nullptr};
    HWND displayHdrEnabled_{nullptr};
    HWND displayFramePacingLabel_{nullptr};
    HWND displayFramePacing_{nullptr};
    HWND displayDetailsHeading_{nullptr};
    HWND displayDetailsText_{nullptr};
    HWND activeFxRoiDetailsHeading_{nullptr};
    HWND activeFxRoiDetailsText_{nullptr};
    HWND pauseButton_{nullptr};
    HWND refreshButton_{nullptr};
    HWND hostLifecycleButton_{nullptr};
    HWND clearLogsButton_{nullptr};
    HWND resetDefaultsButton_{nullptr};

    bafx::windows::NamedPipeIpcClient client_{};
    std::unique_ptr<bafx::release_update::ReleaseUpdateChecker> updateChecker_{};
    bafx::windows::UniqueHandle hostLifetimeMutex_{};
    std::optional<PendingPatch> pendingPatch_{};
    bafx::config::Config config_{};
    std::vector<FxProfileState> fxProfiles_{};
    std::optional<std::string> selectedFxProfileDraft_{};
    std::string fxProfileNameDraft_{};
    DisplayState displayState_{};
    std::wstring displayStateError_{};
    std::wstring displayStateRefreshWarning_{};
    std::string selectedDisplayIdentity_{};
    std::uint64_t generation_{0U};
    std::uint64_t lastUpdateSequence_{0U};
    std::uint32_t hostRetryAttempts_{0U};
    ULONGLONG hostShutdownDeadlineTicks_{0U};
    UINT taskbarCreatedMessage_{0U};
    bool connected_{false};
    bool fxProfileSelectionDirty_{false};
    bool fxProfileNameDirty_{false};
    bool refreshRetrying_{false};
    bool hostVersionBlocked_{false};
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
    bool trayIconAdded_{false};
    Page activePage_{Page::Basic};
    AdvancedSection activeAdvancedSection_{AdvancedSection::Timing};
};

}
