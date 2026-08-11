#include "test_support.hpp"

#include "bafx/windows/package_identity.hpp"
#include "bafx/windows/portable_paths.hpp"
#include "bafx/windows/runtime_diagnostics.hpp"

#include <d3d11.h>

#include <filesystem>
#include <string>

BAFX_TEST(package_identity_probe_is_self_consistent)
{
    const bafx::windows::PackageIdentityInfo identity =
        bafx::windows::queryCurrentPackageIdentity();
    if (identity.present)
    {
        BAFX_CHECK(!identity.fullName.empty());
        BAFX_CHECK(identity.fullNameError == ERROR_SUCCESS);
    }
    else
    {
        BAFX_CHECK(identity.fullName.empty());
    }

    const std::string diagnostic =
        bafx::windows::packageIdentityDiagnostic(identity);
    BAFX_CHECK(diagnostic.find("Package.Identity=") != std::string::npos);
    BAFX_CHECK(diagnostic.find("FullNameError=0x") != std::string::npos);
}

BAFX_TEST(runtime_owned_paths_stay_beside_the_loaded_executable)
{
    const std::filesystem::path executableDirectory =
        bafx::windows::executableDirectory();
    BAFX_CHECK(!executableDirectory.empty());

    const std::filesystem::path logPath =
        bafx::windows::defaultDiagnosticLogPath();
    BAFX_CHECK(logPath.parent_path() == executableDirectory);
    BAFX_CHECK(logPath.filename() == L"ba-click-fx-desktop-support.log");

    const std::filesystem::path escapedPath = bafx::windows::executableFilePath(
        L"C:\\outside\\support.txt",
        L"fallback.txt");
    BAFX_CHECK(escapedPath.parent_path() == executableDirectory);
    BAFX_CHECK(escapedPath.filename() == L"support.txt");

    const std::filesystem::path traversalPath = bafx::windows::executableFilePath(
        L"..",
        L"fallback.txt");
    BAFX_CHECK(traversalPath.parent_path() == executableDirectory);
    BAFX_CHECK(traversalPath.filename() == L"fallback.txt");
}

BAFX_TEST(support_report_contains_alpha_scope_and_graphics_facts)
{
    bafx::windows::SupportReport report("0.1.0-alpha.2");
    report.setPrimaryMonitor(RECT{0, 0, 1920, 1080});
    report.setPrimaryDpi(144U);
    report.setPrimaryDisplayColorCapabilities(
        bafx::windows::DisplayColorCapabilities{
            DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
            10U,
            0.005F,
            1000.0F,
            600.0F,
            true});

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
    BAFX_CHECK(text.find(
        "Display.ColorMode=rgb-full-pq-p2020;capability-only;luminance-valid;alpha-scope-sdr-only")
        != std::string::npos);
    BAFX_CHECK(text.find("Display.DxgiColorSpaceValue=0x0000000C")
        != std::string::npos);
    BAFX_CHECK(text.find("Display.BitsPerColor=10") != std::string::npos);
    BAFX_CHECK(text.find("Display.MaxLuminanceNits=1000.000")
        != std::string::npos);
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
    BAFX_CHECK(text.find("Display.ColorMode=not-probed;alpha-scope-sdr-only")
        != std::string::npos);
    BAFX_CHECK(text.find("Display.DxgiColorSpaceValue=unknown")
        != std::string::npos);
    BAFX_CHECK(text.find("Display.BitsPerColor=unknown") != std::string::npos);

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
