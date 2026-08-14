#include "bafx/windows/composition_renderer.hpp"
#include "bafx/windows/error.hpp"
#include "bafx/windows/gpu_texture_readback.hpp"
#include "bafx/windows/overlay_window.hpp"
#include "bafx/windows/wgc_background_sensor.hpp"

#include "capture_artifact_writer.hpp"
#include "spike_runtime.hpp"

#include <windows.h>
#include <winternl.h>

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace
{

using Microsoft::WRL::ComPtr;
using bafx::capture::ComApartment;
using bafx::capture::Deadline;
using bafx::capture::ProcessWatchdog;
using bafx::capture::QpcClock;

constexpr std::uint32_t defaultTimeoutMilliseconds = 15'000U;
constexpr std::uint32_t maximumTimeoutMilliseconds = 120'000U;
constexpr std::uint32_t watchdogGraceMilliseconds = 3'000U;
constexpr std::size_t maximumMessagesPerPump = 256U;
constexpr std::size_t maximumStableSampleAttempts = 4U;
constexpr std::uint32_t fixtureWidth = 640U;
constexpr std::uint32_t fixtureHeight = 320U;
constexpr std::uint32_t overlayExtent = 256U;
constexpr std::uint32_t overlayOffsetX = 32U;
constexpr std::uint32_t overlayOffsetY = 32U;
constexpr float stableSampleTolerance = 0.01F;
constexpr float differenceThreshold = 0.02F;
constexpr float maximumControlDelta = 0.01F;
constexpr float maximumIncludedDelta = 0.01F;
constexpr float maximumExcludedRange = 0.01F;
constexpr float maximumExcludedBackgroundDelta = 0.01F;
constexpr float minimumOverlayDelta = 0.20F;
constexpr float minimumChangedFraction = 0.95F;
constexpr float maximumMarkerRange = 0.01F;
constexpr float minimumMarkerDelta = 0.10F;
constexpr float markerChannelMargin = 0.05F;
constexpr std::array<std::uint8_t, 3U> backgroundSrgb8{30U, 82U, 146U};
constexpr std::array<std::uint8_t, 3U> includedBeforeMarkerSrgb8{
    224U,
    48U,
    64U};
constexpr std::array<std::uint8_t, 3U> excludedMarkerSrgb8{
    48U,
    220U,
    80U};
constexpr std::array<std::uint8_t, 3U> includedAfterMarkerSrgb8{
    240U,
    208U,
    48U};
constexpr bafx::windows::PixelF probeLinear{0.82F, 0.16F, 0.52F, 1.0F};
constexpr bafx::windows::TextureReadbackRegion overlayRoi{
    overlayOffsetX + 32U,
    overlayOffsetY + 32U,
    overlayExtent - 64U,
    overlayExtent - 64U};
constexpr bafx::windows::TextureReadbackRegion controlRoi{
    416U,
    64U,
    192U,
    192U};
constexpr bafx::windows::TextureReadbackRegion markerRoi{
    328U,
    128U,
    64U,
    64U};
constexpr bafx::windows::TextureReadbackRegion markerReferenceRoi{
    controlRoi.left,
    controlRoi.top,
    markerRoi.width,
    markerRoi.height};
std::string latestPhase{"startup"};

struct ProbeOptions
{
    std::filesystem::path outputDirectory{
        L"artifacts\\local\\spikes\\spk-002-self-exclusion\\current"};
    std::string revision{"unrecorded"};
    std::uint32_t timeoutMilliseconds{defaultTimeoutMilliseconds};
    bool help{false};
};

struct ObserverDevice
{
    ComPtr<ID3D11Device> device{};
    ComPtr<ID3D11DeviceContext> context{};
    D3D_FEATURE_LEVEL featureLevel{D3D_FEATURE_LEVEL_11_0};
};

struct OsVersion
{
    std::uint32_t major{0U};
    std::uint32_t minor{0U};
    std::uint32_t build{0U};
    bool available{false};
};

struct FrameMetadata
{
    std::uint64_t previousGeneration{0U};
    std::uint64_t generation{0U};
    std::int64_t markerNanoseconds{0};
    std::int64_t capturedAtNanoseconds{0};
    bafx::windows::WindowSize contentSize{};
};

struct CapturedFrame
{
    FrameMetadata metadata{};
    bafx::windows::Rgba16FloatImage image{};
};

struct StageObservation
{
    bool requestedExcluded{false};
    bafx::windows::CaptureExclusionStatus affinity{};
    DWORD observedExtendedStyle{0U};
    FrameMetadata first{};
    FrameMetadata second{};
    float stableMaximumRgbDelta{0.0F};
    std::array<std::uint8_t, 3U> markerSrgb8{};
    bafx::windows::Rgba16FloatImage image{};
    bafx::capture::LayerArtifact artifact{};
};

struct DifferenceMetrics
{
    bafx::windows::TextureReadbackRegion roi{};
    float threshold{differenceThreshold};
    float maximumRgbDelta{0.0F};
    std::uint64_t differentPixels{0U};
};

struct SpatialDifferenceMetrics
{
    bafx::windows::TextureReadbackRegion leftRoi{};
    bafx::windows::TextureReadbackRegion rightRoi{};
    float threshold{differenceThreshold};
    float maximumRgbDelta{0.0F};
    std::uint64_t differentPixels{0U};
};

struct MarkerMetrics
{
    std::array<float, 3U> meanLinear{};
    float rgbRange{0.0F};
    SpatialDifferenceMetrics vsReference{};
};

struct CaptureMetrics
{
    DifferenceMetrics includedBeforeVsExcluded{};
    DifferenceMetrics includedAfterVsExcluded{};
    DifferenceMetrics includedBeforeVsAfter{};
    DifferenceMetrics markerIncludedBeforeVsExcluded{};
    DifferenceMetrics markerExcludedVsIncludedAfter{};
    DifferenceMetrics markerIncludedBeforeVsAfter{};
    SpatialDifferenceMetrics excludedOverlayVsControl{};
    MarkerMetrics includedBeforeMarker{};
    MarkerMetrics excludedMarker{};
    MarkerMetrics includedAfterMarker{};
    float controlMaximumRgbDelta{0.0F};
    float excludedOverlayRgbRange{0.0F};
};

struct CaptureDocument
{
    ProbeOptions options{};
    std::string capturedAtUtc{};
    OsVersion osVersion{};
    RECT monitorBounds{};
    RECT captureScreenBounds{};
    RECT overlayScreenBounds{};
    bafx::windows::TextureReadbackRegion captureRegion{};
    bafx::windows::GraphicsDeviceInfo rendererDevice{};
    D3D_FEATURE_LEVEL observerFeatureLevel{D3D_FEATURE_LEVEL_11_0};
    bafx::windows::WgcBackgroundSessionCapabilities wgcCapabilities{};
    StageObservation includedBefore{};
    StageObservation excluded{};
    StageObservation includedAfter{};
    CaptureMetrics metrics{};
    bafx::windows::WgcBackgroundResourceLedgerSnapshot resourceLedger{};
};

class SolidBackgroundWindow final
{
public:
    SolidBackgroundWindow(
        const HINSTANCE instance,
        const RECT bounds)
    {
        registerWindowClass(instance);
        window_ = CreateWindowExW(
            WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            windowClassName,
            L"ba-click-fx SPK-002 WDA controlled background",
            WS_POPUP,
            bounds.left,
            bounds.top,
            bounds.right - bounds.left,
            bounds.bottom - bounds.top,
            nullptr,
            nullptr,
            instance,
            this);
        if (window_ == nullptr)
        {
            bafx::windows::throwLastError(
                "CreateWindowExW(WDA self-exclusion background)");
        }
    }

    ~SolidBackgroundWindow()
    {
        if (window_ != nullptr)
        {
            DestroyWindow(window_);
            window_ = nullptr;
        }
    }

    SolidBackgroundWindow(const SolidBackgroundWindow&) = delete;
    SolidBackgroundWindow& operator=(const SolidBackgroundWindow&) = delete;

    [[nodiscard]] HWND handle() const noexcept
    {
        return window_;
    }

    void show()
    {
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        if (!SetWindowPos(
                window_,
                HWND_TOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW))
        {
            bafx::windows::throwLastError(
                "SetWindowPos(WDA self-exclusion background)");
        }
        if (!InvalidateRect(window_, nullptr, FALSE))
        {
            bafx::windows::throwLastError(
                "InvalidateRect(WDA self-exclusion background)");
        }
        if (!UpdateWindow(window_))
        {
            bafx::windows::throwLastError(
                "UpdateWindow(WDA self-exclusion background)");
        }
    }

    void setMarkerColor(const std::array<std::uint8_t, 3U> srgb)
    {
        markerColor_ = RGB(srgb[0U], srgb[1U], srgb[2U]);
        if (!InvalidateRect(window_, nullptr, FALSE))
        {
            bafx::windows::throwLastError(
                "InvalidateRect(WDA stage marker)");
        }
        if (!UpdateWindow(window_))
        {
            bafx::windows::throwLastError(
                "UpdateWindow(WDA stage marker)");
        }
    }

private:
    static constexpr wchar_t windowClassName[] =
        L"BaClickFxWgcSelfExclusionBackground";

    static void registerWindowClass(const HINSTANCE instance)
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &SolidBackgroundWindow::windowProcedure;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = windowClassName;

        const ATOM atom = RegisterClassExW(&windowClass);
        if (atom == 0U && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            bafx::windows::throwLastError(
                "RegisterClassExW(WDA self-exclusion background)");
        }
    }

    static LRESULT CALLBACK windowProcedure(
        const HWND window,
        const UINT message,
        const WPARAM wParam,
        const LPARAM lParam)
    {
        auto* self = reinterpret_cast<SolidBackgroundWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<SolidBackgroundWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(
                window,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(self));
        }
        if (self == nullptr)
        {
            return DefWindowProcW(window, message, wParam, lParam);
        }
        if (message == WM_ERASEBKGND)
        {
            RECT client{};
            GetClientRect(window, &client);
            const HBRUSH brush = CreateSolidBrush(RGB(
                backgroundSrgb8[0U],
                backgroundSrgb8[1U],
                backgroundSrgb8[2U]));
            if (brush != nullptr)
            {
                FillRect(reinterpret_cast<HDC>(wParam), &client, brush);
                DeleteObject(brush);
            }
            return 1;
        }
        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint{};
            const HDC device = BeginPaint(window, &paint);
            const HBRUSH brush = CreateSolidBrush(RGB(
                backgroundSrgb8[0U],
                backgroundSrgb8[1U],
                backgroundSrgb8[2U]));
            if (brush != nullptr)
            {
                FillRect(device, &paint.rcPaint, brush);
                DeleteObject(brush);
            }
            const RECT markerRectangle{
                static_cast<LONG>(markerRoi.left),
                static_cast<LONG>(markerRoi.top),
                static_cast<LONG>(markerRoi.left + markerRoi.width),
                static_cast<LONG>(markerRoi.top + markerRoi.height)};
            const HBRUSH markerBrush = CreateSolidBrush(self->markerColor_);
            if (markerBrush != nullptr)
            {
                FillRect(device, &markerRectangle, markerBrush);
                DeleteObject(markerBrush);
            }
            EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_NCDESTROY)
        {
            self->window_ = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    HWND window_{nullptr};
    COLORREF markerColor_{RGB(
        backgroundSrgb8[0U],
        backgroundSrgb8[1U],
        backgroundSrgb8[2U])};
};

void reportPhase(const std::string_view phase)
{
    latestPhase.assign(phase);
    // The hard watchdog may stop a blocked driver call. Flush the last safe
    // boundary so a timeout still identifies the operation that did not return.
    std::cerr << "SPK-002 WDA phase: " << phase << std::endl;
}

void enablePerMonitorDpiAwareness()
{
    if (SetProcessDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    {
        return;
    }
    if (GetLastError() != ERROR_ACCESS_DENIED)
    {
        bafx::windows::throwLastError(
            "SetProcessDpiAwarenessContext(WDA self-exclusion spike)");
    }
}

[[nodiscard]] std::optional<std::uint32_t> parseUnsigned(
    const std::wstring_view value)
{
    if (value.empty())
    {
        return std::nullopt;
    }
    const std::wstring owned(value);
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(owned.c_str(), &end, 10);
    if (end == owned.c_str()
        || *end != L'\0'
        || parsed > std::numeric_limits<std::uint32_t>::max())
    {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(parsed);
}

[[nodiscard]] bool isRevisionCharacter(const wchar_t character) noexcept
{
    return (character >= L'0' && character <= L'9')
        || (character >= L'A' && character <= L'Z')
        || (character >= L'a' && character <= L'z')
        || character == L'.'
        || character == L'_'
        || character == L'-';
}

[[nodiscard]] ProbeOptions parseOptions(
    const int argumentCount,
    wchar_t** arguments)
{
    ProbeOptions options{};
    for (int index = 1; index < argumentCount; ++index)
    {
        const std::wstring_view argument(arguments[index]);
        if (argument == L"--help")
        {
            options.help = true;
            continue;
        }
        if (argument.starts_with(L"--output="))
        {
            options.outputDirectory = argument.substr(9U);
            if (options.outputDirectory.empty())
            {
                throw std::invalid_argument("--output requires a directory");
            }
            continue;
        }
        if (argument.starts_with(L"--revision="))
        {
            const std::wstring_view revision = argument.substr(11U);
            if (revision.empty() || revision.size() > 128U)
            {
                throw std::invalid_argument("--revision has an invalid length");
            }
            std::string encoded;
            encoded.reserve(revision.size());
            for (const wchar_t character : revision)
            {
                if (!isRevisionCharacter(character))
                {
                    throw std::invalid_argument(
                        "--revision contains an invalid character");
                }
                encoded.push_back(static_cast<char>(character));
            }
            options.revision = std::move(encoded);
            continue;
        }
        if (argument.starts_with(L"--timeout-ms="))
        {
            const std::optional<std::uint32_t> parsed =
                parseUnsigned(argument.substr(13U));
            if (!parsed.has_value()
                || *parsed == 0U
                || *parsed > maximumTimeoutMilliseconds)
            {
                throw std::invalid_argument("--timeout-ms is out of range");
            }
            options.timeoutMilliseconds = *parsed;
            continue;
        }
        throw std::invalid_argument(
            "Unknown WGC self-exclusion spike option");
    }
    return options;
}

[[nodiscard]] RECT primaryMonitorBounds()
{
    const POINT origin{0, 0};
    const HMONITOR monitor = MonitorFromPoint(
        origin,
        MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO information{};
    information.cbSize = sizeof(information);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &information))
    {
        bafx::windows::throwLastError(
            "GetMonitorInfoW(WDA self-exclusion spike)");
    }
    return information.rcMonitor;
}

[[nodiscard]] RECT centeredFixtureBounds(const RECT monitorBounds)
{
    const LONG monitorWidth = monitorBounds.right - monitorBounds.left;
    const LONG monitorHeight = monitorBounds.bottom - monitorBounds.top;
    if (monitorWidth < static_cast<LONG>(fixtureWidth)
        || monitorHeight < static_cast<LONG>(fixtureHeight))
    {
        throw std::runtime_error(
            "Primary monitor is smaller than the WDA fixture");
    }
    const LONG left = monitorBounds.left
        + (monitorWidth - static_cast<LONG>(fixtureWidth)) / 2;
    const LONG top = monitorBounds.top
        + (monitorHeight - static_cast<LONG>(fixtureHeight)) / 2;
    return RECT{
        left,
        top,
        left + static_cast<LONG>(fixtureWidth),
        top + static_cast<LONG>(fixtureHeight)};
}

[[nodiscard]] RECT overlayBounds(const RECT fixtureBounds) noexcept
{
    const LONG left = fixtureBounds.left + static_cast<LONG>(overlayOffsetX);
    const LONG top = fixtureBounds.top + static_cast<LONG>(overlayOffsetY);
    return RECT{
        left,
        top,
        left + static_cast<LONG>(overlayExtent),
        top + static_cast<LONG>(overlayExtent)};
}

[[nodiscard]] bafx::windows::TextureReadbackRegion captureRegion(
    const RECT monitorBounds,
    const RECT fixtureBounds)
{
    if (fixtureBounds.left < monitorBounds.left
        || fixtureBounds.top < monitorBounds.top
        || fixtureBounds.right > monitorBounds.right
        || fixtureBounds.bottom > monitorBounds.bottom)
    {
        throw std::runtime_error("WDA fixture escaped the captured monitor");
    }
    return bafx::windows::TextureReadbackRegion{
        static_cast<std::uint32_t>(fixtureBounds.left - monitorBounds.left),
        static_cast<std::uint32_t>(fixtureBounds.top - monitorBounds.top),
        fixtureWidth,
        fixtureHeight};
}

void requireVisibleWindow(const HWND window, const std::string_view name)
{
    if (window == nullptr || !IsWindowVisible(window))
    {
        throw std::runtime_error(std::string(name) + " window is not visible");
    }
    DWORD cloaked = 0U;
    const HRESULT result = DwmGetWindowAttribute(
        window,
        DWMWA_CLOAKED,
        &cloaked,
        sizeof(cloaked));
    if (SUCCEEDED(result) && cloaked != 0U)
    {
        throw std::runtime_error(std::string(name) + " window is DWM-cloaked");
    }
}

[[nodiscard]] bool windowContainsPoint(
    const HWND window,
    const POINT point) noexcept
{
    if (!IsWindowVisible(window))
    {
        return false;
    }
    DWORD cloaked = 0U;
    if (SUCCEEDED(DwmGetWindowAttribute(
            window,
            DWMWA_CLOAKED,
            &cloaked,
            sizeof(cloaked)))
        && cloaked != 0U)
    {
        return false;
    }
    RECT bounds{};
    return GetWindowRect(window, &bounds) && PtInRect(&bounds, point);
}

[[nodiscard]] HWND firstVisibleWindowBelowAtPoint(
    const HWND overlay,
    const POINT point) noexcept
{
    constexpr std::size_t maximumWindows = 4096U;
    HWND candidate = GetWindow(overlay, GW_HWNDNEXT);
    for (std::size_t count = 0U;
         candidate != nullptr && count < maximumWindows;
         ++count)
    {
        if (windowContainsPoint(candidate, point))
        {
            return candidate;
        }
        candidate = GetWindow(candidate, GW_HWNDNEXT);
    }
    return nullptr;
}

void establishFixtureZOrder(
    const HWND background,
    const HWND overlay,
    const RECT fixtureScreenBounds)
{
    constexpr UINT flags = SWP_NOMOVE
        | SWP_NOSIZE
        | SWP_NOACTIVATE
        | SWP_SHOWWINDOW;
    if (!SetWindowPos(background, HWND_TOP, 0, 0, 0, 0, flags))
    {
        bafx::windows::throwLastError(
            "SetWindowPos(WDA background top)");
    }
    if (!SetWindowPos(overlay, HWND_TOP, 0, 0, 0, 0, flags))
    {
        bafx::windows::throwLastError("SetWindowPos(WDA overlay top)");
    }
    if (!SetWindowPos(background, overlay, 0, 0, 0, 0, flags))
    {
        bafx::windows::throwLastError(
            "SetWindowPos(WDA background below overlay)");
    }

    const LONG roiLeft = fixtureScreenBounds.left
        + static_cast<LONG>(overlayRoi.left);
    const LONG roiTop = fixtureScreenBounds.top
        + static_cast<LONG>(overlayRoi.top);
    const LONG roiRight = roiLeft + static_cast<LONG>(overlayRoi.width) - 1;
    const LONG roiBottom = roiTop + static_cast<LONG>(overlayRoi.height) - 1;
    const std::array roiCorners{
        POINT{roiLeft, roiTop},
        POINT{roiRight, roiTop},
        POINT{roiLeft, roiBottom},
        POINT{roiRight, roiBottom}};
    for (const POINT point : roiCorners)
    {
        if (!windowContainsPoint(overlay, point)
            || firstVisibleWindowBelowAtPoint(overlay, point) != background)
        {
            throw std::runtime_error(
                "Controlled background is not below every overlay ROI corner");
        }
    }
}

[[nodiscard]] DWORD requireOverlayExtendedStyles(const HWND overlay)
{
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR rawStyle = GetWindowLongPtrW(overlay, GWL_EXSTYLE);
    const DWORD error = GetLastError();
    if (rawStyle == 0 && error != ERROR_SUCCESS)
    {
        bafx::windows::throwLastError(
            "GetWindowLongPtrW(WDA overlay styles)");
    }
    const DWORD style = static_cast<DWORD>(rawStyle);
    if ((style & WS_EX_LAYERED) == 0U
        || (style & WS_EX_TRANSPARENT) == 0U)
    {
        throw std::runtime_error(
            "WDA transition did not restore layered click-through styles");
    }
    return style;
}

[[nodiscard]] bool sameLuid(const LUID left, const LUID right) noexcept
{
    return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
}

[[nodiscard]] ObserverDevice createObserverDevice(const LUID adapterLuid)
{
    ComPtr<IDXGIFactory1> factory;
    bafx::windows::throwIfFailed(
        CreateDXGIFactory1(IID_PPV_ARGS(&factory)),
        "CreateDXGIFactory1(WDA observer)");

    ComPtr<IDXGIAdapter1> selectedAdapter;
    for (UINT index = 0U;; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }
        bafx::windows::throwIfFailed(
            result,
            "IDXGIFactory1::EnumAdapters1(WDA observer)");
        DXGI_ADAPTER_DESC1 description{};
        bafx::windows::throwIfFailed(
            adapter->GetDesc1(&description),
            "IDXGIAdapter1::GetDesc1(WDA observer)");
        if (sameLuid(description.AdapterLuid, adapterLuid))
        {
            selectedAdapter = std::move(adapter);
            break;
        }
    }
    if (selectedAdapter == nullptr)
    {
        throw std::runtime_error(
            "Renderer adapter is unavailable for the WDA observer");
    }

    constexpr std::array featureLevels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    ObserverDevice result{};
    bafx::windows::throwIfFailed(
        D3D11CreateDevice(
            selectedAdapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels.data(),
            static_cast<UINT>(featureLevels.size()),
            D3D11_SDK_VERSION,
            &result.device,
            &result.featureLevel,
            &result.context),
        "D3D11CreateDevice(WDA observer)");
    return result;
}

[[nodiscard]] OsVersion queryOsVersion() noexcept
{
    using RtlGetVersion = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE module = GetModuleHandleW(L"ntdll.dll");
    if (module == nullptr)
    {
        return {};
    }
    const auto function = reinterpret_cast<RtlGetVersion>(
        GetProcAddress(module, "RtlGetVersion"));
    if (function == nullptr)
    {
        return {};
    }
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (function(&version) != 0L)
    {
        return {};
    }
    return OsVersion{
        version.dwMajorVersion,
        version.dwMinorVersion,
        version.dwBuildNumber,
        true};
}

void pumpMessages(Deadline& deadline)
{
    MSG message{};
    for (std::size_t count = 0U;
         count < maximumMessagesPerPump && !deadline.expired();
         ++count)
    {
        if (!PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE))
        {
            break;
        }
        if (message.message == WM_QUIT)
        {
            throw std::runtime_error(
                "WDA self-exclusion spike received WM_QUIT");
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void waitForSensorActivity(
    bafx::windows::WgcBackgroundSensor& sensor,
    Deadline& deadline)
{
    HANDLE frameEvent = sensor.frameAvailableObject();
    if (frameEvent == nullptr)
    {
        throw std::runtime_error("WDA WGC frame event is unavailable");
    }
    const DWORD result = MsgWaitForMultipleObjectsEx(
        1U,
        &frameEvent,
        deadline.nextWaitMilliseconds(),
        QS_ALLINPUT,
        MWMO_INPUTAVAILABLE);
    if (result == WAIT_FAILED)
    {
        bafx::windows::throwLastError(
            "MsgWaitForMultipleObjectsEx(WDA self-exclusion spike)");
    }
    if (result == WAIT_OBJECT_0 + 1U)
    {
        pumpMessages(deadline);
    }
}

void waitForClockToPass(
    const QpcClock& clock,
    const std::int64_t timestampNanoseconds,
    Deadline& deadline)
{
    while (!deadline.expired())
    {
        if (clock.now().count() > timestampNanoseconds)
        {
            return;
        }
        // WGC SystemRelativeTime can lead the consumer's current QPC by a few
        // milliseconds. A bounded message-aware wait keeps stable-pair time
        // intervals disjoint without spinning or introducing an unbounded sleep.
        const DWORD result = MsgWaitForMultipleObjectsEx(
            0U,
            nullptr,
            1U,
            QS_ALLINPUT,
            MWMO_INPUTAVAILABLE);
        if (result == WAIT_FAILED)
        {
            bafx::windows::throwLastError(
                "MsgWaitForMultipleObjectsEx(WDA QPC ordering)");
        }
        if (result == WAIT_OBJECT_0)
        {
            pumpMessages(deadline);
        }
    }
    throw std::runtime_error(
        "Timed out ordering WDA stable-pair QPC intervals");
}

void handleFramePoolResize(
    bafx::windows::WgcBackgroundSensor& sensor)
{
    const std::optional<bafx::windows::WindowSize> pending =
        sensor.pendingFramePoolSize();
    if (!pending.has_value())
    {
        throw std::runtime_error("WDA WGC resize has no pending size");
    }
    sensor.recreateFramePool(*pending);
}

[[nodiscard]] std::uint64_t drainCurrentGeneration(
    bafx::windows::WgcBackgroundSensor& sensor,
    ID3D11DeviceContext* context,
    Deadline& deadline)
{
    for (std::size_t attempt = 0U;
         attempt < 2U && !deadline.expired();
         ++attempt)
    {
        const bafx::windows::WgcBackgroundDrainStatus status =
            sensor.drainLatest(context);
        if (status == bafx::windows::WgcBackgroundDrainStatus::Stopped)
        {
            throw std::runtime_error("WGC stopped before WDA presentation");
        }
        if (status
            == bafx::windows::WgcBackgroundDrainStatus::ReconfigureRequired)
        {
            handleFramePoolResize(sensor);
            continue;
        }
        const std::optional<bafx::windows::WgcBackgroundSample> sample =
            sensor.latestSample();
        return sample.has_value() ? sample->generation : 0U;
    }
    throw std::runtime_error(
        "WDA frame-pool resize did not settle before presentation");
}

[[nodiscard]] CapturedFrame waitForFreshFrame(
    bafx::windows::WgcBackgroundSensor& sensor,
    ID3D11DeviceContext* context,
    const std::uint64_t previousGeneration,
    const bafx::core::MonotonicTime marker,
    const bafx::windows::TextureReadbackRegion region,
    Deadline& deadline)
{
    while (!deadline.expired())
    {
        pumpMessages(deadline);
        const bafx::windows::WgcBackgroundDrainStatus status =
            sensor.drainLatest(context);
        if (status == bafx::windows::WgcBackgroundDrainStatus::Stopped)
        {
            throw std::runtime_error("WGC stopped during WDA capture");
        }
        if (status
            == bafx::windows::WgcBackgroundDrainStatus::ReconfigureRequired)
        {
            handleFramePoolResize(sensor);
            continue;
        }
        if (status == bafx::windows::WgcBackgroundDrainStatus::Updated)
        {
            const std::optional<bafx::windows::WgcBackgroundSample> sample =
                sensor.latestSample();
            if (!sample.has_value()
                || !sample->stamp.canonicalLinearScRgb
                || sample->stamp.excludesOwnOverlay
                || sample->stamp.epoch != sensor.expectedEpoch())
            {
                throw std::runtime_error(
                    "WDA WGC sample violates the sensor contract");
            }
            if (sample->generation > previousGeneration
                && sample->stamp.capturedAt > marker)
            {
                if (region.left + region.width > sample->size.width
                    || region.top + region.height > sample->size.height)
                {
                    throw std::runtime_error(
                        "WDA capture region exceeds the WGC monitor frame");
                }
                ComPtr<ID3D11Resource> resource;
                sample->texture->GetResource(&resource);
                ComPtr<ID3D11Texture2D> texture;
                bafx::windows::throwIfFailed(
                    resource.As(&texture),
                    "WDA sample QueryInterface(ID3D11Texture2D)");
                CapturedFrame frame{};
                frame.metadata.previousGeneration = previousGeneration;
                frame.metadata.generation = sample->generation;
                frame.metadata.markerNanoseconds = marker.count();
                frame.metadata.capturedAtNanoseconds =
                    sample->stamp.capturedAt.count();
                frame.metadata.contentSize = sample->size;
                frame.image = bafx::windows::readbackRgba16FloatTexture(
                    context,
                    texture.Get(),
                    region);
                return frame;
            }
        }
        waitForSensorActivity(sensor, deadline);
    }
    throw std::runtime_error("Timed out waiting for a fresh WDA WGC frame");
}

[[nodiscard]] CapturedFrame captureAttempt(
    bafx::windows::CompositionRenderer& renderer,
    bafx::windows::WgcBackgroundSensor& sensor,
    ID3D11DeviceContext* context,
    const QpcClock& clock,
    const bafx::windows::TextureReadbackRegion region,
    Deadline& deadline)
{
    const std::uint64_t previousGeneration = drainCurrentGeneration(
        sensor,
        context,
        deadline);
    const bafx::core::MonotonicTime marker = clock.now();
    static_cast<void>(renderer.presentCompositionProbeColor(probeLinear));
    bafx::windows::throwIfFailed(
        DwmFlush(),
        "DwmFlush(WDA capture presentation)");
    return waitForFreshFrame(
        sensor,
        context,
        previousGeneration,
        marker,
        region,
        deadline);
}

[[nodiscard]] float maximumRgbDelta(
    const bafx::windows::Rgba16FloatPixel left,
    const bafx::windows::Rgba16FloatPixel right) noexcept
{
    return std::max({
        std::abs(
            bafx::windows::halfToFloat(left.red)
            - bafx::windows::halfToFloat(right.red)),
        std::abs(
            bafx::windows::halfToFloat(left.green)
            - bafx::windows::halfToFloat(right.green)),
        std::abs(
            bafx::windows::halfToFloat(left.blue)
            - bafx::windows::halfToFloat(right.blue))});
}

void requireMatchingImages(
    const bafx::windows::Rgba16FloatImage& left,
    const bafx::windows::Rgba16FloatImage& right)
{
    if (left.width != right.width
        || left.height != right.height
        || left.pixels.size() != right.pixels.size()
        || left.width != fixtureWidth
        || left.height != fixtureHeight)
    {
        throw std::runtime_error(
            "WDA capture images have unexpected dimensions");
    }
}

[[nodiscard]] float imageMaximumRgbDelta(
    const bafx::windows::Rgba16FloatImage& left,
    const bafx::windows::Rgba16FloatImage& right)
{
    requireMatchingImages(left, right);
    float maximum = 0.0F;
    for (std::size_t index = 0U; index < left.pixels.size(); ++index)
    {
        maximum = std::max(
            maximum,
            maximumRgbDelta(left.pixels[index], right.pixels[index]));
    }
    return maximum;
}

[[nodiscard]] DifferenceMetrics compareRegion(
    const bafx::windows::Rgba16FloatImage& left,
    const bafx::windows::Rgba16FloatImage& right,
    const bafx::windows::TextureReadbackRegion region)
{
    requireMatchingImages(left, right);
    if (region.left + region.width > left.width
        || region.top + region.height > left.height)
    {
        throw std::runtime_error("WDA comparison region is invalid");
    }

    DifferenceMetrics metrics{};
    metrics.roi = region;
    for (std::uint32_t y = 0U; y < region.height; ++y)
    {
        for (std::uint32_t x = 0U; x < region.width; ++x)
        {
            const std::size_t index =
                static_cast<std::size_t>(region.top + y) * left.width
                + region.left + x;
            const float delta = maximumRgbDelta(
                left.pixels[index],
                right.pixels[index]);
            metrics.maximumRgbDelta = std::max(
                metrics.maximumRgbDelta,
                delta);
            if (delta > metrics.threshold)
            {
                ++metrics.differentPixels;
            }
        }
    }
    return metrics;
}

[[nodiscard]] SpatialDifferenceMetrics compareSpatialRegions(
    const bafx::windows::Rgba16FloatImage& image,
    const bafx::windows::TextureReadbackRegion leftRegion,
    const bafx::windows::TextureReadbackRegion rightRegion)
{
    if (leftRegion.width != rightRegion.width
        || leftRegion.height != rightRegion.height
        || leftRegion.left + leftRegion.width > image.width
        || leftRegion.top + leftRegion.height > image.height
        || rightRegion.left + rightRegion.width > image.width
        || rightRegion.top + rightRegion.height > image.height)
    {
        throw std::runtime_error("WDA spatial comparison regions are invalid");
    }

    SpatialDifferenceMetrics metrics{};
    metrics.leftRoi = leftRegion;
    metrics.rightRoi = rightRegion;
    for (std::uint32_t y = 0U; y < leftRegion.height; ++y)
    {
        for (std::uint32_t x = 0U; x < leftRegion.width; ++x)
        {
            const std::size_t leftIndex =
                static_cast<std::size_t>(leftRegion.top + y) * image.width
                + leftRegion.left + x;
            const std::size_t rightIndex =
                static_cast<std::size_t>(rightRegion.top + y) * image.width
                + rightRegion.left + x;
            const float delta = maximumRgbDelta(
                image.pixels[leftIndex],
                image.pixels[rightIndex]);
            metrics.maximumRgbDelta = std::max(
                metrics.maximumRgbDelta,
                delta);
            if (delta > metrics.threshold)
            {
                ++metrics.differentPixels;
            }
        }
    }
    return metrics;
}

[[nodiscard]] std::array<float, 3U> regionMeanLinear(
    const bafx::windows::Rgba16FloatImage& image,
    const bafx::windows::TextureReadbackRegion region)
{
    if (region.left + region.width > image.width
        || region.top + region.height > image.height)
    {
        throw std::runtime_error("WDA mean region is invalid");
    }
    std::array<double, 3U> sum{};
    for (std::uint32_t y = 0U; y < region.height; ++y)
    {
        for (std::uint32_t x = 0U; x < region.width; ++x)
        {
            const std::size_t index =
                static_cast<std::size_t>(region.top + y) * image.width
                + region.left + x;
            const bafx::windows::Rgba16FloatPixel pixel = image.pixels[index];
            sum[0U] += bafx::windows::halfToFloat(pixel.red);
            sum[1U] += bafx::windows::halfToFloat(pixel.green);
            sum[2U] += bafx::windows::halfToFloat(pixel.blue);
        }
    }
    const double count = static_cast<double>(region.width) * region.height;
    return std::array{
        static_cast<float>(sum[0U] / count),
        static_cast<float>(sum[1U] / count),
        static_cast<float>(sum[2U] / count)};
}

[[nodiscard]] float regionRgbRange(
    const bafx::windows::Rgba16FloatImage& image,
    const bafx::windows::TextureReadbackRegion region)
{
    if (region.left + region.width > image.width
        || region.top + region.height > image.height)
    {
        throw std::runtime_error("WDA RGB range region is invalid");
    }
    std::array<float, 3U> minimum{
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity()};
    std::array<float, 3U> maximum{
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()};
    for (std::uint32_t y = 0U; y < region.height; ++y)
    {
        for (std::uint32_t x = 0U; x < region.width; ++x)
        {
            const std::size_t index =
                static_cast<std::size_t>(region.top + y) * image.width
                + region.left + x;
            const bafx::windows::Rgba16FloatPixel pixel = image.pixels[index];
            const std::array values{
                bafx::windows::halfToFloat(pixel.red),
                bafx::windows::halfToFloat(pixel.green),
                bafx::windows::halfToFloat(pixel.blue)};
            for (std::size_t channel = 0U; channel < values.size(); ++channel)
            {
                minimum[channel] = std::min(minimum[channel], values[channel]);
                maximum[channel] = std::max(maximum[channel], values[channel]);
            }
        }
    }
    return std::max({
        maximum[0U] - minimum[0U],
        maximum[1U] - minimum[1U],
        maximum[2U] - minimum[2U]});
}

[[nodiscard]] float controlMaximumRgbDelta(
    const StageObservation& includedBefore,
    const StageObservation& excluded,
    const StageObservation& includedAfter)
{
    return std::max({
        compareRegion(
            includedBefore.image,
            excluded.image,
            controlRoi).maximumRgbDelta,
        compareRegion(
            includedAfter.image,
            excluded.image,
            controlRoi).maximumRgbDelta,
        compareRegion(
            includedBefore.image,
            includedAfter.image,
            controlRoi).maximumRgbDelta});
}

[[nodiscard]] MarkerMetrics calculateMarkerMetrics(
    const StageObservation& stage)
{
    MarkerMetrics metrics{};
    metrics.meanLinear = regionMeanLinear(stage.image, markerRoi);
    metrics.rgbRange = regionRgbRange(stage.image, markerRoi);
    metrics.vsReference = compareSpatialRegions(
        stage.image,
        markerRoi,
        markerReferenceRoi);
    return metrics;
}

[[nodiscard]] StageObservation captureStage(
    const bool excluded,
    bafx::windows::OverlayWindow& overlay,
    SolidBackgroundWindow& background,
    const RECT fixtureScreenBounds,
    const std::array<std::uint8_t, 3U> markerSrgb,
    bafx::windows::CompositionRenderer& renderer,
    bafx::windows::WgcBackgroundSensor& sensor,
    ID3D11DeviceContext* context,
    const QpcClock& clock,
    const bafx::windows::TextureReadbackRegion region,
    Deadline& deadline)
{
    background.setMarkerColor(markerSrgb);
    establishFixtureZOrder(
        background.handle(),
        overlay.handle(),
        fixtureScreenBounds);
    const bafx::windows::CaptureExclusionStatus affinity =
        overlay.setCaptureExcluded(excluded);
    const DWORD expected = excluded ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
    if (!affinity.confirmed()
        || affinity.requestedAffinity != expected
        || affinity.observedAffinity != expected)
    {
        throw std::runtime_error(
            "Overlay WDA write/readback did not confirm the requested state");
    }
    const DWORD observedExtendedStyle =
        requireOverlayExtendedStyles(overlay.handle());

    overlay.show();
    establishFixtureZOrder(
        background.handle(),
        overlay.handle(),
        fixtureScreenBounds);
    if (requireOverlayExtendedStyles(overlay.handle())
        != observedExtendedStyle)
    {
        throw std::runtime_error(
            "WDA overlay extended styles changed after z-order restoration");
    }
    requireVisibleWindow(background.handle(), "WDA background");
    requireVisibleWindow(overlay.handle(), "WDA overlay");

    // Prime the production swap chain after the affinity transition. Every
    // recorded attempt uses this same opaque color; only WDA may change pixels.
    static_cast<void>(renderer.presentCompositionProbeColor(probeLinear));
    bafx::windows::throwIfFailed(
        DwmFlush(),
        "DwmFlush(WDA stage warmup)");

    std::optional<CapturedFrame> previous{};
    for (std::size_t attempt = 0U;
         attempt < maximumStableSampleAttempts && !deadline.expired();
         ++attempt)
    {
        if (previous.has_value())
        {
            waitForClockToPass(
                clock,
                previous->metadata.capturedAtNanoseconds,
                deadline);
        }
        CapturedFrame current = captureAttempt(
            renderer,
            sensor,
            context,
            clock,
            region,
            deadline);
        if (previous.has_value())
        {
            const float stability = imageMaximumRgbDelta(
                previous->image,
                current.image);
            if (stability <= stableSampleTolerance)
            {
                StageObservation observation{};
                observation.requestedExcluded = excluded;
                observation.affinity = affinity;
                observation.observedExtendedStyle = observedExtendedStyle;
                observation.first = previous->metadata;
                observation.second = current.metadata;
                observation.stableMaximumRgbDelta = stability;
                observation.markerSrgb8 = markerSrgb;
                observation.image = std::move(current.image);
                return observation;
            }
        }
        previous = std::move(current);
    }
    throw std::runtime_error(
        "WDA stage did not produce two stable fresh samples");
}

[[nodiscard]] CaptureMetrics calculateMetrics(
    const StageObservation& includedBefore,
    const StageObservation& excluded,
    const StageObservation& includedAfter)
{
    CaptureMetrics metrics{};
    metrics.includedBeforeVsExcluded = compareRegion(
        includedBefore.image,
        excluded.image,
        overlayRoi);
    metrics.includedAfterVsExcluded = compareRegion(
        includedAfter.image,
        excluded.image,
        overlayRoi);
    metrics.includedBeforeVsAfter = compareRegion(
        includedBefore.image,
        includedAfter.image,
        overlayRoi);
    metrics.markerIncludedBeforeVsExcluded = compareRegion(
        includedBefore.image,
        excluded.image,
        markerRoi);
    metrics.markerExcludedVsIncludedAfter = compareRegion(
        excluded.image,
        includedAfter.image,
        markerRoi);
    metrics.markerIncludedBeforeVsAfter = compareRegion(
        includedBefore.image,
        includedAfter.image,
        markerRoi);
    metrics.excludedOverlayVsControl = compareSpatialRegions(
        excluded.image,
        overlayRoi,
        controlRoi);
    metrics.includedBeforeMarker = calculateMarkerMetrics(includedBefore);
    metrics.excludedMarker = calculateMarkerMetrics(excluded);
    metrics.includedAfterMarker = calculateMarkerMetrics(includedAfter);
    metrics.controlMaximumRgbDelta = controlMaximumRgbDelta(
        includedBefore,
        excluded,
        includedAfter);
    metrics.excludedOverlayRgbRange = regionRgbRange(
        excluded.image,
        overlayRoi);
    return metrics;
}

void requireFrameMetadata(const FrameMetadata& frame)
{
    if (frame.generation <= frame.previousGeneration
        || frame.capturedAtNanoseconds <= frame.markerNanoseconds)
    {
        throw std::runtime_error(
            "WDA sample is not newer than its generation and QPC markers");
    }
}

void requireStageContract(
    const StageObservation& stage,
    const bool excluded)
{
    const DWORD expected = excluded ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
    if (stage.requestedExcluded != excluded
        || !stage.affinity.confirmed()
        || stage.affinity.requestedAffinity != expected
        || stage.affinity.observedAffinity != expected
        || (stage.observedExtendedStyle & WS_EX_LAYERED) == 0U
        || (stage.observedExtendedStyle & WS_EX_TRANSPARENT) == 0U)
    {
        throw std::runtime_error("WDA stage affinity contract is inconsistent");
    }
    requireFrameMetadata(stage.first);
    requireFrameMetadata(stage.second);
    if (stage.second.generation <= stage.first.generation
        || stage.second.previousGeneration < stage.first.generation
        || stage.stableMaximumRgbDelta > stableSampleTolerance)
    {
        throw std::runtime_error("WDA stable-pair contract is inconsistent");
    }
}

void requireMarkerEvidence(
    const StageObservation& stage,
    const MarkerMetrics& metrics,
    const std::array<std::uint8_t, 3U> expectedSrgb)
{
    const std::uint64_t markerPixels =
        static_cast<std::uint64_t>(markerRoi.width) * markerRoi.height;
    const std::uint64_t minimumDifferentPixels =
        static_cast<std::uint64_t>(
            static_cast<double>(markerPixels) * minimumChangedFraction);
    if (stage.markerSrgb8 != expectedSrgb
        || metrics.rgbRange > maximumMarkerRange
        || metrics.vsReference.maximumRgbDelta < minimumMarkerDelta
        || metrics.vsReference.differentPixels < minimumDifferentPixels)
    {
        throw std::runtime_error(
            "WDA stage marker is absent or not uniform in the raw frame");
    }

    const float red = metrics.meanLinear[0U];
    const float green = metrics.meanLinear[1U];
    const float blue = metrics.meanLinear[2U];
    bool channelIdentityMatches = false;
    if (expectedSrgb == includedBeforeMarkerSrgb8)
    {
        channelIdentityMatches = red > green + markerChannelMargin
            && red > blue + markerChannelMargin;
    }
    else if (expectedSrgb == excludedMarkerSrgb8)
    {
        channelIdentityMatches = green > red + markerChannelMargin
            && green > blue + markerChannelMargin;
    }
    else if (expectedSrgb == includedAfterMarkerSrgb8)
    {
        channelIdentityMatches = red > blue + markerChannelMargin
            && green > blue + markerChannelMargin;
    }
    if (!channelIdentityMatches)
    {
        throw std::runtime_error(
            "WDA raw stage marker does not match its channel identity");
    }
}

void requireCaptureEvidence(const CaptureDocument& document)
{
    requireStageContract(document.includedBefore, false);
    requireStageContract(document.excluded, true);
    requireStageContract(document.includedAfter, false);
    if (document.excluded.first.generation
            <= document.includedBefore.second.generation
        || document.includedAfter.first.generation
            <= document.excluded.second.generation)
    {
        throw std::runtime_error(
            "WDA stage generations did not advance in capture order");
    }
    if (!document.wgcCapabilities.cursorControlConfirmed
        || !document.wgcCapabilities.cursorExcluded
        || document.wgcCapabilities.cursorCaptureEnabled)
    {
        throw std::runtime_error(
            "WDA observer cursor exclusion was not confirmed");
    }

    const std::uint64_t overlayPixels =
        static_cast<std::uint64_t>(overlayRoi.width) * overlayRoi.height;
    const std::uint64_t minimumDifferentPixels =
        static_cast<std::uint64_t>(
            static_cast<double>(overlayPixels) * minimumChangedFraction);
    const auto requireMaterialDifference = [minimumDifferentPixels](
        const DifferenceMetrics& metrics)
    {
        if (metrics.maximumRgbDelta < minimumOverlayDelta
            || metrics.differentPixels < minimumDifferentPixels)
        {
            throw std::runtime_error(
                "WDA exclusion did not reveal the controlled background");
        }
    };
    requireMaterialDifference(document.metrics.includedBeforeVsExcluded);
    requireMaterialDifference(document.metrics.includedAfterVsExcluded);

    if (document.metrics.excludedOverlayRgbRange > maximumExcludedRange)
    {
        throw std::runtime_error(
            "WDA-excluded overlay ROI is not a uniform background");
    }
    if (document.metrics.excludedOverlayVsControl.maximumRgbDelta
            > maximumExcludedBackgroundDelta
        || document.metrics.excludedOverlayVsControl.differentPixels != 0U)
    {
        throw std::runtime_error(
            "WDA-excluded overlay ROI does not reveal the remote background");
    }
    if (document.metrics.includedBeforeVsAfter.maximumRgbDelta
            > maximumIncludedDelta
        || document.metrics.includedBeforeVsAfter.differentPixels != 0U)
    {
        throw std::runtime_error(
            "Repeated WDA-included overlay captures are not stable");
    }
    if (document.metrics.controlMaximumRgbDelta > maximumControlDelta)
    {
        throw std::runtime_error(
            "WDA control ROI changed between affinity modes");
    }

    requireMarkerEvidence(
        document.includedBefore,
        document.metrics.includedBeforeMarker,
        includedBeforeMarkerSrgb8);
    requireMarkerEvidence(
        document.excluded,
        document.metrics.excludedMarker,
        excludedMarkerSrgb8);
    requireMarkerEvidence(
        document.includedAfter,
        document.metrics.includedAfterMarker,
        includedAfterMarkerSrgb8);

    const std::uint64_t markerPixels =
        static_cast<std::uint64_t>(markerRoi.width) * markerRoi.height;
    const std::uint64_t minimumMarkerDifferentPixels =
        static_cast<std::uint64_t>(
            static_cast<double>(markerPixels) * minimumChangedFraction);
    const auto requireDistinctMarkers = [minimumMarkerDifferentPixels](
        const DifferenceMetrics& metrics)
    {
        if (metrics.maximumRgbDelta < minimumMarkerDelta
            || metrics.differentPixels < minimumMarkerDifferentPixels)
        {
            throw std::runtime_error(
                "WDA raw stage markers are not pairwise distinct");
        }
    };
    requireDistinctMarkers(
        document.metrics.markerIncludedBeforeVsExcluded);
    requireDistinctMarkers(
        document.metrics.markerExcludedVsIncludedAfter);
    requireDistinctMarkers(
        document.metrics.markerIncludedBeforeVsAfter);
}

void requireBalancedLedger(
    const bafx::windows::WgcBackgroundResourceLedgerSnapshot& ledger)
{
    const bool balanced = ledger.allReleased()
        && ledger.failures == 0U
        && ledger.framesAcquired == ledger.framesClosed
        && ledger.framePoolsCreated == ledger.framePoolsClosed
        && ledger.sessionsCreated == ledger.sessionsClosed
        && ledger.frameArrivedRegistrations
            == ledger.frameArrivedUnregistrations
        && ledger.itemClosedRegistrations
            == ledger.itemClosedUnregistrations;
    if (!balanced)
    {
        throw std::runtime_error("WDA WGC resource ledger is not balanced");
    }
}

[[nodiscard]] std::string utcTimestamp()
{
    SYSTEMTIME time{};
    GetSystemTime(&time);
    std::ostringstream stream;
    stream << std::setfill('0')
           << std::setw(4) << time.wYear << '-'
           << std::setw(2) << time.wMonth << '-'
           << std::setw(2) << time.wDay << 'T'
           << std::setw(2) << time.wHour << ':'
           << std::setw(2) << time.wMinute << ':'
           << std::setw(2) << time.wSecond << '.'
           << std::setw(3) << time.wMilliseconds << 'Z';
    return stream.str();
}

[[nodiscard]] std::string wideToUtf8(const std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0)
    {
        throw std::runtime_error("Unable to encode adapter name as UTF-8");
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        required,
        nullptr,
        nullptr);
    if (written != required)
    {
        throw std::runtime_error("Unable to encode adapter name as UTF-8");
    }
    return result;
}

void writeJsonString(std::ostream& stream, const std::string_view value)
{
    stream << '"';
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"':
            stream << "\\\"";
            break;
        case '\\':
            stream << "\\\\";
            break;
        case '\n':
            stream << "\\n";
            break;
        case '\r':
            stream << "\\r";
            break;
        case '\t':
            stream << "\\t";
            break;
        default:
            if (character < 0x20U)
            {
                stream << "\\u00"
                       << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<unsigned int>(character)
                       << std::dec << std::setfill(' ');
            }
            else
            {
                stream << static_cast<char>(character);
            }
            break;
        }
    }
    stream << '"';
}

void writeRect(std::ostream& stream, const RECT bounds)
{
    stream << "{\"left\": " << bounds.left
           << ", \"top\": " << bounds.top
           << ", \"right\": " << bounds.right
           << ", \"bottom\": " << bounds.bottom << '}';
}

void writeRegion(
    std::ostream& stream,
    const bafx::windows::TextureReadbackRegion region)
{
    stream << "{\"left\": " << region.left
           << ", \"top\": " << region.top
           << ", \"width\": " << region.width
           << ", \"height\": " << region.height << '}';
}

void writeSrgb8(
    std::ostream& stream,
    const std::array<std::uint8_t, 3U> value)
{
    stream << '['
           << static_cast<unsigned int>(value[0U]) << ", "
           << static_cast<unsigned int>(value[1U]) << ", "
           << static_cast<unsigned int>(value[2U]) << ']';
}

void writeFrameMetadata(
    std::ostream& stream,
    const FrameMetadata& frame)
{
    stream << "{\"previousGeneration\": " << frame.previousGeneration
           << ", \"generation\": " << frame.generation
           << ", \"markerNs\": " << frame.markerNanoseconds
           << ", \"capturedAtNs\": " << frame.capturedAtNanoseconds
           << ", \"contentSize\": {\"width\": "
           << frame.contentSize.width
           << ", \"height\": " << frame.contentSize.height << "}}";
}

void writeAffinity(
    std::ostream& stream,
    const bafx::windows::CaptureExclusionStatus& affinity)
{
    stream << "{\"requested\": " << affinity.requestedAffinity
           << ", \"observed\": " << affinity.observedAffinity
           << ", \"setSucceeded\": "
           << (affinity.setSucceeded ? "true" : "false")
           << ", \"querySucceeded\": "
           << (affinity.querySucceeded ? "true" : "false")
           << ", \"setError\": " << affinity.setError
           << ", \"queryError\": " << affinity.queryError
           << ", \"confirmed\": "
           << (affinity.confirmed() ? "true" : "false") << '}';
}

void writeLedger(
    std::ostream& stream,
    const bafx::windows::WgcBackgroundResourceLedgerSnapshot& ledger)
{
    stream << "{\"framesAcquired\": " << ledger.framesAcquired
           << ", \"framesClosed\": " << ledger.framesClosed
           << ", \"framePoolsCreated\": " << ledger.framePoolsCreated
           << ", \"framePoolsClosed\": " << ledger.framePoolsClosed
           << ", \"framePoolsRecreated\": " << ledger.framePoolsRecreated
           << ", \"sessionsCreated\": " << ledger.sessionsCreated
           << ", \"sessionsClosed\": " << ledger.sessionsClosed
           << ", \"frameArrivedRegistrations\": "
           << ledger.frameArrivedRegistrations
           << ", \"frameArrivedUnregistrations\": "
           << ledger.frameArrivedUnregistrations
           << ", \"itemClosedRegistrations\": "
           << ledger.itemClosedRegistrations
           << ", \"itemClosedUnregistrations\": "
           << ledger.itemClosedUnregistrations
           << ", \"liveFrames\": " << ledger.liveFrames
           << ", \"liveFramePools\": " << ledger.liveFramePools
           << ", \"liveSessions\": " << ledger.liveSessions
           << ", \"liveFrameArrivedRegistrations\": "
           << ledger.liveFrameArrivedRegistrations
           << ", \"liveItemClosedRegistrations\": "
           << ledger.liveItemClosedRegistrations
           << ", \"failures\": " << ledger.failures
           << ", \"allReleased\": "
           << (ledger.allReleased() ? "true" : "false") << '}';
}

void writeStage(
    std::ostream& stream,
    const StageObservation& stage,
    const std::string_view rawFile,
    const std::string_view pngFile)
{
    stream << "{\"requestedExcluded\": "
           << (stage.requestedExcluded ? "true" : "false")
           << ", \"affinity\": ";
    writeAffinity(stream, stage.affinity);
    stream << ", \"observedExtendedStyle\": "
           << stage.observedExtendedStyle
           << ", \"layeredStyleRestored\": "
           << ((stage.observedExtendedStyle & WS_EX_LAYERED) != 0U
                ? "true"
                : "false")
           << ", \"transparentStyleRestored\": "
           << ((stage.observedExtendedStyle & WS_EX_TRANSPARENT) != 0U
                ? "true"
                : "false")
           << ", \"markerSrgb8\": ";
    writeSrgb8(stream, stage.markerSrgb8);
    stream << ", \"stablePair\": {\"first\": ";
    writeFrameMetadata(stream, stage.first);
    stream << ", \"second\": ";
    writeFrameMetadata(stream, stage.second);
    stream << ", \"maximumRgbDelta\": "
           << stage.stableMaximumRgbDelta
           << "}, \"artifact\": {\"raw\": ";
    writeJsonString(stream, rawFile);
    stream << ", \"png\": ";
    writeJsonString(stream, pngFile);
    stream << ", \"width\": " << stage.artifact.width
           << ", \"height\": " << stage.artifact.height
           << ", \"rawBytes\": " << stage.artifact.rawBytes << "}}";
}

void writeDifferenceMetrics(
    std::ostream& stream,
    const DifferenceMetrics& metrics)
{
    stream << "{\"roi\": ";
    writeRegion(stream, metrics.roi);
    stream << ", \"threshold\": " << metrics.threshold
           << ", \"maximumRgbDelta\": " << metrics.maximumRgbDelta
           << ", \"differentPixels\": " << metrics.differentPixels << '}';
}

void writeSpatialDifferenceMetrics(
    std::ostream& stream,
    const SpatialDifferenceMetrics& metrics)
{
    stream << "{\"leftRoi\": ";
    writeRegion(stream, metrics.leftRoi);
    stream << ", \"rightRoi\": ";
    writeRegion(stream, metrics.rightRoi);
    stream << ", \"threshold\": " << metrics.threshold
           << ", \"maximumRgbDelta\": " << metrics.maximumRgbDelta
           << ", \"differentPixels\": " << metrics.differentPixels << '}';
}

void writeMarkerMetrics(
    std::ostream& stream,
    const MarkerMetrics& metrics)
{
    stream << "{\"meanLinear\": {\"r\": " << metrics.meanLinear[0U]
           << ", \"g\": " << metrics.meanLinear[1U]
           << ", \"b\": " << metrics.meanLinear[2U]
           << "}, \"rgbRange\": " << metrics.rgbRange
           << ", \"vsReference\": ";
    writeSpatialDifferenceMetrics(stream, metrics.vsReference);
    stream << '}';
}

void replaceFile(
    const std::filesystem::path& temporaryPath,
    const std::filesystem::path& finalPath,
    const std::string_view operation)
{
    if (!MoveFileExW(
            temporaryPath.c_str(),
            finalPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        bafx::windows::throwLastError(operation);
    }
}

void writeCaptureDocument(
    const std::filesystem::path& outputDirectory,
    const CaptureDocument& document)
{
    std::filesystem::create_directories(outputDirectory);
    const std::filesystem::path finalPath =
        outputDirectory / L"self-exclusion.json";
    const std::filesystem::path temporaryPath =
        outputDirectory / L"self-exclusion.json.tmp";
    std::ofstream stream(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        throw std::runtime_error("Unable to create WDA capture document");
    }
    stream << std::setprecision(9);
    stream << "{\n"
           << "  \"schemaVersion\": 1,\n"
           << "  \"spikeId\": \"SPK-002-WDA-SELF-EXCLUSION\",\n"
           << "  \"applicationVersion\": \"" << BAFX_CAPTURE_VERSION
           << "\",\n"
           << "  \"revision\": ";
    writeJsonString(stream, document.options.revision);
    stream << ",\n  \"capturedAtUtc\": ";
    writeJsonString(stream, document.capturedAtUtc);
    stream << ",\n  \"timeoutMs\": "
           << document.options.timeoutMilliseconds << ",\n"
           << "  \"contract\": {\"scope\": "
           << "\"controlled-monitor-WDA-self-exclusion-pixels-only\", "
           << "\"captureTarget\": \"MONITOR\", "
           << "\"surfaceFormat\": \"DXGI_FORMAT_R16G16B16A16_FLOAT\", "
           << "\"sessionTopology\": "
           << "\"single-session-dynamic-affinity-matrix\", "
           << "\"validatesProductStopWdaStartTransaction\": false, "
           << "\"systemBorderAllowed\": true, "
           << "\"cursorCaptureEnabled\": false, "
           << "\"frameMarkerSemantic\": \"stage-unique-solid-srgb8\", "
           << "\"maximumStableSampleAttempts\": "
           << maximumStableSampleAttempts
           << ", \"stableSampleTolerance\": " << stableSampleTolerance
           << ", \"differenceThreshold\": " << differenceThreshold
           << ", \"maximumControlDelta\": " << maximumControlDelta
           << ", \"maximumIncludedDelta\": " << maximumIncludedDelta
           << ", \"maximumExcludedRange\": " << maximumExcludedRange
           << ", \"maximumExcludedBackgroundDelta\": "
           << maximumExcludedBackgroundDelta
           << ", \"minimumOverlayDelta\": " << minimumOverlayDelta
           << ", \"maximumMarkerRange\": " << maximumMarkerRange
           << ", \"minimumMarkerDelta\": " << minimumMarkerDelta
           << ", \"markerChannelMargin\": " << markerChannelMargin
           << ", \"minimumChangedFraction\": "
           << minimumChangedFraction << "},\n"
           << "  \"osVersion\": ";
    if (document.osVersion.available)
    {
        stream << "{\"major\": " << document.osVersion.major
               << ", \"minor\": " << document.osVersion.minor
               << ", \"build\": " << document.osVersion.build << '}';
    }
    else
    {
        stream << "null";
    }
    stream << ",\n  \"rendererDevice\": {\"driverType\": \"hardware\", "
           << "\"adapter\": ";
    writeJsonString(
        stream,
        wideToUtf8(document.rendererDevice.adapterDescription));
    stream << ", \"adapterLuid\": {\"low\": "
           << document.rendererDevice.adapterLuid.LowPart
           << ", \"high\": " << document.rendererDevice.adapterLuid.HighPart
           << "}, \"vendorId\": " << document.rendererDevice.vendorId
           << ", \"deviceId\": " << document.rendererDevice.deviceId
           << ", \"driverVersion\": ";
    if (document.rendererDevice.driverVersion.has_value())
    {
        stream << *document.rendererDevice.driverVersion;
    }
    else
    {
        stream << "null";
    }
    stream << ", \"featureLevel\": "
           << static_cast<unsigned int>(document.rendererDevice.featureLevel)
           << "},\n"
           << "  \"observerFeatureLevel\": "
           << static_cast<unsigned int>(document.observerFeatureLevel)
           << ",\n"
           << "  \"wgcCapabilities\": {\"borderHidden\": "
           << (document.wgcCapabilities.borderHidden ? "true" : "false")
           << ", \"cursorExcluded\": "
           << (document.wgcCapabilities.cursorExcluded ? "true" : "false")
           << ", \"cursorCaptureEnabled\": "
           << (document.wgcCapabilities.cursorCaptureEnabled
                ? "true"
                : "false")
           << ", \"cursorControlConfirmed\": "
           << (document.wgcCapabilities.cursorControlConfirmed
                ? "true"
                : "false") << "},\n"
           << "  \"fixture\": {\"monitorBounds\": ";
    writeRect(stream, document.monitorBounds);
    stream << ", \"captureScreenBounds\": ";
    writeRect(stream, document.captureScreenBounds);
    stream << ", \"overlayScreenBounds\": ";
    writeRect(stream, document.overlayScreenBounds);
    stream << ", \"captureRegion\": ";
    writeRegion(stream, document.captureRegion);
    stream << ", \"overlayRoi\": ";
    writeRegion(stream, overlayRoi);
    stream << ", \"controlRoi\": ";
    writeRegion(stream, controlRoi);
    stream << ", \"markerRoi\": ";
    writeRegion(stream, markerRoi);
    stream << ", \"markerReferenceRoi\": ";
    writeRegion(stream, markerReferenceRoi);
    stream << ", \"backgroundSrgb8\": ";
    writeSrgb8(stream, backgroundSrgb8);
    stream << ", \"markerColorsSrgb8\": {\"includedBefore\": ";
    writeSrgb8(stream, includedBeforeMarkerSrgb8);
    stream << ", \"excluded\": ";
    writeSrgb8(stream, excludedMarkerSrgb8);
    stream << ", \"includedAfter\": ";
    writeSrgb8(stream, includedAfterMarkerSrgb8);
    stream << "}, \"probeLinear\": {\"r\": " << probeLinear.red
           << ", \"g\": " << probeLinear.green
           << ", \"b\": " << probeLinear.blue
           << ", \"a\": " << probeLinear.alpha << "}},\n"
           << "  \"observations\": {\n"
           << "    \"includedBefore\": ";
    writeStage(
        stream,
        document.includedBefore,
        "included-before.rgba16f",
        "included-before.png");
    stream << ",\n    \"excluded\": ";
    writeStage(
        stream,
        document.excluded,
        "excluded.rgba16f",
        "excluded.png");
    stream << ",\n    \"includedAfter\": ";
    writeStage(
        stream,
        document.includedAfter,
        "included-after.rgba16f",
        "included-after.png");
    stream << "\n  },\n"
           << "  \"metrics\": {\n"
           << "    \"includedBeforeVsExcluded\": ";
    writeDifferenceMetrics(
        stream,
        document.metrics.includedBeforeVsExcluded);
    stream << ",\n    \"includedAfterVsExcluded\": ";
    writeDifferenceMetrics(
        stream,
        document.metrics.includedAfterVsExcluded);
    stream << ",\n    \"includedBeforeVsAfter\": ";
    writeDifferenceMetrics(
        stream,
        document.metrics.includedBeforeVsAfter);
    stream << ",\n    \"markerIncludedBeforeVsExcluded\": ";
    writeDifferenceMetrics(
        stream,
        document.metrics.markerIncludedBeforeVsExcluded);
    stream << ",\n    \"markerExcludedVsIncludedAfter\": ";
    writeDifferenceMetrics(
        stream,
        document.metrics.markerExcludedVsIncludedAfter);
    stream << ",\n    \"markerIncludedBeforeVsAfter\": ";
    writeDifferenceMetrics(
        stream,
        document.metrics.markerIncludedBeforeVsAfter);
    stream << ",\n    \"excludedOverlayVsControl\": ";
    writeSpatialDifferenceMetrics(
        stream,
        document.metrics.excludedOverlayVsControl);
    stream << ",\n    \"markers\": {\"includedBefore\": ";
    writeMarkerMetrics(stream, document.metrics.includedBeforeMarker);
    stream << ", \"excluded\": ";
    writeMarkerMetrics(stream, document.metrics.excludedMarker);
    stream << ", \"includedAfter\": ";
    writeMarkerMetrics(stream, document.metrics.includedAfterMarker);
    stream << "}";
    stream << ",\n    \"controlMaximumRgbDelta\": "
           << document.metrics.controlMaximumRgbDelta
           << ",\n    \"excludedOverlayRgbRange\": "
           << document.metrics.excludedOverlayRgbRange << "\n"
           << "  },\n"
           << "  \"resourceLedger\": ";
    writeLedger(stream, document.resourceLedger);
    stream << "\n}\n";
    stream.flush();
    if (!stream)
    {
        throw std::runtime_error("Unable to write WDA capture document");
    }
    stream.close();
    replaceFile(
        temporaryPath,
        finalPath,
        "MoveFileExW(WDA capture document)");
}

void writeFailureDocument(
    const ProbeOptions& options,
    const std::string_view error,
    const bafx::windows::WgcBackgroundResourceLedgerSnapshot& ledger)
{
    std::filesystem::create_directories(options.outputDirectory);
    const std::filesystem::path finalPath =
        options.outputDirectory / L"failure.json";
    const std::filesystem::path temporaryPath =
        options.outputDirectory / L"failure.json.tmp";
    std::ofstream stream(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        throw std::runtime_error("Unable to create WDA failure document");
    }
    stream << "{\n  \"schemaVersion\": 1,\n"
           << "  \"spikeId\": \"SPK-002-WDA-SELF-EXCLUSION\",\n"
           << "  \"revision\": ";
    writeJsonString(stream, options.revision);
    stream << ",\n  \"phase\": ";
    writeJsonString(stream, latestPhase);
    stream << ",\n  \"error\": ";
    writeJsonString(stream, error);
    stream << ",\n  \"resourceLedger\": ";
    writeLedger(stream, ledger);
    stream << "\n}\n";
    stream.flush();
    if (!stream)
    {
        throw std::runtime_error("Unable to write WDA failure document");
    }
    stream.close();
    replaceFile(
        temporaryPath,
        finalPath,
        "MoveFileExW(WDA failure document)");
}

[[nodiscard]] bafx::windows::WgcBackgroundSensorOptions sensorOptions(
    const std::shared_ptr<bafx::windows::WgcBackgroundResourceLedger>& ledger)
{
    bafx::windows::WgcBackgroundSensorOptions options{};
    options.epoch = 1U;
    options.excludesOwnOverlay = false;
    options.cursorExcluded = true;
    options.allowSystemBorder = true;
    options.resourceLedger = ledger;
    options.cursorCaptureEnabledOverride = false;
    return options;
}

int run(const ProbeOptions& options)
{
    if (options.help)
    {
        std::cout
            << "Usage: ba-click-fx-wgc-self-exclusion-spike "
            << "[--output=DIR] [--revision=GIT] [--timeout-ms=N]\n";
        return 0;
    }

    ProcessWatchdog watchdog(
        options.timeoutMilliseconds + watchdogGraceMilliseconds);
    const auto ledger =
        std::make_shared<bafx::windows::WgcBackgroundResourceLedger>();
    try
    {
        ComApartment apartment{};
        Deadline deadline(std::chrono::milliseconds(options.timeoutMilliseconds));
        enablePerMonitorDpiAwareness();
        QpcClock clock{};

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        if (instance == nullptr)
        {
            bafx::windows::throwLastError(
                "GetModuleHandleW(WDA self-exclusion spike)");
        }
        const RECT monitor = primaryMonitorBounds();
        const HMONITOR monitorHandle = MonitorFromRect(
            &monitor,
            MONITOR_DEFAULTTONULL);
        if (monitorHandle == nullptr)
        {
            throw std::runtime_error("Primary monitor handle is unavailable");
        }
        const RECT fixture = centeredFixtureBounds(monitor);
        const RECT overlayRectangle = overlayBounds(fixture);
        const bafx::windows::TextureReadbackRegion region =
            captureRegion(monitor, fixture);

        SolidBackgroundWindow background(instance, fixture);
        background.show();
        bafx::windows::OverlayWindow overlay(
            instance,
            overlayRectangle,
            L"ba-click-fx SPK-002 WDA overlay");
        overlay.show();
        establishFixtureZOrder(
            background.handle(),
            overlay.handle(),
            fixture);
        bafx::windows::throwIfFailed(
            DwmFlush(),
            "DwmFlush(WDA fixture ready)");

        reportPhase("renderer-create.begin");
        bafx::windows::CompositionRenderer renderer(
            overlay.handle(),
            overlay.size());
        const bafx::windows::GraphicsDeviceInfo rendererDevice =
            renderer.deviceInfo();
        if (rendererDevice.driverType
            != bafx::windows::GraphicsDriverType::Hardware)
        {
            throw std::runtime_error(
                "WDA self-exclusion requires hardware D3D11");
        }
        ObserverDevice observer = createObserverDevice(
            rendererDevice.adapterLuid);
        reportPhase("renderer-create.end");

        CaptureDocument document{};
        document.options = options;
        document.capturedAtUtc = utcTimestamp();
        document.osVersion = queryOsVersion();
        document.monitorBounds = monitor;
        document.captureScreenBounds = fixture;
        document.overlayScreenBounds = overlayRectangle;
        document.captureRegion = region;
        document.rendererDevice = rendererDevice;
        document.observerFeatureLevel = observer.featureLevel;

        reportPhase("wgc-session.begin");
        {
            bafx::windows::WgcBackgroundSensor sensor(
                observer.device.Get(),
                monitorHandle,
                sensorOptions(ledger));
            if (!sensor.running())
            {
                throw std::runtime_error("WDA WGC sensor did not start");
            }
            document.wgcCapabilities = sensor.capabilities();

            reportPhase("included-before.begin");
            document.includedBefore = captureStage(
                false,
                overlay,
                background,
                fixture,
                includedBeforeMarkerSrgb8,
                renderer,
                sensor,
                observer.context.Get(),
                clock,
                region,
                deadline);
            reportPhase("included-before.end");

            reportPhase("excluded.begin");
            document.excluded = captureStage(
                true,
                overlay,
                background,
                fixture,
                excludedMarkerSrgb8,
                renderer,
                sensor,
                observer.context.Get(),
                clock,
                region,
                deadline);
            reportPhase("excluded.end");

            reportPhase("included-after.begin");
            document.includedAfter = captureStage(
                false,
                overlay,
                background,
                fixture,
                includedAfterMarkerSrgb8,
                renderer,
                sensor,
                observer.context.Get(),
                clock,
                region,
                deadline);
            reportPhase("included-after.end");
            sensor.stop();
        }
        reportPhase("wgc-session.end");

        document.resourceLedger = ledger->snapshot();
        requireBalancedLedger(document.resourceLedger);
        document.metrics = calculateMetrics(
            document.includedBefore,
            document.excluded,
            document.includedAfter);
        requireCaptureEvidence(document);

        reportPhase("artifact-write.begin");
        document.includedBefore.artifact =
            bafx::capture::writeLayerArtifact(
                options.outputDirectory,
                L"included-before",
                document.includedBefore.image);
        document.excluded.artifact = bafx::capture::writeLayerArtifact(
            options.outputDirectory,
            L"excluded",
            document.excluded.image);
        document.includedAfter.artifact =
            bafx::capture::writeLayerArtifact(
                options.outputDirectory,
                L"included-after",
                document.includedAfter.image);
        writeCaptureDocument(options.outputDirectory, document);
        reportPhase("artifact-write.end");
        std::wcout << L"Wrote SPK-002 WDA self-exclusion capture: "
                   << (options.outputDirectory / L"self-exclusion.json").wstring()
                   << L'\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        try
        {
            writeFailureDocument(options, error.what(), ledger->snapshot());
        }
        catch (const std::exception& writeError)
        {
            std::cerr << "Unable to write WDA failure evidence: "
                      << writeError.what() << '\n';
        }
        throw;
    }
}

}

int wmain(const int argumentCount, wchar_t** arguments)
{
    try
    {
        return run(parseOptions(argumentCount, arguments));
    }
    catch (const std::exception& error)
    {
        std::cerr << "WGC self-exclusion spike failed: "
                  << error.what() << '\n';
        return 1;
    }
}
