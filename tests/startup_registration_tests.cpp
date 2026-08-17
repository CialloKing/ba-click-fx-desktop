#include "test_support.hpp"

#include "bafx/windows/startup_registration.hpp"

#include <filesystem>

using bafx::windows::controlCenterStartupCommandLine;

BAFX_TEST(startup_registration_quotes_the_control_center_path)
{
    const std::filesystem::path path =
        LR"(C:\Program Files\ba-click-fx-desktop\BAFX.ControlCenter.exe)";
    BAFX_CHECK(
        controlCenterStartupCommandLine(path, false)
        == LR"("C:\Program Files\ba-click-fx-desktop\BAFX.ControlCenter.exe" --startup)");
    BAFX_CHECK(
        controlCenterStartupCommandLine(path, true)
        == LR"("C:\Program Files\ba-click-fx-desktop\BAFX.ControlCenter.exe" --startup --minimized)");
}

BAFX_TEST(startup_registration_rejects_an_unquotable_path)
{
    BAFX_CHECK(controlCenterStartupCommandLine({}, false).empty());
    BAFX_CHECK(
        controlCenterStartupCommandLine(
            std::filesystem::path(LR"(C:\bad"path\BAFX.ControlCenter.exe)"),
            false).empty());
}
