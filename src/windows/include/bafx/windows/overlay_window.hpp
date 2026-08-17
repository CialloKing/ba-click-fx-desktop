#pragma once

#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace bafx::windows
{

struct WindowSize
{
    std::uint32_t width{0};
    std::uint32_t height{0};
};

struct WindowResizeDiagnostics
{
    std::uint64_t clientRectQueryFailures{0U};
    DWORD lastClientRectQueryError{ERROR_SUCCESS};
};

enum class DisplayTopologyChangeSource : std::uint8_t
{
    WindowPosition = 1U << 0U,
    DisplayConfiguration = 1U << 1U,
    Dpi = 1U << 2U,
    Power = 1U << 3U
};

[[nodiscard]] constexpr std::uint8_t displayTopologyChangeSourceMask(
    const DisplayTopologyChangeSource source) noexcept
{
    return static_cast<std::uint8_t>(source);
}

struct DisplayTopologyChange
{
    std::uint8_t sourceMask{0U};
    std::uint32_t latestDpiX{0U};
    std::uint32_t latestDpiY{0U};
    RECT suggestedBounds{};
    bool dpiValid{false};
    bool suggestedBoundsValid{false};
    bool powerUnavailable{false};
    bool powerRestored{false};
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

[[nodiscard]] constexpr bool rawPointerMessageCancelsStroke(
    const UINT message,
    const WPARAM wParam) noexcept
{
    // Raw Input is process-global and does not own Win32 mouse capture.
    // Only removal of the producing device proves that its held stroke ended.
    return message == WM_INPUT_DEVICE_CHANGE && wParam == GIDC_REMOVAL;
}

enum class RawMouseRegistration : std::uint8_t
{
    Enabled,
    Disabled
};

enum class OverlayWindowRole : std::uint8_t
{
    HostShell,
    RenderSurface
};

struct OverlayWindowOptions final
{
    // Render surfaces are topology-owned and never acquire process-global
    // input or exit UI. The Raw Mouse option only applies to the Host shell.
    OverlayWindowRole role{OverlayWindowRole::HostShell};
    RawMouseRegistration rawMouseRegistration{RawMouseRegistration::Enabled};

    [[nodiscard]] static constexpr OverlayWindowOptions hostShell(
        const RawMouseRegistration rawMouse =
            RawMouseRegistration::Enabled) noexcept
    {
        return OverlayWindowOptions{
            OverlayWindowRole::HostShell,
            rawMouse};
    }

    [[nodiscard]] static constexpr OverlayWindowOptions renderSurface() noexcept
    {
        return OverlayWindowOptions{
            OverlayWindowRole::RenderSurface,
            RawMouseRegistration::Disabled};
    }
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
        std::span<const PointerEvent> events);

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
    OverlayWindow(
        HINSTANCE instance,
        RECT bounds,
        std::wstring_view title,
        OverlayWindowOptions options);
    ~OverlayWindow();

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    [[nodiscard]] HWND handle() const noexcept;
    [[nodiscard]] WindowSize size() const noexcept;
    [[nodiscard]] RECT bounds() const;
    [[nodiscard]] OverlayWindowRole role() const noexcept;
    // Per-monitor-v2 keeps client coordinates in physical pixels. Expose the
    // effective window DPI for diagnostics without changing that coordinate contract.
    [[nodiscard]] std::uint32_t effectiveDpi() const noexcept;
    [[nodiscard]] bool closeRequested() const noexcept;
    [[nodiscard]] ExitUiStatus exitUiStatus() const noexcept;
    [[nodiscard]] CaptureExclusionStatus setCaptureExcluded(bool excluded) noexcept;
    [[nodiscard]] CaptureExclusionQueryStatus queryCaptureExcluded(
        bool excluded) const noexcept;
    // Display messages only invalidate placement. The render owner consumes
    // the merged facts so WGC teardown and output rebinding stay transactional.
    [[nodiscard]] std::optional<DisplayTopologyChange>
        takeDisplayTopologyChange() noexcept;
    // Color-mode changes do not require a WGC restart when monitor geometry is
    // stable. Keep a separate signal so the owner can refresh HDR policy only.
    [[nodiscard]] bool takeDisplayColorChange() noexcept;
    [[nodiscard]] std::optional<WindowSize> takePendingResize() noexcept;
    [[nodiscard]] WindowResizeDiagnostics takeWindowResizeDiagnostics() noexcept;
    [[nodiscard]] std::vector<PointerEvent> takePointerEvents() noexcept;
    [[nodiscard]] PointerQueueDiagnostics takePointerQueueDiagnostics() noexcept;
    // Raw Input belongs to the Host shell, but a secondary surface can be the
    // only window notified about a per-monitor DPI or geometry transition.
    void invalidatePointerGeometry() noexcept;

    void setBounds(RECT bounds);
    void show();
    void hide() noexcept;
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
    void registerDisplayPowerNotification() noexcept;
    void unregisterDisplayPowerNotification() noexcept;
    void releaseHostShellRegistrations(HWND window) noexcept;
    void handleRawInput(LPARAM lParam) noexcept;
    void pushPointerEvent(
        PointerEventKind kind,
        POINT position,
        std::int64_t qpc,
        std::uint32_t messageTimeMilliseconds = 0U,
        bool messageTimeValid = false) noexcept;
    void compactPendingPointerEvents() noexcept;
    void cancelPointer() noexcept;
    void recordDisplayTopologyChange(
        DisplayTopologyChangeSource source,
        std::uint32_t latestDpiX = 0U,
        std::uint32_t latestDpiY = 0U,
        const RECT* suggestedBounds = nullptr,
        bool powerUnavailable = false,
        bool powerRestored = false) noexcept;
    void requestClose() noexcept;
    void addNotificationIcon() noexcept;
    void removeNotificationIcon() noexcept;
    void showNotificationMenu() noexcept;

    HINSTANCE instance_{nullptr};
    HWND window_{nullptr};
    OverlayWindowRole role_{OverlayWindowRole::HostShell};
    WindowSize size_{};
    std::optional<DisplayTopologyChange> pendingDisplayTopologyChange_{};
    bool displayColorChangePending_{false};
    bool applyingBounds_{false};
    std::optional<WindowSize> pendingResize_{};
    WindowResizeDiagnostics resizeDiagnostics_{};
    std::vector<PointerEvent> pendingPointerEvents_{};
    PointerQueueDiagnostics pointerQueueDiagnostics_{};
    bool closeRequested_{false};
    bool rawMouseRegistered_{false};
    HPOWERNOTIFY displayPowerNotification_{nullptr};
    std::optional<DWORD> consoleDisplayState_{};
    bool primaryExitHotKeyRegistered_{false};
    bool fallbackExitHotKeyRegistered_{false};
    bool notificationIconAdded_{false};
    bool exitShortcutDown_{false};
    bool leftButtonDown_{false};
    UINT taskbarCreatedMessage_{0U};
    NOTIFYICONDATAW notificationIcon_{};
};

}
