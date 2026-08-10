#include "bafx/windows/overlay_window.hpp"

#include "bafx/windows/error.hpp"

#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace bafx::windows
{
namespace
{

constexpr wchar_t windowClassName[] = L"BaClickFxDesktopOverlay";
constexpr int primaryExitHotKeyIdentifier = 0xBAF0;
constexpr int fallbackExitHotKeyIdentifier = 0xBAF1;
constexpr UINT notificationIconIdentifier = 0xBAF2U;
constexpr UINT notificationExitCommandIdentifier = 0xBAF3U;
constexpr UINT notificationIconMessage = WM_APP + 1U;
constexpr std::size_t maximumPendingPointerEvents = 2048U;

[[nodiscard]] std::uint32_t checkedDimension(const LONG value)
{
    if (value <= 0)
    {
        throw std::invalid_argument("Overlay bounds must have positive dimensions");
    }
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] DWORD applyExtendedStyle(
    const HWND window,
    const LONG_PTR extendedStyle) noexcept
{
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrW(
        window,
        GWL_EXSTYLE,
        extendedStyle);
    const DWORD styleError = GetLastError();
    if (previous == 0 && styleError != ERROR_SUCCESS)
    {
        return styleError;
    }
    if (!SetWindowPos(
            window,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_FRAMECHANGED
                | SWP_NOACTIVATE
                | SWP_NOMOVE
                | SWP_NOSIZE
                | SWP_NOZORDER))
    {
        return GetLastError();
    }
    return ERROR_SUCCESS;
}

}

std::vector<PointerEvent> coalescePointerMoves(
    std::vector<PointerEvent> events) noexcept
{
    std::size_t writeIndex = 0U;
    for (const PointerEvent& event : events)
    {
        if (event.kind == PointerEventKind::Move
            && writeIndex > 0U
            && events[writeIndex - 1U].kind == PointerEventKind::Move
            && events[writeIndex - 1U].screenPosition.x == event.screenPosition.x
            && events[writeIndex - 1U].screenPosition.y == event.screenPosition.y)
        {
            // Queued Raw Input can report the same latest cursor position more
            // than once; only exact duplicates are path-neutral to discard.
            events[writeIndex - 1U] = event;
            continue;
        }
        events[writeIndex] = event;
        ++writeIndex;
    }
    events.resize(writeIndex);
    return events;
}

bool CaptureExclusionStatus::confirmed() const noexcept
{
    return setSucceeded
        && querySucceeded
        && observedAffinity == requestedAffinity;
}

OverlayWindow::OverlayWindow(
    HINSTANCE instance,
    const RECT bounds,
    const std::wstring_view title)
    : instance_(instance)
{
    registerWindowClass(instance_);

    const LONG width = bounds.right - bounds.left;
    const LONG height = bounds.bottom - bounds.top;
    size_ = WindowSize{checkedDimension(width), checkedDimension(height)};

    // Cross-process hit testing does not reliably honor HTTRANSPARENT alone.
    // Keep the layered style so the full-screen overlay can never trap desktop input;
    // capture exclusion remains optional and falls back to FX-only when unavailable.
    const DWORD extendedStyle = WS_EX_NOACTIVATE
        | WS_EX_TOOLWINDOW
        | WS_EX_TOPMOST
        | WS_EX_LAYERED
        | WS_EX_TRANSPARENT
        | WS_EX_NOREDIRECTIONBITMAP;
    const std::wstring ownedTitle(title);
    window_ = CreateWindowExW(
        extendedStyle,
        windowClassName,
        ownedTitle.c_str(),
        WS_POPUP,
        bounds.left,
        bounds.top,
        width,
        height,
        nullptr,
        nullptr,
        instance_,
        this);
    if (window_ == nullptr)
    {
        throwLastError("CreateWindowExW");
    }

    pendingPointerEvents_.reserve(64);
    registerRawMouse();
    primaryExitHotKeyRegistered_ = RegisterHotKey(
        window_,
        primaryExitHotKeyIdentifier,
        MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
        VK_F12) != FALSE;
    fallbackExitHotKeyRegistered_ = RegisterHotKey(
        window_,
        fallbackExitHotKeyIdentifier,
        MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT,
        VK_F12) != FALSE;
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    addNotificationIcon();
}

OverlayWindow::~OverlayWindow()
{
    if (window_ != nullptr)
    {
        removeNotificationIcon();
        unregisterRawMouse();
        if (primaryExitHotKeyRegistered_)
        {
            UnregisterHotKey(window_, primaryExitHotKeyIdentifier);
        }
        if (fallbackExitHotKeyRegistered_)
        {
            UnregisterHotKey(window_, fallbackExitHotKeyIdentifier);
        }
        DestroyWindow(window_);
    }
}

HWND OverlayWindow::handle() const noexcept
{
    return window_;
}

WindowSize OverlayWindow::size() const noexcept
{
    return size_;
}

std::uint32_t OverlayWindow::effectiveDpi() const noexcept
{
    constexpr std::uint32_t defaultDpi = 96U;
    if (window_ == nullptr)
    {
        return defaultDpi;
    }

    const UINT dpi = GetDpiForWindow(window_);
    if (dpi != 0U)
    {
        return static_cast<std::uint32_t>(dpi);
    }

    // GetDpiForWindow can return zero while a window is being destroyed. Keep
    // diagnostics deterministic and avoid exposing an invalid scale factor.
    const UINT systemDpi = GetDpiForSystem();
    return systemDpi == 0U
        ? defaultDpi
        : static_cast<std::uint32_t>(systemDpi);
}

bool OverlayWindow::closeRequested() const noexcept
{
    return closeRequested_;
}

ExitUiStatus OverlayWindow::exitUiStatus() const noexcept
{
    return ExitUiStatus{
        primaryExitHotKeyRegistered_,
        fallbackExitHotKeyRegistered_,
        notificationIconAdded_};
}

CaptureExclusionStatus OverlayWindow::setCaptureExcluded(
    const bool excluded) noexcept
{
    CaptureExclusionStatus status{};
    status.requestedAffinity = excluded ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
    if (window_ == nullptr)
    {
        status.setError = ERROR_INVALID_WINDOW_HANDLE;
        status.queryError = ERROR_INVALID_WINDOW_HANDLE;
        return status;
    }

    const auto applyAffinity = [this, &status]() noexcept
    {
        SetLastError(ERROR_SUCCESS);
        status.setSucceeded = SetWindowDisplayAffinity(
            window_,
            status.requestedAffinity) != FALSE;
        status.setError = status.setSucceeded ? ERROR_SUCCESS : GetLastError();
    };

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR originalStyle = GetWindowLongPtrW(window_, GWL_EXSTYLE);
    const DWORD readStyleError = GetLastError();
    const bool styleAvailable = originalStyle != 0
        || readStyleError == ERROR_SUCCESS;
    const bool transitionLayered = excluded
        && styleAvailable
        && (originalStyle & WS_EX_LAYERED) != 0;
    if (!styleAvailable)
    {
        status.setError = readStyleError;
    }
    else if (!transitionLayered)
    {
        applyAffinity();
    }
    else
    {
        // Windows 10 rejects first-time capture exclusion on a layered HWND.
        // Establish affinity before restoring the style required for reliable
        // cross-process click-through; the effective value survives the change.
        const DWORD removeError = applyExtendedStyle(
            window_,
            originalStyle & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
        if (removeError == ERROR_SUCCESS)
        {
            applyAffinity();
        }
        else
        {
            status.setError = removeError;
        }

        const DWORD restoreError = applyExtendedStyle(window_, originalStyle);
        if (restoreError != ERROR_SUCCESS)
        {
            status.setSucceeded = false;
            status.setError = restoreError;
        }
        else if (status.setSucceeded)
        {
            // Reapply after the transition so success describes the final HWND.
            applyAffinity();
        }
    }

    // Set may report success while an older compositor applies WDA_MONITOR.
    // Always query the effective value before trusting capture exclusion.
    SetLastError(ERROR_SUCCESS);
    status.querySucceeded = GetWindowDisplayAffinity(
        window_,
        &status.observedAffinity) != FALSE;
    if (!status.querySucceeded)
    {
        status.queryError = GetLastError();
    }
    return status;
}

std::optional<WindowSize> OverlayWindow::takePendingResize() noexcept
{
    return std::exchange(pendingResize_, std::nullopt);
}

std::vector<PointerEvent> OverlayWindow::takePointerEvents() noexcept
{
    std::vector<PointerEvent> events;
    events.swap(pendingPointerEvents_);
    pendingPointerEvents_.reserve(64);
    return events;
}

void OverlayWindow::show()
{
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    if (!SetWindowPos(
            window_,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW))
    {
        throwLastError("SetWindowPos");
    }
}

void OverlayWindow::pollExitShortcut() noexcept
{
    const bool f12Down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    const bool controlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool shortcutDown = f12Down && controlDown && (altDown || shiftDown);
    if (shortcutDown && !exitShortcutDown_)
    {
        // Polling remains available when another process owns either registered hot key.
        requestClose();
    }
    exitShortcutDown_ = shortcutDown;
}

void OverlayWindow::pollPointerState() noexcept
{
    if (leftButtonDown_ && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0)
    {
        // Raw Input is normally lossless, but device changes and queue pressure
        // still need a physical-state escape from a permanently held stroke.
        cancelPointer();
    }
}

LRESULT CALLBACK OverlayWindow::windowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam)
{
    OverlayWindow* self = nullptr;
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<OverlayWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = window;
    }
    else
    {
        self = reinterpret_cast<OverlayWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (self != nullptr)
    {
        return self->handleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT OverlayWindow::handleMessage(
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam)
{
    if (taskbarCreatedMessage_ != 0U && message == taskbarCreatedMessage_)
    {
        notificationIconAdded_ = false;
        addNotificationIcon();
        return 0;
    }

    if (message == notificationIconMessage)
    {
        const UINT notificationMessage = LOWORD(lParam);
        if (notificationMessage == WM_CONTEXTMENU
            || notificationMessage == WM_RBUTTONUP)
        {
            showNotificationMenu();
        }
        else if (notificationMessage == WM_LBUTTONDBLCLK)
        {
            requestClose();
        }
        return 0;
    }

    switch (message)
    {
    case WM_NCHITTEST:
        return HTTRANSPARENT;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_ERASEBKGND:
        return 1;

    case WM_INPUT:
        handleRawInput(lParam);
        return DefWindowProcW(window_, message, wParam, lParam);

    case WM_INPUT_DEVICE_CHANGE:
        if (wParam == GIDC_REMOVAL)
        {
            cancelPointer();
        }
        return 0;

    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        cancelPointer();
        return 0;

    case WM_HOTKEY:
        if (static_cast<int>(wParam) == primaryExitHotKeyIdentifier
            || static_cast<int>(wParam) == fallbackExitHotKeyIdentifier)
        {
            requestClose();
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);

    case WM_COMMAND:
        if (LOWORD(wParam) == notificationExitCommandIdentifier)
        {
            requestClose();
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            const auto width = static_cast<std::uint32_t>(LOWORD(lParam));
            const auto height = static_cast<std::uint32_t>(HIWORD(lParam));
            if (width > 0U && height > 0U)
            {
                size_ = WindowSize{width, height};
                pendingResize_ = size_;
            }
        }
        return 0;

    case WM_DPICHANGED:
    {
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        if (!SetWindowPos(
                window_,
                nullptr,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOACTIVATE | SWP_NOZORDER))
        {
            requestClose();
        }
        return 0;
    }

    case WM_CLOSE:
        requestClose();
        return 0;

    case WM_DESTROY:
        closeRequested_ = true;
        window_ = nullptr;
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(window_, message, wParam, lParam);
    }
}

ATOM OverlayWindow::registerWindowClass(const HINSTANCE instance)
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &OverlayWindow::windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = windowClassName;

    const ATOM atom = RegisterClassExW(&windowClass);
    if (atom == 0)
    {
        const DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS)
        {
            throwLastError("RegisterClassExW");
        }
    }
    return atom;
}

void OverlayWindow::registerRawMouse()
{
    RAWINPUTDEVICE mouse{};
    mouse.usUsagePage = 0x01;
    mouse.usUsage = 0x02;
    mouse.dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    mouse.hwndTarget = window_;
    if (!RegisterRawInputDevices(&mouse, 1, sizeof(mouse)))
    {
        throwLastError("RegisterRawInputDevices");
    }
    rawMouseRegistered_ = true;
}

void OverlayWindow::unregisterRawMouse() noexcept
{
    if (!rawMouseRegistered_)
    {
        return;
    }

    RAWINPUTDEVICE mouse{};
    mouse.usUsagePage = 0x01;
    mouse.usUsage = 0x02;
    mouse.dwFlags = RIDEV_REMOVE;
    mouse.hwndTarget = nullptr;
    RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
    rawMouseRegistered_ = false;
}

void OverlayWindow::handleRawInput(const LPARAM lParam) noexcept
{
    RAWINPUT input{};
    UINT size = sizeof(input);
    const UINT bytes = GetRawInputData(
        reinterpret_cast<HRAWINPUT>(lParam),
        RID_INPUT,
        &input,
        &size,
        sizeof(RAWINPUTHEADER));
    if (bytes == static_cast<UINT>(-1) || input.header.dwType != RIM_TYPEMOUSE)
    {
        return;
    }

    POINT screenPosition{};
    LARGE_INTEGER qpc{};
    if (!GetCursorPos(&screenPosition) || !QueryPerformanceCounter(&qpc))
    {
        return;
    }

    const USHORT buttons = input.data.mouse.usButtonFlags;
    if ((buttons & RI_MOUSE_LEFT_BUTTON_DOWN) != 0U)
    {
        leftButtonDown_ = true;
        pushPointerEvent(PointerEventKind::LeftButtonDown, screenPosition, qpc.QuadPart);
    }

    if (input.data.mouse.lLastX != 0 || input.data.mouse.lLastY != 0)
    {
        pushPointerEvent(PointerEventKind::Move, screenPosition, qpc.QuadPart);
    }

    if ((buttons & RI_MOUSE_LEFT_BUTTON_UP) != 0U)
    {
        leftButtonDown_ = false;
        pushPointerEvent(PointerEventKind::LeftButtonUp, screenPosition, qpc.QuadPart);
    }
}

void OverlayWindow::pushPointerEvent(
    const PointerEventKind kind,
    const POINT position,
    const std::int64_t qpc) noexcept
{
    if (pendingPointerEvents_.size() >= maximumPendingPointerEvents)
    {
        // Keep the most recent input under a stalled renderer instead of growing without bound.
        pendingPointerEvents_.erase(
            pendingPointerEvents_.begin(),
            pendingPointerEvents_.begin()
                + static_cast<std::ptrdiff_t>(maximumPendingPointerEvents / 2U));
    }
    pendingPointerEvents_.push_back(PointerEvent{kind, position, qpc});
}

void OverlayWindow::cancelPointer() noexcept
{
    if (!leftButtonDown_)
    {
        return;
    }

    POINT screenPosition{};
    LARGE_INTEGER qpc{};
    GetCursorPos(&screenPosition);
    QueryPerformanceCounter(&qpc);
    leftButtonDown_ = false;
    pushPointerEvent(PointerEventKind::Cancel, screenPosition, qpc.QuadPart);
}

void OverlayWindow::requestClose() noexcept
{
    closeRequested_ = true;
    if (window_ != nullptr)
    {
        PostMessageW(window_, WM_NULL, 0, 0);
    }
}

void OverlayWindow::addNotificationIcon() noexcept
{
    if (window_ == nullptr || notificationIconAdded_)
    {
        return;
    }

    notificationIcon_ = NOTIFYICONDATAW{};
    notificationIcon_.cbSize = sizeof(notificationIcon_);
    notificationIcon_.hWnd = window_;
    notificationIcon_.uID = notificationIconIdentifier;
    notificationIcon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    notificationIcon_.uCallbackMessage = notificationIconMessage;
    notificationIcon_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    constexpr wchar_t tooltip[] = L"ba-click-fx-desktop - right-click to exit";
    static_assert(std::size(tooltip) <= std::size(notificationIcon_.szTip));
    std::copy(std::begin(tooltip), std::end(tooltip), notificationIcon_.szTip);

    notificationIconAdded_ = Shell_NotifyIconW(NIM_ADD, &notificationIcon_) != FALSE;
    if (notificationIconAdded_)
    {
        notificationIcon_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &notificationIcon_);
    }
}

void OverlayWindow::removeNotificationIcon() noexcept
{
    if (!notificationIconAdded_)
    {
        return;
    }
    Shell_NotifyIconW(NIM_DELETE, &notificationIcon_);
    notificationIconAdded_ = false;
}

void OverlayWindow::showNotificationMenu() noexcept
{
    const HMENU menu = CreatePopupMenu();
    if (menu == nullptr)
    {
        return;
    }

    if (!AppendMenuW(
            menu,
            MF_STRING,
            notificationExitCommandIdentifier,
            L"Exit"))
    {
        DestroyMenu(menu);
        return;
    }

    SetMenuDefaultItem(menu, notificationExitCommandIdentifier, FALSE);
    POINT cursor{};
    if (!GetCursorPos(&cursor))
    {
        DestroyMenu(menu);
        return;
    }

    SetForegroundWindow(window_);
    const UINT command = TrackPopupMenuEx(
        menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        cursor.x,
        cursor.y,
        window_,
        nullptr);
    DestroyMenu(menu);
    PostMessageW(window_, WM_NULL, 0, 0);
    if (command == notificationExitCommandIdentifier)
    {
        requestClose();
    }
}

}
