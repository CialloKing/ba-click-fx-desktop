#include "test_support.hpp"

#include "bafx/windows/runtime_diagnostics.hpp"

#include <d3d11.h>

#include <string>

BAFX_TEST(support_report_contains_alpha_scope_and_graphics_facts)
{
    bafx::windows::SupportReport report("0.1.0-alpha.2");
    report.setPrimaryMonitor(RECT{0, 0, 1920, 1080});

    bafx::windows::GraphicsDeviceInfo device{};
    device.driverType = bafx::windows::GraphicsDriverType::Warp;
    device.adapterDescription = L"Microsoft Basic Render Driver";
    device.vendorId = 0x1414U;
    device.deviceId = 0x008CU;
    device.featureLevel = D3D_FEATURE_LEVEL_11_0;
    device.hardwareCreateResult = DXGI_ERROR_UNSUPPORTED;
    report.setDeviceInfo(device);
    report.setExitUiStatus(bafx::windows::ExitUiStatus{true, false, true});

    const std::string text = report.serialize();
    BAFX_CHECK(text.find("Product.Version=0.1.0-alpha.2") != std::string::npos);
    BAFX_CHECK(text.find("Support.Scope=single-primary-monitor;fx-only-or-wgc;sdr-tested")
        != std::string::npos);
    BAFX_CHECK(text.find("Support.WGC=not-probed") != std::string::npos);
    BAFX_CHECK(text.find("Graphics.DriverType=WARP") != std::string::npos);
    BAFX_CHECK(text.find("Graphics.HardwareFallback=WARP") != std::string::npos);
    BAFX_CHECK(text.find("Graphics.Adapter=Microsoft Basic Render Driver")
        != std::string::npos);
    BAFX_CHECK(text.find("Graphics.FeatureLevel=11_0") != std::string::npos);
    BAFX_CHECK(text.find("Exit.PrimaryHotKey=registered") != std::string::npos);
    BAFX_CHECK(text.find("Exit.FallbackHotKey=polling-fallback")
        != std::string::npos);
    BAFX_CHECK(text.find("Exit.NotificationIcon=available") != std::string::npos);
    BAFX_CHECK(text.find("Exit.PollingFallback=enabled") != std::string::npos);
}

BAFX_TEST(support_report_sanitizes_multiline_failures)
{
    bafx::windows::SupportReport report("test");
    report.setFailure("device\nremoved=0x887A0005");
    const std::string text = report.serialize();
    BAFX_CHECK(text.find("Status=Failed") != std::string::npos);
    BAFX_CHECK(text.find("Failure=device removed 0x887A0005") != std::string::npos);
    BAFX_CHECK(text.find("Failure=device\n") == std::string::npos);
}
