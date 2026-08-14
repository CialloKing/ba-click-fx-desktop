#include "bafx/windows/error.hpp"
#include "bafx/windows/wgc_background_sensor.hpp"

#include "spike_runtime.hpp"

#include <windows.h>

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
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
#include <vector>

namespace
{

using Microsoft::WRL::ComPtr;
using bafx::capture::ComApartment;
using bafx::capture::Deadline;
using bafx::capture::ProcessWatchdog;

constexpr std::uint32_t defaultTimeoutMilliseconds = 12'000U;
constexpr std::uint32_t maximumTimeoutMilliseconds = 120'000U;
constexpr std::uint32_t watchdogGraceMilliseconds = 3'000U;
constexpr std::size_t maximumMessagesPerPump = 256U;
constexpr bafx::windows::WindowSize initialWindowSize{320U, 240U};
constexpr bafx::windows::WindowSize resizedWindowSize{480U, 300U};
constexpr bafx::windows::WindowSize restartWindowSize{360U, 220U};
std::string latestPhase{"startup"};

struct ProbeOptions
{
    std::filesystem::path outputDirectory{
        L"artifacts\\local\\spikes\\spk-002-lifecycle\\current"};
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

struct SampleRecord
{
    std::uint64_t generation{0U};
    std::uint64_t epoch{0U};
    bafx::windows::WindowSize size{};
};

struct ReconfigureRecord
{
    bafx::windows::WindowSize size{};
    std::uint64_t epochBefore{0U};
    std::uint64_t epochAfter{0U};
};

struct EventRecord
{
    std::string kind{};
    std::optional<bafx::windows::WindowSize> size{};
    std::optional<std::uint64_t> generation{};
    std::optional<std::uint64_t> epoch{};
};

struct ResizeCloseCapture
{
    std::vector<EventRecord> events{};
    SampleRecord initialFrame{};
    SampleRecord resizedFrame{};
    std::vector<ReconfigureRecord> reconfigurations{};
    bafx::windows::WgcBackgroundSessionCapabilities capabilities{};
    bafx::windows::WgcBackgroundResourceLedgerSnapshot ledger{};
};

struct RestartStopCapture
{
    std::vector<EventRecord> events{};
    SampleRecord initialFrame{};
    bafx::windows::WgcBackgroundSessionCapabilities capabilities{};
    bafx::windows::WgcBackgroundResourceLedgerSnapshot ledger{};
};

struct CaptureDocument
{
    ProbeOptions options{};
    std::string capturedAtUtc{};
    DeviceResources device{};
    ResizeCloseCapture resizeClose{};
    RestartStopCapture restartStop{};
};

class ControlledCaptureWindow final
{
public:
    ControlledCaptureWindow(
        const bafx::windows::WindowSize size,
        const COLORREF color)
        : color_(color)
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        if (instance == nullptr)
        {
            bafx::windows::throwLastError(
                "GetModuleHandleW(WGC lifecycle fixture)");
        }
        registerWindowClass(instance);
        window_ = CreateWindowExW(
            0U,
            windowClassName,
            L"ba-click-fx SPK-002 lifecycle fixture",
            WS_POPUP | WS_VISIBLE,
            80,
            80,
            checkedDimension(size.width),
            checkedDimension(size.height),
            nullptr,
            nullptr,
            instance,
            this);
        if (window_ == nullptr)
        {
            bafx::windows::throwLastError(
                "CreateWindowExW(WGC lifecycle fixture)");
        }
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        if (!UpdateWindow(window_))
        {
            bafx::windows::throwLastError(
                "UpdateWindow(WGC lifecycle fixture)");
        }
    }

    ~ControlledCaptureWindow()
    {
        if (window_ != nullptr)
        {
            DestroyWindow(window_);
            window_ = nullptr;
        }
    }

    ControlledCaptureWindow(const ControlledCaptureWindow&) = delete;
    ControlledCaptureWindow& operator=(const ControlledCaptureWindow&) = delete;

    [[nodiscard]] HWND handle() const noexcept
    {
        return window_;
    }

    void resize(const bafx::windows::WindowSize size)
    {
        if (window_ == nullptr)
        {
            throw std::runtime_error("Cannot resize a closed lifecycle fixture");
        }
        if (!SetWindowPos(
                window_,
                nullptr,
                0,
                0,
                checkedDimension(size.width),
                checkedDimension(size.height),
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
        {
            bafx::windows::throwLastError(
                "SetWindowPos(WGC lifecycle fixture resize)");
        }
        if (!UpdateWindow(window_))
        {
            bafx::windows::throwLastError(
                "UpdateWindow(WGC lifecycle fixture resize)");
        }
    }

    void close()
    {
        if (window_ == nullptr)
        {
            return;
        }
        const HWND closingWindow = window_;
        if (!DestroyWindow(closingWindow))
        {
            bafx::windows::throwLastError(
                "DestroyWindow(WGC lifecycle fixture)");
        }
        window_ = nullptr;
    }

private:
    static constexpr wchar_t windowClassName[] =
        L"BaClickFxWgcLifecycleFixture";

    [[nodiscard]] static int checkedDimension(const std::uint32_t value)
    {
        if (value == 0U
            || value > static_cast<std::uint32_t>(
                std::numeric_limits<int>::max()))
        {
            throw std::invalid_argument(
                "Lifecycle fixture dimensions must be positive INT values");
        }
        return static_cast<int>(value);
    }

    static void registerWindowClass(const HINSTANCE instance)
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &ControlledCaptureWindow::windowProcedure;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = windowClassName;

        const ATOM atom = RegisterClassExW(&windowClass);
        if (atom == 0U && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            bafx::windows::throwLastError(
                "RegisterClassExW(WGC lifecycle fixture)");
        }
    }

    static LRESULT CALLBACK windowProcedure(
        const HWND window,
        const UINT message,
        const WPARAM wParam,
        const LPARAM lParam)
    {
        auto* self = reinterpret_cast<ControlledCaptureWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<ControlledCaptureWindow*>(create->lpCreateParams);
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
    COLORREF color_{RGB(32, 96, 160)};
};

void reportPhase(const std::string_view phase)
{
    latestPhase.assign(phase);
    // std::endl is intentional: the final phase must survive a watchdog kill.
    std::cerr << "SPK-002 lifecycle phase: " << phase << std::endl;
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
            "SetProcessDpiAwarenessContext(WGC lifecycle spike)");
    }
    // A manifest or launcher may have established an equally strict context
    // before wmain. Windows rejects later changes with ERROR_ACCESS_DENIED.
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
            if (revision.empty()
                || revision.size() > 128U)
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
        throw std::invalid_argument("Unknown WGC lifecycle spike option");
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
        "D3D11CreateDevice(WGC lifecycle spike)");

    ComPtr<IDXGIDevice> dxgiDevice;
    bafx::windows::throwIfFailed(
        result.device.As(&dxgiDevice),
        "ID3D11Device::QueryInterface(IDXGIDevice lifecycle spike)");
    ComPtr<IDXGIAdapter> adapter;
    bafx::windows::throwIfFailed(
        dxgiDevice->GetAdapter(&adapter),
        "IDXGIDevice::GetAdapter(lifecycle spike)");
    bafx::windows::throwIfFailed(
        adapter->GetDesc(&result.adapter),
        "IDXGIAdapter::GetDesc(lifecycle spike)");
    return result;
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
            throw std::runtime_error("WGC lifecycle spike received WM_QUIT");
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
        throw std::runtime_error("WGC lifecycle frame event is unavailable");
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
            "MsgWaitForMultipleObjectsEx(WGC lifecycle spike)");
    }
    if (result == WAIT_OBJECT_0 + 1U)
    {
        pumpMessages(deadline);
    }
}

[[nodiscard]] bool sameSize(
    const bafx::windows::WindowSize left,
    const bafx::windows::WindowSize right) noexcept
{
    return left.width == right.width && left.height == right.height;
}

[[nodiscard]] SampleRecord requireLatestSample(
    bafx::windows::WgcBackgroundSensor& sensor)
{
    const std::optional<bafx::windows::WgcBackgroundSample> sample =
        sensor.latestSample();
    if (!sample.has_value()
        || sample->texture == nullptr
        || !sample->stamp.canonicalLinearScRgb
        || sample->stamp.excludesOwnOverlay
        || sample->stamp.epoch != sensor.expectedEpoch())
    {
        throw std::runtime_error(
            "WGC lifecycle sample violates the sensor contract");
    }
    return SampleRecord{
        sample->generation,
        sample->stamp.epoch,
        sample->size};
}

[[nodiscard]] SampleRecord waitForSampleWithReconfigure(
    bafx::windows::WgcBackgroundSensor& sensor,
    ID3D11DeviceContext* context,
    const std::uint64_t previousGeneration,
    const std::size_t minimumReconfigureCount,
    std::vector<ReconfigureRecord>& reconfigurations,
    std::vector<EventRecord>& events,
    Deadline& deadline)
{
    while (!deadline.expired())
    {
        pumpMessages(deadline);
        const bafx::windows::WgcBackgroundDrainStatus status =
            sensor.drainLatest(context);
        if (status == bafx::windows::WgcBackgroundDrainStatus::Stopped)
        {
            throw std::runtime_error(
                "WGC stopped while waiting for a resized sample");
        }
        if (status
            == bafx::windows::WgcBackgroundDrainStatus::ReconfigureRequired)
        {
            const std::optional<bafx::windows::WindowSize> pendingSize =
                sensor.pendingFramePoolSize();
            if (!pendingSize.has_value())
            {
                throw std::runtime_error(
                    "WGC reconfigure status has no pending size");
            }
            const std::uint64_t epochBefore = sensor.expectedEpoch();
            events.push_back(EventRecord{
                "reconfigure-required",
                *pendingSize,
                std::nullopt,
                epochBefore});
            reportPhase("frame-pool-recreate.begin");
            sensor.recreateFramePool(*pendingSize);
            reportPhase("frame-pool-recreate.end");
            const std::uint64_t epochAfter = sensor.expectedEpoch();
            if (epochAfter == epochBefore)
            {
                throw std::runtime_error(
                    "WGC frame pool recreate did not advance the epoch");
            }
            reconfigurations.push_back(ReconfigureRecord{
                *pendingSize,
                epochBefore,
                epochAfter});
            events.push_back(EventRecord{
                "frame-pool-recreated",
                *pendingSize,
                std::nullopt,
                epochAfter});
            continue;
        }
        if (status == bafx::windows::WgcBackgroundDrainStatus::Updated)
        {
            const SampleRecord sample = requireLatestSample(sensor);
            if (sample.generation > previousGeneration
                && reconfigurations.size() >= minimumReconfigureCount)
            {
                return sample;
            }
        }
        waitForSensorActivity(sensor, deadline);
    }
    throw std::runtime_error(
        "Timed out waiting for a reconfigured WGC lifecycle sample");
}

void waitForStopped(
    bafx::windows::WgcBackgroundSensor& sensor,
    ID3D11DeviceContext* context,
    Deadline& deadline)
{
    while (!deadline.expired())
    {
        pumpMessages(deadline);
        if (sensor.drainLatest(context)
            == bafx::windows::WgcBackgroundDrainStatus::Stopped)
        {
            return;
        }
        waitForSensorActivity(sensor, deadline);
    }
    throw std::runtime_error(
        "Timed out waiting for WGC item.Closed cleanup");
}

[[nodiscard]] bafx::windows::WgcBackgroundSensorOptions sensorOptions(
    const std::shared_ptr<bafx::windows::WgcBackgroundResourceLedger>& ledger)
{
    return bafx::windows::WgcBackgroundSensorOptions{
        1U,
        false,
        true,
        true,
        ledger};
}

[[nodiscard]] ResizeCloseCapture collectResizeClose(
    DeviceResources& device,
    ControlledCaptureWindow& window,
    const std::shared_ptr<bafx::windows::WgcBackgroundResourceLedger>& ledger,
    Deadline& deadline)
{
    ResizeCloseCapture capture{};
    capture.events.push_back(EventRecord{
        "target-created",
        initialWindowSize,
        std::nullopt,
        std::nullopt});
    reportPhase("resize-close.sensor-create.begin");
    {
        bafx::windows::WgcBackgroundSensor sensor(
            device.device.Get(),
            window.handle(),
            sensorOptions(ledger));
        reportPhase("resize-close.sensor-create.end");
        if (!sensor.running())
        {
            throw std::runtime_error("WGC resize-close sensor did not start");
        }
        capture.capabilities = sensor.capabilities();
        capture.events.push_back(EventRecord{
            "sensor-started",
            initialWindowSize,
            std::nullopt,
            sensor.expectedEpoch()});

        capture.initialFrame = waitForSampleWithReconfigure(
            sensor,
            device.context.Get(),
            0U,
            0U,
            capture.reconfigurations,
            capture.events,
            deadline);
        capture.events.push_back(EventRecord{
            "frame-updated",
            capture.initialFrame.size,
            capture.initialFrame.generation,
            capture.initialFrame.epoch});

        reportPhase("target-resize.begin");
        window.resize(resizedWindowSize);
        reportPhase("target-resize.end");
        capture.events.push_back(EventRecord{
            "resize-requested",
            resizedWindowSize,
            std::nullopt,
            std::nullopt});
        const std::size_t resizeReconfigureCount =
            capture.reconfigurations.size() + 1U;
        capture.resizedFrame = waitForSampleWithReconfigure(
            sensor,
            device.context.Get(),
            capture.initialFrame.generation,
            resizeReconfigureCount,
            capture.reconfigurations,
            capture.events,
            deadline);
        if (sameSize(capture.initialFrame.size, capture.resizedFrame.size))
        {
            throw std::runtime_error(
                "WGC resize produced an unchanged ContentSize");
        }
        capture.events.push_back(EventRecord{
            "frame-updated",
            capture.resizedFrame.size,
            capture.resizedFrame.generation,
            capture.resizedFrame.epoch});

        reportPhase("target-close.begin");
        window.close();
        reportPhase("target-close.end");
        capture.events.push_back(EventRecord{
            "target-closed",
            std::nullopt,
            std::nullopt,
            std::nullopt});
        waitForStopped(sensor, device.context.Get(), deadline);
        capture.events.push_back(EventRecord{
            "sensor-stopped",
            std::nullopt,
            std::nullopt,
            sensor.expectedEpoch()});
        sensor.stop();
    }
    capture.events.push_back(EventRecord{
        "sensor-destroyed",
        std::nullopt,
        std::nullopt,
        std::nullopt});
    capture.ledger = ledger->snapshot();
    return capture;
}

[[nodiscard]] RestartStopCapture collectRestartStop(
    DeviceResources& device,
    ControlledCaptureWindow& window,
    const std::shared_ptr<bafx::windows::WgcBackgroundResourceLedger>& ledger,
    Deadline& deadline)
{
    RestartStopCapture capture{};
    capture.events.push_back(EventRecord{
        "target-created",
        restartWindowSize,
        std::nullopt,
        std::nullopt});
    reportPhase("restart.sensor-create.begin");
    {
        bafx::windows::WgcBackgroundSensor sensor(
            device.device.Get(),
            window.handle(),
            sensorOptions(ledger));
        reportPhase("restart.sensor-create.end");
        if (!sensor.running())
        {
            throw std::runtime_error("WGC restart sensor did not start");
        }
        capture.capabilities = sensor.capabilities();
        capture.events.push_back(EventRecord{
            "sensor-started",
            restartWindowSize,
            std::nullopt,
            sensor.expectedEpoch()});
        std::vector<ReconfigureRecord> startupReconfigurations;
        capture.initialFrame = waitForSampleWithReconfigure(
            sensor,
            device.context.Get(),
            0U,
            0U,
            startupReconfigurations,
            capture.events,
            deadline);
        capture.events.push_back(EventRecord{
            "frame-updated",
            capture.initialFrame.size,
            capture.initialFrame.generation,
            capture.initialFrame.epoch});

        reportPhase("restart.sensor-stop.begin");
        capture.events.push_back(EventRecord{
            "sensor-stop-requested",
            std::nullopt,
            std::nullopt,
            sensor.expectedEpoch()});
        sensor.stop();
        reportPhase("restart.sensor-stop.end");
        if (sensor.drainLatest(device.context.Get())
            != bafx::windows::WgcBackgroundDrainStatus::Stopped)
        {
            throw std::runtime_error("Explicit WGC stop was not observable");
        }
        capture.events.push_back(EventRecord{
            "sensor-stopped",
            std::nullopt,
            std::nullopt,
            sensor.expectedEpoch()});
        sensor.stop();
        capture.events.push_back(EventRecord{
            "sensor-stop-repeated",
            std::nullopt,
            std::nullopt,
            sensor.expectedEpoch()});
    }
    capture.events.push_back(EventRecord{
        "sensor-destroyed",
        std::nullopt,
        std::nullopt,
        std::nullopt});
    reportPhase("restart.target-close.begin");
    window.close();
    reportPhase("restart.target-close.end");
    capture.events.push_back(EventRecord{
        "target-closed",
        std::nullopt,
        std::nullopt,
        std::nullopt});
    capture.ledger = ledger->snapshot();
    return capture;
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

void writeSize(
    std::ostream& stream,
    const bafx::windows::WindowSize size)
{
    stream << "{\"width\": " << size.width
           << ", \"height\": " << size.height << '}';
}

void writeSample(std::ostream& stream, const SampleRecord& sample)
{
    stream << "{\"generation\": " << sample.generation
           << ", \"epoch\": " << sample.epoch
           << ", \"size\": ";
    writeSize(stream, sample.size);
    stream << '}';
}

void writeCapabilities(
    std::ostream& stream,
    const bafx::windows::WgcBackgroundSessionCapabilities capabilities)
{
    stream << "{\"borderHidden\": "
           << (capabilities.borderHidden ? "true" : "false")
           << ", \"cursorExcluded\": "
           << (capabilities.cursorExcluded ? "true" : "false") << '}';
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

void writeEvents(
    std::ostream& stream,
    const std::vector<EventRecord>& events,
    const std::string_view indent)
{
    stream << "[\n";
    for (std::size_t index = 0U; index < events.size(); ++index)
    {
        const EventRecord& event = events[index];
        stream << indent << "{\"sequence\": " << index
               << ", \"kind\": ";
        writeJsonString(stream, event.kind);
        if (event.size.has_value())
        {
            stream << ", \"size\": ";
            writeSize(stream, *event.size);
        }
        if (event.generation.has_value())
        {
            stream << ", \"generation\": " << *event.generation;
        }
        if (event.epoch.has_value())
        {
            stream << ", \"epoch\": " << *event.epoch;
        }
        stream << '}';
        if (index + 1U < events.size())
        {
            stream << ',';
        }
        stream << '\n';
    }
    stream << std::string(indent.size() - 2U, ' ') << ']';
}

void writeCaptureDocument(
    const std::filesystem::path& outputDirectory,
    const CaptureDocument& document)
{
    std::filesystem::create_directories(outputDirectory);
    const std::filesystem::path finalPath = outputDirectory / L"lifecycle.json";
    const std::filesystem::path temporaryPath =
        outputDirectory / L"lifecycle.json.tmp";
    std::ofstream stream(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        throw std::runtime_error("Unable to create WGC lifecycle capture");
    }

    stream << "{\n"
           << "  \"schemaVersion\": 1,\n"
           << "  \"spikeId\": \"SPK-002-LIFECYCLE\",\n"
           << "  \"applicationVersion\": \"" << BAFX_CAPTURE_VERSION << "\",\n"
           << "  \"revision\": ";
    writeJsonString(stream, document.options.revision);
    stream << ",\n  \"capturedAtUtc\": ";
    writeJsonString(stream, document.capturedAtUtc);
    stream << ",\n"
           << "  \"timeoutMs\": " << document.options.timeoutMilliseconds << ",\n"
           << "  \"contract\": {"
           << "\"scope\": \"controlled-window-lifecycle-only\", "
           << "\"captureTarget\": \"HWND\", "
           << "\"ownerThread\": \"single\", "
           << "\"callbacks\": \"notification-only\", "
           << "\"surfaceFormat\": \"DXGI_FORMAT_R16G16B16A16_FLOAT\", "
           << "\"systemBorderAllowed\": true, "
           << "\"cursorExcluded\": true},\n"
           << "  \"device\": {\"driverType\": \"hardware\", "
           << "\"adapter\": ";
    writeJsonString(stream, wideToUtf8(document.device.adapter.Description));
    stream << ", \"adapterLuid\": {\"low\": "
           << document.device.adapter.AdapterLuid.LowPart
           << ", \"high\": " << document.device.adapter.AdapterLuid.HighPart
           << "}, \"vendorId\": " << document.device.adapter.VendorId
           << ", \"deviceId\": " << document.device.adapter.DeviceId
           << ", \"featureLevel\": "
           << static_cast<unsigned int>(document.device.featureLevel) << "},\n"
           << "  \"scenarios\": {\n"
           << "    \"resizeClose\": {\n"
           << "      \"events\": ";
    writeEvents(stream, document.resizeClose.events, "        ");
    stream << ",\n      \"initialFrame\": ";
    writeSample(stream, document.resizeClose.initialFrame);
    stream << ",\n      \"requestedResize\": ";
    writeSize(stream, resizedWindowSize);
    stream << ",\n      \"reconfigurations\": [";
    for (std::size_t index = 0U;
         index < document.resizeClose.reconfigurations.size();
         ++index)
    {
        const ReconfigureRecord& record =
            document.resizeClose.reconfigurations[index];
        if (index != 0U)
        {
            stream << ", ";
        }
        stream << "{\"size\": ";
        writeSize(stream, record.size);
        stream << ", \"epochBefore\": " << record.epochBefore
               << ", \"epochAfter\": " << record.epochAfter << '}';
    }
    stream << "],\n      \"resizedFrame\": ";
    writeSample(stream, document.resizeClose.resizedFrame);
    stream << ",\n      \"capabilities\": ";
    writeCapabilities(stream, document.resizeClose.capabilities);
    stream << ",\n      \"ledger\": ";
    writeLedger(stream, document.resizeClose.ledger);
    stream << "\n    },\n"
           << "    \"restartStop\": {\n"
           << "      \"events\": ";
    writeEvents(stream, document.restartStop.events, "        ");
    stream << ",\n      \"initialFrame\": ";
    writeSample(stream, document.restartStop.initialFrame);
    stream << ",\n      \"capabilities\": ";
    writeCapabilities(stream, document.restartStop.capabilities);
    stream << ",\n      \"ledger\": ";
    writeLedger(stream, document.restartStop.ledger);
    stream << "\n    }\n"
           << "  }\n"
           << "}\n";
    stream.flush();
    if (!stream)
    {
        throw std::runtime_error("Unable to write WGC lifecycle capture");
    }
    stream.close();

    if (!MoveFileExW(
            temporaryPath.c_str(),
            finalPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        bafx::windows::throwLastError(
            "MoveFileExW(WGC lifecycle capture)");
    }
}

void requireBalancedLedger(
    const std::string_view scenario,
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
        throw std::runtime_error(
            std::string(scenario) + " WGC resource ledger is not balanced");
    }
}

void writeFailureDocument(
    const ProbeOptions& options,
    const std::string_view error,
    const bafx::windows::WgcBackgroundResourceLedgerSnapshot& resizeLedger,
    const bafx::windows::WgcBackgroundResourceLedgerSnapshot& restartLedger)
{
    std::filesystem::create_directories(options.outputDirectory);
    const std::filesystem::path finalPath =
        options.outputDirectory / L"failure.json";
    const std::filesystem::path temporaryPath =
        options.outputDirectory / L"failure.json.tmp";
    std::ofstream stream(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        throw std::runtime_error("Unable to create WGC lifecycle failure evidence");
    }
    stream << "{\n"
           << "  \"schemaVersion\": 1,\n"
           << "  \"spikeId\": \"SPK-002-LIFECYCLE\",\n"
           << "  \"revision\": ";
    writeJsonString(stream, options.revision);
    stream << ",\n  \"phase\": ";
    writeJsonString(stream, latestPhase);
    stream << ",\n  \"error\": ";
    writeJsonString(stream, error);
    stream << ",\n  \"ledgers\": {\"resizeClose\": ";
    writeLedger(stream, resizeLedger);
    stream << ", \"restartStop\": ";
    writeLedger(stream, restartLedger);
    stream << "}\n}\n";
    stream.flush();
    if (!stream)
    {
        throw std::runtime_error("Unable to write WGC lifecycle failure evidence");
    }
    stream.close();
    if (!MoveFileExW(
            temporaryPath.c_str(),
            finalPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        bafx::windows::throwLastError(
            "MoveFileExW(WGC lifecycle failure evidence)");
    }
}

int run(const ProbeOptions& options)
{
    if (options.help)
    {
        std::cout
            << "Usage: ba-click-fx-wgc-lifecycle-spike "
            << "[--output=DIR] [--revision=GIT] [--timeout-ms=N]\n";
        return 0;
    }

    ProcessWatchdog watchdog(
        options.timeoutMilliseconds + watchdogGraceMilliseconds);
    const auto resizeLedger =
        std::make_shared<bafx::windows::WgcBackgroundResourceLedger>();
    const auto restartLedger =
        std::make_shared<bafx::windows::WgcBackgroundResourceLedger>();
    try
    {
        ComApartment apartment{};
        Deadline deadline(std::chrono::milliseconds(options.timeoutMilliseconds));
        enablePerMonitorDpiAwareness();

        reportPhase("device-create.begin");
        DeviceResources device = createHardwareDevice();
        reportPhase("device-create.end");

        CaptureDocument document{};
        document.options = options;
        document.capturedAtUtc = utcTimestamp();
        document.device = device;
        // Create both targets up front so Windows cannot recycle the HWND closed
        // by the first scenario while WGC is still retiring its capture item.
        ControlledCaptureWindow restartWindow(
            restartWindowSize,
            RGB(160, 72, 48));
        ControlledCaptureWindow resizeWindow(
            initialWindowSize,
            RGB(32, 96, 160));
        reportPhase("targets-dwm-ready.begin");
        pumpMessages(deadline);
        bafx::windows::throwIfFailed(
            DwmFlush(),
            "DwmFlush(WGC lifecycle targets)");
        pumpMessages(deadline);
        reportPhase("targets-dwm-ready.end");
        document.resizeClose = collectResizeClose(
            device,
            resizeWindow,
            resizeLedger,
            deadline);
        document.restartStop = collectRestartStop(
            device,
            restartWindow,
            restartLedger,
            deadline);

        reportPhase("artifact-write.begin");
        writeCaptureDocument(options.outputDirectory, document);
        reportPhase("artifact-write.end");
        requireBalancedLedger(
            "resize-close",
            document.resizeClose.ledger);
        requireBalancedLedger(
            "restart-stop",
            document.restartStop.ledger);
        std::wcout << L"Wrote SPK-002 lifecycle capture: "
                   << (options.outputDirectory / L"lifecycle.json").wstring()
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
                resizeLedger->snapshot(),
                restartLedger->snapshot());
        }
        catch (const std::exception& writeError)
        {
            std::cerr << "Unable to write lifecycle failure evidence: "
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
        std::cerr << "WGC lifecycle spike failed: " << error.what() << '\n';
        return 1;
    }
}
