#include "bafx/windows/error.hpp"
#include "bafx/windows/gpu_texture_readback.hpp"
#include "bafx/windows/wgc_background_sensor.hpp"

#include "capture_artifact_writer.hpp"
#include "spike_runtime.hpp"

#include <windows.h>
#include <winternl.h>

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
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

constexpr std::uint32_t defaultTimeoutMilliseconds = 12'000U;
constexpr std::uint32_t maximumTimeoutMilliseconds = 120'000U;
constexpr std::uint32_t watchdogGraceMilliseconds = 3'000U;
constexpr std::size_t maximumMessagesPerPump = 256U;
constexpr bafx::windows::WindowSize fixtureSize{320U, 240U};
constexpr std::uint32_t cursorExtent = 32U;
constexpr std::uint32_t cursorHotspot = cursorExtent / 2U;
constexpr std::uint32_t cursorOpaquePixels = 176U;
constexpr std::uint32_t cursorRoiExtent = 96U;
constexpr float differenceThreshold = 0.02F;
constexpr float maximumBackgroundRange = 0.01F;
constexpr float maximumControlDelta = 0.01F;
constexpr float minimumCursorDelta = 0.25F;
constexpr std::uint64_t minimumDifferentPixels = cursorOpaquePixels / 4U;
constexpr std::uint64_t maximumStabilityDifferentPixels = 4U;
constexpr bafx::windows::TextureReadbackRegion controlRoi{
    16U,
    16U,
    32U,
    32U};
constexpr COLORREF captureColor = RGB(32, 96, 160);
constexpr COLORREF transitionColor = RGB(160, 72, 48);
std::string latestPhase{"startup"};

struct ProbeOptions
{
    std::filesystem::path outputDirectory{
        L"artifacts\\local\\spikes\\spk-002-cursor\\current"};
    std::string revision{"unrecorded"};
    std::uint32_t timeoutMilliseconds{defaultTimeoutMilliseconds};
    bool help{false};
};

struct DeviceResources
{
    ComPtr<ID3D11Device> device{};
    ComPtr<ID3D11DeviceContext> context{};
    DXGI_ADAPTER_DESC adapter{};
    D3D_FEATURE_LEVEL featureLevel{D3D_FEATURE_LEVEL_11_0};
};

struct OsVersion
{
    std::uint32_t major{0U};
    std::uint32_t minor{0U};
    std::uint32_t build{0U};
    bool available{false};
};

struct PixelBounds
{
    std::uint32_t left{0U};
    std::uint32_t top{0U};
    std::uint32_t right{0U};
    std::uint32_t bottom{0U};
};

struct ModeObservation
{
    bool requestedCursorExcluded{false};
    bafx::windows::WgcBackgroundSessionCapabilities capabilities{};
    std::uint64_t previousGeneration{0U};
    std::uint64_t generation{0U};
    std::int64_t markerNanoseconds{0};
    std::int64_t capturedAtNanoseconds{0};
    bafx::windows::WindowSize size{};
    bafx::windows::Rgba16FloatImage image{};
    bafx::windows::WgcBackgroundResourceLedgerSnapshot ledger{};
};

struct PixelComparison
{
    bafx::windows::TextureReadbackRegion roi{};
    float threshold{differenceThreshold};
    float excludedRgbRange{0.0F};
    float maximumRgbDelta{0.0F};
    std::uint64_t differentPixels{0U};
    std::uint64_t edgeDifferentPixels{0U};
    std::optional<PixelBounds> differenceBounds{};
};

struct CaptureDocument
{
    ProbeOptions options{};
    std::string capturedAtUtc{};
    OsVersion osVersion{};
    DeviceResources device{};
    POINT fixtureOrigin{};
    POINT cursorScreenPoint{};
    POINT cursorClientPoint{};
    ModeObservation includedBefore{};
    ModeObservation excluded{};
    ModeObservation includedAfter{};
    PixelComparison includedBeforeComparison{};
    PixelComparison includedAfterComparison{};
    PixelComparison includedStability{};
    float controlMaximumRgbDelta{0.0F};
    bafx::capture::LayerArtifact includedBeforeArtifact{};
    bafx::capture::LayerArtifact excludedArtifact{};
    bafx::capture::LayerArtifact includedAfterArtifact{};
};

class CursorRestorer final
{
public:
    CursorRestorer()
    {
        positionKnown_ = GetCursorPos(&position_) != FALSE;
        CURSORINFO info{};
        info.cbSize = sizeof(info);
        if (GetCursorInfo(&info))
        {
            cursor_ = info.hCursor;
        }
    }

    ~CursorRestorer()
    {
        restore();
    }

    CursorRestorer(const CursorRestorer&) = delete;
    CursorRestorer& operator=(const CursorRestorer&) = delete;

    void restore() noexcept
    {
        if (restored_)
        {
            return;
        }
        if (positionKnown_)
        {
            SetCursorPos(position_.x, position_.y);
        }
        if (cursor_ != nullptr)
        {
            SetCursor(cursor_);
        }
        restored_ = true;
    }

private:
    POINT position_{};
    HCURSOR cursor_{nullptr};
    bool positionKnown_{false};
    bool restored_{false};
};

class ControlledCursorWindow final
{
public:
    ControlledCursorWindow()
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        if (instance == nullptr)
        {
            bafx::windows::throwLastError("GetModuleHandleW(WGC cursor fixture)");
        }
        cursor_ = createProbeCursor(instance);
        registerWindowClass(instance, cursor_);

        const RECT bounds = centeredBounds();
        window_ = CreateWindowExW(
            0U,
            windowClassName,
            L"ba-click-fx SPK-002 cursor fixture",
            WS_POPUP | WS_VISIBLE,
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
            bafx::windows::throwLastError("CreateWindowExW(WGC cursor fixture)");
        }
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
                "SetWindowPos(WGC cursor fixture topmost)");
        }
        if (!UpdateWindow(window_))
        {
            bafx::windows::throwLastError("UpdateWindow(WGC cursor fixture)");
        }
    }

    ~ControlledCursorWindow()
    {
        if (window_ != nullptr)
        {
            DestroyWindow(window_);
            window_ = nullptr;
        }
        if (cursor_ != nullptr)
        {
            DestroyCursor(cursor_);
            cursor_ = nullptr;
        }
    }

    ControlledCursorWindow(const ControlledCursorWindow&) = delete;
    ControlledCursorWindow& operator=(const ControlledCursorWindow&) = delete;

    [[nodiscard]] HWND handle() const noexcept
    {
        return window_;
    }

    void requireCapturable() const
    {
        if (window_ == nullptr || !IsWindow(window_))
        {
            throw std::runtime_error("WGC cursor fixture HWND is invalid");
        }
        if (!IsWindowVisible(window_))
        {
            throw std::runtime_error("WGC cursor fixture HWND is not visible");
        }
    }

    [[nodiscard]] POINT origin() const
    {
        POINT point{};
        if (!ClientToScreen(window_, &point))
        {
            bafx::windows::throwLastError(
                "ClientToScreen(WGC cursor fixture origin)");
        }
        return point;
    }

    [[nodiscard]] POINT cursorClientPoint() const noexcept
    {
        return POINT{
            static_cast<LONG>(fixtureSize.width / 2U),
            static_cast<LONG>(fixtureSize.height / 2U)};
    }

    [[nodiscard]] POINT cursorScreenPoint() const
    {
        POINT point = cursorClientPoint();
        if (!ClientToScreen(window_, &point))
        {
            bafx::windows::throwLastError(
                "ClientToScreen(WGC cursor fixture center)");
        }
        return point;
    }

    void setColor(const COLORREF color)
    {
        color_ = color;
        if (!InvalidateRect(window_, nullptr, FALSE))
        {
            bafx::windows::throwLastError(
                "InvalidateRect(WGC cursor fixture)");
        }
        if (!UpdateWindow(window_))
        {
            bafx::windows::throwLastError(
                "UpdateWindow(WGC cursor fixture repaint)");
        }
    }

    void positionCursor() const
    {
        const POINT point = cursorScreenPoint();
        if (!SetCursorPos(point.x - 1, point.y)
            || !SetCursorPos(point.x, point.y))
        {
            bafx::windows::throwLastError(
                "SetCursorPos(WGC cursor fixture)");
        }
        SetCursor(cursor_);
    }

    void requireCursorState() const
    {
        POINT position{};
        if (!GetCursorPos(&position))
        {
            bafx::windows::throwLastError(
                "GetCursorPos(WGC cursor fixture)");
        }
        const POINT expected = cursorScreenPoint();
        if (position.x != expected.x || position.y != expected.y)
        {
            throw std::runtime_error(
                "System cursor moved during WGC cursor capture");
        }
        if (WindowFromPoint(position) != window_)
        {
            throw std::runtime_error(
                "WGC cursor fixture is not under the requested cursor point");
        }

        CURSORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetCursorInfo(&info))
        {
            bafx::windows::throwLastError(
                "GetCursorInfo(WGC cursor fixture)");
        }
        if ((info.flags & CURSOR_SHOWING) == 0U)
        {
            std::ostringstream message;
            message << "System cursor is not visible during capture; flags=0x"
                    << std::hex << info.flags;
            throw std::runtime_error(message.str());
        }
        if (info.hCursor == nullptr)
        {
            throw std::runtime_error(
                "System cursor has no observable handle during capture");
        }
        // USER may alternate between the caller handle and an internal alias
        // for the same custom raster. The repeated raw-frame mask, rather than
        // handle identity, is the durable shape proof.
    }

private:
    static constexpr wchar_t windowClassName[] =
        L"BaClickFxWgcCursorFixture";

    [[nodiscard]] static HCURSOR createProbeCursor(
        const HINSTANCE instance)
    {
        constexpr std::size_t maskBytes =
            static_cast<std::size_t>(cursorExtent) * cursorExtent / 8U;
        std::array<BYTE, maskBytes> andMask{};
        std::array<BYTE, maskBytes> xorMask{};
        andMask.fill(0xFFU);

        const auto setOpaqueWhite = [&andMask, &xorMask](
            const std::uint32_t x,
            const std::uint32_t y)
        {
            const std::size_t byteIndex =
                static_cast<std::size_t>(y) * (cursorExtent / 8U)
                + x / 8U;
            const BYTE bit = static_cast<BYTE>(0x80U >> (x % 8U));
            andMask[byteIndex] = static_cast<BYTE>(
                andMask[byteIndex] & static_cast<BYTE>(~bit));
            xorMask[byteIndex] = static_cast<BYTE>(
                xorMask[byteIndex] | bit);
        };
        for (std::uint32_t coordinate = 4U; coordinate < 28U; ++coordinate)
        {
            for (std::uint32_t thickness = 14U;
                 thickness < 18U;
                 ++thickness)
            {
                setOpaqueWhite(thickness, coordinate);
                setOpaqueWhite(coordinate, thickness);
            }
        }

        HCURSOR cursor = CreateCursor(
            instance,
            cursorHotspot,
            cursorHotspot,
            cursorExtent,
            cursorExtent,
            andMask.data(),
            xorMask.data());
        if (cursor == nullptr)
        {
            bafx::windows::throwLastError("CreateCursor(WGC cursor fixture)");
        }
        return cursor;
    }

    [[nodiscard]] static RECT centeredBounds()
    {
        POINT primaryPoint{};
        const HMONITOR monitor = MonitorFromPoint(
            primaryPoint,
            MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (monitor == nullptr || !GetMonitorInfoW(monitor, &info))
        {
            bafx::windows::throwLastError(
                "GetMonitorInfoW(WGC cursor fixture)");
        }
        const LONG width = static_cast<LONG>(fixtureSize.width);
        const LONG height = static_cast<LONG>(fixtureSize.height);
        const LONG left = info.rcWork.left
            + (info.rcWork.right - info.rcWork.left - width) / 2;
        const LONG top = info.rcWork.top
            + (info.rcWork.bottom - info.rcWork.top - height) / 2;
        return RECT{left, top, left + width, top + height};
    }

    static void registerWindowClass(
        const HINSTANCE instance,
        const HCURSOR cursor)
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &ControlledCursorWindow::windowProcedure;
        windowClass.hInstance = instance;
        windowClass.hCursor = cursor;
        windowClass.lpszClassName = windowClassName;

        const ATOM atom = RegisterClassExW(&windowClass);
        if (atom == 0U && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            bafx::windows::throwLastError(
                "RegisterClassExW(WGC cursor fixture)");
        }
    }

    static LRESULT CALLBACK windowProcedure(
        const HWND window,
        const UINT message,
        const WPARAM wParam,
        const LPARAM lParam)
    {
        auto* self = reinterpret_cast<ControlledCursorWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<ControlledCursorWindow*>(
                create->lpCreateParams);
            SetWindowLongPtrW(
                window,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(self));
        }
        if (self == nullptr)
        {
            return DefWindowProcW(window, message, wParam, lParam);
        }
        if (message == WM_SETCURSOR
            && LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(self->cursor_);
            return TRUE;
        }
        if (message == WM_ERASEBKGND)
        {
            return 1;
        }
        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint{};
            const HDC device = BeginPaint(window, &paint);
            const HBRUSH brush = CreateSolidBrush(self->color_);
            if (brush == nullptr)
            {
                EndPaint(window, &paint);
                return 0;
            }
            FillRect(device, &paint.rcPaint, brush);
            DeleteObject(brush);
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
    HCURSOR cursor_{nullptr};
    COLORREF color_{captureColor};
};

void reportPhase(const std::string_view phase)
{
    latestPhase.assign(phase);
    // The watchdog can terminate below WinRT or the driver, so always flush
    // the final completed phase before entering either boundary.
    std::cerr << "SPK-002 cursor phase: " << phase << std::endl;
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
            "SetProcessDpiAwarenessContext(WGC cursor spike)");
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
        throw std::invalid_argument("Unknown WGC cursor spike option");
    }
    return options;
}

[[nodiscard]] DeviceResources createHardwareDevice()
{
    constexpr std::array featureLevels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    DeviceResources result{};
    bafx::windows::throwIfFailed(
        D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels.data(),
            static_cast<UINT>(featureLevels.size()),
            D3D11_SDK_VERSION,
            &result.device,
            &result.featureLevel,
            &result.context),
        "D3D11CreateDevice(WGC cursor spike)");

    ComPtr<IDXGIDevice> dxgiDevice;
    bafx::windows::throwIfFailed(
        result.device.As(&dxgiDevice),
        "ID3D11Device::QueryInterface(IDXGIDevice cursor spike)");
    ComPtr<IDXGIAdapter> adapter;
    bafx::windows::throwIfFailed(
        dxgiDevice->GetAdapter(&adapter),
        "IDXGIDevice::GetAdapter(cursor spike)");
    bafx::windows::throwIfFailed(
        adapter->GetDesc(&result.adapter),
        "IDXGIAdapter::GetDesc(cursor spike)");
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
            throw std::runtime_error("WGC cursor spike received WM_QUIT");
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
        throw std::runtime_error("WGC cursor frame event is unavailable");
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
            "MsgWaitForMultipleObjectsEx(WGC cursor spike)");
    }
    if (result == WAIT_OBJECT_0 + 1U)
    {
        pumpMessages(deadline);
    }
}

[[nodiscard]] bafx::windows::Rgba16FloatImage readSample(
    ID3D11DeviceContext* context,
    const bafx::windows::WgcBackgroundSample& sample)
{
    if (sample.texture == nullptr
        || sample.size.width == 0U
        || sample.size.height == 0U)
    {
        throw std::runtime_error("WGC cursor sample has no readable texture");
    }
    ComPtr<ID3D11Resource> resource;
    sample.texture->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    bafx::windows::throwIfFailed(
        resource.As(&texture),
        "WGC cursor sample QueryInterface(ID3D11Texture2D)");
    return bafx::windows::readbackRgba16FloatTexture(
        context,
        texture.Get());
}

[[nodiscard]] ModeObservation waitForFreshImage(
    bafx::windows::WgcBackgroundSensor& sensor,
    ID3D11DeviceContext* context,
    const bool cursorExcluded,
    const std::uint64_t previousGeneration,
    const bafx::core::MonotonicTime marker,
    Deadline& deadline)
{
    while (!deadline.expired())
    {
        pumpMessages(deadline);
        const bafx::windows::WgcBackgroundDrainStatus status =
            sensor.drainLatest(context);
        if (status == bafx::windows::WgcBackgroundDrainStatus::Stopped)
        {
            throw std::runtime_error("WGC stopped during cursor capture");
        }
        if (status
            == bafx::windows::WgcBackgroundDrainStatus::ReconfigureRequired)
        {
            const std::optional<bafx::windows::WindowSize> pending =
                sensor.pendingFramePoolSize();
            if (!pending.has_value())
            {
                throw std::runtime_error(
                    "WGC cursor reconfigure has no pending size");
            }
            sensor.recreateFramePool(*pending);
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
                    "WGC cursor sample violates the sensor contract");
            }
            if (sample->generation > previousGeneration
                && sample->stamp.capturedAt > marker)
            {
                ModeObservation observation{};
                observation.requestedCursorExcluded = cursorExcluded;
                observation.capabilities = sensor.capabilities();
                observation.previousGeneration = previousGeneration;
                observation.generation = sample->generation;
                observation.markerNanoseconds = marker.count();
                observation.capturedAtNanoseconds =
                    sample->stamp.capturedAt.count();
                observation.size = sample->size;
                observation.image = readSample(context, *sample);
                return observation;
            }
        }
        waitForSensorActivity(sensor, deadline);
    }
    throw std::runtime_error("Timed out waiting for a fresh WGC cursor frame");
}

[[nodiscard]] bafx::windows::WgcBackgroundSensorOptions sensorOptions(
    const bool cursorExcluded,
    const std::uint64_t epoch,
    const std::shared_ptr<bafx::windows::WgcBackgroundResourceLedger>& ledger)
{
    bafx::windows::WgcBackgroundSensorOptions options{};
    options.epoch = epoch;
    options.cursorExcluded = cursorExcluded;
    options.allowSystemBorder = true;
    options.resourceLedger = ledger;
    options.cursorCaptureEnabledOverride = !cursorExcluded;
    return options;
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
        throw std::runtime_error("WGC cursor resource ledger is not balanced");
    }
}

[[nodiscard]] ModeObservation captureMode(
    DeviceResources& device,
    ControlledCursorWindow& window,
    const QpcClock& clock,
    const bool cursorExcluded,
    const std::uint64_t epoch,
    const std::shared_ptr<bafx::windows::WgcBackgroundResourceLedger>& ledger,
    Deadline& deadline)
{
    ModeObservation observation{};
    {
        bafx::windows::WgcBackgroundSensor sensor(
            device.device.Get(),
            window.handle(),
            sensorOptions(cursorExcluded, epoch, ledger));
        if (!sensor.running())
        {
            throw std::runtime_error("WGC cursor sensor did not start");
        }

        window.positionCursor();
        pumpMessages(deadline);
        window.requireCursorState();
        const bafx::core::MonotonicTime transitionMarker = clock.now();
        window.setColor(transitionColor);
        bafx::windows::throwIfFailed(
            DwmFlush(),
            "DwmFlush(WGC cursor transition)");
        window.requireCursorState();
        ModeObservation transition = waitForFreshImage(
            sensor,
            device.context.Get(),
            cursorExcluded,
            0U,
            transitionMarker,
            deadline);
        window.requireCursorState();

        window.positionCursor();
        pumpMessages(deadline);
        window.requireCursorState();
        const bafx::core::MonotonicTime captureMarker = clock.now();
        window.setColor(captureColor);
        bafx::windows::throwIfFailed(
            DwmFlush(),
            "DwmFlush(WGC cursor capture)");
        window.requireCursorState();
        observation = waitForFreshImage(
            sensor,
            device.context.Get(),
            cursorExcluded,
            transition.generation,
            captureMarker,
            deadline);
        window.requireCursorState();
        sensor.stop();
    }
    observation.ledger = ledger->snapshot();
    requireBalancedLedger(observation.ledger);
    return observation;
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

[[nodiscard]] float rgbRange(
    const bafx::windows::Rgba16FloatImage& image,
    const bafx::windows::TextureReadbackRegion region)
{
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
            const auto& pixel = image.pixels[
                static_cast<std::size_t>(region.top + y) * image.width
                + region.left + x];
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

[[nodiscard]] float regionMaximumRgbDelta(
    const bafx::windows::Rgba16FloatImage& left,
    const bafx::windows::Rgba16FloatImage& right,
    const bafx::windows::TextureReadbackRegion region)
{
    if (left.width != right.width
        || left.height != right.height
        || region.left + region.width > left.width
        || region.top + region.height > left.height)
    {
        throw std::runtime_error("WGC cursor comparison region is invalid");
    }
    float maximum = 0.0F;
    for (std::uint32_t y = 0U; y < region.height; ++y)
    {
        for (std::uint32_t x = 0U; x < region.width; ++x)
        {
            const std::size_t index =
                static_cast<std::size_t>(region.top + y) * left.width
                + region.left + x;
            maximum = std::max(
                maximum,
                maximumRgbDelta(left.pixels[index], right.pixels[index]));
        }
    }
    return maximum;
}

[[nodiscard]] PixelComparison compareObservations(
    const ModeObservation& included,
    const ModeObservation& excluded,
    const POINT cursorClientPoint)
{
    if (included.image.width != excluded.image.width
        || included.image.height != excluded.image.height
        || included.image.width != fixtureSize.width
        || included.image.height != fixtureSize.height)
    {
        throw std::runtime_error("WGC cursor images have unexpected dimensions");
    }
    const std::uint32_t halfExtent = cursorRoiExtent / 2U;
    if (cursorClientPoint.x < static_cast<LONG>(halfExtent)
        || cursorClientPoint.y < static_cast<LONG>(halfExtent))
    {
        throw std::runtime_error("WGC cursor ROI underflows the fixture");
    }

    PixelComparison comparison{};
    comparison.roi = bafx::windows::TextureReadbackRegion{
        static_cast<std::uint32_t>(cursorClientPoint.x) - halfExtent,
        static_cast<std::uint32_t>(cursorClientPoint.y) - halfExtent,
        cursorRoiExtent,
        cursorRoiExtent};
    comparison.excludedRgbRange = rgbRange(excluded.image, comparison.roi);

    PixelBounds bounds{
        comparison.roi.left + comparison.roi.width,
        comparison.roi.top + comparison.roi.height,
        0U,
        0U};
    for (std::uint32_t y = 0U; y < comparison.roi.height; ++y)
    {
        for (std::uint32_t x = 0U; x < comparison.roi.width; ++x)
        {
            const std::uint32_t imageX = comparison.roi.left + x;
            const std::uint32_t imageY = comparison.roi.top + y;
            const std::size_t index =
                static_cast<std::size_t>(imageY) * included.image.width
                + imageX;
            const float delta = maximumRgbDelta(
                included.image.pixels[index],
                excluded.image.pixels[index]);
            comparison.maximumRgbDelta = std::max(
                comparison.maximumRgbDelta,
                delta);
            if (delta <= comparison.threshold)
            {
                continue;
            }
            ++comparison.differentPixels;
            bounds.left = std::min(bounds.left, imageX);
            bounds.top = std::min(bounds.top, imageY);
            bounds.right = std::max(bounds.right, imageX);
            bounds.bottom = std::max(bounds.bottom, imageY);
            if (x == 0U
                || y == 0U
                || x + 1U == comparison.roi.width
                || y + 1U == comparison.roi.height)
            {
                ++comparison.edgeDifferentPixels;
            }
        }
    }
    if (comparison.differentPixels != 0U)
    {
        comparison.differenceBounds = bounds;
    }
    return comparison;
}

void requireCursorEvidence(
    const ModeObservation& includedBefore,
    const ModeObservation& excluded,
    const ModeObservation& includedAfter,
    const PixelComparison& includedBeforeComparison,
    const PixelComparison& includedAfterComparison,
    const PixelComparison& includedStability,
    const float controlMaximumRgbDelta,
    const POINT cursorClientPoint)
{
    if (includedBefore.requestedCursorExcluded
        || !excluded.requestedCursorExcluded
        || includedAfter.requestedCursorExcluded)
    {
        throw std::runtime_error("WGC cursor mode labels are inverted");
    }
    const auto includedConfirmed = [](const ModeObservation& observation)
    {
        return observation.capabilities.cursorControlConfirmed
            && observation.capabilities.cursorCaptureEnabled
            && !observation.capabilities.cursorExcluded;
    };
    const bool excludedConfirmed =
        excluded.capabilities.cursorControlConfirmed
        && !excluded.capabilities.cursorCaptureEnabled
        && excluded.capabilities.cursorExcluded;
    if (!includedConfirmed(includedBefore)
        || !excludedConfirmed
        || !includedConfirmed(includedAfter))
    {
        throw std::runtime_error("WGC cursor capability readback is inconsistent");
    }

    const auto requireMaterialDifference = [cursorClientPoint](
        const PixelComparison& comparison)
    {
        if (comparison.excludedRgbRange > maximumBackgroundRange)
        {
            throw std::runtime_error(
                "Cursor-excluded WGC ROI is not a uniform controlled background");
        }
        if (comparison.differentPixels < minimumDifferentPixels
            || comparison.maximumRgbDelta < minimumCursorDelta
            || !comparison.differenceBounds.has_value())
        {
            throw std::runtime_error(
                "Cursor inclusion did not produce a material pixel difference");
        }
        if (comparison.edgeDifferentPixels != 0U)
        {
            throw std::runtime_error(
                "WGC cursor difference reached the controlled ROI boundary");
        }

        constexpr std::uint32_t shadowMargin = 8U;
        const PixelBounds bounds = *comparison.differenceBounds;
        const std::uint32_t minimumLeft =
            static_cast<std::uint32_t>(cursorClientPoint.x)
            - cursorHotspot - shadowMargin;
        const std::uint32_t minimumTop =
            static_cast<std::uint32_t>(cursorClientPoint.y)
            - cursorHotspot - shadowMargin;
        const std::uint32_t maximumRight =
            static_cast<std::uint32_t>(cursorClientPoint.x)
            + (cursorExtent - cursorHotspot - 1U) + shadowMargin;
        const std::uint32_t maximumBottom =
            static_cast<std::uint32_t>(cursorClientPoint.y)
            + (cursorExtent - cursorHotspot - 1U) + shadowMargin;
        if (bounds.left < minimumLeft
            || bounds.top < minimumTop
            || bounds.right > maximumRight
            || bounds.bottom > maximumBottom)
        {
            throw std::runtime_error(
                "WGC cursor difference escaped the expected cursor raster");
        }
    };
    requireMaterialDifference(includedBeforeComparison);
    requireMaterialDifference(includedAfterComparison);

    if (controlMaximumRgbDelta > maximumControlDelta)
    {
        throw std::runtime_error(
            "WGC cursor control ROI changed between capture modes");
    }
    if (includedStability.differentPixels
            > maximumStabilityDifferentPixels
        || includedStability.maximumRgbDelta > differenceThreshold)
    {
        throw std::runtime_error(
            "Repeated cursor-included captures are not pixel-stable");
    }

    const auto coordinateDistance = [](const std::uint32_t left,
                                       const std::uint32_t right) noexcept
    {
        return left > right ? left - right : right - left;
    };
    const PixelBounds beforeBounds =
        *includedBeforeComparison.differenceBounds;
    const PixelBounds afterBounds =
        *includedAfterComparison.differenceBounds;
    if (coordinateDistance(beforeBounds.left, afterBounds.left) > 1U
        || coordinateDistance(beforeBounds.top, afterBounds.top) > 1U
        || coordinateDistance(beforeBounds.right, afterBounds.right) > 1U
        || coordinateDistance(beforeBounds.bottom, afterBounds.bottom) > 1U)
    {
        throw std::runtime_error(
            "Repeated cursor-included difference bounds moved");
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

void writeMode(
    std::ostream& stream,
    const ModeObservation& observation,
    const std::string_view rawFile,
    const std::string_view pngFile,
    const bafx::capture::LayerArtifact artifact)
{
    stream << "{\"requestedCursorExcluded\": "
           << (observation.requestedCursorExcluded ? "true" : "false")
           << ", \"capabilities\": {\"borderHidden\": "
           << (observation.capabilities.borderHidden ? "true" : "false")
           << ", \"cursorExcluded\": "
           << (observation.capabilities.cursorExcluded ? "true" : "false")
           << ", \"cursorCaptureEnabled\": "
           << (observation.capabilities.cursorCaptureEnabled
                ? "true"
                : "false")
           << ", \"cursorControlConfirmed\": "
           << (observation.capabilities.cursorControlConfirmed
                ? "true"
                : "false")
           << "}, \"previousGeneration\": "
           << observation.previousGeneration
           << ", \"generation\": " << observation.generation
           << ", \"markerNanoseconds\": "
           << observation.markerNanoseconds
           << ", \"capturedAtNanoseconds\": "
           << observation.capturedAtNanoseconds
           << ", \"size\": {\"width\": " << observation.size.width
           << ", \"height\": " << observation.size.height
           << "}, \"artifact\": {\"raw\": ";
    writeJsonString(stream, rawFile);
    stream << ", \"png\": ";
    writeJsonString(stream, pngFile);
    stream << ", \"width\": " << artifact.width
           << ", \"height\": " << artifact.height
           << ", \"rawBytes\": " << artifact.rawBytes
           << "}, \"ledger\": ";
    writeLedger(stream, observation.ledger);
    stream << '}';
}

void writeComparison(
    std::ostream& stream,
    const PixelComparison& comparison)
{
    stream << "{\"roi\": {\"left\": " << comparison.roi.left
           << ", \"top\": " << comparison.roi.top
           << ", \"width\": " << comparison.roi.width
           << ", \"height\": " << comparison.roi.height
           << "}, \"threshold\": " << comparison.threshold
           << ", \"referenceRgbRange\": "
           << comparison.excludedRgbRange
           << ", \"maximumRgbDelta\": "
           << comparison.maximumRgbDelta
           << ", \"differentPixels\": "
           << comparison.differentPixels
           << ", \"edgeDifferentPixels\": "
           << comparison.edgeDifferentPixels
           << ", \"differenceBounds\": ";
    if (comparison.differenceBounds.has_value())
    {
        const PixelBounds bounds = *comparison.differenceBounds;
        stream << "{\"left\": " << bounds.left
               << ", \"top\": " << bounds.top
               << ", \"right\": " << bounds.right
               << ", \"bottom\": " << bounds.bottom << '}';
    }
    else
    {
        stream << "null";
    }
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
    const std::filesystem::path finalPath = outputDirectory / L"cursor.json";
    const std::filesystem::path temporaryPath =
        outputDirectory / L"cursor.json.tmp";
    std::ofstream stream(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        throw std::runtime_error("Unable to create WGC cursor capture");
    }
    stream << "{\n"
           << "  \"schemaVersion\": 1,\n"
           << "  \"spikeId\": \"SPK-002-CURSOR\",\n"
           << "  \"applicationVersion\": \"" << BAFX_CAPTURE_VERSION
           << "\",\n"
           << "  \"revision\": ";
    writeJsonString(stream, document.options.revision);
    stream << ",\n  \"capturedAtUtc\": ";
    writeJsonString(stream, document.capturedAtUtc);
    stream << ",\n  \"timeoutMs\": "
           << document.options.timeoutMilliseconds << ",\n"
           << "  \"contract\": {\"scope\": "
           << "\"controlled-window-cursor-pixels-only\", "
           << "\"captureTarget\": \"HWND\", "
           << "\"surfaceFormat\": \"DXGI_FORMAT_R16G16B16A16_FLOAT\", "
           << "\"systemBorderAllowed\": true, "
           << "\"cursorShape\": \"custom-monochrome-cross\", "
           << "\"cursorExtent\": " << cursorExtent
           << ", \"cursorHotspot\": " << cursorHotspot
           << ", \"cursorOpaquePixels\": " << cursorOpaquePixels
           << ", \"differenceThreshold\": " << differenceThreshold
           << ", \"minimumDifferentPixels\": " << minimumDifferentPixels
           << ", \"minimumCursorDelta\": " << minimumCursorDelta
           << ", \"maximumBackgroundRange\": "
           << maximumBackgroundRange
           << ", \"maximumControlDelta\": " << maximumControlDelta
           << ", \"maximumStabilityDifferentPixels\": "
           << maximumStabilityDifferentPixels << "},\n"
           << "  \"os\": {\"available\": "
           << (document.osVersion.available ? "true" : "false")
           << ", \"major\": " << document.osVersion.major
           << ", \"minor\": " << document.osVersion.minor
           << ", \"build\": " << document.osVersion.build << "},\n"
           << "  \"device\": {\"driverType\": \"hardware\", "
           << "\"adapter\": ";
    writeJsonString(
        stream,
        wideToUtf8(document.device.adapter.Description));
    stream << ", \"adapterLuid\": {\"low\": "
           << document.device.adapter.AdapterLuid.LowPart
           << ", \"high\": " << document.device.adapter.AdapterLuid.HighPart
           << "}, \"vendorId\": " << document.device.adapter.VendorId
           << ", \"deviceId\": " << document.device.adapter.DeviceId
           << ", \"featureLevel\": "
           << static_cast<unsigned int>(document.device.featureLevel)
           << "},\n"
           << "  \"fixture\": {\"size\": {\"width\": "
           << fixtureSize.width << ", \"height\": " << fixtureSize.height
           << "}, \"screenOrigin\": {\"x\": " << document.fixtureOrigin.x
           << ", \"y\": " << document.fixtureOrigin.y
           << "}, \"cursorScreenPoint\": {\"x\": "
           << document.cursorScreenPoint.x << ", \"y\": "
           << document.cursorScreenPoint.y
           << "}, \"cursorClientPoint\": {\"x\": "
           << document.cursorClientPoint.x << ", \"y\": "
           << document.cursorClientPoint.y << "}},\n"
           << "  \"observations\": {\n"
           << "    \"includedBefore\": ";
    writeMode(
        stream,
        document.includedBefore,
        "included-before.rgba16f",
        "included-before.png",
        document.includedBeforeArtifact);
    stream << ",\n    \"excluded\": ";
    writeMode(
        stream,
        document.excluded,
        "excluded.rgba16f",
        "excluded.png",
        document.excludedArtifact);
    stream << ",\n    \"includedAfter\": ";
    writeMode(
        stream,
        document.includedAfter,
        "included-after.rgba16f",
        "included-after.png",
        document.includedAfterArtifact);
    stream << "\n  },\n"
           << "  \"comparisons\": {\n"
           << "    \"includedBeforeVsExcluded\": ";
    writeComparison(stream, document.includedBeforeComparison);
    stream << ",\n    \"includedAfterVsExcluded\": ";
    writeComparison(stream, document.includedAfterComparison);
    stream << ",\n    \"includedBeforeVsAfter\": ";
    writeComparison(stream, document.includedStability);
    stream << ",\n    \"controlRoi\": {\"left\": " << controlRoi.left
           << ", \"top\": " << controlRoi.top
           << ", \"width\": " << controlRoi.width
           << ", \"height\": " << controlRoi.height << "},\n"
           << "    \"controlMaximumRgbDelta\": "
           << document.controlMaximumRgbDelta << "\n"
           << "  }\n}\n";
    stream.flush();
    if (!stream)
    {
        throw std::runtime_error("Unable to write WGC cursor capture");
    }
    stream.close();
    replaceFile(
        temporaryPath,
        finalPath,
        "MoveFileExW(WGC cursor capture)");
}

void writeFailureDocument(
    const ProbeOptions& options,
    const std::string_view error,
    const bafx::windows::WgcBackgroundResourceLedgerSnapshot&
        includedBeforeLedger,
    const bafx::windows::WgcBackgroundResourceLedgerSnapshot& excludedLedger,
    const bafx::windows::WgcBackgroundResourceLedgerSnapshot&
        includedAfterLedger)
{
    std::filesystem::create_directories(options.outputDirectory);
    const std::filesystem::path finalPath =
        options.outputDirectory / L"failure.json";
    const std::filesystem::path temporaryPath =
        options.outputDirectory / L"failure.json.tmp";
    std::ofstream stream(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        throw std::runtime_error("Unable to create WGC cursor failure evidence");
    }
    stream << "{\n  \"schemaVersion\": 1,\n"
           << "  \"spikeId\": \"SPK-002-CURSOR\",\n"
           << "  \"revision\": ";
    writeJsonString(stream, options.revision);
    stream << ",\n  \"phase\": ";
    writeJsonString(stream, latestPhase);
    stream << ",\n  \"error\": ";
    writeJsonString(stream, error);
    stream << ",\n  \"ledgers\": {\"includedBefore\": ";
    writeLedger(stream, includedBeforeLedger);
    stream << ", \"excluded\": ";
    writeLedger(stream, excludedLedger);
    stream << ", \"includedAfter\": ";
    writeLedger(stream, includedAfterLedger);
    stream << "}\n}\n";
    stream.flush();
    if (!stream)
    {
        throw std::runtime_error("Unable to write WGC cursor failure evidence");
    }
    stream.close();
    replaceFile(
        temporaryPath,
        finalPath,
        "MoveFileExW(WGC cursor failure evidence)");
}

int run(const ProbeOptions& options)
{
    if (options.help)
    {
        std::cout
            << "Usage: ba-click-fx-wgc-cursor-spike "
            << "[--output=DIR] [--revision=GIT] [--timeout-ms=N]\n";
        return 0;
    }

    ProcessWatchdog watchdog(
        options.timeoutMilliseconds + watchdogGraceMilliseconds);
    const auto includedBeforeLedger =
        std::make_shared<bafx::windows::WgcBackgroundResourceLedger>();
    const auto excludedLedger =
        std::make_shared<bafx::windows::WgcBackgroundResourceLedger>();
    const auto includedAfterLedger =
        std::make_shared<bafx::windows::WgcBackgroundResourceLedger>();
    try
    {
        ComApartment apartment{};
        Deadline deadline(std::chrono::milliseconds(options.timeoutMilliseconds));
        enablePerMonitorDpiAwareness();
        QpcClock clock{};

        reportPhase("device-create.begin");
        DeviceResources device = createHardwareDevice();
        reportPhase("device-create.end");
        CursorRestorer cursorRestorer{};
        ControlledCursorWindow window{};
        window.requireCapturable();
        pumpMessages(deadline);
        bafx::windows::throwIfFailed(
            DwmFlush(),
            "DwmFlush(WGC cursor fixture ready)");

        CaptureDocument document{};
        document.options = options;
        document.capturedAtUtc = utcTimestamp();
        document.osVersion = queryOsVersion();
        document.device = device;
        document.fixtureOrigin = window.origin();
        document.cursorScreenPoint = window.cursorScreenPoint();
        document.cursorClientPoint = window.cursorClientPoint();

        reportPhase("included-before-capture.begin");
        document.includedBefore = captureMode(
            device,
            window,
            clock,
            false,
            1U,
            includedBeforeLedger,
            deadline);
        reportPhase("included-before-capture.end");
        reportPhase("excluded-capture.begin");
        document.excluded = captureMode(
            device,
            window,
            clock,
            true,
            2U,
            excludedLedger,
            deadline);
        reportPhase("excluded-capture.end");
        reportPhase("included-after-capture.begin");
        document.includedAfter = captureMode(
            device,
            window,
            clock,
            false,
            3U,
            includedAfterLedger,
            deadline);
        reportPhase("included-after-capture.end");
        cursorRestorer.restore();

        document.includedBeforeComparison = compareObservations(
            document.includedBefore,
            document.excluded,
            document.cursorClientPoint);
        document.includedAfterComparison = compareObservations(
            document.includedAfter,
            document.excluded,
            document.cursorClientPoint);
        document.includedStability = compareObservations(
            document.includedBefore,
            document.includedAfter,
            document.cursorClientPoint);
        document.controlMaximumRgbDelta = std::max({
            regionMaximumRgbDelta(
                document.includedBefore.image,
                document.excluded.image,
                controlRoi),
            regionMaximumRgbDelta(
                document.includedAfter.image,
                document.excluded.image,
                controlRoi),
            regionMaximumRgbDelta(
                document.includedBefore.image,
                document.includedAfter.image,
                controlRoi)});
        requireCursorEvidence(
            document.includedBefore,
            document.excluded,
            document.includedAfter,
            document.includedBeforeComparison,
            document.includedAfterComparison,
            document.includedStability,
            document.controlMaximumRgbDelta,
            document.cursorClientPoint);

        reportPhase("artifact-write.begin");
        document.includedBeforeArtifact = bafx::capture::writeLayerArtifact(
            options.outputDirectory,
            L"included-before",
            document.includedBefore.image);
        document.excludedArtifact = bafx::capture::writeLayerArtifact(
            options.outputDirectory,
            L"excluded",
            document.excluded.image);
        document.includedAfterArtifact = bafx::capture::writeLayerArtifact(
            options.outputDirectory,
            L"included-after",
            document.includedAfter.image);
        writeCaptureDocument(options.outputDirectory, document);
        reportPhase("artifact-write.end");
        std::wcout << L"Wrote SPK-002 cursor capture: "
                   << (options.outputDirectory / L"cursor.json").wstring()
                   << L'\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        try
        {
            writeFailureDocument(
                options,
                error.what(),
                includedBeforeLedger->snapshot(),
                excludedLedger->snapshot(),
                includedAfterLedger->snapshot());
        }
        catch (const std::exception& writeError)
        {
            std::cerr << "Unable to write cursor failure evidence: "
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
        std::cerr << "WGC cursor spike failed: " << error.what() << '\n';
        return 1;
    }
}
