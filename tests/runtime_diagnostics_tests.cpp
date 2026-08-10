#include "test_support.hpp"

#include "bafx/windows/runtime_diagnostics.hpp"

#include <d3d11.h>

#include <string>

BAFX_TEST(support_report_contains_alpha_scope_and_graphics_facts)
{
    bafx::windows::SupportReport report("0.1.0-alpha.2");
    report.setPrimaryMonitor(RECT{0, 0, 1920, 1080});
    report.setPrimaryDpi(144U);

    bafx::windows::GraphicsDeviceInfo device{};
    device.driverType = bafx::windows::GraphicsDriverType::Warp;
    device.adapterDescription = L"Microsoft Basic Render Driver";
    device.vendorId = 0x1414U;
    device.deviceId = 0x008CU;
    device.featureLevel = D3D_FEATURE_LEVEL_11_0;
    device.hardwareCreateResult = DXGI_ERROR_UNSUPPORTED;
    report.setDeviceInfo(device);
    report.setExitUiStatus(bafx::windows::ExitUiStatus{true, false, true});
    report.setConfigurationSchemaVersion(3U);
    report.setControlServiceAvailable(true);

    const std::string text = report.serialize();
    BAFX_CHECK(text.find("Product.Version=0.1.0-alpha.2") != std::string::npos);
    BAFX_CHECK(text.find("Support.Scope=single-primary-monitor;fx-only-or-wgc;sdr-tested")
        != std::string::npos);
    BAFX_CHECK(text.find("Support.WGC=not-probed") != std::string::npos);
    BAFX_CHECK(text.find("Configuration.SchemaVersion=3") != std::string::npos);
    BAFX_CHECK(text.find("IPC.ControlService=active") != std::string::npos);
    BAFX_CHECK(text.find("Graphics.DriverType=WARP") != std::string::npos);
    BAFX_CHECK(text.find("Graphics.HardwareFallback=WARP") != std::string::npos);
    BAFX_CHECK(text.find("Graphics.Adapter=Microsoft Basic Render Driver")
        != std::string::npos);
    BAFX_CHECK(text.find("Graphics.FeatureLevel=11_0") != std::string::npos);
    BAFX_CHECK(text.find("Display.PrimaryDpi=144") != std::string::npos);
    BAFX_CHECK(text.find("Exit.PrimaryHotKey=registered") != std::string::npos);
    BAFX_CHECK(text.find("Exit.FallbackHotKey=polling-fallback")
        != std::string::npos);
    BAFX_CHECK(text.find("Exit.NotificationIcon=available") != std::string::npos);
    BAFX_CHECK(text.find("Exit.PollingFallback=enabled") != std::string::npos);
}

BAFX_TEST(support_report_marks_primary_dpi_unknown_until_probed)
{
    bafx::windows::SupportReport report("test");
    const std::string text = report.serialize();
    BAFX_CHECK(text.find("Display.PrimaryDpi=unknown") != std::string::npos);

    report.setPrimaryDpi(0U);
    BAFX_CHECK(
        report.serialize().find("Display.PrimaryDpi=unknown")
        != std::string::npos);
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

BAFX_TEST(capture_exclusion_diagnostic_preserves_win32_evidence)
{
    bafx::windows::CaptureExclusionStatus status{};
    status.requestedAffinity = WDA_EXCLUDEFROMCAPTURE;
    status.observedAffinity = WDA_NONE;
    status.setError = ERROR_INVALID_PARAMETER;
    status.querySucceeded = true;

    const std::string text = bafx::windows::captureExclusionDiagnostic(status);
    BAFX_CHECK(text.find("Requested=0x00000011") != std::string::npos);
    BAFX_CHECK(text.find("Observed=0x00000000") != std::string::npos);
    BAFX_CHECK(text.find("Set=failed;SetError=0x00000057") != std::string::npos);
    BAFX_CHECK(text.find("Query=succeeded;QueryError=0x00000000")
        != std::string::npos);
}

BAFX_TEST(capture_exclusion_confirmation_requires_complete_evidence)
{
    bafx::windows::CaptureExclusionStatus confirmed{};
    confirmed.requestedAffinity = WDA_EXCLUDEFROMCAPTURE;
    confirmed.observedAffinity = WDA_EXCLUDEFROMCAPTURE;
    confirmed.setSucceeded = true;
    confirmed.querySucceeded = true;
    BAFX_CHECK(confirmed.confirmed());

    // A matching observed value is not enough when SetWindowDisplayAffinity
    // failed: it could be a value left over from a previous mode.
    confirmed.setSucceeded = false;
    BAFX_CHECK(!confirmed.confirmed());

    confirmed.setSucceeded = true;
    confirmed.querySucceeded = false;
    BAFX_CHECK(!confirmed.confirmed());

    confirmed.querySucceeded = true;
    confirmed.observedAffinity = WDA_NONE;
    BAFX_CHECK(!confirmed.confirmed());

    bafx::windows::CaptureExclusionStatus disabled{};
    disabled.requestedAffinity = WDA_NONE;
    disabled.observedAffinity = WDA_NONE;
    disabled.setSucceeded = true;
    disabled.querySucceeded = true;
    BAFX_CHECK(disabled.confirmed());
}
