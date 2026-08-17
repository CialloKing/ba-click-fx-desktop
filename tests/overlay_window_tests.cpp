#include "test_support.hpp"

#include "bafx/windows/overlay_window.hpp"

#include <windows.h>

using namespace bafx::windows;

namespace
{

[[nodiscard]] bool sameRect(const RECT& left, const RECT& right) noexcept
{
    return left.left == right.left
        && left.top == right.top
        && left.right == right.right
        && left.bottom == right.bottom;
}

}

BAFX_TEST(raw_pointer_stroke_ignores_traditional_capture_changes)
{
    BAFX_CHECK(!rawPointerMessageCancelsStroke(WM_CANCELMODE, 0U));
    BAFX_CHECK(!rawPointerMessageCancelsStroke(WM_CAPTURECHANGED, 0U));
    BAFX_CHECK(!rawPointerMessageCancelsStroke(
        WM_INPUT_DEVICE_CHANGE,
        GIDC_ARRIVAL));
    BAFX_CHECK(rawPointerMessageCancelsStroke(
        WM_INPUT_DEVICE_CHANGE,
        GIDC_REMOVAL));
}

BAFX_TEST(overlay_publishes_display_topology_changes_once)
{
    OverlayWindow window(
        GetModuleHandleW(nullptr),
        RECT{0, 0, 64, 64},
        L"ba-click-fx-overlay-window-test",
        RawMouseRegistration::Disabled);
    static_cast<void>(window.takePendingResize());

    BAFX_CHECK(!window.takeDisplayTopologyChange());
    SendMessageW(window.handle(), WM_DISPLAYCHANGE, 32U, MAKELPARAM(1920, 1080));
    BAFX_CHECK(window.takeDisplayTopologyChange());
    BAFX_CHECK(!window.takeDisplayTopologyChange());
}

BAFX_TEST(overlay_defers_dpi_placement_to_the_render_owner)
{
    OverlayWindow window(
        GetModuleHandleW(nullptr),
        RECT{0, 0, 64, 64},
        L"ba-click-fx-overlay-dpi-test",
        RawMouseRegistration::Disabled);
    static_cast<void>(window.takePendingResize());

    RECT before{};
    BAFX_CHECK(GetWindowRect(window.handle(), &before) != FALSE);
    RECT suggested{100, 100, 420, 300};
    SendMessageW(
        window.handle(),
        WM_DPICHANGED,
        MAKEWPARAM(144U, 144U),
        reinterpret_cast<LPARAM>(&suggested));

    RECT after{};
    BAFX_CHECK(GetWindowRect(window.handle(), &after) != FALSE);
    BAFX_CHECK(sameRect(before, after));
    BAFX_CHECK(window.takeDisplayTopologyChange());
    BAFX_CHECK(!window.takePendingResize().has_value());

    const std::vector<PointerEvent> events = window.takePointerEvents();
    BAFX_CHECK(events.size() == 1U);
    BAFX_CHECK(events.front().kind == PointerEventKind::Cancel);
}

BAFX_TEST(overlay_applies_physical_monitor_bounds)
{
    OverlayWindow window(
        GetModuleHandleW(nullptr),
        RECT{0, 0, 64, 64},
        L"ba-click-fx-overlay-bounds-test",
        RawMouseRegistration::Disabled);
    static_cast<void>(window.takePendingResize());

    const RECT requested{10, 20, 138, 116};
    window.setBounds(requested);

    RECT observed{};
    BAFX_CHECK(GetWindowRect(window.handle(), &observed) != FALSE);
    BAFX_CHECK(sameRect(requested, observed));
    const std::optional<WindowSize> resize = window.takePendingResize();
    BAFX_CHECK(resize.has_value());
    BAFX_CHECK(resize->width == 128U);
    BAFX_CHECK(resize->height == 96U);
    BAFX_CHECK(!window.takeDisplayTopologyChange());
}

BAFX_TEST(overlay_detects_geometry_changes_outside_its_owner_api)
{
    OverlayWindow window(
        GetModuleHandleW(nullptr),
        RECT{0, 0, 64, 64},
        L"ba-click-fx-overlay-external-move-test",
        RawMouseRegistration::Disabled);
    static_cast<void>(window.takePendingResize());

    BAFX_CHECK(SetWindowPos(
        window.handle(),
        nullptr,
        20,
        30,
        96,
        80,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER) != FALSE);
    BAFX_CHECK(window.takeDisplayTopologyChange());
    const std::vector<PointerEvent> events = window.takePointerEvents();
    BAFX_CHECK(events.size() == 1U);
    BAFX_CHECK(events.front().kind == PointerEventKind::Cancel);
}
