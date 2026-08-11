#include "control_center_window.hpp"

#include "bafx/windows/portable_paths.hpp"

#include <commctrl.h>
#include <shellapi.h>

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
// WGC/D3D startup can take several seconds on a cold process. The control
// center keeps probing long enough for that process to become controllable.
constexpr std::uint32_t hostRetryLimit = 40U;
constexpr int minimumClientWidth = 860;
constexpr int minimumClientHeight = 520;
constexpr int defaultClientWidth = 960;
constexpr int defaultClientHeight = 600;
constexpr DWORD controlCenterWindowStyle =
    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;

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

void moveControl(
    const HWND control,
    const int x,
    const int y,
    const int width,
    const int height) noexcept
{
    if (control != nullptr)
    {
        static_cast<void>(MoveWindow(control, x, y, width, height, TRUE));
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
    }
    return -1;
}

[[nodiscard]] int renderModeIndex(const bafx::config::RenderMode mode) noexcept
{
    switch (mode)
    {
    case bafx::config::RenderMode::BackgroundAware:
        return 0;
    case bafx::config::RenderMode::Classic:
        return 1;
    case bafx::config::RenderMode::LightBackground:
        return 2;
    }
    return -1;
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
    RECT bounds{0, 0, scale(defaultClientWidth), scale(defaultClientHeight)};
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

    const int windowWidth = bounds.right - bounds.left;
    const int windowHeight = bounds.bottom - bounds.top;
    RECT workArea{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0U, &workArea, 0U) == FALSE)
    {
        workArea = RECT{
            0,
            0,
            GetSystemMetrics(SM_CXSCREEN),
            GetSystemMetrics(SM_CYSCREEN)};
    }
    const int workAreaWidth = static_cast<int>(workArea.right - workArea.left);
    const int workAreaHeight = static_cast<int>(workArea.bottom - workArea.top);
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
    case WM_DPICHANGED:
    {
        dpi_ = HIWORD(wParam);
        if (dpi_ == 0U)
        {
            dpi_ = USER_DEFAULT_SCREEN_DPI;
        }
        createFonts();
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested != nullptr)
        {
            SetWindowPos(
                window_,
                nullptr,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOACTIVATE | SWP_NOZORDER);
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
            scale(minimumClientWidth),
            scale(minimumClientHeight)};
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
            3.0,
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
            bloomIntensity_,
            L"Bloom 强度",
            0.0,
            8.0,
            0.05,
            "effects.bloomIntensity",
            ControlId::BloomIntensity);

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
        static_cast<void>(SendMessageW(bloomQuality_, CB_SETMINVISIBLE, 4U, 0));
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
        static_cast<void>(SendMessageW(backgroundMode_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(L"贴近原版")));
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

    const std::array required{
        titleText_,
        statusText_,
        messageText_,
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
        pauseButton_,
        refreshButton_,
        hostLifecycleButton_};
    if (!slidersCreated
        || std::ranges::find(required, nullptr) != required.end())
    {
        return false;
    }

    applyFonts();
    applyDpiMetrics();
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
        -MulDiv(10, static_cast<int>(dpi_), 72),
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
        -MulDiv(20, static_cast<int>(dpi_), 72),
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
        -MulDiv(11, static_cast<int>(dpi_), 72),
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
        bloomIntensity_.label,
        bloomIntensity_.trackbar,
        bloomIntensity_.valueText,
        bloomQualityLabel_,
        bloomQuality_,
        backgroundModeLabel_,
        backgroundMode_,
        cursorExcluded_,
        allowSystemBorder_,
        pauseButton_,
        refreshButton_,
        hostLifecycleButton_};
    for (const HWND control : normalControls)
    {
        setControlFont(control, normalFont_);
    }
    setControlFont(titleText_, titleFont_);
    setControlFont(effectsHeading_, sectionFont_);
    setControlFont(backgroundHeading_, sectionFont_);
}

void ControlCenterWindow::applyDpiMetrics() const noexcept
{
    const std::array comboBoxes{
        bloomQuality_,
        backgroundMode_};
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

void ControlCenterWindow::layoutControls(
    const int clientWidth,
    const int clientHeight) const noexcept
{
    if (titleText_ == nullptr || clientWidth <= 0 || clientHeight <= 0)
    {
        return;
    }

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
    const int messageHeight = scale(56);
    moveControl(messageText_, margin, scale(82), clientWidth - margin * 2, messageHeight);

    const int contentTop = scale(146);
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
}

void ControlCenterWindow::layoutSlider(
    const SliderControl& slider,
    const int x,
    const int y,
    const int width,
    const int height) const noexcept
{
    const int labelWidth = scale(106);
    const int valueWidth = scale(56);
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

int ControlCenterWindow::scale(const int logicalPixels) const noexcept
{
    return MulDiv(logicalPixels, static_cast<int>(dpi_), 96);
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
                applyPatch("background.mode", "\"classic\"");
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
    case ControlId::GlobalScale:
    case ControlId::TrailLength:
    case ControlId::TrailWidth:
    case ControlId::BloomIntensity:
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
        &bloomIntensity_};
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
    setSliderValue(bloomIntensity_, config.effects.bloomIntensity);
    static_cast<void>(SendMessageW(
        bloomQuality_,
        CB_SETCURSEL,
        qualityIndex(config.effects.bloomQuality),
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
        setInfo(L"配置已变化", L"已刷新 Host 的最新设置，请再次调整。");
        static_cast<void>(refreshFromHost());
        return;
    }
    setError(describeResponse(response));
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

    const HINSTANCE result = ShellExecuteW(
        nullptr,
        L"open",
        hostPath.c_str(),
        nullptr,
        hostPath.parent_path().c_str(),
        SW_SHOWNOACTIVATE);
    const INT_PTR resultCode = reinterpret_cast<INT_PTR>(result);
    if (resultCode <= 32)
    {
        hostStartPending_ = false;
        updateHostLifecycleButton();
        setError(L"启动 Host 失败，ShellExecute 错误码："
            + std::to_wstring(resultCode));
        return;
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
        bloomIntensity_.trackbar,
        bloomQuality_,
        backgroundMode_,
        cursorExcluded_,
        allowSystemBorder_,
        pauseButton_};
    for (const HWND control : controls)
    {
        if (control != nullptr)
        {
            EnableWindow(control, enabled);
        }
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
