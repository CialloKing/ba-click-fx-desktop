#include "control_center_window.hpp"

#include "config_commands.hpp"
#include "control_center_layout.hpp"
#include "package_activation.hpp"
#include "startup_config.hpp"

#include "product/version.hpp"
#include "bafx/windows/recording_compatibility.hpp"
#include "bafx/windows/portable_paths.hpp"
#include "bafx/windows/spout2_sender.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <locale>
#include <limits>
#include <exception>
#include <sstream>
#include <string>
#include <utility>

namespace bafx::control_center
{
namespace
{

constexpr UINT_PTR patchTimerId = 1U;
constexpr UINT_PTR hostRetryTimerId = 2U;
constexpr UINT_PTR hostShutdownTimerId = 3U;
constexpr UINT_PTR updateCheckTimerId = 4U;
constexpr UINT_PTR displayStateTimerId = 5U;
constexpr UINT patchDelayMilliseconds = 120U;
constexpr UINT hostRetryDelayMilliseconds = 250U;
constexpr UINT hostShutdownPollDelayMilliseconds = 100U;
constexpr UINT updateCheckPollDelayMilliseconds = 100U;
constexpr UINT displayStatePollDelayMilliseconds = 1'000U;
constexpr DWORD controlCenterIpcTimeoutMilliseconds = 100U;
constexpr ULONGLONG hostShutdownTimeoutMilliseconds = 10'000U;
constexpr UINT redrawAfterInteractiveResizeMessage = WM_APP + 1U;
constexpr UINT trayNotificationMessage = WM_APP + 2U;
constexpr UINT trayIconIdentifier = 1U;
constexpr UINT trayRestoreCommand = 1U;
constexpr UINT trayPauseCommand = 2U;
constexpr UINT trayExitCommand = 3U;
constexpr std::size_t offlineDisplayItemBase = 1U << 16U;
constexpr int themeColorReturnNotification = 0x7FFF;
constexpr wchar_t themeColorEditOriginalProcedureProperty[] =
    L"BAFX.ControlCenter.ThemeColorEditOriginalProcedure";
#if defined(BAFX_ENABLE_SPOUT2)
constexpr wchar_t obsSpoutPluginPage[] =
    L"https://github.com/Off-World-Live/obs-spout2-plugin/releases";
#endif
// WGC/D3D startup can take several seconds on a cold process. The control
// center keeps probing long enough for that process to become controllable.
constexpr std::uint32_t hostRetryLimit = 40U;
// Group boxes are visual siblings rather than opaque child containers. The
// parent must paint behind them so a live resize erases vacated control pixels.
constexpr DWORD controlCenterWindowStyle =
    WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS;
static_assert((controlCenterWindowStyle & WS_CLIPCHILDREN) == 0U);

[[nodiscard]] bool openFixedOfficialPage(
    const HWND owner,
    const wchar_t* const url)
{
    const HINSTANCE result = ShellExecuteW(
        owner,
        L"open",
        url,
        nullptr,
        nullptr,
        SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

struct InstallationStatePresentation final
{
    std::wstring titleLabel{};
    std::wstring details{};
};

[[nodiscard]] std::wstring asciiVersionToWide(
    const std::string_view value)
{
    return std::wstring(value.begin(), value.end());
}

[[nodiscard]] std::wstring_view invalidInstallStateLabel(
    const PackageActivationStateStatus status) noexcept
{
    switch (status)
    {
    case PackageActivationStateStatus::Missing:
        return tr(TextId::Missing);
    case PackageActivationStateStatus::Corrupt:
        return tr(TextId::Corrupt);
    case PackageActivationStateStatus::VersionMismatch:
        return tr(TextId::VersionMismatch);
    case PackageActivationStateStatus::PartialUpgrade:
        return tr(TextId::PartialUpgrade);
    case PackageActivationStateStatus::RepairRequired:
        return tr(TextId::InstallerRepairRequired);
    case PackageActivationStateStatus::Valid:
    case PackageActivationStateStatus::BackupRecovered:
        return tr(TextId::UnknownFailure);
    }
    return tr(TextId::UnknownFailure);
}

[[nodiscard]] InstallationStatePresentation installationStatePresentation(
    const std::filesystem::path& executableDirectory)
{
    const PackageActivationIdentityResult packageIdentity =
        readPackageActivationState(executableDirectory);
    if (!packageIdentity.installStatePresent
        && packageIdentity.status == PackageActivationStateStatus::Missing)
    {
        return InstallationStatePresentation{
            tr(TextId::Portable),
            tr(TextId::PortableDetails)};
    }
    if (packageIdentity.succeeded())
    {
        const PackageActivationIdentity& identity = *packageIdentity.identity;
        std::wstring details = tr(TextId::InstalledDetails);
        if (packageIdentity.recoveredFromBackup())
        {
            details += tr(TextId::BackupRecovered);
        }
        details += tr(TextId::ProductVersionLabel);
        details += asciiVersionToWide(identity.productVersion);
        details += tr(TextId::PackageVersionLabel);
        details += asciiVersionToWide(identity.packageVersion);
        if (packageIdentity.certificateStatus
            == PackageCertificateStatus::Expired)
        {
            details +=
                tr(TextId::CertificateExpired);
        }
        return InstallationStatePresentation{
            tr(TextId::Installed),
            std::move(details)};
    }

    std::wstring details = formatText(TextId::InvalidInstallationDetails,
        {std::wstring(invalidInstallStateLabel(packageIdentity.status))});
    if (packageIdentity.identity.has_value())
    {
        details += tr(TextId::ProductVersionLabel);
        details += asciiVersionToWide(
            packageIdentity.identity->productVersion);
        details += tr(TextId::PackageVersionLabel);
        details += asciiVersionToWide(
            packageIdentity.identity->packageVersion);
    }
    details += tr(TextId::RepairInstallation);
    return InstallationStatePresentation{
        tr(TextId::InvalidInstallation),
        std::move(details)};
}

[[nodiscard]] std::wstring hresultText(const HRESULT result)
{
    std::wostringstream stream;
    stream << L"0x"
           << std::uppercase
           << std::hex
           << std::setw(8)
           << std::setfill(L'0')
           << static_cast<unsigned long>(result);
    return stream.str();
}

#if defined(BAFX_ENABLE_SPOUT2)
[[nodiscard]] std::wstring spout2StatusText(const std::string_view status)
{
    if (status == "disabled")
    {
        return tr(TextId::Disabled);
    }
    if (status == "waiting-for-frame")
    {
        return tr(TextId::WaitingForFrame);
    }
    if (status == "sent")
    {
        return tr(TextId::Sending);
    }
    if (status == "unavailable")
    {
        return tr(TextId::UnavailableBuild);
    }
    if (status == "failed")
    {
        return tr(TextId::SendFailed);
    }
    return tr(TextId::UnknownStatus);
}
#endif

[[nodiscard]] bafx::windows::IpcClientOptions controlCenterIpcOptions()
{
    bafx::windows::IpcClientOptions options{};
    // Control Center runs transactions on its window thread. A short local
    // timeout keeps a starting or unavailable Host from freezing the UI.
    options.timeoutMilliseconds = controlCenterIpcTimeoutMilliseconds;
    return options;
}

[[nodiscard]] HMENU controlMenu(const int id) noexcept
{
    if (id == 0)
    {
        return nullptr;
    }
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

[[nodiscard]] RECT monitorWorkArea(const HMONITOR monitor) noexcept
{
    MONITORINFO information{};
    information.cbSize = sizeof(information);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &information) != FALSE)
    {
        return information.rcWork;
    }

    RECT workArea{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0U, &workArea, 0U) != FALSE)
    {
        return workArea;
    }
    return RECT{
        0,
        0,
        GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN)};
}

[[nodiscard]] PixelSize maximumClientSize(
    const RECT workArea,
    const UINT dpi) noexcept
{
    const int workAreaWidth = static_cast<int>(
        workArea.right - workArea.left);
    const int workAreaHeight = static_cast<int>(
        workArea.bottom - workArea.top);
    RECT nonClientBounds{};
    int nonClientWidth = 0;
    int nonClientHeight = 0;
    if (AdjustWindowRectExForDpi(
            &nonClientBounds,
            controlCenterWindowStyle,
            FALSE,
            0U,
            dpi) != FALSE)
    {
        nonClientWidth = nonClientBounds.right - nonClientBounds.left;
        nonClientHeight = nonClientBounds.bottom - nonClientBounds.top;
    }
    return PixelSize{
        (std::max)(1, workAreaWidth - nonClientWidth),
        (std::max)(1, workAreaHeight - nonClientHeight)};
}

void moveControl(
    const HWND control,
    const int x,
    const int y,
    const int width,
    const int height) noexcept
{
    if (control != nullptr)
    {
        // Suppress intermediate paints while the sibling controls overlap.
        // layoutControls() redraws the complete parent and child tree after
        // every control has reached its final position.
        static_cast<void>(SetWindowPos(
            control,
            nullptr,
            x,
            y,
            width,
            height,
            SWP_NOACTIVATE
                | SWP_NOCOPYBITS
                | SWP_NOREDRAW
                | SWP_NOOWNERZORDER
                | SWP_NOZORDER));
    }
}

void setControlFont(const HWND control, const HFONT font) noexcept
{
    if (control != nullptr && font != nullptr)
    {
        static_cast<void>(SendMessageW(
            control,
            WM_SETFONT,
            reinterpret_cast<WPARAM>(font),
            TRUE));
    }
}

[[nodiscard]] COLORREF themeColorRef(const std::string_view value) noexcept
{
    if (value.size() != 7U || value.front() != '#')
    {
        return RGB(76, 167, 255);
    }
    const auto channel = [](const char high, const char low) noexcept
    {
        const auto nibble = [](const char character) noexcept
        {
            if (character >= '0' && character <= '9')
            {
                return static_cast<unsigned int>(character - '0');
            }
            if (character >= 'a' && character <= 'f')
            {
                return static_cast<unsigned int>(character - 'a' + 10);
            }
            if (character >= 'A' && character <= 'F')
            {
                return static_cast<unsigned int>(character - 'A' + 10);
            }
            return 0U;
        };
        return (nibble(high) << 4U) | nibble(low);
    };
    return RGB(
        channel(value[1], value[2]),
        channel(value[3], value[4]),
        channel(value[5], value[6]));
}

LRESULT CALLBACK themeColorEditProcedure(
    const HWND edit,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) noexcept
{
    const WNDPROC original = reinterpret_cast<WNDPROC>(GetPropW(
        edit,
        themeColorEditOriginalProcedureProperty));
    if (message == WM_KEYDOWN && wParam == VK_RETURN)
    {
        const HWND parent = GetParent(edit);
        if (parent != nullptr)
        {
            static_cast<void>(SendMessageW(
                parent,
                WM_COMMAND,
                MAKEWPARAM(
                    GetDlgCtrlID(edit),
                    themeColorReturnNotification),
                reinterpret_cast<LPARAM>(edit)));
        }
        return 0;
    }
    return original == nullptr
        ? DefWindowProcW(edit, message, wParam, lParam)
        : CallWindowProcW(original, edit, message, wParam, lParam);
}

[[nodiscard]] int qualityIndex(const bafx::config::BloomQuality quality) noexcept
{
    switch (quality)
    {
    case bafx::config::BloomQuality::Low:
        return 0;
    case bafx::config::BloomQuality::Medium:
        return 1;
    case bafx::config::BloomQuality::High:
        return 2;
    case bafx::config::BloomQuality::Ultra:
        return 3;
    case bafx::config::BloomQuality::Custom:
        return 4;
    }
    return -1;
}

[[nodiscard]] int renderModeIndex(const bafx::config::RenderMode mode) noexcept
{
    switch (mode)
    {
    case bafx::config::RenderMode::BackgroundAware:
        return 0;
    case bafx::config::RenderMode::RecordingCompatible:
        return 1;
    case bafx::config::RenderMode::LightBackground:
        return 2;
    }
    return -1;
}

[[nodiscard]] int effectsModeIndex(
    const bafx::config::EffectsMode mode) noexcept
{
    return mode == bafx::config::EffectsMode::Core ? 1 : 0;
}

[[nodiscard]] int framePacingIndex(
    const bafx::config::FramePacing pacing) noexcept
{
    switch (pacing)
    {
    case bafx::config::FramePacing::MatchDisplay:
        return 0;
    case bafx::config::FramePacing::Fixed60:
        return 1;
    case bafx::config::FramePacing::Fixed120:
        return 2;
    case bafx::config::FramePacing::Fixed144:
        return 3;
    case bafx::config::FramePacing::Unlimited:
        return 4;
    }
    return -1;
}

[[nodiscard]] std::optional<bafx::config::FramePacing> selectedFramePacing(
    const HWND comboBox) noexcept
{
    if (comboBox == nullptr)
    {
        return std::nullopt;
    }

    switch (SendMessageW(comboBox, CB_GETCURSEL, 0U, 0))
    {
    case 0:
        return bafx::config::FramePacing::MatchDisplay;
    case 1:
        return bafx::config::FramePacing::Fixed60;
    case 2:
        return bafx::config::FramePacing::Fixed120;
    case 3:
        return bafx::config::FramePacing::Fixed144;
    case 4:
        return bafx::config::FramePacing::Unlimited;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] std::wstring framePacingText(
    const bafx::config::FramePacing pacing)
{
    switch (pacing)
    {
    case bafx::config::FramePacing::MatchDisplay:
        return tr(TextId::MatchDisplay);
    case bafx::config::FramePacing::Fixed60:
        return tr(TextId::Fixed60);
    case bafx::config::FramePacing::Fixed120:
        return tr(TextId::Fixed120);
    case bafx::config::FramePacing::Fixed144:
        return tr(TextId::Fixed144);
    case bafx::config::FramePacing::Unlimited:
        return tr(TextId::UnlimitedFps);
    }
    return tr(TextId::Unknown);
}

void initializeFramePacingCombo(const HWND comboBox) noexcept
{
    if (comboBox == nullptr)
    {
        return;
    }

    const std::array labels{
        tr(TextId::MatchDisplay),
        tr(TextId::Fixed60),
        tr(TextId::Fixed120),
        tr(TextId::Fixed144),
        tr(TextId::UnlimitedFps)};
    for (const wchar_t* label : labels)
    {
        static_cast<void>(SendMessageW(
            comboBox,
            CB_ADDSTRING,
            0U,
            reinterpret_cast<LPARAM>(label)));
    }
    static_cast<void>(SendMessageW(
        comboBox,
        CB_SETMINVISIBLE,
        labels.size(),
        0));
}

[[nodiscard]] std::string displaySessionIdentity(
    const DisplaySessionState& session)
{
    if (session.displayKey.has_value())
    {
        return "stable:" + *session.displayKey;
    }

    // A transient identity keeps the same row selected while the Host state
    // refreshes. It must never be used as a persisted configuration key.
    return "transient:" + session.device + '\n' + session.monitor;
}

[[nodiscard]] std::string offlineDisplayIdentity(
    const bafx::config::DisplayOverrideConfig& overrideConfig)
{
    return "stable:" + overrideConfig.displayKey;
}

[[nodiscard]] std::wstring driverStateText(
    const DisplayDriverState driver)
{
    switch (driver)
    {
    case DisplayDriverState::Hardware:
        return tr(TextId::Hardware);
    case DisplayDriverState::Warp:
        return tr(TextId::Warp);
    case DisplayDriverState::Unknown:
        return tr(TextId::Unknown);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring outputStateText(
    const DisplayOutputState output)
{
    switch (output)
    {
    case DisplayOutputState::ConservativeSdr:
        return tr(TextId::ConservativeSdr);
    case DisplayOutputState::LinearScRgb:
        return tr(TextId::LinearScrgb);
    case DisplayOutputState::Unknown:
        return tr(TextId::Unknown);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring colorStateText(const DisplayColorState color)
{
    switch (color)
    {
    case DisplayColorState::Sdr:
        return L"SDR";
    case DisplayColorState::WideColorGamut:
        return tr(TextId::WideGamut);
    case DisplayColorState::Hdr:
        return L"HDR";
    case DisplayColorState::Unknown:
        return tr(TextId::Unknown);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring optionalBooleanText(
    const std::optional<bool> value,
    const std::wstring_view trueText,
    const std::wstring_view falseText)
{
    if (!value.has_value())
    {
        return tr(TextId::Unknown);
    }
    return std::wstring(*value ? trueText : falseText);
}

[[nodiscard]] std::wstring refreshRateText(
    const std::optional<DisplayRefreshState>& refresh)
{
    if (!refresh.has_value()
        || refresh->numerator == 0U
        || refresh->denominator == 0U)
    {
        return tr(TextId::Unknown);
    }

    const double hertz = static_cast<double>(refresh->numerator)
        / static_cast<double>(refresh->denominator);
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(2) << hertz << L" Hz";
    return stream.str();
}

[[nodiscard]] std::wstring topologyStateText(
    const DisplayTopologyState state)
{
    switch (state)
    {
    case DisplayTopologyState::Complete:
        return tr(TextId::Complete);
    case DisplayTopologyState::Incomplete:
        return tr(TextId::Incomplete);
    case DisplayTopologyState::NoActiveDisplays:
        return tr(TextId::NoActiveDisplays);
    case DisplayTopologyState::QueryFailed:
        return tr(TextId::QueryFailed);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring colorMonitorStateText(
    const DisplayColorMonitorState state)
{
    switch (state)
    {
    case DisplayColorMonitorState::Active:
        return tr(TextId::Active);
    case DisplayColorMonitorState::InvalidTarget:
        return tr(TextId::InvalidTarget);
    case DisplayColorMonitorState::Unsupported:
        return tr(TextId::UnsupportedSystem);
    case DisplayColorMonitorState::Failed:
        return tr(TextId::Failed);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring colorSnapshotStateText(
    const DisplayColorSnapshotState state)
{
    switch (state)
    {
    case DisplayColorSnapshotState::Fresh:
        return tr(TextId::LatestCompleteContract);
    case DisplayColorSnapshotState::RetainedTransaction:
        return tr(TextId::RetainedInTransaction);
    case DisplayColorSnapshotState::RetainedLastKnown:
        return tr(TextId::LastCompleteContract);
    case DisplayColorSnapshotState::Unavailable:
        return tr(TextId::Unavailable);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring cadenceFallbackText(
    const DisplayCadenceFallbackState state)
{
    switch (state)
    {
    case DisplayCadenceFallbackState::None:
        return tr(TextId::None);
    case DisplayCadenceFallbackState::NoPhysicalTargets:
        return tr(TextId::NoPhysicalTargets);
    case DisplayCadenceFallbackState::PhysicalTargetUnavailable:
        return tr(TextId::PhysicalTargetUnavailable);
    case DisplayCadenceFallbackState::DrrPhysicalRefreshRateUnavailable:
        return tr(TextId::DrrRateUnavailable);
    case DisplayCadenceFallbackState::InvalidEffectiveRefreshRate:
        return tr(TextId::InvalidEffectiveRate);
    case DisplayCadenceFallbackState::MixedCloneRefreshRates:
        return tr(TextId::CloneRateConflict);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring captureCadenceText(
    const DisplayCaptureCadenceState state)
{
    switch (state)
    {
    case DisplayCaptureCadenceState::Inactive:
        return tr(TextId::Inactive);
    case DisplayCaptureCadenceState::WrongMonitor:
        return tr(TextId::CaptureTargetMismatch);
    case DisplayCaptureCadenceState::TargetRate:
        return tr(TextId::TargetRefreshRate);
    case DisplayCaptureCadenceState::ConservativeFallback:
        return tr(TextId::ConservativeFallback);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring producerCadenceText(
    const DisplayProducerCadenceState state)
{
    switch (state)
    {
    case DisplayProducerCadenceState::NotRequested:
        return tr(TextId::NotRequested);
    case DisplayProducerCadenceState::Applied:
        return tr(TextId::Applied);
    case DisplayProducerCadenceState::InterfaceUnavailable:
        return tr(TextId::InterfaceUnavailable);
    case DisplayProducerCadenceState::Rejected:
        return tr(TextId::SystemDenied);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring outputMappingText(
    const DisplayOutputMappingState state)
{
    switch (state)
    {
    case DisplayOutputMappingState::ConservativeSdr:
        return tr(TextId::ConservativeSdr);
    case DisplayOutputMappingState::AdvancedColorScRgb:
        return L"Advanced Color scRGB";
    case DisplayOutputMappingState::HdrSceneReferredScRgb:
        return L"HDR scene-referred scRGB";
    case DisplayOutputMappingState::Unknown:
        return tr(TextId::Unknown);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring outputFallbackText(
    const DisplayOutputFallbackState state)
{
    switch (state)
    {
    case DisplayOutputFallbackState::None:
        return tr(TextId::None);
    case DisplayOutputFallbackState::ConservativeSdr:
        return tr(TextId::SdrFallback);
    }
    return tr(TextId::Unknown);
}

[[nodiscard]] std::wstring optionalHresultText(
    const std::optional<std::int32_t> result)
{
    if (!result.has_value())
    {
        return tr(TextId::NotQueried);
    }
    return hresultText(static_cast<HRESULT>(*result));
}

[[nodiscard]] std::wstring optionalNitsText(
    const std::optional<float> nits)
{
    if (!nits.has_value())
    {
        return tr(TextId::Unknown);
    }

    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(2) << *nits << L" nits";
    return stream.str();
}

[[nodiscard]] std::wstring activeFxRoiPathText(
    const ActiveFxRoiPathState path)
{
    switch (path)
    {
    case ActiveFxRoiPathState::Disabled:
        return tr(TextId::TurnedOff);
    case ActiveFxRoiPathState::Idle:
        return tr(TextId::Idle);
    case ActiveFxRoiPathState::FullScreen:
        return tr(TextId::FullScreenBloom);
    case ActiveFxRoiPathState::RoiWarmup:
        return tr(TextId::RoiWarmup);
    case ActiveFxRoiPathState::RoiPrefilter:
        return tr(TextId::RoiFirstLevel);
    case ActiveFxRoiPathState::RoiPyramid:
        return tr(TextId::RoiFullPyramid);
    case ActiveFxRoiPathState::Unavailable:
        return tr(TextId::Unavailable);
    }
    return tr(TextId::Unavailable);
}

[[nodiscard]] std::wstring activeFxRoiReasonText(
    const ActiveFxRoiReasonState reason)
{
    switch (reason)
    {
    case ActiveFxRoiReasonState::Disabled:
        return tr(TextId::SwitchOff);
    case ActiveFxRoiReasonState::NoContent:
        return tr(TextId::NoVisibleEffects);
    case ActiveFxRoiReasonState::BloomDisabled:
        return tr(TextId::BloomOff);
    case ActiveFxRoiReasonState::CoreMode:
        return tr(TextId::CoreMode);
    case ActiveFxRoiReasonState::BackgroundDifferentialBloom:
        return tr(TextId::DifferentialBloomFullScreen);
    case ActiveFxRoiReasonState::TouchesBoundary:
        return tr(TextId::EffectTouchesEdge);
    case ActiveFxRoiReasonState::AreaTooLarge:
        return tr(TextId::RoiTooLarge);
    case ActiveFxRoiReasonState::BenefitTooSmall:
        return tr(TextId::SmallExpectedBenefit);
    case ActiveFxRoiReasonState::Context1Unavailable:
        return tr(TextId::Context1Unavailable);
    case ActiveFxRoiReasonState::SharedTargetFullWrite:
        return tr(TextId::SharedTargetFullWrite);
    case ActiveFxRoiReasonState::Applied:
        return tr(TextId::Applied);
    case ActiveFxRoiReasonState::RendererFallback:
        return tr(TextId::RendererFallback);
    case ActiveFxRoiReasonState::Unavailable:
    case ActiveFxRoiReasonState::Count:
        return tr(TextId::DiagnosticsUnavailable);
    }
    return tr(TextId::DiagnosticsUnavailable);
}

[[nodiscard]] std::wstring activeFxRoiRectText(
    const std::optional<ActiveFxRoiRectState>& rect)
{
    if (!rect.has_value())
    {
        return tr(TextId::None);
    }
    std::wostringstream stream;
    stream << L"[" << rect->left << L", " << rect->top
           << L" - " << rect->right << L", " << rect->bottom << L"]";
    return stream.str();
}

[[nodiscard]] std::wstring activeFxRoiGpuValueText(
    const std::optional<double> value)
{
    if (!value.has_value())
    {
        return L"--";
    }
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(2) << *value;
    return stream.str();
}

void appendActiveFxRoiGpuStage(
    std::wostringstream& stream,
    const std::wstring_view label,
    const ActiveFxRoiGpuPercentileState& stage)
{
    stream << label << L" "
           << activeFxRoiGpuValueText(stage.p50Microseconds)
           << L"/"
           << activeFxRoiGpuValueText(stage.p95Microseconds);
}

[[nodiscard]] std::wstring activeFxRoiPixelRatioText(
    const std::uint64_t drawnPixels,
    const std::uint64_t fullPixels)
{
    if (fullPixels == 0U)
    {
        return L"--";
    }
    const double ratio = static_cast<double>(drawnPixels)
        * 100.0
        / static_cast<double>(fullPixels);
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(1) << ratio << L"%";
    return stream.str();
}

void appendActiveFxRoiStageDetails(
    std::wostringstream& details,
    const std::wstring_view label,
    const ActiveFxRoiStageState& stage)
{
    details << L"\r\n  " << label << tr(TextId::RoiPixelColumns)
            << stage.fullPixels << L"/"
            << stage.candidatePixels << L"/"
            << stage.drawnPixels << L"/"
            << stage.clearedPixels
            << tr(TextId::DrawnColumn)
            << activeFxRoiPixelRatioText(
                stage.drawnPixels,
                stage.fullPixels);
}

void appendActiveFxRoiPathDetails(
    std::wostringstream& details,
    const std::wstring_view label,
    const ActiveFxRoiPathRuntimeState& path)
{
    details << L"\r\n[" << label << L"] "
            << activeFxRoiPathText(path.actualPath)
            << L" | " << activeFxRoiReasonText(path.decisionReason)
            << tr(TextId::RoiCurrentColumns)
            << (path.requested ? tr(TextId::BooleanYes) : tr(TextId::BooleanNo)) << L"/"
            << (path.executed ? tr(TextId::BooleanYes) : tr(TextId::BooleanNo)) << L"/"
            << (path.eligible ? tr(TextId::BooleanYes) : tr(TextId::BooleanNo)) << L"/"
            << (path.warmup ? tr(TextId::BooleanYes) : tr(TextId::BooleanNo))
            << tr(TextId::RoiFrameColumns)
            << path.observedFrames << L"/"
            << path.requestedFrames << L"/"
            << path.eligibleFrames << L"/"
            << path.appliedFrames << L"/"
            << path.warmupFrames << L"/"
            << path.fallbackFrames
            << tr(TextId::BloomPixelColumns)
            << path.fullPixels << L"/"
            << path.candidatePixels << L"/"
            << path.drawnPixels << L"/"
            << path.clearedPixels
            << tr(TextId::DrawRatioColumn)
            << activeFxRoiPixelRatioText(
                path.drawnPixels,
                path.fullPixels);
    appendActiveFxRoiStageDetails(
        details,
        L"Prefilter",
        path.stages.prefilter);
    appendActiveFxRoiStageDetails(
        details,
        L"Downsample",
        path.stages.downsample);
    appendActiveFxRoiStageDetails(
        details,
        L"Upsample",
        path.stages.upsample);
    appendActiveFxRoiStageDetails(details, L"Resolve", path.stages.resolve);
    details << L"\r\nDirty " << activeFxRoiRectText(path.dirtyRect)
            << L" | Aligned " << activeFxRoiRectText(path.alignedRect)
            << L"\r\nGuard " << path.guardX << L" x " << path.guardY
            << L" | Phase " << path.phase
            << L"\r\nGPU us p50/p95：";
    appendActiveFxRoiGpuStage(details, L"Prefilter", path.gpu.prefilter);
    details << L" | ";
    appendActiveFxRoiGpuStage(details, L"Pyramid", path.gpu.pyramid);
    details << L" | ";
    appendActiveFxRoiGpuStage(
        details,
        L"Final",
        path.gpu.finalComposite);

    details << tr(TextId::ReasonCountsLabel);
    bool hasReason = false;
    for (std::size_t index = 0U;
         index < path.reasonCounts.size();
         ++index)
    {
        if (path.reasonCounts[index] == 0U)
        {
            continue;
        }
        if (hasReason)
        {
            details << L"；";
        }
        details << activeFxRoiReasonText(
            static_cast<ActiveFxRoiReasonState>(index))
                << L" " << path.reasonCounts[index];
        hasReason = true;
    }
    if (!hasReason)
    {
        details << tr(TextId::None);
    }
}

}

ControlCenterWindow::ControlCenterWindow(const HINSTANCE instance) noexcept
    : instance_(instance)
    , client_(controlCenterIpcOptions())
{
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
}

ControlCenterWindow::~ControlCenterWindow()
{
    if (window_ != nullptr)
    {
        KillTimer(window_, patchTimerId);
        KillTimer(window_, hostRetryTimerId);
        KillTimer(window_, hostShutdownTimerId);
        KillTimer(window_, updateCheckTimerId);
        KillTimer(window_, displayStateTimerId);
        DestroyWindow(window_);
        window_ = nullptr;
    }
    hostLifetimeMutex_.reset();
    destroyFonts();
}

bool ControlCenterWindow::create(
    const int showCommand,
    const bool startHostOnLaunch)
{
    languagePath_ = languagePreferencePath(executableDirectory());
    languagePreference_ = loadLanguagePreference(languagePath_);
    setUiLanguage(languagePreference_);
    if (updateChecker_ == nullptr)
    {
        try
        {
            updateChecker_ = std::make_unique<
                bafx::release_update::ReleaseUpdateChecker>(
                    bafx::release_update::ReleaseVersion{
                        .major = bafx::product::versionMajor,
                        .minor = bafx::product::versionMinor,
                        .patch = bafx::product::versionPatch},
                    bafx::release_update::makeWinHttpReleaseTransport());
        }
        catch (...)
        {
            // Update checking is optional. Allocation failure must not hide
            // Host lifecycle controls or local version information.
            updateChecker_.reset();
        }
    }

    if (!registerWindowClass())
    {
        return false;
    }

    // GetDpiForSystem() can return the virtualized 96-DPI value before this
    // process owns a top-level window. The desktop window reports the primary
    // monitor DPI that is needed for the initial centered window.
    dpi_ = GetDpiForWindow(GetDesktopWindow());
    if (dpi_ == 0U)
    {
        dpi_ = USER_DEFAULT_SCREEN_DPI;
    }
    const HMONITOR primaryMonitor = MonitorFromPoint(
        POINT{0, 0},
        MONITOR_DEFAULTTOPRIMARY);
    layoutMonitor_ = primaryMonitor;
    const RECT workArea = monitorWorkArea(primaryMonitor);
    layoutDpi_ = controlCenterLayoutDpi(
        maximumClientSize(workArea, dpi_),
        dpi_);
    RECT bounds{
        0,
        0,
        scale(defaultControlCenterClientWidth),
        scale(defaultControlCenterClientHeight)};
    if (AdjustWindowRectExForDpi(
            &bounds,
            controlCenterWindowStyle,
            FALSE,
            0U,
            dpi_) == FALSE)
    {
        lastError_ = GetLastError();
        return false;
    }

    const int workAreaWidth = static_cast<int>(workArea.right - workArea.left);
    const int workAreaHeight = static_cast<int>(workArea.bottom - workArea.top);
    const PixelSize windowSize = clampPixelSize(
        PixelSize{bounds.right - bounds.left, bounds.bottom - bounds.top},
        PixelSize{workAreaWidth, workAreaHeight});
    const int windowWidth = windowSize.width;
    const int windowHeight = windowSize.height;
    const int windowX = static_cast<int>(workArea.left) + (std::max)(
        0,
        (workAreaWidth - windowWidth) / 2);
    const int windowY = static_cast<int>(workArea.top) + (std::max)(
        0,
        (workAreaHeight - windowHeight) / 2);

    std::wstring windowCaption = L"BAFX Control Center ";
    windowCaption += utf8ToWide(bafx::product::version);

    // CW_USEDEFAULT makes Windows choose the size of an overlapped window and
    // discards our DPI-scaled dimensions. Explicit coordinates preserve the
    // client area that layoutControls() was designed for.
    window_ = CreateWindowExW(
        0U,
        controlCenterWindowClassName.data(),
        windowCaption.c_str(),
        controlCenterWindowStyle,
        windowX,
        windowY,
        windowWidth,
        windowHeight,
        nullptr,
        nullptr,
        instance_,
        this);
    if (window_ == nullptr)
    {
        lastError_ = GetLastError();
        return false;
    }

    dpi_ = GetDpiForWindow(window_);
    if (dpi_ == 0U)
    {
        dpi_ = USER_DEFAULT_SCREEN_DPI;
    }
    layoutMonitor_ = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
    layoutDpi_ = controlCenterLayoutDpi(
        maximumClientSize(
            monitorWorkArea(layoutMonitor_),
            dpi_),
        dpi_);
    createFonts();
    if (!createControls())
    {
        lastError_ = GetLastError();
        DestroyWindow(window_);
        window_ = nullptr;
        return false;
    }
    if (updateChecker_ == nullptr)
    {
        setText(
            latestVersionText_,
            TextId::LatestCheckerUnavailable);
        EnableWindow(checkForUpdatesButton_, FALSE);
        EnableWindow(openReleaseButton_, FALSE);
    }

    RECT client{};
    if (GetClientRect(window_, &client) != FALSE)
    {
        layoutControls(client.right, client.bottom);
    }
    updateControls(HostState{}, loadStartupConfig(executableDirectory()));
    hostRunning_ = hostMutexPresent();
    setConnected(false);
    setText(statusText_, TextId::ConnectingHost);
    updateHostLifecycleButton();
    ShowWindow(window_, showCommand == 0 ? SW_SHOWNORMAL : showCommand);
    UpdateWindow(window_);

    if (!refreshFromHost())
    {
        if (startHostOnLaunch && !hostRunning_)
        {
            // The Run entry launches the Control Center so packaged and
            // portable activation continue to share one Host startup path.
            startHostFromBundle();
        }
        else if (!hostVersionBlocked_)
        {
            scheduleHostRefreshRetry();
        }
    }
    else
    {
        updateHostLifecycleButton();
    }
    return true;
}

int ControlCenterWindow::runMessageLoop() noexcept
{
    MSG message{};
    while (true)
    {
        const BOOL result = GetMessageW(&message, nullptr, 0U, 0U);
        if (result == 0)
        {
            return static_cast<int>(message.wParam);
        }
        if (result < 0)
        {
            return 1;
        }
        if (captureHotkeyMessage(message))
        {
            continue;
        }
        // Modeless native pages need dialog navigation explicitly. Recording
        // consumes its keys first so Enter/Tab/Escape remain recordable.
        if (IsDialogMessageW(window_, &message))
        {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

DWORD ControlCenterWindow::lastError() const noexcept
{
    return lastError_;
}

LRESULT CALLBACK ControlCenterWindow::windowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) noexcept
{
    ControlCenterWindow* self = reinterpret_cast<ControlCenterWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<ControlCenterWindow*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
    }

    if (self == nullptr)
    {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    try
    {
        const LRESULT result = self->handleMessage(message, wParam, lParam);
        if (message == WM_NCDESTROY)
        {
            // The HWND remains valid through default non-client teardown.
            // Clear the object backlink only after that final message returns.
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            self->window_ = nullptr;
        }
        return result;
    }
    catch (...)
    {
        self->setError(TextId::WindowMessageError);
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

LRESULT ControlCenterWindow::handleMessage(
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam)
{
    if (taskbarCreatedMessage_ != 0U && message == taskbarCreatedMessage_)
    {
        // Explorer owns the notification area and drops all icons when it
        // restarts. Re-add ours only when the current configuration owns it.
        trayIconAdded_ = false;
        if (config_.system.closeToTray)
        {
            static_cast<void>(ensureTrayIcon());
        }
        return 0;
    }

    switch (message)
    {
    case WM_COMMAND:
        onCommand(LOWORD(wParam), HIWORD(wParam));
        return 0;
    case WM_DRAWITEM:
    {
        const auto* drawItem = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (drawItem == nullptr
            || drawItem->CtlID != static_cast<UINT>(ControlId::ThemeColorPreview))
        {
            return 0;
        }
        const COLORREF color = themeColorRef(config_.effects.themeColor);
        HBRUSH brush = CreateSolidBrush(color);
        if (brush != nullptr)
        {
            FillRect(drawItem->hDC, &drawItem->rcItem, brush);
            DeleteObject(brush);
        }
        FrameRect(
            drawItem->hDC,
            &drawItem->rcItem,
            static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        if ((drawItem->itemState & ODS_FOCUS) != 0U)
        {
            DrawFocusRect(drawItem->hDC, &drawItem->rcItem);
        }
        return TRUE;
    }
    case WM_HSCROLL:
        onSliderChanged(reinterpret_cast<HWND>(lParam));
        return 0;
    case WM_TIMER:
        onTimer(static_cast<UINT_PTR>(wParam));
        return 0;
    case trayNotificationMessage:
        if (wParam != trayIconIdentifier)
        {
            return 0;
        }
        if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK)
        {
            restoreFromTray();
        }
        else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU)
        {
            showTrayMenu();
        }
        return 0;
    case WM_WINDOWPOSCHANGING:
    {
        auto* position = reinterpret_cast<WINDOWPOS*>(lParam);
        if (position != nullptr && (position->flags & SWP_NOSIZE) == 0U)
        {
            // The top-level sizing transaction otherwise copies its old
            // client bitmap after child controls have moved. Discarding those
            // pixels lets the complete WM_SIZE redraw become authoritative.
            position->flags |= SWP_NOCOPYBITS;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }
    case WM_SIZE:
    {
        RECT client{};
        if (GetClientRect(window_, &client) != FALSE)
        {
            // GetClientRect avoids the 16-bit LPARAM truncation used by
            // LOWORD/HIWORD when a high-DPI window spans a large monitor.
            layoutControls(client.right - client.left, client.bottom - client.top);
        }
        return 0;
    }
    case WM_ENTERSIZEMOVE:
        interactiveMoveResize_ = true;
        return 0;
    case WM_EXITSIZEMOVE:
        interactiveMoveResize_ = false;
        adaptLayoutToMonitor(
            MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
            false);
        // Windows can retain the last backing surface until the modal sizing
        // loop returns. Repaint once more from the normal message loop so the
        // final child layout and parent background are committed together.
        if (PostMessageW(
                window_,
                redrawAfterInteractiveResizeMessage,
                0U,
                0) == FALSE)
        {
            redrawWindowTree();
        }
        return 0;
    case redrawAfterInteractiveResizeMessage:
        redrawWindowTree();
        return 0;
    case WM_MOVE:
        if (!interactiveMoveResize_)
        {
            adaptLayoutToMonitor(
                MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
                false);
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_DISPLAYCHANGE:
        adaptLayoutToMonitor(
            MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
            true);
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_SETTINGCHANGE:
        if (languagePreference_ == UiLanguage::System)
        {
            const auto previous = currentUiLanguage();
            setUiLanguage(languagePreference_);
            if (previous != currentUiLanguage())
            {
                retranslateUi();
            }
        }
        if (wParam == SPI_SETWORKAREA)
        {
            adaptLayoutToMonitor(
                MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
                true);
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_DPICHANGED:
    {
        dpi_ = HIWORD(wParam);
        if (dpi_ == 0U)
        {
            dpi_ = USER_DEFAULT_SCREEN_DPI;
        }
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested != nullptr)
        {
            const HMONITOR targetMonitor = MonitorFromRect(
                suggested,
                MONITOR_DEFAULTTONEAREST);
            layoutMonitor_ = targetMonitor;
            const RECT workArea = monitorWorkArea(targetMonitor);
            layoutDpi_ = controlCenterLayoutDpi(
                maximumClientSize(workArea, dpi_),
                dpi_);
            createFonts();

            RECT minimumBounds{
                0,
                0,
                scale(minimumControlCenterClientWidth),
                scale(minimumControlCenterClientHeight)};
            static_cast<void>(AdjustWindowRectExForDpi(
                &minimumBounds,
                static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE)),
                FALSE,
                static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_EXSTYLE)),
                dpi_));
            const PixelSize targetSize = clampPixelSize(
                PixelSize{
                    (std::max)(
                        suggested->right - suggested->left,
                        minimumBounds.right - minimumBounds.left),
                    (std::max)(
                        suggested->bottom - suggested->top,
                        minimumBounds.bottom - minimumBounds.top)},
                PixelSize{
                    static_cast<int>(workArea.right - workArea.left),
                    static_cast<int>(workArea.bottom - workArea.top)});
            const int targetX = (std::clamp)(
                static_cast<int>(suggested->left),
                static_cast<int>(workArea.left),
                static_cast<int>(workArea.right) - targetSize.width);
            const int targetY = (std::clamp)(
                static_cast<int>(suggested->top),
                static_cast<int>(workArea.top),
                static_cast<int>(workArea.bottom) - targetSize.height);
            SetWindowPos(
                window_,
                nullptr,
                targetX,
                targetY,
                targetSize.width,
                targetSize.height,
                SWP_NOACTIVATE | SWP_NOZORDER);
        }
        else
        {
            layoutMonitor_ = MonitorFromWindow(
                window_,
                MONITOR_DEFAULTTONEAREST);
            layoutDpi_ = controlCenterLayoutDpi(
                maximumClientSize(monitorWorkArea(layoutMonitor_), dpi_),
                dpi_);
            createFonts();
        }
        RECT client{};
        if (GetClientRect(window_, &client) != FALSE)
        {
            layoutControls(client.right - client.left, client.bottom - client.top);
        }
        return 0;
    }
    case WM_GETMINMAXINFO:
    {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        if (limits == nullptr)
        {
            return 0;
        }
        RECT minimumBounds{
            0,
            0,
            scale(minimumControlCenterClientWidth),
            scale(minimumControlCenterClientHeight)};
        if (AdjustWindowRectExForDpi(
                &minimumBounds,
                static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE)),
                FALSE,
                static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_EXSTYLE)),
                dpi_) != FALSE)
        {
            limits->ptMinTrackSize.x = minimumBounds.right - minimumBounds.left;
            limits->ptMinTrackSize.y = minimumBounds.bottom - minimumBounds.top;
        }
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0U) == SC_CLOSE
            && config_.system.closeToTray)
        {
            if (!prepareToClose())
            {
                return 0;
            }
            if (ensureTrayIcon())
            {
                ShowWindow(window_, SW_HIDE);
                return 0;
            }
            // Never leave the process running without a visible recovery
            // entry when Explorer rejects notification-area registration.
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_ACTIVATEAPP:
        if (wParam == FALSE)
        {
            endHotkeyCapture();
        }
        return 0;
    case WM_CLOSE:
        closeControlCenter();
        return 0;
    case WM_DESTROY:
        KillTimer(window_, hotkeyTimerId);
        if (themeColorEdit_ != nullptr)
        {
            RemovePropW(
                themeColorEdit_,
                themeColorEditOriginalProcedureProperty);
        }
        KillTimer(window_, patchTimerId);
        KillTimer(window_, hostRetryTimerId);
        KillTimer(window_, hostShutdownTimerId);
        KillTimer(window_, updateCheckTimerId);
        KillTimer(window_, displayStateTimerId);
        if (updateChecker_ != nullptr)
        {
            // WinHTTP cancellation is part of the window lifetime contract;
            // join before the HWND and its controls become invalid.
            updateChecker_->cancel();
        }
        removeTrayIcon();
        hostLifetimeMutex_.reset();
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        return DefWindowProcW(window_, message, wParam, lParam);
    default:
        return DefWindowProcW(window_, message, wParam, lParam);
    }
}

bool ControlCenterWindow::registerWindowClass() noexcept
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance_;
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = controlCenterWindowClassName.data();
    windowClass.hIconSm = windowClass.hIcon;

    if (RegisterClassExW(&windowClass) != 0U)
    {
        return true;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_CLASS_ALREADY_EXISTS)
    {
        return true;
    }
    lastError_ = error;
    return false;
}

bool ControlCenterWindow::createControls()
{
    const std::wstring productVersion = utf8ToWide(bafx::product::version);
    const InstallationStatePresentation installationState =
        installationStatePresentation(executableDirectory());
    const std::wstring pageTitle = L"BAFX Desktop " + productVersion
        + L" · " + installationState.titleLabel;
    titleText_ = createChild(
        L"STATIC",
        pageTitle.c_str(),
        SS_LEFT | SS_NOPREFIX);
    statusText_ = createChild(
        L"STATIC",
        TextId::ConnectingHost,
        SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS);
    messageText_ = createChild(
        L"EDIT",
        L"",
        ES_LEFT | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL);
    basicPageButton_ = createChild(
        L"BUTTON",
        TextId::BasicPage,
        BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
        ControlId::BasicPage);
    advancedPageButton_ = createChild(
        L"BUTTON",
        TextId::AdvancedPage,
        BS_AUTORADIOBUTTON | WS_TABSTOP,
        ControlId::AdvancedPage);
    displayPageButton_ = createChild(
        L"BUTTON",
        TextId::DisplayPage,
        BS_AUTORADIOBUTTON | WS_TABSTOP,
        ControlId::DisplayPage);
    hotkeysPageButton_ = createChild(L"BUTTON", TextId::HotkeysPage,
        BS_AUTORADIOBUTTON | WS_TABSTOP, ControlId::HotkeysPage);
    systemPageButton_ = createChild(
        L"BUTTON",
        TextId::SystemPage,
        BS_AUTORADIOBUTTON | WS_TABSTOP,
        ControlId::SystemPage);
    advancedTimingSectionButton_ = createChild(
        L"BUTTON",
        TextId::TimingSection,
        BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
        ControlId::AdvancedTimingSection);
    advancedParticlesSectionButton_ = createChild(
        L"BUTTON",
        TextId::ParticlesSection,
        BS_AUTORADIOBUTTON | WS_TABSTOP,
        ControlId::AdvancedParticlesSection);
    advancedRingsSectionButton_ = createChild(
        L"BUTTON",
        TextId::RingsSection,
        BS_AUTORADIOBUTTON | WS_TABSTOP,
        ControlId::AdvancedRingsSection);
    advancedClickShardsSectionButton_ = createChild(
        L"BUTTON",
        TextId::ClickShardsSection,
        BS_AUTORADIOBUTTON | WS_TABSTOP,
        ControlId::AdvancedClickShardsSection);
    advancedBloomSectionButton_ = createChild(
        L"BUTTON",
        TextId::BloomSection,
        BS_AUTORADIOBUTTON | WS_TABSTOP,
        ControlId::AdvancedBloomSection);
    advancedLayersSectionButton_ = createChild(
        L"BUTTON",
        TextId::LayersSection,
        BS_AUTORADIOBUTTON | WS_TABSTOP,
        ControlId::AdvancedLayersSection);
    effectsHeading_ = createChild(
        L"BUTTON",
        TextId::EffectsHeading,
        BS_GROUPBOX);

    effectsModeLabel_ = createChild(
        L"STATIC",
        TextId::EffectsModeLabel,
        SS_LEFT | SS_NOPREFIX);
    effectsMode_ = createChild(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
        ControlId::EffectsMode);
    if (effectsMode_ != nullptr)
    {
        static_cast<void>(SendMessageW(
            effectsMode_,
            CB_ADDSTRING,
            0U,
            reinterpret_cast<LPARAM>(tr(TextId::FullEffects))));
        static_cast<void>(SendMessageW(
            effectsMode_,
            CB_ADDSTRING,
            0U,
            reinterpret_cast<LPARAM>(tr(TextId::CoreEffects))));
        static_cast<void>(SendMessageW(effectsMode_, CB_SETMINVISIBLE, 2U, 0));
    }

    effectsEnabled_ = createChild(
        L"BUTTON",
        TextId::EnableEffects,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::EffectsEnabled);
    clickEnabled_ = createChild(
        L"BUTTON",
        TextId::ClickEffects,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::ClickEnabled);
    trailEnabled_ = createChild(
        L"BUTTON",
        TextId::MouseTrail,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::TrailEnabled);
    trailAlwaysOn_ = createChild(
        L"BUTTON",
        TextId::AlwaysOnTrail,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::TrailAlwaysOn);
    leftClickEnabled_ = createChild(
        L"BUTTON",
        TextId::LeftClick,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::LeftClickEnabled);
    rightClickEnabled_ = createChild(
        L"BUTTON",
        TextId::RightClick,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::RightClickEnabled);
    middleClickEnabled_ = createChild(
        L"BUTTON",
        TextId::MiddleClick,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::MiddleClickEnabled);

    const bool slidersCreated = createSlider(
        globalScale_,
        TextId::EffectScale,
        0.1,
        4.0,
        0.05,
        "effects.globalScale",
        ControlId::GlobalScale)
        && createSlider(
            trailLength_,
            TextId::TrailLength,
            0.0,
            10000.0 / 300.0,
            0.05,
            "effects.trailLength",
            ControlId::TrailLength)
        && createSlider(
            trailWidth_,
            TextId::TrailWidth,
            0.1,
            4.0,
            0.05,
            "effects.trailWidth",
            ControlId::TrailWidth)
        && createSlider(
            inputSamplingRate_,
            TextId::SamplingRate,
            0.0,
            1000.0,
            1.0,
            "input.samplingRateHz",
            ControlId::InputSamplingRate)
        && createSlider(
            bloomIntensity_,
            TextId::BloomIntensity,
            0.0,
            10.0,
            0.05,
            "effects.bloomIntensity",
            ControlId::BloomIntensity);

    const bool advancedSlidersCreated = createSlider(
        opacity_,
        TextId::Opacity,
        0.0,
        1.0,
        0.01,
        "effects.opacity",
        ControlId::Opacity)
        && createSlider(
            clickTimeScale_,
            TextId::ClickSpeed,
            0.01,
            4.0,
            0.01,
            "effects.clickTimeScale",
            ControlId::ClickTimeScale)
        && createSlider(
            trailTimeScale_,
            TextId::TrailSpeed,
            0.01,
            4.0,
            0.01,
            "effects.trailTimeScale",
            ControlId::TrailTimeScale)
        && createSlider(
            trailLifetimeMs_,
            TextId::TrailLifetime,
            0.0,
            10000.0,
            1.0,
            "effects.trailLifetimeMs",
            ControlId::TrailLifetimeMs)
        && createSlider(
            bloomDiffusion_,
            TextId::BloomDiffusion,
            0.0,
            10.0,
            0.01,
            "effects.bloomDiffusion",
            ControlId::BloomDiffusion)
        && createSlider(
            bloomThreshold_,
            TextId::BloomThreshold,
            0.0,
            64.0,
            0.01,
            "effects.bloomThreshold",
            ControlId::BloomThreshold)
        && createSlider(
            bloomSoftKnee_,
            TextId::BloomSoftKnee,
            0.0,
            1.0,
            0.01,
            "effects.bloomSoftKnee",
            ControlId::BloomSoftKnee)
        && createSlider(
            bloomClamp_,
            TextId::BloomClamp,
            0.0,
            65504.0,
            1.0,
            "effects.bloomClamp",
            ControlId::BloomClamp);

    const bool particleSlidersCreated = createSlider(
        diskRadius_,
        TextId::DiskRadius,
        20.0,
        120.0,
        0.01,
        "effects.diskRadius",
        ControlId::DiskRadius)
        && createSlider(
            diskLifetimeMs_,
            TextId::DiskLifetime,
            50.0,
            500.0,
            1.0,
            "effects.diskLifetimeMs",
            ControlId::DiskLifetimeMs)
        && createSlider(
            ringsHdrIntensity_,
            TextId::RingsHdrIntensity,
            0.0,
            8.0,
            0.01,
            "effects.ringsHdrIntensity",
            ControlId::RingsHdrIntensity)
        && createSlider(
            shardsHdrIntensity_,
            TextId::ShardsHdrIntensity,
            0.0,
            8.0,
            0.01,
            "effects.shardsHdrIntensity",
            ControlId::ShardsHdrIntensity)
        && createSlider(
            trailOpacity_,
            TextId::TrailOpacity,
            0.0,
            1.0,
            0.01,
            "effects.trailOpacity",
            ControlId::TrailOpacity);

    const bool ringSlidersCreated = createSlider(
        ringsCount_,
        TextId::RingCount,
        0.0,
        6.0,
        1.0,
        "effects.ringsCount",
        ControlId::RingsCount)
        && createSlider(
            ringsLifetimeMs_,
            TextId::RingLifetime,
            50.0,
            2000.0,
            1.0,
            "effects.ringsLifetimeMs",
            ControlId::RingsLifetimeMs)
        && createSlider(
            ringsRadiusMin_,
            TextId::RingRadiusMin,
            20.0,
            120.0,
            0.01,
            "effects.ringsRadiusMin",
            ControlId::RingsRadiusMin)
        && createSlider(
            ringsRadiusMax_,
            TextId::RingRadiusMax,
            20.0,
            120.0,
            0.01,
            "effects.ringsRadiusMax",
            ControlId::RingsRadiusMax)
        && createSlider(
            ringsAngularVelocityMultiplier_,
            TextId::RingAngularVelocity,
            1.0,
            30.0,
            0.01,
            "effects.ringsAngularVelocityMultiplier",
            ControlId::RingsAngularVelocityMultiplier)
        && createSlider(
            ringsRotationDirection_,
            TextId::RingDirection,
            -1.0,
            1.0,
            2.0,
            "effects.ringsRotationDirection",
            ControlId::RingsRotationDirection);

    const bool clickShardSlidersCreated = createSlider(
        shardsClickCount_,
        TextId::ClickShardCount,
        0.0,
        12.0,
        1.0,
        "effects.shardsClickCount",
        ControlId::ShardsClickCount)
        && createSlider(
            shardsClickLifetimeMinMs_,
            TextId::LifetimeMin,
            100.0,
            1000.0,
            1.0,
            "effects.shardsClickLifetimeMinMs",
            ControlId::ShardsClickLifetimeMinMs)
        && createSlider(
            shardsClickLifetimeMaxMs_,
            TextId::LifetimeMax,
            100.0,
            1000.0,
            1.0,
            "effects.shardsClickLifetimeMaxMs",
            ControlId::ShardsClickLifetimeMaxMs)
        && createSlider(
            shardsClickRadius_,
            TextId::SpawnRadius,
            0.0,
            200.0,
            0.01,
            "effects.shardsClickRadius",
            ControlId::ShardsClickRadius)
        && createSlider(
            shardsClickSpeedMin_,
            TextId::SpeedMin,
            0.0,
            200.0,
            0.01,
            "effects.shardsClickSpeedMin",
            ControlId::ShardsClickSpeedMin)
        && createSlider(
            shardsClickSpeedMax_,
            TextId::SpeedMax,
            0.0,
            200.0,
            0.01,
            "effects.shardsClickSpeedMax",
            ControlId::ShardsClickSpeedMax)
        && createSlider(
            shardsSizeMin_,
            TextId::ShardSizeMin,
            0.0,
            100.0,
            0.01,
            "effects.shardsSizeMin",
            ControlId::ShardsSizeMin)
        && createSlider(
            shardsSizeMax_,
            TextId::ShardSizeMax,
            0.0,
            100.0,
            0.01,
            "effects.shardsSizeMax",
            ControlId::ShardsSizeMax);

    themeColorLabel_ = createChild(
        L"STATIC",
        TextId::ThemeColor,
        SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX);
    themeColorEdit_ = createChild(
        L"EDIT",
        L"#4ca7ff",
        ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
        ControlId::ThemeColorEdit);
    themeColorPreview_ = createChild(
        L"STATIC",
        L"",
        SS_OWNERDRAW | WS_TABSTOP,
        ControlId::ThemeColorPreview);
    themeColorChoose_ = createChild(
        L"BUTTON",
        TextId::ChooseThemeColor,
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::ThemeColorChoose);
    if (themeColorEdit_ != nullptr)
    {
        const WNDPROC original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            themeColorEdit_,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(themeColorEditProcedure)));
        static_cast<void>(SetPropW(
            themeColorEdit_,
            themeColorEditOriginalProcedureProperty,
            reinterpret_cast<HANDLE>(original)));
    }

    advancedTimingHeading_ = createChild(
        L"BUTTON",
        TextId::TimingSection,
        BS_GROUPBOX);
    advancedParticlesHeading_ = createChild(
        L"BUTTON",
        TextId::ParticlesSection,
        BS_GROUPBOX);
    advancedRingsHeading_ = createChild(
        L"BUTTON",
        TextId::RingsSection,
        BS_GROUPBOX);
    advancedClickShardsHeading_ = createChild(
        L"BUTTON",
        TextId::ClickShardsSection,
        BS_GROUPBOX);
    advancedBloomHeading_ = createChild(
        L"BUTTON",
        TextId::BloomSection,
        BS_GROUPBOX);
    advancedLayersHeading_ = createChild(
        L"BUTTON",
        TextId::EffectLayers,
        BS_GROUPBOX);
    diskLayerEnabled_ = createChild(
        L"BUTTON",
        TextId::CenterDisk,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::DiskLayerEnabled);
    ringsLayerEnabled_ = createChild(
        L"BUTTON",
        TextId::Rings,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::RingsLayerEnabled);
    clickShardsLayerEnabled_ = createChild(
        L"BUTTON",
        TextId::ClickShardsSection,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::ClickShardsLayerEnabled);
    trailShardsLayerEnabled_ = createChild(
        L"BUTTON",
        TextId::TrailShards,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::TrailShardsLayerEnabled);
    trailLayerEnabled_ = createChild(
        L"BUTTON",
        TextId::TrailLine,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::TrailLayerEnabled);
    bloomLayerEnabled_ = createChild(
        L"BUTTON",
        L"Bloom",
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::BloomLayerEnabled);

    bloomQualityLabel_ = createChild(
        L"STATIC",
        TextId::BloomSpread,
        SS_LEFT | SS_NOPREFIX);
    bloomQuality_ = createChild(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
        ControlId::BloomQuality);
    if (bloomQuality_ != nullptr)
    {
        static_cast<void>(SendMessageW(bloomQuality_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(tr(TextId::Compact))));
        static_cast<void>(SendMessageW(bloomQuality_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(tr(TextId::Moderate))));
        static_cast<void>(SendMessageW(bloomQuality_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(tr(TextId::Original))));
        static_cast<void>(SendMessageW(bloomQuality_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(tr(TextId::ExtraWide))));
        static_cast<void>(SendMessageW(bloomQuality_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(tr(TextId::Custom))));
        static_cast<void>(SendMessageW(bloomQuality_, CB_SETMINVISIBLE, 5U, 0));
    }

    backgroundHeading_ = createChild(
        L"BUTTON",
        TextId::BackgroundAndProfiles,
        BS_GROUPBOX);
    backgroundModeLabel_ = createChild(
        L"STATIC",
        TextId::RenderMode,
        SS_LEFT | SS_NOPREFIX);
    backgroundMode_ = createChild(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
        ControlId::BackgroundMode);
    if (backgroundMode_ != nullptr)
    {
        static_cast<void>(SendMessageW(backgroundMode_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(tr(TextId::BackgroundAware))));
        static_cast<void>(SendMessageW(backgroundMode_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(tr(TextId::RecordingCompatible))));
        static_cast<void>(SendMessageW(backgroundMode_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(tr(TextId::LightBackground))));
        static_cast<void>(SendMessageW(backgroundMode_, CB_SETMINVISIBLE, 3U, 0));
    }

    cursorExcluded_ = createChild(
        L"BUTTON",
        TextId::ExcludeCursor,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::CursorExcluded);
    allowSystemBorder_ = createChild(
        L"BUTTON",
        TextId::AllowCaptureBorder,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::AllowSystemBorder);
    idleOptimization_ = createChild(
        L"BUTTON",
        TextId::IdleOptimization,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::IdleOptimization);
    systemSettingsHeading_ = createChild(
        L"BUTTON",
        TextId::SystemBehavior,
        BS_GROUPBOX);
    languageLabel_ = createChild(L"STATIC", TextId::Language, SS_LEFT | SS_NOPREFIX);
    languageSelector_ = createChild(WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_TABSTOP, ControlId::Language);
    // Autonyms remain recognizable even when the current interface is unfamiliar.
    for (const auto* name : {L"跟随系统 / System", L"简体中文", L"English"})
    {
        SendMessageW(languageSelector_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(name));
    }
    SendMessageW(languageSelector_, CB_SETCURSEL, static_cast<WPARAM>(languagePreference_), 0);
    startWithWindows_ = createChild(
        L"BUTTON",
        TextId::StartWithWindows,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::StartWithWindows);
    startMinimized_ = createChild(
        L"BUTTON",
        TextId::StartMinimized,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::StartMinimized);
    closeToTray_ = createChild(
        L"BUTTON",
        TextId::CloseToTray,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::CloseToTray);
#if defined(BAFX_ENABLE_SPOUT2)
    spout2Enabled_ = createChild(
        L"BUTTON",
        TextId::EnableSpout,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::Spout2Enabled);
    spout2SenderStatus_ = createChild(
        L"EDIT",
        TextId::SenderDisconnected,
        ES_MULTILINE | ES_READONLY | WS_VSCROLL);
    obsSpoutPluginStatus_ = createChild(
        L"EDIT",
        TextId::ObsNotChecked,
        ES_MULTILINE | ES_READONLY | WS_VSCROLL);
    spout2ObsHint_ = createChild(
        L"STATIC",
        TextId::ObsHint,
        SS_LEFT | SS_NOPREFIX);
    refreshObsSpoutPluginButton_ = createChild(
        L"BUTTON",
        TextId::CheckObsPlugin,
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::RefreshObsSpoutPlugin);
    openObsSpoutPluginPageButton_ = createChild(
        L"BUTTON",
        TextId::OpenPluginPage,
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::OpenObsSpoutPluginPage);
#endif
    clearLogsButton_ = createChild(
        L"BUTTON",
        TextId::ClearLogs,
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::ClearLogs);
    versionUpdateHeading_ = createChild(
        L"BUTTON",
        TextId::VersionMaintenance,
        BS_GROUPBOX);
    const std::wstring controlCenterVersion =
        tr(TextId::ControlCenterVersionLabel) + productVersion;
    controlCenterVersionText_ = createChild(
        L"STATIC",
        controlCenterVersion.c_str(),
        SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS);
    hostVersionText_ = createChild(
        L"STATIC",
        TextId::HostVersionPending,
        SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS);
    installStateText_ = createChild(
        L"EDIT",
        installationState.details.c_str(),
        ES_LEFT | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL);
    latestVersionText_ = createChild(
        L"STATIC",
        TextId::LatestNotChecked,
        SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS);
    // Native child creation order is also the dialog Tab order. Keep these
    // actions adjacent so the update controller can wire them without moving
    // keyboard focus semantics.
    checkForUpdatesButton_ = createChild(
        L"BUTTON",
        TextId::CheckUpdates,
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::CheckForUpdates);
    openReleaseButton_ = createChild(
        L"BUTTON",
        TextId::OpenRelease,
        BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED,
        ControlId::OpenRelease);
    repositoryStarHint_ = createChild(
        L"STATIC",
        TextId::StarHint,
        SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS);
    openRepositoryButton_ = createChild(
        L"BUTTON",
        TextId::OpenRepository,
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::OpenRepository);

    displaySettingsHeading_ = createChild(
        L"BUTTON",
        TextId::DisplayPage,
        BS_GROUPBOX);
    displaySelectorLabel_ = createChild(
        L"STATIC",
        TextId::DisplaySelectorLabel,
        SS_LEFT | SS_NOPREFIX);
    displaySelector_ = createChild(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
        ControlId::DisplaySelector);
    if (displaySelector_ != nullptr)
    {
        static_cast<void>(SendMessageW(
            displaySelector_,
            CB_SETMINVISIBLE,
            8U,
            0));
    }
    displaySummaryText_ = createChild(
        L"STATIC",
        TextId::DisplaysNotLoaded,
        SS_LEFT | SS_NOPREFIX);
    hdrEnabled_ = createChild(
        L"BUTTON",
        TextId::RequestHdr,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::HdrEnabled);
    activeFxRoiEnabled_ = createChild(
        L"BUTTON",
        TextId::EnableRoi,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::ActiveFxRoiEnabled);
    framePacingLabel_ = createChild(
        L"STATIC",
        TextId::GlobalFramePacing,
        SS_LEFT | SS_NOPREFIX);
    framePacing_ = createChild(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
        ControlId::FramePacing);
    initializeFramePacingCombo(framePacing_);
    displayIndependent_ = createChild(
        L"BUTTON",
        TextId::IndependentSettings,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::DisplayIndependent);
    displayEffectsEnabled_ = createChild(
        L"BUTTON",
        TextId::DisplayEffects,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::DisplayEffectsEnabled);
    displayHdrEnabled_ = createChild(
        L"BUTTON",
        TextId::DisplayHdr,
        BS_AUTOCHECKBOX | WS_TABSTOP,
        ControlId::DisplayHdrEnabled);
    displayFramePacingLabel_ = createChild(
        L"STATIC",
        TextId::DisplayFramePacing,
        SS_LEFT | SS_NOPREFIX);
    displayFramePacing_ = createChild(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
        ControlId::DisplayFramePacing);
    initializeFramePacingCombo(displayFramePacing_);
    displayDetailsHeading_ = createChild(
        L"BUTTON",
        TextId::DisplayDetails,
        BS_GROUPBOX);
    displayDetailsText_ = createChild(
        L"EDIT",
        TextId::DisplayConnectHint,
        ES_LEFT
            | ES_MULTILINE
            | ES_READONLY
            | ES_AUTOVSCROLL
            | WS_BORDER
            | WS_VSCROLL
            | WS_TABSTOP);
    activeFxRoiDetailsHeading_ = createChild(
        L"BUTTON",
        TextId::RoiPanel,
        BS_GROUPBOX);
    activeFxRoiDetailsText_ = createChild(
        L"EDIT",
        TextId::RoiConnectHint,
        ES_LEFT
            | ES_MULTILINE
            | ES_READONLY
            | ES_AUTOVSCROLL
            | WS_BORDER
            | WS_VSCROLL
            | WS_TABSTOP);
    pauseButton_ = createChild(
        L"BUTTON",
        TextId::PauseEffects,
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::Pause);
    refreshButton_ = createChild(
        L"BUTTON",
        TextId::RefreshState,
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::Refresh);
    hostLifecycleButton_ = createChild(
        L"BUTTON",
        TextId::StartHost,
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::HostLifecycle);
    resetDefaultsButton_ = createChild(
        L"BUTTON",
        TextId::ResetDefaults,
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::ResetDefaults);
    // Create Profile controls after the action buttons so native dialog Tab
    // order follows the Basic page's visual top-to-bottom order.
    fxProfileLabel_ = createChild(
        L"STATIC",
        TextId::FxProfiles,
        SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX);
    fxProfileSelector_ = createChild(
        WC_COMBOBOXW,
        L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
        ControlId::FxProfileSelector);
    if (fxProfileSelector_ != nullptr)
    {
        static_cast<void>(SendMessageW(
            fxProfileSelector_,
            CB_SETMINVISIBLE,
            8U,
            0));
    }
    fxProfileNameEdit_ = createChild(
        L"EDIT",
        L"",
        ES_LEFT | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
        ControlId::FxProfileName);
    if (fxProfileNameEdit_ != nullptr)
    {
        // The Host contract accepts at most 40 UTF-16 code units. Limiting the
        // native edit prevents names that can never be persisted.
        static_cast<void>(SendMessageW(
            fxProfileNameEdit_,
            EM_SETLIMITTEXT,
            40U,
            0));
        static_cast<void>(SendMessageW(
            fxProfileNameEdit_,
            EM_SETCUEBANNER,
            TRUE,
            reinterpret_cast<LPARAM>(tr(TextId::ProfileName))));
    }
    applyFxProfileButton_ = createChild(
        L"BUTTON",
        TextId::Apply,
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::ApplyFxProfile);
    saveFxProfileButton_ = createChild(
        L"BUTTON",
        TextId::SaveCurrent,
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::SaveFxProfile);
    deleteFxProfileButton_ = createChild(
        L"BUTTON",
        TextId::Delete,
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::DeleteFxProfile);

    if (!createHotkeyControls())
    {
        return false;
    }

    const std::array required{
        titleText_,
        statusText_,
        messageText_,
        basicPageButton_,
        advancedPageButton_,
        displayPageButton_,
        hotkeysPageButton_,
        systemPageButton_,
        effectsHeading_,
        effectsEnabled_,
        effectsModeLabel_,
        effectsMode_,
        clickEnabled_,
        trailEnabled_,
        diskLayerEnabled_,
        ringsLayerEnabled_,
        clickShardsLayerEnabled_,
        trailShardsLayerEnabled_,
        trailLayerEnabled_,
        bloomLayerEnabled_,
        trailAlwaysOn_,
        leftClickEnabled_,
        rightClickEnabled_,
        middleClickEnabled_,
        bloomQualityLabel_,
        bloomQuality_,
        backgroundHeading_,
        backgroundModeLabel_,
        backgroundMode_,
        cursorExcluded_,
        allowSystemBorder_,
        idleOptimization_,
        fxProfileLabel_,
        fxProfileSelector_,
        fxProfileNameEdit_,
        applyFxProfileButton_,
        saveFxProfileButton_,
        deleteFxProfileButton_,
        systemSettingsHeading_,
        languageLabel_,
        languageSelector_,
        startWithWindows_,
        startMinimized_,
        closeToTray_,
        versionUpdateHeading_,
        controlCenterVersionText_,
        hostVersionText_,
        installStateText_,
        latestVersionText_,
        checkForUpdatesButton_,
        openReleaseButton_,
        repositoryStarHint_,
        openRepositoryButton_,
#if defined(BAFX_ENABLE_SPOUT2)
        spout2Enabled_,
        spout2SenderStatus_,
        obsSpoutPluginStatus_,
        spout2ObsHint_,
        refreshObsSpoutPluginButton_,
        openObsSpoutPluginPageButton_,
#endif
        displaySettingsHeading_,
        displaySelectorLabel_,
        displaySelector_,
        displaySummaryText_,
        hdrEnabled_,
        activeFxRoiEnabled_,
        framePacingLabel_,
        framePacing_,
        displayIndependent_,
        displayEffectsEnabled_,
        displayHdrEnabled_,
        displayFramePacingLabel_,
        displayFramePacing_,
        displayDetailsHeading_,
        displayDetailsText_,
        activeFxRoiDetailsHeading_,
        activeFxRoiDetailsText_,
        pauseButton_,
        refreshButton_,
        hostLifecycleButton_,
        clearLogsButton_,
        resetDefaultsButton_,
        advancedTimingHeading_,
        advancedParticlesHeading_,
        advancedRingsHeading_,
        advancedClickShardsHeading_,
        advancedBloomHeading_,
        advancedLayersHeading_,
        themeColorLabel_,
        themeColorEdit_,
        themeColorPreview_,
        themeColorChoose_,
        advancedTimingSectionButton_,
        advancedParticlesSectionButton_,
        advancedRingsSectionButton_,
        advancedClickShardsSectionButton_,
        advancedBloomSectionButton_,
        advancedLayersSectionButton_};
    if (!slidersCreated
        || !advancedSlidersCreated
        || !particleSlidersCreated
        || !ringSlidersCreated
        || !clickShardSlidersCreated
        || std::ranges::find(required, nullptr) != required.end())
    {
        return false;
    }

    applyFonts();
    applyDpiMetrics();
    selectPage(Page::Basic);
#if defined(BAFX_ENABLE_SPOUT2)
    refreshObsPluginStatus();
#endif
    return true;
}

HWND ControlCenterWindow::createChild(
    const wchar_t* const className,
    const wchar_t* const text,
    const DWORD style,
    const ControlId id) const noexcept
{
    return CreateWindowExW(
        0U,
        className,
        text,
        WS_CHILD | WS_VISIBLE | style,
        0,
        0,
        0,
        0,
        window_,
        controlMenu(static_cast<int>(id)),
        instance_,
        nullptr);
}

bool ControlCenterWindow::createSlider(
    SliderControl& slider,
    const TextId label,
    const double minimum,
    const double maximum,
    const double step,
    std::string path,
    const ControlId id)
{
    slider.label = createChild(
        L"STATIC",
        label,
        SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX);
    slider.trackbar = createChild(
        TRACKBAR_CLASSW,
        L"",
        TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
        id);
    slider.valueText = createChild(
        L"STATIC",
        L"0",
        SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX);
    slider.minimum = minimum;
    slider.maximum = maximum;
    slider.step = step;
    slider.path = std::move(path);

    if (slider.label == nullptr
        || slider.trackbar == nullptr
        || slider.valueText == nullptr)
    {
        return false;
    }

    const int maximumPosition = static_cast<int>(std::lround(
        (maximum - minimum) / step));
    static_cast<void>(SendMessageW(slider.trackbar, TBM_SETRANGEMIN, FALSE, 0));
    static_cast<void>(SendMessageW(
        slider.trackbar,
        TBM_SETRANGEMAX,
        TRUE,
        maximumPosition));
    static_cast<void>(SendMessageW(slider.trackbar, TBM_SETPAGESIZE, 0U, 5));
    return true;
}

void ControlCenterWindow::createFonts()
{
    const HFONT oldNormal = normalFont_;
    const HFONT oldTitle = titleFont_;
    const HFONT oldSection = sectionFont_;

    const HFONT newNormal = CreateFontW(
        -MulDiv(10, static_cast<int>(layoutDpi_), 72),
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
    const HFONT newTitle = CreateFontW(
        -MulDiv(20, static_cast<int>(layoutDpi_), 72),
        0,
        0,
        0,
        FW_SEMIBOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
    const HFONT newSection = CreateFontW(
        -MulDiv(11, static_cast<int>(layoutDpi_), 72),
        0,
        0,
        0,
        FW_SEMIBOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");

    if (newNormal == nullptr || newTitle == nullptr || newSection == nullptr)
    {
        if (newNormal != nullptr)
        {
            DeleteObject(newNormal);
        }
        if (newTitle != nullptr)
        {
            DeleteObject(newTitle);
        }
        if (newSection != nullptr)
        {
            DeleteObject(newSection);
        }
        // Retain the currently selected fonts. Deleting a font that remains
        // selected in child controls would leave GDI with a dangling handle.
        return;
    }

    normalFont_ = newNormal;
    titleFont_ = newTitle;
    sectionFont_ = newSection;
    applyFonts();
    applyDpiMetrics();
    if (oldNormal != nullptr)
    {
        DeleteObject(oldNormal);
    }
    if (oldTitle != nullptr)
    {
        DeleteObject(oldTitle);
    }
    if (oldSection != nullptr)
    {
        DeleteObject(oldSection);
    }
}

void ControlCenterWindow::destroyFonts() noexcept
{
    if (normalFont_ != nullptr)
    {
        DeleteObject(normalFont_);
        normalFont_ = nullptr;
    }
    if (titleFont_ != nullptr)
    {
        DeleteObject(titleFont_);
        titleFont_ = nullptr;
    }
    if (sectionFont_ != nullptr)
    {
        DeleteObject(sectionFont_);
        sectionFont_ = nullptr;
    }
}

void ControlCenterWindow::applyFonts() const noexcept
{
    for (const HWND control : hotkeyControls_)
    {
        setControlFont(control, normalFont_);
    }
    setControlFont(hotkeysPageButton_, normalFont_);
    const std::array normalControls{
        statusText_,
        messageText_,
        basicPageButton_,
        advancedPageButton_,
        displayPageButton_,
        systemPageButton_,
        effectsEnabled_,
        effectsModeLabel_,
        effectsMode_,
        clickEnabled_,
        trailEnabled_,
        diskLayerEnabled_,
        ringsLayerEnabled_,
        clickShardsLayerEnabled_,
        trailShardsLayerEnabled_,
        trailLayerEnabled_,
        bloomLayerEnabled_,
        trailAlwaysOn_,
        leftClickEnabled_,
        rightClickEnabled_,
        middleClickEnabled_,
        globalScale_.label,
        globalScale_.trackbar,
        globalScale_.valueText,
        trailLength_.label,
        trailLength_.trackbar,
        trailLength_.valueText,
        trailWidth_.label,
        trailWidth_.trackbar,
        trailWidth_.valueText,
        inputSamplingRate_.label,
        inputSamplingRate_.trackbar,
        inputSamplingRate_.valueText,
        bloomIntensity_.label,
        bloomIntensity_.trackbar,
        bloomIntensity_.valueText,
        opacity_.label,
        opacity_.trackbar,
        opacity_.valueText,
        clickTimeScale_.label,
        clickTimeScale_.trackbar,
        clickTimeScale_.valueText,
        trailTimeScale_.label,
        trailTimeScale_.trackbar,
        trailTimeScale_.valueText,
        trailLifetimeMs_.label,
        trailLifetimeMs_.trackbar,
        trailLifetimeMs_.valueText,
        bloomDiffusion_.label,
        bloomDiffusion_.trackbar,
        bloomDiffusion_.valueText,
        bloomThreshold_.label,
        bloomThreshold_.trackbar,
        bloomThreshold_.valueText,
        bloomSoftKnee_.label,
        bloomSoftKnee_.trackbar,
        bloomSoftKnee_.valueText,
        bloomClamp_.label,
        bloomClamp_.trackbar,
        bloomClamp_.valueText,
        diskRadius_.label,
        diskRadius_.trackbar,
        diskRadius_.valueText,
        diskLifetimeMs_.label,
        diskLifetimeMs_.trackbar,
        diskLifetimeMs_.valueText,
        ringsHdrIntensity_.label,
        ringsHdrIntensity_.trackbar,
        ringsHdrIntensity_.valueText,
        ringsCount_.label,
        ringsCount_.trackbar,
        ringsCount_.valueText,
        ringsLifetimeMs_.label,
        ringsLifetimeMs_.trackbar,
        ringsLifetimeMs_.valueText,
        ringsRadiusMin_.label,
        ringsRadiusMin_.trackbar,
        ringsRadiusMin_.valueText,
        ringsRadiusMax_.label,
        ringsRadiusMax_.trackbar,
        ringsRadiusMax_.valueText,
        ringsAngularVelocityMultiplier_.label,
        ringsAngularVelocityMultiplier_.trackbar,
        ringsAngularVelocityMultiplier_.valueText,
        ringsRotationDirection_.label,
        ringsRotationDirection_.trackbar,
        ringsRotationDirection_.valueText,
        shardsHdrIntensity_.label,
        shardsHdrIntensity_.trackbar,
        shardsHdrIntensity_.valueText,
        shardsClickCount_.label,
        shardsClickCount_.trackbar,
        shardsClickCount_.valueText,
        shardsClickLifetimeMinMs_.label,
        shardsClickLifetimeMinMs_.trackbar,
        shardsClickLifetimeMinMs_.valueText,
        shardsClickLifetimeMaxMs_.label,
        shardsClickLifetimeMaxMs_.trackbar,
        shardsClickLifetimeMaxMs_.valueText,
        shardsClickRadius_.label,
        shardsClickRadius_.trackbar,
        shardsClickRadius_.valueText,
        shardsClickSpeedMin_.label,
        shardsClickSpeedMin_.trackbar,
        shardsClickSpeedMin_.valueText,
        shardsClickSpeedMax_.label,
        shardsClickSpeedMax_.trackbar,
        shardsClickSpeedMax_.valueText,
        shardsSizeMin_.label,
        shardsSizeMin_.trackbar,
        shardsSizeMin_.valueText,
        shardsSizeMax_.label,
        shardsSizeMax_.trackbar,
        shardsSizeMax_.valueText,
        trailOpacity_.label,
        trailOpacity_.trackbar,
        trailOpacity_.valueText,
        themeColorLabel_,
        themeColorEdit_,
        themeColorPreview_,
        themeColorChoose_,
        bloomQualityLabel_,
        bloomQuality_,
        backgroundModeLabel_,
        backgroundMode_,
        cursorExcluded_,
        allowSystemBorder_,
        idleOptimization_,
        fxProfileLabel_,
        fxProfileSelector_,
        fxProfileNameEdit_,
        applyFxProfileButton_,
        saveFxProfileButton_,
        deleteFxProfileButton_,
        startWithWindows_,
        startMinimized_,
        closeToTray_,
        controlCenterVersionText_,
        hostVersionText_,
        installStateText_,
        latestVersionText_,
        checkForUpdatesButton_,
        openReleaseButton_,
        repositoryStarHint_,
        openRepositoryButton_,
#if defined(BAFX_ENABLE_SPOUT2)
        spout2Enabled_,
        spout2SenderStatus_,
        obsSpoutPluginStatus_,
        spout2ObsHint_,
        refreshObsSpoutPluginButton_,
        openObsSpoutPluginPageButton_,
#endif
        displaySelectorLabel_,
        displaySelector_,
        displaySummaryText_,
        hdrEnabled_,
        activeFxRoiEnabled_,
        framePacingLabel_,
        framePacing_,
        displayIndependent_,
        displayEffectsEnabled_,
        displayHdrEnabled_,
        displayFramePacingLabel_,
        displayFramePacing_,
        displayDetailsText_,
        activeFxRoiDetailsText_,
        pauseButton_,
        refreshButton_,
        hostLifecycleButton_,
        clearLogsButton_,
        resetDefaultsButton_};
    for (const HWND control : normalControls)
    {
        setControlFont(control, normalFont_);
    }
    setControlFont(titleText_, titleFont_);
    setControlFont(effectsHeading_, sectionFont_);
    setControlFont(backgroundHeading_, sectionFont_);
    setControlFont(systemSettingsHeading_, sectionFont_);
    setControlFont(languageLabel_, normalFont_);
    setControlFont(languageSelector_, normalFont_);
    setControlFont(versionUpdateHeading_, sectionFont_);
    setControlFont(advancedTimingHeading_, sectionFont_);
    setControlFont(advancedParticlesHeading_, sectionFont_);
    setControlFont(advancedRingsHeading_, sectionFont_);
    setControlFont(advancedClickShardsHeading_, sectionFont_);
    setControlFont(advancedBloomHeading_, sectionFont_);
    setControlFont(advancedLayersHeading_, sectionFont_);
    setControlFont(displaySettingsHeading_, sectionFont_);
    setControlFont(displayDetailsHeading_, sectionFont_);
    setControlFont(activeFxRoiDetailsHeading_, sectionFont_);
    setControlFont(advancedTimingSectionButton_, normalFont_);
    setControlFont(advancedParticlesSectionButton_, normalFont_);
    setControlFont(advancedRingsSectionButton_, normalFont_);
    setControlFont(advancedClickShardsSectionButton_, normalFont_);
    setControlFont(advancedBloomSectionButton_, normalFont_);
    setControlFont(advancedLayersSectionButton_, normalFont_);
}

void ControlCenterWindow::applyDpiMetrics() const noexcept
{
    SendMessageW(backgroundMode_, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(scale(470)), 0);
    SendMessageW(effectsMode_, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(scale(320)), 0);
    const std::array comboBoxes{
        languageSelector_,
        bloomQuality_,
        effectsMode_,
        backgroundMode_,
        fxProfileSelector_,
        displaySelector_,
        framePacing_,
        displayFramePacing_};
    for (const HWND comboBox : comboBoxes)
    {
        if (comboBox == nullptr)
        {
            continue;
        }
        // A custom font does not automatically update the native combo-box
        // item height after WM_DPICHANGED. Explicit metrics prevent clipped
        // Chinese glyphs on 125%-200% scale factors.
        static_cast<void>(SendMessageW(
            comboBox,
            CB_SETITEMHEIGHT,
            static_cast<WPARAM>(-1),
            scale(26)));
        static_cast<void>(SendMessageW(comboBox, CB_SETITEMHEIGHT, 0U, scale(26)));
    }
    if (fxProfileSelector_ != nullptr)
    {
        // The collapsed selector stays compact; the drop-down is wider so
        // distinct long Profile names remain identifiable.
        static_cast<void>(SendMessageW(
            fxProfileSelector_,
            CB_SETDROPPEDWIDTH,
            scale(320),
            0));
    }
}

void ControlCenterWindow::adaptLayoutToMonitor(
    const HMONITOR monitor,
    const bool force)
{
    if (window_ == nullptr || monitor == nullptr)
    {
        return;
    }
    if (!force && monitor == layoutMonitor_)
    {
        return;
    }

    const UINT windowDpi = GetDpiForWindow(window_);
    if (windowDpi != 0U && windowDpi != dpi_)
    {
        // A real DPI transition is finalized by WM_DPICHANGED, which also
        // supplies the OS-recommended bounds. Do not race that transaction
        // from an earlier WM_MOVE notification.
        return;
    }

    const RECT workArea = monitorWorkArea(monitor);
    const UINT nextLayoutDpi = controlCenterLayoutDpi(
        maximumClientSize(workArea, dpi_),
        dpi_);
    const bool layoutChanged = nextLayoutDpi != layoutDpi_;
    layoutMonitor_ = monitor;
    if (layoutChanged)
    {
        layoutDpi_ = nextLayoutDpi;
        createFonts();
    }

    if (IsIconic(window_) != FALSE || IsZoomed(window_) != FALSE)
    {
        RECT client{};
        if (layoutChanged && GetClientRect(window_, &client) != FALSE)
        {
            layoutControls(
                client.right - client.left,
                client.bottom - client.top);
        }
        return;
    }

    RECT currentBounds{};
    if (GetWindowRect(window_, &currentBounds) == FALSE)
    {
        return;
    }
    RECT minimumBounds{
        0,
        0,
        scale(minimumControlCenterClientWidth),
        scale(minimumControlCenterClientHeight)};
    static_cast<void>(AdjustWindowRectExForDpi(
        &minimumBounds,
        static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE)),
        FALSE,
        static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_EXSTYLE)),
        dpi_));

    const int currentWidth = currentBounds.right - currentBounds.left;
    const int currentHeight = currentBounds.bottom - currentBounds.top;
    const PixelSize targetSize = clampPixelSize(
        PixelSize{
            (std::max)(
                currentWidth,
                static_cast<int>(minimumBounds.right - minimumBounds.left)),
            (std::max)(
                currentHeight,
                static_cast<int>(minimumBounds.bottom - minimumBounds.top))},
        PixelSize{
            static_cast<int>(workArea.right - workArea.left),
            static_cast<int>(workArea.bottom - workArea.top)});
    const int targetX = (std::clamp)(
        static_cast<int>(currentBounds.left),
        static_cast<int>(workArea.left),
        static_cast<int>(workArea.right) - targetSize.width);
    const int targetY = (std::clamp)(
        static_cast<int>(currentBounds.top),
        static_cast<int>(workArea.top),
        static_cast<int>(workArea.bottom) - targetSize.height);
    const bool sizeChanged = targetSize.width != currentWidth
        || targetSize.height != currentHeight;
    const bool positionChanged = targetX != currentBounds.left
        || targetY != currentBounds.top;

    BOOL repositioned = TRUE;
    if (sizeChanged || positionChanged)
    {
        repositioned = SetWindowPos(
            window_,
            nullptr,
            targetX,
            targetY,
            targetSize.width,
            targetSize.height,
            SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOZORDER);
    }
    if (layoutChanged && (!sizeChanged || repositioned == FALSE))
    {
        RECT client{};
        if (GetClientRect(window_, &client) != FALSE)
        {
            layoutControls(
                client.right - client.left,
                client.bottom - client.top);
        }
    }
}

void ControlCenterWindow::layoutControls(
    const int clientWidth,
    const int clientHeight) const noexcept
{
    if (titleText_ == nullptr || clientWidth <= 0 || clientHeight <= 0)
    {
        return;
    }

    // Keep all sibling moves paint-free while their overlapping rectangles are
    // changing. The final redraw below is the single committed frame for the
    // complete layout, including the parent background.

    const int margin = scale(24);
    const int columnGap = scale(24);
    const int availableRightWidth = (std::clamp)(
        clientWidth / 3,
        scale(280),
        scale(340));
    const int leftWidth = clientWidth
        - margin * 2
        - columnGap
        - availableRightWidth;
    const int rightX = margin + leftWidth + columnGap;

    moveControl(titleText_, margin, scale(16), clientWidth - margin * 2, scale(36));
    moveControl(statusText_, margin, scale(54), clientWidth - margin * 2, scale(24));
    // setInfo() writes a title and detail on separate lines. Reserve both lines
    // while keeping the page-tab band fixed across DPI transitions.
    const int messageHeight = scale(36);
    moveControl(messageText_, margin, scale(82), clientWidth - margin * 2, messageHeight);

    const int contentTop = scale(150);
    const int tabWidth = (clientWidth - margin * 2 - scale(32)) / 5;
    const int tabGap = scale(8);
    moveControl(
        basicPageButton_,
        margin,
        scale(120),
        tabWidth,
        scale(30));
    moveControl(
        advancedPageButton_,
        margin + tabWidth + tabGap,
        scale(120),
        tabWidth,
        scale(30));
    moveControl(
        displayPageButton_,
        margin + (tabWidth + tabGap) * 2,
        scale(120),
        tabWidth,
        scale(30));
    moveControl(
        hotkeysPageButton_,
        margin + (tabWidth + tabGap) * 3,
        scale(120),
        tabWidth,
        scale(30));
    moveControl(systemPageButton_, margin + (tabWidth + tabGap) * 4,
        scale(120), tabWidth, scale(30));

    if (activePage_ == Page::Hotkeys)
    {
        layoutHotkeyControls(clientWidth, clientHeight);
        redrawWindowTree();
        return;
    }

    if (activePage_ == Page::DisplayPerformance)
    {
        const int actionHeight = scale(38);
        const int actionGap = scale(10);
        const int actionY = (std::max)(
            contentTop + scale(300),
            clientHeight - margin - actionHeight);
        const int panelHeight = (std::max)(
            scale(300),
            actionY - contentTop - scale(12));
        const int settingsWidth = (std::clamp)(
            clientWidth / 3,
            scale(300),
            scale(360));
        const int detailsX = margin + settingsWidth + columnGap;
        const int detailsWidth = (std::max)(
            scale(1),
            clientWidth - detailsX - margin);
        const int inset = scale(16);
        const int settingsContentX = margin + inset;
        const int settingsContentWidth = (std::max)(
            scale(1),
            settingsWidth - inset * 2);

        moveControl(
            displaySettingsHeading_,
            margin,
            contentTop,
            settingsWidth,
            panelHeight);
        moveControl(
            displaySelectorLabel_,
            settingsContentX,
            contentTop + scale(30),
            settingsContentWidth,
            scale(22));
        moveControl(
            displaySelector_,
            settingsContentX,
            contentTop + scale(52),
            settingsContentWidth,
            scale(34));
        moveControl(
            displaySummaryText_,
            settingsContentX,
            contentTop + scale(94),
            settingsContentWidth,
            scale(48));
        moveControl(
            hdrEnabled_,
            settingsContentX,
            contentTop + scale(148),
            settingsContentWidth,
            scale(30));
        moveControl(
            activeFxRoiEnabled_,
            settingsContentX,
            contentTop + scale(184),
            settingsContentWidth,
            scale(30));
        moveControl(
            framePacingLabel_,
            settingsContentX,
            contentTop + scale(220),
            settingsContentWidth,
            scale(22));
        moveControl(
            framePacing_,
            settingsContentX,
            contentTop + scale(242),
            settingsContentWidth,
            scale(34));

        const int detailsPanelHeight = (std::clamp)(
            panelHeight / 2,
            scale(174),
            scale(220));
        const int roiPanelGap = scale(10);
        const int roiPanelY = contentTop
            + detailsPanelHeight
            + roiPanelGap;
        const int roiPanelHeight = (std::max)(
            scale(1),
            panelHeight - detailsPanelHeight - roiPanelGap);
        moveControl(
            displayDetailsHeading_,
            detailsX,
            contentTop,
            detailsWidth,
            detailsPanelHeight);
        const int detailsContentX = detailsX + inset;
        const int detailsContentWidth = (std::max)(
            scale(1),
            detailsWidth - inset * 2);
        const int policyColumnGap = scale(12);
        const int policyColumnWidth = (std::max)(
            scale(1),
            (detailsContentWidth - policyColumnGap) / 2);
        moveControl(
            displayIndependent_,
            detailsContentX,
            contentTop + scale(28),
            policyColumnWidth,
            scale(30));
        moveControl(
            displayEffectsEnabled_,
            detailsContentX + policyColumnWidth + policyColumnGap,
            contentTop + scale(28),
            policyColumnWidth,
            scale(30));
        moveControl(
            displayHdrEnabled_,
            detailsContentX,
            contentTop + scale(62),
            policyColumnWidth,
            scale(30));
        moveControl(
            displayFramePacingLabel_,
            detailsContentX + policyColumnWidth + policyColumnGap,
            contentTop + scale(62),
            policyColumnWidth,
            scale(22));
        moveControl(
            displayFramePacing_,
            detailsContentX + policyColumnWidth + policyColumnGap,
            contentTop + scale(84),
            policyColumnWidth,
            scale(34));
        moveControl(
            displayDetailsText_,
            detailsContentX,
            contentTop + scale(124),
            detailsContentWidth,
            (std::max)(scale(1), detailsPanelHeight - scale(140)));
        moveControl(
            activeFxRoiDetailsHeading_,
            detailsX,
            roiPanelY,
            detailsWidth,
            roiPanelHeight);
        moveControl(
            activeFxRoiDetailsText_,
            detailsContentX,
            roiPanelY + scale(28),
            detailsContentWidth,
            (std::max)(scale(1), roiPanelHeight - scale(44)));

        const int actionWidth = (clientWidth - margin * 2 - actionGap * 3) / 4;
        moveControl(
            pauseButton_,
            margin,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            refreshButton_,
            margin + actionWidth + actionGap,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            hostLifecycleButton_,
            margin + (actionWidth + actionGap) * 2,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            resetDefaultsButton_,
            margin + (actionWidth + actionGap) * 3,
            actionY,
            actionWidth,
            actionHeight);
        redrawWindowTree();
        return;
    }

    if (activePage_ == Page::System)
    {
        const int actionHeight = scale(38);
        const int actionGap = scale(10);
#if defined(BAFX_ENABLE_SPOUT2)
        constexpr int minimumSystemContentHeight = 356;
        constexpr int minimumSystemPanelHeight = 324;
#else
        constexpr int minimumSystemContentHeight = 336;
        constexpr int minimumSystemPanelHeight = 324;
#endif
        const int actionY = (std::max)(
            contentTop + scale(minimumSystemContentHeight),
            clientHeight - margin - actionHeight);
        const int panelHeight = (std::max)(
            scale(minimumSystemPanelHeight),
            actionY - contentTop - scale(12));
        const int panelWidth = clientWidth - margin * 2;
        const int panelGap = scale(16);
        const int updatePanelWidth = (std::clamp)(
            panelWidth * 2 / 5,
            scale(300),
            scale(340));
        const int systemPanelWidth = (std::max)(
            scale(1),
            panelWidth - panelGap - updatePanelWidth);
        const int updatePanelX = margin + systemPanelWidth + panelGap;
        const int inset = scale(16);
        const int contentX = margin + inset;
        const int contentWidth = (std::max)(
            scale(1),
            systemPanelWidth - inset * 2);

        moveControl(
            systemSettingsHeading_,
            margin,
            contentTop,
            systemPanelWidth,
            panelHeight);
        moveControl(languageLabel_, contentX, contentTop + scale(32), scale(108), scale(26));
        moveControl(languageSelector_, contentX + scale(116), contentTop + scale(28),
            contentWidth - scale(116), scale(160));
        moveControl(
            startWithWindows_,
            contentX,
            contentTop + scale(64),
            contentWidth,
            scale(28));
        moveControl(
            startMinimized_,
            contentX,
            contentTop + scale(92),
            contentWidth,
            scale(28));
        moveControl(
            closeToTray_,
            contentX,
            contentTop + scale(120),
            contentWidth,
            scale(28));
#if defined(BAFX_ENABLE_SPOUT2)
        moveControl(
            spout2Enabled_,
            contentX,
            contentTop + scale(148),
            contentWidth,
            scale(28));
        moveControl(
            spout2SenderStatus_,
            contentX,
            contentTop + scale(180),
            contentWidth,
            scale(56));
        moveControl(
            obsSpoutPluginStatus_,
            contentX,
            contentTop + scale(240),
            contentWidth,
            scale(42));
        moveControl(
            spout2ObsHint_,
            contentX,
            contentTop + scale(284),
            contentWidth,
            scale(36));
        const int obsButtonGap = scale(10);
        const int obsButtonWidth = (contentWidth - obsButtonGap) / 2;
        moveControl(
            refreshObsSpoutPluginButton_,
            contentX,
            contentTop + scale(328),
            obsButtonWidth,
            scale(30));
        moveControl(
            openObsSpoutPluginPageButton_,
            contentX + obsButtonWidth + obsButtonGap,
            contentTop + scale(328),
            obsButtonWidth,
            scale(30));
#endif

        moveControl(
            versionUpdateHeading_,
            updatePanelX,
            contentTop,
            updatePanelWidth,
            panelHeight);
        const int updateContentX = updatePanelX + inset;
        const int updateContentWidth = (std::max)(
            scale(1),
            updatePanelWidth - inset * 2);
        moveControl(
            controlCenterVersionText_,
            updateContentX,
            contentTop + scale(36),
            updateContentWidth,
            scale(24));
        moveControl(
            hostVersionText_,
            updateContentX,
            contentTop + scale(68),
            updateContentWidth,
            scale(24));
        moveControl(
            installStateText_,
            updateContentX,
            contentTop + scale(100),
            updateContentWidth,
            scale(44));
        moveControl(
            latestVersionText_,
            updateContentX,
            contentTop + scale(152),
            updateContentWidth,
            scale(40));
        const int updateButtonGap = scale(10);
        const int updateButtonWidth = (std::max)(
            scale(1),
            (updateContentWidth - updateButtonGap) / 2);
        moveControl(
            checkForUpdatesButton_,
            updateContentX,
            contentTop + scale(204),
            updateButtonWidth,
            scale(32));
        moveControl(
            openReleaseButton_,
            updateContentX + updateButtonWidth + updateButtonGap,
            contentTop + scale(204),
            updateButtonWidth,
            scale(32));
        moveControl(
            repositoryStarHint_,
            updateContentX,
            contentTop + scale(248),
            updateContentWidth,
            scale(24));
        moveControl(
            openRepositoryButton_,
            updateContentX,
            contentTop + scale(280),
            updateContentWidth,
            scale(32));

        moveControl(clearLogsButton_, updateContentX, contentTop + scale(316),
            updateContentWidth, scale(30));

        const int actionWidth = (clientWidth - margin * 2 - actionGap * 3) / 4;
        moveControl(
            pauseButton_,
            margin,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            refreshButton_,
            margin + actionWidth + actionGap,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            hostLifecycleButton_,
            margin + (actionWidth + actionGap) * 2,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            resetDefaultsButton_,
            margin + (actionWidth + actionGap) * 3,
            actionY,
            actionWidth,
            actionHeight);
        redrawWindowTree();
        return;
    }

    if (activePage_ == Page::Advanced)
    {
        const int sectionButtonGap = scale(8);
        constexpr int sectionButtonCount = 6;
        const int sectionButtonWidth = (std::max)(
            scale(1),
            (clientWidth - margin * 2
                - sectionButtonGap * (sectionButtonCount - 1))
                / sectionButtonCount);
        moveControl(
            advancedTimingSectionButton_,
            margin,
            contentTop,
            sectionButtonWidth,
            scale(30));
        moveControl(
            advancedParticlesSectionButton_,
            margin + sectionButtonWidth + sectionButtonGap,
            contentTop,
            sectionButtonWidth,
            scale(30));
        moveControl(
            advancedRingsSectionButton_,
            margin + (sectionButtonWidth + sectionButtonGap) * 2,
            contentTop,
            sectionButtonWidth,
            scale(30));
        moveControl(
            advancedClickShardsSectionButton_,
            margin + (sectionButtonWidth + sectionButtonGap) * 3,
            contentTop,
            sectionButtonWidth,
            scale(30));
        moveControl(
            advancedBloomSectionButton_,
            margin + (sectionButtonWidth + sectionButtonGap) * 4,
            contentTop,
            sectionButtonWidth,
            scale(30));
        moveControl(
            advancedLayersSectionButton_,
            margin + (sectionButtonWidth + sectionButtonGap) * 5,
            contentTop,
            sectionButtonWidth,
            scale(30));

        const int panelTop = contentTop + scale(38);
        const int actionHeight = scale(38);
        const int actionGap = scale(10);
        const int actionY = (std::max)(
            panelTop + scale(262),
            clientHeight - margin - actionHeight);
        const int groupHeight = (std::max)(
            scale(262),
            actionY - panelTop - scale(12));
        const int groupWidth = clientWidth - margin * 2;
        const int inset = scale(16);
        const int advancedColumnGap = scale(24);
        const int columnWidth = (std::max)(
            scale(1),
            (groupWidth - inset * 2 - advancedColumnGap) / 2);
        const int left = margin + inset;
        const int right = left + columnWidth + advancedColumnGap;
        const int rowTop = panelTop + scale(32);
        const int nextRowTop = rowTop + scale(50);
        const int thirdRowTop = nextRowTop + scale(50);
        const int fourthRowTop = thirdRowTop + scale(50);

        switch (activeAdvancedSection_)
        {
        case AdvancedSection::Timing:
            moveControl(
                advancedTimingHeading_,
                margin,
                panelTop,
                groupWidth,
                groupHeight);
            layoutSlider(opacity_, left, rowTop, columnWidth, scale(40));
            layoutSlider(
                clickTimeScale_,
                left,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                trailTimeScale_,
                right,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                trailLifetimeMs_,
                right,
                nextRowTop,
                columnWidth,
                scale(40));
            break;
        case AdvancedSection::Particles:
            moveControl(
                advancedParticlesHeading_,
                margin,
                panelTop,
                groupWidth,
                groupHeight);
            layoutSlider(diskRadius_, left, rowTop, columnWidth, scale(40));
            layoutSlider(
                diskLifetimeMs_,
                left,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                trailOpacity_,
                left,
                thirdRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                ringsHdrIntensity_,
                right,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsHdrIntensity_,
                right,
                nextRowTop,
                columnWidth,
                scale(40));
            moveControl(
                themeColorLabel_,
                left,
                fourthRowTop,
                scale(72),
                scale(40));
            moveControl(
                themeColorEdit_,
                left + scale(78),
                fourthRowTop,
                scale(112),
                scale(40));
            moveControl(
                themeColorPreview_,
                left + scale(196),
                fourthRowTop,
                scale(40),
                scale(40));
            moveControl(
                themeColorChoose_,
                left + scale(242),
                fourthRowTop,
                scale(78),
                scale(40));
            break;
        case AdvancedSection::Rings:
            moveControl(
                advancedRingsHeading_,
                margin,
                panelTop,
                groupWidth,
                groupHeight);
            layoutSlider(ringsCount_, left, rowTop, columnWidth, scale(40));
            layoutSlider(
                ringsLifetimeMs_,
                right,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                ringsRadiusMin_,
                left,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                ringsRadiusMax_,
                right,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                ringsAngularVelocityMultiplier_,
                left,
                thirdRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                ringsRotationDirection_,
                right,
                thirdRowTop,
                columnWidth,
                scale(40));
            break;
        case AdvancedSection::ClickShards:
            moveControl(
                advancedClickShardsHeading_,
                margin,
                panelTop,
                groupWidth,
                groupHeight);
            layoutSlider(
                shardsClickCount_,
                left,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsClickRadius_,
                right,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsClickLifetimeMinMs_,
                left,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsClickLifetimeMaxMs_,
                right,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsClickSpeedMin_,
                left,
                thirdRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsClickSpeedMax_,
                right,
                thirdRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsSizeMin_,
                left,
                fourthRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                shardsSizeMax_,
                right,
                fourthRowTop,
                columnWidth,
                scale(40));
            break;
        case AdvancedSection::Bloom:
            moveControl(
                advancedBloomHeading_,
                margin,
                panelTop,
                groupWidth,
                groupHeight);
            layoutSlider(
                bloomDiffusion_,
                left,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                bloomThreshold_,
                left,
                nextRowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                bloomSoftKnee_,
                right,
                rowTop,
                columnWidth,
                scale(40));
            layoutSlider(
                bloomClamp_,
                right,
                nextRowTop,
                columnWidth,
                scale(40));
            break;
        case AdvancedSection::Layers:
        {
            moveControl(
                advancedLayersHeading_,
                margin,
                panelTop,
                groupWidth,
                groupHeight);
            const int layerColumnGap = scale(16);
            const int layerColumnWidth = (std::max)(
                scale(1),
                (groupWidth - inset * 2 - layerColumnGap * 2) / 3);
            const int layerMiddle = left + layerColumnWidth + layerColumnGap;
            const int layerRight = layerMiddle
                + layerColumnWidth + layerColumnGap;
            moveControl(
                diskLayerEnabled_,
                left,
                rowTop,
                layerColumnWidth,
                scale(40));
            moveControl(
                ringsLayerEnabled_,
                layerMiddle,
                rowTop,
                layerColumnWidth,
                scale(40));
            moveControl(
                clickShardsLayerEnabled_,
                layerRight,
                rowTop,
                layerColumnWidth,
                scale(40));
            moveControl(
                trailShardsLayerEnabled_,
                left,
                nextRowTop,
                layerColumnWidth,
                scale(40));
            moveControl(
                trailLayerEnabled_,
                layerMiddle,
                nextRowTop,
                layerColumnWidth,
                scale(40));
            moveControl(
                bloomLayerEnabled_,
                layerRight,
                nextRowTop,
                layerColumnWidth,
                scale(40));
            break;
        }
        }

        const int actionWidth = (clientWidth - margin * 2 - actionGap * 3) / 4;
        moveControl(
            pauseButton_,
            margin,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            refreshButton_,
            margin + (actionWidth + actionGap),
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            hostLifecycleButton_,
            margin + (actionWidth + actionGap) * 2,
            actionY,
            actionWidth,
            actionHeight);
        moveControl(
            resetDefaultsButton_,
            margin + (actionWidth + actionGap) * 3,
            actionY,
            actionWidth,
            actionHeight);
        redrawWindowTree();
        return;
    }

    const int groupHeight = (std::max)(
        scale(350),
        clientHeight - contentTop - margin);
    moveControl(effectsHeading_, margin, contentTop, leftWidth, groupHeight);

    const int groupInset = scale(16);
    const int groupLeft = margin + groupInset;
    const int groupWidth = (std::max)(scale(1), leftWidth - groupInset * 2);
    moveControl(
        effectsModeLabel_,
        groupLeft,
        contentTop + scale(28),
        groupWidth,
        scale(22));
    moveControl(
        effectsMode_,
        groupLeft,
        contentTop + scale(50),
        groupWidth,
        scale(34));
    const int checkboxTop = contentTop + scale(88);
    const int checkboxWidth = groupWidth / 4;
    moveControl(effectsEnabled_, groupLeft, checkboxTop, checkboxWidth, scale(30));
    moveControl(clickEnabled_, groupLeft + checkboxWidth, checkboxTop, checkboxWidth, scale(30));
    moveControl(trailEnabled_, groupLeft + checkboxWidth * 2, checkboxTop, checkboxWidth, scale(30));
    moveControl(
        trailAlwaysOn_,
        groupLeft + checkboxWidth * 3,
        checkboxTop,
        groupWidth - checkboxWidth * 3,
        scale(30));

    // Keep the button-specific input switches on their own row so translated
    // labels remain readable on the smallest supported DPI-scaled layout.
    const int inputCheckboxTop = checkboxTop + scale(30);
    moveControl(
        leftClickEnabled_,
        groupLeft,
        inputCheckboxTop,
        checkboxWidth,
        scale(30));
    moveControl(
        rightClickEnabled_,
        groupLeft + checkboxWidth,
        inputCheckboxTop,
        checkboxWidth,
        scale(30));
    moveControl(
        middleClickEnabled_,
        groupLeft + checkboxWidth * 2,
        inputCheckboxTop,
        checkboxWidth,
        scale(30));

    int sliderTop = inputCheckboxTop + scale(30);
    layoutSlider(globalScale_, groupLeft, sliderTop, groupWidth, scale(40));
    sliderTop += scale(44);
    layoutSlider(trailLength_, groupLeft, sliderTop, groupWidth, scale(40));
    sliderTop += scale(44);
    layoutSlider(trailWidth_, groupLeft, sliderTop, groupWidth, scale(40));
    sliderTop += scale(44);
    layoutSlider(inputSamplingRate_, groupLeft, sliderTop, groupWidth, scale(40));
    sliderTop += scale(44);
    layoutSlider(bloomIntensity_, groupLeft, sliderTop, groupWidth, scale(40));
    sliderTop += scale(44);

    const int labelWidth = scale(106);
    moveControl(bloomQualityLabel_, groupLeft, sliderTop, labelWidth, scale(34));
    moveControl(
        bloomQuality_,
        groupLeft + labelWidth,
        sliderTop,
        groupWidth - labelWidth,
        scale(34));

    moveControl(backgroundHeading_, rightX, contentTop, availableRightWidth, groupHeight);
    const int rightContentX = rightX + groupInset;
    const int rightContentWidth = (std::max)(
        scale(1),
        availableRightWidth - groupInset * 2);
    moveControl(
        backgroundModeLabel_,
        rightContentX,
        contentTop + scale(32),
        rightContentWidth,
        scale(24));
    moveControl(
        backgroundMode_,
        rightContentX,
        contentTop + scale(58),
        rightContentWidth,
        scale(34));
    moveControl(
        cursorExcluded_,
        rightContentX,
        contentTop + scale(101),
        rightContentWidth,
        scale(30));
    moveControl(
        allowSystemBorder_,
        rightContentX,
        contentTop + scale(133),
        rightContentWidth,
        scale(30));
    moveControl(
        idleOptimization_,
        rightContentX,
        contentTop + scale(165),
        rightContentWidth,
        scale(30));
    moveControl(
        pauseButton_,
        rightContentX,
        contentTop + scale(201),
        rightContentWidth,
        scale(38));

    const int actionGap = scale(10);
    const int actionWidth = (rightContentWidth - actionGap) / 2;
    moveControl(
        refreshButton_,
        rightContentX,
        contentTop + scale(249),
        actionWidth,
        scale(38));
    moveControl(
        hostLifecycleButton_,
        rightContentX + actionWidth + actionGap,
        contentTop + scale(249),
        actionWidth,
        scale(38));
    moveControl(
        resetDefaultsButton_,
        rightContentX,
        contentTop + scale(297),
        rightContentWidth,
        scale(38));

    const int profileLabelWidth = scale(92);
    moveControl(
        fxProfileLabel_,
        rightContentX,
        contentTop + scale(341),
        profileLabelWidth,
        scale(34));
    moveControl(
        fxProfileSelector_,
        rightContentX + profileLabelWidth,
        contentTop + scale(341),
        rightContentWidth - profileLabelWidth,
        scale(34));

    const int profileButtonGap = scale(6);
    const int profileApplyWidth = scale(52);
    const int profileSaveWidth = scale(60);
    const int profileDeleteWidth = scale(58);
    const int profileNameWidth = (std::max)(
        scale(1),
        rightContentWidth
            - profileApplyWidth
            - profileSaveWidth
            - profileDeleteWidth
            - profileButtonGap * 3);
    const int profileRowTop = contentTop + scale(379);
    moveControl(
        fxProfileNameEdit_,
        rightContentX,
        profileRowTop,
        profileNameWidth,
        scale(34));
    const int profileApplyX = rightContentX
        + profileNameWidth
        + profileButtonGap;
    moveControl(
        applyFxProfileButton_,
        profileApplyX,
        profileRowTop,
        profileApplyWidth,
        scale(34));
    const int profileSaveX = profileApplyX
        + profileApplyWidth
        + profileButtonGap;
    moveControl(
        saveFxProfileButton_,
        profileSaveX,
        profileRowTop,
        profileSaveWidth,
        scale(34));
    moveControl(
        deleteFxProfileButton_,
        profileSaveX + profileSaveWidth + profileButtonGap,
        profileRowTop,
        profileDeleteWidth,
        scale(34));

    redrawWindowTree();
}

void ControlCenterWindow::redrawWindowTree() const noexcept
{
    if (window_ == nullptr)
    {
        return;
    }

    static_cast<void>(RedrawWindow(
        window_,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW));
}

void ControlCenterWindow::layoutSlider(
    const SliderControl& slider,
    const int x,
    const int y,
    const int width,
    const int height) const noexcept
{
    // English ring/shard labels need a little more room at the minimum width.
    const int labelWidth = scale(152);
    const int valueWidth = scale(64);
    const int gap = scale(8);
    moveControl(slider.label, x, y, labelWidth, height);
    moveControl(
        slider.trackbar,
        x + labelWidth,
        y,
        width - labelWidth - valueWidth - gap,
        height);
    moveControl(
        slider.valueText,
        x + width - valueWidth,
        y,
        valueWidth,
        height);
}

void ControlCenterWindow::setPageControlVisible(
    const HWND control,
    const bool visible) const noexcept
{
    if (control != nullptr)
    {
        ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
    }
}

void ControlCenterWindow::selectPage(const Page page) noexcept
{
    if (activePage_ == Page::Hotkeys && page != Page::Hotkeys)
    {
        endHotkeyCapture();
    }
    activePage_ = page;
    KillTimer(window_, hotkeyTimerId);
    if (page == Page::Hotkeys)
    {
        if (connected_)
        {
            static_cast<void>(refreshHotkeys());
        }
        SetTimer(window_, hotkeyTimerId, 1'000U, nullptr);
    }
    updatePageVisibility();
    updateDisplayStatePolling();
}

void ControlCenterWindow::updateDisplayStatePolling() noexcept
{
    if (window_ == nullptr)
    {
        return;
    }
    KillTimer(window_, displayStateTimerId);
    if (connected_ && activePage_ == Page::DisplayPerformance)
    {
        // ROI diagnostics are observational. A failed timer registration only
        // disables automatic refresh and must never affect Host settings.
        static_cast<void>(SetTimer(
            window_,
            displayStateTimerId,
            displayStatePollDelayMilliseconds,
            nullptr));
    }
}

void ControlCenterWindow::selectAdvancedSection(
    const AdvancedSection section) noexcept
{
    activeAdvancedSection_ = section;
    updatePageVisibility();
}

void ControlCenterWindow::updatePageVisibility() noexcept
{
    const bool hotkeys = activePage_ == Page::Hotkeys;
    setChecked(hotkeysPageButton_, hotkeys);
    for (const HWND control : hotkeyControls_)
    {
        setPageControlVisible(control, hotkeys);
    }
    const bool basic = activePage_ == Page::Basic;
    const bool advanced = activePage_ == Page::Advanced;
    const bool display = activePage_ == Page::DisplayPerformance;
    const bool system = activePage_ == Page::System;
    static_cast<void>(SendMessageW(
        basicPageButton_,
        BM_SETCHECK,
        basic ? BST_CHECKED : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        advancedPageButton_,
        BM_SETCHECK,
        advanced ? BST_CHECKED : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        displayPageButton_,
        BM_SETCHECK,
        display ? BST_CHECKED : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        systemPageButton_,
        BM_SETCHECK,
        system ? BST_CHECKED : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        advancedTimingSectionButton_,
        BM_SETCHECK,
        activeAdvancedSection_ == AdvancedSection::Timing
            ? BST_CHECKED
            : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        advancedParticlesSectionButton_,
        BM_SETCHECK,
        activeAdvancedSection_ == AdvancedSection::Particles
            ? BST_CHECKED
            : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        advancedRingsSectionButton_,
        BM_SETCHECK,
        activeAdvancedSection_ == AdvancedSection::Rings
            ? BST_CHECKED
            : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        advancedClickShardsSectionButton_,
        BM_SETCHECK,
        activeAdvancedSection_ == AdvancedSection::ClickShards
            ? BST_CHECKED
            : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        advancedBloomSectionButton_,
        BM_SETCHECK,
        activeAdvancedSection_ == AdvancedSection::Bloom
            ? BST_CHECKED
            : BST_UNCHECKED,
        0));
    static_cast<void>(SendMessageW(
        advancedLayersSectionButton_,
        BM_SETCHECK,
        activeAdvancedSection_ == AdvancedSection::Layers
            ? BST_CHECKED
            : BST_UNCHECKED,
        0));

    const std::array basicControls{
        effectsHeading_,
        effectsEnabled_,
        effectsModeLabel_,
        effectsMode_,
        clickEnabled_,
        trailEnabled_,
        trailAlwaysOn_,
        leftClickEnabled_,
        rightClickEnabled_,
        middleClickEnabled_,
        globalScale_.label,
        globalScale_.trackbar,
        globalScale_.valueText,
        trailLength_.label,
        trailLength_.trackbar,
        trailLength_.valueText,
        trailWidth_.label,
        trailWidth_.trackbar,
        trailWidth_.valueText,
        inputSamplingRate_.label,
        inputSamplingRate_.trackbar,
        inputSamplingRate_.valueText,
        bloomIntensity_.label,
        bloomIntensity_.trackbar,
        bloomIntensity_.valueText,
        bloomQualityLabel_,
        bloomQuality_,
        backgroundHeading_,
        backgroundModeLabel_,
        backgroundMode_,
        cursorExcluded_,
        allowSystemBorder_,
        idleOptimization_,
        fxProfileLabel_,
        fxProfileSelector_,
        fxProfileNameEdit_,
        applyFxProfileButton_,
        saveFxProfileButton_,
        deleteFxProfileButton_};
    for (const HWND control : basicControls)
    {
        setPageControlVisible(control, basic);
    }

    const std::array advancedSectionButtons{
        advancedTimingSectionButton_,
        advancedParticlesSectionButton_,
        advancedRingsSectionButton_,
        advancedClickShardsSectionButton_,
        advancedBloomSectionButton_,
        advancedLayersSectionButton_};
    for (const HWND control : advancedSectionButtons)
    {
        setPageControlVisible(control, advanced);
    }

    const bool timing = advanced
        && activeAdvancedSection_ == AdvancedSection::Timing;
    const std::array advancedTimingControls{
        advancedTimingHeading_,
        opacity_.label,
        opacity_.trackbar,
        opacity_.valueText,
        clickTimeScale_.label,
        clickTimeScale_.trackbar,
        clickTimeScale_.valueText,
        trailTimeScale_.label,
        trailTimeScale_.trackbar,
        trailTimeScale_.valueText,
        trailLifetimeMs_.label,
        trailLifetimeMs_.trackbar,
        trailLifetimeMs_.valueText};
    for (const HWND control : advancedTimingControls)
    {
        setPageControlVisible(control, timing);
    }

    const bool particles = advanced
        && activeAdvancedSection_ == AdvancedSection::Particles;
    const std::array advancedParticleControls{
        advancedParticlesHeading_,
        diskRadius_.label,
        diskRadius_.trackbar,
        diskRadius_.valueText,
        diskLifetimeMs_.label,
        diskLifetimeMs_.trackbar,
        diskLifetimeMs_.valueText,
        ringsHdrIntensity_.label,
        ringsHdrIntensity_.trackbar,
        ringsHdrIntensity_.valueText,
        shardsHdrIntensity_.label,
        shardsHdrIntensity_.trackbar,
        shardsHdrIntensity_.valueText,
        trailOpacity_.label,
        trailOpacity_.trackbar,
        trailOpacity_.valueText,
        themeColorLabel_,
        themeColorEdit_,
        themeColorPreview_,
        themeColorChoose_};
    for (const HWND control : advancedParticleControls)
    {
        setPageControlVisible(control, particles);
    }

    const bool rings = advanced
        && activeAdvancedSection_ == AdvancedSection::Rings;
    const std::array advancedRingControls{
        advancedRingsHeading_,
        ringsCount_.label,
        ringsCount_.trackbar,
        ringsCount_.valueText,
        ringsLifetimeMs_.label,
        ringsLifetimeMs_.trackbar,
        ringsLifetimeMs_.valueText,
        ringsRadiusMin_.label,
        ringsRadiusMin_.trackbar,
        ringsRadiusMin_.valueText,
        ringsRadiusMax_.label,
        ringsRadiusMax_.trackbar,
        ringsRadiusMax_.valueText,
        ringsAngularVelocityMultiplier_.label,
        ringsAngularVelocityMultiplier_.trackbar,
        ringsAngularVelocityMultiplier_.valueText,
        ringsRotationDirection_.label,
        ringsRotationDirection_.trackbar,
        ringsRotationDirection_.valueText};
    for (const HWND control : advancedRingControls)
    {
        setPageControlVisible(control, rings);
    }

    const bool clickShards = advanced
        && activeAdvancedSection_ == AdvancedSection::ClickShards;
    const std::array advancedClickShardControls{
        advancedClickShardsHeading_,
        shardsClickCount_.label,
        shardsClickCount_.trackbar,
        shardsClickCount_.valueText,
        shardsClickLifetimeMinMs_.label,
        shardsClickLifetimeMinMs_.trackbar,
        shardsClickLifetimeMinMs_.valueText,
        shardsClickLifetimeMaxMs_.label,
        shardsClickLifetimeMaxMs_.trackbar,
        shardsClickLifetimeMaxMs_.valueText,
        shardsClickRadius_.label,
        shardsClickRadius_.trackbar,
        shardsClickRadius_.valueText,
        shardsClickSpeedMin_.label,
        shardsClickSpeedMin_.trackbar,
        shardsClickSpeedMin_.valueText,
        shardsClickSpeedMax_.label,
        shardsClickSpeedMax_.trackbar,
        shardsClickSpeedMax_.valueText,
        shardsSizeMin_.label,
        shardsSizeMin_.trackbar,
        shardsSizeMin_.valueText,
        shardsSizeMax_.label,
        shardsSizeMax_.trackbar,
        shardsSizeMax_.valueText};
    for (const HWND control : advancedClickShardControls)
    {
        setPageControlVisible(control, clickShards);
    }

    const bool bloom = advanced
        && activeAdvancedSection_ == AdvancedSection::Bloom;
    const std::array advancedBloomControls{
        advancedBloomHeading_,
        bloomDiffusion_.label,
        bloomDiffusion_.trackbar,
        bloomDiffusion_.valueText,
        bloomThreshold_.label,
        bloomThreshold_.trackbar,
        bloomThreshold_.valueText,
        bloomSoftKnee_.label,
        bloomSoftKnee_.trackbar,
        bloomSoftKnee_.valueText,
        bloomClamp_.label,
        bloomClamp_.trackbar,
        bloomClamp_.valueText};
    for (const HWND control : advancedBloomControls)
    {
        setPageControlVisible(control, bloom);
    }

    const bool layers = advanced
        && activeAdvancedSection_ == AdvancedSection::Layers;
    const std::array advancedLayerControls{
        advancedLayersHeading_,
        diskLayerEnabled_,
        ringsLayerEnabled_,
        clickShardsLayerEnabled_,
        trailShardsLayerEnabled_,
        trailLayerEnabled_,
        bloomLayerEnabled_};
    for (const HWND control : advancedLayerControls)
    {
        setPageControlVisible(control, layers);
    }

    const std::array displayControls{
        displaySettingsHeading_,
        displaySelectorLabel_,
        displaySelector_,
        displaySummaryText_,
        hdrEnabled_,
        activeFxRoiEnabled_,
        framePacingLabel_,
        framePacing_,
        displayIndependent_,
        displayEffectsEnabled_,
        displayHdrEnabled_,
        displayFramePacingLabel_,
        displayFramePacing_,
        displayDetailsHeading_,
        displayDetailsText_,
        activeFxRoiDetailsHeading_,
        activeFxRoiDetailsText_};
    for (const HWND control : displayControls)
    {
        setPageControlVisible(control, display);
    }

    const std::array systemControls{
        systemSettingsHeading_,
        languageLabel_,
        languageSelector_,
        startWithWindows_,
        startMinimized_,
        closeToTray_,
        versionUpdateHeading_,
        controlCenterVersionText_,
        hostVersionText_,
        installStateText_,
        latestVersionText_,
        checkForUpdatesButton_,
        openReleaseButton_,
        repositoryStarHint_,
        openRepositoryButton_,
#if defined(BAFX_ENABLE_SPOUT2)
        spout2Enabled_,
        spout2SenderStatus_,
        obsSpoutPluginStatus_,
        spout2ObsHint_,
        refreshObsSpoutPluginButton_,
        openObsSpoutPluginPageButton_,
#endif
        clearLogsButton_};
    for (const HWND control : systemControls)
    {
        setPageControlVisible(control, system);
    }

    if (window_ != nullptr)
    {
        RECT client{};
        if (GetClientRect(window_, &client) != FALSE)
        {
            layoutControls(
                client.right - client.left,
                client.bottom - client.top);
            return;
        }
        redrawWindowTree();
    }
}

int ControlCenterWindow::scale(const int logicalPixels) const noexcept
{
    return MulDiv(logicalPixels, static_cast<int>(layoutDpi_), 96);
}

void ControlCenterWindow::onCommand(
    const int id,
    const int notificationCode)
{
    if (notificationCode == BN_CLICKED && onHotkeyCommand(id))
    {
        return;
    }
    if (updatingControls_)
    {
        return;
    }

    switch (static_cast<ControlId>(id))
    {
    case ControlId::Language:
        if (notificationCode == CBN_SELCHANGE)
        {
            changeLanguage();
        }
        break;
    case ControlId::BasicPage:
        if (notificationCode == BN_CLICKED)
        {
            selectPage(Page::Basic);
        }
        break;
    case ControlId::AdvancedPage:
        if (notificationCode == BN_CLICKED)
        {
            selectPage(Page::Advanced);
        }
        break;
    case ControlId::DisplayPage:
        if (notificationCode == BN_CLICKED)
        {
            selectPage(Page::DisplayPerformance);
        }
        break;
    case ControlId::SystemPage:
        if (notificationCode == BN_CLICKED)
        {
            selectPage(Page::System);
#if defined(BAFX_ENABLE_SPOUT2)
            refreshObsPluginStatus();
#endif
        }
        break;
    case ControlId::HotkeysPage:
        if (notificationCode == BN_CLICKED)
        {
            selectPage(Page::Hotkeys);
        }
        break;
    case ControlId::AdvancedTimingSection:
        if (notificationCode == BN_CLICKED)
        {
            selectAdvancedSection(AdvancedSection::Timing);
        }
        break;
    case ControlId::AdvancedParticlesSection:
        if (notificationCode == BN_CLICKED)
        {
            selectAdvancedSection(AdvancedSection::Particles);
        }
        break;
    case ControlId::AdvancedRingsSection:
        if (notificationCode == BN_CLICKED)
        {
            selectAdvancedSection(AdvancedSection::Rings);
        }
        break;
    case ControlId::AdvancedClickShardsSection:
        if (notificationCode == BN_CLICKED)
        {
            selectAdvancedSection(AdvancedSection::ClickShards);
        }
        break;
    case ControlId::AdvancedBloomSection:
        if (notificationCode == BN_CLICKED)
        {
            selectAdvancedSection(AdvancedSection::Bloom);
        }
        break;
    case ControlId::AdvancedLayersSection:
        if (notificationCode == BN_CLICKED)
        {
            selectAdvancedSection(AdvancedSection::Layers);
        }
        break;
    case ControlId::Pause:
        if (notificationCode == BN_CLICKED)
        {
            sendCommand(paused_ ? "Resume" : "Pause");
        }
        break;
    case ControlId::EffectsEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch("effects.enabled", isChecked(effectsEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::EffectsMode:
        if (notificationCode == CBN_SELCHANGE)
        {
            const LRESULT selected = SendMessageW(
                effectsMode_,
                CB_GETCURSEL,
                0U,
                0U);
            if (selected == 0)
            {
                applyPatch("performance.effectsMode", "\"full\"");
            }
            else if (selected == 1)
            {
                applyPatch("performance.effectsMode", "\"core\"");
            }
            else
            {
                setError(TextId::UnknownEffectsMode);
            }
        }
        break;
    case ControlId::ClickEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch("effects.clickEnabled", isChecked(clickEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::TrailEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch("effects.trailEnabled", isChecked(trailEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::DiskLayerEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "effects.diskLayerEnabled",
                isChecked(diskLayerEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::RingsLayerEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "effects.ringsLayerEnabled",
                isChecked(ringsLayerEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::ClickShardsLayerEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "effects.clickShardsLayerEnabled",
                isChecked(clickShardsLayerEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::TrailShardsLayerEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "effects.trailShardsLayerEnabled",
                isChecked(trailShardsLayerEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::TrailLayerEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "effects.trailLayerEnabled",
                isChecked(trailLayerEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::BloomLayerEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "effects.bloomLayerEnabled",
                isChecked(bloomLayerEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::TrailAlwaysOn:
        if (notificationCode == BN_CLICKED)
        {
            // The persisted field keeps its historical pressed-only wording;
            // expose the user-facing switch as the positive inverse.
            applyPatch(
                "input.trailOnlyWhilePressed",
                isChecked(trailAlwaysOn_) ? "false" : "true");
        }
        break;
    case ControlId::LeftClickEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "input.leftClick",
                isChecked(leftClickEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::RightClickEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "input.rightClick",
                isChecked(rightClickEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::MiddleClickEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "input.middleClick",
                isChecked(middleClickEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::BloomQuality:
        if (notificationCode == CBN_SELCHANGE)
        {
            switch (SendMessageW(bloomQuality_, CB_GETCURSEL, 0U, 0))
            {
            case 0:
                applyPatch("effects.bloomQuality", "\"low\"");
                break;
            case 1:
                applyPatch("effects.bloomQuality", "\"medium\"");
                break;
            case 2:
                applyPatch("effects.bloomQuality", "\"high\"");
                break;
            case 3:
                applyPatch("effects.bloomQuality", "\"ultra\"");
                break;
            case 4:
                // Custom is a derived state selected by the continuous slider.
                // Selecting it cannot invent a missing diffusion value.
                break;
            default:
                setError(TextId::UnknownBloomQuality);
                break;
            }
        }
        break;
    case ControlId::BackgroundMode:
        if (notificationCode == CBN_SELCHANGE)
        {
            switch (SendMessageW(backgroundMode_, CB_GETCURSEL, 0U, 0))
            {
            case 0:
                applyPatch("background.mode", "\"background-aware\"");
                break;
            case 1:
            {
                const bafx::windows::RecordingCompatibleAvailability availability =
                    bafx::windows::queryRecordingCompatibleAvailability();
                if (!availability.supported)
                {
                    const bool queryFailed =
                        !availability.versionQuerySucceeded;
                    bafx::windows::appendRecordingCompatibleControlCenterDiagnostic(
                        availability,
                        queryFailed
                            ? "recording-compatible-test: version-query-failed"
                            : "recording-compatible-test: unsupported-build",
                        "recording-compatible",
                        bafx::config::toString(config_.background.mode),
                        bafx::windows::recordingCompatibleAvailabilityReasonName(
                            availability.reason),
                        generation_);
                    const int previousIndex = renderModeIndex(
                        config_.background.mode);
                    static_cast<void>(SendMessageW(
                        backgroundMode_,
                        CB_SETCURSEL,
                        previousIndex < 0 ? 0 : previousIndex,
                        0));
                    if (!availability.versionQuerySucceeded)
                    {
                        localizedMessageBox(
                            window_,
                            TextId::RecordingVersionUnknown,
                            TextId::RecordingTest,
                            MB_OK | MB_ICONWARNING);
                    }
                    else
                    {
                        const std::wstring detectedVersion = utf8ToWide(
                            bafx::windows::recordingCompatibleVersionString(
                                availability));
                        const UiMessage message(TextId::RecordingUnsupported, {detectedVersion});
                        localizedMessageBox(
                            window_,
                            message,
                            TextId::RecordingTest,
                            MB_OK | MB_ICONWARNING);
                    }
                    break;
                }
                bafx::windows::appendRecordingCompatibleControlCenterDiagnostic(
                    availability,
                    "recording-compatible-test: selected",
                    "recording-compatible",
                    bafx::config::toString(config_.background.mode),
                    "available",
                    generation_);
                applyPatch("background.mode", "\"recording-compatible\"");
            }
                break;
            case 2:
                applyPatch("background.mode", "\"light-background\"");
                break;
            default:
                setError(TextId::UnknownBackgroundMode);
                break;
            }
        }
        break;
    case ControlId::CursorExcluded:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "background.cursorExcluded",
                isChecked(cursorExcluded_) ? "true" : "false");
        }
        break;
    case ControlId::AllowSystemBorder:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "background.allowSystemBorder",
                isChecked(allowSystemBorder_) ? "true" : "false");
        }
        break;
    case ControlId::IdleOptimization:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "performance.idleOptimization",
                isChecked(idleOptimization_) ? "true" : "false");
        }
        break;
    case ControlId::FxProfileSelector:
        if (notificationCode == CBN_SELCHANGE)
        {
            onFxProfileSelectionChanged();
        }
        break;
    case ControlId::FxProfileName:
        if (notificationCode == EN_CHANGE)
        {
            const std::optional<std::string> draft =
                fxProfileNameFromEdit();
            fxProfileNameDraft_ = draft.value_or(std::string{});
            fxProfileNameDirty_ = true;
            updateFxProfileActionState();
        }
        break;
    case ControlId::ApplyFxProfile:
        if (notificationCode == BN_CLICKED)
        {
            applySelectedFxProfile();
        }
        break;
    case ControlId::SaveFxProfile:
        if (notificationCode == BN_CLICKED)
        {
            saveCurrentFxProfile();
        }
        break;
    case ControlId::DeleteFxProfile:
        if (notificationCode == BN_CLICKED)
        {
            deleteSelectedFxProfile();
        }
        break;
    case ControlId::StartWithWindows:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "system.startWithWindows",
                isChecked(startWithWindows_) ? "true" : "false");
        }
        break;
    case ControlId::StartMinimized:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "system.startMinimized",
                isChecked(startMinimized_) ? "true" : "false");
        }
        break;
    case ControlId::CloseToTray:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "system.closeToTray",
                isChecked(closeToTray_) ? "true" : "false");
        }
        break;
    case ControlId::CheckForUpdates:
        if (notificationCode == BN_CLICKED)
        {
            beginManualUpdateCheck();
        }
        break;
    case ControlId::OpenRelease:
        if (notificationCode == BN_CLICKED)
        {
            openOfficialLatestRelease();
        }
        break;
    case ControlId::OpenRepository:
        if (notificationCode == BN_CLICKED)
        {
            openOfficialProjectRepository();
        }
        break;
#if defined(BAFX_ENABLE_SPOUT2)
    case ControlId::Spout2Enabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "system.spout2Enabled",
                isChecked(spout2Enabled_) ? "true" : "false");
        }
        break;
    case ControlId::RefreshObsSpoutPlugin:
        if (notificationCode == BN_CLICKED)
        {
            refreshObsPluginStatus();
        }
        break;
    case ControlId::OpenObsSpoutPluginPage:
        if (notificationCode == BN_CLICKED)
        {
            openObsPluginPage();
        }
        break;
#endif
    case ControlId::DisplaySelector:
        if (notificationCode == CBN_SELCHANGE)
        {
            updateDisplayPolicyControls();
            updateDisplayDetails();
        }
        break;
    case ControlId::HdrEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "display.hdrEnabled",
                isChecked(hdrEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::ActiveFxRoiEnabled:
        if (notificationCode == BN_CLICKED)
        {
            applyPatch(
                "performance.activeFxRoiEnabled",
                isChecked(activeFxRoiEnabled_) ? "true" : "false");
        }
        break;
    case ControlId::FramePacing:
        if (notificationCode == CBN_SELCHANGE)
        {
            const std::optional<bafx::config::FramePacing> framePacing =
                selectedFramePacing(framePacing_);
            if (!framePacing.has_value())
            {
                setError(TextId::UnknownFramePacing);
                break;
            }
            const std::string value = "\""
                + std::string(bafx::config::toString(*framePacing))
                + "\"";
            applyPatch("performance.framePacing", value);
        }
        break;
    case ControlId::DisplayIndependent:
        if (notificationCode == BN_CLICKED)
        {
            if (isChecked(displayIndependent_))
            {
                setSelectedDisplayOverride();
            }
            else
            {
                removeSelectedDisplayOverride();
            }
        }
        break;
    case ControlId::DisplayEffectsEnabled:
    case ControlId::DisplayHdrEnabled:
        if (notificationCode == BN_CLICKED)
        {
            setSelectedDisplayOverride();
        }
        break;
    case ControlId::DisplayFramePacing:
        if (notificationCode == CBN_SELCHANGE)
        {
            setSelectedDisplayOverride();
        }
        break;
    case ControlId::Refresh:
        if (notificationCode == BN_CLICKED)
        {
            static_cast<void>(refreshFromHost());
#if defined(BAFX_ENABLE_SPOUT2)
            refreshObsPluginStatus();
#endif
        }
        break;
    case ControlId::HostLifecycle:
        if (notificationCode == BN_CLICKED)
        {
            if (hostRunning_ || hostStartPending_ || hostMutexPresent())
            {
                stopHost();
            }
            else
            {
                startHostFromBundle();
            }
        }
        break;
    case ControlId::ClearLogs:
        if (notificationCode == BN_CLICKED)
        {
            clearDiagnosticLogs();
        }
        break;
    case ControlId::ResetDefaults:
        if (notificationCode == BN_CLICKED)
        {
            resetDefaults();
        }
        break;
    case ControlId::GlobalScale:
    case ControlId::TrailLength:
    case ControlId::TrailWidth:
    case ControlId::InputSamplingRate:
    case ControlId::BloomIntensity:
    case ControlId::Opacity:
    case ControlId::ClickTimeScale:
    case ControlId::TrailTimeScale:
    case ControlId::TrailLifetimeMs:
    case ControlId::BloomDiffusion:
    case ControlId::BloomThreshold:
    case ControlId::BloomSoftKnee:
    case ControlId::BloomClamp:
    case ControlId::DiskRadius:
    case ControlId::DiskLifetimeMs:
    case ControlId::RingsHdrIntensity:
    case ControlId::RingsCount:
    case ControlId::RingsLifetimeMs:
    case ControlId::RingsRadiusMin:
    case ControlId::RingsRadiusMax:
    case ControlId::RingsAngularVelocityMultiplier:
    case ControlId::RingsRotationDirection:
    case ControlId::ShardsHdrIntensity:
    case ControlId::ShardsClickCount:
    case ControlId::ShardsClickLifetimeMinMs:
    case ControlId::ShardsClickLifetimeMaxMs:
    case ControlId::ShardsClickRadius:
    case ControlId::ShardsClickSpeedMin:
    case ControlId::ShardsClickSpeedMax:
    case ControlId::ShardsSizeMin:
    case ControlId::ShardsSizeMax:
    case ControlId::TrailOpacity:
        break;
    case ControlId::ThemeColorEdit:
        if (notificationCode == EN_KILLFOCUS
            || notificationCode == themeColorReturnNotification)
        {
            commitThemeColor();
        }
        break;
    case ControlId::ThemeColorChoose:
        if (notificationCode == BN_CLICKED)
        {
            chooseThemeColor();
        }
        break;
    }
}

void ControlCenterWindow::onSliderChanged(const HWND trackbar)
{
    if (updatingControls_ || trackbar == nullptr)
    {
        return;
    }

    const std::array sliders{
        &globalScale_,
        &trailLength_,
        &trailWidth_,
        &inputSamplingRate_,
        &bloomIntensity_,
        &opacity_,
        &clickTimeScale_,
        &trailTimeScale_,
        &trailLifetimeMs_,
        &bloomDiffusion_,
        &bloomThreshold_,
        &bloomSoftKnee_,
        &bloomClamp_,
        &diskRadius_,
        &diskLifetimeMs_,
        &ringsHdrIntensity_,
        &ringsCount_,
        &ringsLifetimeMs_,
        &ringsRadiusMin_,
        &ringsRadiusMax_,
        &ringsAngularVelocityMultiplier_,
        &ringsRotationDirection_,
        &shardsHdrIntensity_,
        &shardsClickCount_,
        &shardsClickLifetimeMinMs_,
        &shardsClickLifetimeMaxMs_,
        &shardsClickRadius_,
        &shardsClickSpeedMin_,
        &shardsClickSpeedMax_,
        &shardsSizeMin_,
        &shardsSizeMax_,
        &trailOpacity_};
    for (SliderControl* const slider : sliders)
    {
        if (slider->trackbar == trackbar)
        {
            updateSliderValueText(*slider);
            queueNumberPatch(*slider);
            return;
        }
    }
}

void ControlCenterWindow::commitThemeColor()
{
    if (updatingControls_ || themeColorEdit_ == nullptr)
    {
        return;
    }
    if (!connected_)
    {
        setText(
            themeColorEdit_,
            utf8ToWide(config_.effects.themeColor).c_str());
        setInfo(TextId::HostDisconnected, TextId::StartHostForColor);
        return;
    }

    const int length = GetWindowTextLengthW(themeColorEdit_);
    if (length < 0)
    {
        return;
    }
    std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
    const int copied = GetWindowTextW(
        themeColorEdit_,
        text.data(),
        static_cast<int>(text.size()));
    if (copied < 0)
    {
        return;
    }
    text.resize(static_cast<std::size_t>(copied));
    const std::string value = wideToUtf8(text);
    const std::string valueJson = std::string("\"") + value + "\"";
    if (!commitPendingPatch())
    {
        return;
    }
    applyPatchRequest(fxPatchRequest(
        generation_,
        "effects.themeColor",
        valueJson));
}

void ControlCenterWindow::chooseThemeColor()
{
    if (!connected_)
    {
        setInfo(TextId::HostDisconnected, TextId::StartHostForColor);
        return;
    }

    static COLORREF customColors[16]{};
    CHOOSECOLORW chooser{};
    chooser.lStructSize = sizeof(chooser);
    chooser.hwndOwner = window_;
    chooser.rgbResult = themeColorRef(config_.effects.themeColor);
    chooser.lpCustColors = customColors;
    chooser.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&chooser) == FALSE)
    {
        return;
    }

    char value[8]{};
    const int written = std::snprintf(
        value,
        sizeof(value),
        "#%02x%02x%02x",
        static_cast<unsigned int>(GetRValue(chooser.rgbResult)),
        static_cast<unsigned int>(GetGValue(chooser.rgbResult)),
        static_cast<unsigned int>(GetBValue(chooser.rgbResult)));
    if (written != 7)
    {
        return;
    }
    setText(themeColorEdit_, utf8ToWide(value).c_str());
    commitThemeColor();
}

void ControlCenterWindow::queueNumberPatch(const SliderControl& slider)
{
    // Capture the newly edited control before committing another slider. That
    // commit refreshes the entire UI and would otherwise erase this visible
    // value before it can be queued.
    const std::string path = slider.path;
    const std::string valueJson = numberJson(sliderValue(slider));
    if (pendingPatch_.has_value() && pendingPatch_->path != slider.path)
    {
        // A quick move to another field must not discard the prior setting.
        if (!commitPendingPatch())
        {
            return;
        }
    }
    pendingPatch_ = PendingPatch{generation_, path, valueJson};
    KillTimer(window_, patchTimerId);
    if (SetTimer(window_, patchTimerId, patchDelayMilliseconds, nullptr) == 0U)
    {
        commitPendingPatch();
    }
}

bool ControlCenterWindow::commitPendingPatch()
{
    KillTimer(window_, patchTimerId);
    if (!pendingPatch_.has_value())
    {
        return true;
    }

    PendingPatch patch = std::move(*pendingPatch_);
    pendingPatch_.reset();
    return applyPatchRequest(patchRequest(
        patch.generation,
        patch.path,
        patch.valueJson));
}

bool ControlCenterWindow::readyForFxProfileMutation()
{
    const bool hadPendingPatch = pendingPatch_.has_value();
    if (!commitPendingPatch())
    {
        return false;
    }
    if (!hadPendingPatch)
    {
        return true;
    }

    // A slider commit refreshes both the catalog and current effects. Requiring
    // a second click makes the user revalidate the Profile identity instead of
    // carrying an earlier confirmation across that refresh.
    setInfo(
        TextId::ParametersSavedFirst,
        TextId::ParametersSavedHint);
    return false;
}

bool ControlCenterWindow::applyFxProfileMutationRequest(std::string command)
{
    if (!connected_)
    {
        setInfo(TextId::HostDisconnected, TextId::StartHostAndRefresh);
        return false;
    }

    const bafx::windows::IpcClientResponse response = client_.transact(command);
    if (response.succeeded())
    {
        // The Host has committed the mutation even if the following read is
        // interrupted. Clear stale drafts before the single refresh so retrying
        // cannot accidentally repeat an already-completed operation.
        fxProfileSelectionDirty_ = false;
        fxProfileNameDirty_ = false;
        selectedFxProfileDraft_.reset();
        static_cast<void>(refreshFromHost());
        return true;
    }
    if (response.errorCode == "generation_conflict")
    {
        static_cast<void>(refreshFromHost());
        setInfo(TextId::ConfigChanged, TextId::ConfigRefreshed);
        return false;
    }

    const UiMessage error = describeResponse(response);
    static_cast<void>(refreshFromHost());
    setError(error);
    return false;
}

void ControlCenterWindow::beginManualUpdateCheck()
{
    if (updateChecker_ == nullptr)
    {
        setError(TextId::CheckerUnavailable);
        return;
    }

    // This is the only UI entry that starts network work. Construction,
    // normal startup and --startup deliberately remain offline.
    if (!updateChecker_->start())
    {
        pollManualUpdateCheck();
        return;
    }

    if (SetTimer(
            window_,
            updateCheckTimerId,
            updateCheckPollDelayMilliseconds,
            nullptr) == 0U)
    {
        updateChecker_->cancel();
        setText(latestVersionText_, TextId::LatestCheckFailed);
        EnableWindow(checkForUpdatesButton_, TRUE);
        EnableWindow(openReleaseButton_, FALSE);
        setError(TextId::CheckerMonitorFailed);
        return;
    }

    pollManualUpdateCheck();
}

void ControlCenterWindow::pollManualUpdateCheck()
{
    if (updateChecker_ == nullptr)
    {
        return;
    }

    const bafx::release_update::UpdateCheckSnapshot snapshot =
        updateChecker_->snapshot();
    const bool checking =
        snapshot.status == bafx::release_update::UpdateCheckStatus::Checking;
    const bool updateAvailable =
        snapshot.status
        == bafx::release_update::UpdateCheckStatus::UpdateAvailable;
    EnableWindow(checkForUpdatesButton_, checking ? FALSE : TRUE);
    EnableWindow(openReleaseButton_, updateAvailable ? TRUE : FALSE);

    UiMessage latestText(TextId::LatestVersionLabel);
    switch (snapshot.status)
    {
    case bafx::release_update::UpdateCheckStatus::Idle:
        latestText += TextId::NotChecked;
        break;
    case bafx::release_update::UpdateCheckStatus::Checking:
        latestText += TextId::Checking;
        break;
    case bafx::release_update::UpdateCheckStatus::Current:
        latestText += utf8ToWide(snapshot.latestTagName);
        latestText += TextId::CurrentVersionSuffix;
        break;
    case bafx::release_update::UpdateCheckStatus::UpdateAvailable:
        latestText += utf8ToWide(snapshot.latestTagName);
        latestText += TextId::UpdateAvailableSuffix;
        break;
    case bafx::release_update::UpdateCheckStatus::Ahead:
        latestText += utf8ToWide(snapshot.latestTagName);
        latestText += TextId::AheadVersionSuffix;
        break;
    case bafx::release_update::UpdateCheckStatus::Failed:
        latestText += TextId::CheckFailed;
        break;
    }
    setText(latestVersionText_, latestText);

    if (checking)
    {
        if (snapshot.sequence != lastUpdateSequence_)
        {
            setInfo(
                TextId::CheckingUpdates,
                TextId::CheckUpdatesHint);
        }
        lastUpdateSequence_ = snapshot.sequence;
        return;
    }

    KillTimer(window_, updateCheckTimerId);
    if (snapshot.sequence == lastUpdateSequence_)
    {
        return;
    }
    lastUpdateSequence_ = snapshot.sequence;
    switch (snapshot.status)
    {
    case bafx::release_update::UpdateCheckStatus::Current:
        setInfo(TextId::UpToDate, latestText);
        break;
    case bafx::release_update::UpdateCheckStatus::UpdateAvailable:
        setInfo(
            TextId::UpdateAvailable,
            TextId::UpdateAvailableHint);
        break;
    case bafx::release_update::UpdateCheckStatus::Ahead:
        setInfo(TextId::LocalVersionAhead, TextId::LocalVersionAheadHint);
        break;
    case bafx::release_update::UpdateCheckStatus::Failed:
        setInfo(
            TextId::UpdateCheckFailed,
            snapshot.failure.empty()
                ? UiMessage(TextId::UpdateCheckFailedHint)
                : UiMessage(TextId::UpdateCheckFailedHint) + L"\r\n" + utf8ToWide(snapshot.failure));
        break;
    case bafx::release_update::UpdateCheckStatus::Idle:
    case bafx::release_update::UpdateCheckStatus::Checking:
        break;
    }
}

void ControlCenterWindow::updateVersionPresentation()
{
    const auto installation = installationStatePresentation(executableDirectory());
    setText(titleText_, L"BAFX Desktop " + utf8ToWide(bafx::product::version)
        + L" · " + installation.titleLabel);
    setText(controlCenterVersionText_, UiMessage(TextId::ControlCenterVersionLabel)
        + utf8ToWide(bafx::product::version));
    setText(installStateText_, installation.details);
}

void ControlCenterWindow::openOfficialLatestRelease()
{
    if (updateChecker_ == nullptr
        || updateChecker_->snapshot().status
            != bafx::release_update::UpdateCheckStatus::UpdateAvailable)
    {
        return;
    }

    // Network response URLs never reach shell navigation.
    if (!openFixedOfficialPage(
            window_,
            bafx::release_update::officialLatestReleasePageUrl().data()))
    {
        setError(TextId::ReleaseOpenFailed);
    }
}

void ControlCenterWindow::openOfficialProjectRepository()
{
    if (!openFixedOfficialPage(
            window_,
            bafx::release_update::officialProjectRepositoryUrl().data()))
    {
        setError(TextId::RepositoryOpenFailed);
    }
}

void ControlCenterWindow::onTimer(const UINT_PTR timerId)
{
    if (timerId == hotkeyTimerId)
    {
        if (activePage_ == Page::Hotkeys && connected_ && IsWindowVisible(window_) && !IsIconic(window_))
        {
            const std::string command = hotkeyCaptureToken_ == 0U
                ? "GetHotkeyState"
                : "GetHotkeyState " + std::to_string(hotkeyCaptureToken_);
            static_cast<void>(refreshHotkeys(command));
        }
        return;
    }
    if (timerId == displayStateTimerId)
    {
        if (activePage_ != Page::DisplayPerformance || !connected_)
        {
            updateDisplayStatePolling();
            return;
        }
        static_cast<void>(refreshDisplayStateFromHost());
        return;
    }
    if (timerId == updateCheckTimerId)
    {
        pollManualUpdateCheck();
        return;
    }
    if (timerId == patchTimerId)
    {
        commitPendingPatch();
        return;
    }
    if (timerId == hostShutdownTimerId)
    {
        if (!hostShutdownPending_)
        {
            KillTimer(window_, hostShutdownTimerId);
            return;
        }

        bool mutexPresent = hostMutexPresent();
        if (hostLifetimeMutex_.get() == nullptr && mutexPresent)
        {
            const HANDLE lifetimeMutex = OpenMutexW(
                SYNCHRONIZE | MUTEX_MODIFY_STATE,
                FALSE,
                bafx::windows::kHostSingleInstanceMutexName);
            if (lifetimeMutex != nullptr)
            {
                hostLifetimeMutex_.reset(lifetimeMutex);
            }
            // Once the named object exists, the launched process has crossed
            // the startup race even if this process cannot observe its handle.
            hostStartPending_ = false;
        }

        if (hostLifetimeMutex_.get() != nullptr)
        {
            const DWORD waitResult = WaitForSingleObject(hostLifetimeMutex_.get(), 0U);
            if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED)
            {
                // Waiting on a mutex grants ownership, including the abandoned
                // case. Release it before dropping our observation handle.
                static_cast<void>(ReleaseMutex(hostLifetimeMutex_.get()));
                finishHostShutdown();
                return;
            }
            if (waitResult == WAIT_FAILED)
            {
                recoverHostShutdown(
                    TextId::HostExitMonitorLost);
                return;
            }
        }
        else if (!mutexPresent)
        {
            if (!hostStartPending_)
            {
                finishHostShutdown();
                return;
            }
        }

        mutexPresent = hostLifetimeMutex_.get() != nullptr || hostMutexPresent();
        if (mutexPresent && !hostShutdownCommandAcknowledged_)
        {
            const bafx::windows::IpcClientResponse response =
                client_.transact("Shutdown");
            hostShutdownCommandAcknowledged_ = response.succeeded();
        }

        if (hostShutdownDeadlineTicks_ != 0U
            && GetTickCount64() >= hostShutdownDeadlineTicks_)
        {
            if (!mutexPresent)
            {
                finishHostShutdown();
                return;
            }
            // The mutex still prevents duplicate launch. Restore the close
            // button so a failed control service never strands this window.
            recoverHostShutdown(
                TextId::HostShutdownTimeout);
        }
        return;
    }
    if (timerId != hostRetryTimerId)
    {
        return;
    }

    if (hostShutdownPending_ || hostRetryAttempts_ == 0U)
    {
        KillTimer(window_, hostRetryTimerId);
        return;
    }
    if (refreshFromHost())
    {
        KillTimer(window_, hostRetryTimerId);
        hostRetryAttempts_ = 0U;
        hostStartPending_ = false;
        updateHostLifecycleButton();
        return;
    }
    if (hostRetryAttempts_ > 0U)
    {
        --hostRetryAttempts_;
    }
    if (hostRetryAttempts_ == 0U)
    {
        KillTimer(window_, hostRetryTimerId);
        if (hostStartPending_)
        {
            hostStartPending_ = false;
            hostRunning_ = hostMutexPresent();
            updateHostLifecycleButton();
            if (hostRunning_)
            {
                setError(TextId::HostServiceTimeout);
            }
            else
            {
                setError(TextId::HostStartTimeout);
            }
        }
    }
}

bool ControlCenterWindow::refreshFromHost()
{
    hostVersionBlocked_ = false;
    const bool mutexPresent = hostMutexPresent();
    if (!mutexPresent)
    {
        hostRunning_ = hostStartPending_;
        setConnected(false);
        setText(
            hostVersionText_,
            hostStartPending_
                ? TextId::HostVersionStarting
                : TextId::HostVersionStopped);
        if (!hostShutdownPending_ && !hostStartPending_)
        {
            setText(statusText_, TextId::HostNotRunning);
            setInfo(TextId::HostNotRunning, TextId::StartHostHint);
        }
        return false;
    }

    hostRunning_ = true;
    const bafx::windows::IpcClientResponse stateResponse = client_.transact("GetState");
    if (!stateResponse.succeeded())
    {
        setConnected(false);
        setText(hostVersionText_, TextId::HostVersionUnreadable);
        if (!hostShutdownPending_ && !hostStartPending_)
        {
            if (hostRunning_)
            {
                setText(statusText_, TextId::HostServiceUnavailable);
                setInfo(
                    TextId::HostNotReady,
                    TextId::HostNotReadyHint);
            }
            else
            {
                setText(statusText_, TextId::HostNotRunning);
                setInfo(TextId::CannotConnectHost, describeResponse(stateResponse));
            }
        }
        return false;
    }

    hostRunning_ = true;
    const HostStateParseResult state = parseHostState(stateResponse.payload);
    if (!state.succeeded())
    {
        setConnected(false);
        setText(hostVersionText_, TextId::HostVersionInvalidState);
        setText(statusText_, TextId::HostInvalidState);
        setError(utf8ToWide(state.error));
        return false;
    }
    updateHostVersionText(*state.state);
    if (!state.state->settingsCompatible())
    {
        rejectIncompatibleHostVersion(*state.state);
        return false;
    }

    // GetState is the compatibility gate. Never read configuration from a
    // Host that does not explicitly identify as this Control Center version.
    const bafx::windows::IpcClientResponse configResponse =
        client_.transact("GetConfig");
    if (!configResponse.succeeded())
    {
        setConnected(false);
        setText(statusText_, TextId::HostConfigReadFailed);
        setError(describeResponse(configResponse));
        return false;
    }

    const bafx::config::ConfigLoadResult config = bafx::config::parseJson(
        configResponse.payload);
    if (!config.succeeded())
    {
        setConnected(false);
        setText(statusText_, TextId::HostInvalidConfig);
        setError(utf8ToWide(config.message));
        return false;
    }

    const bafx::windows::IpcClientResponse confirmedStateResponse =
        client_.transact("GetState");
    const HostStateParseResult confirmedState = confirmedStateResponse.succeeded()
        ? parseHostState(confirmedStateResponse.payload)
        : HostStateParseResult{};
    if (!confirmedStateResponse.succeeded() || !confirmedState.succeeded())
    {
        setConnected(false);
        setText(
            hostVersionText_,
            confirmedStateResponse.succeeded()
                ? TextId::HostVersionInvalidRecheck
                : TextId::HostVersionRecheckFailed);
        setText(statusText_, TextId::HostStateRecheckFailed);
        setError(confirmedStateResponse.succeeded()
            ? utf8ToWide(confirmedState.error)
            : describeResponse(confirmedStateResponse));
        return false;
    }
    updateHostVersionText(*confirmedState.state);
    if (!confirmedState.state->settingsCompatible())
    {
        rejectIncompatibleHostVersion(*confirmedState.state);
        return false;
    }
    if (confirmedState.state->generation != state.state->generation)
    {
        // GetState and GetConfig are separate pipe records. Retry once when a
        // concurrent mutation lands between them rather than publishing a torn
        // generation/config pair to the controls.
        if (refreshRetrying_)
        {
            setConnected(false);
            setInfo(
                TextId::HostStateChanging,
                TextId::HostStateChangingHint);
            return false;
        }
        refreshRetrying_ = true;
        const bool refreshed = refreshFromHost();
        refreshRetrying_ = false;
        return refreshed;
    }

    displayState_ = {};
    displayStateError_.clear();
    displayStateRefreshWarning_.clear();
    static_cast<void>(refreshDisplayStateFromHost());

    updateControls(*confirmedState.state, config.config);
    return true;
}

bool ControlCenterWindow::refreshDisplayStateFromHost()
{
    const bafx::windows::IpcClientResponse response =
        client_.transact("GetDisplayState");
    UiMessage failure;
    DisplayStateParseResult parsed{};
    if (!response.succeeded())
    {
        failure = UiMessage(TextId::DisplayRefreshFailedPrefix) + describeResponse(response);
    }
    else
    {
        parsed = parseDisplayState(response.payload);
        if (!parsed.succeeded())
        {
            failure = UiMessage(TextId::DisplayInvalidStatePrefix)
                + utf8ToWide(parsed.error);
        }
    }

    if (!failure.empty())
    {
        if (displayState_.sessions.empty()
            && displayState_.offlineOverrides.empty())
        {
            displayStateError_ = std::move(failure);
            displayStateRefreshWarning_.clear();
        }
        else
        {
            // Keep the last valid immutable snapshot visible. A transient IPC
            // error must not erase evidence or mutate any configuration.
            displayStateRefreshWarning_ = std::move(failure);
        }
        updateDisplayControls(config_);
        return false;
    }

    displayState_ = std::move(*parsed.state);
    displayStateError_.clear();
    displayStateRefreshWarning_.clear();
    updateDisplayControls(config_);
    return true;
}

UiMessage::Argument ControlCenterWindow::hostVersionDescription(
    const HostState& state)
{
    switch (state.productVersionStatus)
    {
    case HostProductVersionStatus::Match:
    case HostProductVersionStatus::Mismatch:
        return utf8ToWide(*state.productVersion);
    case HostProductVersionStatus::Missing:
        return TextId::LegacyHostVersion;
    case HostProductVersionStatus::Invalid:
        // Invalid protocol text may contain control characters. Do not echo
        // it into a Win32 label or let it forge an extra status line.
        return TextId::InvalidVersion;
    }
    return TextId::Unrecognized;
}

void ControlCenterWindow::updateHostVersionText(const HostState& state)
{
    const UiMessage::Argument version = hostVersionDescription(state);
    const UiMessage::Argument suffix = state.productVersionStatus == HostProductVersionStatus::Mismatch
        ? UiMessage::Argument(TextId::HostVersionMismatchSuffix) : UiMessage::Argument(std::wstring{});
    setText(hostVersionText_, UiMessage(TextId::HostVersionFormat, {version, suffix}));
}

void ControlCenterWindow::rejectIncompatibleHostVersion(
    const HostState& state)
{
    hostVersionBlocked_ = true;
    hostRunning_ = true;
    KillTimer(window_, patchTimerId);
    pendingPatch_.reset();
    KillTimer(window_, hostRetryTimerId);
    hostRetryAttempts_ = 0U;
    hostStartPending_ = false;
    setConnected(false);
    updateHostVersionText(state);

    setText(statusText_, TextId::IncompatibleHostStatus);
    setInfo(
        TextId::IncompatibleHost,
        UiMessage(TextId::IncompatibleHostMessage, {hostVersionDescription(state)}));
}

void ControlCenterWindow::updateControls(
    const HostState& state,
    const bafx::config::Config& config)
{
    presentationState_ = state;
    generation_ = state.generation;
    paused_ = state.paused;
    config_ = config;
    if (!hotkeyDraftDirty_)
    {
        hotkeyDraft_ = hotkeyBaseline_ = config.hotkeys;
        hotkeyDraftGeneration_ = state.generation;
        hotkeyDraftConflicted_ = false;
    }
    else if (hotkeyBaseline_ == config.hotkeys)
    {
        // Non-hotkey writes share the global generation. Keep a valid shortcut
        // draft based on the same saved bindings eligible for its next save.
        hotkeyDraftGeneration_ = state.generation;
        hotkeyDraftConflicted_ = false;
    }
    else
    {
        hotkeyDraftConflicted_ = true;
    }
    updateHotkeyControls();
    updatingControls_ = true;

    if (config.system.closeToTray)
    {
        static_cast<void>(ensureTrayIcon());
    }
    else
    {
        removeTrayIcon();
    }

    setChecked(effectsEnabled_, config.effects.enabled);
    static_cast<void>(SendMessageW(
        effectsMode_,
        CB_SETCURSEL,
        effectsModeIndex(config.performance.effectsMode),
        0));
    setChecked(clickEnabled_, config.effects.clickEnabled);
    setChecked(trailEnabled_, config.effects.trailEnabled);
    setChecked(diskLayerEnabled_, config.effects.diskLayerEnabled);
    setChecked(ringsLayerEnabled_, config.effects.ringsLayerEnabled);
    setChecked(
        clickShardsLayerEnabled_,
        config.effects.clickShardsLayerEnabled);
    setChecked(
        trailShardsLayerEnabled_,
        config.effects.trailShardsLayerEnabled);
    setChecked(trailLayerEnabled_, config.effects.trailLayerEnabled);
    setChecked(bloomLayerEnabled_, config.effects.bloomLayerEnabled);
    setChecked(trailAlwaysOn_, !config.input.trailOnlyWhilePressed);
    setChecked(leftClickEnabled_, config.input.leftClick);
    setChecked(rightClickEnabled_, config.input.rightClick);
    setChecked(middleClickEnabled_, config.input.middleClick);
    setSliderValue(globalScale_, config.effects.globalScale);
    setSliderValue(trailLength_, config.effects.trailLength);
    setSliderValue(trailWidth_, config.effects.trailWidth);
    setSliderValue(inputSamplingRate_, config.input.samplingRateHz);
    setSliderValue(bloomIntensity_, config.effects.bloomIntensity);
    setSliderValue(opacity_, config.effects.opacity);
    setSliderValue(clickTimeScale_, config.effects.clickTimeScale);
    setSliderValue(trailTimeScale_, config.effects.trailTimeScale);
    setSliderValue(trailLifetimeMs_, config.effects.trailLifetimeMs);
    setSliderValue(bloomDiffusion_, config.effects.bloomDiffusion);
    setSliderValue(bloomThreshold_, config.effects.bloomThreshold);
    setSliderValue(bloomSoftKnee_, config.effects.bloomSoftKnee);
    setSliderValue(bloomClamp_, config.effects.bloomClamp);
    setSliderValue(diskRadius_, config.effects.diskRadius);
    setSliderValue(diskLifetimeMs_, config.effects.diskLifetimeMs);
    setSliderValue(ringsHdrIntensity_, config.effects.ringsHdrIntensity);
    setSliderValue(ringsCount_, config.effects.ringsCount);
    setSliderValue(ringsLifetimeMs_, config.effects.ringsLifetimeMs);
    setSliderValue(ringsRadiusMin_, config.effects.ringsRadiusMin);
    setSliderValue(ringsRadiusMax_, config.effects.ringsRadiusMax);
    setSliderValue(
        ringsAngularVelocityMultiplier_,
        config.effects.ringsAngularVelocityMultiplier);
    setSliderValue(
        ringsRotationDirection_,
        config.effects.ringsRotationDirection);
    setSliderValue(shardsHdrIntensity_, config.effects.shardsHdrIntensity);
    setSliderValue(shardsClickCount_, config.effects.shardsClickCount);
    setSliderValue(
        shardsClickLifetimeMinMs_,
        config.effects.shardsClickLifetimeMinMs);
    setSliderValue(
        shardsClickLifetimeMaxMs_,
        config.effects.shardsClickLifetimeMaxMs);
    setSliderValue(shardsClickRadius_, config.effects.shardsClickRadius);
    setSliderValue(
        shardsClickSpeedMin_,
        config.effects.shardsClickSpeedMin);
    setSliderValue(
        shardsClickSpeedMax_,
        config.effects.shardsClickSpeedMax);
    setSliderValue(shardsSizeMin_, config.effects.shardsSizeMin);
    setSliderValue(shardsSizeMax_, config.effects.shardsSizeMax);
    setSliderValue(trailOpacity_, config.effects.trailOpacity);
    setText(
        themeColorEdit_,
        utf8ToWide(config.effects.themeColor).c_str());
    InvalidateRect(themeColorPreview_, nullptr, TRUE);
    static_cast<void>(SendMessageW(
        bloomQuality_,
        CB_SETCURSEL,
        qualityIndex(bafx::config::bloomQualityForDiffusion(
            config.effects.bloomDiffusion)),
        0));
    static_cast<void>(SendMessageW(
        backgroundMode_,
        CB_SETCURSEL,
        renderModeIndex(config.background.mode),
        0));
    setChecked(cursorExcluded_, config.background.cursorExcluded);
    setChecked(
        allowSystemBorder_,
        config.background.allowSystemBorder);
    setChecked(idleOptimization_, config.performance.idleOptimization);
    updateFxProfileControls(state);
    setChecked(
        activeFxRoiEnabled_,
        config.performance.activeFxRoiEnabled);
    setChecked(startWithWindows_, config.system.startWithWindows);
    setChecked(startMinimized_, config.system.startMinimized);
    setChecked(closeToTray_, config.system.closeToTray);
#if defined(BAFX_ENABLE_SPOUT2)
    setChecked(spout2Enabled_, config.system.spout2Enabled);
    updateSpout2Status(state);
#endif
    updateDisplayControls(config);
    setText(pauseButton_, paused_ ? TextId::ResumeEffects : TextId::PauseEffects);

    updatingControls_ = false;
    hostRunning_ = true;
    setConnected(true);
    setText(statusText_, UiMessage(TextId::ConnectedStatus,
        {paused_ ? TextId::Paused : TextId::Running,
            state.backgroundCapture == "active" ? UiMessage::Argument(TextId::BackgroundActive)
                : (state.backgroundCapture == "fallback-fx-only" ? UiMessage::Argument(TextId::BackgroundFallback)
                    : UiMessage::Argument(utf8ToWide(state.backgroundCapture)))}));
    if (!hostShutdownPending_)
    {
        if (!state.fxProfileWarning.empty())
        {
            setInfo(
                TextId::ProfilesSkipped,
                TextId::ProfilesSkippedHint);
        }
        else
        {
            clearInfo();
        }
    }
}

void ControlCenterWindow::updateFxProfileControls(const HostState& state)
{
    fxProfiles_ = state.fxProfiles;
    static_cast<void>(SendMessageW(
        fxProfileSelector_,
        CB_RESETCONTENT,
        0U,
        0));
    static_cast<void>(SendMessageW(
        fxProfileSelector_,
        CB_ADDSTRING,
        0U,
        reinterpret_cast<LPARAM>(tr(TextId::Custom))));

    LRESULT activeIndex = 0;
    for (std::size_t index = 0U; index < fxProfiles_.size(); ++index)
    {
        const FxProfileState& profile = fxProfiles_[index];
        const std::wstring name = profileDisplayName(profile.name, profile.builtIn);
        static_cast<void>(SendMessageW(
            fxProfileSelector_,
            CB_ADDSTRING,
            0U,
            reinterpret_cast<LPARAM>(name.c_str())));
        if (profile.name == state.activeFxProfile)
        {
            activeIndex = static_cast<LRESULT>(index + 1U);
        }
    }

    LRESULT selectedIndex = activeIndex;
    if (fxProfileSelectionDirty_)
    {
        if (!selectedFxProfileDraft_.has_value())
        {
            selectedIndex = 0;
        }
        else
        {
            const FxProfileState* const drafted = findFxProfile(
                *selectedFxProfileDraft_);
            if (drafted != nullptr)
            {
                selectedIndex = static_cast<LRESULT>(
                    drafted - fxProfiles_.data() + 1);
            }
            else
            {
                // An external delete invalidates the explicit selection. Fall
                // back to the Host's active value rather than keeping a dead
                // combo-box index.
                fxProfileSelectionDirty_ = false;
                selectedFxProfileDraft_.reset();
            }
        }
    }
    static_cast<void>(SendMessageW(
        fxProfileSelector_,
        CB_SETCURSEL,
        static_cast<WPARAM>(selectedIndex),
        0));

    const FxProfileState* const active = selectedFxProfile();
    if (!fxProfileNameDirty_)
    {
        std::wstring editableName;
        if (active != nullptr && !active->builtIn)
        {
            fxProfileNameDraft_ = active->name;
            editableName = utf8ToWide(active->name);
        }
        else
        {
            fxProfileNameDraft_.clear();
        }
        setText(fxProfileNameEdit_, editableName.c_str());
    }
    updateFxProfileActionState();
}

void ControlCenterWindow::updateFxProfileActionState() const noexcept
{
    const bool profileControlsEnabled = connected_;
    const FxProfileState* const selected = selectedFxProfile();
    const bool hasName = fxProfileNameEdit_ != nullptr
        && GetWindowTextLengthW(fxProfileNameEdit_) > 0;

    EnableWindow(
        fxProfileSelector_,
        profileControlsEnabled ? TRUE : FALSE);
    EnableWindow(
        fxProfileNameEdit_,
        profileControlsEnabled ? TRUE : FALSE);
    EnableWindow(
        applyFxProfileButton_,
        profileControlsEnabled && selected != nullptr ? TRUE : FALSE);
    EnableWindow(
        saveFxProfileButton_,
        profileControlsEnabled && hasName ? TRUE : FALSE);
    EnableWindow(
        deleteFxProfileButton_,
        profileControlsEnabled
                && selected != nullptr
                && !selected->builtIn
            ? TRUE
            : FALSE);
}

void ControlCenterWindow::onFxProfileSelectionChanged()
{
    const FxProfileState* const selected = selectedFxProfile();
    fxProfileSelectionDirty_ = true;
    selectedFxProfileDraft_ = selected == nullptr
        ? std::nullopt
        : std::optional<std::string>(selected->name);
    const std::wstring editableName = selected != nullptr && !selected->builtIn
        ? utf8ToWide(selected->name)
        : std::wstring{};
    setText(fxProfileNameEdit_, editableName.c_str());
    fxProfileNameDraft_ = selected != nullptr && !selected->builtIn
        ? selected->name
        : std::string{};
    fxProfileNameDirty_ = false;
    updateFxProfileActionState();
}

void ControlCenterWindow::applySelectedFxProfile()
{
    if (!connected_)
    {
        setInfo(TextId::HostDisconnected, TextId::StartHostForProfile);
        return;
    }
    if (!readyForFxProfileMutation())
    {
        return;
    }
    const FxProfileState* const selected = selectedFxProfile();
    if (selected == nullptr)
    {
        setInfo(TextId::NoProfileSelected, TextId::SelectSavedProfile);
        return;
    }

    const std::string name = selected->name;
    static_cast<void>(applyFxProfileMutationRequest(fxProfileRequest(
        "ApplyFxProfile",
        generation_,
        name)));
}

void ControlCenterWindow::saveCurrentFxProfile()
{
    if (!connected_)
    {
        setInfo(TextId::HostDisconnected, TextId::StartHostToSaveProfile);
        return;
    }
    if (!readyForFxProfileMutation())
    {
        return;
    }
    const std::optional<std::string> name = fxProfileNameFromEdit();
    if (!name.has_value())
    {
        setInfo(TextId::EmptyProfileName, TextId::ProfileNameLengthHint);
        return;
    }

    const FxProfileState* const existing = findFxProfile(*name);
    if (*name == "自定义" || (existing != nullptr && existing->builtIn))
    {
        setInfo(
            TextId::ProfileNameUnavailable,
            TextId::ReservedProfileNameHint);
        return;
    }
    if (existing != nullptr)
    {
        const UiMessage message(TextId::OverwriteProfileQuestion, {utf8ToWide(*name)});
        if (localizedMessageBox(
                window_,
                message,
                TextId::OverwriteProfile,
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        {
            return;
        }
    }

    static_cast<void>(applyFxProfileMutationRequest(fxProfileRequest(
        "SaveFxProfile",
        generation_,
        *name)));
}

void ControlCenterWindow::deleteSelectedFxProfile()
{
    if (!connected_)
    {
        setInfo(TextId::HostDisconnected, TextId::StartHostToDeleteProfile);
        return;
    }
    if (!readyForFxProfileMutation())
    {
        return;
    }
    const FxProfileState* const selected = selectedFxProfile();
    if (selected == nullptr)
    {
        setInfo(TextId::NoProfileSelected, TextId::SelectCustomProfile);
        return;
    }
    if (selected->builtIn)
    {
        setInfo(TextId::BuiltinProfileProtected, TextId::DeleteCustomOnly);
        return;
    }

    const std::string name = selected->name;
    const UiMessage message(TextId::DeleteProfileQuestion, {utf8ToWide(name)});
    if (localizedMessageBox(
            window_,
            message,
            TextId::DeleteProfile,
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
    {
        return;
    }

    static_cast<void>(applyFxProfileMutationRequest(fxProfileRequest(
        "DeleteFxProfile",
        generation_,
        name)));
}

const FxProfileState* ControlCenterWindow::selectedFxProfile() const noexcept
{
    if (fxProfileSelector_ == nullptr)
    {
        return nullptr;
    }
    const LRESULT selected = SendMessageW(
        fxProfileSelector_,
        CB_GETCURSEL,
        0U,
        0);
    if (selected <= 0)
    {
        return nullptr;
    }
    const std::size_t profileIndex = static_cast<std::size_t>(selected - 1);
    return profileIndex < fxProfiles_.size()
        ? &fxProfiles_[profileIndex]
        : nullptr;
}

const FxProfileState* ControlCenterWindow::findFxProfile(
    const std::string_view name) const
{
    const std::wstring requested = utf8ToWide(name);
    if (requested.empty())
    {
        return nullptr;
    }
    for (const FxProfileState& profile : fxProfiles_)
    {
        const std::wstring existing = utf8ToWide(profile.name);
        if (!existing.empty()
            && CompareStringOrdinal(
                requested.data(),
                static_cast<int>(requested.size()),
                existing.data(),
                static_cast<int>(existing.size()),
                TRUE) == CSTR_EQUAL)
        {
            return &profile;
        }
    }
    return nullptr;
}

std::optional<std::string> ControlCenterWindow::fxProfileNameFromEdit() const
{
    if (fxProfileNameEdit_ == nullptr)
    {
        return std::nullopt;
    }
    const int length = GetWindowTextLengthW(fxProfileNameEdit_);
    if (length <= 0)
    {
        return std::nullopt;
    }

    std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
    const int copied = GetWindowTextW(
        fxProfileNameEdit_,
        text.data(),
        static_cast<int>(text.size()));
    if (copied <= 0)
    {
        return std::nullopt;
    }
    text.resize(static_cast<std::size_t>(copied));
    std::string converted = wideToUtf8(text);
    if (converted.empty())
    {
        return std::nullopt;
    }
    return converted;
}

#if defined(BAFX_ENABLE_SPOUT2)
void ControlCenterWindow::updateSpout2Status(const HostState& state)
{
    std::wstring text = tr(TextId::SenderStatusLabel)
        + spout2StatusText(state.spout2Status)
        + tr(TextId::NameColumn)
        + utf8ToWide(state.spout2Sender);
    if (!state.spout2Error.empty())
    {
        text += tr(TextId::ErrorLine) + utf8ToWide(state.spout2Error);
    }
    else if (state.spout2OutputContract
        != bafx::windows::spout2OutputContract)
    {
        text += tr(TextId::OutputContractMismatch)
            + utf8ToWide(state.spout2OutputContract);
    }
    else
    {
        text += tr(TextId::SpoutOutputDescription);
        text += tr(TextId::SpoutObsAlphaHint);
    }
    setText(spout2SenderStatus_, text.c_str());
}

void ControlCenterWindow::refreshObsPluginStatus()
{
    obsPluginState_ = probeObsSpoutPlugin();
    updateObsPluginPresentation();
}

void ControlCenterWindow::updateObsPluginPresentation()
{
    if (!obsPluginState_.has_value())
    {
        return;
    }
    const ObsSpoutPluginProbeResult& result = *obsPluginState_;
    std::wstring text;
    switch (result.state)
    {
    case ObsSpoutPluginState::Missing:
        text = tr(TextId::ObsPluginMissing);
        break;
    case ObsSpoutPluginState::InstalledObsNotRunning:
        text = tr(TextId::ObsInstalledNotRunning);
        break;
    case ObsSpoutPluginState::Loaded:
        text = tr(TextId::ObsPluginLoaded);
        break;
    case ObsSpoutPluginState::InstalledNotLoaded:
        text = tr(TextId::ObsPluginNotLoaded);
        break;
    case ObsSpoutPluginState::InspectionUnavailable:
        text = tr(TextId::ObsInspectionUnavailable);
        break;
    }

    if (!result.pluginVersion.empty())
    {
        text += L" | v" + utf8ToWide(result.pluginVersion);
    }
    if (!result.pluginArchitecture.empty())
    {
        text += L" | " + utf8ToWide(result.pluginArchitecture);
    }
    if (!result.pluginPath.empty())
    {
        text += tr(TextId::LocationLine) + result.pluginPath.native();
    }
    setText(obsSpoutPluginStatus_, text.c_str());
}

void ControlCenterWindow::openObsPluginPage()
{
    if (!openFixedOfficialPage(window_, obsSpoutPluginPage))
    {
        setError(TextId::ObsPluginOpenFailed);
    }
}
#endif

void ControlCenterWindow::updateDisplayControls(
    const bafx::config::Config& config)
{
    setChecked(hdrEnabled_, config.display.hdrEnabled);
    static_cast<void>(SendMessageW(
        framePacing_,
        CB_SETCURSEL,
        framePacingIndex(config.performance.framePacing),
        0));

    static_cast<void>(SendMessageW(displaySelector_, CB_RESETCONTENT, 0U, 0));
    if (!displayStateError_.empty()
        || (displayState_.sessions.empty()
            && displayState_.offlineOverrides.empty()))
    {
        updateDisplayPolicyControls();
        updateDisplayDetails();
        return;
    }

    LRESULT selectedIndex = CB_ERR;
    LRESULT primaryIndex = CB_ERR;
    LRESULT coordinatorIndex = CB_ERR;
    for (std::size_t index = 0U;
         index < displayState_.sessions.size();
         ++index)
    {
        const DisplaySessionState& session = displayState_.sessions[index];
        std::wstring label = utf8ToWide(session.device);
        if (!session.monitor.empty())
        {
            label += L" | " + utf8ToWide(session.monitor);
        }
        if (session.primary && session.coordinator)
        {
            label += tr(TextId::PrimaryCoordinatorSuffix);
        }
        else if (session.primary)
        {
            label += tr(TextId::PrimaryDisplaySuffix);
        }
        else if (session.coordinator)
        {
            label += tr(TextId::CoordinatorSuffix);
        }

        const LRESULT comboIndex = SendMessageW(
            displaySelector_,
            CB_ADDSTRING,
            0U,
            reinterpret_cast<LPARAM>(label.c_str()));
        if (comboIndex == CB_ERR || comboIndex == CB_ERRSPACE)
        {
            displayStateError_ = TextId::DisplayAllocationFailed;
            static_cast<void>(SendMessageW(
                displaySelector_,
                CB_RESETCONTENT,
                0U,
                0));
            updateDisplayDetails();
            return;
        }
        static_cast<void>(SendMessageW(
            displaySelector_,
            CB_SETITEMDATA,
            static_cast<WPARAM>(comboIndex),
            static_cast<LPARAM>(index)));

        if (displaySessionIdentity(session) == selectedDisplayIdentity_)
        {
            selectedIndex = comboIndex;
        }
        if (session.primary && primaryIndex == CB_ERR)
        {
            primaryIndex = comboIndex;
        }
        if (session.coordinator && coordinatorIndex == CB_ERR)
        {
            coordinatorIndex = comboIndex;
        }
    }

    for (std::size_t index = 0U;
         index < displayState_.offlineOverrides.size();
         ++index)
    {
        const bafx::config::DisplayOverrideConfig& overrideConfig =
            displayState_.offlineOverrides[index];
        const std::wstring label = tr(TextId::OfflineOverridePrefix)
            + utf8ToWide(overrideConfig.displayKey);
        const LRESULT comboIndex = SendMessageW(
            displaySelector_,
            CB_ADDSTRING,
            0U,
            reinterpret_cast<LPARAM>(label.c_str()));
        if (comboIndex == CB_ERR || comboIndex == CB_ERRSPACE)
        {
            displayStateError_ = TextId::DisplayAllocationFailed;
            static_cast<void>(SendMessageW(
                displaySelector_,
                CB_RESETCONTENT,
                0U,
                0));
            updateDisplayDetails();
            return;
        }

        // Keep disconnected policies outside the active-session index range.
        // The item remains removable without pretending it has runtime state.
        static_cast<void>(SendMessageW(
            displaySelector_,
            CB_SETITEMDATA,
            static_cast<WPARAM>(comboIndex),
            static_cast<LPARAM>(offlineDisplayItemBase + index)));
        if (offlineDisplayIdentity(overrideConfig)
            == selectedDisplayIdentity_)
        {
            selectedIndex = comboIndex;
        }
    }

    if (selectedIndex == CB_ERR)
    {
        selectedIndex = primaryIndex != CB_ERR
            ? primaryIndex
            : coordinatorIndex;
    }
    if (selectedIndex == CB_ERR)
    {
        selectedIndex = 0;
    }
    static_cast<void>(SendMessageW(
        displaySelector_,
        CB_SETCURSEL,
        static_cast<WPARAM>(selectedIndex),
        0));
    updateDisplayPolicyControls();
    updateDisplayDetails();
}

void ControlCenterWindow::updateDisplayPolicyControls() noexcept
{
    const bool wasUpdating = updatingControls_;
    updatingControls_ = true;

    const DisplaySessionState* const session = selectedDisplaySession();
    const bafx::config::DisplayOverrideConfig* const offlineOverride =
        selectedOfflineDisplayOverride();
    const std::string* displayKey = nullptr;
    if (session != nullptr && session->displayKey.has_value())
    {
        displayKey = &*session->displayKey;
    }
    else if (offlineOverride != nullptr)
    {
        displayKey = &offlineOverride->displayKey;
    }

    const bafx::config::DisplayOverrideConfig* overrideConfig = nullptr;
    if (offlineOverride != nullptr)
    {
        overrideConfig = offlineOverride;
    }
    else if (displayKey != nullptr)
    {
        overrideConfig = bafx::config::findDisplayOverride(
            config_.display,
            *displayKey);
    }

    const bool independent = overrideConfig != nullptr;
    bafx::config::ResolvedDisplayPolicy policy = displayKey != nullptr
        ? bafx::config::resolveDisplayPolicy(config_, *displayKey)
        : bafx::config::resolveDisplayPolicy(config_, {});
    if (offlineOverride != nullptr)
    {
        // Offline entries come from the Host's authoritative schema-4 list.
        // Do not replace that runtime fact with a separately fetched config.
        policy.enabled = offlineOverride->enabled;
        policy.hdrEnabled = offlineOverride->hdrEnabled;
        policy.framePacing = offlineOverride->framePacing;
        policy.overridden = true;
    }
    setChecked(displayIndependent_, independent);
    setChecked(displayEffectsEnabled_, policy.enabled);
    setChecked(displayHdrEnabled_, policy.hdrEnabled);
    static_cast<void>(SendMessageW(
        displayFramePacing_,
        CB_SETCURSEL,
        framePacingIndex(policy.framePacing),
        0));

    const bool canWrite = connected_ && displayKey != nullptr;
    EnableWindow(displayIndependent_, canWrite ? TRUE : FALSE);
    const BOOL policyEnabled = canWrite
            && independent
            && offlineOverride == nullptr
        ? TRUE
        : FALSE;
    EnableWindow(displayEffectsEnabled_, policyEnabled);
    EnableWindow(displayHdrEnabled_, policyEnabled);
    EnableWindow(displayFramePacingLabel_, policyEnabled);
    EnableWindow(displayFramePacing_, policyEnabled);

    updatingControls_ = wasUpdating;
}

void ControlCenterWindow::updateDisplayDetails()
{
    updateActiveFxRoiDetails();
    if (!displayStateError_.empty())
    {
        setText(displaySummaryText_, TextId::DisplayStatusUnavailable);
        setText(displayDetailsText_, displayStateError_.render().c_str());
        return;
    }

    std::wostringstream summary;
    summary << tr(TextId::TopologyLabel) << topologyStateText(displayState_.topologyStatus)
            << tr(TextId::SessionsColumn) << displayState_.sessions.size()
            << tr(TextId::OfflineColumn) << displayState_.offlineOverrides.size()
            << tr(TextId::GenerationLine) << displayState_.runtimeGeneration
            << L" / " << displayState_.configGeneration
            << L" / " << displayState_.appliedConfigGeneration;
    setText(displaySummaryText_, summary.str().c_str());

    const DisplaySessionState* const selectedSession = selectedDisplaySession();
    const bafx::config::DisplayOverrideConfig* const offlineOverride =
        selectedOfflineDisplayOverride();
    if (selectedSession == nullptr && offlineOverride == nullptr)
    {
        setText(
            displayDetailsText_,
            displayState_.sessions.empty()
                    && displayState_.offlineOverrides.empty()
                ? TextId::NoDisplaySessions
                : TextId::SelectDisplayHint);
        return;
    }

    if (offlineOverride != nullptr)
    {
        selectedDisplayIdentity_ = offlineDisplayIdentity(*offlineOverride);
        std::wostringstream details;
        details << tr(TextId::GlobalTopologyLabel)
                << topologyStateText(displayState_.topologyStatus)
                << tr(TextId::ErrorColumn)
                << hresultText(static_cast<HRESULT>(
                    displayState_.topologyError))
                << tr(TextId::OfflineAuthoritativeColumn)
                << tr(TextId::OfflineDisplayKeyLine)
                << utf8ToWide(offlineOverride->displayKey)
                << tr(TextId::EffectsLine)
                << (offlineOverride->enabled ? tr(TextId::On) : tr(TextId::Off))
                << tr(TextId::HdrRequestColumn)
                << (offlineOverride->hdrEnabled ? tr(TextId::On) : tr(TextId::Off))
                << L" | "
                << framePacingText(offlineOverride->framePacing)
                << tr(TextId::OfflineDisplayHint);
        setText(displayDetailsText_, details.str().c_str());
        return;
    }

    const DisplaySessionState& session = *selectedSession;
    selectedDisplayIdentity_ = displaySessionIdentity(session);

    const std::int64_t width = static_cast<std::int64_t>(session.right)
        - static_cast<std::int64_t>(session.left);
    const std::int64_t height = static_cast<std::int64_t>(session.bottom)
        - static_cast<std::int64_t>(session.top);
    const std::wstring role = session.primary
        ? (session.coordinator ? tr(TextId::PrimaryCoordinatorRole) : tr(TextId::PrimaryDisplayRole))
        : (session.coordinator ? tr(TextId::CoordinatorRole) : tr(TextId::ExtendedDisplayRole));
    const std::wstring captureState = session.backgroundCaptureActive
        ? tr(TextId::Active)
        : tr(TextId::Inactive);
    const std::wstring restartState = session.backgroundCaptureRestartAllowed
        ? tr(TextId::Allowed)
        : tr(TextId::Disallowed);
    const bafx::config::ResolvedDisplayPolicy policy =
        session.displayKey.has_value()
        ? bafx::config::resolveDisplayPolicy(config_, *session.displayKey)
        : bafx::config::resolveDisplayPolicy(config_, {});
    const std::wstring policySource = policy.overridden
        ? tr(TextId::IndependentPolicy)
        : (session.displayKey.has_value() ? tr(TextId::InheritedPolicy) : tr(TextId::InheritedUnstablePolicy));
    const std::wstring sourceId = session.sourceId.has_value()
        ? std::to_wstring(*session.sourceId)
        : tr(TextId::Unknown);

    std::wstring faultState;
    if (!session.renderFaulted && !session.outputContractFaulted)
    {
        faultState = tr(TextId::None);
    }
    else
    {
        if (session.renderFaulted)
        {
            faultState = tr(TextId::RenderingFault);
        }
        if (session.outputContractFaulted)
        {
            if (!faultState.empty())
            {
                faultState += L"、";
            }
            faultState += tr(TextId::OutputContractFault);
        }
    }

    std::wostringstream details;
    details << tr(TextId::GlobalTopologyLabel)
            << topologyStateText(displayState_.topologyStatus)
            << tr(TextId::ErrorColumn)
            << hresultText(static_cast<HRESULT>(
                displayState_.topologyError))
            << tr(TextId::OfflineListColumn)
            << (displayState_.offlineOverridesAuthoritative
                ? tr(TextId::Authoritative)
                : tr(TextId::TopologyRecoveryPending))
            << tr(TextId::DeviceLine) << utf8ToWide(session.device)
            << L" | " << utf8ToWide(session.monitor)
            << tr(TextId::RoleLine) << role
            << tr(TextId::DisplayKeyColumn)
            << (session.displayKey.has_value()
                ? utf8ToWide(*session.displayKey)
                : tr(TextId::Unknown))
            << tr(TextId::DesktopLine) << width << L" x " << height
            << L" @ (" << session.left << L", " << session.top << L")"
            << L" | DPI：" << session.windowDpi
            << L" / " << session.targetDpiX << L" x " << session.targetDpiY
            << tr(TextId::SourceIdentityLine)
            << (session.sourceAdapterResolved ? tr(TextId::Resolved) : tr(TextId::Unresolved))
            << L" | Source "
            << (session.sourceIdentityResolved ? tr(TextId::Resolved) : tr(TextId::Unresolved))
            << L" | ID " << sourceId
            << tr(TextId::PhysicalTargetsColumn) << session.physicalTargetCount
            << L"\r\nGPU：" << utf8ToWide(session.adapter)
            << tr(TextId::DriverColumn) << driverStateText(session.driver)
            << tr(TextId::ConfiguredRequestLine) << policySource
            << tr(TextId::EffectsColumn) << (policy.enabled ? tr(TextId::On) : tr(TextId::Off))
            << L" | HDR " << (policy.hdrEnabled ? tr(TextId::On) : tr(TextId::Off))
            << L" | " << framePacingText(policy.framePacing)
            << tr(TextId::HostAppliedLine)
            << (session.effectsEnabled ? tr(TextId::On) : tr(TextId::Off))
            << L" | HDR " << (session.hdrEnabled ? tr(TextId::On) : tr(TextId::Off))
            << L" | " << framePacingText(session.framePacing)
            << tr(TextId::RefreshRatesLine) << refreshRateText(session.displayRefresh)
            << tr(TextId::CaptureColumn) << refreshRateText(session.captureRefresh)
            << tr(TextId::CapturePolicyColumn)
            << captureCadenceText(session.captureCadenceStatus)
            << tr(TextId::RefreshPolicyLine)
            << refreshRateText(session.producerPolicyRefresh)
            << L" | freshness "
            << refreshRateText(session.freshnessPolicyRefresh)
            << L" / " << session.freshnessPeriodUs << L" us"
            << L"\r\nWGC producer："
            << producerCadenceText(session.producerCadenceStatus)
            << tr(TextId::RequestedColumn) << session.producerRequestedPeriodUs << L" us"
            << tr(TextId::ActualColumn) << session.producerAppliedPeriodUs << L" us"
            << tr(TextId::ResultColumn)
            << hresultText(static_cast<HRESULT>(session.producerResult))
            << tr(TextId::CadenceFallbackLine)
            << cadenceFallbackText(session.cadenceFallbackReason)
            << tr(TextId::OutputRequestedLine) << outputStateText(session.requestedOutput)
            << tr(TextId::ResolvedColumn) << outputStateText(session.resolvedOutput)
            << tr(TextId::ActualColumn) << outputStateText(session.actualOutput)
            << tr(TextId::OutputMappingLine)
            << outputMappingText(session.resolvedOutputMapping)
            << tr(TextId::ActualColumn)
            << outputMappingText(session.actualOutputMapping)
            << tr(TextId::OutputFallbackLine) << outputFallbackText(session.outputFallback)
            << tr(TextId::ResultColumn)
            << hresultText(static_cast<HRESULT>(session.outputFallbackResult))
            << tr(TextId::PolicySatisfiedColumn)
            << (session.outputPolicySatisfied ? tr(TextId::BooleanYes) : tr(TextId::BooleanNo))
            << tr(TextId::SystemColorLine) << colorStateText(session.colorMode)
            << L" | HDR "
            << optionalBooleanText(session.hdrSupported, tr(TextId::Supported), tr(TextId::Unsupported))
            << L" / "
            << optionalBooleanText(session.hdrActive, tr(TextId::Activated), tr(TextId::NotActivated))
            << tr(TextId::UserSwitchColumn)
            << optionalBooleanText(
                session.hdrUserEnabled,
                tr(TextId::On),
                tr(TextId::Off))
            << tr(TextId::PolicyLimitColumn)
            << optionalBooleanText(
                session.advancedColorLimitedByPolicy,
                tr(TextId::BooleanYes),
                tr(TextId::BooleanNo))
            << tr(TextId::ColorMonitorLine)
            << colorMonitorStateText(session.colorMonitorStatus)
            << L" | HRESULT "
            << hresultText(static_cast<HRESULT>(session.colorMonitorHresult))
            << tr(TextId::MonitorGenerationColumn) << session.colorMonitorGeneration
            << tr(TextId::QueryGenerationColumn) << session.colorQueryGeneration
            << tr(TextId::ColorContractLine)
            << colorSnapshotStateText(session.colorSnapshotDisposition)
            << tr(TextId::CompleteColumn)
            << (session.colorSnapshotComplete ? tr(TextId::BooleanYes) : tr(TextId::BooleanNo))
            << tr(TextId::RetriesColumn) << session.colorRefreshRetriesRemaining
            << tr(TextId::AdvancedColorQueryLine)
            << optionalHresultText(session.advancedColorQueryResult)
            << L"\r\nSDR white level："
            << optionalNitsText(session.sdrWhiteLevelNits)
            << tr(TextId::QueryColumn)
            << optionalHresultText(session.sdrWhiteLevelQueryResult)
            << tr(TextId::RetainedColumn)
            << optionalBooleanText(
                session.sdrWhiteLevelRetained,
                tr(TextId::BooleanYes),
                tr(TextId::BooleanNo))
            << tr(TextId::PhysicalTargetsMatchColumn)
            << optionalBooleanText(
                session.sdrWhiteLevelConsistent,
                tr(TextId::BooleanYes),
                tr(TextId::BooleanNo))
            << tr(TextId::BackgroundCaptureLine) << captureState
            << tr(TextId::RestartColumn) << restartState
            << tr(TextId::RuntimeFaultLine) << faultState;

    for (std::size_t index = 0U;
         index < session.physicalCadence.size();
         ++index)
    {
        const DisplayPhysicalCadenceState& physical =
            session.physicalCadence[index];
        details << tr(TextId::PhysicalTargetLine) << index + 1U
                << tr(TextId::VirtualRateColumn) << refreshRateText(physical.virtualRefresh)
                << tr(TextId::PhysicalRateColumn) << refreshRateText(physical.physicalRefresh)
                << tr(TextId::CaptureColumn) << refreshRateText(physical.captureRefresh)
                << L" | DRR boost "
                << (physical.drrBoosted ? tr(TextId::BooleanYes) : tr(TextId::BooleanNo))
                << tr(TextId::AvailableColumn) << (physical.available ? tr(TextId::BooleanYes) : tr(TextId::BooleanNo));
    }
    if (!session.backgroundCaptureFailure.empty())
    {
        details << tr(TextId::CaptureErrorLine)
                << utf8ToWide(session.backgroundCaptureFailure);
    }
    setText(displayDetailsText_, details.str().c_str());
    static_cast<void>(SendMessageW(displayDetailsText_, EM_SETSEL, 0U, 0));
    static_cast<void>(SendMessageW(displayDetailsText_, EM_SCROLLCARET, 0U, 0));
}

void ControlCenterWindow::updateActiveFxRoiDetails()
{
    if (activeFxRoiDetailsText_ == nullptr)
    {
        return;
    }
    if (!displayStateError_.empty())
    {
        setText(activeFxRoiDetailsText_, displayStateError_.render().c_str());
        return;
    }

    const DisplaySessionState* const session = selectedDisplaySession();
    if (session == nullptr)
    {
        setText(
            activeFxRoiDetailsText_,
            selectedOfflineDisplayOverride() != nullptr
                ? TextId::OfflineRoiHint
                : TextId::SelectActiveDisplayForRoi);
        return;
    }

    const ActiveFxRoiRuntimeState& roi = session->activeFxRoi;
    const bool stale = activeFxRoiSampleIsStale(roi);
    std::wostringstream details;
    details << tr(TextId::StatusLabel) << (stale ? tr(TextId::StaleSample) : tr(TextId::FreshSample))
            << tr(TextId::SwitchColumn) << (roi.enabled ? tr(TextId::On) : tr(TextId::Off))
            << tr(TextId::FrameColumn) << roi.lastFrameId
            << tr(TextId::WindowDurationLine) << roi.sampleWindowMs
            << tr(TextId::SampleAgeColumn) << roi.sampleAgeMs << L" ms"
            << tr(TextId::RoiCompositionHint)
            << tr(TextId::RoiSavingsHint);
    if (!displayStateRefreshWarning_.empty())
    {
        details << tr(TextId::RefreshWarningLine) << displayStateRefreshWarning_.render();
    }
    appendActiveFxRoiPathDetails(details, L"Primary", roi.primary);
    appendActiveFxRoiPathDetails(
        details,
        L"Recording rebuild",
        roi.recordingRebuild);

    setText(activeFxRoiDetailsText_, details.str().c_str());
    static_cast<void>(SendMessageW(
        activeFxRoiDetailsText_,
        EM_SETSEL,
        0U,
        0));
    static_cast<void>(SendMessageW(
        activeFxRoiDetailsText_,
        EM_SCROLLCARET,
        0U,
        0));
}

void ControlCenterWindow::setSelectedDisplayOverride()
{
    const DisplaySessionState* const session = selectedDisplaySession();
    if (session == nullptr || !session->displayKey.has_value())
    {
        updateDisplayPolicyControls();
        setInfo(
            TextId::OverrideSaveFailed,
            TextId::StableDisplayKeyMissing);
        return;
    }

    const std::optional<bafx::config::FramePacing> framePacing =
        selectedFramePacing(displayFramePacing_);
    if (!framePacing.has_value())
    {
        updateDisplayPolicyControls();
        setError(TextId::UnknownDisplayFramePacing);
        return;
    }

    // A pending slider commit refreshes every control. Capture this explicit
    // display edit first so that refresh cannot replace it with the old Host
    // values before the override request is assembled.
    const std::string displayKey = *session->displayKey;
    const bool effectsEnabled = isChecked(displayEffectsEnabled_);
    const bool hdrEnabled = isChecked(displayHdrEnabled_);
    if (!commitPendingPatch())
    {
        return;
    }

    bafx::config::DisplayOverrideConfig overrideConfig{};
    overrideConfig.displayKey = displayKey;
    overrideConfig.enabled = effectsEnabled;
    overrideConfig.hdrEnabled = hdrEnabled;
    overrideConfig.framePacing = *framePacing;
    applyDisplayPolicyCommand(setDisplayOverrideRequest(
        generation_,
        overrideConfig));
}

void ControlCenterWindow::removeSelectedDisplayOverride()
{
    if (!commitPendingPatch())
    {
        return;
    }
    const DisplaySessionState* const session = selectedDisplaySession();
    const bafx::config::DisplayOverrideConfig* const offlineOverride =
        selectedOfflineDisplayOverride();
    const std::string* displayKey = nullptr;
    bool overrideExists = false;
    if (offlineOverride != nullptr)
    {
        displayKey = &offlineOverride->displayKey;
        overrideExists = true;
    }
    else if (session != nullptr && session->displayKey.has_value())
    {
        displayKey = &*session->displayKey;
        overrideExists = bafx::config::findDisplayOverride(
            config_.display,
            *displayKey) != nullptr;
    }

    if (displayKey == nullptr)
    {
        updateDisplayPolicyControls();
        setInfo(
            TextId::RestoreGlobalFailed,
            TextId::StableDisplayKeyMissing);
        return;
    }
    if (!overrideExists)
    {
        updateDisplayPolicyControls();
        updateDisplayDetails();
        return;
    }

    applyDisplayPolicyCommand(removeDisplayOverrideRequest(
        generation_,
        *displayKey));
}

void ControlCenterWindow::applyDisplayPolicyCommand(std::string command)
{
    if (!connected_)
    {
        updateDisplayPolicyControls();
        setInfo(TextId::HostDisconnected, TextId::StartHostAndRefresh);
        return;
    }

    const bafx::windows::IpcClientResponse response = client_.transact(command);
    if (response.succeeded())
    {
        static_cast<void>(refreshFromHost());
        return;
    }
    if (response.errorCode == "generation_conflict")
    {
        static_cast<void>(refreshFromHost());
        setInfo(TextId::ConfigChanged, TextId::ConfigRefreshed);
        return;
    }

    const UiMessage error = describeResponse(response);
    static_cast<void>(refreshFromHost());
    setError(error);
}

bool ControlCenterWindow::applyPatch(
    const std::string_view path,
    const std::string_view valueJson)
{
    if (!commitPendingPatch())
    {
        return false;
    }
    return applyPatchRequest(patchRequest(generation_, path, valueJson));
}

bool ControlCenterWindow::applyPatchRequest(std::string command)
{
    if (!connected_)
    {
        setInfo(TextId::HostDisconnected, TextId::StartHostAndRefresh);
        return false;
    }

    const bafx::windows::IpcClientResponse response = client_.transact(command);
    if (response.succeeded())
    {
        return refreshFromHost();
    }
    if (response.errorCode == "generation_conflict")
    {
        static_cast<void>(refreshFromHost());
        setInfo(TextId::ConfigChanged, TextId::ConfigRefreshed);
        return false;
    }
    const UiMessage error = describeResponse(response);
    // A rejected write left the Host unchanged. Restore every optimistic
    // control value before presenting the failure so the UI remains truthful.
    static_cast<void>(refreshFromHost());
    setError(error);
    return false;
}

void ControlCenterWindow::sendCommand(const std::string_view command)
{
    if (!connected_)
    {
        setInfo(TextId::HostDisconnected, TextId::StartCompatibleHost);
        return;
    }
    if (!commitPendingPatch())
    {
        return;
    }
    const bafx::windows::IpcClientResponse response = client_.transact(command);
    if (!response.succeeded())
    {
        setError(describeResponse(response));
        return;
    }
    static_cast<void>(refreshFromHost());
}

void ControlCenterWindow::clearDiagnosticLogs()
{
    if (!connected_)
    {
        setInfo(TextId::HostDisconnected, TextId::StartHostToClearLogs);
        return;
    }

    const int choice = localizedMessageBox(
        window_,
        TextId::ClearLogsQuestion,
        TextId::ClearLogs,
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (choice != IDYES)
    {
        return;
    }

    const bafx::windows::IpcClientResponse response = client_.transact(
        "ClearLogs");
    if (!response.succeeded())
    {
        setError(describeResponse(response));
        return;
    }

    // The Host owns the JSON response contract. Parse only the three unsigned
    // counters needed for the user-facing summary and reject malformed data.
    const auto readCounter = [&response](const std::string_view name)
        -> std::optional<std::uint64_t>
    {
        const std::string needle = "\"" + std::string(name) + "\":";
        const std::size_t start = response.payload.find(needle);
        if (start == std::string::npos)
        {
            return std::nullopt;
        }
        const std::size_t valueStart = start + needle.size();
        std::size_t valueEnd = valueStart;
        while (valueEnd < response.payload.size()
            && response.payload[valueEnd] >= '0'
            && response.payload[valueEnd] <= '9')
        {
            ++valueEnd;
        }
        if (valueEnd == valueStart
            || (valueEnd < response.payload.size()
                && response.payload[valueEnd] != ','
                && response.payload[valueEnd] != '}'))
        {
            return std::nullopt;
        }
        std::uint64_t value = 0U;
        const auto parsed = std::from_chars(
            response.payload.data() + valueStart,
            response.payload.data() + valueEnd,
            value);
        if (parsed.ec != std::errc{} || parsed.ptr != response.payload.data() + valueEnd)
        {
            return std::nullopt;
        }
        return value;
    };

    const std::optional<std::uint64_t> removedFiles = readCounter(
        "removedFiles");
    const std::optional<std::uint64_t> removedBytes = readCounter(
        "removedBytes");
    const std::optional<std::uint64_t> failedFiles = readCounter(
        "failedFiles");
    if (!removedFiles.has_value()
        || !removedBytes.has_value()
        || !failedFiles.has_value())
    {
        setInfo(
            TextId::ClearLogsUnparsed,
            TextId::ClearLogsUnparsedHint);
        return;
    }

    const UiMessage summary(TextId::ClearLogsResult,
        {std::to_wstring(*removedFiles), std::to_wstring(*removedBytes), std::to_wstring(*failedFiles)});
    setInfo(
        *failedFiles == 0U ? TextId::LogsCleared : TextId::LogsPartlyCleared,
        summary);
}

void ControlCenterWindow::resetDefaults()
{
    if (!connected_)
    {
        setInfo(TextId::HostDisconnected, TextId::StartHostAndRefresh);
        return;
    }

    if (!confirmHotkeyDraft())
    {
        return;
    }

    const int choice = localizedMessageBox(
        window_,
        TextId::ResetQuestion,
        TextId::ResetSettings,
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (choice != IDYES)
    {
        return;
    }

    // A delayed slider patch must not overwrite the defaults after reset.
    KillTimer(window_, patchTimerId);
    pendingPatch_.reset();
    sendCommand(defaultConfigRequest(config_.hotkeys));
}

void ControlCenterWindow::startHostFromBundle()
{
    if (hostShutdownPending_)
    {
        return;
    }
    if (hostStartPending_)
    {
        return;
    }
    hostRunning_ = hostMutexPresent();
    if (hostRunning_)
    {
        updateHostLifecycleButton();
        scheduleHostRefreshRetry();
        setInfo(
            TextId::HostAlreadyRunning,
            TextId::HostAlreadyRunningHint);
        return;
    }
    std::filesystem::path hostPath;
    try
    {
        hostPath = executableDirectory() / L"ba-click-fx-desktop.exe";
    }
    catch (const std::exception& error)
    {
        setError(utf8ToWide(error.what()));
        return;
    }
    if (!std::filesystem::is_regular_file(hostPath))
    {
        setInfo(
            TextId::HostNotFound,
            TextId::HostPathHint);
        return;
    }

    const PackageActivationIdentityResult packageIdentity =
        readPackageActivationState(hostPath.parent_path());
    if (packageIdentity.installStatePresent)
    {
        if (!packageIdentity.succeeded())
        {
            setInfo(
                TextId::InstallationInvalid,
                UiMessage(packageIdentity.error)
                    + TextId::RepairInstallSuffix);
            return;
        }

        const PackageActivationResult activation = activatePackagedHost(
            packageIdentity.identity->appUserModelId);
        if (!activation.succeeded())
        {
            setError(
                UiMessage(TextId::PackageActivationFailedPrefix)
                + hresultText(activation.result));
            return;
        }
    }
    else
    {
        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startupInfo.wShowWindow = SW_SHOWNOACTIVATE;
        PROCESS_INFORMATION processInfo{};
        std::wstring commandLine = L"\"" + hostPath.wstring() + L"\"";
        const std::wstring workingDirectory = hostPath.parent_path().wstring();
        if (CreateProcessW(
                hostPath.c_str(),
                commandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_DEFAULT_ERROR_MODE,
                nullptr,
                workingDirectory.c_str(),
                &startupInfo,
                &processInfo) == FALSE)
        {
            const DWORD error = GetLastError();
            setError(UiMessage(TextId::PortableHostStartFailedPrefix)
                + std::to_wstring(error));
            return;
        }
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
    }

    hostRunning_ = true;
    scheduleHostRefreshRetry(true);
    setInfo(TextId::StartingHost, TextId::StartingHostHint);
}

void ControlCenterWindow::stopHost()
{
    if (hostShutdownPending_)
    {
        return;
    }

    const bool mutexPresent = hostMutexPresent();
    if (!mutexPresent && !hostStartPending_)
    {
        hostRunning_ = false;
        setConnected(false);
        setText(hostVersionText_, TextId::HostVersionStopped);
        setText(statusText_, TextId::HostNotRunning);
        setInfo(TextId::HostStopped, TextId::NoHostToStop);
        return;
    }

    if (connected_ && pendingPatch_.has_value())
    {
        commitPendingPatch();
    }
    if (mutexPresent)
    {
        const HANDLE lifetimeMutex = OpenMutexW(
            SYNCHRONIZE | MUTEX_MODIFY_STATE,
            FALSE,
            bafx::windows::kHostSingleInstanceMutexName);
        hostLifetimeMutex_.reset(lifetimeMutex);
        hostStartPending_ = false;
    }

    // Host owns renderer teardown. The control center requests orderly exit
    // over IPC and observes the exact single-instance mutex; it never searches
    // by executable name or terminates an unrelated process.
    hostShutdownCommandAcknowledged_ = false;
    if (mutexPresent)
    {
        const bafx::windows::IpcClientResponse response =
            client_.transact("Shutdown");
        hostShutdownCommandAcknowledged_ = response.succeeded();
    }

    KillTimer(window_, hostRetryTimerId);
    hostRetryAttempts_ = 0U;
    hostShutdownPending_ = true;
    hostRunning_ = true;
    setConnected(false);
    updateHostLifecycleButton();
    setText(statusText_, TextId::StoppingHostStatus);
    setInfo(TextId::StoppingHost, TextId::StoppingHostHint);
    scheduleHostShutdownPoll();
}

bool ControlCenterWindow::hostMutexPresent() const noexcept
{
    const HANDLE mutex = OpenMutexW(
        SYNCHRONIZE,
        FALSE,
        bafx::windows::kHostSingleInstanceMutexName);
    if (mutex != nullptr)
    {
        CloseHandle(mutex);
        return true;
    }

    // Access denied still proves that a named kernel object exists. This can
    // happen when Host and Control Center run at different integrity levels.
    return GetLastError() == ERROR_ACCESS_DENIED;
}

void ControlCenterWindow::scheduleHostRefreshRetry(const bool startPending) noexcept
{
    // Host recreates its single pipe instance after each short-lived client.
    // A bounded retry removes that startup race without a resident worker.
    hostRetryAttempts_ = hostRetryLimit;
    hostStartPending_ = startPending;
    hostRunning_ = hostRunning_ || hostMutexPresent();
    updateHostLifecycleButton();
    if (startPending)
    {
        setText(statusText_, TextId::StartingHostStatus);
        setText(hostVersionText_, TextId::HostVersionStarting);
    }
    else
    {
        setText(hostVersionText_, TextId::HostVersionConnecting);
    }
    KillTimer(window_, hostRetryTimerId);
    if (SetTimer(
            window_,
            hostRetryTimerId,
            hostRetryDelayMilliseconds,
            nullptr) == 0U)
    {
        hostRetryAttempts_ = 0U;
        hostStartPending_ = false;
        updateHostLifecycleButton();
    }
}

void ControlCenterWindow::scheduleHostShutdownPoll() noexcept
{
    hostShutdownDeadlineTicks_ = GetTickCount64()
        + hostShutdownTimeoutMilliseconds;
    KillTimer(window_, hostShutdownTimerId);
    if (SetTimer(
            window_,
            hostShutdownTimerId,
            hostShutdownPollDelayMilliseconds,
            nullptr) == 0U)
    {
        recoverHostShutdown(
            TextId::HostExitMonitorFailed);
    }
}

void ControlCenterWindow::finishHostShutdown() noexcept
{
    KillTimer(window_, hostShutdownTimerId);
    hostLifetimeMutex_.reset();
    hostShutdownDeadlineTicks_ = 0U;
    hostShutdownPending_ = false;
    hostShutdownCommandAcknowledged_ = false;
    hostRunning_ = false;
    hostStartPending_ = false;
    hostVersionBlocked_ = false;
    setConnected(false);
    setText(hostVersionText_, TextId::HostVersionStopped);
    setText(statusText_, TextId::HostStopped);
    setInfo(TextId::HostStopped, TextId::RestartHostHint);
}

void ControlCenterWindow::recoverHostShutdown(const UiMessage& message)
{
    KillTimer(window_, hostShutdownTimerId);
    hostLifetimeMutex_.reset();
    hostShutdownDeadlineTicks_ = 0U;
    hostShutdownPending_ = false;
    hostShutdownCommandAcknowledged_ = false;
    hostStartPending_ = false;
    hostRunning_ = hostMutexPresent();
    hostVersionBlocked_ = false;
    setConnected(false);
    setText(
        hostVersionText_,
        hostRunning_ ? TextId::HostVersionUnreadable : TextId::HostVersionStopped);
    setText(
        statusText_,
        hostRunning_ ? TextId::HostStillRunning : TextId::HostStopped);
    setError(message);
}

void ControlCenterWindow::updateHostLifecycleButton() const noexcept
{
    if (hostLifecycleButton_ == nullptr)
    {
        return;
    }
    const TextId text = hostShutdownPending_
        ? TextId::Stopping
        : (hostRunning_
            ? TextId::StopHost
            : (hostStartPending_ ? TextId::Starting : TextId::StartHost));
    setText(hostLifecycleButton_, text);
    const BOOL lifecycleEnabled = hostShutdownPending_ ? FALSE : TRUE;
    EnableWindow(hostLifecycleButton_, lifecycleEnabled);
    if (refreshButton_ != nullptr)
    {
        EnableWindow(refreshButton_, lifecycleEnabled);
    }
}

bool ControlCenterWindow::ensureTrayIcon() noexcept
{
    if (trayIconAdded_)
    {
        return true;
    }
    if (window_ == nullptr)
    {
        return false;
    }

    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window_;
    icon.uID = trayIconIdentifier;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = trayNotificationMessage;
    icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    const std::wstring tooltip = formatText(TextId::TrayTooltip, {utf8ToWide(bafx::product::version)});
    const std::size_t tooltipLength = (std::min)(
        tooltip.size(),
        std::size(icon.szTip) - 1U);
    std::copy_n(tooltip.data(), tooltipLength, icon.szTip);
    icon.szTip[tooltipLength] = L'\0';
    trayIconAdded_ = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
    return trayIconAdded_;
}

void ControlCenterWindow::removeTrayIcon() noexcept
{
    if (!trayIconAdded_ || window_ == nullptr)
    {
        return;
    }

    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window_;
    icon.uID = trayIconIdentifier;
    static_cast<void>(Shell_NotifyIconW(NIM_DELETE, &icon));
    trayIconAdded_ = false;
}

void ControlCenterWindow::restoreFromTray() noexcept
{
    if (window_ == nullptr)
    {
        return;
    }
    ShowWindow(window_, SW_RESTORE);
    static_cast<void>(SetForegroundWindow(window_));
}

HMENU ControlCenterWindow::createTrayMenu() const
{
    const HMENU menu = CreatePopupMenu();
    if (menu == nullptr)
    {
        return nullptr;
    }

    static_cast<void>(AppendMenuW(
        menu,
        MF_STRING | MF_DEFAULT,
        trayRestoreCommand,
        tr(TextId::OpenControlCenter)));
    static_cast<void>(AppendMenuW(
        menu,
        MF_STRING | (connected_ ? MF_ENABLED : MF_GRAYED),
        trayPauseCommand,
        connected_ && paused_ ? tr(TextId::ResumeEffects) : tr(TextId::PauseEffects)));
    static_cast<void>(AppendMenuW(menu, MF_SEPARATOR, 0U, nullptr));
    static_cast<void>(AppendMenuW(
        menu,
        MF_STRING,
        trayExitCommand,
        tr(TextId::ExitControlCenter)));
    return menu;
}

void ControlCenterWindow::showTrayMenu()
{
    if (window_ == nullptr)
    {
        return;
    }
    // The window can stay hidden for a long time. Refresh before deriving the
    // action label so another local IPC client cannot leave the tray state stale.
    static_cast<void>(refreshFromHost());
    const HMENU menu = createTrayMenu();
    if (menu == nullptr)
    {
        return;
    }
    POINT cursor{};
    if (GetCursorPos(&cursor) == FALSE)
    {
        DestroyMenu(menu);
        return;
    }

    static_cast<void>(SetForegroundWindow(window_));
    const UINT command = TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        cursor.x,
        cursor.y,
        0,
        window_,
        nullptr);
    DestroyMenu(menu);
    static_cast<void>(PostMessageW(window_, WM_NULL, 0U, 0));
    if (command == trayRestoreCommand)
    {
        restoreFromTray();
    }
    else if (command == trayPauseCommand)
    {
        sendCommand(paused_ ? "Resume" : "Pause");
    }
    else if (command == trayExitCommand)
    {
        closeControlCenter();
    }
}

bool ControlCenterWindow::prepareToClose()
{
    if (!confirmHotkeyDraft())
    {
        return false;
    }
    return commitPendingPatch();
}

void ControlCenterWindow::closeControlCenter()
{
    if (prepareToClose())
    {
        DestroyWindow(window_);
    }
}

void ControlCenterWindow::setConnected(const bool connected) noexcept
{
    connected_ = connected;
    if (connected)
    {
        hostRunning_ = true;
    }
    const BOOL enabled = connected ? TRUE : FALSE;
    const std::array controls{
        effectsEnabled_,
        effectsMode_,
        clickEnabled_,
        trailEnabled_,
        diskLayerEnabled_,
        ringsLayerEnabled_,
        clickShardsLayerEnabled_,
        trailShardsLayerEnabled_,
        trailLayerEnabled_,
        bloomLayerEnabled_,
        trailAlwaysOn_,
        leftClickEnabled_,
        rightClickEnabled_,
        middleClickEnabled_,
        globalScale_.trackbar,
        trailLength_.trackbar,
        trailWidth_.trackbar,
        inputSamplingRate_.trackbar,
        bloomIntensity_.trackbar,
        opacity_.trackbar,
        clickTimeScale_.trackbar,
        trailTimeScale_.trackbar,
        trailLifetimeMs_.trackbar,
        bloomDiffusion_.trackbar,
        bloomThreshold_.trackbar,
        bloomSoftKnee_.trackbar,
        bloomClamp_.trackbar,
        diskRadius_.trackbar,
        diskLifetimeMs_.trackbar,
        ringsHdrIntensity_.trackbar,
        ringsCount_.trackbar,
        ringsLifetimeMs_.trackbar,
        ringsRadiusMin_.trackbar,
        ringsRadiusMax_.trackbar,
        ringsAngularVelocityMultiplier_.trackbar,
        ringsRotationDirection_.trackbar,
        shardsHdrIntensity_.trackbar,
        shardsClickCount_.trackbar,
        shardsClickLifetimeMinMs_.trackbar,
        shardsClickLifetimeMaxMs_.trackbar,
        shardsClickRadius_.trackbar,
        shardsClickSpeedMin_.trackbar,
        shardsClickSpeedMax_.trackbar,
        shardsSizeMin_.trackbar,
        shardsSizeMax_.trackbar,
        trailOpacity_.trackbar,
        themeColorEdit_,
        themeColorChoose_,
        bloomQuality_,
        backgroundMode_,
        cursorExcluded_,
        allowSystemBorder_,
        idleOptimization_,
        fxProfileSelector_,
        fxProfileNameEdit_,
        applyFxProfileButton_,
        saveFxProfileButton_,
        deleteFxProfileButton_,
        startWithWindows_,
        startMinimized_,
        closeToTray_,
#if defined(BAFX_ENABLE_SPOUT2)
        spout2Enabled_,
#endif
        hdrEnabled_,
        activeFxRoiEnabled_,
        framePacing_,
        pauseButton_,
        clearLogsButton_,
        resetDefaultsButton_};
    for (const HWND control : controls)
    {
        if (control != nullptr)
        {
            EnableWindow(control, enabled);
        }
    }
    updateFxProfileActionState();
    if (!connected)
    {
        clearHotkeyCaptureLocally();
        hotkeyStateKnown_ = false;
        displayedHotkeyCleanupError_ = 0U;
        displayedHotkeyActionError_.clear();
#if defined(BAFX_ENABLE_SPOUT2)
        setText(spout2SenderStatus_, TextId::SenderDisconnected);
#endif
        displayState_ = {};
        displayStateError_ = TextId::DisconnectedDisplayHint;
        displayStateRefreshWarning_.clear();
        static_cast<void>(SendMessageW(
            displaySelector_,
            CB_RESETCONTENT,
            0U,
            0));
        updateDisplayDetails();
    }
    else if (activePage_ == Page::Hotkeys)
    {
        SetTimer(window_, hotkeyTimerId, 1'000U, nullptr);
    }
    if (displaySelector_ != nullptr)
    {
        const bool selectorEnabled = connected
            && displayStateError_.empty()
            && !displayState_.sessions.empty();
        EnableWindow(displaySelector_, selectorEnabled ? TRUE : FALSE);
    }
    updateDisplayPolicyControls();
    updateHotkeyControls();
    updateHostLifecycleButton();
    updateDisplayStatePolling();
}

void ControlCenterWindow::setInfo(
    const UiMessage& title,
    const UiMessage& message)
{
    infoTitle_ = title;
    infoMessage_ = message;
    const std::wstring text = title.render() + L"\r\n" + message.render();
    SetWindowTextW(messageText_, text.c_str());
}

void ControlCenterWindow::setError(const UiMessage& message)
{
    setInfo(TextId::OperationFailed, message);
}

void ControlCenterWindow::clearInfo() noexcept
{
    infoTitle_ = UiMessage{};
    infoMessage_ = UiMessage{};
    SetWindowTextW(messageText_, L"");
}

bool ControlCenterWindow::isChecked(const HWND control) const noexcept
{
    return SendMessageW(control, BM_GETCHECK, 0U, 0) == BST_CHECKED;
}

void ControlCenterWindow::setChecked(
    const HWND control,
    const bool checked) const noexcept
{
    static_cast<void>(SendMessageW(
        control,
        BM_SETCHECK,
        checked ? BST_CHECKED : BST_UNCHECKED,
        0));
}

const DisplaySessionState* ControlCenterWindow::selectedDisplaySession()
    const noexcept
{
    if (displaySelector_ == nullptr)
    {
        return nullptr;
    }
    const LRESULT selected = SendMessageW(
        displaySelector_,
        CB_GETCURSEL,
        0U,
        0);
    if (selected == CB_ERR)
    {
        return nullptr;
    }
    const LRESULT itemData = SendMessageW(
        displaySelector_,
        CB_GETITEMDATA,
        static_cast<WPARAM>(selected),
        0);
    if (itemData == CB_ERR
        || static_cast<std::size_t>(itemData) >= displayState_.sessions.size())
    {
        return nullptr;
    }
    return &displayState_.sessions[static_cast<std::size_t>(itemData)];
}

const bafx::config::DisplayOverrideConfig*
ControlCenterWindow::selectedOfflineDisplayOverride() const noexcept
{
    if (displaySelector_ == nullptr)
    {
        return nullptr;
    }
    const LRESULT selected = SendMessageW(
        displaySelector_,
        CB_GETCURSEL,
        0U,
        0);
    if (selected == CB_ERR)
    {
        return nullptr;
    }
    const LRESULT itemData = SendMessageW(
        displaySelector_,
        CB_GETITEMDATA,
        static_cast<WPARAM>(selected),
        0);
    if (itemData == CB_ERR
        || itemData < static_cast<LRESULT>(offlineDisplayItemBase))
    {
        return nullptr;
    }

    const std::size_t index = static_cast<std::size_t>(itemData)
        - offlineDisplayItemBase;
    if (index >= displayState_.offlineOverrides.size())
    {
        return nullptr;
    }
    return &displayState_.offlineOverrides[index];
}

double ControlCenterWindow::sliderValue(const SliderControl& slider) const noexcept
{
    const LRESULT position = SendMessageW(slider.trackbar, TBM_GETPOS, 0U, 0);
    return std::clamp(
        slider.minimum + static_cast<double>(position) * slider.step,
        slider.minimum,
        slider.maximum);
}

void ControlCenterWindow::setSliderValue(
    SliderControl& slider,
    const double value) const noexcept
{
    const double clamped = std::clamp(value, slider.minimum, slider.maximum);
    const int position = static_cast<int>(std::lround(
        (clamped - slider.minimum) / slider.step));
    static_cast<void>(SendMessageW(slider.trackbar, TBM_SETPOS, TRUE, position));
    updateSliderValueText(slider);
}

void ControlCenterWindow::updateSliderValueText(
    const SliderControl& slider) const noexcept
{
    const std::wstring text = numberText(sliderValue(slider));
    setText(slider.valueText, text.c_str());
}

std::wstring ControlCenterWindow::utf8ToWide(const std::string_view value)
{
    if (value.empty())
    {
        return {};
    }
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return tr(TextId::TextTooLong);
    }

    const int count = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (count <= 0)
    {
        return tr(TextId::InvalidHostUtf8);
    }

    std::wstring result(static_cast<std::size_t>(count), L'\0');
    static_cast<void>(MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        count));
    return result;
}

std::string ControlCenterWindow::wideToUtf8(const std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return {};
    }
    const int count = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (count <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            count,
            nullptr,
            nullptr) != count)
    {
        return {};
    }
    return result;
}

UiMessage ControlCenterWindow::describeResponse(
    const bafx::windows::IpcClientResponse& response)
{
    if (response.errorCode.empty() && response.errorMessage.empty())
    {
        return TextId::EmptyResponse;
    }
    constexpr std::array errors{
        std::pair{"timeout", TextId::HostRequestTimeout},
        std::pair{"connect_failed", TextId::HostConnectFailed},
        std::pair{"read_failed", TextId::HostIoFailed},
        std::pair{"write_failed", TextId::HostIoFailed},
        std::pair{"invalid_response", TextId::HostInvalidReply},
        std::pair{"response_too_large", TextId::HostInvalidReply},
        std::pair{"invalid_options", TextId::InvalidHostRequest},
        std::pair{"invalid_request", TextId::InvalidHostRequest},
        std::pair{"empty_request", TextId::InvalidHostRequest},
        std::pair{"request_too_large", TextId::InvalidHostRequest},
        std::pair{"invalid_command", TextId::InvalidHostRequest},
        std::pair{"unknown_command", TextId::InvalidHostRequest},
        std::pair{"missing_payload", TextId::InvalidHostRequest},
        std::pair{"unexpected_payload", TextId::InvalidHostRequest},
        std::pair{"command_limit", TextId::HostServiceFailed},
        std::pair{"internal_error", TextId::HostServiceFailed},
        std::pair{"handler_unavailable", TextId::HostServiceFailed},
        std::pair{"handler_error", TextId::HostServiceFailed},
        std::pair{"invalid_fx_profile", TextId::InvalidProfileAction},
        std::pair{"fx_profile_not_found", TextId::ProfileMissing},
        std::pair{"fx_profile_limit_reached", TextId::ProfileLimitReached},
        std::pair{"fx_profile_duplicate", TextId::ProfileDuplicate},
        std::pair{"fx_profile_store_write_failed", TextId::ConfigWriteFailed},
        std::pair{"invalid_fx_params", TextId::InvalidConfig},
        std::pair{"invalid_display_override", TextId::InvalidConfig},
        std::pair{"invalid_hotkeys", TextId::InvalidHostHotkeys},
        std::pair{"invalid_capture_token", TextId::RecordingExpired},
        std::pair{"hotkey_operation_failed", TextId::HotkeysUnavailable},
        std::pair{"generation_conflict", TextId::ConfigRefreshed},
        std::pair{"config_write_failed", TextId::ConfigWriteFailed},
        std::pair{"invalid_config", TextId::InvalidConfig},
        std::pair{"unsupported_os_build", TextId::UnsupportedOs},
        std::pair{"os_version_unavailable", TextId::RecordingVersionUnknown},
        std::pair{"hotkeys_unavailable", TextId::HotkeysUnavailable},
        std::pair{"hotkey_registration_failed", TextId::RegistrationUnavailable},
        std::pair{"hotkey_activation_unconfirmed", TextId::HotkeysSavedRestartHint},
        std::pair{"hotkey_cleanup_failed", TextId::HotkeyCleanupFailed},
        std::pair{"system_integration_failed", TextId::SystemIntegrationFailed}};
    TextId explanation = TextId::HostRequestFailed;
    for (const auto& [code, text] : errors)
    {
        if (response.errorCode == code)
        {
            explanation = text;
            break;
        }
    }
    std::wstring diagnostic = utf8ToWide(response.errorCode + ": " + response.errorMessage);
    if (response.win32Error != ERROR_SUCCESS)
    {
        diagnostic += L" (Win32: " + std::to_wstring(response.win32Error) + L")";
    }
    return UiMessage(TextId::ServiceError, {explanation, diagnostic});
}

std::string ControlCenterWindow::numberJson(const double value)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(3) << value;
    return stream.str();
}

std::string ControlCenterWindow::patchRequest(
    const std::uint64_t generation,
    const std::string_view path,
    const std::string_view valueJson)
{
    return "SetConfig {\"generation\":" + std::to_string(generation)
        + ",\"path\":\"" + std::string(path)
        + "\",\"value\":" + std::string(valueJson) + "}";
}

std::string ControlCenterWindow::fxPatchRequest(
    const std::uint64_t generation,
    const std::string_view path,
    const std::string_view valueJson)
{
    return "SetFxParam {\"generation\":" + std::to_string(generation)
        + ",\"path\":\"" + std::string(path)
        + "\",\"value\":" + std::string(valueJson) + "}";
}

std::string ControlCenterWindow::fxProfileRequest(
    const std::string_view command,
    const std::uint64_t generation,
    const std::string_view name)
{
    // Profile names consume the remainder of the line, so spaces remain part
    // of the name and need no secondary quoting convention.
    return std::string(command)
        + " "
        + std::to_string(generation)
        + " "
        + std::string(name);
}

std::wstring ControlCenterWindow::numberText(const double value)
{
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(2) << value;
    std::wstring text = stream.str();
    while (!text.empty() && text.back() == L'0')
    {
        text.pop_back();
    }
    if (!text.empty() && text.back() == L'.')
    {
        text.pop_back();
    }
    return text;
}

std::filesystem::path ControlCenterWindow::executableDirectory()
{
    return bafx::windows::executableDirectory();
}

}
