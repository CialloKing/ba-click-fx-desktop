#include "control_center_window.hpp"

#include "config_commands.hpp"
#include "control_center_layout.hpp"
#include "control_center_display.hpp"
#include "display_state_poller.hpp"
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
constexpr UINT_PTR displayStateCompletionTimerId = 7U;
constexpr UINT displayStateCompletionDelayMilliseconds = 50U;
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

// Keep shell invocation shared while callers constrain URLs and local paths.
// Preserve the native result so failed directory opens retain their error code.
[[nodiscard]] INT_PTR navigateShell(
    const HWND owner,
    const wchar_t* const operation,
    const wchar_t* const target)
{
    const HINSTANCE result = ShellExecuteW(
        owner,
        operation,
        target,
        nullptr,
        nullptr,
        SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result);
}

[[nodiscard]] bool openFixedOfficialPage(
    const HWND owner,
    const wchar_t* const url)
{
    return navigateShell(owner, L"open", url) > 32;
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
        invalidateDisplayStateRefresh();
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
        invalidateDisplayStateRefresh();
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

    const bool slidersCreated = createSliders();

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
    openLogDirectoryButton_ = createChild(
        L"BUTTON",
        TextId::OpenLogDirectory,
        BS_PUSHBUTTON | WS_TABSTOP,
        ControlId::OpenLogDirectory);
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
        pauseButton_,
        refreshButton_,
        hostLifecycleButton_,
        resetDefaultsButton_};
    if (!slidersCreated
        || std::ranges::find(required, nullptr) != required.end()
        || std::ranges::any_of(pageControlDescriptors(), [this](const auto& descriptor)
        {
            return this->*descriptor.control == nullptr;
        }))
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
    if (connected_ && !hostShutdownPending_ && activePage_ == Page::DisplayPerformance)
    {
        // ROI diagnostics are observational. A failed timer registration only
        // disables automatic refresh and must never affect Host settings.
        static_cast<void>(SetTimer(
            window_,
            displayStateTimerId,
            displayStatePollDelayMilliseconds,
            nullptr));
        requestDisplayStateRefresh();
    }
    else
    {
        invalidateDisplayStateRefresh();
    }
}

void ControlCenterWindow::selectAdvancedSection(
    const AdvancedSection section) noexcept
{
    activeAdvancedSection_ = section;
    updatePageVisibility();
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
    case ControlId::OpenLogDirectory:
        if (notificationCode == BN_CLICKED)
        {
            openLogDirectory();
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

    for (const auto& descriptor : sliderDescriptors())
    {
        SliderControl& slider = this->*descriptor.control;
        if (slider.trackbar == trackbar)
        {
            updateSliderValueText(slider);
            queueNumberPatch(slider);
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
    if (timerId == displayStateCompletionTimerId)
    {
        pollDisplayStateRefresh();
        return;
    }
    if (timerId == displayStateTimerId)
    {
        if (activePage_ != Page::DisplayPerformance || !connected_)
        {
            updateDisplayStatePolling();
            return;
        }
        pollDisplayStateRefresh();
        requestDisplayStateRefresh();
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
    invalidateDisplayStateRefresh();
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

    updateControls(*confirmedState.state, config.config);
    return true;
}

void ControlCenterWindow::requestDisplayStateRefresh() noexcept
{
    if (!connected_ || hostShutdownPending_ || activePage_ != Page::DisplayPerformance
        || IsWindowVisible(window_) == FALSE || IsIconic(window_) != FALSE)
    {
        return;
    }
    try
    {
        if (displayStatePoller_ == nullptr)
        {
            displayStatePoller_ = std::make_unique<DisplayStatePoller>(controlCenterIpcOptions());
        }
        if (displayStatePoller_->request(generation_))
        {
            // The one-second timer remains a fallback if completion polling
            // cannot be registered. Neither timer waits for an IPC response.
            static_cast<void>(SetTimer(window_, displayStateCompletionTimerId,
                displayStateCompletionDelayMilliseconds, nullptr));
        }
    }
    catch (...)
    {
        logControlCenterEvent("Display.PollStartFailed", {}, bafx::windows::DiagnosticLevel::Warning);
    }
}

void ControlCenterWindow::invalidateDisplayStateRefresh() noexcept
{
    if (window_ != nullptr)
    {
        KillTimer(window_, displayStateCompletionTimerId);
    }
    if (displayStatePoller_ != nullptr)
    {
        displayStatePoller_->invalidate();
    }
}

void ControlCenterWindow::pollDisplayStateRefresh()
{
    if (displayStatePoller_ == nullptr)
    {
        KillTimer(window_, displayStateCompletionTimerId);
        return;
    }
    if (!connected_ || hostShutdownPending_ || activePage_ != Page::DisplayPerformance
        || IsWindowVisible(window_) == FALSE || IsIconic(window_) != FALSE)
    {
        invalidateDisplayStateRefresh();
        return;
    }
    if (auto result = displayStatePoller_->takeResult(); result.has_value())
    {
        if (result->generation == generation_)
        {
            static_cast<void>(acceptDisplayStateResponse(std::move(*result)));
        }
    }
    if (!displayStatePoller_->busy())
    {
        KillTimer(window_, displayStateCompletionTimerId);
    }
}

bool ControlCenterWindow::acceptDisplayStateResponse(DisplayStatePollResult result)
{
    const auto& response = result.response;
    auto& parsed = result.parsed;
    UiMessage failure;
    if (!response.succeeded())
    {
        failure = UiMessage(TextId::DisplayRefreshFailedPrefix) + describeResponse(response);
    }
    else
    {
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
    for (const auto& descriptor : sliderDescriptors())
    {
        setSliderValue(this->*descriptor.control, descriptor.read(config));
    }
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

void ControlCenterWindow::openLogDirectory()
{
    try
    {
        // Control Center runs without Host's package identity. Reuse its
        // install-state path resolution so installed logs open under data.
        const std::filesystem::path directory = startupConfigPath(executableDirectory()).parent_path();
        std::filesystem::create_directories(directory);
        const INT_PTR result = navigateShell(window_, L"explore", directory.c_str());
        if (result <= 32)
        {
            const auto code = std::to_string(result);
            logControlCenterEvent("Log.DirectoryOpenFailed", {{"Error.ShellCode", code}},
                bafx::windows::DiagnosticLevel::Warning);
            setError(TextId::LogDirectoryOpenFailed);
        }
        else
        {
            logControlCenterEvent("Log.DirectoryOpened");
        }
    }
    catch (const std::exception& error)
    {
        setError(UiMessage(TextId::LogDirectoryOpenFailed) + L"\r\n" + utf8ToWide(error.what()));
    }
}

void ControlCenterWindow::clearDiagnosticLogs()
{
    const int choice = localizedMessageBox(
        window_,
        connected_ ? TextId::ClearLogsQuestion : TextId::ClearLocalLogsQuestion,
        TextId::ClearLogs,
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (choice != IDYES)
    {
        return;
    }

    // Each process clears its own writer; disconnected UI maintenance must
    // never remove a Host file that another process could still be writing.
    const auto local = bafx::windows::clearDiagnosticLogs(controlCenterLogPath());
    const auto localFiles = std::to_string(local.removedFiles);
    const auto localBytes = std::to_string(local.removedBytes);
    const auto localFailures = std::to_string(local.failedFiles);
    const auto localError = std::to_string(local.firstError.value());
    logControlCenterEvent("Log.Cleanup", {
        {"Log.Cleanup.RemovedFiles", localFiles}, {"Log.Cleanup.RemovedBytes", localBytes},
        {"Log.Cleanup.FailedFiles", localFailures}, {"Error.Code", localError}},
        local.failedFiles == 0U ? bafx::windows::DiagnosticLevel::Info
                               : bafx::windows::DiagnosticLevel::Warning);
    const UiMessage localSummary(TextId::ClearLogsResult,
        {std::to_wstring(local.removedFiles), std::to_wstring(local.removedBytes),
            std::to_wstring(local.failedFiles)});
    if (!connected_)
    {
        setInfo(local.failedFiles == 0U ? TextId::LocalLogsCleared : TextId::LogsPartlyCleared,
            localSummary + L"\r\n" + UiMessage(TextId::HostLogsNotCleared));
        return;
    }

    const bafx::windows::IpcClientResponse response = client_.transact(
        "ClearLogs");
    if (!response.succeeded())
    {
        setInfo(TextId::LogsPartlyCleared,
            localSummary + L"\r\n" + UiMessage(TextId::HostLogsNotCleared)
                + L"\r\n" + describeResponse(response));
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
            localSummary + L"\r\n" + UiMessage(TextId::ClearLogsUnparsedHint));
        return;
    }

    // The response can be malformed even after IPC succeeds. Avoid wrapping
    // externally supplied counters into a misleading success summary.
    if (*removedFiles > (std::numeric_limits<std::uint64_t>::max)() - local.removedFiles
        || *removedBytes > (std::numeric_limits<std::uint64_t>::max)() - local.removedBytes
        || *failedFiles > (std::numeric_limits<std::uint64_t>::max)() - local.failedFiles)
    {
        setInfo(TextId::ClearLogsUnparsed,
            localSummary + L"\r\n" + UiMessage(TextId::ClearLogsUnparsedHint));
        return;
    }
    const auto totalFailures = *failedFiles + local.failedFiles;
    const UiMessage summary(TextId::ClearLogsResult,
        {std::to_wstring(*removedFiles + local.removedFiles),
            std::to_wstring(*removedBytes + local.removedBytes), std::to_wstring(totalFailures)});
    setInfo(
        totalFailures == 0U ? TextId::LogsCleared : TextId::LogsPartlyCleared,
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
    if (connected_ != connected)
    {
        logControlCenterEvent("Host.ConnectionChanged", {
            {"Host.Connected", connected ? "true" : "false"},
            {"Host.VersionBlocked", hostVersionBlocked_ ? "true" : "false"}});
    }
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
        resetDefaultsButton_};
    for (const HWND control : controls)
    {
        if (control != nullptr)
        {
            EnableWindow(control, enabled);
        }
    }
    for (const auto& descriptor : sliderDescriptors())
    {
        const auto& slider = this->*descriptor.control;
        if (slider.trackbar != nullptr)
        {
            EnableWindow(slider.trackbar, enabled);
        }
    }
    EnableWindow(clearLogsButton_, TRUE);
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
    try
    {
        // Stable English diagnostics remain searchable after a UI language
        // switch. Repeated polling must not duplicate an unchanged message.
        const auto detail = message.render(UiLanguage::English);
        if (title.id != infoTitle_.id || detail != infoMessage_.render(UiLanguage::English))
        {
            const auto text = wideToUtf8(detail);
            logControlCenterEvent("UI.StatusChanged", {
                {"UI.Title", textIdName(title.id)}, {"UI.Message", textIdName(message.id)},
                {"Message", text}}, title.id == TextId::OperationFailed
                    ? bafx::windows::DiagnosticLevel::Error
                    : (title.id == TextId::UpdateCheckFailed
                        ? bafx::windows::DiagnosticLevel::Warning : bafx::windows::DiagnosticLevel::Info));
        }
    }
    catch (...)
    {
    }
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
