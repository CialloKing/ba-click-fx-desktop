#include "test_support.hpp"

#include "control_center_layout.hpp"

using bafx::control_center::PixelSize;
using bafx::control_center::clampPixelSize;
using bafx::control_center::controlCenterLayoutDpi;

BAFX_TEST(control_center_layout_preserves_requested_dpi_when_it_fits)
{
    BAFX_CHECK(controlCenterLayoutDpi(PixelSize{960, 600}, 96U) == 96U);
    BAFX_CHECK(controlCenterLayoutDpi(PixelSize{1440, 900}, 144U) == 144U);
    BAFX_CHECK(controlCenterLayoutDpi(PixelSize{1920, 1080}, 192U) == 192U);
}

BAFX_TEST(control_center_layout_uses_stable_compact_steps_for_small_work_areas)
{
    BAFX_CHECK(controlCenterLayoutDpi(PixelSize{1340, 690}, 144U) == 120U);
    BAFX_CHECK(controlCenterLayoutDpi(PixelSize{1880, 980}, 192U) == 180U);
    BAFX_CHECK(controlCenterLayoutDpi(PixelSize{700, 400}, 192U) == 96U);
}

BAFX_TEST(control_center_window_size_never_exceeds_the_work_area)
{
    const PixelSize clamped = clampPixelSize(
        PixelSize{1'440, 900},
        PixelSize{1'366, 728});
    BAFX_CHECK(clamped.width == 1'366);
    BAFX_CHECK(clamped.height == 728);

    const PixelSize unchanged = clampPixelSize(
        PixelSize{960, 600},
        PixelSize{1'920, 1'040});
    BAFX_CHECK(unchanged.width == 960);
    BAFX_CHECK(unchanged.height == 600);
}
