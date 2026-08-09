#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace bafx::windows
{

struct WindowSize
{
    std::uint32_t width{0};
    std::uint32_t height{0};
};

class OverlayWindow final
{
public:
    OverlayWindow(HINSTANCE instance, RECT bounds, std::wstring_view title);
    ~OverlayWindow();

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    [[nodiscard]] HWND handle() const noexcept;
    [[nodiscard]] WindowSize size() const noexcept;
    [[nodiscard]] bool closeRequested() const noexcept;
    [[nodiscard]] std::optional<WindowSize> takePendingResize() noexcept;

    void show();

private:
    static LRESULT CALLBACK windowProcedure(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam);

    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    static ATOM registerWindowClass(HINSTANCE instance);

    HINSTANCE instance_{nullptr};
    HWND window_{nullptr};
    WindowSize size_{};
    std::optional<WindowSize> pendingResize_{};
    bool closeRequested_{false};
};

}

