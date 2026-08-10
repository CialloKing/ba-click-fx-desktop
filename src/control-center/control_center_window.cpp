#include "control_center_window.hpp"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <locale>
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
constexpr ULONGLONG hostShutdownWarningMilliseconds = 4'000U;
constexpr std::uint32_t hostRetryLimit = 8U;
constexpr int minimumClientWidth = 760;
constexpr int minimumClientHeight = 460;

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

[[nodiscard]] int captureModeIndex(const bafx::config::CaptureMode mode) noexcept
{
    switch (mode)
    {
    case bafx::config::CaptureMode::FxOnly:
        return 0;
    case bafx::config::CaptureMode::BackgroundAware:
        return 1;
    case bafx::config::CaptureMode::RecordingCompatible:
        return 2;
    }
    return -1;
}

}

ControlCenterWindow::ControlCenterWindow(const HINSTANCE instance) noexcept
    : instance_(instance)
{
}

ControlCenterWindow::~ControlCenterWindow()
{
    destroyFonts();
}

bool ControlCenterWindow::create(const int showCommand)
{
    if (!registerWindowClass())
    {
        return false;
    }

    dpi_ = GetDpiForSystem();
    RECT bounds{0, 0, scale(minimumClientWidth), scale(minimumClientHeight)};
    if (AdjustWindowRectExForDpi(
            &bounds,
            WS_OVERLAPPEDWINDOW,
            FALSE,
            0U,
            dpi_) == FALSE)
    {
        lastError_ = GetLastError();
        return false;
    }

    window_ = CreateWindowExW(
        0U,
        windowClassName,
        windowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        bounds.right - bounds.left,
        bounds.bottom - bounds.top,
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
        return self->handleMessage(message, wParam, lParam);
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
        layoutControls(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_DPICHANGED:
    {
        dpi_ = HIWORD(wParam);
        createFonts();
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(
            window_,
            nullptr,
            suggested->left,
            suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOACTIVATE | SWP_NOZORDER);
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
        PostQuitMessage(0);
        return 0;
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
        SS_LEFT | SS_NOPREFIX);
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
        L"Bloom 质量",
        SS_LEFT | SS_NOPREFIX);
    bloomQuality_ = createChild(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
        ControlId::BloomQuality);
    if (bloomQuality_ != nullptr)
    {
        static_cast<void>(SendMessageW(bloomQuality_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(L"低")));
        static_cast<void>(SendMessageW(bloomQuality_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(L"中")));
        static_cast<void>(SendMessageW(bloomQuality_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(L"高")));
        static_cast<void>(SendMessageW(bloomQuality_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(L"极高")));
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
        static_cast<void>(SendMessageW(backgroundMode_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(L"仅特效")));
        static_cast<void>(SendMessageW(backgroundMode_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(L"背景感知")));
        static_cast<void>(SendMessageW(backgroundMode_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(L"录制兼容")));
        static_cast<void>(SendMessageW(backgroundMode_, CB_SETMINVISIBLE, 3U, 0));
    }

    cursorExcluded_ = createChild(
        L"BUTTON",
        L"排除鼠标指针",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::CursorExcluded);
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
        bloomQualityLabel_,
        bloomQuality_,
        backgroundHeading_,
        backgroundModeLabel_,
        backgroundMode_,
        cursorExcluded_,
        pauseButton_,
        refreshButton_,
        hostLifecycleButton_};
    if (!slidersCreated
        || std::ranges::find(required, nullptr) != required.end())
    {
        return false;
    }

    applyFonts();
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
        SS_RIGHT | SS_CENTERIMAGE | SS_NOPREFIX);
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

    normalFont_ = CreateFontW(
        -MulDiv(9, static_cast<int>(dpi_), 72),
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
    titleFont_ = CreateFontW(
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
    sectionFont_ = CreateFontW(
        -MulDiv(12, static_cast<int>(dpi_), 72),
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

    applyFonts();
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

void ControlCenterWindow::layoutControls(
    const int clientWidth,
    const int clientHeight) const noexcept
{
    if (titleText_ == nullptr)
    {
        return;
    }

    const int margin = scale(20);
    const int columnGap = scale(20);
    const int rightWidth = scale(230);
    const int leftWidth = (std::max)(
        scale(360),
        clientWidth - margin * 2 - columnGap - rightWidth);
    const int rightX = margin + leftWidth + columnGap;
    const int availableRightWidth = (std::max)(
        scale(180),
        clientWidth - rightX - margin);

    moveControl(titleText_, margin, scale(14), clientWidth - margin * 2, scale(34));
    moveControl(statusText_, margin, scale(50), clientWidth - margin * 2, scale(22));
    const int messageHeight = (std::clamp)(
        clientHeight - scale(418),
        scale(28),
        scale(48));
    moveControl(messageText_, margin, scale(78), clientWidth - margin * 2, messageHeight);

    const int contentTop = scale(84) + messageHeight;
    moveControl(effectsHeading_, margin, contentTop, leftWidth, scale(314));

    const int groupInset = scale(12);
    const int groupLeft = margin + groupInset;
    const int groupWidth = leftWidth - groupInset * 2;
    const int checkboxTop = contentTop + scale(27);
    const int checkboxWidth = groupWidth / 3;
    moveControl(effectsEnabled_, groupLeft, checkboxTop, checkboxWidth, scale(26));
    moveControl(clickEnabled_, groupLeft + checkboxWidth, checkboxTop, checkboxWidth, scale(26));
    moveControl(trailEnabled_, groupLeft + checkboxWidth * 2, checkboxTop, checkboxWidth, scale(26));

    int sliderTop = checkboxTop + scale(32);
    layoutSlider(globalScale_, groupLeft, sliderTop, groupWidth, scale(38));
    sliderTop += scale(42);
    layoutSlider(trailLength_, groupLeft, sliderTop, groupWidth, scale(38));
    sliderTop += scale(42);
    layoutSlider(trailWidth_, groupLeft, sliderTop, groupWidth, scale(38));
    sliderTop += scale(42);
    layoutSlider(bloomIntensity_, groupLeft, sliderTop, groupWidth, scale(38));
    sliderTop += scale(44);

    const int labelWidth = scale(94);
    moveControl(bloomQualityLabel_, groupLeft, sliderTop, labelWidth, scale(28));
    moveControl(
        bloomQuality_,
        groupLeft + labelWidth,
        sliderTop,
        groupWidth - labelWidth,
        scale(28));

    moveControl(backgroundHeading_, rightX, contentTop, availableRightWidth, scale(226));
    const int rightContentX = rightX + groupInset;
    const int rightContentWidth = availableRightWidth - groupInset * 2;
    moveControl(
        backgroundModeLabel_,
        rightContentX,
        contentTop + scale(27),
        rightContentWidth,
        scale(22));
    moveControl(
        backgroundMode_,
        rightContentX,
        contentTop + scale(51),
        rightContentWidth,
        scale(28));
    moveControl(
        cursorExcluded_,
        rightContentX,
        contentTop + scale(87),
        rightContentWidth,
        scale(28));
    moveControl(
        pauseButton_,
        rightContentX,
        contentTop + scale(127),
        rightContentWidth,
        scale(34));

    const int actionGap = scale(8);
    const int actionWidth = (rightContentWidth - actionGap) / 2;
    moveControl(
        refreshButton_,
        rightContentX,
        contentTop + scale(173),
        actionWidth,
        scale(34));
    moveControl(
        hostLifecycleButton_,
        rightContentX + actionWidth + actionGap,
        contentTop + scale(173),
        actionWidth,
        scale(34));
}

void ControlCenterWindow::layoutSlider(
    const SliderControl& slider,
    const int x,
    const int y,
    const int width,
    const int height) const noexcept
{
    const int labelWidth = scale(94);
    const int valueWidth = scale(48);
    const int gap = scale(4);
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
                applyPatch("background.mode", "\"fx-only\"");
                break;
            case 1:
                applyPatch("background.mode", "\"background-aware\"");
                break;
            case 2:
                applyPatch("background.mode", "\"recording-compatible\"");
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
    case ControlId::Refresh:
        if (notificationCode == BN_CLICKED)
        {
            static_cast<void>(refreshFromHost());
        }
        break;
    case ControlId::HostLifecycle:
        if (notificationCode == BN_CLICKED)
        {
            if (connected_)
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
        if (!hostShutdownPending_ || hostLifetimeMutex_.get() == nullptr)
        {
            KillTimer(window_, hostShutdownTimerId);
            return;
        }

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
            // An invalid observation handle cannot prove process exit. Keep
            // lifecycle controls disabled instead of guessing from pipe state.
            KillTimer(window_, hostShutdownTimerId);
            hostShutdownDeadlineTicks_ = 0U;
            setError(L"无法确认 Host 是否已经退出，启动按钮将保持禁用。");
            return;
        }

        if (hostShutdownDeadlineTicks_ != 0U
            && GetTickCount64() >= hostShutdownDeadlineTicks_)
        {
            // Keep observing the owned mutex after the warning. Re-enabling
            // Start here would race the old process while it still owns the
            // single-instance contract.
            hostShutdownDeadlineTicks_ = 0U;
            SetWindowTextW(statusText_, L"Host 关闭时间超过预期");
            setError(L"Host 仍在释放资源；确认退出前不会重复启动进程。");
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
            updateHostLifecycleButton();
            setError(L"Host 启动超时，请查看支持日志后重试。");
        }
    }
}

bool ControlCenterWindow::refreshFromHost()
{
    const bafx::windows::IpcClientResponse stateResponse = client_.transact("GetState");
    if (!stateResponse.succeeded())
    {
        setConnected(false);
        if (!hostShutdownPending_ && !hostStartPending_)
        {
            SetWindowTextW(statusText_, L"Host 未运行或控制服务不可用");
            setInfo(L"无法连接 Host", describeResponse(stateResponse));
        }
        return false;
    }

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
        captureModeIndex(config.background.mode),
        0));
    setChecked(cursorExcluded_, config.background.cursorExcluded);
    SetWindowTextW(pauseButton_, paused_ ? L"恢复特效" : L"暂停特效");

    updatingControls_ = false;
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
    if (hostStartPending_ || hostShutdownPending_)
    {
        return;
    }
    const std::filesystem::path hostPath = executableDirectory()
        / L"ba-click-fx-desktop.exe";
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

    scheduleHostRefreshRetry(true);
    setInfo(L"正在启动 Host", L"Host 初始化完成后会自动刷新。");
}

void ControlCenterWindow::stopHost()
{
    if (!connected_ || hostStartPending_ || hostShutdownPending_)
    {
        return;
    }

    if (pendingPatch_.has_value())
    {
        commitPendingPatch();
        if (!connected_)
        {
            return;
        }
    }
    const HANDLE lifetimeMutex = OpenMutexW(
        SYNCHRONIZE | MUTEX_MODIFY_STATE,
        FALSE,
        bafx::windows::kHostSingleInstanceMutexName);
    if (lifetimeMutex == nullptr)
    {
        setError(L"无法取得 Host 生命周期句柄，已取消关闭请求。");
        return;
    }

    hostLifetimeMutex_.reset(lifetimeMutex);
    // Host owns renderer teardown. The control center requests orderly exit
    // over IPC and observes the exact single-instance mutex; it never searches
    // by executable name or terminates an unrelated process.
    const bafx::windows::IpcClientResponse response = client_.transact("Shutdown");
    if (!response.succeeded())
    {
        hostLifetimeMutex_.reset();
        if (!response.transportSucceeded())
        {
            setConnected(false);
            SetWindowTextW(statusText_, L"Host 未运行或控制服务不可用");
        }
        setError(describeResponse(response));
        return;
    }

    KillTimer(window_, hostRetryTimerId);
    hostRetryAttempts_ = 0U;
    hostStartPending_ = false;
    hostShutdownPending_ = true;
    setConnected(false);
    updateHostLifecycleButton();
    SetWindowTextW(statusText_, L"正在关闭 Host...");
    setInfo(L"正在关闭 Host", L"等待主程序释放渲染与捕获资源。");
    scheduleHostShutdownPoll();
}

void ControlCenterWindow::scheduleHostRefreshRetry(const bool startPending) noexcept
{
    // Host recreates its single pipe instance after each short-lived client.
    // A bounded retry removes that startup race without a resident worker.
    hostRetryAttempts_ = hostRetryLimit;
    hostStartPending_ = startPending;
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
        + hostShutdownWarningMilliseconds;
    KillTimer(window_, hostShutdownTimerId);
    if (SetTimer(
            window_,
            hostShutdownTimerId,
            hostShutdownPollDelayMilliseconds,
            nullptr) == 0U)
    {
        hostShutdownDeadlineTicks_ = 0U;
        setError(L"无法监视 Host 退出，启动按钮将保持禁用。");
    }
}

void ControlCenterWindow::finishHostShutdown() noexcept
{
    KillTimer(window_, hostShutdownTimerId);
    hostLifetimeMutex_.reset();
    hostShutdownDeadlineTicks_ = 0U;
    hostShutdownPending_ = false;
    setConnected(false);
    SetWindowTextW(statusText_, L"Host 已关闭");
    setInfo(L"Host 已关闭", L"点击“启动 Host”可重新开启特效。");
}

void ControlCenterWindow::updateHostLifecycleButton() const noexcept
{
    if (hostLifecycleButton_ == nullptr)
    {
        return;
    }
    const wchar_t* const text = hostShutdownPending_
        ? L"正在关闭..."
        : (hostStartPending_
            ? L"正在启动..."
            : (connected_ ? L"关闭 Host" : L"启动 Host"));
    SetWindowTextW(hostLifecycleButton_, text);
    const BOOL lifecycleEnabled = hostShutdownPending_ || hostStartPending_
        ? FALSE
        : TRUE;
    EnableWindow(hostLifecycleButton_, lifecycleEnabled);
    if (refreshButton_ != nullptr)
    {
        EnableWindow(refreshButton_, lifecycleEnabled);
    }
}

void ControlCenterWindow::setConnected(const bool connected) noexcept
{
    connected_ = connected;
    const BOOL enabled = connected ? TRUE : FALSE;
    const std::array controls{
        effectsEnabled_,
        clickEnabled_,
        trailEnabled_,
        globalScale_.trackbar,
        trailLength_.trackbar,
        trailWidth_.trackbar,
        bloomIntensity_.trackbar,
        bloomQuality_,
        backgroundMode_,
        cursorExcluded_,
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
    std::array<wchar_t, 32'768U> buffer{};
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0U || length >= buffer.size())
    {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
}

}
