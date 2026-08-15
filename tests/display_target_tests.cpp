#include "test_support.hpp"

#include "display_target.hpp"

#include <cstdint>

using namespace bafx::desktop;

namespace
{

[[nodiscard]] HMONITOR monitor(const std::uintptr_t value) noexcept
{
    return reinterpret_cast<HMONITOR>(value);
}

}

BAFX_TEST(display_target_identity_includes_monitor_device_and_bounds)
{
    const DisplayTarget first{
        monitor(1U),
        L"\\\\.\\DISPLAY1",
        RECT{0, 0, 1920, 1080}};
    const DisplayTarget same = first;
    const DisplayTarget otherMonitor{
        monitor(2U),
        L"\\\\.\\DISPLAY2",
        RECT{0, 0, 1920, 1080}};

    BAFX_CHECK(sameDisplayTarget(first, same));
    BAFX_CHECK(!sameDisplayTarget(first, otherMonitor));
}

BAFX_TEST(display_target_identity_preserves_negative_virtual_coordinates)
{
    const DisplayTarget first{
        monitor(1U),
        L"\\\\.\\DISPLAY1",
        RECT{-1920, 0, 0, 1080}};
    const DisplayTarget moved{
        monitor(1U),
        L"\\\\.\\DISPLAY1",
        RECT{0, 0, 1920, 1080}};

    BAFX_CHECK(!sameDisplayTarget(first, moved));
    const bafx::windows::WindowSize size = displayTargetSize(first);
    BAFX_CHECK(size.width == 1920U);
    BAFX_CHECK(size.height == 1080U);
}

BAFX_TEST(display_target_intent_pins_geometry_application)
{
    const DisplayTarget target{
        monitor(1U),
        L"\\\\.\\DISPLAY1",
        RECT{0, 0, 2560, 1440}};
    const DisplayTargetIntent stable{target, false};
    const DisplayTargetIntent topologyChange{target, true};

    BAFX_CHECK(sameDisplayTargetIntent(stable, stable));
    BAFX_CHECK(!sameDisplayTargetIntent(stable, topologyChange));
}

BAFX_TEST(display_target_diagnostic_format_preserves_identity_and_origin)
{
    const DisplayTarget target{
        monitor(0x2AU),
        L"\\\\.\\DISPLAY2",
        RECT{-2560, 0, 0, 1440}};

    BAFX_CHECK(displayTargetDeviceUtf8(target) == "\\\\.\\DISPLAY2");
    BAFX_CHECK(formatDisplayTargetBounds(target) == "2560x1440@-2560,0");
    const std::string monitorText = formatDisplayTargetMonitor(target);
    BAFX_CHECK(monitorText.starts_with("0x"));
    BAFX_CHECK(monitorText.ends_with("2A"));
}
