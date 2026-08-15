#pragma once

#include <windows.h>
#include <shellapi.h>

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

struct ExitUiStatus
{
    bool primaryHotKeyRegistered{false};
    bool fallbackHotKeyRegistered{false};
    bool notificationIconAdded{false};
};

struct CaptureExclusionStatus
{
    DWORD requestedAffinity{WDA_NONE};
    DWORD observedAffinity{WDA_NONE};
    DWORD setError{ERROR_SUCCESS};
    DWORD queryError{ERROR_SUCCESS};
    bool setSucceeded{false};
    bool querySucceeded{false};

    [[nodiscard]] bool confirmed() const noexcept;
};

struct CaptureExclusionQueryStatus
{
    DWORD expectedAffinity{WDA_NONE};
    DWORD observedAffinity{WDA_NONE};
    DWORD queryError{ERROR_SUCCESS};
    bool querySucceeded{false};

    [[nodiscard]] bool confirmed() const noexcept;
};

enum class PointerEventKind : std::uint8_t
{
    Move,
    LeftButtonDown,
    LeftButtonUp,
    Cancel
};

struct PointerEvent
{
    PointerEventKind kind{PointerEventKind::Move};
    POINT screenPosition{};
    std::int64_t qpcTimestamp{0};
    // GetMessageTime is retained separately from the dispatch QPC sample so
    // the host can identify input that waited in the Win32 queue.
    std::uint32_t messageTimeMilliseconds{0U};
    bool messageTimeValid{false};
};

struct PointerFrameEdge
{
    PointerEventKind kind{PointerEventKind::Cancel};
    PointerEvent trigger{};
};

struct PointerFrameSnapshot
{
    bool heldBefore{false};
    bool heldAfter{false};
    bool hasFinalHeldMove{false};
    bool hasFinalFreeMove{false};
    std::vector<PointerFrameEdge> edges{};
    std::optional<PointerEvent> latestNonCancelSample{};
    std::optional<PointerEvent> latestMoveSample{};
};

struct PointerQueueDiagnostics
{
    std::uint64_t rawInputMessages{0U};
    std::uint64_t moveEvents{0U};
    std::uint64_t buttonEdges{0U};
    std::uint64_t cancelEvents{0U};
    std::uint64_t compactedMoveEvents{0U};
    std::uint64_t overflowMoveDrops{0U};
    std::uint64_t messageTimeUnavailable{0U};
    std::uint32_t maximumPendingEvents{0U};
    std::uint32_t maximumWin32QueueAgeMilliseconds{0U};
};

enum class RawMouseRegistration : std::uint8_t
{
    Enabled,
    Disabled
};

[[nodiscard]] constexpr std::uint32_t win32MessageQueueAgeMilliseconds(
    const std::uint32_t dispatchTick,
    const std::uint32_t messageTime) noexcept
{
    return dispatchTick - messageTime;
}

// Legacy Unity input exposes one pointer state per rendered frame rather than
// replaying every OS move. This adapter owns only that pure state reduction;
// coordinate sampling, mapping, and effect dispatch remain host concerns.
class PointerFrameAdapter final
{
public:
    [[nodiscard]] PointerFrameSnapshot consume(
        const std::vector<PointerEvent>& events);

    [[nodiscard]] bool held() const noexcept;

private:
    bool held_{false};
};

[[nodiscard]] std::vector<PointerEvent> coalescePointerMoves(
    std::vector<PointerEvent> events) noexcept;

[[nodiscard]] std::vector<PointerEvent> compactPointerEventBacklog(
    std::vector<PointerEvent> events) noexcept;

class OverlayWindow final
{
public:
    OverlayWindow(
        HINSTANCE instance,
        RECT bounds,
        std::wstring_view title,
        RawMouseRegistration rawMouseRegistration = RawMouseRegistration::Enabled);
    ~OverlayWindow();

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    [[nodiscard]] HWND handle() const noexcept;
    [[nodiscard]] WindowSize size() const noexcept;
    // Per-monitor-v2 keeps client coordinates in physical pixels. Expose the
    // effective window DPI for diagnostics without changing that coordinate contract.
    [[nodiscard]] std::uint32_t effectiveDpi() const noexcept;
    [[nodiscard]] bool closeRequested() const noexcept;
    [[nodiscard]] ExitUiStatus exitUiStatus() const noexcept;
    [[nodiscard]] CaptureExclusionStatus setCaptureExcluded(bool excluded) noexcept;
    [[nodiscard]] CaptureExclusionQueryStatus queryCaptureExcluded(
        bool excluded) const noexcept;
    [[nodiscard]] std::optional<WindowSize> takePendingResize() noexcept;
    [[nodiscard]] std::vector<PointerEvent> takePointerEvents() noexcept;
    [[nodiscard]] PointerQueueDiagnostics takePointerQueueDiagnostics() noexcept;

    void show();
    void pollExitShortcut() noexcept;
    void pollPointerState() noexcept;

private:
    static LRESULT CALLBACK windowProcedure(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam);

    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    static ATOM registerWindowClass(HINSTANCE instance);
    void registerRawMouse();
    void unregisterRawMouse() noexcept;
    void releaseInputRegistrations(HWND window) noexcept;
    void handleRawInput(LPARAM lParam) noexcept;
    void pushPointerEvent(
        PointerEventKind kind,
        POINT position,
        std::int64_t qpc,
        std::uint32_t messageTimeMilliseconds = 0U,
        bool messageTimeValid = false) noexcept;
    void compactPendingPointerEvents() noexcept;
    void cancelPointer() noexcept;
    void requestClose() noexcept;
    void addNotificationIcon() noexcept;
    void removeNotificationIcon() noexcept;
    void showNotificationMenu() noexcept;

    HINSTANCE instance_{nullptr};
    HWND window_{nullptr};
    WindowSize size_{};
    std::optional<WindowSize> pendingResize_{};
    std::vector<PointerEvent> pendingPointerEvents_{};
    PointerQueueDiagnostics pointerQueueDiagnostics_{};
    bool closeRequested_{false};
    bool rawMouseRegistered_{false};
    bool primaryExitHotKeyRegistered_{false};
    bool fallbackExitHotKeyRegistered_{false};
    bool notificationIconAdded_{false};
    bool exitShortcutDown_{false};
    bool leftButtonDown_{false};
    UINT taskbarCreatedMessage_{0U};
    NOTIFYICONDATAW notificationIcon_{};
};

}
