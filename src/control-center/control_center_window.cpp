#include "control_center_window.hpp"

#include "config_commands.hpp"
#include "control_center_layout.hpp"
#include "package_activation.hpp"
#include "startup_config.hpp"

#include "bafx/windows/recording_compatibility.hpp"
#include "bafx/windows/portable_paths.hpp"
#include "bafx/windows/spout2_sender.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <locale>
#include <limits>
#include <exception>
#include <sstream>
#include <string>
#include <utility>

namespace bafx::control_center
{
namespace
{

constexpr wchar_t windowClassName[] = L"BAFX.NativeControlCenter.Window.v1";
constexpr wchar_t windowTitle[] = L"BAFX Control Center";
constexpr UINT_PTR patchTimerId = 1U;
constexpr UINT_PTR hostRetryTimerId = 2U;
constexpr UINT_PTR hostShutdownTimerId = 3U;
constexpr UINT patchDelayMilliseconds = 120U;
constexpr UINT hostRetryDelayMilliseconds = 250U;
constexpr UINT hostShutdownPollDelayMilliseconds = 100U;
constexpr DWORD controlCenterIpcTimeoutMilliseconds = 100U;
constexpr ULONGLONG hostShutdownTimeoutMilliseconds = 10'000U;
constexpr UINT redrawAfterInteractiveResizeMessage = WM_APP + 1U;
constexpr UINT trayNotificationMessage = WM_APP + 2U;
constexpr UINT trayIconIdentifier = 1U;
constexpr UINT trayRestoreCommand = 1U;
constexpr UINT trayExitCommand = 2U;
constexpr std::size_t offlineDisplayItemBase = 1U << 16U;
constexpr int themeColorReturnNotification = 0x7FFF;
constexpr wchar_t themeColorEditOriginalProcedureProperty[] =
    L"BAFX.ControlCenter.ThemeColorEditOriginalProcedure";
#if defined(BAFX_ENABLE_SPOUT2)
constexpr wchar_t obsSpoutPluginPage[] =
    L"https://github.com/Off-World-Live/obs-spout2-plugin/releases";
#endif
// WGC/D3D startup can take several seconds on a cold process. The control
// center keeps probing long enough for that process to become controllable.
constexpr std::uint32_t hostRetryLimit = 40U;
// Group boxes are visual siblings rather than opaque child containers. The
// parent must paint behind them so a live resize erases vacated control pixels.
constexpr DWORD controlCenterWindowStyle =
    WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS;
static_assert((controlCenterWindowStyle & WS_CLIPCHILDREN) == 0U);

[[nodiscard]] std::wstring hresultText(const HRESULT result)
{
    std::wostringstream stream;
    stream << L"0x"
           << std::uppercase
           << std::hex
           << std::setw(8)
           << std::setfill(L'0')
           << static_cast<unsigned long>(result);
    return stream.str();
}

#if defined(BAFX_ENABLE_SPOUT2)
[[nodiscard]] std::wstring spout2StatusText(const std::string_view status)
{
    if (status == "disabled")
    {
        return L"未启用";
    }
    if (status == "waiting-for-frame")
    {
        return L"等待首帧";
    }
    if (status == "sent")
    {
        return L"正常发送";
    }
    if (status == "unavailable")
    {
        return L"当前构建不可用";
    }
    if (status == "failed")
    {
        return L"发送失败";
    }
    return L"未知状态";
}
#endif

[[nodiscard]] bafx::windows::IpcClientOptions controlCenterIpcOptions()
{
    bafx::windows::IpcClientOptions options{};
    // Control Center runs transactions on its window thread. A short local
    // timeout keeps a starting or unavailable Host from freezing the UI.
    options.timeoutMilliseconds = controlCenterIpcTimeoutMilliseconds;
    return options;
}

[[nodiscard]] HMENU controlMenu(const int id) noexcept
{
    if (id == 0)
    {
        return nullptr;
    }
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

[[nodiscard]] RECT monitorWorkArea(const HMONITOR monitor) noexcept
{
    MONITORINFO information{};
    information.cbSize = sizeof(information);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &information) != FALSE)
    {
        return information.rcWork;
    }

    RECT workArea{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0U, &workArea, 0U) != FALSE)
    {
        return workArea;
    }
    return RECT{
        0,
        0,
        GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN)};
}

[[nodiscard]] PixelSize maximumClientSize(
    const RECT workArea,
    const UINT dpi) noexcept
{
    const int workAreaWidth = static_cast<int>(
        workArea.right - workArea.left);
    const int workAreaHeight = static_cast<int>(
        workArea.bottom - workArea.top);
    RECT nonClientBounds{};
    int nonClientWidth = 0;
    int nonClientHeight = 0;
    if (AdjustWindowRectExForDpi(
            &nonClientBounds,
            controlCenterWindowStyle,
            FALSE,
            0U,
            dpi) != FALSE)
    {
        nonClientWidth = nonClientBounds.right - nonClientBounds.left;
        nonClientHeight = nonClientBounds.bottom - nonClientBounds.top;
    }
    return PixelSize{
        (std::max)(1, workAreaWidth - nonClientWidth),
        (std::max)(1, workAreaHeight - nonClientHeight)};
}

void moveControl(
    const HWND control,
    const int x,
    const int y,
    const int width,
    const int height) noexcept
{
    if (control != nullptr)
    {
        // Suppress intermediate paints while the sibling controls overlap.
        // layoutControls() redraws the complete parent and child tree after
        // every control has reached its final position.
        static_cast<void>(SetWindowPos(
            control,
            nullptr,
            x,
            y,
            width,
            height,
            SWP_NOACTIVATE
                | SWP_NOCOPYBITS
                | SWP_NOREDRAW
                | SWP_NOOWNERZORDER
                | SWP_NOZORDER));
    }
}

void setControlFont(const HWND control, const HFONT font) noexcept
{
    if (control != nullptr && font != nullptr)
    {
        static_cast<void>(SendMessageW(
            control,
            WM_SETFONT,
            reinterpret_cast<WPARAM>(font),
            TRUE));
    }
}

[[nodiscard]] COLORREF themeColorRef(const std::string_view value) noexcept
{
    if (value.size() != 7U || value.front() != '#')
    {
        return RGB(76, 167, 255);
    }
    const auto channel = [](const char high, const char low) noexcept
    {
        const auto nibble = [](const char character) noexcept
        {
            if (character >= '0' && character <= '9')
            {
                return static_cast<unsigned int>(character - '0');
            }
            if (character >= 'a' && character <= 'f')
            {
                return static_cast<unsigned int>(character - 'a' + 10);
            }
            if (character >= 'A' && character <= 'F')
            {
                return static_cast<unsigned int>(character - 'A' + 10);
            }
            return 0U;
        };
        return (nibble(high) << 4U) | nibble(low);
    };
    return RGB(
        channel(value[1], value[2]),
        channel(value[3], value[4]),
        channel(value[5], value[6]));
}

LRESULT CALLBACK themeColorEditProcedure(
    const HWND edit,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) noexcept
{
    const WNDPROC original = reinterpret_cast<WNDPROC>(GetPropW(
        edit,
        themeColorEditOriginalProcedureProperty));
    if (message == WM_KEYDOWN && wParam == VK_RETURN)
    {
        const HWND parent = GetParent(edit);
        if (parent != nullptr)
        {
            static_cast<void>(SendMessageW(
                parent,
                WM_COMMAND,
                MAKEWPARAM(
                    GetDlgCtrlID(edit),
                    themeColorReturnNotification),
                reinterpret_cast<LPARAM>(edit)));
        }
        return 0;
    }
    return original == nullptr
        ? DefWindowProcW(edit, message, wParam, lParam)
        : CallWindowProcW(original, edit, message, wParam, lParam);
}

[[nodiscard]] int qualityIndex(const bafx::config::BloomQuality quality) noexcept
{
    switch (quality)
    {
    case bafx::config::BloomQuality::Low:
        return 0;
    case bafx::config::BloomQuality::Medium:
        return 1;
    case bafx::config::BloomQuality::High:
        return 2;
    case bafx::config::BloomQuality::Ultra:
        return 3;
    case bafx::config::BloomQuality::Custom:
        return 4;
    }
    return -1;
}

[[nodiscard]] int renderModeIndex(const bafx::config::RenderMode mode) noexcept
{
    switch (mode)
    {
    case bafx::config::RenderMode::BackgroundAware:
        return 0;
    case bafx::config::RenderMode::RecordingCompatible:
        return 1;
    case bafx::config::RenderMode::LightBackground:
        return 2;
    }
    return -1;
}

[[nodiscard]] int effectsModeIndex(
    const bafx::config::EffectsMode mode) noexcept
{
    return mode == bafx::config::EffectsMode::Core ? 1 : 0;
}

[[nodiscard]] int framePacingIndex(
    const bafx::config::FramePacing pacing) noexcept
{
    switch (pacing)
    {
    case bafx::config::FramePacing::MatchDisplay:
        return 0;
    case bafx::config::FramePacing::Fixed60:
        return 1;
    case bafx::config::FramePacing::Fixed120:
        return 2;
    case bafx::config::FramePacing::Fixed144:
        return 3;
    }
    return -1;
}

[[nodiscard]] std::optional<bafx::config::FramePacing> selectedFramePacing(
    const HWND comboBox) noexcept
{
    if (comboBox == nullptr)
    {
        return std::nullopt;
    }

    switch (SendMessageW(comboBox, CB_GETCURSEL, 0U, 0))
    {
    case 0:
        return bafx::config::FramePacing::MatchDisplay;
    case 1:
        return bafx::config::FramePacing::Fixed60;
    case 2:
        return bafx::config::FramePacing::Fixed120;
    case 3:
        return bafx::config::FramePacing::Fixed144;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::wstring framePacingText(
    const bafx::config::FramePacing pacing)
{
    switch (pacing)
    {
    case bafx::config::FramePacing::MatchDisplay:
        return L"跟随显示器";
    case bafx::config::FramePacing::Fixed60:
        return L"固定 60 FPS";
    case bafx::config::FramePacing::Fixed120:
        return L"固定 120 FPS";
    case bafx::config::FramePacing::Fixed144:
        return L"固定 144 FPS";
    }
    return L"未知";
}

void initializeFramePacingCombo(const HWND comboBox) noexcept
{
    if (comboBox == nullptr)
    {
        return;
    }

    static constexpr std::array labels{
        L"跟随显示器",
        L"固定 60 FPS",
        L"固定 120 FPS",
        L"固定 144 FPS"};
    for (const wchar_t* label : labels)
    {
        static_cast<void>(SendMessageW(
            comboBox,
            CB_ADDSTRING,
            0U,
            reinterpret_cast<LPARAM>(label)));
    }
    static_cast<void>(SendMessageW(
        comboBox,
        CB_SETMINVISIBLE,
        labels.size(),
        0));
}

[[nodiscard]] std::string displaySessionIdentity(
    const DisplaySessionState& session)
{
    if (session.displayKey.has_value())
    {
        return "stable:" + *session.displayKey;
    }

    // A transient identity keeps the same row selected while the Host state
    // refreshes. It must never be used as a persisted configuration key.
    return "transient:" + session.device + '\n' + session.monitor;
}

[[nodiscard]] std::string offlineDisplayIdentity(
    const bafx::config::DisplayOverrideConfig& overrideConfig)
{
    return "stable:" + overrideConfig.displayKey;
}

[[nodiscard]] std::wstring driverStateText(
    const DisplayDriverState driver)
{
    switch (driver)
    {
    case DisplayDriverState::Hardware:
        return L"硬件";
    case DisplayDriverState::Warp:
        return L"WARP 软件渲染";
    case DisplayDriverState::Unknown:
        return L"未知";
    }
    return L"未知";
}

[[nodiscard]] std::wstring outputStateText(
    const DisplayOutputState output)
{
    switch (output)
    {
    case DisplayOutputState::ConservativeSdr:
        return L"保守 SDR";
    case DisplayOutputState::LinearScRgb:
        return L"线性 scRGB";
    case DisplayOutputState::Unknown:
        return L"未知";
    }
    return L"未知";
}

[[nodiscard]] std::wstring colorStateText(const DisplayColorState color)
{
    switch (color)
    {
    case DisplayColorState::Sdr:
        return L"SDR";
    case DisplayColorState::WideColorGamut:
        return L"广色域";
    case DisplayColorState::Hdr:
        return L"HDR";
    case DisplayColorState::Unknown:
        return L"未知";
    }
    return L"未知";
}

[[nodiscard]] std::wstring optionalBooleanText(
    const std::optional<bool> value,
    const std::wstring_view trueText,
    const std::wstring_view falseText)
{
    if (!value.has_value())
    {
        return L"未知";
    }
    return std::wstring(*value ? trueText : falseText);
}

[[nodiscard]] std::wstring refreshRateText(
    const std::optional<DisplayRefreshState>& refresh)
{
    if (!refresh.has_value()
        || refresh->numerator == 0U
        || refresh->denominator == 0U)
    {
        return L"未知";
    }

    const double hertz = static_cast<double>(refresh->numerator)
        / static_cast<double>(refresh->denominator);
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(2) << hertz << L" Hz";
    return stream.str();
}

[[nodiscard]] std::wstring topologyStateText(
    const DisplayTopologyState state)
{
    switch (state)
    {
    case DisplayTopologyState::Complete:
        return L"完整";
    case DisplayTopologyState::Incomplete:
        return L"不完整";
    case DisplayTopologyState::NoActiveDisplays:
        return L"没有活动显示器";
    case DisplayTopologyState::QueryFailed:
        return L"查询失败";
    }
    return L"未知";
}

[[nodiscard]] std::wstring colorMonitorStateText(
    const DisplayColorMonitorState state)
{
    switch (state)
    {
    case DisplayColorMonitorState::Active:
        return L"活动";
    case DisplayColorMonitorState::InvalidTarget:
        return L"目标无效";
    case DisplayColorMonitorState::Unsupported:
        return L"系统不支持";
    case DisplayColorMonitorState::Failed:
        return L"失败";
    }
    return L"未知";
}

[[nodiscard]] std::wstring colorSnapshotStateText(
    const DisplayColorSnapshotState state)
{
    switch (state)
    {
    case DisplayColorSnapshotState::Fresh:
        return L"最新完整合同";
    case DisplayColorSnapshotState::RetainedTransaction:
        return L"事务内保留";
    case DisplayColorSnapshotState::RetainedLastKnown:
        return L"保留最后完整合同";
    case DisplayColorSnapshotState::Unavailable:
        return L"不可用";
    }
    return L"未知";
}

[[nodiscard]] std::wstring cadenceFallbackText(
    const DisplayCadenceFallbackState state)
{
    switch (state)
    {
    case DisplayCadenceFallbackState::None:
        return L"无";
    case DisplayCadenceFallbackState::NoPhysicalTargets:
        return L"没有物理目标";
    case DisplayCadenceFallbackState::PhysicalTargetUnavailable:
        return L"物理目标不可用";
    case DisplayCadenceFallbackState::DrrPhysicalRefreshRateUnavailable:
        return L"DRR 物理刷新率不可用";
    case DisplayCadenceFallbackState::InvalidEffectiveRefreshRate:
        return L"有效刷新率无效";
    case DisplayCadenceFallbackState::MixedCloneRefreshRates:
        return L"克隆目标刷新率冲突，采用 60 Hz";
    }
    return L"未知";
}

[[nodiscard]] std::wstring captureCadenceText(
    const DisplayCaptureCadenceState state)
{
    switch (state)
    {
    case DisplayCaptureCadenceState::Inactive:
        return L"未活动";
    case DisplayCaptureCadenceState::WrongMonitor:
        return L"捕获目标不匹配";
    case DisplayCaptureCadenceState::TargetRate:
        return L"采用目标刷新率";
    case DisplayCaptureCadenceState::ConservativeFallback:
        return L"保守回退";
    }
    return L"未知";
}

[[nodiscard]] std::wstring producerCadenceText(
    const DisplayProducerCadenceState state)
{
    switch (state)
    {
    case DisplayProducerCadenceState::NotRequested:
        return L"未请求";
    case DisplayProducerCadenceState::Applied:
        return L"已应用";
    case DisplayProducerCadenceState::InterfaceUnavailable:
        return L"接口不可用";
    case DisplayProducerCadenceState::Rejected:
        return L"系统拒绝";
    }
    return L"未知";
}

[[nodiscard]] std::wstring outputMappingText(
    const DisplayOutputMappingState state)
{
    switch (state)
    {
    case DisplayOutputMappingState::ConservativeSdr:
        return L"保守 SDR";
    case DisplayOutputMappingState::AdvancedColorScRgb:
        return L"Advanced Color scRGB";
    case DisplayOutputMappingState::HdrSceneReferredScRgb:
        return L"HDR scene-referred scRGB";
    case DisplayOutputMappingState::Unknown:
        return L"未知";
    }
    return L"未知";
}

[[nodiscard]] std::wstring outputFallbackText(
    const DisplayOutputFallbackState state)
{
    switch (state)
    {
    case DisplayOutputFallbackState::None:
        return L"无";
    case DisplayOutputFallbackState::ConservativeSdr:
        return L"回退到保守 SDR";
    }
    return L"未知";
}

[[nodiscard]] std::wstring optionalHresultText(
    const std::optional<std::int32_t> result)
{
    if (!result.has_value())
    {
        return L"未查询";
    }
    return hresultText(static_cast<HRESULT>(*result));
}

[[nodiscard]] std::wstring optionalNitsText(
    const std::optional<float> nits)
{
    if (!nits.has_value())
    {
        return L"未知";
    }

    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(2) << *nits << L" nits";
    return stream.str();
}

}

ControlCenterWindow::ControlCenterWindow(const HINSTANCE instance) noexcept
    : instance_(instance)
    , client_(controlCenterIpcOptions())
{
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
}

ControlCenterWindow::~ControlCenterWindow()
{
    if (window_ != nullptr)
    {
        KillTimer(window_, patchTimerId);
        KillTimer(window_, hostRetryTimerId);
        KillTimer(window_, hostShutdownTimerId);
        DestroyWindow(window_);
        window_ = nullptr;
    }
    hostLifetimeMutex_.reset();
    destroyFonts();
}

bool ControlCenterWindow::create(
    const int showCommand,
    const bool startHostOnLaunch)
{
    if (!registerWindowClass())
    {
        return false;
    }

    // GetDpiForSystem() can return the virtualized 96-DPI value before this
    // process owns a top-level window. The desktop window reports the primary
    // monitor DPI that is needed for the initial centered window.
    dpi_ = GetDpiForWindow(GetDesktopWindow());
    if (dpi_ == 0U)
    {
        dpi_ = USER_DEFAULT_SCREEN_DPI;
    }
    const HMONITOR primaryMonitor = MonitorFromPoint(
        POINT{0, 0},
        MONITOR_DEFAULTTOPRIMARY);
    layoutMonitor_ = primaryMonitor;
    const RECT workArea = monitorWorkArea(primaryMonitor);
    layoutDpi_ = controlCenterLayoutDpi(
        maximumClientSize(workArea, dpi_),
        dpi_);
    RECT bounds{
        0,
        0,
        scale(defaultControlCenterClientWidth),
        scale(defaultControlCenterClientHeight)};
    if (AdjustWindowRectExForDpi(
            &bounds,
            controlCenterWindowStyle,
            FALSE,
            0U,
            dpi_) == FALSE)
    {
        lastError_ = GetLastError();
        return false;
    }

    const int workAreaWidth = static_cast<int>(workArea.right - workArea.left);
    const int workAreaHeight = static_cast<int>(workArea.bottom - workArea.top);
    const PixelSize windowSize = clampPixelSize(
        PixelSize{bounds.right - bounds.left, bounds.bottom - bounds.top},
        PixelSize{workAreaWidth, workAreaHeight});
    const int windowWidth = windowSize.width;
    const int windowHeight = windowSize.height;
    const int windowX = static_cast<int>(workArea.left) + (std::max)(
        0,
        (workAreaWidth - windowWidth) / 2);
    const int windowY = static_cast<int>(workArea.top) + (std::max)(
        0,
        (workAreaHeight - windowHeight) / 2);

    // CW_USEDEFAULT makes Windows choose the size of an overlapped window and
    // discards our DPI-scaled dimensions. Explicit coordinates preserve the
    // client area that layoutControls() was designed for.
    window_ = CreateWindowExW(
        0U,
        windowClassName,
        windowTitle,
        controlCenterWindowStyle,
        windowX,
        windowY,
        windowWidth,
        windowHeight,
        nullptr,
        nullptr,
        instance_,
        this);
    if (window_ == nullptr)
    {
        lastError_ = GetLastError();
        return false;
    }

    dpi_ = GetDpiForWindow(window_);
    if (dpi_ == 0U)
    {
        dpi_ = USER_DEFAULT_SCREEN_DPI;
    }
    layoutMonitor_ = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
    layoutDpi_ = controlCenterLayoutDpi(
        maximumClientSize(
            monitorWorkArea(layoutMonitor_),
            dpi_),
        dpi_);
    createFonts();
    if (!createControls())
    {
        lastError_ = GetLastError();
        DestroyWindow(window_);
        window_ = nullptr;
        return false;
    }

    RECT client{};
    if (GetClientRect(window_, &client) != FALSE)
    {
        layoutControls(client.right, client.bottom);
    }
    updateControls(HostState{}, loadStartupConfig(executableDirectory()));
    hostRunning_ = hostMutexPresent();
    setConnected(false);
    SetWindowTextW(statusText_, L"正在连接 Host...");
    updateHostLifecycleButton();
    ShowWindow(window_, showCommand == 0 ? SW_SHOWNORMAL : showCommand);
    UpdateWindow(window_);

    if (!refreshFromHost())
    {
        if (startHostOnLaunch)
        {
            // The Run entry launches the Control Center so packaged and
            // portable activation continue to share one Host startup path.
            startHostFromBundle();
        }
        else
        {
            scheduleHostRefreshRetry();
        }
    }
    else
    {
        updateHostLifecycleButton();
    }
    return true;
}

int ControlCenterWindow::runMessageLoop() const noexcept
{
    MSG message{};
    while (true)
    {
        const BOOL result = GetMessageW(&message, nullptr, 0U, 0U);
        if (result == 0)
        {
            return static_cast<int>(message.wParam);
        }
        if (result < 0)
        {
            return 1;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

DWORD ControlCenterWindow::lastError() const noexcept
{
    return lastError_;
}

LRESULT CALLBACK ControlCenterWindow::windowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) noexcept
{
    ControlCenterWindow* self = reinterpret_cast<ControlCenterWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<ControlCenterWindow*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
    }

    if (self == nullptr)
    {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    try
    {
        const LRESULT result = self->handleMessage(message, wParam, lParam);
        if (message == WM_NCDESTROY)
        {
            // The HWND remains valid through default non-client teardown.
            // Clear the object backlink only after that final message returns.
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            self->window_ = nullptr;
        }
        return result;
    }
    catch (...)
    {
        self->setError(L"控制中心处理窗口消息时发生内部错误。");
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

LRESULT ControlCenterWindow::handleMessage(
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam)
{
    if (taskbarCreatedMessage_ != 0U && message == taskbarCreatedMessage_)
    {
        // Explorer owns the notification area and drops all icons when it
        // restarts. Re-add ours only when the current configuration owns it.
        trayIconAdded_ = false;
        if (config_.system.closeToTray)
        {
            static_cast<void>(ensureTrayIcon());
        }
        return 0;
    }

    switch (message)
    {
    case WM_COMMAND:
        onCommand(LOWORD(wParam), HIWORD(wParam));
        return 0;
    case WM_DRAWITEM:
    {
        const auto* drawItem = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (drawItem == nullptr
            || drawItem->CtlID != static_cast<UINT>(ControlId::ThemeColorPreview))
        {
            return 0;
        }
        const COLORREF color = themeColorRef(config_.effects.themeColor);
        HBRUSH brush = CreateSolidBrush(color);
        if (brush != nullptr)
        {
            FillRect(drawItem->hDC, &drawItem->rcItem, brush);
            DeleteObject(brush);
        }
        FrameRect(
            drawItem->hDC,
            &drawItem->rcItem,
            static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        if ((drawItem->itemState & ODS_FOCUS) != 0U)
        {
            DrawFocusRect(drawItem->hDC, &drawItem->rcItem);
        }
        return TRUE;
    }
    case WM_HSCROLL:
        onSliderChanged(reinterpret_cast<HWND>(lParam));
        return 0;
    case WM_TIMER:
        onTimer(static_cast<UINT_PTR>(wParam));
        return 0;
    case trayNotificationMessage:
        if (wParam != trayIconIdentifier)
        {
            return 0;
        }
        if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK)
        {
            restoreFromTray();
        }
        else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
        {
            showTrayMenu();
        }
        return 0;
    case WM_WINDOWPOSCHANGING:
    {
        auto* position = reinterpret_cast<WINDOWPOS*>(lParam);
        if (position != nullptr && (position->flags & SWP_NOSIZE) == 0U)
        {
            // The top-level sizing transaction otherwise copies its old
            // client bitmap after child controls have moved. Discarding those
            // pixels lets the complete WM_SIZE redraw become authoritative.
            position->flags |= SWP_NOCOPYBITS;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }
    case WM_SIZE:
    {
        RECT client{};
        if (GetClientRect(window_, &client) != FALSE)
        {
            // GetClientRect avoids the 16-bit LPARAM truncation used by
            // LOWORD/HIWORD when a high-DPI window spans a large monitor.
            layoutControls(client.right - client.left, client.bottom - client.top);
        }
        return 0;
    }
    case WM_ENTERSIZEMOVE:
        interactiveMoveResize_ = true;
        return 0;
    case WM_EXITSIZEMOVE:
        interactiveMoveResize_ = false;
        adaptLayoutToMonitor(
            MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
            false);
        // Windows can retain the last backing surface until the modal sizing
        // loop returns. Repaint once more from the normal message loop so the
        // final child layout and parent background are committed together.
        if (PostMessageW(
                window_,
                redrawAfterInteractiveResizeMessage,
                0U,
                0) == FALSE)
        {
            redrawWindowTree();
        }
        return 0;
    case redrawAfterInteractiveResizeMessage:
        redrawWindowTree();
        return 0;
    case WM_MOVE:
        if (!interactiveMoveResize_)
        {
            adaptLayoutToMonitor(
                MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
                false);
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_DISPLAYCHANGE:
        adaptLayoutToMonitor(
            MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
            true);
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_SETTINGCHANGE:
        if (wParam == SPI_SETWORKAREA)
        {
            adaptLayoutToMonitor(
                MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
                true);
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_DPICHANGED:
    {
        dpi_ = HIWORD(wParam);
        if (dpi_ == 0U)
        {
            dpi_ = USER_DEFAULT_SCREEN_DPI;
        }
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested != nullptr)
        {
            const HMONITOR targetMonitor = MonitorFromRect(
                suggested,
                MONITOR_DEFAULTTONEAREST);
            layoutMonitor_ = targetMonitor;
            const RECT workArea = monitorWorkArea(targetMonitor);
            layoutDpi_ = controlCenterLayoutDpi(
                maximumClientSize(workArea, dpi_),
                dpi_);
            createFonts();

            RECT minimumBounds{
                0,
                0,
                scale(minimumControlCenterClientWidth),
                scale(minimumControlCenterClientHeight)};
            static_cast<void>(AdjustWindowRectExForDpi(
                &minimumBounds,
                static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE)),
                FALSE,
                static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_EXSTYLE)),
                dpi_));
            const PixelSize targetSize = clampPixelSize(
                PixelSize{
                    (std::max)(
                        suggested->right - suggested->left,
                        minimumBounds.right - minimumBounds.left),
                    (std::max)(
                        suggested->bottom - suggested->top,
                        minimumBounds.bottom - minimumBounds.top)},
                PixelSize{
                    static_cast<int>(workArea.right - workArea.left),
                    static_cast<int>(workArea.bottom - workArea.top)});
            const int targetX = (std::clamp)(
                static_cast<int>(suggested->left),
                static_cast<int>(workArea.left),
                static_cast<int>(workArea.right) - targetSize.width);
            const int targetY = (std::clamp)(
                static_cast<int>(suggested->top),
                static_cast<int>(workArea.top),
                static_cast<int>(workArea.bottom) - targetSize.height);
            SetWindowPos(
                window_,
                nullptr,
                targetX,
                targetY,
                targetSize.width,
                targetSize.height,
                SWP_NOACTIVATE | SWP_NOZORDER);
        }
        else
        {
            layoutMonitor_ = MonitorFromWindow(
                window_,
                MONITOR_DEFAULTTONEAREST);
            layoutDpi_ = controlCenterLayoutDpi(
                maximumClientSize(monitorWorkArea(layoutMonitor_), dpi_),
                dpi_);
            createFonts();
        }
        RECT client{};
        if (GetClientRect(window_, &client) != FALSE)
        {
            layoutControls(client.right - client.left, client.bottom - client.top);
        }
        return 0;
    }
    case WM_GETMINMAXINFO:
    {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        if (limits == nullptr)
        {
            return 0;
        }
        RECT minimumBounds{
            0,
            0,
            scale(minimumControlCenterClientWidth),
            scale(minimumControlCenterClientHeight)};
        if (AdjustWindowRectExForDpi(
                &minimumBounds,
                static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE)),
                FALSE,
                static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_EXSTYLE)),
                dpi_) != FALSE)
        {
            limits->ptMinTrackSize.x = minimumBounds.right - minimumBounds.left;
            limits->ptMinTrackSize.y = minimumBounds.bottom - minimumBounds.top;
        }
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0U) == SC_CLOSE
            && config_.system.closeToTray)
        {
            commitPendingPatch();
            if (ensureTrayIcon())
            {
                ShowWindow(window_, SW_HIDE);
                return 0;
            }
            // Never leave the process running without a visible recovery
            // entry when Explorer rejects notification-area registration.
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_CLOSE:
        commitPendingPatch();
        DestroyWindow(window_);
        return 0;
    case WM_DESTROY:
        if (themeColorEdit_ != nullptr)
        {
            RemovePropW(
                themeColorEdit_,
                themeColorEditOriginalProcedureProperty);
        }
        KillTimer(window_, patchTimerId);
        KillTimer(window_, hostRetryTimerId);
        KillTimer(window_, hostShutdownTimerId);
        removeTrayIcon();
        hostLifetimeMutex_.reset();
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        return DefWindowProcW(window_, message, wParam, lParam);
    default:
        return DefWindowProcW(window_, message, wParam, lParam);
    }
}

bool ControlCenterWindow::registerWindowClass() noexcept
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance_;
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = windowClassName;
    windowClass.hIconSm = windowClass.hIcon;

    if (RegisterClassExW(&windowClass) != 0U)
    {
        return true;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_CLASS_ALREADY_EXISTS)
    {
        return true;
    }
    lastError_ = error;
    return false;
}

bool ControlCenterWindow::createControls()
{
    titleText_ = createChild(
        L"STATIC",
        L"BAFX Desktop",
        SS_LEFT | SS_NOPREFIX);
    statusText_ = createChild(
        L"STATIC",
        L"正在连接 Host...",
        SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS);
    messageText_ = createChild(
        L"STATIC",
        L"",
        SS_LEFT | SS_NOPREFIX);
    basicPageButton_ = createChild(
        L"BUTTON",
        L"基础设置",
        BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
        ControlId::BasicPage);
    advancedPageButton_ = createChild(
        L"BUTTON",
        L"高级参数",
        BS_AUTORADIOBUTTON | WS_TABSTOP,
        ControlId::AdvancedPage);
    displayPageButton_ = createChild(
        L"BUTTON",
        L"显示与性能",
        BS_AUTORADIOBUTTON | WS_TABSTOP,
        ControlId::DisplayPage);
    systemPageButton_ = createChild(
        L"BUTTON",
        L"系统",
        BS_AUTORADIOBUTTON | WS_TABSTOP,
        ControlId::SystemPage);
    advancedTimingSectionButton_ = createChild(
        L"BUTTON",
        L"时间与透明度",
        BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
        ControlId::AdvancedTimingSection);
    advancedParticlesSectionButton_ = createChild(
        L"BUTTON",
        L"粒子与材质",
        BS_AUTORADIOBUTTON | WS_TABSTOP,
        ControlId::AdvancedParticlesSection);
    advancedRingsSectionButton_ = createChild(
        L"BUTTON",
        L"圆环参数",
        BS_AUTORADIOBUTTON | WS_TABSTOP,
        ControlId::AdvancedRingsSection);
    advancedClickShardsSectionButton_ = createChild(
        L"BUTTON",
        L"点击碎片",
        BS_AUTORADIOBUTTON | WS_TABSTOP,
        ControlId::AdvancedClickShardsSection);
    advancedBloomSectionButton_ = createChild(
        L"BUTTON",
        L"Bloom 参数",
        BS_AUTORADIOBUTTON | WS_TABSTOP,
        ControlId::AdvancedBloomSection);
    effectsHeading_ = createChild(
        L"BUTTON",
        L"特效",
        BS_GROUPBOX);

    effectsModeLabel_ = createChild(
        L"STATIC",
        L"性能模式",
        SS_LEFT | SS_NOPREFIX);
    effectsMode_ = createChild(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
        ControlId::EffectsMode);
    if (effectsMode_ != nullptr)
    {
        static_cast<void>(SendMessageW(
            effectsMode_,
            CB_ADDSTRING,
            0U,
            reinterpret_cast<LPARAM>(L"完整特效")));
        static_cast<void>(SendMessageW(
            effectsMode_,
            CB_ADDSTRING,
            0U,
            reinterpret_cast<LPARAM>(L"核心性能模式（关闭 Bloom）")));
        static_cast<void>(SendMessageW(effectsMode_, CB_SETMINVISIBLE, 2U, 0));
    }

    effectsEnabled_ = createChild(
        L"BUTTON",
        L"启用特效",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::EffectsEnabled);
    clickEnabled_ = createChild(
        L"BUTTON",
        L"点击特效",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::ClickEnabled);
    trailEnabled_ = createChild(
        L"BUTTON",
        L"鼠标拖尾",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::TrailEnabled);
    trailAlwaysOn_ = createChild(
        L"BUTTON",
        L"拖尾常驻",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::TrailAlwaysOn);
    leftClickEnabled_ = createChild(
        L"BUTTON",
        L"左键触发",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::LeftClickEnabled);
    rightClickEnabled_ = createChild(
        L"BUTTON",
        L"右键触发",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::RightClickEnabled);
    middleClickEnabled_ = createChild(
        L"BUTTON",
        L"中键触发",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::MiddleClickEnabled);

    const bool slidersCreated = createSlider(
        globalScale_,
        L"效果大小",
        0.1,
        4.0,
        0.05,
        "effects.globalScale",
        ControlId::GlobalScale)
        && createSlider(
            trailLength_,
            L"拖尾长度",
            0.0,
            10000.0 / 300.0,
            0.05,
            "effects.trailLength",
            ControlId::TrailLength)
        && createSlider(
            trailWidth_,
            L"拖尾宽度",
            0.1,
            4.0,
            0.05,
            "effects.trailWidth",
            ControlId::TrailWidth)
        && createSlider(
            inputSamplingRate_,
            L"输入采样率上限 (Hz)",
            0.0,
            1000.0,
            1.0,
            "input.samplingRateHz",
            ControlId::InputSamplingRate)
        && createSlider(
            bloomIntensity_,
            L"Bloom 强度",
            0.0,
            10.0,
            0.05,
            "effects.bloomIntensity",
            ControlId::BloomIntensity);

    const bool advancedSlidersCreated = createSlider(
        opacity_,
        L"透明度",
        0.0,
        1.0,
        0.01,
        "effects.opacity",
        ControlId::Opacity)
        && createSlider(
            clickTimeScale_,
            L"点击动画速度",
            0.01,
            4.0,
            0.01,
            "effects.clickTimeScale",
            ControlId::ClickTimeScale)
        && createSlider(
            trailTimeScale_,
            L"拖尾动画速度",
            0.01,
            4.0,
            0.01,
            "effects.trailTimeScale",
            ControlId::TrailTimeScale)
        && createSlider(
            trailLifetimeMs_,
            L"拖尾寿命 (ms)",
            0.0,
            10000.0,
            1.0,
            "effects.trailLifetimeMs",
            ControlId::TrailLifetimeMs)
        && createSlider(
            bloomDiffusion_,
            L"Bloom 扩散",
            0.0,
            10.0,
            0.01,
            "effects.bloomDiffusion",
            ControlId::BloomDiffusion)
        && createSlider(
            bloomThreshold_,
            L"Bloom 阈值",
            0.0,
            64.0,
            0.01,
            "effects.bloomThreshold",
            ControlId::BloomThreshold)
        && createSlider(
            bloomSoftKnee_,
            L"Bloom 软阈值",
            0.0,
            1.0,
            0.01,
            "effects.bloomSoftKnee",
            ControlId::BloomSoftKnee)
        && createSlider(
            bloomClamp_,
            L"Bloom 亮度上限",
            0.0,
            65504.0,
            1.0,
            "effects.bloomClamp",
            ControlId::BloomClamp);

    const bool particleSlidersCreated = createSlider(
        diskRadius_,
        L"光盘半径",
        20.0,
        120.0,
        0.01,
        "effects.diskRadius",
        ControlId::DiskRadius)
        && createSlider(
            diskLifetimeMs_,
            L"光盘寿命 (ms)",
            50.0,
            500.0,
            1.0,
            "effects.diskLifetimeMs",
            ControlId::DiskLifetimeMs)
        && createSlider(
            ringsHdrIntensity_,
            L"圆环 HDR 强度",
            0.0,
            8.0,
            0.01,
            "effects.ringsHdrIntensity",
            ControlId::RingsHdrIntensity)
        && createSlider(
            shardsHdrIntensity_,
            L"碎片 HDR 强度",
            0.0,
            8.0,
            0.01,
            "effects.shardsHdrIntensity",
            ControlId::ShardsHdrIntensity)
        && createSlider(
            trailOpacity_,
            L"拖尾透明度",
            0.0,
            1.0,
            0.01,
            "effects.trailOpacity",
            ControlId::TrailOpacity);

    const bool ringSlidersCreated = createSlider(
        ringsCount_,
        L"圆环数量",
        0.0,
        6.0,
        1.0,
        "effects.ringsCount",
        ControlId::RingsCount)
        && createSlider(
            ringsLifetimeMs_,
            L"圆环寿命 (ms)",
            50.0,
            2000.0,
            1.0,
            "effects.ringsLifetimeMs",
            ControlId::RingsLifetimeMs)
        && createSlider(
            ringsRadiusMin_,
            L"圆环最小半径",
            20.0,
            120.0,
            0.01,
            "effects.ringsRadiusMin",
            ControlId::RingsRadiusMin)
        && createSlider(
            ringsRadiusMax_,
            L"圆环最大半径",
            20.0,
            120.0,
            0.01,
            "effects.ringsRadiusMax",
            ControlId::RingsRadiusMax)
        && createSlider(
            ringsAngularVelocityMultiplier_,
            L"圆环角速度倍率",
            1.0,
            30.0,
            0.01,
            "effects.ringsAngularVelocityMultiplier",
            ControlId::RingsAngularVelocityMultiplier)
        && createSlider(
            ringsRotationDirection_,
            L"圆环旋转方向",
            -1.0,
            1.0,
            2.0,
            "effects.ringsRotationDirection",
            ControlId::RingsRotationDirection);

    const bool clickShardSlidersCreated = createSlider(
        shardsClickCount_,
        L"点击碎片数量",
        0.0,
        12.0,
        1.0,
        "effects.shardsClickCount",
        ControlId::ShardsClickCount)
        && createSlider(
            shardsClickLifetimeMinMs_,
            L"寿命下限 (ms)",
            100.0,
            1000.0,
            1.0,
            "effects.shardsClickLifetimeMinMs",
            ControlId::ShardsClickLifetimeMinMs)
        && createSlider(
            shardsClickLifetimeMaxMs_,
            L"寿命上限 (ms)",
            100.0,
            1000.0,
            1.0,
            "effects.shardsClickLifetimeMaxMs",
            ControlId::ShardsClickLifetimeMaxMs)
        && createSlider(
            shardsClickRadius_,
            L"出生半径",
            0.0,
            200.0,
            0.01,
            "effects.shardsClickRadius",
            ControlId::ShardsClickRadius)
        && createSlider(
            shardsClickSpeedMin_,
            L"速度下限",
            0.0,
            200.0,
            0.01,
            "effects.shardsClickSpeedMin",
            ControlId::ShardsClickSpeedMin)
        && createSlider(
            shardsClickSpeedMax_,
            L"速度上限",
            0.0,
            200.0,
            0.01,
            "effects.shardsClickSpeedMax",
            ControlId::ShardsClickSpeedMax)
        && createSlider(
            shardsSizeMin_,
            L"共享碎片尺寸下限",
            0.0,
            100.0,
            0.01,
            "effects.shardsSizeMin",
            ControlId::ShardsSizeMin)
        && createSlider(
            shardsSizeMax_,
            L"共享碎片尺寸上限",
            0.0,
            100.0,
            0.01,
            "effects.shardsSizeMax",
            ControlId::ShardsSizeMax);

    themeColorLabel_ = createChild(
        L"STATIC",
        L"主题色",
        SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX);
    themeColorEdit_ = createChild(
        L"EDIT",
        L"#4ca7ff",
        ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
        ControlId::ThemeColorEdit);
    themeColorPreview_ = createChild(
        L"STATIC",
        L"",
        SS_OWNERDRAW | WS_TABSTOP,
        ControlId::ThemeColorPreview);
    themeColorChoose_ = createChild(
        L"BUTTON",
        L"取色...",
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::ThemeColorChoose);
    if (themeColorEdit_ != nullptr)
    {
        const WNDPROC original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            themeColorEdit_,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(themeColorEditProcedure)));
        static_cast<void>(SetPropW(
            themeColorEdit_,
            themeColorEditOriginalProcedureProperty,
            reinterpret_cast<HANDLE>(original)));
    }

    advancedTimingHeading_ = createChild(
        L"BUTTON",
        L"时间与透明度",
        BS_GROUPBOX);
    advancedParticlesHeading_ = createChild(
        L"BUTTON",
        L"粒子与材质",
        BS_GROUPBOX);
    advancedRingsHeading_ = createChild(
        L"BUTTON",
        L"圆环参数",
        BS_GROUPBOX);
    advancedClickShardsHeading_ = createChild(
        L"BUTTON",
        L"点击碎片",
        BS_GROUPBOX);
    advancedBloomHeading_ = createChild(
        L"BUTTON",
        L"Bloom 参数",
        BS_GROUPBOX);

    bloomQualityLabel_ = createChild(
        L"STATIC",
        L"光晕扩散",
        SS_LEFT | SS_NOPREFIX);
    bloomQuality_ = createChild(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
        ControlId::BloomQuality);
    if (bloomQuality_ != nullptr)
    {
        static_cast<void>(SendMessageW(bloomQuality_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(L"紧凑")));
        static_cast<void>(SendMessageW(bloomQuality_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(L"适中")));
        static_cast<void>(SendMessageW(bloomQuality_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(L"原版")));
        static_cast<void>(SendMessageW(bloomQuality_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(L"极宽")));
        static_cast<void>(SendMessageW(bloomQuality_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(L"自定义")));
        static_cast<void>(SendMessageW(bloomQuality_, CB_SETMINVISIBLE, 5U, 0));
    }

    backgroundHeading_ = createChild(
        L"BUTTON",
        L"背景与主程序",
        BS_GROUPBOX);
    backgroundModeLabel_ = createChild(
        L"STATIC",
        L"渲染模式",
        SS_LEFT | SS_NOPREFIX);
    backgroundMode_ = createChild(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
        ControlId::BackgroundMode);
    if (backgroundMode_ != nullptr)
    {
        static_cast<void>(SendMessageW(backgroundMode_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(L"背景感知")));
        static_cast<void>(SendMessageW(backgroundMode_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(L"录屏兼容（测试，仅 Windows 11 26H1 及以后）")));
        static_cast<void>(SendMessageW(backgroundMode_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(L"浅色背景优化")));
        static_cast<void>(SendMessageW(backgroundMode_, CB_SETMINVISIBLE, 3U, 0));
    }

    cursorExcluded_ = createChild(
        L"BUTTON",
        L"排除鼠标指针",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::CursorExcluded);
    allowSystemBorder_ = createChild(
        L"BUTTON",
        L"允许黄色捕获边框",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::AllowSystemBorder);
    idleOptimization_ = createChild(
        L"BUTTON",
        L"空闲时降低资源占用",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::IdleOptimization);
    systemSettingsHeading_ = createChild(
        L"BUTTON",
        L"系统行为",
        BS_GROUPBOX);
    startWithWindows_ = createChild(
        L"BUTTON",
        L"随 Windows 启动",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::StartWithWindows);
    startMinimized_ = createChild(
        L"BUTTON",
        L"启动时最小化控制中心",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::StartMinimized);
    closeToTray_ = createChild(
        L"BUTTON",
        L"关闭控制中心时隐藏到托盘",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::CloseToTray);
#if defined(BAFX_ENABLE_SPOUT2)
    spout2Enabled_ = createChild(
        L"BUTTON",
        L"启用 OBS 透明特效输出",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::Spout2Enabled);
    spout2SenderStatus_ = createChild(
        L"STATIC",
        L"发送者状态：Host 未连接",
        SS_LEFT | SS_NOPREFIX);
    obsSpoutPluginStatus_ = createChild(
        L"STATIC",
        L"OBS 插件状态：尚未检测",
        SS_LEFT | SS_NOPREFIX);
    spout2ObsHint_ = createChild(
        L"STATIC",
        L"OBS 源须置顶并使用 Premultiplied Alpha；本程序不会自动修改 OBS。",
        SS_LEFT | SS_NOPREFIX);
    refreshObsSpoutPluginButton_ = createChild(
        L"BUTTON",
        L"重新检测 OBS 插件",
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::RefreshObsSpoutPlugin);
    openObsSpoutPluginPageButton_ = createChild(
        L"BUTTON",
        L"打开官方插件页面",
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::OpenObsSpoutPluginPage);
#endif
    clearLogsButton_ = createChild(
        L"BUTTON",
        L"清理诊断日志",
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::ClearLogs);

    displaySettingsHeading_ = createChild(
        L"BUTTON",
        L"显示与性能",
        BS_GROUPBOX);
    displaySelectorLabel_ = createChild(
        L"STATIC",
        L"显示器与离线独立设置",
        SS_LEFT | SS_NOPREFIX);
    displaySelector_ = createChild(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
        ControlId::DisplaySelector);
    if (displaySelector_ != nullptr)
    {
        static_cast<void>(SendMessageW(
            displaySelector_,
            CB_SETMINVISIBLE,
            8U,
            0));
    }
    displaySummaryText_ = createChild(
        L"STATIC",
        L"逐屏状态尚未加载",
        SS_LEFT | SS_NOPREFIX);
    hdrEnabled_ = createChild(
        L"BUTTON",
        L"全局请求 HDR 屏幕输出",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::HdrEnabled);
    framePacingLabel_ = createChild(
        L"STATIC",
        L"全局帧率策略",
        SS_LEFT | SS_NOPREFIX);
    framePacing_ = createChild(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
        ControlId::FramePacing);
    initializeFramePacingCombo(framePacing_);
    displayIndependent_ = createChild(
        L"BUTTON",
        L"使用独立设置",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::DisplayIndependent);
    displayEffectsEnabled_ = createChild(
        L"BUTTON",
        L"在此显示器启用特效",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::DisplayEffectsEnabled);
    displayHdrEnabled_ = createChild(
        L"BUTTON",
        L"在此显示器请求 HDR 输出",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::DisplayHdrEnabled);
    displayFramePacingLabel_ = createChild(
        L"STATIC",
        L"独立帧率策略",
        SS_LEFT | SS_NOPREFIX);
    displayFramePacing_ = createChild(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
        ControlId::DisplayFramePacing);
    initializeFramePacingCombo(displayFramePacing_);
    displayDetailsHeading_ = createChild(
        L"BUTTON",
        L"所选显示器设置与状态",
        BS_GROUPBOX);
    displayDetailsText_ = createChild(
        L"EDIT",
        L"Host 连接后显示逐屏运行状态。",
        ES_LEFT
            | ES_MULTILINE
            | ES_READONLY
            | ES_AUTOVSCROLL
            | WS_BORDER
            | WS_VSCROLL
            | WS_TABSTOP);
    pauseButton_ = createChild(
        L"BUTTON",
        L"暂停特效",
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::Pause);
    refreshButton_ = createChild(
        L"BUTTON",
        L"刷新状态",
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::Refresh);
    hostLifecycleButton_ = createChild(
        L"BUTTON",
        L"启动 Host",
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::HostLifecycle);
    resetDefaultsButton_ = createChild(
        L"BUTTON",
        L"重置默认",
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::ResetDefaults);

    const std::array required{
        titleText_,
        statusText_,
        messageText_,
        basicPageButton_,
        advancedPageButton_,
        displayPageButton_,
        systemPageButton_,
        effectsHeading_,
        effectsEnabled_,
        effectsModeLabel_,
        effectsMode_,
        clickEnabled_,
        trailEnabled_,
        trailAlwaysOn_,
        leftClickEnabled_,
        rightClickEnabled_,
        middleClickEnabled_,
        bloomQualityLabel_,
        bloomQuality_,
        backgroundHeading_,
        backgroundModeLabel_,
        backgroundMode_,
        cursorExcluded_,
        allowSystemBorder_,
        idleOptimization_,
        systemSettingsHeading_,
        startWithWindows_,
        startMinimized_,
        closeToTray_,
#if defined(BAFX_ENABLE_SPOUT2)
        spout2Enabled_,
        spout2SenderStatus_,
        obsSpoutPluginStatus_,
        spout2ObsHint_,
        refreshObsSpoutPluginButton_,
        openObsSpoutPluginPageButton_,
#endif
        displaySettingsHeading_,
        displaySelectorLabel_,
        displaySelector_,
        displaySummaryText_,
        hdrEnabled_,
        framePacingLabel_,
        framePacing_,
        displayIndependent_,
        displayEffectsEnabled_,
        displayHdrEnabled_,
        displayFramePacingLabel_,
        displayFramePacing_,
        displayDetailsHeading_,
        displayDetailsText_,
        pauseButton_,
        refreshButton_,
        hostLifecycleButton_,
        clearLogsButton_,
        resetDefaultsButton_,
        advancedTimingHeading_,
        advancedParticlesHeading_,
        advancedRingsHeading_,
        advancedClickShardsHeading_,
        advancedBloomHeading_,
        themeColorLabel_,
        themeColorEdit_,
        themeColorPreview_,
        themeColorChoose_,
        advancedTimingSectionButton_,
        advancedParticlesSectionButton_,
        advancedRingsSectionButton_,
        advancedClickShardsSectionButton_,
        advancedBloomSectionButton_};
    if (!slidersCreated
        || !advancedSlidersCreated
        || !particleSlidersCreated
        || !ringSlidersCreated
        || !clickShardSlidersCreated
        || std::ranges::find(required, nullptr) != required.end())
    {
        return false;
    }

    applyFonts();
    applyDpiMetrics();
    selectPage(Page::Basic);
#if defined(BAFX_ENABLE_SPOUT2)
    refreshObsPluginStatus();
#endif
    return true;
}

HWND ControlCenterWindow::createChild(
    const wchar_t* const className,
    const wchar_t* const text,
    const DWORD style,
    const ControlId id) const noexcept
{
    return CreateWindowExW(
        0U,
        className,
        text,
        WS_CHILD | WS_VISIBLE | style,
        0,
        0,
        0,
        0,
        window_,
        controlMenu(static_cast<int>(id)),
        instance_,
        nullptr);
}

bool ControlCenterWindow::createSlider(
    SliderControl& slider,
    const wchar_t* const label,
    const double minimum,
    const double maximum,
    const double step,
    std::string path,
    const ControlId id)
{
    slider.label = createChild(
        L"STATIC",
        label,
        SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX);
    slider.trackbar = createChild(
        TRACKBAR_CLASSW,
        L"",
        TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
        id);
    slider.valueText = createChild(
        L"STATIC",
        L"0",
        SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX);
    slider.minimum = minimum;
    slider.maximum = maximum;
    slider.step = step;
    slider.path = std::move(path);

    if (slider.label == nullptr
        || slider.trackbar == nullptr
        || slider.valueText == nullptr)
    {
        return false;
    }

    const int maximumPosition = static_cast<int>(std::lround(
        (maximum - minimum) / step));
    static_cast<void>(SendMessageW(slider.trackbar, TBM_SETRANGEMIN, FALSE, 0));
    static_cast<void>(SendMessageW(
        slider.trackbar,
        TBM_SETRANGEMAX,
        TRUE,
        maximumPosition));
    static_cast<void>(SendMessageW(slider.trackbar, TBM_SETPAGESIZE, 0U, 5));
    return true;
}

void ControlCenterWindow::createFonts()
{
    const HFONT oldNormal = normalFont_;
    const HFONT oldTitle = titleFont_;
    const HFONT oldSection = sectionFont_;

    const HFONT newNormal = CreateFontW(
        -MulDiv(10, static_cast<int>(layoutDpi_), 72),
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
    const HFONT newTitle = CreateFontW(
        -MulDiv(20, static_cast<int>(layoutDpi_), 72),
        0,
        0,
        0,
        FW_SEMIBOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
    const HFONT newSection = CreateFontW(
        -MulDiv(11, static_cast<int>(layoutDpi_), 72),
        0,
        0,
        0,
        FW_SEMIBOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");

    if (newNormal == nullptr || newTitle == nullptr || newSection == nullptr)
    {
        if (newNormal != nullptr)
        {
            DeleteObject(newNormal);
        }
        if (newTitle != nullptr)
        {
            DeleteObject(newTitle);
        }
        if (newSection != nullptr)
        {
            DeleteObject(newSection);
        }
        // Retain the currently selected fonts. Deleting a font that remains
        // selected in child controls would leave GDI with a dangling handle.
        return;
    }

    normalFont_ = newNormal;
    titleFont_ = newTitle;
    sectionFont_ = newSection;
    applyFonts();
    applyDpiMetrics();
    if (oldNormal != nullptr)
    {
        DeleteObject(oldNormal);
    }
    if (oldTitle != nullptr)
    {
        DeleteObject(oldTitle);
    }
    if (oldSection != nullptr)
    {
        DeleteObject(oldSection);
    }
}

void ControlCenterWindow::destroyFonts() noexcept
{
    if (normalFont_ != nullptr)
    {
        DeleteObject(normalFont_);
        normalFont_ = nullptr;
    }
    if (titleFont_ != nullptr)
    {
        DeleteObject(titleFont_);
        titleFont_ = nullptr;
    }
    if (sectionFont_ != nullptr)
    {
        DeleteObject(sectionFont_);
        sectionFont_ = nullptr;
    }
}

void ControlCenterWindow::applyFonts() const noexcept
{
    const std::array normalControls{
        statusText_,
        messageText_,
        basicPageButton_,
        advancedPageButton_,
        displayPageButton_,
        systemPageButton_,
        effectsEnabled_,
        effectsModeLabel_,
        effectsMode_,
        clickEnabled_,
        trailEnabled_,
        trailAlwaysOn_,
        leftClickEnabled_,
        rightClickEnabled_,
        middleClickEnabled_,
        globalScale_.label,
        globalScale_.trackbar,
        globalScale_.valueText,
        trailLength_.label,
        trailLength_.trackbar,
        trailLength_.valueText,
        trailWidth_.label,
        trailWidth_.trackbar,
        trailWidth_.valueText,
        inputSamplingRate_.label,
        inputSamplingRate_.trackbar,
        inputSamplingRate_.valueText,
        bloomIntensity_.label,
        bloomIntensity_.trackbar,
        bloomIntensity_.valueText,
        opacity_.label,
        opacity_.trackbar,
        opacity_.valueText,
        clickTimeScale_.label,
        clickTimeScale_.trackbar,
        clickTimeScale_.valueText,
        trailTimeScale_.label,
        trailTimeScale_.trackbar,
        trailTimeScale_.valueText,
        trailLifetimeMs_.label,
        trailLifetimeMs_.trackbar,
        trailLifetimeMs_.valueText,
        bloomDiffusion_.label,
        bloomDiffusion_.trackbar,
        bloomDiffusion_.valueText,
        bloomThreshold_.label,
        bloomThreshold_.trackbar,
        bloomThreshold_.valueText,
        bloomSoftKnee_.label,
        bloomSoftKnee_.trackbar,
        bloomSoftKnee_.valueText,
        bloomClamp_.label,
        bloomClamp_.trackbar,
        bloomClamp_.valueText,
        diskRadius_.label,
        diskRadius_.trackbar,
        diskRadius_.valueText,
        diskLifetimeMs_.label,
        diskLifetimeMs_.trackbar,
        diskLifetimeMs_.valueText,
        ringsHdrIntensity_.label,
        ringsHdrIntensity_.trackbar,
        ringsHdrIntensity_.valueText,
        ringsCount_.label,
        ringsCount_.trackbar,
        ringsCount_.valueText,
        ringsLifetimeMs_.label,
        ringsLifetimeMs_.trackbar,
        ringsLifetimeMs_.valueText,
        ringsRadiusMin_.label,
        ringsRadiusMin_.trackbar,
        ringsRadiusMin_.valueText,
        ringsRadiusMax_.label,
        ringsRadiusMax_.trackbar,
        ringsRadiusMax_.valueText,
        ringsAngularVelocityMultiplier_.label,
        ringsAngularVelocityMultiplier_.trackbar,
        ringsAngularVelocityMultiplier_.valueText,
        ringsRotationDirection_.label,
        ringsRotationDirection_.trackbar,
        ringsRotationDirection_.valueText,
        shardsHdrIntensity_.label,
        shardsHdrIntensity_.trackbar,
        shardsHdrIntensity_.valueText,
        shardsClickCount_.label,
        shardsClickCount_.trackbar,
        shardsClickCount_.valueText,
        shardsClickLifetimeMinMs_.label,
        shardsClickLifetimeMinMs_.trackbar,
        shardsClickLifetimeMinMs_.valueText,
        shardsClickLifetimeMaxMs_.label,
        shardsClickLifetimeMaxMs_.trackbar,
        shardsClickLifetimeMaxMs_.valueText,
        shardsClickRadius_.label,
        shardsClickRadius_.trackbar,
        shardsClickRadius_.valueText,
        shardsClickSpeedMin_.label,
        shardsClickSpeedMin_.trackbar,
        shardsClickSpeedMin_.valueText,
        shardsClickSpeedMax_.label,
        shardsClickSpeedMax_.trackbar,
        shardsClickSpeedMax_.valueText,
        shardsSizeMin_.label,
        shardsSizeMin_.trackbar,
        shardsSizeMin_.valueText,
        shardsSizeMax_.label,
        shardsSizeMax_.trackbar,
        shardsSizeMax_.valueText,
        trailOpacity_.label,
        trailOpacity_.trackbar,
        trailOpacity_.valueText,
        themeColorLabel_,
        themeColorEdit_,
        themeColorPreview_,
        themeColorChoose_,
        bloomQualityLabel_,
        bloomQuality_,
        backgroundModeLabel_,
        backgroundMode_,
        cursorExcluded_,
        allowSystemBorder_,
        idleOptimization_,
        startWithWindows_,
        startMinimized_,
        closeToTray_,
#if defined(BAFX_ENABLE_SPOUT2)
        spout2Enabled_,
        spout2SenderStatus_,
        obsSpoutPluginStatus_,
        spout2ObsHint_,
        refreshObsSpoutPluginButton_,
        openObsSpoutPluginPageButton_,
#endif
        displaySelectorLabel_,
        displaySelector_,
        displaySummaryText_,
        hdrEnabled_,
        framePacingLabel_,
        framePacing_,
        displayIndependent_,
        displayEffectsEnabled_,
        displayHdrEnabled_,
        displayFramePacingLabel_,
        displayFramePacing_,
        displayDetailsText_,
        pauseButton_,
        refreshButton_,
        hostLifecycleButton_,
        clearLogsButton_,
        resetDefaultsButton_};
    for (const HWND control : normalControls)
    {
        setControlFont(control, normalFont_);
    }
    setControlFont(titleText_, titleFont_);
    setControlFont(effectsHeading_, sectionFont_);
    setControlFont(backgroundHeading_, sectionFont_);
    setControlFont(systemSettingsHeading_, sectionFont_);
    setControlFont(advancedTimingHeading_, sectionFont_);
    setControlFont(advancedParticlesHeading_, sectionFont_);
    setControlFont(advancedRingsHeading_, sectionFont_);
    setControlFont(advancedClickShardsHeading_, sectionFont_);
    setControlFont(advancedBloomHeading_, sectionFont_);
    setControlFont(displaySettingsHeading_, sectionFont_);
    setControlFont(displayDetailsHeading_, sectionFont_);
    setControlFont(advancedTimingSectionButton_, normalFont_);
    setControlFont(advancedParticlesSectionButton_, normalFont_);
    setControlFont(advancedRingsSectionButton_, normalFont_);
    setControlFont(advancedClickShardsSectionButton_, normalFont_);
    setControlFont(advancedBloomSectionButton_, normalFont_);
}

void ControlCenterWindow::applyDpiMetrics() const noexcept
{
    const std::array comboBoxes{
        bloomQuality_,
        effectsMode_,
        backgroundMode_,
        displaySelector_,
        framePacing_,
        displayFramePacing_};
    for (const HWND comboBox : comboBoxes)
    {
        if (comboBox == nullptr)
        {
            continue;
        }
        // A custom font does not automatically update the native combo-box
        // item height after WM_DPICHANGED. Explicit metrics prevent clipped
        // Chinese glyphs on 125%-200% scale factors.
        static_cast<void>(SendMessageW(
            comboBox,
            CB_SETITEMHEIGHT,
            static_cast<WPARAM>(-1),
            scale(26)));
        static_cast<void>(SendMessageW(comboBox, CB_SETITEMHEIGHT, 0U, scale(26)));
    }
}

void ControlCenterWindow::adaptLayoutToMonitor(
    const HMONITOR monitor,
    const bool force)
{
    if (window_ == nullptr || monitor == nullptr)
    {
        return;
    }
    if (!force && monitor == layoutMonitor_)
    {
        return;
    }

    const UINT windowDpi = GetDpiForWindow(window_);
    if (windowDpi != 0U && windowDpi != dpi_)
    {
        // A real DPI transition is finalized by WM_DPICHANGED, which also
        // supplies the OS-recommended bounds. Do not race that transaction
        // from an earlier WM_MOVE notification.
        return;
    }

    const RECT workArea = monitorWorkArea(monitor);
    const UINT nextLayoutDpi = controlCenterLayoutDpi(
        maximumClientSize(workArea, dpi_),
        dpi_);
    const bool layoutChanged = nextLayoutDpi != layoutDpi_;
    layoutMonitor_ = monitor;
    if (layoutChanged)
    {
        layoutDpi_ = nextLayoutDpi;
        createFonts();
    }

    if (IsIconic(window_) != FALSE || IsZoomed(window_) != FALSE)
    {
        RECT client{};
        if (layoutChanged && GetClientRect(window_, &client) != FALSE)
        {
            layoutControls(
                client.right - client.left,
                client.bottom - client.top);
        }
        return;
    }

    RECT currentBounds{};
    if (GetWindowRect(window_, &currentBounds) == FALSE)
    {
        return;
    }
    RECT minimumBounds{
        0,
        0,
        scale(minimumControlCenterClientWidth),
        scale(minimumControlCenterClientHeight)};
    static_cast<void>(AdjustWindowRectExForDpi(
        &minimumBounds,
        static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE)),
        FALSE,
        static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_EXSTYLE)),
        dpi_));

    const int currentWidth = currentBounds.right - currentBounds.left;
    const int currentHeight = currentBounds.bottom - currentBounds.top;
    const PixelSize targetSize = clampPixelSize(
        PixelSize{
            (std::max)(
                currentWidth,
                static_cast<int>(minimumBounds.right - minimumBounds.left)),
            (std::max)(
                currentHeight,
                static_cast<int>(minimumBounds.bottom - minimumBounds.top))},
        PixelSize{
            static_cast<int>(workArea.right - workArea.left),
            static_cast<int>(workArea.bottom - workArea.top)});
    const int targetX = (std::clamp)(
        static_cast<int>(currentBounds.left),
        static_cast<int>(workArea.left),
        static_cast<int>(workArea.right) - targetSize.width);
    const int targetY = (std::clamp)(
        static_cast<int>(currentBounds.top),
        static_cast<int>(workArea.top),
        static_cast<int>(workArea.bottom) - targetSize.height);
    const bool sizeChanged = targetSize.width != currentWidth
        || targetSize.height != currentHeight;
    const bool positionChanged = targetX != currentBounds.left
        || targetY != currentBounds.top;

    BOOL repositioned = TRUE;
    if (sizeChanged || positionChanged)
    {
        repositioned = SetWindowPos(
            window_,
            nullptr,
            targetX,
            targetY,
            targetSize.width,
            targetSize.height,
            SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOZORDER);
    }
    if (layoutChanged && (!sizeChanged || repositioned == FALSE))
    {
        RECT client{};
        if (GetClientRect(window_, &client) != FALSE)
        {
            layoutControls(
                client.right - client.left,
                client.bottom - client.top);
        }
    }
}

void ControlCenterWindow::layoutControls(
    const int clientWidth,
    const int clientHeight) const noexcept
{
    if (titleText_ == nullptr || clientWidth <= 0 || clientHeight <= 0)
    {
        return;
    }

    // Keep all sibling moves paint-free while their overlapping rectangles are
    // changing. The final redraw below is the single committed frame for the
    // complete layout, including the parent background.

    const int margin = scale(24);
    const int columnGap = scale(24);
    const int availableRightWidth = (std::clamp)(
        clientWidth / 3,
        scale(280),
        scale(340));
    const int leftWidth = clientWidth
        - margin * 2
        - columnGap
        - availableRightWidth;
    const int rightX = margin + leftWidth + columnGap;

    moveControl(titleText_, margin, scale(16), clientWidth - margin * 2, scale(36));
    moveControl(statusText_, margin, scale(54), clientWidth - margin * 2, scale(24));
    // setInfo() writes a title and detail on separate lines. Reserve both lines
    // while keeping the page-tab band fixed across DPI transitions.
    const int messageHeight = scale(36);
    moveControl(messageText_, margin, scale(82), clientWidth - margin * 2, messageHeight);

    const int contentTop = scale(150);
    const int tabWidth = scale(132);
    const int tabGap = scale(8);
    moveControl(
        basicPageButton_,
        margin,
        scale(120),
        tabWidth,
        scale(30));
    moveControl(
        advancedPageButton_,
        margin + tabWidth + tabGap,
        scale(120),
        tabWidth,
        scale(30));
    moveControl(
        displayPageButton_,
        margin + (tabWidth + tabGap) * 2,
        scale(120),
        tabWidth,
        scale(30));
    moveControl(
        systemPageButton_,
        margin + (tabWidth + tabGap) * 3,
        scale(120),
        tabWidth,
        scale(30));

    if (activePage_ == Page::DisplayPerformance)
    {
        const int actionHeight = scale(38);
        const int actionGap = scale(10);
        const int actionY = (std::max)(
            contentTop + scale(262),
            clientHeight - margin - actionHeight);
        const int panelHeight = (std::max)(
            scale(262),
            actionY - contentTop - scale(12));
        const int settingsWidth = (std::clamp)(
            clientWidth / 3,
            scale(300),
            scale(360));
        const int detailsX = margin + settingsWidth + columnGap;
        const int detailsWidth = (std::max)(
            scale(1),
            clientWidth - detailsX - margin);
        const int inset = scale(16);
        const int settingsContentX = margin + inset;
        const int settingsContentWidth = (std::max)(
            scale(1),
            settingsWidth - inset * 2);

        moveControl(
            displaySettingsHeading_,
            margin,
            contentTop,
            settingsWidth,
            panelHeight);
        moveControl(
            displaySelectorLabel_,
            settingsContentX,
            contentTop + scale(30),
            settingsContentWidth,
            scale(22));
        moveControl(
            displaySelector_,
            settingsContentX,
            contentTop + scale(52),
            settingsContentWidth,
            scale(34));
        moveControl(
            displaySummaryText_,
            settingsContentX,
            contentTop + scale(94),
            settingsContentWidth,
            scale(48));
        moveControl(
            hdrEnabled_,
            settingsContentX,
            contentTop + scale(148),
            settingsContentWidth,
            scale(30));
        moveControl(
            framePacingLabel_,
            settingsContentX,
            contentTop + scale(184),
            settingsContentWidth,
            scale(22));
        moveControl(
            framePacing_,
            settingsContentX,
            contentTop + scale(206),
            settingsContentWidth,
            scale(34));

        moveControl(
            displayDetailsHeading_,
            detailsX,
            contentTop,
            detailsWidth,
            panelHeight);
        const int detailsContentX = detailsX + inset;
        const int detailsContentWidth = (std::max)(
            scale(1),
            detailsWidth - inset * 2);
        const int policyColumnGap = scale(12);
        const int policyColumnWidth = (std::max)(
            scale(1),
            (detailsContentWidth - policyColumnGap) / 2);
        moveControl(
            displayIndependent_,
            detailsContentX,
            contentTop + scale(28),
            policyColumnWidth,
            scale(30));
        moveControl(
            displayEffectsEnabled_,
            detailsContentX + policyColumnWidth + policyColumnGap,
            contentTop + scale(28),
            policyColumnWidth,
            scale(30));
        moveControl(
            displayHdrEnabled_,
            detailsContentX,
            contentTop + scale(62),
            policyColumnWidth,
            scale(30));
        moveControl(
            displayFramePacingLabel_,
            detailsContentX + policyColumnWidth + policyColumnGap,
            contentTop + scale(62),
            policyColumnWidth,
            scale(22));
        moveControl(
            displayFramePacing_,
            detailsContentX + policyColumnWidth + policyColumnGap,
            contentTop + scale(84),
            policyColumnWidth,
            scale(34));
        moveControl(
            displayDetailsText_,
            detailsContentX,
            contentTop + scale(124),
            detailsContentWidth,
            (std::max)(scale(1), panelHeight - scale(140)));

        const int actionWidth = (clientWidth - margin * 2 - actionGap * 3) / 4;
        moveControl(
            pauseButton_,
            margin,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            refreshButton_,
            margin + actionWidth + actionGap,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            hostLifecycleButton_,
            margin + (actionWidth + actionGap) * 2,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            resetDefaultsButton_,
            margin + (actionWidth + actionGap) * 3,
            actionY,
            actionWidth,
            actionHeight);
        redrawWindowTree();
        return;
    }

    if (activePage_ == Page::System)
    {
        const int actionHeight = scale(38);
        const int actionGap = scale(10);
#if defined(BAFX_ENABLE_SPOUT2)
        constexpr int minimumSystemContentHeight = 356;
        constexpr int minimumSystemPanelHeight = 324;
#else
        constexpr int minimumSystemContentHeight = 192;
        constexpr int minimumSystemPanelHeight = 160;
#endif
        const int actionY = (std::max)(
            contentTop + scale(minimumSystemContentHeight),
            clientHeight - margin - actionHeight);
        const int panelHeight = (std::max)(
            scale(minimumSystemPanelHeight),
            actionY - contentTop - scale(12));
        const int panelWidth = clientWidth - margin * 2;
        const int inset = scale(16);
        const int contentX = margin + inset;
        const int contentWidth = (std::max)(scale(1), panelWidth - inset * 2);

        moveControl(
            systemSettingsHeading_,
            margin,
            contentTop,
            panelWidth,
            panelHeight);
        moveControl(
            startWithWindows_,
            contentX,
            contentTop + scale(32),
            contentWidth,
            scale(30));
        moveControl(
            startMinimized_,
            contentX,
            contentTop + scale(64),
            contentWidth,
            scale(30));
        moveControl(
            closeToTray_,
            contentX,
            contentTop + scale(96),
            contentWidth,
            scale(30));
#if defined(BAFX_ENABLE_SPOUT2)
        moveControl(
            spout2Enabled_,
            contentX,
            contentTop + scale(128),
            contentWidth,
            scale(30));
        moveControl(
            spout2SenderStatus_,
            contentX,
            contentTop + scale(164),
            contentWidth,
            scale(42));
        moveControl(
            obsSpoutPluginStatus_,
            contentX,
            contentTop + scale(208),
            contentWidth,
            scale(42));
        moveControl(
            spout2ObsHint_,
            contentX,
            contentTop + scale(252),
            contentWidth,
            scale(26));
        const int obsButtonGap = scale(10);
        const int obsButtonWidth = (contentWidth - obsButtonGap) / 2;
        moveControl(
            refreshObsSpoutPluginButton_,
            contentX,
            contentTop + scale(282),
            obsButtonWidth,
            scale(30));
        moveControl(
            openObsSpoutPluginPageButton_,
            contentX + obsButtonWidth + obsButtonGap,
            contentTop + scale(282),
            obsButtonWidth,
            scale(30));
        moveControl(
            clearLogsButton_,
            contentX,
            contentTop + scale(316),
            contentWidth,
            scale(30));
#else
        moveControl(
            clearLogsButton_,
            contentX,
            contentTop + scale(164),
            contentWidth,
            scale(30));
#endif

        const int actionWidth = (clientWidth - margin * 2 - actionGap * 3) / 4;
        moveControl(
            pauseButton_,
            margin,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            refreshButton_,
            margin + actionWidth + actionGap,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            hostLifecycleButton_,
            margin + (actionWidth + actionGap) * 2,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            resetDefaultsButton_,
            margin + (actionWidth + actionGap) * 3,
            actionY,
            actionWidth,
            actionHeight);
        redrawWindowTree();
        return;
    }

    if (activePage_ == Page::Advanced)
    {
        const int sectionButtonGap = scale(8);
        constexpr int sectionButtonCount = 5;
        const int sectionButtonWidth = (std::max)(
            scale(1),
            (clientWidth - margin * 2
                - sectionButtonGap * (sectionButtonCount - 1))
                / sectionButtonCount);
        moveControl(
            advancedTimingSectionButton_,
            margin,
            contentTop,
            sectionButtonWidth,
            scale(30));
        moveControl(
            advancedParticlesSectionButton_,
            margin + sectionButtonWidth + sectionButtonGap,
            contentTop,
            sectionButtonWidth,
            scale(30));
        moveControl(
            advancedRingsSectionButton_,
            margin + (sectionButtonWidth + sectionButtonGap) * 2,
            contentTop,
            sectionButtonWidth,
            scale(30));
        moveControl(
            advancedClickShardsSectionButton_,
            margin + (sectionButtonWidth + sectionButtonGap) * 3,
            contentTop,
            sectionButtonWidth,
            scale(30));
        moveControl(
            advancedBloomSectionButton_,
            margin + (sectionButtonWidth + sectionButtonGap) * 4,
            contentTop,
            sectionButtonWidth,
            scale(30));

        const int panelTop = contentTop + scale(38);
        const int actionHeight = scale(38);
        const int actionGap = scale(10);
        const int actionY = (std::max)(
            panelTop + scale(262),
            clientHeight - margin - actionHeight);
        const int groupHeight = (std::max)(
            scale(262),
            actionY - panelTop - scale(12));
        const int groupWidth = clientWidth - margin * 2;
        const int inset = scale(16);
        const int advancedColumnGap = scale(24);
        const int columnWidth = (std::max)(
            scale(1),
            (groupWidth - inset * 2 - advancedColumnGap) / 2);
        const int left = margin + inset;
        const int right = left + columnWidth + advancedColumnGap;
        const int rowTop = panelTop + scale(32);
        const int nextRowTop = rowTop + scale(50);
        const int thirdRowTop = nextRowTop + scale(50);
        const int fourthRowTop = thirdRowTop + scale(50);

        switch (activeAdvancedSection_)
        {
        case AdvancedSection::Timing:
            moveControl(
                advancedTimingHeading_,
                margin,
                panelTop,
                groupWidth,
                groupHeight);
            layoutSlider(opacity_, left, rowTop, columnWidth, scale(40));
            layoutSlider(
                clickTimeScale_,
                left,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                trailTimeScale_,
                right,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                trailLifetimeMs_,
                right,
                nextRowTop,
                columnWidth,
                scale(40));
            break;
        case AdvancedSection::Particles:
            moveControl(
                advancedParticlesHeading_,
                margin,
                panelTop,
                groupWidth,
                groupHeight);
            layoutSlider(diskRadius_, left, rowTop, columnWidth, scale(40));
            layoutSlider(
                diskLifetimeMs_,
                left,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                trailOpacity_,
                left,
                thirdRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                ringsHdrIntensity_,
                right,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsHdrIntensity_,
                right,
                nextRowTop,
                columnWidth,
                scale(40));
            moveControl(
                themeColorLabel_,
                left,
                fourthRowTop,
                scale(72),
                scale(40));
            moveControl(
                themeColorEdit_,
                left + scale(78),
                fourthRowTop,
                scale(112),
                scale(40));
            moveControl(
                themeColorPreview_,
                left + scale(196),
                fourthRowTop,
                scale(40),
                scale(40));
            moveControl(
                themeColorChoose_,
                left + scale(242),
                fourthRowTop,
                scale(78),
                scale(40));
            break;
        case AdvancedSection::Rings:
            moveControl(
                advancedRingsHeading_,
                margin,
                panelTop,
                groupWidth,
                groupHeight);
            layoutSlider(ringsCount_, left, rowTop, columnWidth, scale(40));
            layoutSlider(
                ringsLifetimeMs_,
                right,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                ringsRadiusMin_,
                left,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                ringsRadiusMax_,
                right,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                ringsAngularVelocityMultiplier_,
                left,
                thirdRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                ringsRotationDirection_,
                right,
                thirdRowTop,
                columnWidth,
                scale(40));
            break;
        case AdvancedSection::ClickShards:
            moveControl(
                advancedClickShardsHeading_,
                margin,
                panelTop,
                groupWidth,
                groupHeight);
            layoutSlider(
                shardsClickCount_,
                left,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsClickRadius_,
                right,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsClickLifetimeMinMs_,
                left,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsClickLifetimeMaxMs_,
                right,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsClickSpeedMin_,
                left,
                thirdRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsClickSpeedMax_,
                right,
                thirdRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsSizeMin_,
                left,
                fourthRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsSizeMax_,
                right,
                fourthRowTop,
                columnWidth,
                scale(40));
            break;
        case AdvancedSection::Bloom:
            moveControl(
                advancedBloomHeading_,
                margin,
                panelTop,
                groupWidth,
                groupHeight);
            layoutSlider(
                bloomDiffusion_,
                left,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                bloomThreshold_,
                left,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                bloomSoftKnee_,
                right,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                bloomClamp_,
                right,
                nextRowTop,
                columnWidth,
                scale(40));
            break;
        }

        const int actionWidth = (clientWidth - margin * 2 - actionGap * 3) / 4;
        moveControl(
            pauseButton_,
            margin,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            refreshButton_,
            margin + (actionWidth + actionGap),
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            hostLifecycleButton_,
            margin + (actionWidth + actionGap) * 2,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            resetDefaultsButton_,
            margin + (actionWidth + actionGap) * 3,
            actionY,
            actionWidth,
            actionHeight);
        redrawWindowTree();
        return;
    }

    const int groupHeight = (std::max)(
        scale(350),
        clientHeight - contentTop - margin);
    moveControl(effectsHeading_, margin, contentTop, leftWidth, groupHeight);

    const int groupInset = scale(16);
    const int groupLeft = margin + groupInset;
    const int groupWidth = (std::max)(scale(1), leftWidth - groupInset * 2);
    moveControl(
        effectsModeLabel_,
        groupLeft,
        contentTop + scale(28),
        groupWidth,
        scale(22));
    moveControl(
        effectsMode_,
        groupLeft,
        contentTop + scale(50),
        groupWidth,
        scale(34));
    const int checkboxTop = contentTop + scale(88);
    const int checkboxWidth = groupWidth / 4;
    moveControl(effectsEnabled_, groupLeft, checkboxTop, checkboxWidth, scale(30));
    moveControl(clickEnabled_, groupLeft + checkboxWidth, checkboxTop, checkboxWidth, scale(30));
    moveControl(trailEnabled_, groupLeft + checkboxWidth * 2, checkboxTop, checkboxWidth, scale(30));
    moveControl(
        trailAlwaysOn_,
        groupLeft + checkboxWidth * 3,
        checkboxTop,
        groupWidth - checkboxWidth * 3,
        scale(30));

    // Keep the button-specific input switches on their own row so translated
    // labels remain readable on the smallest supported DPI-scaled layout.
    const int inputCheckboxTop = checkboxTop + scale(30);
    moveControl(
        leftClickEnabled_,
        groupLeft,
        inputCheckboxTop,
        checkboxWidth,
        scale(30));
    moveControl(
        rightClickEnabled_,
        groupLeft + checkboxWidth,
        inputCheckboxTop,
        checkboxWidth,
        scale(30));
    moveControl(
        middleClickEnabled_,
        groupLeft + checkboxWidth * 2,
        inputCheckboxTop,
        checkboxWidth,
        scale(30));

    int sliderTop = inputCheckboxTop + scale(30);
    layoutSlider(globalScale_, groupLeft, sliderTop, groupWidth, scale(40));
    sliderTop += scale(44);
    layoutSlider(trailLength_, groupLeft, sliderTop, groupWidth, scale(40));
    sliderTop += scale(44);
    layoutSlider(trailWidth_, groupLeft, sliderTop, groupWidth, scale(40));
    sliderTop += scale(44);
    layoutSlider(inputSamplingRate_, groupLeft, sliderTop, groupWidth, scale(40));
    sliderTop += scale(44);
    layoutSlider(bloomIntensity_, groupLeft, sliderTop, groupWidth, scale(40));
    sliderTop += scale(44);

    const int labelWidth = scale(106);
    moveControl(bloomQualityLabel_, groupLeft, sliderTop, labelWidth, scale(34));
    moveControl(
        bloomQuality_,
        groupLeft + labelWidth,
        sliderTop,
        groupWidth - labelWidth,
        scale(34));

    moveControl(backgroundHeading_, rightX, contentTop, availableRightWidth, groupHeight);
    const int rightContentX = rightX + groupInset;
    const int rightContentWidth = (std::max)(
        scale(1),
        availableRightWidth - groupInset * 2);
    moveControl(
        backgroundModeLabel_,
        rightContentX,
        contentTop + scale(32),
        rightContentWidth,
        scale(24));
    moveControl(
        backgroundMode_,
        rightContentX,
        contentTop + scale(58),
        rightContentWidth,
        scale(34));
    moveControl(
        cursorExcluded_,
        rightContentX,
        contentTop + scale(101),
        rightContentWidth,
        scale(30));
    moveControl(
        allowSystemBorder_,
        rightContentX,
        contentTop + scale(133),
        rightContentWidth,
        scale(30));
    moveControl(
        idleOptimization_,
        rightContentX,
        contentTop + scale(165),
        rightContentWidth,
        scale(30));
    moveControl(
        pauseButton_,
        rightContentX,
        contentTop + scale(201),
        rightContentWidth,
        scale(38));

    const int actionGap = scale(10);
    const int actionWidth = (rightContentWidth - actionGap) / 2;
    moveControl(
        refreshButton_,
        rightContentX,
        contentTop + scale(249),
        actionWidth,
        scale(38));
    moveControl(
        hostLifecycleButton_,
        rightContentX + actionWidth + actionGap,
        contentTop + scale(249),
        actionWidth,
        scale(38));
    moveControl(
        resetDefaultsButton_,
        rightContentX,
        contentTop + scale(297),
        rightContentWidth,
        scale(38));

    redrawWindowTree();
}

void ControlCenterWindow::redrawWindowTree() const noexcept
{
    if (window_ == nullptr)
    {
        return;
    }

    static_cast<void>(RedrawWindow(
        window_,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW));
}

void ControlCenterWindow::layoutSlider(
    const SliderControl& slider,
    const int x,
    const int y,
    const int width,
    const int height) const noexcept
{
    const int labelWidth = scale(132);
    const int valueWidth = scale(64);
    const int gap = scale(8);
    moveControl(slider.label, x, y, labelWidth, height);
    moveControl(
        slider.trackbar,
        x + labelWidth,
        y,
        width - labelWidth - valueWidth - gap,
        height);
    moveControl(
        slider.valueText,
        x + width - valueWidth,
        y,
        valueWidth,
        height);
}

void ControlCenterWindow::setPageControlVisible(
    const HWND control,
    const bool visible) const noexcept
{
    if (control != nullptr)
    {
        ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
    }
}

void ControlCenterWindow::selectPage(const Page page) noexcept
{
    activePage_ = page;
    updatePageVisibility();
}

void ControlCenterWindow::selectAdvancedSection(
    const AdvancedSection section) noexcept
{
    activeAdvancedSection_ = section;
    updatePageVisibility();
}

void ControlCenterWindow::updatePageVisibility() noexcept
{
    const bool basic = activePage_ == Page::Basic;
    const bool advanced = activePage_ == Page::Advanced;
    const bool display = activePage_ == Page::DisplayPerformance;
    const bool system = activePage_ == Page::System;
    static_cast<void>(SendMessageW(
        basicPageButton_,
        BM_SETCHECK,
        basic ? BST_CHECKED : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        advancedPageButton_,
        BM_SETCHECK,
        advanced ? BST_CHECKED : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        displayPageButton_,
        BM_SETCHECK,
        display ? BST_CHECKED : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        systemPageButton_,
        BM_SETCHECK,
        system ? BST_CHECKED : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        advancedTimingSectionButton_,
        BM_SETCHECK,
        activeAdvancedSection_ == AdvancedSection::Timing
            ? BST_CHECKED
            : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        advancedParticlesSectionButton_,
        BM_SETCHECK,
        activeAdvancedSection_ == AdvancedSection::Particles
            ? BST_CHECKED
            : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        advancedRingsSectionButton_,
        BM_SETCHECK,
        activeAdvancedSection_ == AdvancedSection::Rings
            ? BST_CHECKED
            : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        advancedClickShardsSectionButton_,
        BM_SETCHECK,
        activeAdvancedSection_ == AdvancedSection::ClickShards
            ? BST_CHECKED
            : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        advancedBloomSectionButton_,
        BM_SETCHECK,
        activeAdvancedSection_ == AdvancedSection::Bloom
            ? BST_CHECKED
            : BST_UNCHECKED,
        0));

    const std::array basicControls{
        effectsHeading_,
        effectsEnabled_,
        effectsModeLabel_,
        effectsMode_,
        clickEnabled_,
        trailEnabled_,
        trailAlwaysOn_,
        leftClickEnabled_,
        rightClickEnabled_,
        middleClickEnabled_,
        globalScale_.label,
        globalScale_.trackbar,
        globalScale_.valueText,
        trailLength_.label,
        trailLength_.trackbar,
        trailLength_.valueText,
        trailWidth_.label,
        trailWidth_.trackbar,
        trailWidth_.valueText,
        inputSamplingRate_.label,
        inputSamplingRate_.trackbar,
        inputSamplingRate_.valueText,
        bloomIntensity_.label,
        bloomIntensity_.trackbar,
        bloomIntensity_.valueText,
        bloomQualityLabel_,
        bloomQuality_,
        backgroundHeading_,
        backgroundModeLabel_,
        backgroundMode_,
        cursorExcluded_,
        allowSystemBorder_,
        idleOptimization_};
    for (const HWND control : basicControls)
    {
        setPageControlVisible(control, basic);
    }

    const std::array advancedSectionButtons{
        advancedTimingSectionButton_,
        advancedParticlesSectionButton_,
        advancedRingsSectionButton_,
        advancedClickShardsSectionButton_,
        advancedBloomSectionButton_};
    for (const HWND control : advancedSectionButtons)
    {
        setPageControlVisible(control, advanced);
    }

    const bool timing = advanced
        && activeAdvancedSection_ == AdvancedSection::Timing;
    const std::array advancedTimingControls{
        advancedTimingHeading_,
        opacity_.label,
        opacity_.trackbar,
        opacity_.valueText,
        clickTimeScale_.label,
        clickTimeScale_.trackbar,
        clickTimeScale_.valueText,
        trailTimeScale_.label,
        trailTimeScale_.trackbar,
        trailTimeScale_.valueText,
        trailLifetimeMs_.label,
        trailLifetimeMs_.trackbar,
        trailLifetimeMs_.valueText};
    for (const HWND control : advancedTimingControls)
    {
        setPageControlVisible(control, timing);
    }

    const bool particles = advanced
        && activeAdvancedSection_ == AdvancedSection::Particles;
    const std::array advancedParticleControls{
        advancedParticlesHeading_,
        diskRadius_.label,
        diskRadius_.trackbar,
        diskRadius_.valueText,
        diskLifetimeMs_.label,
        diskLifetimeMs_.trackbar,
        diskLifetimeMs_.valueText,
        ringsHdrIntensity_.label,
        ringsHdrIntensity_.trackbar,
        ringsHdrIntensity_.valueText,
        shardsHdrIntensity_.label,
        shardsHdrIntensity_.trackbar,
        shardsHdrIntensity_.valueText,
        trailOpacity_.label,
        trailOpacity_.trackbar,
        trailOpacity_.valueText,
        themeColorLabel_,
        themeColorEdit_,
        themeColorPreview_,
        themeColorChoose_};
    for (const HWND control : advancedParticleControls)
    {
        setPageControlVisible(control, particles);
    }

    const bool rings = advanced
        && activeAdvancedSection_ == AdvancedSection::Rings;
    const std::array advancedRingControls{
        advancedRingsHeading_,
        ringsCount_.label,
        ringsCount_.trackbar,
        ringsCount_.valueText,
        ringsLifetimeMs_.label,
        ringsLifetimeMs_.trackbar,
        ringsLifetimeMs_.valueText,
        ringsRadiusMin_.label,
        ringsRadiusMin_.trackbar,
        ringsRadiusMin_.valueText,
        ringsRadiusMax_.label,
        ringsRadiusMax_.trackbar,
        ringsRadiusMax_.valueText,
        ringsAngularVelocityMultiplier_.label,
        ringsAngularVelocityMultiplier_.trackbar,
        ringsAngularVelocityMultiplier_.valueText,
        ringsRotationDirection_.label,
        ringsRotationDirection_.trackbar,
        ringsRotationDirection_.valueText};
    for (const HWND control : advancedRingControls)
    {
        setPageControlVisible(control, rings);
    }

    const bool clickShards = advanced
        && activeAdvancedSection_ == AdvancedSection::ClickShards;
    const std::array advancedClickShardControls{
        advancedClickShardsHeading_,
        shardsClickCount_.label,
        shardsClickCount_.trackbar,
        shardsClickCount_.valueText,
        shardsClickLifetimeMinMs_.label,
        shardsClickLifetimeMinMs_.trackbar,
        shardsClickLifetimeMinMs_.valueText,
        shardsClickLifetimeMaxMs_.label,
        shardsClickLifetimeMaxMs_.trackbar,
        shardsClickLifetimeMaxMs_.valueText,
        shardsClickRadius_.label,
        shardsClickRadius_.trackbar,
        shardsClickRadius_.valueText,
        shardsClickSpeedMin_.label,
        shardsClickSpeedMin_.trackbar,
        shardsClickSpeedMin_.valueText,
        shardsClickSpeedMax_.label,
        shardsClickSpeedMax_.trackbar,
        shardsClickSpeedMax_.valueText,
        shardsSizeMin_.label,
        shardsSizeMin_.trackbar,
        shardsSizeMin_.valueText,
        shardsSizeMax_.label,
        shardsSizeMax_.trackbar,
        shardsSizeMax_.valueText};
    for (const HWND control : advancedClickShardControls)
    {
        setPageControlVisible(control, clickShards);
    }

    const bool bloom = advanced
        && activeAdvancedSection_ == AdvancedSection::Bloom;
    const std::array advancedBloomControls{
        advancedBloomHeading_,
        bloomDiffusion_.label,
        bloomDiffusion_.trackbar,
        bloomDiffusion_.valueText,
        bloomThreshold_.label,
        bloomThreshold_.trackbar,
        bloomThreshold_.valueText,
        bloomSoftKnee_.label,
        bloomSoftKnee_.trackbar,
        bloomSoftKnee_.valueText,
        bloomClamp_.label,
        bloomClamp_.trackbar,
        bloomClamp_.valueText};
    for (const HWND control : advancedBloomControls)
    {
        setPageControlVisible(control, bloom);
    }

    const std::array displayControls{
        displaySettingsHeading_,
        displaySelectorLabel_,
        displaySelector_,
        displaySummaryText_,
        hdrEnabled_,
        framePacingLabel_,
        framePacing_,
        displayIndependent_,
        displayEffectsEnabled_,
        displayHdrEnabled_,
        displayFramePacingLabel_,
        displayFramePacing_,
        displayDetailsHeading_,
        displayDetailsText_};
    for (const HWND control : displayControls)
    {
        setPageControlVisible(control, display);
    }

    const std::array systemControls{
        systemSettingsHeading_,
        startWithWindows_,
        startMinimized_,
        closeToTray_,
#if defined(BAFX_ENABLE_SPOUT2)
        spout2Enabled_,
        spout2SenderStatus_,
        obsSpoutPluginStatus_,
        spout2ObsHint_,
        refreshObsSpoutPluginButton_,
        openObsSpoutPluginPageButton_,
#endif
        clearLogsButton_};
    for (const HWND control : systemControls)
    {
        setPageControlVisible(control, system);
    }

    if (window_ != nullptr)
    {
        RECT client{};
        if (GetClientRect(window_, &client) != FALSE)
        {
            layoutControls(
                client.right - client.left,
                client.bottom - client.top);
            return;
        }
        redrawWindowTree();
    }
}

int ControlCenterWindow::scale(const int logicalPixels) const noexcept
{
    return MulDiv(logicalPixels, static_cast<int>(layoutDpi_), 96);
}

void ControlCenterWindow::onCommand(
    const int id,
    const int notificationCode)
{
    if (updatingControls_)
    {
        return;
    }

    switch (static_cast<ControlId>(id))
    {
    case ControlId::BasicPage:
        if (notificationCode == BN_CLICKED)
        {
            selectPage(Page::Basic);
        }
        break;
    case ControlId::AdvancedPage:
        if (notificationCode == BN_CLICKED)
        {
            selectPage(Page::Advanced);
        }
        break;
    case ControlId::DisplayPage:
        if (notificationCode == BN_CLICKED)
        {
            selectPage(Page::DisplayPerformance);
        }
        break;
    case ControlId::SystemPage:
        if (notificationCode == BN_CLICKED)
        {
            selectPage(Page::System);
#if defined(BAFX_ENABLE_SPOUT2)
            refreshObsPluginStatus();
#endif
        }
        break;
    case ControlId::AdvancedTimingSection:
        if (notificationCode == BN_CLICKED)
        {
            selectAdvancedSection(AdvancedSection::Timing);
        }
        break;
    case ControlId::AdvancedParticlesSection:
        if (notificationCode == BN_CLICKED)
        {
            selectAdvancedSection(AdvancedSection::Particles);
        }
        break;
    case ControlId::AdvancedRingsSection:
        if (notificationCode == BN_CLICKED)
        {
            selectAdvancedSection(AdvancedSection::Rings);
        }
        break;
    case ControlId::AdvancedClickShardsSection:
        if (notificationCode == BN_CLICKED)
        {
            selectAdvancedSection(AdvancedSection::ClickShards);
        }
        break;
    case ControlId::AdvancedBloomSection:
        if (notificationCode == BN_CLICKED)
        {
            selectAdvancedSection(AdvancedSection::Bloom);
        }
        break;
    case ControlId::Pause:
        if (notificationCode == BN_CLICKED)
        {
            sendCommand(paused_ ? "Resume" : "Pause");
        }
        break;
    case ControlId::EffectsEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch("effects.enabled", isChecked(effectsEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::EffectsMode:
        if (notificationCode == CBN_SELCHANGE)
        {
            const LRESULT selected = SendMessageW(
                effectsMode_,
                CB_GETCURSEL,
                0U,
                0U);
            if (selected == 0)
            {
                applyPatch("performance.effectsMode", "\"full\"");
            }
            else if (selected == 1)
            {
                applyPatch("performance.effectsMode", "\"core\"");
            }
            else
            {
                setError(L"未知的性能模式选择。");
            }
        }
        break;
    case ControlId::ClickEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch("effects.clickEnabled", isChecked(clickEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::TrailEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch("effects.trailEnabled", isChecked(trailEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::TrailAlwaysOn:
        if (notificationCode == BN_CLICKED)
        {
            // The persisted field keeps its historical pressed-only wording;
            // expose the user-facing switch as the positive inverse.
            applyPatch(
                "input.trailOnlyWhilePressed",
                isChecked(trailAlwaysOn_) ? "false" : "true");
        }
        break;
    case ControlId::LeftClickEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "input.leftClick",
                isChecked(leftClickEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::RightClickEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "input.rightClick",
                isChecked(rightClickEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::MiddleClickEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "input.middleClick",
                isChecked(middleClickEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::BloomQuality:
        if (notificationCode == CBN_SELCHANGE)
        {
            switch (SendMessageW(bloomQuality_, CB_GETCURSEL, 0U, 0))
            {
            case 0:
                applyPatch("effects.bloomQuality", "\"low\"");
                break;
            case 1:
                applyPatch("effects.bloomQuality", "\"medium\"");
                break;
            case 2:
                applyPatch("effects.bloomQuality", "\"high\"");
                break;
            case 3:
                applyPatch("effects.bloomQuality", "\"ultra\"");
                break;
            case 4:
                // Custom is a derived state selected by the continuous slider.
                // Selecting it cannot invent a missing diffusion value.
                break;
            default:
                setError(L"未知的 Bloom 质量选择。");
                break;
            }
        }
        break;
    case ControlId::BackgroundMode:
        if (notificationCode == CBN_SELCHANGE)
        {
            switch (SendMessageW(backgroundMode_, CB_GETCURSEL, 0U, 0))
            {
            case 0:
                applyPatch("background.mode", "\"background-aware\"");
                break;
            case 1:
            {
                const bafx::windows::RecordingCompatibleAvailability availability =
                    bafx::windows::queryRecordingCompatibleAvailability();
                if (!availability.supported)
                {
                    const bool queryFailed =
                        !availability.versionQuerySucceeded;
                    bafx::windows::appendRecordingCompatibleControlCenterDiagnostic(
                        availability,
                        queryFailed
                            ? "recording-compatible-test: version-query-failed"
                            : "recording-compatible-test: unsupported-build",
                        "recording-compatible",
                        bafx::config::toString(config_.background.mode),
                        bafx::windows::recordingCompatibleAvailabilityReasonName(
                            availability.reason),
                        generation_);
                    const int previousIndex = renderModeIndex(
                        config_.background.mode);
                    static_cast<void>(SendMessageW(
                        backgroundMode_,
                        CB_SETCURSEL,
                        previousIndex < 0 ? 0 : previousIndex,
                        0));
                    if (!availability.versionQuerySucceeded)
                    {
                        MessageBoxW(
                            window_,
                            L"录屏兼容测试模式仅支持 Windows 11 26H1 及以后（OS build 28000 或更高）。\r\n当前系统版本无法确认，设置未更改。",
                            L"录屏兼容测试模式",
                            MB_OK | MB_ICONWARNING);
                    }
                    else
                    {
                        const std::wstring detectedVersion = utf8ToWide(
                            bafx::windows::recordingCompatibleVersionString(
                                availability));
                        const std::wstring message =
                            L"录屏兼容测试模式仅支持 Windows 11 26H1 及以后（OS build 28000 或更高）。\r\n当前系统为 "
                            + detectedVersion
                            + L"，设置未更改。";
                        MessageBoxW(
                            window_,
                            message.c_str(),
                            L"录屏兼容测试模式",
                            MB_OK | MB_ICONWARNING);
                    }
                    break;
                }
                bafx::windows::appendRecordingCompatibleControlCenterDiagnostic(
                    availability,
                    "recording-compatible-test: selected",
                    "recording-compatible",
                    bafx::config::toString(config_.background.mode),
                    "available",
                    generation_);
                applyPatch("background.mode", "\"recording-compatible\"");
            }
                break;
            case 2:
                applyPatch("background.mode", "\"light-background\"");
                break;
            default:
                setError(L"未知的背景模式选择。");
                break;
            }
        }
        break;
    case ControlId::CursorExcluded:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "background.cursorExcluded",
                isChecked(cursorExcluded_) ? "true" : "false");
        }
        break;
    case ControlId::AllowSystemBorder:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "background.allowSystemBorder",
                isChecked(allowSystemBorder_) ? "true" : "false");
        }
        break;
    case ControlId::IdleOptimization:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "performance.idleOptimization",
                isChecked(idleOptimization_) ? "true" : "false");
        }
        break;
    case ControlId::StartWithWindows:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "system.startWithWindows",
                isChecked(startWithWindows_) ? "true" : "false");
        }
        break;
    case ControlId::StartMinimized:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "system.startMinimized",
                isChecked(startMinimized_) ? "true" : "false");
        }
        break;
    case ControlId::CloseToTray:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "system.closeToTray",
                isChecked(closeToTray_) ? "true" : "false");
        }
        break;
#if defined(BAFX_ENABLE_SPOUT2)
    case ControlId::Spout2Enabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "system.spout2Enabled",
                isChecked(spout2Enabled_) ? "true" : "false");
        }
        break;
    case ControlId::RefreshObsSpoutPlugin:
        if (notificationCode == BN_CLICKED)
        {
            refreshObsPluginStatus();
        }
        break;
    case ControlId::OpenObsSpoutPluginPage:
        if (notificationCode == BN_CLICKED)
        {
            openObsPluginPage();
        }
        break;
#endif
    case ControlId::DisplaySelector:
        if (notificationCode == CBN_SELCHANGE)
        {
            updateDisplayPolicyControls();
            updateDisplayDetails();
        }
        break;
    case ControlId::HdrEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "display.hdrEnabled",
                isChecked(hdrEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::FramePacing:
        if (notificationCode == CBN_SELCHANGE)
        {
            switch (SendMessageW(framePacing_, CB_GETCURSEL, 0U, 0))
            {
            case 0:
                applyPatch("performance.framePacing", "\"match-display\"");
                break;
            case 1:
                applyPatch("performance.framePacing", "\"60\"");
                break;
            case 2:
                applyPatch("performance.framePacing", "\"120\"");
                break;
            case 3:
                applyPatch("performance.framePacing", "\"144\"");
                break;
            default:
                setError(L"未知的帧率策略选择。");
                break;
            }
        }
        break;
    case ControlId::DisplayIndependent:
        if (notificationCode == BN_CLICKED)
        {
            if (isChecked(displayIndependent_))
            {
                setSelectedDisplayOverride();
            }
            else
            {
                removeSelectedDisplayOverride();
            }
        }
        break;
    case ControlId::DisplayEffectsEnabled:
    case ControlId::DisplayHdrEnabled:
        if (notificationCode == BN_CLICKED)
        {
            setSelectedDisplayOverride();
        }
        break;
    case ControlId::DisplayFramePacing:
        if (notificationCode == CBN_SELCHANGE)
        {
            setSelectedDisplayOverride();
        }
        break;
    case ControlId::Refresh:
        if (notificationCode == BN_CLICKED)
        {
            static_cast<void>(refreshFromHost());
#if defined(BAFX_ENABLE_SPOUT2)
            refreshObsPluginStatus();
#endif
        }
        break;
    case ControlId::HostLifecycle:
        if (notificationCode == BN_CLICKED)
        {
            if (hostRunning_ || hostStartPending_ || hostMutexPresent())
            {
                stopHost();
            }
            else
            {
                startHostFromBundle();
            }
        }
        break;
    case ControlId::ClearLogs:
        if (notificationCode == BN_CLICKED)
        {
            clearDiagnosticLogs();
        }
        break;
    case ControlId::ResetDefaults:
        if (notificationCode == BN_CLICKED)
        {
            resetDefaults();
        }
        break;
    case ControlId::GlobalScale:
    case ControlId::TrailLength:
    case ControlId::TrailWidth:
    case ControlId::InputSamplingRate:
    case ControlId::BloomIntensity:
    case ControlId::Opacity:
    case ControlId::ClickTimeScale:
    case ControlId::TrailTimeScale:
    case ControlId::TrailLifetimeMs:
    case ControlId::BloomDiffusion:
    case ControlId::BloomThreshold:
    case ControlId::BloomSoftKnee:
    case ControlId::BloomClamp:
    case ControlId::DiskRadius:
    case ControlId::DiskLifetimeMs:
    case ControlId::RingsHdrIntensity:
    case ControlId::RingsCount:
    case ControlId::RingsLifetimeMs:
    case ControlId::RingsRadiusMin:
    case ControlId::RingsRadiusMax:
    case ControlId::RingsAngularVelocityMultiplier:
    case ControlId::RingsRotationDirection:
    case ControlId::ShardsHdrIntensity:
    case ControlId::ShardsClickCount:
    case ControlId::ShardsClickLifetimeMinMs:
    case ControlId::ShardsClickLifetimeMaxMs:
    case ControlId::ShardsClickRadius:
    case ControlId::ShardsClickSpeedMin:
    case ControlId::ShardsClickSpeedMax:
    case ControlId::ShardsSizeMin:
    case ControlId::ShardsSizeMax:
    case ControlId::TrailOpacity:
        break;
    case ControlId::ThemeColorEdit:
        if (notificationCode == EN_KILLFOCUS
            || notificationCode == themeColorReturnNotification)
        {
            commitThemeColor();
        }
        break;
    case ControlId::ThemeColorChoose:
        if (notificationCode == BN_CLICKED)
        {
            chooseThemeColor();
        }
        break;
    }
}

void ControlCenterWindow::onSliderChanged(const HWND trackbar)
{
    if (updatingControls_ || trackbar == nullptr)
    {
        return;
    }

    const std::array sliders{
        &globalScale_,
        &trailLength_,
        &trailWidth_,
        &inputSamplingRate_,
        &bloomIntensity_,
        &opacity_,
        &clickTimeScale_,
        &trailTimeScale_,
        &trailLifetimeMs_,
        &bloomDiffusion_,
        &bloomThreshold_,
        &bloomSoftKnee_,
        &bloomClamp_,
        &diskRadius_,
        &diskLifetimeMs_,
        &ringsHdrIntensity_,
        &ringsCount_,
        &ringsLifetimeMs_,
        &ringsRadiusMin_,
        &ringsRadiusMax_,
        &ringsAngularVelocityMultiplier_,
        &ringsRotationDirection_,
        &shardsHdrIntensity_,
        &shardsClickCount_,
        &shardsClickLifetimeMinMs_,
        &shardsClickLifetimeMaxMs_,
        &shardsClickRadius_,
        &shardsClickSpeedMin_,
        &shardsClickSpeedMax_,
        &shardsSizeMin_,
        &shardsSizeMax_,
        &trailOpacity_};
    for (SliderControl* const slider : sliders)
    {
        if (slider->trackbar == trackbar)
        {
            updateSliderValueText(*slider);
            queueNumberPatch(*slider);
            return;
        }
    }
}

void ControlCenterWindow::commitThemeColor()
{
    if (updatingControls_ || themeColorEdit_ == nullptr)
    {
        return;
    }
    if (!connected_)
    {
        SetWindowTextW(
            themeColorEdit_,
            utf8ToWide(config_.effects.themeColor).c_str());
        setInfo(L"Host 未连接", L"请先启动 Host，然后设置主题色。");
        return;
    }

    const int length = GetWindowTextLengthW(themeColorEdit_);
    if (length < 0)
    {
        return;
    }
    std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
    const int copied = GetWindowTextW(
        themeColorEdit_,
        text.data(),
        static_cast<int>(text.size()));
    if (copied < 0)
    {
        return;
    }
    text.resize(static_cast<std::size_t>(copied));
    const std::string value = wideToUtf8(text);
    const std::string valueJson = std::string("\"") + value + "\"";
    applyPatchRequest(fxPatchRequest(
        generation_,
        "effects.themeColor",
        valueJson));
}

void ControlCenterWindow::chooseThemeColor()
{
    if (!connected_)
    {
        setInfo(L"Host 未连接", L"请先启动 Host，然后设置主题色。");
        return;
    }

    static COLORREF customColors[16]{};
    CHOOSECOLORW chooser{};
    chooser.lStructSize = sizeof(chooser);
    chooser.hwndOwner = window_;
    chooser.rgbResult = themeColorRef(config_.effects.themeColor);
    chooser.lpCustColors = customColors;
    chooser.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&chooser) == FALSE)
    {
        return;
    }

    char value[8]{};
    const int written = std::snprintf(
        value,
        sizeof(value),
        "#%02x%02x%02x",
        static_cast<unsigned int>(GetRValue(chooser.rgbResult)),
        static_cast<unsigned int>(GetGValue(chooser.rgbResult)),
        static_cast<unsigned int>(GetBValue(chooser.rgbResult)));
    if (written != 7)
    {
        return;
    }
    SetWindowTextW(themeColorEdit_, utf8ToWide(value).c_str());
    commitThemeColor();
}

void ControlCenterWindow::queueNumberPatch(const SliderControl& slider)
{
    if (pendingPatch_.has_value() && pendingPatch_->path != slider.path)
    {
        // A quick move to another field must not discard the prior setting.
        commitPendingPatch();
    }
    pendingPatch_ = PendingPatch{slider.path, numberJson(sliderValue(slider))};
    KillTimer(window_, patchTimerId);
    if (SetTimer(window_, patchTimerId, patchDelayMilliseconds, nullptr) == 0U)
    {
        commitPendingPatch();
    }
}

void ControlCenterWindow::commitPendingPatch()
{
    KillTimer(window_, patchTimerId);
    if (!pendingPatch_.has_value())
    {
        return;
    }

    PendingPatch patch = std::move(*pendingPatch_);
    pendingPatch_.reset();
    applyPatch(patch.path, patch.valueJson);
}

void ControlCenterWindow::onTimer(const UINT_PTR timerId)
{
    if (timerId == patchTimerId)
    {
        commitPendingPatch();
        return;
    }
    if (timerId == hostShutdownTimerId)
    {
        if (!hostShutdownPending_)
        {
            KillTimer(window_, hostShutdownTimerId);
            return;
        }

        bool mutexPresent = hostMutexPresent();
        if (hostLifetimeMutex_.get() == nullptr && mutexPresent)
        {
            const HANDLE lifetimeMutex = OpenMutexW(
                SYNCHRONIZE | MUTEX_MODIFY_STATE,
                FALSE,
                bafx::windows::kHostSingleInstanceMutexName);
            if (lifetimeMutex != nullptr)
            {
                hostLifetimeMutex_.reset(lifetimeMutex);
            }
            // Once the named object exists, the launched process has crossed
            // the startup race even if this process cannot observe its handle.
            hostStartPending_ = false;
        }

        if (hostLifetimeMutex_.get() != nullptr)
        {
            const DWORD waitResult = WaitForSingleObject(hostLifetimeMutex_.get(), 0U);
            if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED)
            {
                // Waiting on a mutex grants ownership, including the abandoned
                // case. Release it before dropping our observation handle.
                static_cast<void>(ReleaseMutex(hostLifetimeMutex_.get()));
                finishHostShutdown();
                return;
            }
            if (waitResult == WAIT_FAILED)
            {
                recoverHostShutdown(
                    L"无法继续监视 Host 退出，请重试关闭操作。");
                return;
            }
        }
        else if (!mutexPresent)
        {
            if (!hostStartPending_)
            {
                finishHostShutdown();
                return;
            }
        }

        mutexPresent = hostLifetimeMutex_.get() != nullptr || hostMutexPresent();
        if (mutexPresent && !hostShutdownCommandAcknowledged_)
        {
            const bafx::windows::IpcClientResponse response =
                client_.transact("Shutdown");
            hostShutdownCommandAcknowledged_ = response.succeeded();
        }

        if (hostShutdownDeadlineTicks_ != 0U
            && GetTickCount64() >= hostShutdownDeadlineTicks_)
        {
            if (!mutexPresent)
            {
                finishHostShutdown();
                return;
            }
            // The mutex still prevents duplicate launch. Restore the close
            // button so a failed control service never strands this window.
            recoverHostShutdown(
                L"Host 未在预期时间内退出，可以再次尝试有序关闭。");
        }
        return;
    }
    if (timerId != hostRetryTimerId)
    {
        return;
    }

    if (hostShutdownPending_ || hostRetryAttempts_ == 0U)
    {
        KillTimer(window_, hostRetryTimerId);
        return;
    }
    if (refreshFromHost())
    {
        KillTimer(window_, hostRetryTimerId);
        hostRetryAttempts_ = 0U;
        hostStartPending_ = false;
        updateHostLifecycleButton();
        return;
    }
    if (hostRetryAttempts_ > 0U)
    {
        --hostRetryAttempts_;
    }
    if (hostRetryAttempts_ == 0U)
    {
        KillTimer(window_, hostRetryTimerId);
        if (hostStartPending_)
        {
            hostStartPending_ = false;
            hostRunning_ = hostMutexPresent();
            updateHostLifecycleButton();
            if (hostRunning_)
            {
                setError(L"Host 控制服务启动超时，但进程仍在运行；可点击“关闭 Host”。");
            }
            else
            {
                setError(L"Host 启动超时，请查看支持日志后重试。");
            }
        }
    }
}

bool ControlCenterWindow::refreshFromHost()
{
    const bool mutexPresent = hostMutexPresent();
    if (!mutexPresent)
    {
        hostRunning_ = hostStartPending_;
        setConnected(false);
        if (!hostShutdownPending_ && !hostStartPending_)
        {
            SetWindowTextW(statusText_, L"Host 未运行");
            setInfo(L"Host 未运行", L"点击“启动 Host”开启特效。");
        }
        return false;
    }

    hostRunning_ = true;
    const bafx::windows::IpcClientResponse stateResponse = client_.transact("GetState");
    if (!stateResponse.succeeded())
    {
        setConnected(false);
        if (!hostShutdownPending_ && !hostStartPending_)
        {
            if (hostRunning_)
            {
                SetWindowTextW(statusText_, L"Host 正在运行，控制服务暂不可用");
                setInfo(
                    L"Host 尚未就绪",
                    L"可以刷新状态、等待初始化，或点击“关闭 Host”重试正常退出。");
            }
            else
            {
                SetWindowTextW(statusText_, L"Host 未运行");
                setInfo(L"无法连接 Host", describeResponse(stateResponse));
            }
        }
        return false;
    }

    hostRunning_ = true;
    const bafx::windows::IpcClientResponse configResponse = client_.transact("GetConfig");
    if (!configResponse.succeeded())
    {
        setConnected(false);
        SetWindowTextW(statusText_, L"Host 配置读取失败");
        setError(describeResponse(configResponse));
        return false;
    }

    const HostStateParseResult state = parseHostState(stateResponse.payload);
    if (!state.succeeded())
    {
        setConnected(false);
        SetWindowTextW(statusText_, L"Host 返回了无法识别的状态数据");
        setError(utf8ToWide(state.error));
        return false;
    }

    const bafx::config::ConfigLoadResult config = bafx::config::parseJson(
        configResponse.payload);
    if (!config.succeeded())
    {
        setConnected(false);
        SetWindowTextW(statusText_, L"Host 返回了无法识别的配置数据");
        setError(utf8ToWide(config.message));
        return false;
    }

    displayState_ = {};
    displayStateError_.clear();
    const bafx::windows::IpcClientResponse displayResponse =
        client_.transact("GetDisplayState");
    if (!displayResponse.succeeded())
    {
        displayStateError_ = L"逐屏运行状态读取失败："
            + describeResponse(displayResponse);
    }
    else
    {
        DisplayStateParseResult display = parseDisplayState(
            displayResponse.payload);
        if (display.succeeded())
        {
            displayState_ = std::move(*display.state);
        }
        else
        {
            displayStateError_ = L"逐屏运行状态格式无效："
                + utf8ToWide(display.error);
        }
    }

    updateControls(*state.state, config.config);
    return true;
}

void ControlCenterWindow::updateControls(
    const HostState& state,
    const bafx::config::Config& config)
{
    generation_ = state.generation;
    paused_ = state.paused;
    config_ = config;
    updatingControls_ = true;

    if (config.system.closeToTray)
    {
        static_cast<void>(ensureTrayIcon());
    }
    else
    {
        removeTrayIcon();
    }

    setChecked(effectsEnabled_, config.effects.enabled);
    static_cast<void>(SendMessageW(
        effectsMode_,
        CB_SETCURSEL,
        effectsModeIndex(config.performance.effectsMode),
        0));
    setChecked(clickEnabled_, config.effects.clickEnabled);
    setChecked(trailEnabled_, config.effects.trailEnabled);
    setChecked(trailAlwaysOn_, !config.input.trailOnlyWhilePressed);
    setChecked(leftClickEnabled_, config.input.leftClick);
    setChecked(rightClickEnabled_, config.input.rightClick);
    setChecked(middleClickEnabled_, config.input.middleClick);
    setSliderValue(globalScale_, config.effects.globalScale);
    setSliderValue(trailLength_, config.effects.trailLength);
    setSliderValue(trailWidth_, config.effects.trailWidth);
    setSliderValue(inputSamplingRate_, config.input.samplingRateHz);
    setSliderValue(bloomIntensity_, config.effects.bloomIntensity);
    setSliderValue(opacity_, config.effects.opacity);
    setSliderValue(clickTimeScale_, config.effects.clickTimeScale);
    setSliderValue(trailTimeScale_, config.effects.trailTimeScale);
    setSliderValue(trailLifetimeMs_, config.effects.trailLifetimeMs);
    setSliderValue(bloomDiffusion_, config.effects.bloomDiffusion);
    setSliderValue(bloomThreshold_, config.effects.bloomThreshold);
    setSliderValue(bloomSoftKnee_, config.effects.bloomSoftKnee);
    setSliderValue(bloomClamp_, config.effects.bloomClamp);
    setSliderValue(diskRadius_, config.effects.diskRadius);
    setSliderValue(diskLifetimeMs_, config.effects.diskLifetimeMs);
    setSliderValue(ringsHdrIntensity_, config.effects.ringsHdrIntensity);
    setSliderValue(ringsCount_, config.effects.ringsCount);
    setSliderValue(ringsLifetimeMs_, config.effects.ringsLifetimeMs);
    setSliderValue(ringsRadiusMin_, config.effects.ringsRadiusMin);
    setSliderValue(ringsRadiusMax_, config.effects.ringsRadiusMax);
    setSliderValue(
        ringsAngularVelocityMultiplier_,
        config.effects.ringsAngularVelocityMultiplier);
    setSliderValue(
        ringsRotationDirection_,
        config.effects.ringsRotationDirection);
    setSliderValue(shardsHdrIntensity_, config.effects.shardsHdrIntensity);
    setSliderValue(shardsClickCount_, config.effects.shardsClickCount);
    setSliderValue(
        shardsClickLifetimeMinMs_,
        config.effects.shardsClickLifetimeMinMs);
    setSliderValue(
        shardsClickLifetimeMaxMs_,
        config.effects.shardsClickLifetimeMaxMs);
    setSliderValue(shardsClickRadius_, config.effects.shardsClickRadius);
    setSliderValue(
        shardsClickSpeedMin_,
        config.effects.shardsClickSpeedMin);
    setSliderValue(
        shardsClickSpeedMax_,
        config.effects.shardsClickSpeedMax);
    setSliderValue(shardsSizeMin_, config.effects.shardsSizeMin);
    setSliderValue(shardsSizeMax_, config.effects.shardsSizeMax);
    setSliderValue(trailOpacity_, config.effects.trailOpacity);
    SetWindowTextW(
        themeColorEdit_,
        utf8ToWide(config.effects.themeColor).c_str());
    InvalidateRect(themeColorPreview_, nullptr, TRUE);
    static_cast<void>(SendMessageW(
        bloomQuality_,
        CB_SETCURSEL,
        qualityIndex(bafx::config::bloomQualityForDiffusion(
            config.effects.bloomDiffusion)),
        0));
    static_cast<void>(SendMessageW(
        backgroundMode_,
        CB_SETCURSEL,
        renderModeIndex(config.background.mode),
        0));
    setChecked(cursorExcluded_, config.background.cursorExcluded);
    setChecked(
        allowSystemBorder_,
        config.background.allowSystemBorder);
    setChecked(idleOptimization_, config.performance.idleOptimization);
    setChecked(startWithWindows_, config.system.startWithWindows);
    setChecked(startMinimized_, config.system.startMinimized);
    setChecked(closeToTray_, config.system.closeToTray);
#if defined(BAFX_ENABLE_SPOUT2)
    setChecked(spout2Enabled_, config.system.spout2Enabled);
    updateSpout2Status(state);
#endif
    updateDisplayControls(config);
    SetWindowTextW(pauseButton_, paused_ ? L"恢复特效" : L"暂停特效");

    updatingControls_ = false;
    hostRunning_ = true;
    setConnected(true);
    const std::wstring captureStatus = utf8ToWide(state.backgroundCapture);
    const std::wstring status = std::wstring(L"Host 已连接 | ")
        + (paused_ ? L"已暂停" : L"运行中")
        + L" | 背景采样：" + captureStatus;
    SetWindowTextW(statusText_, status.c_str());
    if (!hostShutdownPending_)
    {
        clearInfo();
    }
}

#if defined(BAFX_ENABLE_SPOUT2)
void ControlCenterWindow::updateSpout2Status(const HostState& state)
{
    std::wstring text = L"发送者状态："
        + spout2StatusText(state.spout2Status)
        + L" | 名称："
        + utf8ToWide(state.spout2Sender);
    if (!state.spout2Error.empty())
    {
        text += L"\r\n错误：" + utf8ToWide(state.spout2Error);
    }
    else if (state.spout2OutputContract
        != bafx::windows::spout2OutputContract)
    {
        text += L"\r\n输出契约不匹配："
            + utf8ToWide(state.spout2OutputContract);
    }
    else
    {
        text += L"\r\n输出：BGRA8 / sRGB / 预乘 Alpha / 仅特效";
    }
    SetWindowTextW(spout2SenderStatus_, text.c_str());
}

void ControlCenterWindow::refreshObsPluginStatus()
{
    const ObsSpoutPluginProbeResult result = probeObsSpoutPlugin();
    std::wstring text;
    switch (result.state)
    {
    case ObsSpoutPluginState::Missing:
        text = L"OBS 插件状态：未找到 win-spout.dll";
        break;
    case ObsSpoutPluginState::InstalledObsNotRunning:
        text = L"OBS 插件状态：已安装；启动 OBS 后可确认加载";
        break;
    case ObsSpoutPluginState::Loaded:
        text = L"OBS 插件状态：已由 OBS 加载";
        break;
    case ObsSpoutPluginState::InstalledNotLoaded:
        text = L"OBS 插件状态：OBS 已运行，但 win-spout.dll 未加载";
        break;
    case ObsSpoutPluginState::InspectionUnavailable:
        text = L"OBS 插件状态：无法确认 OBS 加载状态（权限受限）";
        break;
    }

    if (!result.pluginVersion.empty())
    {
        text += L" | v" + utf8ToWide(result.pluginVersion);
    }
    if (!result.pluginArchitecture.empty())
    {
        text += L" | " + utf8ToWide(result.pluginArchitecture);
    }
    if (!result.pluginPath.empty())
    {
        text += L"\r\n位置：" + result.pluginPath.native();
    }
    SetWindowTextW(obsSpoutPluginStatus_, text.c_str());
}

void ControlCenterWindow::openObsPluginPage()
{
    const HINSTANCE result = ShellExecuteW(
        window_,
        L"open",
        obsSpoutPluginPage,
        nullptr,
        nullptr,
        SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32)
    {
        setError(L"无法打开 OBS Spout2 官方插件页面。请检查默认浏览器设置。");
    }
}
#endif

void ControlCenterWindow::updateDisplayControls(
    const bafx::config::Config& config)
{
    setChecked(hdrEnabled_, config.display.hdrEnabled);
    static_cast<void>(SendMessageW(
        framePacing_,
        CB_SETCURSEL,
        framePacingIndex(config.performance.framePacing),
        0));

    static_cast<void>(SendMessageW(displaySelector_, CB_RESETCONTENT, 0U, 0));
    if (!displayStateError_.empty()
        || (displayState_.sessions.empty()
            && displayState_.offlineOverrides.empty()))
    {
        updateDisplayPolicyControls();
        updateDisplayDetails();
        return;
    }

    LRESULT selectedIndex = CB_ERR;
    LRESULT primaryIndex = CB_ERR;
    LRESULT coordinatorIndex = CB_ERR;
    for (std::size_t index = 0U;
         index < displayState_.sessions.size();
         ++index)
    {
        const DisplaySessionState& session = displayState_.sessions[index];
        std::wstring label = utf8ToWide(session.device);
        if (!session.monitor.empty())
        {
            label += L" | " + utf8ToWide(session.monitor);
        }
        if (session.primary && session.coordinator)
        {
            label += L"（主显示器，帧协调器）";
        }
        else if (session.primary)
        {
            label += L"（主显示器）";
        }
        else if (session.coordinator)
        {
            label += L"（帧协调器）";
        }

        const LRESULT comboIndex = SendMessageW(
            displaySelector_,
            CB_ADDSTRING,
            0U,
            reinterpret_cast<LPARAM>(label.c_str()));
        if (comboIndex == CB_ERR || comboIndex == CB_ERRSPACE)
        {
            displayStateError_ = L"显示器列表无法分配足够的界面资源。";
            static_cast<void>(SendMessageW(
                displaySelector_,
                CB_RESETCONTENT,
                0U,
                0));
            updateDisplayDetails();
            return;
        }
        static_cast<void>(SendMessageW(
            displaySelector_,
            CB_SETITEMDATA,
            static_cast<WPARAM>(comboIndex),
            static_cast<LPARAM>(index)));

        if (displaySessionIdentity(session) == selectedDisplayIdentity_)
        {
            selectedIndex = comboIndex;
        }
        if (session.primary && primaryIndex == CB_ERR)
        {
            primaryIndex = comboIndex;
        }
        if (session.coordinator && coordinatorIndex == CB_ERR)
        {
            coordinatorIndex = comboIndex;
        }
    }

    for (std::size_t index = 0U;
         index < displayState_.offlineOverrides.size();
         ++index)
    {
        const bafx::config::DisplayOverrideConfig& overrideConfig =
            displayState_.offlineOverrides[index];
        const std::wstring label = L"离线独立设置 | "
            + utf8ToWide(overrideConfig.displayKey);
        const LRESULT comboIndex = SendMessageW(
            displaySelector_,
            CB_ADDSTRING,
            0U,
            reinterpret_cast<LPARAM>(label.c_str()));
        if (comboIndex == CB_ERR || comboIndex == CB_ERRSPACE)
        {
            displayStateError_ = L"显示器列表无法分配足够的界面资源。";
            static_cast<void>(SendMessageW(
                displaySelector_,
                CB_RESETCONTENT,
                0U,
                0));
            updateDisplayDetails();
            return;
        }

        // Keep disconnected policies outside the active-session index range.
        // The item remains removable without pretending it has runtime state.
        static_cast<void>(SendMessageW(
            displaySelector_,
            CB_SETITEMDATA,
            static_cast<WPARAM>(comboIndex),
            static_cast<LPARAM>(offlineDisplayItemBase + index)));
        if (offlineDisplayIdentity(overrideConfig)
            == selectedDisplayIdentity_)
        {
            selectedIndex = comboIndex;
        }
    }

    if (selectedIndex == CB_ERR)
    {
        selectedIndex = primaryIndex != CB_ERR
            ? primaryIndex
            : coordinatorIndex;
    }
    if (selectedIndex == CB_ERR)
    {
        selectedIndex = 0;
    }
    static_cast<void>(SendMessageW(
        displaySelector_,
        CB_SETCURSEL,
        static_cast<WPARAM>(selectedIndex),
        0));
    updateDisplayPolicyControls();
    updateDisplayDetails();
}

void ControlCenterWindow::updateDisplayPolicyControls() noexcept
{
    const bool wasUpdating = updatingControls_;
    updatingControls_ = true;

    const DisplaySessionState* const session = selectedDisplaySession();
    const bafx::config::DisplayOverrideConfig* const offlineOverride =
        selectedOfflineDisplayOverride();
    const std::string* displayKey = nullptr;
    if (session != nullptr && session->displayKey.has_value())
    {
        displayKey = &*session->displayKey;
    }
    else if (offlineOverride != nullptr)
    {
        displayKey = &offlineOverride->displayKey;
    }

    const bafx::config::DisplayOverrideConfig* overrideConfig = nullptr;
    if (offlineOverride != nullptr)
    {
        overrideConfig = offlineOverride;
    }
    else if (displayKey != nullptr)
    {
        overrideConfig = bafx::config::findDisplayOverride(
            config_.display,
            *displayKey);
    }

    const bool independent = overrideConfig != nullptr;
    bafx::config::ResolvedDisplayPolicy policy = displayKey != nullptr
        ? bafx::config::resolveDisplayPolicy(config_, *displayKey)
        : bafx::config::resolveDisplayPolicy(config_, {});
    if (offlineOverride != nullptr)
    {
        // Offline entries come from the Host's authoritative schema-2 list.
        // Do not replace that runtime fact with a separately fetched config.
        policy.enabled = offlineOverride->enabled;
        policy.hdrEnabled = offlineOverride->hdrEnabled;
        policy.framePacing = offlineOverride->framePacing;
        policy.overridden = true;
    }
    setChecked(displayIndependent_, independent);
    setChecked(displayEffectsEnabled_, policy.enabled);
    setChecked(displayHdrEnabled_, policy.hdrEnabled);
    static_cast<void>(SendMessageW(
        displayFramePacing_,
        CB_SETCURSEL,
        framePacingIndex(policy.framePacing),
        0));

    const bool canWrite = connected_ && displayKey != nullptr;
    EnableWindow(displayIndependent_, canWrite ? TRUE : FALSE);
    const BOOL policyEnabled = canWrite
            && independent
            && offlineOverride == nullptr
        ? TRUE
        : FALSE;
    EnableWindow(displayEffectsEnabled_, policyEnabled);
    EnableWindow(displayHdrEnabled_, policyEnabled);
    EnableWindow(displayFramePacingLabel_, policyEnabled);
    EnableWindow(displayFramePacing_, policyEnabled);

    updatingControls_ = wasUpdating;
}

void ControlCenterWindow::updateDisplayDetails()
{
    if (!displayStateError_.empty())
    {
        SetWindowTextW(displaySummaryText_, L"逐屏运行状态不可用");
        SetWindowTextW(displayDetailsText_, displayStateError_.c_str());
        return;
    }

    std::wostringstream summary;
    summary << L"拓扑 " << topologyStateText(displayState_.topologyStatus)
            << L" | 会话 " << displayState_.sessions.size()
            << L" | 离线 " << displayState_.offlineOverrides.size()
            << L"\r\n代次 R/C/A " << displayState_.runtimeGeneration
            << L" / " << displayState_.configGeneration
            << L" / " << displayState_.appliedConfigGeneration;
    SetWindowTextW(displaySummaryText_, summary.str().c_str());

    const DisplaySessionState* const selectedSession = selectedDisplaySession();
    const bafx::config::DisplayOverrideConfig* const offlineOverride =
        selectedOfflineDisplayOverride();
    if (selectedSession == nullptr && offlineOverride == nullptr)
    {
        SetWindowTextW(
            displayDetailsText_,
            displayState_.sessions.empty()
                    && displayState_.offlineOverrides.empty()
                ? L"Host 当前没有可显示的活动会话或离线独立设置。"
                : L"请选择一个显示器或离线独立设置查看状态。");
        return;
    }

    if (offlineOverride != nullptr)
    {
        selectedDisplayIdentity_ = offlineDisplayIdentity(*offlineOverride);
        std::wostringstream details;
        details << L"全局拓扑："
                << topologyStateText(displayState_.topologyStatus)
                << L" | 错误 "
                << hresultText(static_cast<HRESULT>(
                    displayState_.topologyError))
                << L" | 离线列表：权威"
                << L"\r\n离线独立设置\r\n显示标识："
                << utf8ToWide(offlineOverride->displayKey)
                << L"\r\n特效："
                << (offlineOverride->enabled ? L"开启" : L"关闭")
                << L" | HDR 请求："
                << (offlineOverride->hdrEnabled ? L"开启" : L"关闭")
                << L" | "
                << framePacingText(offlineOverride->framePacing)
                << L"\r\n此显示器当前未连接，因此没有可报告的 HDR、颜色、"
                   L"刷新率或输出运行状态。取消“使用独立设置”可删除此策略。";
        SetWindowTextW(displayDetailsText_, details.str().c_str());
        return;
    }

    const DisplaySessionState& session = *selectedSession;
    selectedDisplayIdentity_ = displaySessionIdentity(session);

    const std::int64_t width = static_cast<std::int64_t>(session.right)
        - static_cast<std::int64_t>(session.left);
    const std::int64_t height = static_cast<std::int64_t>(session.bottom)
        - static_cast<std::int64_t>(session.top);
    const std::wstring role = session.primary
        ? (session.coordinator ? L"主显示器、帧协调器" : L"主显示器")
        : (session.coordinator ? L"帧协调器" : L"扩展显示器");
    const std::wstring captureState = session.backgroundCaptureActive
        ? L"活动"
        : L"未活动";
    const std::wstring restartState = session.backgroundCaptureRestartAllowed
        ? L"允许"
        : L"不允许";
    const bafx::config::ResolvedDisplayPolicy policy =
        session.displayKey.has_value()
        ? bafx::config::resolveDisplayPolicy(config_, *session.displayKey)
        : bafx::config::resolveDisplayPolicy(config_, {});
    const std::wstring policySource = policy.overridden
        ? L"独立设置"
        : (session.displayKey.has_value() ? L"全局继承" : L"全局继承（无稳定标识）");
    const std::wstring sourceId = session.sourceId.has_value()
        ? std::to_wstring(*session.sourceId)
        : L"未知";

    std::wstring faultState;
    if (!session.renderFaulted && !session.outputContractFaulted)
    {
        faultState = L"无";
    }
    else
    {
        if (session.renderFaulted)
        {
            faultState = L"渲染故障";
        }
        if (session.outputContractFaulted)
        {
            if (!faultState.empty())
            {
                faultState += L"、";
            }
            faultState += L"输出合同故障";
        }
    }

    std::wostringstream details;
    details << L"全局拓扑："
            << topologyStateText(displayState_.topologyStatus)
            << L" | 错误 "
            << hresultText(static_cast<HRESULT>(
                displayState_.topologyError))
            << L" | 离线列表 "
            << (displayState_.offlineOverridesAuthoritative
                ? L"权威"
                : L"待拓扑恢复")
            << L"\r\n设备：" << utf8ToWide(session.device)
            << L" | " << utf8ToWide(session.monitor)
            << L"\r\n角色：" << role
            << L" | 显示标识："
            << (session.displayKey.has_value()
                ? utf8ToWide(*session.displayKey)
                : L"未知")
            << L"\r\n桌面：" << width << L" x " << height
            << L" @ (" << session.left << L", " << session.top << L")"
            << L" | DPI：" << session.windowDpi
            << L" / " << session.targetDpiX << L" x " << session.targetDpiY
            << L"\r\n来源身份：Adapter "
            << (session.sourceAdapterResolved ? L"已解析" : L"未解析")
            << L" | Source "
            << (session.sourceIdentityResolved ? L"已解析" : L"未解析")
            << L" | ID " << sourceId
            << L" | 物理目标 " << session.physicalTargetCount
            << L"\r\nGPU：" << utf8ToWide(session.adapter)
            << L" | 驱动：" << driverStateText(session.driver)
            << L"\r\n配置请求：" << policySource
            << L" | 特效 " << (policy.enabled ? L"开启" : L"关闭")
            << L" | HDR " << (policy.hdrEnabled ? L"开启" : L"关闭")
            << L" | " << framePacingText(policy.framePacing)
            << L"\r\nHost 已应用：特效 "
            << (session.effectsEnabled ? L"开启" : L"关闭")
            << L" | HDR " << (session.hdrEnabled ? L"开启" : L"关闭")
            << L" | " << framePacingText(session.framePacing)
            << L"\r\n刷新率：显示 " << refreshRateText(session.displayRefresh)
            << L" | 捕获 " << refreshRateText(session.captureRefresh)
            << L" | 捕获策略 "
            << captureCadenceText(session.captureCadenceStatus)
            << L"\r\n刷新率策略：producer "
            << refreshRateText(session.producerPolicyRefresh)
            << L" | freshness "
            << refreshRateText(session.freshnessPolicyRefresh)
            << L" / " << session.freshnessPeriodUs << L" us"
            << L"\r\nWGC producer："
            << producerCadenceText(session.producerCadenceStatus)
            << L" | 请求 " << session.producerRequestedPeriodUs << L" us"
            << L" | 实际 " << session.producerAppliedPeriodUs << L" us"
            << L" | 结果 "
            << hresultText(static_cast<HRESULT>(session.producerResult))
            << L"\r\nCadence 回退："
            << cadenceFallbackText(session.cadenceFallbackReason)
            << L"\r\n输出：请求 " << outputStateText(session.requestedOutput)
            << L" | 解析 " << outputStateText(session.resolvedOutput)
            << L" | 实际 " << outputStateText(session.actualOutput)
            << L"\r\n输出映射：解析 "
            << outputMappingText(session.resolvedOutputMapping)
            << L" | 实际 "
            << outputMappingText(session.actualOutputMapping)
            << L"\r\n输出回退：" << outputFallbackText(session.outputFallback)
            << L" | 结果 "
            << hresultText(static_cast<HRESULT>(session.outputFallbackResult))
            << L" | 策略满足 "
            << (session.outputPolicySatisfied ? L"是" : L"否")
            << L"\r\n系统实际色彩：" << colorStateText(session.colorMode)
            << L" | HDR "
            << optionalBooleanText(session.hdrSupported, L"支持", L"不支持")
            << L" / "
            << optionalBooleanText(session.hdrActive, L"已激活", L"未激活")
            << L" | 用户开关 "
            << optionalBooleanText(
                session.hdrUserEnabled,
                L"开启",
                L"关闭")
            << L" | 策略限制 "
            << optionalBooleanText(
                session.advancedColorLimitedByPolicy,
                L"是",
                L"否")
            << L"\r\n颜色监视："
            << colorMonitorStateText(session.colorMonitorStatus)
            << L" | HRESULT "
            << hresultText(static_cast<HRESULT>(session.colorMonitorHresult))
            << L" | 监视代次 " << session.colorMonitorGeneration
            << L" | 查询代次 " << session.colorQueryGeneration
            << L"\r\n颜色合同："
            << colorSnapshotStateText(session.colorSnapshotDisposition)
            << L" | 完整 "
            << (session.colorSnapshotComplete ? L"是" : L"否")
            << L" | 剩余重试 " << session.colorRefreshRetriesRemaining
            << L"\r\nAdvanced Color 查询："
            << optionalHresultText(session.advancedColorQueryResult)
            << L"\r\nSDR white level："
            << optionalNitsText(session.sdrWhiteLevelNits)
            << L" | 查询 "
            << optionalHresultText(session.sdrWhiteLevelQueryResult)
            << L" | 保留 "
            << optionalBooleanText(
                session.sdrWhiteLevelRetained,
                L"是",
                L"否")
            << L" | 物理目标一致 "
            << optionalBooleanText(
                session.sdrWhiteLevelConsistent,
                L"是",
                L"否")
            << L"\r\n背景采样：" << captureState
            << L" | 重启：" << restartState
            << L"\r\n运行故障：" << faultState;

    for (std::size_t index = 0U;
         index < session.physicalCadence.size();
         ++index)
    {
        const DisplayPhysicalCadenceState& physical =
            session.physicalCadence[index];
        details << L"\r\n物理目标 " << index + 1U
                << L"：虚拟 " << refreshRateText(physical.virtualRefresh)
                << L" | 物理 " << refreshRateText(physical.physicalRefresh)
                << L" | 捕获 " << refreshRateText(physical.captureRefresh)
                << L" | DRR boost "
                << (physical.drrBoosted ? L"是" : L"否")
                << L" | 可用 " << (physical.available ? L"是" : L"否");
    }
    if (!session.backgroundCaptureFailure.empty())
    {
        details << L"\r\n捕获错误："
                << utf8ToWide(session.backgroundCaptureFailure);
    }
    SetWindowTextW(displayDetailsText_, details.str().c_str());
    static_cast<void>(SendMessageW(displayDetailsText_, EM_SETSEL, 0U, 0));
    static_cast<void>(SendMessageW(displayDetailsText_, EM_SCROLLCARET, 0U, 0));
}

void ControlCenterWindow::setSelectedDisplayOverride()
{
    const DisplaySessionState* const session = selectedDisplaySession();
    if (session == nullptr || !session->displayKey.has_value())
    {
        updateDisplayPolicyControls();
        setInfo(
            L"无法保存独立设置",
            L"Host 未提供此显示器的稳定标识；请刷新状态后重试。");
        return;
    }

    const std::optional<bafx::config::FramePacing> framePacing =
        selectedFramePacing(displayFramePacing_);
    if (!framePacing.has_value())
    {
        updateDisplayPolicyControls();
        setError(L"未知的逐显示器帧率策略选择。");
        return;
    }

    bafx::config::DisplayOverrideConfig overrideConfig{};
    overrideConfig.displayKey = *session->displayKey;
    overrideConfig.enabled = isChecked(displayEffectsEnabled_);
    overrideConfig.hdrEnabled = isChecked(displayHdrEnabled_);
    overrideConfig.framePacing = *framePacing;
    applyDisplayPolicyCommand(setDisplayOverrideRequest(
        generation_,
        overrideConfig));
}

void ControlCenterWindow::removeSelectedDisplayOverride()
{
    const DisplaySessionState* const session = selectedDisplaySession();
    const bafx::config::DisplayOverrideConfig* const offlineOverride =
        selectedOfflineDisplayOverride();
    const std::string* displayKey = nullptr;
    bool overrideExists = false;
    if (offlineOverride != nullptr)
    {
        displayKey = &offlineOverride->displayKey;
        overrideExists = true;
    }
    else if (session != nullptr && session->displayKey.has_value())
    {
        displayKey = &*session->displayKey;
        overrideExists = bafx::config::findDisplayOverride(
            config_.display,
            *displayKey) != nullptr;
    }

    if (displayKey == nullptr)
    {
        updateDisplayPolicyControls();
        setInfo(
            L"无法恢复全局设置",
            L"Host 未提供此显示器的稳定标识；请刷新状态后重试。");
        return;
    }
    if (!overrideExists)
    {
        updateDisplayPolicyControls();
        updateDisplayDetails();
        return;
    }

    applyDisplayPolicyCommand(removeDisplayOverrideRequest(
        generation_,
        *displayKey));
}

void ControlCenterWindow::applyDisplayPolicyCommand(std::string command)
{
    if (!connected_)
    {
        updateDisplayPolicyControls();
        setInfo(L"Host 未连接", L"请先启动 Host，然后刷新状态。");
        return;
    }

    const bafx::windows::IpcClientResponse response = client_.transact(command);
    if (response.succeeded())
    {
        static_cast<void>(refreshFromHost());
        return;
    }
    if (response.errorCode == "generation_conflict")
    {
        static_cast<void>(refreshFromHost());
        setInfo(L"配置已变化", L"已刷新 Host 的最新设置，请再次调整。");
        return;
    }

    const std::wstring error = describeResponse(response);
    static_cast<void>(refreshFromHost());
    setError(error);
}

void ControlCenterWindow::applyPatch(
    const std::string_view path,
    const std::string_view valueJson)
{
    applyPatchRequest(patchRequest(generation_, path, valueJson));
}

void ControlCenterWindow::applyPatchRequest(std::string command)
{
    if (!connected_)
    {
        setInfo(L"Host 未连接", L"请先启动 Host，然后刷新状态。");
        return;
    }

    const bafx::windows::IpcClientResponse response = client_.transact(command);
    if (response.succeeded())
    {
        static_cast<void>(refreshFromHost());
        return;
    }
    if (response.errorCode == "generation_conflict")
    {
        static_cast<void>(refreshFromHost());
        setInfo(L"配置已变化", L"已刷新 Host 的最新设置，请再次调整。");
        return;
    }
    const std::wstring error = describeResponse(response);
    // A rejected write left the Host unchanged. Restore every optimistic
    // control value before presenting the failure so the UI remains truthful.
    static_cast<void>(refreshFromHost());
    setError(error);
}

void ControlCenterWindow::sendCommand(const std::string_view command)
{
    const bafx::windows::IpcClientResponse response = client_.transact(command);
    if (!response.succeeded())
    {
        setError(describeResponse(response));
        return;
    }
    static_cast<void>(refreshFromHost());
}

void ControlCenterWindow::clearDiagnosticLogs()
{
    if (!connected_)
    {
        setInfo(L"Host 未连接", L"请先启动 Host，然后清理诊断日志。\r\n"
            L"日志文件位于 Host 的 data 目录。回退配置不会受影响。");
        return;
    }

    const int choice = MessageBoxW(
        window_,
        L"确定删除当前诊断日志和轮转备份吗？\r\n\r\n"
        L"这不会停止 Host 或修改配置；删除后会重新写入一条清理结果日志。",
        L"清理诊断日志",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (choice != IDYES)
    {
        return;
    }

    const bafx::windows::IpcClientResponse response = client_.transact(
        "ClearLogs");
    if (!response.succeeded())
    {
        setError(describeResponse(response));
        return;
    }

    // The Host owns the JSON response contract. Parse only the three unsigned
    // counters needed for the user-facing summary and reject malformed data.
    const auto readCounter = [&response](const std::string_view name)
        -> std::optional<std::uint64_t>
    {
        const std::string needle = "\"" + std::string(name) + "\":";
        const std::size_t start = response.payload.find(needle);
        if (start == std::string::npos)
        {
            return std::nullopt;
        }
        const std::size_t valueStart = start + needle.size();
        std::size_t valueEnd = valueStart;
        while (valueEnd < response.payload.size()
            && response.payload[valueEnd] >= '0'
            && response.payload[valueEnd] <= '9')
        {
            ++valueEnd;
        }
        if (valueEnd == valueStart
            || (valueEnd < response.payload.size()
                && response.payload[valueEnd] != ','
                && response.payload[valueEnd] != '}'))
        {
            return std::nullopt;
        }
        std::uint64_t value = 0U;
        const auto parsed = std::from_chars(
            response.payload.data() + valueStart,
            response.payload.data() + valueEnd,
            value);
        if (parsed.ec != std::errc{} || parsed.ptr != response.payload.data() + valueEnd)
        {
            return std::nullopt;
        }
        return value;
    };

    const std::optional<std::uint64_t> removedFiles = readCounter(
        "removedFiles");
    const std::optional<std::uint64_t> removedBytes = readCounter(
        "removedBytes");
    const std::optional<std::uint64_t> failedFiles = readCounter(
        "failedFiles");
    if (!removedFiles.has_value()
        || !removedBytes.has_value()
        || !failedFiles.has_value())
    {
        setInfo(
            L"日志已清理，但统计响应无法解析",
            L"Host 已完成清理。请提交新的诊断日志以便检查清理结果。");
        return;
    }

    const std::wstring summary =
        L"删除文件：" + std::to_wstring(*removedFiles)
        + L"；释放空间：" + std::to_wstring(*removedBytes)
        + L" 字节；失败文件：" + std::to_wstring(*failedFiles);
    setInfo(
        *failedFiles == 0U ? L"诊断日志已清理" : L"诊断日志部分清理",
        summary);
}

void ControlCenterWindow::resetDefaults()
{
    if (!connected_)
    {
        setInfo(L"Host 未连接", L"请先启动 Host，然后刷新状态。");
        return;
    }

    const int choice = MessageBoxW(
        window_,
        L"确定将特效、输入和背景设置全部恢复为默认值吗？\r\n\r\n"
        L"当前暂停或运行状态不会改变。",
        L"重置默认设置",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (choice != IDYES)
    {
        return;
    }

    // A delayed slider patch must not overwrite the defaults after reset.
    KillTimer(window_, patchTimerId);
    pendingPatch_.reset();
    sendCommand(defaultConfigRequest());
}

void ControlCenterWindow::startHostFromBundle()
{
    if (hostShutdownPending_)
    {
        return;
    }
    if (hostStartPending_)
    {
        return;
    }
    hostRunning_ = hostMutexPresent();
    if (hostRunning_)
    {
        updateHostLifecycleButton();
        scheduleHostRefreshRetry();
        setInfo(
            L"Host 已在运行",
            L"控制服务尚未连接，正在继续刷新；也可以点击“关闭 Host”重试退出。");
        return;
    }
    std::filesystem::path hostPath;
    try
    {
        hostPath = executableDirectory() / L"ba-click-fx-desktop.exe";
    }
    catch (const std::exception& error)
    {
        setError(utf8ToWide(error.what()));
        return;
    }
    if (!std::filesystem::is_regular_file(hostPath))
    {
        setInfo(
            L"未找到 Host",
            L"请将 Control Center 与 ba-click-fx-desktop.exe 放在同一目录。");
        return;
    }

    const PackageActivationIdentityResult packageIdentity =
        readPackageActivationState(hostPath.parent_path());
    if (packageIdentity.installStatePresent)
    {
        if (!packageIdentity.succeeded())
        {
            setInfo(
                L"安装状态无效",
                packageIdentity.error
                    + L" 请重新运行安装器进行修复。");
            return;
        }

        const PackageActivationResult activation = activatePackagedHost(
            packageIdentity.identity->appUserModelId);
        if (!activation.succeeded())
        {
            setError(
                L"通过 Package Activation 启动 Host 失败，HRESULT："
                + hresultText(activation.result));
            return;
        }
    }
    else
    {
        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startupInfo.wShowWindow = SW_SHOWNOACTIVATE;
        PROCESS_INFORMATION processInfo{};
        std::wstring commandLine = L"\"" + hostPath.wstring() + L"\"";
        const std::wstring workingDirectory = hostPath.parent_path().wstring();
        if (CreateProcessW(
                hostPath.c_str(),
                commandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_DEFAULT_ERROR_MODE,
                nullptr,
                workingDirectory.c_str(),
                &startupInfo,
                &processInfo) == FALSE)
        {
            const DWORD error = GetLastError();
            setError(L"启动 portable Host 失败，Win32 错误码："
                + std::to_wstring(error));
            return;
        }
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
    }

    hostRunning_ = true;
    scheduleHostRefreshRetry(true);
    setInfo(L"正在启动 Host", L"Host 初始化完成后会自动刷新。");
}

void ControlCenterWindow::stopHost()
{
    if (hostShutdownPending_)
    {
        return;
    }

    const bool mutexPresent = hostMutexPresent();
    if (!mutexPresent && !hostStartPending_)
    {
        hostRunning_ = false;
        setConnected(false);
        SetWindowTextW(statusText_, L"Host 未运行");
        setInfo(L"Host 已关闭", L"未发现需要关闭的 Host 进程。");
        return;
    }

    if (connected_ && pendingPatch_.has_value())
    {
        commitPendingPatch();
    }
    if (mutexPresent)
    {
        const HANDLE lifetimeMutex = OpenMutexW(
            SYNCHRONIZE | MUTEX_MODIFY_STATE,
            FALSE,
            bafx::windows::kHostSingleInstanceMutexName);
        hostLifetimeMutex_.reset(lifetimeMutex);
        hostStartPending_ = false;
    }

    // Host owns renderer teardown. The control center requests orderly exit
    // over IPC and observes the exact single-instance mutex; it never searches
    // by executable name or terminates an unrelated process.
    hostShutdownCommandAcknowledged_ = false;
    if (mutexPresent)
    {
        const bafx::windows::IpcClientResponse response =
            client_.transact("Shutdown");
        hostShutdownCommandAcknowledged_ = response.succeeded();
    }

    KillTimer(window_, hostRetryTimerId);
    hostRetryAttempts_ = 0U;
    hostShutdownPending_ = true;
    hostRunning_ = true;
    setConnected(false);
    updateHostLifecycleButton();
    SetWindowTextW(statusText_, L"正在关闭 Host...");
    setInfo(L"正在关闭 Host", L"等待主程序释放渲染与捕获资源。");
    scheduleHostShutdownPoll();
}

bool ControlCenterWindow::hostMutexPresent() const noexcept
{
    const HANDLE mutex = OpenMutexW(
        SYNCHRONIZE,
        FALSE,
        bafx::windows::kHostSingleInstanceMutexName);
    if (mutex != nullptr)
    {
        CloseHandle(mutex);
        return true;
    }

    // Access denied still proves that a named kernel object exists. This can
    // happen when Host and Control Center run at different integrity levels.
    return GetLastError() == ERROR_ACCESS_DENIED;
}

void ControlCenterWindow::scheduleHostRefreshRetry(const bool startPending) noexcept
{
    // Host recreates its single pipe instance after each short-lived client.
    // A bounded retry removes that startup race without a resident worker.
    hostRetryAttempts_ = hostRetryLimit;
    hostStartPending_ = startPending;
    hostRunning_ = hostRunning_ || hostMutexPresent();
    updateHostLifecycleButton();
    if (startPending)
    {
        SetWindowTextW(statusText_, L"正在启动 Host...");
    }
    KillTimer(window_, hostRetryTimerId);
    if (SetTimer(
            window_,
            hostRetryTimerId,
            hostRetryDelayMilliseconds,
            nullptr) == 0U)
    {
        hostRetryAttempts_ = 0U;
        hostStartPending_ = false;
        updateHostLifecycleButton();
    }
}

void ControlCenterWindow::scheduleHostShutdownPoll() noexcept
{
    hostShutdownDeadlineTicks_ = GetTickCount64()
        + hostShutdownTimeoutMilliseconds;
    KillTimer(window_, hostShutdownTimerId);
    if (SetTimer(
            window_,
            hostShutdownTimerId,
            hostShutdownPollDelayMilliseconds,
            nullptr) == 0U)
    {
        recoverHostShutdown(
            L"无法启动 Host 退出监视，请重试关闭操作。");
    }
}

void ControlCenterWindow::finishHostShutdown() noexcept
{
    KillTimer(window_, hostShutdownTimerId);
    hostLifetimeMutex_.reset();
    hostShutdownDeadlineTicks_ = 0U;
    hostShutdownPending_ = false;
    hostShutdownCommandAcknowledged_ = false;
    hostRunning_ = false;
    hostStartPending_ = false;
    setConnected(false);
    SetWindowTextW(statusText_, L"Host 已关闭");
    setInfo(L"Host 已关闭", L"点击“启动 Host”可重新开启特效。");
}

void ControlCenterWindow::recoverHostShutdown(const std::wstring_view message)
{
    KillTimer(window_, hostShutdownTimerId);
    hostLifetimeMutex_.reset();
    hostShutdownDeadlineTicks_ = 0U;
    hostShutdownPending_ = false;
    hostShutdownCommandAcknowledged_ = false;
    hostStartPending_ = false;
    hostRunning_ = hostMutexPresent();
    setConnected(false);
    SetWindowTextW(
        statusText_,
        hostRunning_ ? L"Host 仍在运行" : L"Host 已关闭");
    setError(message);
}

void ControlCenterWindow::updateHostLifecycleButton() const noexcept
{
    if (hostLifecycleButton_ == nullptr)
    {
        return;
    }
    const wchar_t* const text = hostShutdownPending_
        ? L"正在关闭..."
        : (hostRunning_
            ? L"关闭 Host"
            : (hostStartPending_ ? L"正在启动..." : L"启动 Host"));
    SetWindowTextW(hostLifecycleButton_, text);
    const BOOL lifecycleEnabled = hostShutdownPending_ ? FALSE : TRUE;
    EnableWindow(hostLifecycleButton_, lifecycleEnabled);
    if (refreshButton_ != nullptr)
    {
        EnableWindow(refreshButton_, lifecycleEnabled);
    }
}

bool ControlCenterWindow::ensureTrayIcon() noexcept
{
    if (trayIconAdded_)
    {
        return true;
    }
    if (window_ == nullptr)
    {
        return false;
    }

    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window_;
    icon.uID = trayIconIdentifier;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = trayNotificationMessage;
    icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    constexpr std::wstring_view tooltip = L"BAFX Control Center";
    const std::size_t tooltipLength = (std::min)(
        tooltip.size(),
        std::size(icon.szTip) - 1U);
    std::copy_n(tooltip.data(), tooltipLength, icon.szTip);
    icon.szTip[tooltipLength] = L'\0';
    trayIconAdded_ = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
    return trayIconAdded_;
}

void ControlCenterWindow::removeTrayIcon() noexcept
{
    if (!trayIconAdded_ || window_ == nullptr)
    {
        return;
    }

    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window_;
    icon.uID = trayIconIdentifier;
    static_cast<void>(Shell_NotifyIconW(NIM_DELETE, &icon));
    trayIconAdded_ = false;
}

void ControlCenterWindow::restoreFromTray() noexcept
{
    if (window_ == nullptr)
    {
        return;
    }
    ShowWindow(window_, SW_RESTORE);
    static_cast<void>(SetForegroundWindow(window_));
}

void ControlCenterWindow::showTrayMenu()
{
    if (window_ == nullptr)
    {
        return;
    }
    const HMENU menu = CreatePopupMenu();
    if (menu == nullptr)
    {
        return;
    }

    static_cast<void>(AppendMenuW(
        menu,
        MF_STRING | MF_DEFAULT,
        trayRestoreCommand,
        L"打开控制中心"));
    static_cast<void>(AppendMenuW(
        menu,
        MF_STRING,
        trayExitCommand,
        L"退出控制中心"));
    POINT cursor{};
    if (GetCursorPos(&cursor) == FALSE)
    {
        DestroyMenu(menu);
        return;
    }

    static_cast<void>(SetForegroundWindow(window_));
    const UINT command = TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        cursor.x,
        cursor.y,
        0,
        window_,
        nullptr);
    DestroyMenu(menu);
    static_cast<void>(PostMessageW(window_, WM_NULL, 0U, 0));
    if (command == trayRestoreCommand)
    {
        restoreFromTray();
    }
    else if (command == trayExitCommand)
    {
        commitPendingPatch();
        DestroyWindow(window_);
    }
}

void ControlCenterWindow::setConnected(const bool connected) noexcept
{
    connected_ = connected;
    if (connected)
    {
        hostRunning_ = true;
    }
    const BOOL enabled = connected ? TRUE : FALSE;
    const std::array controls{
        effectsEnabled_,
        effectsMode_,
        clickEnabled_,
        trailEnabled_,
        trailAlwaysOn_,
        leftClickEnabled_,
        rightClickEnabled_,
        middleClickEnabled_,
        globalScale_.trackbar,
        trailLength_.trackbar,
        trailWidth_.trackbar,
        inputSamplingRate_.trackbar,
        bloomIntensity_.trackbar,
        opacity_.trackbar,
        clickTimeScale_.trackbar,
        trailTimeScale_.trackbar,
        trailLifetimeMs_.trackbar,
        bloomDiffusion_.trackbar,
        bloomThreshold_.trackbar,
        bloomSoftKnee_.trackbar,
        bloomClamp_.trackbar,
        diskRadius_.trackbar,
        diskLifetimeMs_.trackbar,
        ringsHdrIntensity_.trackbar,
        ringsCount_.trackbar,
        ringsLifetimeMs_.trackbar,
        ringsRadiusMin_.trackbar,
        ringsRadiusMax_.trackbar,
        ringsAngularVelocityMultiplier_.trackbar,
        ringsRotationDirection_.trackbar,
        shardsHdrIntensity_.trackbar,
        shardsClickCount_.trackbar,
        shardsClickLifetimeMinMs_.trackbar,
        shardsClickLifetimeMaxMs_.trackbar,
        shardsClickRadius_.trackbar,
        shardsClickSpeedMin_.trackbar,
        shardsClickSpeedMax_.trackbar,
        shardsSizeMin_.trackbar,
        shardsSizeMax_.trackbar,
        trailOpacity_.trackbar,
        bloomQuality_,
        backgroundMode_,
        cursorExcluded_,
        allowSystemBorder_,
        idleOptimization_,
        startWithWindows_,
        startMinimized_,
        closeToTray_,
#if defined(BAFX_ENABLE_SPOUT2)
        spout2Enabled_,
#endif
        hdrEnabled_,
        framePacing_,
        pauseButton_,
        clearLogsButton_,
        resetDefaultsButton_};
    for (const HWND control : controls)
    {
        if (control != nullptr)
        {
            EnableWindow(control, enabled);
        }
    }
    if (!connected)
    {
#if defined(BAFX_ENABLE_SPOUT2)
        SetWindowTextW(spout2SenderStatus_, L"发送者状态：Host 未连接");
#endif
        displayState_ = {};
        displayStateError_ = L"Host 未连接，逐屏运行状态不可用。";
        static_cast<void>(SendMessageW(
            displaySelector_,
            CB_RESETCONTENT,
            0U,
            0));
        updateDisplayDetails();
    }
    if (displaySelector_ != nullptr)
    {
        const bool selectorEnabled = connected
            && displayStateError_.empty()
            && !displayState_.sessions.empty();
        EnableWindow(displaySelector_, selectorEnabled ? TRUE : FALSE);
    }
    updateDisplayPolicyControls();
    updateHostLifecycleButton();
}

void ControlCenterWindow::setInfo(
    const std::wstring_view title,
    const std::wstring_view message)
{
    const std::wstring text = std::wstring(title) + L"\r\n" + std::wstring(message);
    SetWindowTextW(messageText_, text.c_str());
}

void ControlCenterWindow::setError(const std::wstring_view message)
{
    setInfo(L"操作未完成", message);
}

void ControlCenterWindow::clearInfo() noexcept
{
    SetWindowTextW(messageText_, L"");
}

bool ControlCenterWindow::isChecked(const HWND control) const noexcept
{
    return SendMessageW(control, BM_GETCHECK, 0U, 0) == BST_CHECKED;
}

void ControlCenterWindow::setChecked(
    const HWND control,
    const bool checked) const noexcept
{
    static_cast<void>(SendMessageW(
        control,
        BM_SETCHECK,
        checked ? BST_CHECKED : BST_UNCHECKED,
        0));
}

const DisplaySessionState* ControlCenterWindow::selectedDisplaySession()
    const noexcept
{
    if (displaySelector_ == nullptr)
    {
        return nullptr;
    }
    const LRESULT selected = SendMessageW(
        displaySelector_,
        CB_GETCURSEL,
        0U,
        0);
    if (selected == CB_ERR)
    {
        return nullptr;
    }
    const LRESULT itemData = SendMessageW(
        displaySelector_,
        CB_GETITEMDATA,
        static_cast<WPARAM>(selected),
        0);
    if (itemData == CB_ERR
        || static_cast<std::size_t>(itemData) >= displayState_.sessions.size())
    {
        return nullptr;
    }
    return &displayState_.sessions[static_cast<std::size_t>(itemData)];
}

const bafx::config::DisplayOverrideConfig*
ControlCenterWindow::selectedOfflineDisplayOverride() const noexcept
{
    if (displaySelector_ == nullptr)
    {
        return nullptr;
    }
    const LRESULT selected = SendMessageW(
        displaySelector_,
        CB_GETCURSEL,
        0U,
        0);
    if (selected == CB_ERR)
    {
        return nullptr;
    }
    const LRESULT itemData = SendMessageW(
        displaySelector_,
        CB_GETITEMDATA,
        static_cast<WPARAM>(selected),
        0);
    if (itemData == CB_ERR
        || itemData < static_cast<LRESULT>(offlineDisplayItemBase))
    {
        return nullptr;
    }

    const std::size_t index = static_cast<std::size_t>(itemData)
        - offlineDisplayItemBase;
    if (index >= displayState_.offlineOverrides.size())
    {
        return nullptr;
    }
    return &displayState_.offlineOverrides[index];
}

double ControlCenterWindow::sliderValue(const SliderControl& slider) const noexcept
{
    const LRESULT position = SendMessageW(slider.trackbar, TBM_GETPOS, 0U, 0);
    return std::clamp(
        slider.minimum + static_cast<double>(position) * slider.step,
        slider.minimum,
        slider.maximum);
}

void ControlCenterWindow::setSliderValue(
    SliderControl& slider,
    const double value) const noexcept
{
    const double clamped = std::clamp(value, slider.minimum, slider.maximum);
    const int position = static_cast<int>(std::lround(
        (clamped - slider.minimum) / slider.step));
    static_cast<void>(SendMessageW(slider.trackbar, TBM_SETPOS, TRUE, position));
    updateSliderValueText(slider);
}

void ControlCenterWindow::updateSliderValueText(
    const SliderControl& slider) const noexcept
{
    const std::wstring text = numberText(sliderValue(slider));
    SetWindowTextW(slider.valueText, text.c_str());
}

std::wstring ControlCenterWindow::utf8ToWide(const std::string_view value)
{
    if (value.empty())
    {
        return {};
    }
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return L"文本长度超过 Win32 转换限制";
    }

    const int count = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (count <= 0)
    {
        return L"无法解码来自 Host 的 UTF-8 文本";
    }

    std::wstring result(static_cast<std::size_t>(count), L'\0');
    static_cast<void>(MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        count));
    return result;
}

std::string ControlCenterWindow::wideToUtf8(const std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return {};
    }
    const int count = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (count <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            count,
            nullptr,
            nullptr) != count)
    {
        return {};
    }
    return result;
}

std::wstring ControlCenterWindow::describeResponse(
    const bafx::windows::IpcClientResponse& response)
{
    if (!response.errorMessage.empty())
    {
        return utf8ToWide(response.errorMessage);
    }
    if (!response.errorCode.empty())
    {
        return utf8ToWide(response.errorCode);
    }
    return L"控制服务未返回可用响应";
}

std::string ControlCenterWindow::numberJson(const double value)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(3) << value;
    return stream.str();
}

std::string ControlCenterWindow::patchRequest(
    const std::uint64_t generation,
    const std::string_view path,
    const std::string_view valueJson)
{
    return "SetConfig {\"generation\":" + std::to_string(generation)
        + ",\"path\":\"" + std::string(path)
        + "\",\"value\":" + std::string(valueJson) + "}";
}

std::string ControlCenterWindow::fxPatchRequest(
    const std::uint64_t generation,
    const std::string_view path,
    const std::string_view valueJson)
{
    return "SetFxParam {\"generation\":" + std::to_string(generation)
        + ",\"path\":\"" + std::string(path)
        + "\",\"value\":" + std::string(valueJson) + "}";
}

std::wstring ControlCenterWindow::numberText(const double value)
{
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(2) << value;
    std::wstring text = stream.str();
    while (!text.empty() && text.back() == L'0')
    {
        text.pop_back();
    }
    if (!text.empty() && text.back() == L'.')
    {
        text.pop_back();
    }
    return text;
}

std::filesystem::path ControlCenterWindow::executableDirectory()
{
    return bafx::windows::executableDirectory();
}

}
