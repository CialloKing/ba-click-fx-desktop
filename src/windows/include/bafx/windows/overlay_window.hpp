#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace bafx::windows
{

struct WindowSize
{
    std::uint32_t width{0};
    std::uint32_t height{0};
};

enum class PointerEventKind : std::uint8_t
{
    Move,
    LeftButtonDown,
    LeftButtonUp
};

struct PointerEvent
{
    PointerEventKind kind{PointerEventKind::Move};
    POINT screenPosition{};
    std::int64_t qpcTimestamp{0};
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
    [[nodiscard]] std::vector<PointerEvent> takePointerEvents() noexcept;

    void show();

private:
    static LRESULT CALLBACK windowProcedure(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam);

    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    static ATOM registerWindowClass(HINSTANCE instance);
    void registerRawMouse();
    void handleRawInput(LPARAM lParam) noexcept;
    void pushPointerEvent(PointerEventKind kind, POINT position, std::int64_t qpc) noexcept;

    HINSTANCE instance_{nullptr};
    HWND window_{nullptr};
    WindowSize size_{};
    std::optional<WindowSize> pendingResize_{};
    std::vector<PointerEvent> pendingPointerEvents_{};
    bool closeRequested_{false};
    bool exitHotKeyRegistered_{false};
};

}
