#include "test_support.hpp"

#include "bafx/windows/borderless_capture_access.hpp"
#include "bafx/windows/error.hpp"
#include "bafx/windows/package_identity.hpp"
#include "bafx/windows/portable_paths.hpp"
#include "bafx/windows/runtime_diagnostics.hpp"

#include <appmodel.h>
#include <d3d11.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{

class TemporaryDiagnosticDirectory final
{
public:
    TemporaryDiagnosticDirectory()
    {
        path_ = std::filesystem::temp_directory_path()
            / ("bafx-runtime-diagnostics-"
                + std::to_string(GetCurrentProcessId())
                + "-"
                + std::to_string(GetTickCount64()));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDiagnosticDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDiagnosticDirectory(const TemporaryDiagnosticDirectory&) = delete;
    TemporaryDiagnosticDirectory& operator=(
        const TemporaryDiagnosticDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_{};
};

void writeText(const std::filesystem::path& path, const std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

[[nodiscard]] std::string readText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

[[nodiscard]] std::filesystem::path backupPath(
    const std::filesystem::path& path,
    const std::uint32_t index)
{
    std::filesystem::path backup(path);
    backup += L"." + std::to_wstring(index);
    return backup;
}

}

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

BAFX_TEST(device_lost_hresult_classifier_covers_reset_and_driver_failures)
{
    BAFX_CHECK(
        bafx::windows::isDeviceLostResult(DXGI_ERROR_DEVICE_REMOVED));
    BAFX_CHECK(
        bafx::windows::isDeviceLostResult(DXGI_ERROR_DEVICE_RESET));
    BAFX_CHECK(
        bafx::windows::isDeviceLostResult(DXGI_ERROR_DEVICE_HUNG));
    BAFX_CHECK(
        bafx::windows::isDeviceLostResult(DXGI_ERROR_DRIVER_INTERNAL_ERROR));
    BAFX_CHECK(!bafx::windows::isDeviceLostResult(E_FAIL));
}

BAFX_TEST(borderless_access_diagnostic_names_are_stable)
{
    const bafx::windows::BorderlessCaptureAccessResult denied{
        bafx::windows::BorderlessCaptureAccessStatus::DeniedByUser,
        E_ACCESSDENIED};
    BAFX_CHECK(!bafx::windows::borderlessCaptureAccessAllowed(denied));
    const std::string diagnostic =
        bafx::windows::borderlessCaptureAccessDiagnostic(denied);
    BAFX_CHECK(diagnostic.find("WGC.BorderlessAccess=denied-by-user")
        != std::string::npos);
    BAFX_CHECK(diagnostic.find("HRESULT=0x80070005") != std::string::npos);

    const bafx::windows::BorderlessCaptureAccessResult timedOut{
        bafx::windows::BorderlessCaptureAccessStatus::TimedOut,
        HRESULT_FROM_WIN32(ERROR_TIMEOUT)};
    const std::string timeoutDiagnostic =
        bafx::windows::borderlessCaptureAccessDiagnostic(timedOut);
    BAFX_CHECK(timeoutDiagnostic.find("WGC.BorderlessAccess=timed-out")
        != std::string::npos);
    BAFX_CHECK(timeoutDiagnostic.find("HRESULT=0x800705B4")
        != std::string::npos);
}

BAFX_TEST(borderless_access_preserves_package_identity_probe_failures)
{
    const bafx::windows::PackageIdentityInfo notPackaged{
        false,
        {},
        {},
        static_cast<DWORD>(APPMODEL_ERROR_NO_PACKAGE),
        static_cast<DWORD>(APPMODEL_ERROR_NO_PACKAGE)};
    bafx::windows::BorderlessCaptureAccessRequest portableRequest;
    portableRequest.begin(notPackaged);
    const auto portablePoll = portableRequest.poll();
    BAFX_CHECK(portablePoll.result.has_value());
    const bafx::windows::BorderlessCaptureAccessResult portable =
        *portablePoll.result;
    BAFX_CHECK(
        portable.status
        == bafx::windows::BorderlessCaptureAccessStatus::NotPackaged);
    BAFX_CHECK(
        portable.error
        == HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE));

    const bafx::windows::PackageIdentityInfo probeFailed{
        false,
        {},
        {},
        ERROR_NOT_ENOUGH_MEMORY,
        ERROR_NOT_ENOUGH_MEMORY};
    bafx::windows::BorderlessCaptureAccessRequest failureRequest;
    failureRequest.begin(probeFailed);
    const auto failurePoll = failureRequest.poll();
    BAFX_CHECK(failurePoll.result.has_value());
    const bafx::windows::BorderlessCaptureAccessResult failure =
        *failurePoll.result;
    BAFX_CHECK(
        failure.status
        == bafx::windows::BorderlessCaptureAccessStatus::Failed);
    BAFX_CHECK(failure.error == HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY));
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
    BAFX_CHECK(logPath.parent_path() == bafx::windows::runtimeDataDirectory());

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
    report.setPrimaryRefreshRate(
        bafx::windows::DisplayRefreshRate{60'000U, 1001U});
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
    report.setDisplayRuntimeSummary(
        bafx::windows::DisplayRuntimeSummary{
            2U,
            bafx::windows::CompositionOutputPreference::PreferLinearScRgb,
            bafx::windows::CompositionOutputPreference::ConservativeSdr,
            true,
            true,
            false});

    const std::string text = report.serialize();
    BAFX_CHECK(text.find("Product.Version=0.1.0-alpha.2") != std::string::npos);
    BAFX_CHECK(text.find(
        "Support.Scope=multi-display-runtime;fx-only-or-wgc;hardware-validation-not-run")
        != std::string::npos);
    BAFX_CHECK(text.find("Support.HDR=implemented-not-verified")
        != std::string::npos);
    BAFX_CHECK(text.find("Support.HDR.Validation=not-run")
        != std::string::npos);
    BAFX_CHECK(text.find("Support.MultiDisplay=implemented-not-verified")
        != std::string::npos);
    BAFX_CHECK(text.find("Display.SessionCount=2") != std::string::npos);
    BAFX_CHECK(text.find(
        "Display.Output.RequestedPreference=prefer-linear-scrgb")
        != std::string::npos);
    BAFX_CHECK(text.find(
        "Display.Output.EffectivePreference=conservative-sdr")
        != std::string::npos);
    BAFX_CHECK(text.find("Display.ColorSnapshotComplete=true")
        != std::string::npos);
    BAFX_CHECK(text.find("Display.HdrCapabilityObserved=true")
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
    BAFX_CHECK(text.find("Display.RefreshRateSource=dwm-composition-timing")
        != std::string::npos);
    BAFX_CHECK(text.find("Display.RefreshRateNumerator=60000")
        != std::string::npos);
    BAFX_CHECK(text.find("Display.RefreshRateDenominator=1001")
        != std::string::npos);
    BAFX_CHECK(text.find("Display.RefreshRateHz=59.940") != std::string::npos);
    BAFX_CHECK(text.find("Display.RefreshPeriodUs=16683.333")
        != std::string::npos);
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
    BAFX_CHECK(text.find("Log.SchemaVersion=2") != std::string::npos);
    BAFX_CHECK(text.find("Log.SessionId=") != std::string::npos);
}

BAFX_TEST(diagnostic_events_are_structured_and_keep_equals_inside_values)
{
    const TemporaryDiagnosticDirectory temporary;
    const std::filesystem::path logPath = temporary.path() / "support.log";
    const std::array fields{
        bafx::windows::DiagnosticField{"Runtime.Component", "render"},
        bafx::windows::DiagnosticField{"Runtime.Detail", "left=right\ncontinued"}};

    bafx::windows::appendDiagnosticEvent(
        logPath,
        "Runtime.Sample",
        fields);

    const std::string text = readText(logPath);
    BAFX_CHECK(text.find("Log.SchemaVersion=2\n") != std::string::npos);
    BAFX_CHECK(text.find("Log.SessionId=") != std::string::npos);
    BAFX_CHECK(text.find("Event.Sequence=") != std::string::npos);
    BAFX_CHECK(text.find("Event.MonotonicUs=") != std::string::npos);
    BAFX_CHECK(text.find("Event.ProcessId=") != std::string::npos);
    BAFX_CHECK(text.find("Event.ThreadId=") != std::string::npos);
    BAFX_CHECK(text.find("Event.Level=Info\n") != std::string::npos);
    BAFX_CHECK(text.find("Event.Name=Runtime.Sample\n") != std::string::npos);
    BAFX_CHECK(text.find("Runtime.Component=render\n") != std::string::npos);
    BAFX_CHECK(
        text.find("Runtime.Detail=left=right continued\n")
        != std::string::npos);
}

BAFX_TEST(diagnostic_log_rotation_keeps_a_bounded_backup_chain)
{
    const TemporaryDiagnosticDirectory temporary;
    const std::filesystem::path logPath = temporary.path() / "support.log";
    writeText(logPath, "current");
    writeText(backupPath(logPath, 1U), "previous");
    writeText(backupPath(logPath, 2U), "oldest");

    bafx::windows::rotateDiagnosticLog(
        logPath,
        bafx::windows::DiagnosticLogRetention{1U, 2U});

    BAFX_CHECK(!std::filesystem::exists(logPath));
    BAFX_CHECK(readText(backupPath(logPath, 1U)) == "current");
    BAFX_CHECK(readText(backupPath(logPath, 2U)) == "previous");

    bafx::windows::appendDiagnosticEvent(logPath, "AfterRotation");
    BAFX_CHECK(std::filesystem::exists(logPath));
    BAFX_CHECK(
        readText(logPath).find("Event.Name=AfterRotation\n")
        != std::string::npos);
}

BAFX_TEST(support_report_marks_primary_dpi_unknown_until_probed)
{
    bafx::windows::SupportReport report("test");
    const std::string text = report.serialize();
    BAFX_CHECK(text.find("Display.PrimaryDpi=unknown") != std::string::npos);
    BAFX_CHECK(text.find("Display.RefreshRateSource=not-probed")
        != std::string::npos);
    BAFX_CHECK(text.find("Display.RefreshRateHz=unknown") != std::string::npos);
    BAFX_CHECK(text.find("Display.ColorMode=not-probed;alpha-scope-sdr-only")
        != std::string::npos);
    BAFX_CHECK(text.find("Display.DxgiColorSpaceValue=unknown")
        != std::string::npos);
    BAFX_CHECK(text.find("Display.BitsPerColor=unknown") != std::string::npos);

    report.setPrimaryDpi(0U);
    report.setPrimaryRefreshRate(bafx::windows::DisplayRefreshRate{});
    BAFX_CHECK(
        report.serialize().find("Display.PrimaryDpi=unknown")
        != std::string::npos);
    BAFX_CHECK(
        report.serialize().find("Display.RefreshRateHz=unknown")
        != std::string::npos);
}

BAFX_TEST(support_report_clears_stale_display_color_after_retarget)
{
    bafx::windows::SupportReport report("test");
    report.setPrimaryDisplayColorCapabilities(
        bafx::windows::DisplayColorCapabilities{
            DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
            10U,
            0.005F,
            1000.0F,
            600.0F,
            true});
    report.clearPrimaryDisplayColorCapabilities();

    const std::string text = report.serialize();
    BAFX_CHECK(text.find("Display.ColorMode=not-probed;alpha-scope-sdr-only")
        != std::string::npos);
    BAFX_CHECK(text.find("Display.DxgiColorSpaceValue=unknown")
        != std::string::npos);
}

BAFX_TEST(support_report_distinguishes_unknown_fx_only_capture_visibility)
{
    bafx::windows::SupportReport report("test");
    report.setBackgroundCaptureStatus(
        bafx::windows::BackgroundCaptureStatus::
            FallbackFxOnlyCaptureVisibilityUnknown);

    BAFX_CHECK(
        report.serialize().find(
            "Support.WGC=fallback-fx-only-capture-visibility-unknown")
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

BAFX_TEST(capture_exclusion_health_query_requires_effective_affinity)
{
    bafx::windows::CaptureExclusionQueryStatus status{};
    status.expectedAffinity = WDA_EXCLUDEFROMCAPTURE;
    status.observedAffinity = WDA_EXCLUDEFROMCAPTURE;
    status.querySucceeded = true;
    BAFX_CHECK(status.confirmed());

    status.querySucceeded = false;
    status.queryError = ERROR_INVALID_WINDOW_HANDLE;
    BAFX_CHECK(!status.confirmed());

    const std::string failed =
        bafx::windows::captureExclusionQueryDiagnostic(status);
    BAFX_CHECK(failed.find("Expected=0x00000011") != std::string::npos);
    BAFX_CHECK(failed.find("Observed=0x00000011") != std::string::npos);
    BAFX_CHECK(
        failed.find("Query=failed;QueryError=0x00000578")
        != std::string::npos);
    BAFX_CHECK(failed.find("Confirmed=false") != std::string::npos);

    status.querySucceeded = true;
    status.queryError = ERROR_SUCCESS;
    status.observedAffinity = WDA_NONE;
    BAFX_CHECK(!status.confirmed());
}
