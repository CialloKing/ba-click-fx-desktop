#include "bafx/windows/overlay_window.hpp"

#include "bafx/windows/error.hpp"

#include <windowsx.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace bafx::windows
{
namespace
{

constexpr wchar_t windowClassName[] = L"BaClickFxDesktopOverlay";

[[nodiscard]] std::uint32_t checkedDimension(const LONG value)
{
    if (value <= 0)
    {
        throw std::invalid_argument("Overlay bounds must have positive dimensions");
    }
    return static_cast<std::uint32_t>(value);
}

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

    const DWORD extendedStyle = WS_EX_NOACTIVATE
        | WS_EX_TOOLWINDOW
        | WS_EX_TOPMOST
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
}

OverlayWindow::~OverlayWindow()
{
    if (window_ != nullptr)
    {
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

bool OverlayWindow::closeRequested() const noexcept
{
    return closeRequested_;
}

std::optional<WindowSize> OverlayWindow::takePendingResize() noexcept
{
    return std::exchange(pendingResize_, std::nullopt);
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
    switch (message)
    {
    case WM_NCHITTEST:
        return HTTRANSPARENT;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_ERASEBKGND:
        return 1;

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
            closeRequested_ = true;
        }
        return 0;
    }

    case WM_CLOSE:
        closeRequested_ = true;
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

}

