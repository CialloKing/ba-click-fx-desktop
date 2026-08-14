#include "bafx/core/background_freshness.hpp"
#include "bafx/windows/composition_renderer.hpp"
#include "bafx/windows/display_capabilities.hpp"
#include "bafx/windows/error.hpp"
#include "bafx/windows/gpu_texture_readback.hpp"
#include "bafx/windows/overlay_window.hpp"
#include "bafx/windows/unique_handle.hpp"
#include "bafx/windows/wgc_background_sensor.hpp"

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
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
using bafx::capture::ComApartment;
using bafx::capture::Deadline;
using bafx::capture::ProcessWatchdog;

constexpr std::uint32_t defaultTimeoutMilliseconds = 20'000U;
constexpr std::uint32_t maximumTimeoutMilliseconds = 120'000U;
constexpr std::uint32_t probeExtentPixels = 256U;
constexpr std::size_t maximumMessagesPerPump = 256U;
constexpr std::size_t maximumSampleAttempts = 3U;
constexpr float stableSampleTolerance = 0.01F;

struct ProbeOptions
{
    std::filesystem::path outputDirectory{
        L"artifacts\\local\\spikes\\spk-001\\current"};
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

struct CapturedSample
{
    std::uint64_t generation{0U};
    std::int64_t capturedAtNanoseconds{0};
    bafx::windows::WindowSize contentSize{};
    bafx::windows::PixelF pixel{};
};

struct SampleAttempt
{
    std::int64_t presentMarkerNanoseconds{0};
    bafx::windows::PixelF prePresentPixel{};
    std::array<std::uint8_t, 3U> desktopGdiSrgb{};
    CapturedSample sample{};
};

struct PresentationCapture
{
    std::string name{};
    bafx::windows::PixelF requested{};
    std::vector<SampleAttempt> attempts{};
    std::optional<std::array<std::size_t, 2U>> stablePair{};
};

struct BackgroundCapture
{
    std::string name{};
    std::array<std::uint8_t, 3U> srgb{};
    PresentationCapture baseline{};
    std::vector<PresentationCapture> sources{};
};

struct CaptureDocument
{
    std::string capturedAtUtc{};
    ProbeOptions options{};
    OsVersion osVersion{};
    RECT monitorBounds{};
    RECT probeBounds{};
    bafx::windows::GraphicsDeviceInfo rendererDevice{};
    D3D_FEATURE_LEVEL observerFeatureLevel{D3D_FEATURE_LEVEL_11_0};
    std::optional<bafx::windows::DisplayColorCapabilities> display{};
    bafx::windows::CaptureExclusionStatus captureAffinity{};
    bafx::windows::WgcBackgroundSessionCapabilities wgcCapabilities{};
    std::vector<BackgroundCapture> backgrounds{};
};

class QpcClock final
{
public:
    QpcClock()
    {
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        {
            bafx::windows::throwLastError("QueryPerformanceFrequency");
        }
        frequency_ = frequency.QuadPart;
    }

    [[nodiscard]] bafx::core::MonotonicTime now() const
    {
        LARGE_INTEGER counter{};
        if (!QueryPerformanceCounter(&counter))
        {
            bafx::windows::throwLastError("QueryPerformanceCounter");
        }

        constexpr std::int64_t nanosecondsPerSecond = 1'000'000'000LL;
        const std::int64_t seconds = counter.QuadPart / frequency_;
        const std::int64_t remainder = counter.QuadPart % frequency_;
        return bafx::core::MonotonicTime{
            seconds * nanosecondsPerSecond
            + remainder * nanosecondsPerSecond / frequency_};
    }

private:
    std::int64_t frequency_{0};
};

class SolidBackgroundWindow final
{
public:
    SolidBackgroundWindow(
        const HINSTANCE instance,
        const RECT bounds,
        const COLORREF color)
        : color_(color)
    {
        registerWindowClass(instance);
        window_ = CreateWindowExW(
            WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            windowClassName,
            L"ba-click-fx SPK-001 controlled background",
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
            bafx::windows::throwLastError("CreateWindowExW(composition spike background)");
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
            bafx::windows::throwLastError("SetWindowPos(composition spike background)");
        }
    }

    void setColor(const COLORREF color)
    {
        color_ = color;
        if (!InvalidateRect(window_, nullptr, FALSE))
        {
            bafx::windows::throwLastError("InvalidateRect(composition spike background)");
        }
        UpdateWindow(window_);
    }

private:
    static constexpr wchar_t windowClassName[] =
        L"BaClickFxCompositionSpikeBackground";

    static ATOM registerWindowClass(const HINSTANCE instance)
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
                "RegisterClassExW(composition spike background)");
        }
        return atom;
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
            const HBRUSH brush = CreateSolidBrush(self->color_);
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
            const HBRUSH brush = CreateSolidBrush(self->color_);
            if (brush != nullptr)
            {
                FillRect(device, &paint.rcPaint, brush);
                DeleteObject(brush);
            }
            EndPaint(window, &paint);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    HWND window_{nullptr};
    COLORREF color_{RGB(0, 0, 0)};
};

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
    if (end == owned.c_str() || *end != L'\0'
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
        }
        else if (argument.starts_with(L"--output="))
        {
            const std::wstring_view value = argument.substr(9U);
            if (value.empty())
            {
                throw std::invalid_argument("--output requires a directory");
            }
            options.outputDirectory = std::wstring(value);
        }
        else if (argument.starts_with(L"--revision="))
        {
            const std::wstring_view value = argument.substr(11U);
            if (value.empty())
            {
                throw std::invalid_argument("--revision requires a value");
            }
            options.revision.clear();
            options.revision.reserve(value.size());
            for (const wchar_t character : value)
            {
                if (!isRevisionCharacter(character))
                {
                    throw std::invalid_argument(
                        "--revision contains a character that is unsafe for JSON");
                }
                options.revision.push_back(static_cast<char>(character));
            }
        }
        else if (argument.starts_with(L"--timeout-ms="))
        {
            const std::optional<std::uint32_t> parsed = parseUnsigned(
                argument.substr(13U));
            if (!parsed.has_value()
                || *parsed == 0U
                || *parsed > maximumTimeoutMilliseconds)
            {
                throw std::invalid_argument(
                    "--timeout-ms requires an integer in [1, 120000]");
            }
            options.timeoutMilliseconds = *parsed;
        }
        else
        {
            throw std::invalid_argument("Unknown composition spike option");
        }
    }
    return options;
}

void printUsage()
{
    std::wcout
        << L"Usage: ba-click-fx-composition-spike [--output=DIR] "
        << L"[--revision=GIT] [--timeout-ms=N]\n";
}

[[nodiscard]] RECT primaryMonitorBounds()
{
    const POINT origin{0, 0};
    const HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO information{};
    information.cbSize = sizeof(information);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &information))
    {
        bafx::windows::throwLastError("GetMonitorInfoW(primary monitor)");
    }
    return information.rcMonitor;
}

[[nodiscard]] RECT centeredProbeBounds(const RECT monitorBounds)
{
    const LONG monitorWidth = monitorBounds.right - monitorBounds.left;
    const LONG monitorHeight = monitorBounds.bottom - monitorBounds.top;
    if (monitorWidth < static_cast<LONG>(probeExtentPixels)
        || monitorHeight < static_cast<LONG>(probeExtentPixels))
    {
        throw std::runtime_error("Primary monitor is smaller than the probe window");
    }

    const LONG left = monitorBounds.left
        + (monitorWidth - static_cast<LONG>(probeExtentPixels)) / 2;
    const LONG top = monitorBounds.top
        + (monitorHeight - static_cast<LONG>(probeExtentPixels)) / 2;
    return RECT{
        left,
        top,
        left + static_cast<LONG>(probeExtentPixels),
        top + static_cast<LONG>(probeExtentPixels)};
}

void requireVisibleCompositionWindow(
    const HWND window,
    const std::string_view name)
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
    return GetWindowRect(window, &bounds)
        && PtInRect(&bounds, point);
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

void establishProbeZOrder(
    const HWND background,
    const HWND overlay,
    const POINT probeCenter)
{
    constexpr UINT flags = SWP_NOMOVE
        | SWP_NOSIZE
        | SWP_NOACTIVATE
        | SWP_SHOWWINDOW;
    // HWND_TOPMOST keeps a window in the band but need not reorder a window
    // already in that band. HWND_TOP makes this probe's two-window stack
    // deterministic without changing either window's topmost style.
    if (!SetWindowPos(background, HWND_TOP, 0, 0, 0, 0, flags))
    {
        bafx::windows::throwLastError(
            "SetWindowPos(composition spike background top)");
    }
    if (!SetWindowPos(overlay, HWND_TOP, 0, 0, 0, 0, flags))
    {
        bafx::windows::throwLastError(
            "SetWindowPos(composition spike overlay top)");
    }
    if (!SetWindowPos(background, overlay, 0, 0, 0, 0, flags))
    {
        bafx::windows::throwLastError(
            "SetWindowPos(composition spike background below overlay)");
    }
    if (firstVisibleWindowBelowAtPoint(overlay, probeCenter) != background)
    {
        throw std::runtime_error(
            "Controlled background is not directly below the overlay at the sample point");
    }
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
        "CreateDXGIFactory1(composition spike observer)");

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
            "IDXGIFactory1::EnumAdapters1(composition spike observer)");

        DXGI_ADAPTER_DESC1 description{};
        bafx::windows::throwIfFailed(
            adapter->GetDesc1(&description),
            "IDXGIAdapter1::GetDesc1(composition spike observer)");
        if (sameLuid(description.AdapterLuid, adapterLuid))
        {
            selectedAdapter = std::move(adapter);
            break;
        }
    }
    if (selectedAdapter == nullptr)
    {
        throw std::runtime_error(
            "Renderer adapter is unavailable for the WGC observer");
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
        "D3D11CreateDevice(composition spike observer)");
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
        case '\b':
            stream << "\\b";
            break;
        case '\f':
            stream << "\\f";
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

void writeJsonFloat(std::ostream& stream, const float value)
{
    if (std::isfinite(value))
    {
        stream << value;
    }
    else
    {
        // Keep the raw artifact valid JSON so the offline verifier can report
        // a channel-specific compositor failure instead of a parse failure.
        stream << "null";
    }
}

void writePixel(std::ostream& stream, const bafx::windows::PixelF pixel)
{
    stream << "{\"r\": ";
    writeJsonFloat(stream, pixel.red);
    stream << ", \"g\": ";
    writeJsonFloat(stream, pixel.green);
    stream << ", \"b\": ";
    writeJsonFloat(stream, pixel.blue);
    stream << ", \"a\": ";
    writeJsonFloat(stream, pixel.alpha);
    stream << '}';
}

[[nodiscard]] const char* colorSpaceName(
    const DXGI_COLOR_SPACE_TYPE colorSpace) noexcept
{
    switch (colorSpace)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        return "rgb-full-g22-p709";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        return "rgb-full-g10-p709";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        return "rgb-full-pq-p2020";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709:
        return "rgb-studio-g22-p709";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
        return "rgb-studio-pq-p2020";
    default:
        return "other";
    }
}

void writePresentation(
    std::ostream& stream,
    const PresentationCapture& presentation,
    const std::string_view indent)
{
    stream << indent << "{\"name\": ";
    writeJsonString(stream, presentation.name);
    stream << ", \"requestedSource\": ";
    writePixel(stream, presentation.requested);
    stream << ", \"attempts\": [\n";
    for (std::size_t index = 0U; index < presentation.attempts.size(); ++index)
    {
        const SampleAttempt& attempt = presentation.attempts[index];
        stream << indent << "  {\"presentMarkerNs\": "
               << attempt.presentMarkerNanoseconds
               << ", \"prePresentPixel\": ";
        writePixel(stream, attempt.prePresentPixel);
        stream << ", \"desktopGdiDiagnosticSrgb8\": ["
               << static_cast<unsigned int>(attempt.desktopGdiSrgb[0U]) << ", "
               << static_cast<unsigned int>(attempt.desktopGdiSrgb[1U]) << ", "
               << static_cast<unsigned int>(attempt.desktopGdiSrgb[2U]) << ']';
        stream << ", \"sample\": {\"generation\": "
               << attempt.sample.generation
               << ", \"capturedAtNs\": "
               << attempt.sample.capturedAtNanoseconds
               << ", \"contentSize\": {\"width\": "
               << attempt.sample.contentSize.width
               << ", \"height\": " << attempt.sample.contentSize.height << '}'
               << ", \"pixel\": ";
        writePixel(stream, attempt.sample.pixel);
        stream << "}}";
        if (index + 1U < presentation.attempts.size())
        {
            stream << ',';
        }
        stream << '\n';
    }
    stream << indent << "], \"stablePair\": ";
    if (presentation.stablePair.has_value())
    {
        stream << '[' << (*presentation.stablePair)[0U]
               << ", " << (*presentation.stablePair)[1U] << ']';
    }
    else
    {
        stream << "null";
    }
    stream << '}';
}

void writeCaptureDocument(
    const std::filesystem::path& outputDirectory,
    const CaptureDocument& document)
{
    std::filesystem::create_directories(outputDirectory);
    const std::filesystem::path finalPath = outputDirectory / L"capture.json";
    const std::filesystem::path temporaryPath = outputDirectory / L"capture.json.tmp";
    std::ofstream stream(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        throw std::runtime_error("Unable to create composition spike capture");
    }

    stream << std::setprecision(9);
    stream << "{\n"
           << "  \"schemaVersion\": 1,\n"
           << "  \"spikeId\": \"SPK-001\",\n"
           << "  \"applicationVersion\": \"" << BAFX_CAPTURE_VERSION << "\",\n"
           << "  \"revision\": ";
    writeJsonString(stream, document.options.revision);
    stream << ",\n  \"capturedAtUtc\": ";
    writeJsonString(stream, document.capturedAtUtc);
    stream << ",\n"
           << "  \"timeoutMs\": " << document.options.timeoutMilliseconds << ",\n"
           << "  \"contract\": {\"surfaceFormat\": "
           << "\"DXGI_FORMAT_R16G16B16A16_FLOAT\", "
           << "\"swapChainAlphaMode\": \"premultiplied\", "
           << "\"swapChainColorSpace\": \"rgb-full-g10-p709\", "
           << "\"observerFormat\": \"DXGI_FORMAT_R16G16B16A16_FLOAT\", "
           << "\"observerExcludesOwnOverlay\": false, "
           << "\"cursorExcluded\": true, "
           << "\"systemBorderAllowed\": true, "
           << "\"sourceInjection\": \"ClearRenderTargetView-production-swap-chain\", "
           << "\"desktopGdiDiagnosticSemantic\": \"diagnostic-only-unsynchronized\", "
           << "\"nonFiniteJsonEncoding\": \"null\", "
           << "\"formula\": \"C=S.rgb+(1-S.a)*B\", "
           << "\"stableSampleTolerance\": " << stableSampleTolerance << "},\n"
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
    stream << ",\n"
           << "  \"monitorBounds\": {\"left\": " << document.monitorBounds.left
           << ", \"top\": " << document.monitorBounds.top
           << ", \"right\": " << document.monitorBounds.right
           << ", \"bottom\": " << document.monitorBounds.bottom << "},\n"
           << "  \"probeBounds\": {\"left\": " << document.probeBounds.left
           << ", \"top\": " << document.probeBounds.top
           << ", \"right\": " << document.probeBounds.right
           << ", \"bottom\": " << document.probeBounds.bottom << "},\n"
           << "  \"rendererDevice\": {\"driverType\": \"hardware\", "
           << "\"adapter\": ";
    writeJsonString(stream, wideToUtf8(document.rendererDevice.adapterDescription));
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
           << static_cast<unsigned int>(document.observerFeatureLevel) << ",\n"
           << "  \"display\": ";
    if (document.display.has_value())
    {
        stream << "{\"colorSpace\": "
               << static_cast<unsigned int>(document.display->colorSpace)
               << ", \"colorSpaceName\": ";
        writeJsonString(stream, colorSpaceName(document.display->colorSpace));
        stream << ", \"bitsPerColor\": " << document.display->bitsPerColor
               << ", \"minimumLuminanceNits\": "
               << document.display->minimumLuminanceNits
               << ", \"maximumLuminanceNits\": "
               << document.display->maximumLuminanceNits
               << ", \"maximumFullFrameLuminanceNits\": "
               << document.display->maximumFullFrameLuminanceNits
               << ", \"luminanceMetadataValid\": "
               << (document.display->luminanceMetadataValid ? "true" : "false")
               << '}';
    }
    else
    {
        stream << "null";
    }
    stream << ",\n"
           << "  \"captureAffinity\": {\"requested\": "
           << document.captureAffinity.requestedAffinity
           << ", \"observed\": " << document.captureAffinity.observedAffinity
           << ", \"confirmed\": "
           << (document.captureAffinity.confirmed() ? "true" : "false")
           << "},\n"
           << "  \"wgcCapabilities\": {\"borderHidden\": "
           << (document.wgcCapabilities.borderHidden ? "true" : "false")
           << ", \"cursorExcluded\": "
           << (document.wgcCapabilities.cursorExcluded ? "true" : "false")
           << "},\n"
           << "  \"backgrounds\": [\n";
    for (std::size_t backgroundIndex = 0U;
         backgroundIndex < document.backgrounds.size();
         ++backgroundIndex)
    {
        const BackgroundCapture& background = document.backgrounds[backgroundIndex];
        stream << "    {\"name\": ";
        writeJsonString(stream, background.name);
        stream << ", \"srgb8\": ["
               << static_cast<unsigned int>(background.srgb[0U]) << ", "
               << static_cast<unsigned int>(background.srgb[1U]) << ", "
               << static_cast<unsigned int>(background.srgb[2U])
               << "], \"baseline\":\n";
        writePresentation(stream, background.baseline, "      ");
        stream << ",\n      \"sources\": [\n";
        for (std::size_t sourceIndex = 0U;
             sourceIndex < background.sources.size();
             ++sourceIndex)
        {
            writePresentation(stream, background.sources[sourceIndex], "        ");
            if (sourceIndex + 1U < background.sources.size())
            {
                stream << ',';
            }
            stream << '\n';
        }
        stream << "      ]}";
        if (backgroundIndex + 1U < document.backgrounds.size())
        {
            stream << ',';
        }
        stream << '\n';
    }
    stream << "  ]\n}\n";
    stream.flush();
    if (!stream)
    {
        throw std::runtime_error("Unable to write composition spike capture");
    }
    stream.close();

    if (!MoveFileExW(
            temporaryPath.c_str(),
            finalPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        bafx::windows::throwLastError("MoveFileExW(composition spike capture)");
    }
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
            throw std::runtime_error("Composition spike received WM_QUIT");
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

[[nodiscard]] bafx::windows::PixelF readSampleCenter(
    ID3D11DeviceContext* context,
    const bafx::windows::WgcBackgroundSample& sample)
{
    if (sample.texture == nullptr
        || sample.size.width == 0U
        || sample.size.height == 0U)
    {
        throw std::runtime_error("WGC sample has no readable texture");
    }

    ComPtr<ID3D11Resource> resource;
    sample.texture->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    bafx::windows::throwIfFailed(
        resource.As(&texture),
        "WGC sample resource QueryInterface(ID3D11Texture2D)");
    const bafx::windows::Rgba16FloatImage image =
        bafx::windows::readbackRgba16FloatTexture(
            context,
            texture.Get(),
            bafx::windows::TextureReadbackRegion{
                sample.size.width / 2U,
                sample.size.height / 2U,
                1U,
                1U});
    const bafx::windows::Rgba16FloatPixel pixel = image.pixels.front();
    return bafx::windows::PixelF{
        bafx::windows::halfToFloat(pixel.red),
        bafx::windows::halfToFloat(pixel.green),
        bafx::windows::halfToFloat(pixel.blue),
        bafx::windows::halfToFloat(pixel.alpha)};
}

[[nodiscard]] CapturedSample waitForFreshSample(
    bafx::windows::WgcBackgroundSensor& sensor,
    ID3D11DeviceContext* context,
    const std::uint64_t previousGeneration,
    const bafx::core::MonotonicTime presentMarker,
    Deadline& deadline)
{
    HANDLE frameEvent = sensor.frameAvailableObject();
    if (frameEvent == nullptr)
    {
        throw std::runtime_error("WGC frame event is unavailable");
    }

    std::uint64_t latestGeneration = previousGeneration;
    while (!deadline.expired())
    {
        pumpMessages(deadline);
        const bafx::windows::WgcBackgroundDrainStatus status =
            sensor.drainLatest(context);
        if (status == bafx::windows::WgcBackgroundDrainStatus::Stopped)
        {
            throw std::runtime_error("WGC stopped during composition capture");
        }
        if (status
            == bafx::windows::WgcBackgroundDrainStatus::ReconfigureRequired)
        {
            throw std::runtime_error("Display changed during composition capture");
        }
        if (status == bafx::windows::WgcBackgroundDrainStatus::Updated)
        {
            const std::optional<bafx::windows::WgcBackgroundSample> sample =
                sensor.latestSample();
            if (!sample.has_value())
            {
                throw std::runtime_error("WGC updated without a sample");
            }
            if (sample->generation > latestGeneration)
            {
                latestGeneration = sample->generation;
                if (sample->stamp.capturedAt > presentMarker)
                {
                    if (sample->stamp.epoch != sensor.expectedEpoch()
                        || !sample->stamp.canonicalLinearScRgb
                        || sample->stamp.excludesOwnOverlay)
                    {
                        throw std::runtime_error(
                            "WGC sample violates the composition probe contract");
                    }
                    return CapturedSample{
                        sample->generation,
                        sample->stamp.capturedAt.count(),
                        sample->size,
                        readSampleCenter(context, *sample)};
                }
            }
        }

        const DWORD waitResult = MsgWaitForMultipleObjectsEx(
            1U,
            &frameEvent,
            deadline.nextWaitMilliseconds(),
            QS_ALLINPUT,
            MWMO_INPUTAVAILABLE);
        if (waitResult == WAIT_FAILED)
        {
            bafx::windows::throwLastError(
                "MsgWaitForMultipleObjectsEx(composition spike)");
        }
        if (waitResult == WAIT_OBJECT_0 + 1U)
        {
            pumpMessages(deadline);
        }
    }
    throw std::runtime_error("Timed out waiting for a fresh WGC sample");
}

[[nodiscard]] bool pixelsStable(
    const bafx::windows::PixelF left,
    const bafx::windows::PixelF right) noexcept
{
    return std::abs(left.red - right.red) <= stableSampleTolerance
        && std::abs(left.green - right.green) <= stableSampleTolerance
        && std::abs(left.blue - right.blue) <= stableSampleTolerance
        && std::abs(left.alpha - right.alpha) <= stableSampleTolerance;
}

[[nodiscard]] SampleAttempt captureAttempt(
    bafx::windows::CompositionRenderer& renderer,
    const bafx::windows::PixelF source,
    bafx::windows::WgcBackgroundSensor& sensor,
    ID3D11DeviceContext* context,
    const QpcClock& clock,
    const POINT probeCenter,
    Deadline& deadline)
{
    const bafx::windows::WgcBackgroundDrainStatus drainStatus =
        sensor.drainLatest(context);
    if (drainStatus == bafx::windows::WgcBackgroundDrainStatus::Stopped
        || drainStatus
            == bafx::windows::WgcBackgroundDrainStatus::ReconfigureRequired)
    {
        throw std::runtime_error("WGC became unavailable before presentation");
    }
    const std::optional<bafx::windows::WgcBackgroundSample> previous =
        sensor.latestSample();
    const std::uint64_t previousGeneration = previous.has_value()
        ? previous->generation
        : 0U;

    const bafx::core::MonotonicTime marker = clock.now();
    const bafx::windows::PixelF prePresent =
        renderer.presentCompositionProbeColor(source);
    bafx::windows::throwIfFailed(DwmFlush(), "DwmFlush(composition spike)");

    const HDC desktop = GetDC(nullptr);
    if (desktop == nullptr)
    {
        bafx::windows::throwLastError("GetDC(composition spike desktop)");
    }
    const COLORREF desktopColor = GetPixel(desktop, probeCenter.x, probeCenter.y);
    ReleaseDC(nullptr, desktop);
    if (desktopColor == CLR_INVALID)
    {
        throw std::runtime_error("GetPixel failed for the composition probe center");
    }
    return SampleAttempt{
        marker.count(),
        prePresent,
        std::array<std::uint8_t, 3U>{
            GetRValue(desktopColor),
            GetGValue(desktopColor),
            GetBValue(desktopColor)},
        waitForFreshSample(
            sensor,
            context,
            previousGeneration,
            marker,
            deadline)};
}

[[nodiscard]] PresentationCapture capturePresentation(
    const std::string_view name,
    const bafx::windows::PixelF source,
    bafx::windows::CompositionRenderer& renderer,
    bafx::windows::WgcBackgroundSensor& sensor,
    ID3D11DeviceContext* context,
    const QpcClock& clock,
    const POINT probeCenter,
    Deadline& deadline)
{
    // Prime the flip-model queue before placing a timestamp marker. A second
    // identical Present then guarantees a dirty frame after that marker even
    // when the desktop would otherwise be static.
    static_cast<void>(renderer.presentCompositionProbeColor(source));
    bafx::windows::throwIfFailed(DwmFlush(), "DwmFlush(composition spike warmup)");

    PresentationCapture capture{};
    capture.name = std::string(name);
    capture.requested = source;
    capture.attempts.reserve(maximumSampleAttempts);
    for (std::size_t index = 0U; index < maximumSampleAttempts; ++index)
    {
        capture.attempts.push_back(captureAttempt(
            renderer,
            source,
            sensor,
            context,
            clock,
            probeCenter,
            deadline));
        if (capture.attempts.size() >= 2U)
        {
            const std::size_t right = capture.attempts.size() - 1U;
            const std::size_t left = right - 1U;
            if (pixelsStable(
                    capture.attempts[left].sample.pixel,
                    capture.attempts[right].sample.pixel))
            {
                capture.stablePair = std::array{left, right};
                break;
            }
        }
    }
    return capture;
}

[[nodiscard]] COLORREF backgroundColor(
    const std::array<std::uint8_t, 3U> srgb) noexcept
{
    return RGB(srgb[0U], srgb[1U], srgb[2U]);
}

[[nodiscard]] CaptureDocument collectCapture(const ProbeOptions& options)
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    if (instance == nullptr)
    {
        bafx::windows::throwLastError("GetModuleHandleW");
    }

    const RECT monitorBounds = primaryMonitorBounds();
    const HMONITOR monitor = MonitorFromRect(
        &monitorBounds,
        MONITOR_DEFAULTTONULL);
    if (monitor == nullptr)
    {
        throw std::runtime_error("Primary monitor handle is unavailable");
    }
    const RECT probeBounds = centeredProbeBounds(monitorBounds);
    const POINT probeCenter{
        probeBounds.left + (probeBounds.right - probeBounds.left) / 2,
        probeBounds.top + (probeBounds.bottom - probeBounds.top) / 2};

    SolidBackgroundWindow background(
        instance,
        probeBounds,
        RGB(0, 0, 0));
    background.show();

    bafx::windows::OverlayWindow overlay(
        instance,
        probeBounds,
        L"ba-click-fx SPK-001 overlay");
    bafx::windows::CompositionRenderer renderer(
        overlay.handle(),
        overlay.size());
    const bafx::windows::CaptureExclusionStatus affinity =
        overlay.setCaptureExcluded(false);
    if (!affinity.confirmed() || affinity.observedAffinity != WDA_NONE)
    {
        throw std::runtime_error(
            "Overlay capture affinity is not confirmed as WDA_NONE");
    }
    overlay.show();

    const bafx::windows::GraphicsDeviceInfo rendererDevice = renderer.deviceInfo();
    if (rendererDevice.driverType != bafx::windows::GraphicsDriverType::Hardware)
    {
        throw std::runtime_error(
            "SPK-001 requires the production renderer to use hardware D3D11");
    }
    ObserverDevice observer = createObserverDevice(rendererDevice.adapterLuid);
    bafx::windows::WgcBackgroundSensor sensor(
        observer.device.Get(),
        monitor,
        bafx::windows::WgcBackgroundSensorOptions{
            1U,
            false,
            true,
            true});

    Deadline deadline(std::chrono::milliseconds(options.timeoutMilliseconds));
    QpcClock clock{};
    const std::array backgroundDefinitions{
        std::pair{"black", std::array<std::uint8_t, 3U>{0U, 0U, 0U}},
        std::pair{"gray-18-percent", std::array<std::uint8_t, 3U>{119U, 119U, 119U}},
        std::pair{"color", std::array<std::uint8_t, 3U>{52U, 120U, 212U}},
        std::pair{"white", std::array<std::uint8_t, 3U>{255U, 255U, 255U}}};
    const std::array sourceDefinitions{
        std::pair{"transparent", bafx::windows::PixelF{0.0F, 0.0F, 0.0F, 0.0F}},
        std::pair{"additive-0.25", bafx::windows::PixelF{0.25F, 0.25F, 0.25F, 0.0F}},
        std::pair{"extended-1-alpha-0.25", bafx::windows::PixelF{1.0F, 1.0F, 1.0F, 0.25F}},
        std::pair{"extended-4-alpha-0.5", bafx::windows::PixelF{4.0F, 4.0F, 4.0F, 0.5F}}};

    CaptureDocument document{};
    document.capturedAtUtc = utcTimestamp();
    document.options = options;
    document.osVersion = queryOsVersion();
    document.monitorBounds = monitorBounds;
    document.probeBounds = probeBounds;
    document.rendererDevice = rendererDevice;
    document.observerFeatureLevel = observer.featureLevel;
    document.display = bafx::windows::queryDisplayColorCapabilities(monitor);
    document.captureAffinity = affinity;
    document.wgcCapabilities = sensor.capabilities();
    document.backgrounds.reserve(backgroundDefinitions.size());

    for (const auto& [backgroundName, srgb] : backgroundDefinitions)
    {
        if (deadline.expired())
        {
            throw std::runtime_error("Composition spike exceeded its total deadline");
        }
        background.setColor(backgroundColor(srgb));
        // Starting monitor capture can insert system-owned visuals into the
        // topmost band. Reassert the controlled background/overlay order for
        // every matrix row before placing a presentation marker.
        background.show();
        overlay.show();
        establishProbeZOrder(
            background.handle(),
            overlay.handle(),
            probeCenter);
        requireVisibleCompositionWindow(background.handle(), "background");
        requireVisibleCompositionWindow(overlay.handle(), "overlay");
        bafx::windows::throwIfFailed(
            DwmFlush(),
            "DwmFlush(composition spike background)");

        BackgroundCapture capture{};
        capture.name = backgroundName;
        capture.srgb = srgb;
        capture.baseline = capturePresentation(
            "baseline",
            bafx::windows::PixelF{},
            renderer,
            sensor,
            observer.context.Get(),
            clock,
            probeCenter,
            deadline);
        capture.sources.reserve(sourceDefinitions.size());
        for (const auto& [sourceName, source] : sourceDefinitions)
        {
            capture.sources.push_back(capturePresentation(
                sourceName,
                source,
                renderer,
                sensor,
                observer.context.Get(),
                clock,
                probeCenter,
                deadline));
        }
        document.backgrounds.push_back(std::move(capture));
    }
    return document;
}

int run(const ProbeOptions& options)
{
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
        && GetLastError() != ERROR_ACCESS_DENIED)
    {
        bafx::windows::throwLastError("SetProcessDpiAwarenessContext");
    }

    ComApartment apartment{};
    ProcessWatchdog watchdog(options.timeoutMilliseconds);
    const CaptureDocument document = collectCapture(options);
    writeCaptureDocument(options.outputDirectory, document);
    std::wcout << L"Wrote "
               << (options.outputDirectory / L"capture.json").wstring()
               << L'\n';
    return 0;
}

}

int wmain(const int argumentCount, wchar_t** arguments)
{
    try
    {
        const ProbeOptions options = parseOptions(argumentCount, arguments);
        if (options.help)
        {
            printUsage();
            return 0;
        }
        return run(options);
    }
    catch (const std::exception& error)
    {
        std::cerr << "ba-click-fx-composition-spike: " << error.what() << '\n';
        return 1;
    }
}
