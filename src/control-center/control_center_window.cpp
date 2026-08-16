#include "control_center_window.hpp"

#include "config_commands.hpp"
#include "control_center_layout.hpp"
#include "package_activation.hpp"

#include "bafx/windows/portable_paths.hpp"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <cmath>
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

[[nodiscard]] std::string displaySessionKey(
    const DisplaySessionState& session)
{
    return session.device + '\n' + session.monitor;
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

}

ControlCenterWindow::ControlCenterWindow(const HINSTANCE instance) noexcept
    : instance_(instance)
    , client_(controlCenterIpcOptions())
{
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

bool ControlCenterWindow::create(const int showCommand)
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
    updateControls(HostState{}, bafx::config::defaultConfig());
    hostRunning_ = hostMutexPresent();
    setConnected(false);
    SetWindowTextW(statusText_, L"正在连接 Host...");
    updateHostLifecycleButton();
    ShowWindow(window_, showCommand == 0 ? SW_SHOWNORMAL : showCommand);
    UpdateWindow(window_);

    if (!refreshFromHost())
    {
        scheduleHostRefreshRetry();
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
    switch (message)
    {
    case WM_COMMAND:
        onCommand(LOWORD(wParam), HIWORD(wParam));
        return 0;
    case WM_HSCROLL:
        onSliderChanged(reinterpret_cast<HWND>(lParam));
        return 0;
    case WM_TIMER:
        onTimer(static_cast<UINT_PTR>(wParam));
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
    case WM_CLOSE:
        commitPendingPatch();
        DestroyWindow(window_);
        return 0;
    case WM_DESTROY:
        KillTimer(window_, patchTimerId);
        KillTimer(window_, hostRetryTimerId);
        KillTimer(window_, hostShutdownTimerId);
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
            "bloom.intensity",
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
            "trail.lifetimeMs",
            ControlId::TrailLifetimeMs)
        && createSlider(
            bloomDiffusion_,
            L"Bloom 扩散",
            0.0,
            10.0,
            0.01,
            "bloom.diffusion",
            ControlId::BloomDiffusion)
        && createSlider(
            bloomThreshold_,
            L"Bloom 阈值",
            0.0,
            64.0,
            0.01,
            "bloom.threshold",
            ControlId::BloomThreshold)
        && createSlider(
            bloomSoftKnee_,
            L"Bloom 软阈值",
            0.0,
            1.0,
            0.01,
            "bloom.softKnee",
            ControlId::BloomSoftKnee)
        && createSlider(
            bloomClamp_,
            L"Bloom 亮度上限",
            0.0,
            65504.0,
            1.0,
            "bloom.clamp",
            ControlId::BloomClamp);

    const bool particleSlidersCreated = createSlider(
        diskRadius_,
        L"光盘半径",
        20.0,
        120.0,
        0.01,
        "disk.radius",
        ControlId::DiskRadius)
        && createSlider(
            diskLifetimeMs_,
            L"光盘寿命 (ms)",
            50.0,
            500.0,
            1.0,
            "disk.lifetimeMs",
            ControlId::DiskLifetimeMs)
        && createSlider(
            ringsHdrIntensity_,
            L"圆环 HDR 强度",
            0.0,
            8.0,
            0.01,
            "rings.hdrIntensity",
            ControlId::RingsHdrIntensity)
        && createSlider(
            shardsHdrIntensity_,
            L"碎片 HDR 强度",
            0.0,
            8.0,
            0.01,
            "shards.hdrIntensity",
            ControlId::ShardsHdrIntensity)
        && createSlider(
            trailOpacity_,
            L"拖尾透明度",
            0.0,
            1.0,
            0.01,
            "trail.trailOpacity",
            ControlId::TrailOpacity);

    const bool ringSlidersCreated = createSlider(
        ringsCount_,
        L"圆环数量",
        0.0,
        6.0,
        1.0,
        "rings.count",
        ControlId::RingsCount)
        && createSlider(
            ringsLifetimeMs_,
            L"圆环寿命 (ms)",
            50.0,
            2000.0,
            1.0,
            "rings.lifetimeMs",
            ControlId::RingsLifetimeMs)
        && createSlider(
            ringsRadiusMin_,
            L"圆环最小半径",
            20.0,
            120.0,
            0.01,
            "rings.radiusMin",
            ControlId::RingsRadiusMin)
        && createSlider(
            ringsRadiusMax_,
            L"圆环最大半径",
            20.0,
            120.0,
            0.01,
            "rings.radiusMax",
            ControlId::RingsRadiusMax)
        && createSlider(
            ringsAngularVelocityMultiplier_,
            L"圆环角速度倍率",
            1.0,
            30.0,
            0.01,
            "rings.angularVelocityMultiplier",
            ControlId::RingsAngularVelocityMultiplier)
        && createSlider(
            ringsRotationDirection_,
            L"圆环旋转方向",
            -1.0,
            1.0,
            2.0,
            "rings.rotationDirection",
            ControlId::RingsRotationDirection);

    const bool clickShardSlidersCreated = createSlider(
        shardsClickCount_,
        L"点击碎片数量",
        0.0,
        12.0,
        1.0,
        "shards.clickCount",
        ControlId::ShardsClickCount)
        && createSlider(
            shardsClickLifetimeMinMs_,
            L"寿命下限 (ms)",
            100.0,
            1000.0,
            1.0,
            "shards.clickLifetimeMinMs",
            ControlId::ShardsClickLifetimeMinMs)
        && createSlider(
            shardsClickLifetimeMaxMs_,
            L"寿命上限 (ms)",
            100.0,
            1000.0,
            1.0,
            "shards.clickLifetimeMaxMs",
            ControlId::ShardsClickLifetimeMaxMs)
        && createSlider(
            shardsClickRadius_,
            L"出生半径",
            0.0,
            200.0,
            0.01,
            "shards.clickRadius",
            ControlId::ShardsClickRadius)
        && createSlider(
            shardsClickSpeedMin_,
            L"速度下限",
            0.0,
            200.0,
            0.01,
            "shards.clickSpeedMin",
            ControlId::ShardsClickSpeedMin)
        && createSlider(
            shardsClickSpeedMax_,
            L"速度上限",
            0.0,
            200.0,
            0.01,
            "shards.clickSpeedMax",
            ControlId::ShardsClickSpeedMax)
        && createSlider(
            shardsSizeMin_,
            L"共享碎片尺寸下限",
            0.0,
            100.0,
            0.01,
            "shards.sizeMin",
            ControlId::ShardsSizeMin)
        && createSlider(
            shardsSizeMax_,
            L"共享碎片尺寸上限",
            0.0,
            100.0,
            0.01,
            "shards.sizeMax",
            ControlId::ShardsSizeMax);

    advancedTimingHeading_ = createChild(
        L"BUTTON",
        L"Web API 时间与透明度",
        BS_GROUPBOX);
    advancedParticlesHeading_ = createChild(
        L"BUTTON",
        L"Web API 粒子与材质",
        BS_GROUPBOX);
    advancedRingsHeading_ = createChild(
        L"BUTTON",
        L"Web API 圆环参数",
        BS_GROUPBOX);
    advancedClickShardsHeading_ = createChild(
        L"BUTTON",
        L"Web API 点击碎片",
        BS_GROUPBOX);
    advancedBloomHeading_ = createChild(
        L"BUTTON",
        L"Web API Bloom",
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
        static_cast<void>(SendMessageW(backgroundMode_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(L"录屏兼容拟合")));
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

    displaySettingsHeading_ = createChild(
        L"BUTTON",
        L"显示与性能",
        BS_GROUPBOX);
    displaySelectorLabel_ = createChild(
        L"STATIC",
        L"运行状态显示器",
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
    if (framePacing_ != nullptr)
    {
        static_cast<void>(SendMessageW(
            framePacing_,
            CB_ADDSTRING,
            0U,
            reinterpret_cast<LPARAM>(L"跟随显示器")));
        static_cast<void>(SendMessageW(
            framePacing_,
            CB_ADDSTRING,
            0U,
            reinterpret_cast<LPARAM>(L"固定 60 FPS")));
        static_cast<void>(SendMessageW(
            framePacing_,
            CB_ADDSTRING,
            0U,
            reinterpret_cast<LPARAM>(L"固定 120 FPS")));
        static_cast<void>(SendMessageW(
            framePacing_,
            CB_ADDSTRING,
            0U,
            reinterpret_cast<LPARAM>(L"固定 144 FPS")));
        static_cast<void>(SendMessageW(framePacing_, CB_SETMINVISIBLE, 4U, 0));
    }
    displayDetailsHeading_ = createChild(
        L"BUTTON",
        L"所选显示器实际状态",
        BS_GROUPBOX);
    displayDetailsText_ = createChild(
        L"STATIC",
        L"Host 连接后显示逐屏运行状态。",
        SS_LEFT | SS_NOPREFIX);
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
        effectsHeading_,
        effectsEnabled_,
        clickEnabled_,
        trailEnabled_,
        trailAlwaysOn_,
        bloomQualityLabel_,
        bloomQuality_,
        backgroundHeading_,
        backgroundModeLabel_,
        backgroundMode_,
        cursorExcluded_,
        allowSystemBorder_,
        displaySettingsHeading_,
        displaySelectorLabel_,
        displaySelector_,
        displaySummaryText_,
        hdrEnabled_,
        framePacingLabel_,
        framePacing_,
        displayDetailsHeading_,
        displayDetailsText_,
        pauseButton_,
        refreshButton_,
        hostLifecycleButton_,
        resetDefaultsButton_,
        advancedTimingHeading_,
        advancedParticlesHeading_,
        advancedRingsHeading_,
        advancedClickShardsHeading_,
        advancedBloomHeading_,
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
        effectsEnabled_,
        clickEnabled_,
        trailEnabled_,
        trailAlwaysOn_,
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
        bloomQualityLabel_,
        bloomQuality_,
        backgroundModeLabel_,
        backgroundMode_,
        cursorExcluded_,
        allowSystemBorder_,
        displaySelectorLabel_,
        displaySelector_,
        displaySummaryText_,
        hdrEnabled_,
        framePacingLabel_,
        framePacing_,
        displayDetailsText_};
    for (const HWND control : normalControls)
    {
        setControlFont(control, normalFont_);
    }
    setControlFont(titleText_, titleFont_);
    setControlFont(effectsHeading_, sectionFont_);
    setControlFont(backgroundHeading_, sectionFont_);
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
        backgroundMode_,
        displaySelector_,
        framePacing_};
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
        moveControl(
            displayDetailsText_,
            detailsX + inset,
            contentTop + scale(30),
            (std::max)(scale(1), detailsWidth - inset * 2),
            (std::max)(scale(1), panelHeight - scale(46)));

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
    const int checkboxTop = contentTop + scale(32);
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

    int sliderTop = checkboxTop + scale(34);
    layoutSlider(globalScale_, groupLeft, sliderTop, groupWidth, scale(40));
    sliderTop += scale(46);
    layoutSlider(trailLength_, groupLeft, sliderTop, groupWidth, scale(40));
    sliderTop += scale(46);
    layoutSlider(trailWidth_, groupLeft, sliderTop, groupWidth, scale(40));
    sliderTop += scale(46);
    layoutSlider(inputSamplingRate_, groupLeft, sliderTop, groupWidth, scale(40));
    sliderTop += scale(46);
    layoutSlider(bloomIntensity_, groupLeft, sliderTop, groupWidth, scale(40));
    sliderTop += scale(48);

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
        pauseButton_,
        rightContentX,
        contentTop + scale(179),
        rightContentWidth,
        scale(38));

    const int actionGap = scale(10);
    const int actionWidth = (rightContentWidth - actionGap) / 2;
    moveControl(
        refreshButton_,
        rightContentX,
        contentTop + scale(229),
        actionWidth,
        scale(38));
    moveControl(
        hostLifecycleButton_,
        rightContentX + actionWidth + actionGap,
        contentTop + scale(229),
        actionWidth,
        scale(38));
    moveControl(
        resetDefaultsButton_,
        rightContentX,
        contentTop + scale(279),
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
        clickEnabled_,
        trailEnabled_,
        trailAlwaysOn_,
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
        allowSystemBorder_};
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
        trailOpacity_.valueText};
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
        displayDetailsHeading_,
        displayDetailsText_};
    for (const HWND control : displayControls)
    {
        setPageControlVisible(control, display);
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
                applyPatch("background.mode", "\"recording-compatible\"");
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
    case ControlId::DisplaySelector:
        if (notificationCode == CBN_SELCHANGE)
        {
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
    case ControlId::Refresh:
        if (notificationCode == BN_CLICKED)
        {
            static_cast<void>(refreshFromHost());
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
    updatingControls_ = true;

    setChecked(effectsEnabled_, config.effects.enabled);
    setChecked(clickEnabled_, config.effects.clickEnabled);
    setChecked(trailEnabled_, config.effects.trailEnabled);
    setChecked(trailAlwaysOn_, !config.input.trailOnlyWhilePressed);
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
    if (!displayStateError_.empty() || displayState_.sessions.empty())
    {
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

        if (displaySessionKey(session) == selectedDisplayKey_)
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
    updateDisplayDetails();
}

void ControlCenterWindow::updateDisplayDetails()
{
    if (!displayStateError_.empty())
    {
        SetWindowTextW(displaySummaryText_, L"逐屏运行状态不可用");
        SetWindowTextW(displayDetailsText_, displayStateError_.c_str());
        return;
    }
    if (displayState_.sessions.empty())
    {
        SetWindowTextW(displaySummaryText_, L"Host 当前没有活动显示会话");
        SetWindowTextW(
            displayDetailsText_,
            L"等待 Host 创建显示会话后刷新状态。");
        return;
    }

    const LRESULT selected = SendMessageW(
        displaySelector_,
        CB_GETCURSEL,
        0U,
        0);
    if (selected == CB_ERR)
    {
        SetWindowTextW(displaySummaryText_, L"尚未选择显示器");
        SetWindowTextW(displayDetailsText_, L"请选择一个显示器查看状态。");
        return;
    }
    const LRESULT itemData = SendMessageW(
        displaySelector_,
        CB_GETITEMDATA,
        static_cast<WPARAM>(selected),
        0);
    if (itemData == CB_ERR
        || static_cast<std::size_t>(itemData) >= displayState_.sessions.size())
    {
        SetWindowTextW(displaySummaryText_, L"显示器状态索引无效");
        SetWindowTextW(
            displayDetailsText_,
            L"请刷新状态以重新同步显示器列表。");
        return;
    }

    const DisplaySessionState& session = displayState_.sessions[
        static_cast<std::size_t>(itemData)];
    selectedDisplayKey_ = displaySessionKey(session);

    std::wstring summary = L"Host 报告 "
        + std::to_wstring(displayState_.sessions.size())
        + L" 个显示会话 | 状态代次 "
        + std::to_wstring(displayState_.generation);
    SetWindowTextW(displaySummaryText_, summary.c_str());

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
    details << L"设备：" << utf8ToWide(session.device)
            << L" | " << utf8ToWide(session.monitor)
            << L"\r\n角色：" << role
            << L"\r\n桌面：" << width << L" x " << height
            << L" @ (" << session.left << L", " << session.top << L")"
            << L" | DPI：" << session.windowDpi
            << L" / " << session.targetDpiX << L" x " << session.targetDpiY
            << L"\r\n刷新率：显示 " << refreshRateText(session.displayRefresh)
            << L" | 捕获 " << refreshRateText(session.captureRefresh)
            << L"\r\nGPU：" << utf8ToWide(session.adapter)
            << L" | 驱动：" << driverStateText(session.driver)
            << L"\r\n\r\n配置 HDR 请求："
            << (isChecked(hdrEnabled_) ? L"开启" : L"关闭")
            << L"\r\n输出：请求 " << outputStateText(session.requestedOutput)
            << L" | 解析 " << outputStateText(session.resolvedOutput)
            << L" | 实际 " << outputStateText(session.actualOutput)
            << L"\r\n策略满足："
            << (session.outputPolicySatisfied ? L"是" : L"否")
            << L" | 系统色彩：" << colorStateText(session.colorMode)
            << L"\r\nHDR："
            << optionalBooleanText(session.hdrSupported, L"支持", L"不支持")
            << L" / "
            << optionalBooleanText(session.hdrActive, L"已激活", L"未激活")
            << L"\r\n背景采样：" << captureState
            << L" | 重启：" << restartState
            << L"\r\n运行故障：" << faultState;
    if (!session.backgroundCaptureFailure.empty())
    {
        details << L"\r\n捕获错误："
                << utf8ToWide(session.backgroundCaptureFailure);
    }
    SetWindowTextW(displayDetailsText_, details.str().c_str());
}

void ControlCenterWindow::applyPatch(
    const std::string_view path,
    const std::string_view valueJson)
{
    if (!connected_)
    {
        setInfo(L"Host 未连接", L"请先启动 Host，然后刷新状态。");
        return;
    }

    const bafx::windows::IpcClientResponse response = client_.transact(
        patchRequest(generation_, path, valueJson));
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
        clickEnabled_,
        trailEnabled_,
        trailAlwaysOn_,
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
        hdrEnabled_,
        framePacing_,
        pauseButton_,
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
