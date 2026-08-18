#ifndef ENABLE_WINRT_EXPERIMENTAL_TYPES
#define ENABLE_WINRT_EXPERIMENTAL_TYPES
#endif

#include "bafx/windows/wgc_background_sensor.hpp"

#include "bafx/windows/detail/wgc_frame_notification.hpp"
#include "bafx/windows/detail/wgc_experimental_abi.hpp"
#include "bafx/windows/detail/wgc_stop_sequence.hpp"
#include "bafx/windows/error.hpp"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.UI.h>
#include <winrt/base.h>

#include <chrono>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace bafx::windows
{
namespace
{

using Microsoft::WRL::ComPtr;
using bafx::windows::detail::Direct3D11CaptureFrame3Abi;
using bafx::windows::detail::DisplayGraphicsCaptureSessionAbi;
using bafx::windows::detail::GraphicsCaptureSession7Abi;
using bafx::windows::detail::WindowIdAbi;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::SizeInt32;

constexpr int captureBufferCount = 2;
static_assert(captureBufferCount > 0);
constexpr std::uint32_t maximumFramesPerDrain =
    static_cast<std::uint32_t>(captureBufferCount);
constexpr DirectXPixelFormat capturePixelFormat =
    DirectXPixelFormat::R16G16B16A16Float;
constexpr DXGI_FORMAT captureDxgiFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

using GetWindowIdFromWindowFunction =
    HRESULT(WINAPI*)(HWND, WindowIdAbi*);

class WindowIdInteropResolver final
{
public:
    WindowIdInteropResolver() noexcept
        : module_(LoadLibraryW(L"ext-ms-win-windowing-external-l1-1-0.dll"))
    {
        if (module_ != nullptr)
        {
            function_ = reinterpret_cast<GetWindowIdFromWindowFunction>(
                GetProcAddress(module_, "GetWindowIdFromWindow"));
            if (function_ == nullptr)
            {
                FreeLibrary(module_);
                module_ = nullptr;
            }
        }
    }

    ~WindowIdInteropResolver()
    {
        if (module_ != nullptr)
        {
            FreeLibrary(module_);
        }
    }

    WindowIdInteropResolver(const WindowIdInteropResolver&) = delete;
    WindowIdInteropResolver& operator=(const WindowIdInteropResolver&) = delete;

    [[nodiscard]] HRESULT getWindowId(
        const HWND window,
        WindowIdAbi* const id) const noexcept
    {
        if (function_ == nullptr)
        {
            return E_NOINTERFACE;
        }
        return function_(window, id);
    }

private:
    HMODULE module_{nullptr};
    GetWindowIdFromWindowFunction function_{nullptr};
};

// Cursor and border controls were added after the original Windows 10 SDK.
// Keep their stable WinRT ABI local so an older build machine does not remove
// capabilities that a newer target system can provide at runtime.
MIDL_INTERFACE("2C39AE40-7D2E-5044-804E-8B6799D4CF9E")
GraphicsCaptureSession2Abi : public IInspectable
{
public:
    virtual HRESULT STDMETHODCALLTYPE get_IsCursorCaptureEnabled(
        bool* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsCursorCaptureEnabled(
        bool value) = 0;
};

MIDL_INTERFACE("F2CDD966-22AE-5EA1-9596-3A289344C3BE")
GraphicsCaptureSession3Abi : public IInspectable
{
public:
    virtual HRESULT STDMETHODCALLTYPE get_IsBorderRequired(
        bool* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsBorderRequired(
        bool value) = 0;
};

// Windows 11 24H2 added IGraphicsCaptureSession5 after the original Windows
// 10 SDK. Keep its documented ABI local so every build emits the same runtime
// capability and only the target OS decides whether QueryInterface succeeds.
struct GraphicsCaptureTimeSpanAbi final
{
    std::int64_t duration{0};
};

static_assert(sizeof(GraphicsCaptureTimeSpanAbi) == sizeof(std::int64_t));

MIDL_INTERFACE("67C0EA62-1F85-5061-925A-239BE0AC09CB")
GraphicsCaptureSession5Abi : public IInspectable
{
public:
    virtual HRESULT STDMETHODCALLTYPE get_MinUpdateInterval(
        GraphicsCaptureTimeSpanAbi* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_MinUpdateInterval(
        GraphicsCaptureTimeSpanAbi value) = 0;
};

struct OwnedBackgroundTexture
{
    ComPtr<ID3D11Texture2D> texture{};
    ComPtr<ID3D11ShaderResourceView> shaderResource{};
    WindowSize size{};
};

[[nodiscard]] WindowSize checkedSize(const SizeInt32 size)
{
    if (size.Width <= 0 || size.Height <= 0)
    {
        throw std::runtime_error("WGC reported a non-positive content size");
    }
    return WindowSize{
        static_cast<std::uint32_t>(size.Width),
        static_cast<std::uint32_t>(size.Height)};
}

[[nodiscard]] OwnedBackgroundTexture createOwnedTexture(
    ID3D11Device* device,
    const WindowSize size)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = size.width;
    description.Height = size.height;
    description.MipLevels = 1U;
    description.ArraySize = 1U;
    description.Format = captureDxgiFormat;
    description.SampleDesc = DXGI_SAMPLE_DESC{1U, 0U};
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    OwnedBackgroundTexture result{};
    result.size = size;
    throwIfFailed(
        device->CreateTexture2D(&description, nullptr, &result.texture),
        "ID3D11Device::CreateTexture2D(WGC owned background)");
    throwIfFailed(
        device->CreateShaderResourceView(
            result.texture.Get(),
            nullptr,
            &result.shaderResource),
        "ID3D11Device::CreateShaderResourceView(WGC owned background)");
    return result;
}

[[nodiscard]] IDirect3DDevice createWinrtDevice(ID3D11Device* device)
{
    ComPtr<IDXGIDevice> dxgiDevice;
    throwIfFailed(
        device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)),
        "ID3D11Device::QueryInterface(IDXGIDevice for WGC)");

    winrt::com_ptr<IInspectable> inspectable;
    throwIfFailed(
        CreateDirect3D11DeviceFromDXGIDevice(
            dxgiDevice.Get(),
            inspectable.put()),
        "CreateDirect3D11DeviceFromDXGIDevice");
    return inspectable.as<IDirect3DDevice>();
}

[[nodiscard]] GraphicsCaptureItem createMonitorItem(const HMONITOR monitor)
{
    const auto factory = winrt::get_activation_factory<GraphicsCaptureItem>();
    const auto interop = factory.as<IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    throwIfFailed(
        interop->CreateForMonitor(
            monitor,
            winrt::guid_of<winrt::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
            winrt::put_abi(item)),
        "IGraphicsCaptureItemInterop::CreateForMonitor");
    return item;
}

[[nodiscard]] GraphicsCaptureItem createWindowItem(const HWND window)
{
    const auto factory = winrt::get_activation_factory<GraphicsCaptureItem>();
    const auto interop = factory.as<IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    throwIfFailed(
        interop->CreateForWindow(
            window,
            winrt::guid_of<GraphicsCaptureItem>(),
            winrt::put_abi(item)),
        "IGraphicsCaptureItemInterop::CreateForWindow");
    return item;
}

[[nodiscard]] ComPtr<ID3D11Texture2D> textureFromFrame(
    const Direct3D11CaptureFrame& frame)
{
    const auto access = frame.Surface().as<
        ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> texture;
    throwIfFailed(
        access->GetInterface(IID_PPV_ARGS(&texture)),
        "IDirect3DDxgiInterfaceAccess::GetInterface(ID3D11Texture2D)");
    return texture;
}

[[nodiscard]] bafx::core::MonotonicTime captureTime(
    const Direct3D11CaptureFrame& frame)
{
    return std::chrono::duration_cast<bafx::core::MonotonicTime>(
        frame.SystemRelativeTime());
}

[[nodiscard]] std::uint64_t nextGeneration(
    const std::uint64_t generation) noexcept
{
    if (generation == std::numeric_limits<std::uint64_t>::max())
    {
        return 1U;
    }
    return generation + 1U;
}

}

std::string_view wgcSessionWindowExclusionStatusName(
    const WgcSessionWindowExclusionStatus status) noexcept
{
    switch (status)
    {
    case WgcSessionWindowExclusionStatus::NotRequested:
        return "not-requested";
    case WgcSessionWindowExclusionStatus::Applied:
        return "applied";
    case WgcSessionWindowExclusionStatus::InterfaceUnavailable:
        return "interface-unavailable";
    case WgcSessionWindowExclusionStatus::Rejected:
        return "rejected";
    }
    return "rejected";
}

namespace detail
{

bool sessionWindowExclusionFrameMatches(
    const bool required,
    const HRESULT frameQueryResult,
    const HRESULT frameIterationResult,
    const std::uint64_t expectedIteration,
    const std::uint64_t frameIteration) noexcept
{
    return !required
        || (SUCCEEDED(frameQueryResult)
            && SUCCEEDED(frameIterationResult)
            && frameIteration == expectedIteration);
}

bool observeSessionWindowExclusionFrame(
    WgcSessionWindowExclusionState& state,
    const HRESULT frameQueryResult,
    const HRESULT frameIterationResult,
    const std::uint64_t frameIteration) noexcept
{
    state.frameQueryResult = frameQueryResult;
    state.frameIterationResult = frameIterationResult;
    state.lastFrameIteration = frameIteration;
    const bool matched = sessionWindowExclusionFrameMatches(
        true,
        frameQueryResult,
        frameIterationResult,
        state.setIteration,
        frameIteration);
    if (!matched)
    {
        // Keep lifetime evidence without turning separated stale frames into
        // the consecutive instability threshold used for runtime fallback.
        if (state.rejectedFrameCount
            < (std::numeric_limits<std::uint64_t>::max)())
        {
            ++state.rejectedFrameCount;
        }
        if (state.consecutiveRejectedFrameCount
            < (std::numeric_limits<std::uint64_t>::max)())
        {
            ++state.consecutiveRejectedFrameCount;
        }
        return false;
    }

    state.consecutiveRejectedFrameCount = 0U;
    state.frameIterationConfirmed = true;
    return true;
}

}

enum class WgcBackgroundSensor::ResourceLedgerEvent : std::uint8_t
{
    FrameAcquired,
    FrameClosed,
    FramePoolCreated,
    FramePoolClosed,
    FramePoolRecreated,
    SessionCreated,
    SessionClosed,
    FrameArrivedRegistered,
    FrameArrivedUnregistered,
    ItemClosedRegistered,
    ItemClosedUnregistered,
    Failure
};

bool WgcBackgroundResourceLedgerSnapshot::allReleased() const noexcept
{
    return liveFrames == 0U
        && liveFramePools == 0U
        && liveSessions == 0U
        && liveFrameArrivedRegistrations == 0U
        && liveItemClosedRegistrations == 0U;
}

std::string_view wgcBackgroundStopStageName(
    const WgcBackgroundStopStage stage) noexcept
{
    switch (stage)
    {
    case WgcBackgroundStopStage::Stop:
        return "stop";
    case WgcBackgroundStopStage::FrameArrivedUnregister:
        return "frame-arrived-unregister";
    case WgcBackgroundStopStage::ItemClosedUnregister:
        return "item-closed-unregister";
    case WgcBackgroundStopStage::SessionClose:
        return "session-close";
    case WgcBackgroundStopStage::FramePoolClose:
        return "frame-pool-close";
    }
    return "unknown";
}

std::string_view wgcBackgroundStopStageStateName(
    const WgcBackgroundStopStageState state) noexcept
{
    switch (state)
    {
    case WgcBackgroundStopStageState::Begin:
        return "begin";
    case WgcBackgroundStopStageState::Succeeded:
        return "succeeded";
    case WgcBackgroundStopStageState::Failed:
        return "failed";
    }
    return "unknown";
}

std::string wgcBackgroundResourceLedgerDiagnostic(
    const WgcBackgroundResourceLedgerSnapshot& snapshot)
{
    std::ostringstream stream;
    stream << "WGC.ResourceLedger.FramesAcquired=" << snapshot.framesAcquired
           << ";FramesClosed=" << snapshot.framesClosed
           << ";FramePoolsCreated=" << snapshot.framePoolsCreated
           << ";FramePoolsClosed=" << snapshot.framePoolsClosed
           << ";FramePoolsRecreated=" << snapshot.framePoolsRecreated
           << ";SessionsCreated=" << snapshot.sessionsCreated
           << ";SessionsClosed=" << snapshot.sessionsClosed
           << ";FrameArrivedRegistrations="
           << snapshot.frameArrivedRegistrations
           << ";FrameArrivedUnregistrations="
           << snapshot.frameArrivedUnregistrations
           << ";ItemClosedRegistrations=" << snapshot.itemClosedRegistrations
           << ";ItemClosedUnregistrations="
           << snapshot.itemClosedUnregistrations
           << ";LiveFrames=" << snapshot.liveFrames
           << ";LiveFramePools=" << snapshot.liveFramePools
           << ";LiveSessions=" << snapshot.liveSessions
           << ";LiveFrameArrivedRegistrations="
           << snapshot.liveFrameArrivedRegistrations
           << ";LiveItemClosedRegistrations="
           << snapshot.liveItemClosedRegistrations
           << ";Failures=" << snapshot.failures
           << ";AllReleased=" << (snapshot.allReleased() ? "true" : "false");
    return stream.str();
}

WgcBackgroundResourceLedgerSnapshot
WgcBackgroundResourceLedger::snapshot() const noexcept
{
    return WgcBackgroundResourceLedgerSnapshot{
        framesAcquired_.load(std::memory_order_relaxed),
        framesClosed_.load(std::memory_order_relaxed),
        framePoolsCreated_.load(std::memory_order_relaxed),
        framePoolsClosed_.load(std::memory_order_relaxed),
        framePoolsRecreated_.load(std::memory_order_relaxed),
        sessionsCreated_.load(std::memory_order_relaxed),
        sessionsClosed_.load(std::memory_order_relaxed),
        frameArrivedRegistrations_.load(std::memory_order_relaxed),
        frameArrivedUnregistrations_.load(std::memory_order_relaxed),
        itemClosedRegistrations_.load(std::memory_order_relaxed),
        itemClosedUnregistrations_.load(std::memory_order_relaxed),
        liveFrames_.load(std::memory_order_relaxed),
        liveFramePools_.load(std::memory_order_relaxed),
        liveSessions_.load(std::memory_order_relaxed),
        liveFrameArrivedRegistrations_.load(std::memory_order_relaxed),
        liveItemClosedRegistrations_.load(std::memory_order_relaxed),
        failures_.load(std::memory_order_relaxed)};
}

std::string wgcBackgroundStopDiagnostic(
    const WgcBackgroundStopDiagnostics& diagnostics)
{
    const auto microseconds = [](const std::chrono::nanoseconds duration)
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            duration).count();
    };
    std::ostringstream stream;
    stream << "WGC.Stop.SensorPresent="
           << (diagnostics.sensorPresent ? "true" : "false")
           << ";WGC.Stop.FrameArrivedUnregisterFailed="
           << (diagnostics.frameArrivedUnregisterFailed ? "true" : "false")
           << ";WGC.Stop.ItemClosedUnregisterFailed="
           << (diagnostics.itemClosedUnregisterFailed ? "true" : "false")
           << ";WGC.Stop.SessionCloseFailed="
           << (diagnostics.sessionCloseFailed ? "true" : "false")
           << ";WGC.Stop.FramePoolCloseFailed="
           << (diagnostics.framePoolCloseFailed ? "true" : "false")
           << ";WGC.Stop.OwnerThreadMismatch="
           << (diagnostics.ownerThreadMismatch ? "true" : "false")
           << ";WGC.Stop.Completed="
           << (diagnostics.completed ? "true" : "false")
           << ";WGC.Stop.OverallSucceeded="
           << (diagnostics.overallSucceeded ? "true" : "false")
           << ";WGC.Stop.DeferredReport="
           << (diagnostics.deferredReport ? "true" : "false")
           << ";WGC.Stop.FrameArrivedUnregisterUs="
           << microseconds(diagnostics.frameArrivedUnregister)
           << ";WGC.Stop.ItemClosedUnregisterUs="
           << microseconds(diagnostics.itemClosedUnregister)
           << ";WGC.Stop.SessionCloseUs="
           << microseconds(diagnostics.sessionClose)
           << ";WGC.Stop.FramePoolCloseUs="
           << microseconds(diagnostics.framePoolClose)
           << ";WGC.Stop.TotalUs="
           << microseconds(diagnostics.total);
    return stream.str();
}

void detail::WgcBackgroundStopMailbox::record(
    WgcBackgroundStopDiagnostics diagnostics) noexcept
{
    diagnostics.deferredReport = false;
    diagnostics_ = diagnostics;
    sensorStopPending_ = diagnostics.sensorPresent;
    if (diagnostics.sensorPresent
        && (!diagnostics.completed || !diagnostics.overallSucceeded))
    {
        // A failed WinRT close leaves ownership uncertain. Reading the
        // diagnostic must never turn that process-lifetime fact back into an
        // eligible WGC restart.
        restartBlocked_ = true;
    }
}

void detail::WgcBackgroundStopMailbox::recordNoSensor() noexcept
{
    if (sensorStopPending_)
    {
        // The owner has not logged the preceding render-side stop yet. Mark
        // the handoff instead of replacing its phase timings with zeroes.
        diagnostics_.deferredReport = true;
        return;
    }

    diagnostics_ = WgcBackgroundStopDiagnostics{};
    diagnostics_.completed = true;
    diagnostics_.overallSucceeded = true;
}

WgcBackgroundStopDiagnostics detail::WgcBackgroundStopMailbox::take() noexcept
{
    const WgcBackgroundStopDiagnostics result = diagnostics_;
    diagnostics_ = WgcBackgroundStopDiagnostics{};
    sensorStopPending_ = false;
    return result;
}

bool detail::WgcBackgroundStopMailbox::restartAllowed() const noexcept
{
    return !restartBlocked_;
}

WgcBackgroundStopResultObserver
detail::WgcBackgroundStopMailbox::resultObserver() noexcept
{
    return WgcBackgroundStopResultObserver{this, &observeResult};
}

void detail::WgcBackgroundStopMailbox::observeResult(
    const void* const context,
    const WgcBackgroundStopDiagnostics& diagnostics) noexcept
{
    if (context == nullptr)
    {
        return;
    }
    auto& mailbox = *static_cast<detail::WgcBackgroundStopMailbox*>(
        const_cast<void*>(context));
    mailbox.record(diagnostics);
}

void WgcBackgroundSensor::recordResourceLedgerEvent(
    const std::shared_ptr<WgcBackgroundResourceLedger>& ledger,
    const ResourceLedgerEvent event) noexcept
{
    if (ledger == nullptr)
    {
        return;
    }

    const auto decrementLive = [&ledger](
        std::atomic<std::uint64_t>& live) noexcept
    {
        std::uint64_t current = live.load(std::memory_order_relaxed);
        while (current != 0U)
        {
            if (live.compare_exchange_weak(
                    current,
                    current - 1U,
                    std::memory_order_relaxed))
            {
                return;
            }
        }
        // An unmatched release is itself lifecycle evidence, so retain it as
        // a failure instead of wrapping the unsigned live counter.
        ledger->failures_.fetch_add(1U, std::memory_order_relaxed);
    };

    switch (event)
    {
    case ResourceLedgerEvent::FrameAcquired:
        ledger->framesAcquired_.fetch_add(1U, std::memory_order_relaxed);
        ledger->liveFrames_.fetch_add(1U, std::memory_order_relaxed);
        break;
    case ResourceLedgerEvent::FrameClosed:
        ledger->framesClosed_.fetch_add(1U, std::memory_order_relaxed);
        decrementLive(ledger->liveFrames_);
        break;
    case ResourceLedgerEvent::FramePoolCreated:
        ledger->framePoolsCreated_.fetch_add(1U, std::memory_order_relaxed);
        ledger->liveFramePools_.fetch_add(1U, std::memory_order_relaxed);
        break;
    case ResourceLedgerEvent::FramePoolClosed:
        ledger->framePoolsClosed_.fetch_add(1U, std::memory_order_relaxed);
        decrementLive(ledger->liveFramePools_);
        break;
    case ResourceLedgerEvent::FramePoolRecreated:
        ledger->framePoolsRecreated_.fetch_add(1U, std::memory_order_relaxed);
        break;
    case ResourceLedgerEvent::SessionCreated:
        ledger->sessionsCreated_.fetch_add(1U, std::memory_order_relaxed);
        ledger->liveSessions_.fetch_add(1U, std::memory_order_relaxed);
        break;
    case ResourceLedgerEvent::SessionClosed:
        ledger->sessionsClosed_.fetch_add(1U, std::memory_order_relaxed);
        decrementLive(ledger->liveSessions_);
        break;
    case ResourceLedgerEvent::FrameArrivedRegistered:
        ledger->frameArrivedRegistrations_.fetch_add(1U, std::memory_order_relaxed);
        ledger->liveFrameArrivedRegistrations_.fetch_add(
            1U,
            std::memory_order_relaxed);
        break;
    case ResourceLedgerEvent::FrameArrivedUnregistered:
        ledger->frameArrivedUnregistrations_.fetch_add(
            1U,
            std::memory_order_relaxed);
        decrementLive(ledger->liveFrameArrivedRegistrations_);
        break;
    case ResourceLedgerEvent::ItemClosedRegistered:
        ledger->itemClosedRegistrations_.fetch_add(1U, std::memory_order_relaxed);
        ledger->liveItemClosedRegistrations_.fetch_add(
            1U,
            std::memory_order_relaxed);
        break;
    case ResourceLedgerEvent::ItemClosedUnregistered:
        ledger->itemClosedUnregistrations_.fetch_add(
            1U,
            std::memory_order_relaxed);
        decrementLive(ledger->liveItemClosedRegistrations_);
        break;
    case ResourceLedgerEvent::Failure:
        ledger->failures_.fetch_add(1U, std::memory_order_relaxed);
        break;
    }
}

struct WgcBackgroundSensor::Implementation
{
    Implementation(
        ID3D11Device* sourceDevice,
        GraphicsCaptureItem sourceItem,
        const WgcBackgroundSensorOptions sourceOptions)
        : device(sourceDevice)
        , direct3dDevice(createWinrtDevice(sourceDevice))
        , item(std::move(sourceItem))
        , options(sourceOptions)
        , ownerThreadId(GetCurrentThreadId())
        , ledger(sourceOptions.resourceLedger)
        , sessionWindowExclusionState(
              sourceOptions.sessionWindowExclusionState != nullptr
                  ? sourceOptions.sessionWindowExclusionState
                  : std::make_shared<WgcSessionWindowExclusionState>())
        , notification(std::make_shared<detail::WgcFrameNotification>())
    {
        if (sourceDevice == nullptr)
        {
            throw std::invalid_argument("WGC background sensor requires a D3D11 device");
        }
        if (!item)
        {
            throw std::invalid_argument("WGC background sensor requires a capture item");
        }
        if (!GraphicsCaptureSession::IsSupported())
        {
            throw HResultError(
                HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
                "GraphicsCaptureSession::IsSupported");
        }

        try
        {
            const SizeInt32 initialContentSize = item.Size();
            poolSize = checkedSize(initialContentSize);
            ownedTexture = createOwnedTexture(device.Get(), poolSize);
            framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
                direct3dDevice,
                capturePixelFormat,
                captureBufferCount,
                initialContentSize);
            recordResourceLedgerEvent(
                ledger,
                ResourceLedgerEvent::FramePoolCreated);

            const std::shared_ptr<detail::WgcFrameNotification> callbackState =
                notification;
            frameArrivedToken = framePool.FrameArrived(
                [callbackState](const auto&, const auto&) noexcept
                {
                    callbackState->notifyFrame();
                });
            frameArrivedRegistered = true;
            recordResourceLedgerEvent(
                ledger,
                ResourceLedgerEvent::FrameArrivedRegistered);

            itemClosedToken = item.Closed(
                [callbackState](const auto&, const auto&) noexcept
                {
                    callbackState->notifyItemClosed();
                });
            itemClosedRegistered = true;
            recordResourceLedgerEvent(
                ledger,
                ResourceLedgerEvent::ItemClosedRegistered);

            session = framePool.CreateCaptureSession(item);
            recordResourceLedgerEvent(
                ledger,
                ResourceLedgerEvent::SessionCreated);
            auto* const sessionUnknown = reinterpret_cast<IUnknown*>(
                winrt::get_abi(session));
            ComPtr<GraphicsCaptureSession3Abi> borderSession;
            const HRESULT borderQueryResult = sessionUnknown->QueryInterface(
                IID_PPV_ARGS(&borderSession));
            if (!options.allowSystemBorder)
            {
                if (FAILED(borderQueryResult))
                {
                    throw HResultError(
                        borderQueryResult,
                        "borderless WGC session is unavailable");
                }
                const HRESULT borderWriteResult =
                    borderSession->put_IsBorderRequired(false);
                if (FAILED(borderWriteResult))
                {
                    throw HResultError(
                        borderWriteResult,
                        "IGraphicsCaptureSession3::IsBorderRequired(false)");
                }
                bool borderRequired = true;
                const HRESULT borderReadResult =
                    borderSession->get_IsBorderRequired(&borderRequired);
                if (FAILED(borderReadResult))
                {
                    throw HResultError(
                        borderReadResult,
                        "IGraphicsCaptureSession3::IsBorderRequired(readback)");
                }
                capabilities.borderHidden = !borderRequired;
                if (!capabilities.borderHidden)
                {
                    throw HResultError(
                        E_ACCESSDENIED,
                        "Windows kept the WGC system capture border enabled");
                }
            }
            else if (SUCCEEDED(borderQueryResult))
            {
                bool borderRequired = true;
                const HRESULT borderReadResult =
                    borderSession->get_IsBorderRequired(&borderRequired);
                if (SUCCEEDED(borderReadResult))
                {
                    capabilities.borderHidden = !borderRequired;
                }
                // The visible border is explicitly allowed, so a failed
                // optional readback does not block capture.
            }

            if (options.cursorExcluded
                || options.cursorCaptureEnabledOverride.has_value())
            {
                ComPtr<GraphicsCaptureSession2Abi> cursorSession;
                const HRESULT cursorQueryResult = sessionUnknown->QueryInterface(
                    IID_PPV_ARGS(&cursorSession));
                if (FAILED(cursorQueryResult))
                {
                    throw HResultError(
                        cursorQueryResult,
                        "GraphicsCaptureSession::QueryInterface(IGraphicsCaptureSession2)");
                }
                const bool requestedCursorCaptureEnabled =
                    options.cursorCaptureEnabledOverride.value_or(false);
                if (options.cursorCaptureEnabledOverride.has_value()
                    && options.cursorExcluded
                        == requestedCursorCaptureEnabled)
                {
                    throw std::invalid_argument(
                        "WGC cursor options request contradictory states");
                }
                // A captured cursor would be mistaken for the desktop beneath
                // the FX. Validation probes also use this path to explicitly
                // enable inclusion instead of trusting the session default.
                const HRESULT cursorWriteResult =
                    cursorSession->put_IsCursorCaptureEnabled(
                        requestedCursorCaptureEnabled);
                if (FAILED(cursorWriteResult))
                {
                    throw HResultError(
                        cursorWriteResult,
                        "IGraphicsCaptureSession2::IsCursorCaptureEnabled(write)");
                }
                bool cursorCaptureEnabled = true;
                const HRESULT cursorReadResult =
                    cursorSession->get_IsCursorCaptureEnabled(
                        &cursorCaptureEnabled);
                if (FAILED(cursorReadResult))
                {
                    throw HResultError(
                        cursorReadResult,
                        "IGraphicsCaptureSession2::IsCursorCaptureEnabled(readback)");
                }
                capabilities.cursorCaptureEnabled = cursorCaptureEnabled;
                capabilities.cursorExcluded = !cursorCaptureEnabled;
                capabilities.cursorControlConfirmed =
                    cursorCaptureEnabled == requestedCursorCaptureEnabled;
                if (!capabilities.cursorControlConfirmed)
                {
                    throw HResultError(
                        E_FAIL,
                        "IGraphicsCaptureSession2 cursor readback mismatched the request");
                }
            }
            if (options.minimumUpdateInterval.has_value()
                && *options.minimumUpdateInterval
                    > bafx::core::MonotonicTime::zero())
            {
                capabilities.producerCadence =
                    configureMinimumUpdateInterval(
                        *options.minimumUpdateInterval);
            }
            // Exclusion must be the final session configuration so the
            // returned iteration also covers border, cursor, and cadence
            // writes performed above.
            configureSessionWindowExclusion(sessionUnknown);
            try
            {
                session.StartCapture();
            }
            catch (const winrt::hresult_error& error)
            {
                throw HResultError(error.code(), "GraphicsCaptureSession::StartCapture");
            }
            isRunning = true;
        }
        catch (...)
        {
            stop();
            throw;
        }
    }

    void publishSessionWindowExclusionState() noexcept
    {
        capabilities.sessionWindowExclusion = *sessionWindowExclusionState;
    }

    void rejectSessionWindowExclusion(
        const HRESULT result,
        const std::string_view operation)
    {
        sessionWindowExclusionState->status =
            result == E_NOINTERFACE
            ? WgcSessionWindowExclusionStatus::InterfaceUnavailable
            : WgcSessionWindowExclusionStatus::Rejected;
        publishSessionWindowExclusionState();
        throw HResultError(result, std::string(operation));
    }

    void configureSessionWindowExclusion(IUnknown* const sessionUnknown)
    {
        if (!options.requireSessionWindowExclusion)
        {
            publishSessionWindowExclusionState();
            return;
        }
        if (sessionUnknown == nullptr
            || options.excludedWindow == nullptr
            || !IsWindow(options.excludedWindow))
        {
            rejectSessionWindowExclusion(
                E_INVALIDARG,
                "session window exclusion requires a live Overlay HWND");
        }

#if defined(BAFX_WGC_WINDOW_ID_PROJECTION_UNAVAILABLE)
        // SDK 19041 has no projected WindowId value type.  The optional
        // capability remains runtime-gated, but this binary cannot construct
        // the WinRT iterable required by SetWindowExclusionList.
        rejectSessionWindowExclusion(
            E_NOINTERFACE,
            "session window exclusion WindowId projection is unavailable");
#else
        using winrt::Windows::UI::WindowId;

        ComPtr<DisplayGraphicsCaptureSessionAbi> displaySession;
        sessionWindowExclusionState->displaySessionQueryResult =
            sessionUnknown->QueryInterface(IID_PPV_ARGS(&displaySession));
        if (FAILED(
                sessionWindowExclusionState->displaySessionQueryResult))
        {
            rejectSessionWindowExclusion(
                sessionWindowExclusionState->displaySessionQueryResult,
                "GraphicsCaptureSession::QueryInterface("
                "IDisplayGraphicsCaptureSession)");
        }

        ComPtr<GraphicsCaptureSession7Abi> sessionIteration;
        sessionWindowExclusionState->sessionIterationQueryResult =
            sessionUnknown->QueryInterface(IID_PPV_ARGS(&sessionIteration));
        if (FAILED(
                sessionWindowExclusionState->sessionIterationQueryResult))
        {
            rejectSessionWindowExclusion(
                sessionWindowExclusionState->sessionIterationQueryResult,
                "GraphicsCaptureSession::QueryInterface("
                "IGraphicsCaptureSession7)");
        }

        WindowIdAbi abiWindowId{};
        const WindowIdInteropResolver windowIdInterop{};
        sessionWindowExclusionState->windowIdResult =
            windowIdInterop.getWindowId(
                options.excludedWindow,
                &abiWindowId);
        if (FAILED(sessionWindowExclusionState->windowIdResult))
        {
            rejectSessionWindowExclusion(
                sessionWindowExclusionState->windowIdResult,
                "GetWindowIdFromWindow(session window exclusion)");
        }
        sessionWindowExclusionState->requestedWindowId = abiWindowId.Value;

        auto values = winrt::single_threaded_vector<WindowId>();
        values.Append(WindowId{abiWindowId.Value});
        const auto iterable = values.as<
            winrt::Windows::Foundation::Collections::IIterable<WindowId>>();
        sessionWindowExclusionState->setResult =
            displaySession->SetWindowExclusionList(
                winrt::get_abi(iterable),
                &sessionWindowExclusionState->setIteration);
        if (FAILED(sessionWindowExclusionState->setResult))
        {
            rejectSessionWindowExclusion(
                sessionWindowExclusionState->setResult,
                "IDisplayGraphicsCaptureSession::SetWindowExclusionList");
        }

        void* rawView = nullptr;
        sessionWindowExclusionState->getResult =
            displaySession->GetWindowExclusionList(&rawView);
        if (FAILED(sessionWindowExclusionState->getResult)
            || rawView == nullptr)
        {
            rejectSessionWindowExclusion(
                FAILED(sessionWindowExclusionState->getResult)
                    ? sessionWindowExclusionState->getResult
                    : E_UNEXPECTED,
                "IDisplayGraphicsCaptureSession::GetWindowExclusionList");
        }

        winrt::Windows::Foundation::Collections::IVectorView<WindowId> view(
            rawView,
            winrt::take_ownership_from_abi);
        if (view.Size() == 1U)
        {
            sessionWindowExclusionState->observedWindowId =
                view.GetAt(0U).Value;
        }
        sessionWindowExclusionState->windowIdRoundTripConfirmed =
            view.Size() == 1U
            && sessionWindowExclusionState->observedWindowId
                == sessionWindowExclusionState->requestedWindowId;
        if (!sessionWindowExclusionState->windowIdRoundTripConfirmed)
        {
            rejectSessionWindowExclusion(
                E_FAIL,
                "session window exclusion WindowId readback mismatch");
        }

        sessionWindowExclusionState->sessionIterationResult =
            sessionIteration->get_ConfigurationIteration(
                &sessionWindowExclusionState->sessionIteration);
        if (FAILED(sessionWindowExclusionState->sessionIterationResult)
            || sessionWindowExclusionState->sessionIteration
                != sessionWindowExclusionState->setIteration)
        {
            rejectSessionWindowExclusion(
                FAILED(sessionWindowExclusionState->sessionIterationResult)
                    ? sessionWindowExclusionState->sessionIterationResult
                    : E_FAIL,
                "session window exclusion configuration iteration mismatch");
        }

        sessionWindowExclusionState->status =
            WgcSessionWindowExclusionStatus::Applied;
        publishSessionWindowExclusionState();
#endif
    }

    ~Implementation()
    {
        stop();
    }

    [[nodiscard]] Direct3D11CaptureFrame tryGetNextFrame()
    {
        Direct3D11CaptureFrame frame = framePool.TryGetNextFrame();
        if (frame)
        {
            recordResourceLedgerEvent(
                ledger,
                ResourceLedgerEvent::FrameAcquired);
        }
        return frame;
    }

    void closeOwnedFrame(Direct3D11CaptureFrame& frame) noexcept
    {
        if (!frame)
        {
            return;
        }
        try
        {
            frame.Close();
        }
        catch (...)
        {
            // Releasing the projection is still necessary when IClosable fails.
            recordResourceLedgerEvent(ledger, ResourceLedgerEvent::Failure);
        }
        frame = nullptr;
        recordResourceLedgerEvent(ledger, ResourceLedgerEvent::FrameClosed);
    }

    [[nodiscard]] WgcBackgroundDrainDiagnostics drainLatestDetailed(
        ID3D11DeviceContext* context)
    {
        WgcBackgroundDrainDiagnostics diagnostics{};
        diagnostics.epoch = options.epoch;
        diagnostics.frameArrivedCallbacksTotal = notification->generation();
        diagnostics.acceptedGeneration = sampleGeneration;
        if (!isRunning)
        {
            diagnostics.status = WgcBackgroundDrainStatus::Stopped;
            return diagnostics;
        }
        if (context == nullptr)
        {
            throw std::invalid_argument("WGC drain requires a D3D11 device context");
        }
        if (notification->itemClosed())
        {
            stop();
            diagnostics.status = WgcBackgroundDrainStatus::Stopped;
            return diagnostics;
        }
        if (pendingFramePoolSize.has_value())
        {
            diagnostics.status = WgcBackgroundDrainStatus::ReconfigureRequired;
            return diagnostics;
        }

        const std::uint64_t observedGeneration =
            notification->generation();
        diagnostics.frameArrivedCallbacksTotal = observedGeneration;
        Direct3D11CaptureFrame latest = tryGetNextFrame();
        if (!latest)
        {
            notification->resetAfterDrain(observedGeneration, false);
            return diagnostics;
        }
        diagnostics.framesAcquired = 1U;

        try
        {
            std::uint32_t consumedFrameCount = 1U;
            while (consumedFrameCount < maximumFramesPerDrain)
            {
                Direct3D11CaptureFrame next = tryGetNextFrame();
                if (!next)
                {
                    break;
                }
                closeOwnedFrame(latest);
                latest = std::move(next);
                ++consumedFrameCount;
                ++diagnostics.framesAcquired;
                ++diagnostics.framesSuperseded;
            }
            // A hot producer must not monopolize the Render Owner. An extra
            // wake is harmless when the queue contained exactly the budget.
            const bool queuedFramesMayRemain =
                consumedFrameCount == maximumFramesPerDrain;

            if (options.requireSessionWindowExclusion)
            {
                ComPtr<Direct3D11CaptureFrame3Abi> frameIteration;
                const HRESULT frameQueryResult =
                    reinterpret_cast<IUnknown*>(
                        winrt::get_abi(latest))->QueryInterface(
                            IID_PPV_ARGS(&frameIteration));
                HRESULT frameIterationResult = S_FALSE;
                std::uint64_t frameConfigurationIteration = 0U;
                if (SUCCEEDED(frameQueryResult))
                {
                    frameIterationResult = frameIteration->
                        get_ConfigurationIteration(
                            &frameConfigurationIteration);
                }
                const bool frameIterationMatched =
                    detail::observeSessionWindowExclusionFrame(
                        *sessionWindowExclusionState,
                        frameQueryResult,
                        frameIterationResult,
                        frameConfigurationIteration);
                diagnostics.frameConfigurationQueryResult = frameQueryResult;
                diagnostics.frameConfigurationIterationResult =
                    frameIterationResult;
                diagnostics.expectedFrameConfigurationIteration =
                    sessionWindowExclusionState->setIteration;
                diagnostics.frameConfigurationIteration =
                    frameConfigurationIteration;
                diagnostics.configurationIterationRejectedFramesTotal =
                    sessionWindowExclusionState->rejectedFrameCount;
                diagnostics.configurationIterationConsecutiveRejectedFrames =
                    sessionWindowExclusionState->
                        consecutiveRejectedFrameCount;
                diagnostics.frameConfigurationIterationConfirmed =
                    sessionWindowExclusionState->frameIterationConfirmed;
                if (!frameIterationMatched)
                {
                    ++diagnostics.configurationIterationRejectedFrames;
                    publishSessionWindowExclusionState();
                    closeOwnedFrame(latest);
                    notification->resetAfterDrain(
                        observedGeneration,
                        queuedFramesMayRemain);
                    return diagnostics;
                }
                sessionWindowExclusionState->frameIterationConfirmed = true;
                publishSessionWindowExclusionState();
            }

            const SizeInt32 contentSize = latest.ContentSize();
            const WindowSize checkedContentSize = checkedSize(contentSize);
            if (checkedContentSize.width != poolSize.width
                || checkedContentSize.height != poolSize.height)
            {
                closeOwnedFrame(latest);
                latestBackground.reset();
                lastAcceptedTimestamp.reset();
                pendingFramePoolSize = checkedContentSize;
                pendingFramePoolGeneration = observedGeneration;
                // Leave the manual-reset event signaled until the owner runs
                // the explicit Recreate action. This prevents a wait between
                // detection and the lifecycle transaction.
                diagnostics.status = WgcBackgroundDrainStatus::ReconfigureRequired;
                return diagnostics;
            }

            const bafx::core::MonotonicTime timestamp = captureTime(latest);
            if (lastAcceptedTimestamp.has_value()
                && timestamp < *lastAcceptedTimestamp)
            {
                // A regressed driver timestamp must not make an older desktop
                // image look fresh again. Keep the previous sample so it can
                // age out through the normal fallback policy.
                closeOwnedFrame(latest);
                diagnostics.timestampRejectedFrames = 1U;
                notification->resetAfterDrain(
                    observedGeneration,
                    queuedFramesMayRemain);
                return diagnostics;
            }
            const ComPtr<ID3D11Texture2D> sourceTexture = textureFromFrame(latest);
            D3D11_TEXTURE2D_DESC sourceDescription{};
            sourceTexture->GetDesc(&sourceDescription);
            if (sourceDescription.Format != captureDxgiFormat
                || sourceDescription.Width < poolSize.width
                || sourceDescription.Height < poolSize.height)
            {
                throw std::runtime_error("WGC returned an incompatible frame texture");
            }

            const D3D11_BOX sourceBox{
                0U,
                0U,
                0U,
                poolSize.width,
                poolSize.height,
                1U};
            const auto copyStartedAt = std::chrono::steady_clock::now();
            context->CopySubresourceRegion(
                ownedTexture.texture.Get(),
                0U,
                0U,
                0U,
                0U,
                sourceTexture.Get(),
                0U,
                &sourceBox);
            diagnostics.ownedCopySubmitCpu =
                std::chrono::steady_clock::now() - copyStartedAt;
            diagnostics.ownedCopySubmitted = true;

            closeOwnedFrame(latest);
            lastAcceptedTimestamp = timestamp;
            sampleGeneration = nextGeneration(sampleGeneration);
            latestBackground = WgcBackgroundSample{
                ownedTexture.shaderResource.Get(),
                bafx::core::BackgroundFrameStamp{
                    timestamp,
                    options.epoch,
                    true,
                    options.excludesOwnOverlay},
                poolSize,
                sampleGeneration};
            diagnostics.status = WgcBackgroundDrainStatus::Updated;
            diagnostics.accepted = true;
            diagnostics.acceptedGeneration = sampleGeneration;
            notification->resetAfterDrain(
                observedGeneration,
                queuedFramesMayRemain);
            return diagnostics;
        }
        catch (...)
        {
            closeOwnedFrame(latest);
            throw;
        }
    }

    [[nodiscard]] WgcBackgroundTransportSnapshot transportSnapshot() const noexcept
    {
        const bool closed = notification->itemClosed();
        return WgcBackgroundTransportSnapshot{
            options.epoch,
            notification->generation(),
            sampleGeneration,
            isRunning && !closed,
            closed};
    }

    [[nodiscard]] WgcProducerCadenceState configureMinimumUpdateInterval(
        const bafx::core::MonotonicTime interval) noexcept
    {
        WgcProducerCadenceState state{};
        state.requested = interval;
        if (!session || interval <= bafx::core::MonotonicTime::zero())
        {
            state.status = WgcProducerCadenceStatus::Rejected;
            state.result = E_INVALIDARG;
            capabilities.producerCadence = state;
            return state;
        }

        ComPtr<GraphicsCaptureSession5Abi> cadenceSession;
        auto* const sessionUnknown = reinterpret_cast<IUnknown*>(
            winrt::get_abi(session));
        const HRESULT queryResult = sessionUnknown->QueryInterface(
            IID_PPV_ARGS(&cadenceSession));
        if (FAILED(queryResult))
        {
            state.status = queryResult == E_NOINTERFACE
                ? WgcProducerCadenceStatus::InterfaceUnavailable
                : WgcProducerCadenceStatus::Rejected;
            state.result = queryResult;
            capabilities.producerCadence = state;
            return state;
        }

        using winrt::Windows::Foundation::TimeSpan;
        TimeSpan requested = std::chrono::duration_cast<TimeSpan>(interval);
        if (requested <= TimeSpan::zero())
        {
            requested = TimeSpan{1};
        }
        const GraphicsCaptureTimeSpanAbi requestedAbi{requested.count()};
        const HRESULT writeResult =
            cadenceSession->put_MinUpdateInterval(requestedAbi);
        if (FAILED(writeResult))
        {
            // Producer throttling is optional. Consumer-side cadence remains
            // authoritative when the runtime rejects this newer property.
            state.status = WgcProducerCadenceStatus::Rejected;
            state.result = writeResult;
            capabilities.producerCadence = state;
            return state;
        }

        GraphicsCaptureTimeSpanAbi appliedAbi{};
        const HRESULT readResult =
            cadenceSession->get_MinUpdateInterval(&appliedAbi);
        if (FAILED(readResult) || appliedAbi.duration <= 0)
        {
            state.status = WgcProducerCadenceStatus::Rejected;
            state.result = FAILED(readResult) ? readResult : E_UNEXPECTED;
            capabilities.producerCadence = state;
            return state;
        }

        const TimeSpan applied{appliedAbi.duration};
        state.status = WgcProducerCadenceStatus::Applied;
        state.applied = std::chrono::duration_cast<
            bafx::core::MonotonicTime>(applied);
        state.result = S_OK;
        capabilities.producerCadence = state;
        return state;
    }

    void recreateFramePool(const WindowSize size)
    {
        if (!isRunning || notification->itemClosed())
        {
            throw std::runtime_error(
                "WGC frame pool cannot be recreated after session stop");
        }
        if (!pendingFramePoolSize.has_value()
            || pendingFramePoolSize->width != size.width
            || pendingFramePoolSize->height != size.height)
        {
            throw std::invalid_argument(
                "WGC frame pool recreate size does not match the pending frame");
        }

        OwnedBackgroundTexture resizedTexture = createOwnedTexture(
            device.Get(),
            size);
        const SizeInt32 contentSize{
            static_cast<std::int32_t>(size.width),
            static_cast<std::int32_t>(size.height)};
        framePool.Recreate(
            direct3dDevice,
            capturePixelFormat,
            captureBufferCount,
            contentSize);
        recordResourceLedgerEvent(
            ledger,
            ResourceLedgerEvent::FramePoolRecreated);
        ownedTexture = std::move(resizedTexture);
        poolSize = size;
        options.epoch = nextGeneration(options.epoch);
        latestBackground.reset();
        lastAcceptedTimestamp.reset();
        pendingFramePoolSize.reset();
        // Recreate discards the old pool backlog. A callback that raced this
        // action still preserves a conservative extra wake via generation.
        notification->resetAfterDrain(pendingFramePoolGeneration, false);
    }

    void stop() noexcept
    {
        if (stopDiagnostics.completed)
        {
            return;
        }
        const auto stopStartedAt = std::chrono::steady_clock::now();
        detail::WgcBackgroundStopSequence sequence(
            options.stopObserver,
            ownerThreadId,
            GetCurrentThreadId());
        stopDiagnostics.sensorPresent = true;
        stopDiagnostics.ownerThreadMismatch = !sequence.ownerThreadMatched();
        notification->beginStop();
        if (frameArrivedRegistered && framePool)
        {
            const detail::WgcBackgroundStopOperationResult result =
                sequence.run(
                    WgcBackgroundStopStage::FrameArrivedUnregister,
                    [this]()
                    {
                        framePool.FrameArrived(frameArrivedToken);
                    });
            if (!result.succeeded)
            {
                // The pool may already be torn down after a device loss.
                stopDiagnostics.frameArrivedUnregisterFailed = true;
                recordResourceLedgerEvent(ledger, ResourceLedgerEvent::Failure);
            }
            frameArrivedRegistered = false;
            recordResourceLedgerEvent(
                ledger,
                ResourceLedgerEvent::FrameArrivedUnregistered);
            stopDiagnostics.frameArrivedUnregister = result.elapsed;
        }
        if (itemClosedRegistered && item)
        {
            const detail::WgcBackgroundStopOperationResult result =
                sequence.run(
                    WgcBackgroundStopStage::ItemClosedUnregister,
                    [this]()
                    {
                        item.Closed(itemClosedToken);
                    });
            if (!result.succeeded)
            {
                // The item can close itself before the owner reaches shutdown.
                stopDiagnostics.itemClosedUnregisterFailed = true;
                recordResourceLedgerEvent(ledger, ResourceLedgerEvent::Failure);
            }
            itemClosedRegistered = false;
            recordResourceLedgerEvent(
                ledger,
                ResourceLedgerEvent::ItemClosedUnregistered);
            stopDiagnostics.itemClosedUnregister = result.elapsed;
        }
        if (session)
        {
            const detail::WgcBackgroundStopOperationResult result =
                sequence.run(
                    WgcBackgroundStopStage::SessionClose,
                    [this]()
                    {
                        session.Close();
                    });
            if (!result.succeeded)
            {
                // Shutdown must not replace an earlier rendering failure.
                stopDiagnostics.sessionCloseFailed = true;
                recordResourceLedgerEvent(ledger, ResourceLedgerEvent::Failure);
            }
            session = nullptr;
            recordResourceLedgerEvent(
                ledger,
                ResourceLedgerEvent::SessionClosed);
            stopDiagnostics.sessionClose = result.elapsed;
        }
        if (framePool)
        {
            const detail::WgcBackgroundStopOperationResult result =
                sequence.run(
                    WgcBackgroundStopStage::FramePoolClose,
                    [this]()
                    {
                        framePool.Close();
                    });
            if (!result.succeeded)
            {
                // The device may already be removed during process shutdown.
                stopDiagnostics.framePoolCloseFailed = true;
                recordResourceLedgerEvent(ledger, ResourceLedgerEvent::Failure);
            }
            framePool = nullptr;
            recordResourceLedgerEvent(
                ledger,
                ResourceLedgerEvent::FramePoolClosed);
            stopDiagnostics.framePoolClose = result.elapsed;
        }
        item = nullptr;
        direct3dDevice = nullptr;
        latestBackground.reset();
        lastAcceptedTimestamp.reset();
        pendingFramePoolSize.reset();
        ownedTexture = {};
        isRunning = false;
        stopDiagnostics.total =
            std::chrono::steady_clock::now() - stopStartedAt;
        stopDiagnostics.completed = true;
        stopDiagnostics.overallSucceeded =
            !stopDiagnostics.frameArrivedUnregisterFailed
            && !stopDiagnostics.itemClosedUnregisterFailed
            && !stopDiagnostics.sessionCloseFailed
            && !stopDiagnostics.framePoolCloseFailed
            && !stopDiagnostics.ownerThreadMismatch;
        sequence.complete(stopDiagnostics.overallSucceeded);
        // Construction rollback has no published sensor object. Hand the final
        // aggregate to its caller before this Implementation can disappear.
        options.stopResultObserver.notify(stopDiagnostics);
    }

    ComPtr<ID3D11Device> device{};
    IDirect3DDevice direct3dDevice{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool framePool{nullptr};
    GraphicsCaptureSession session{nullptr};
    WgcBackgroundSensorOptions options{};
    DWORD ownerThreadId{0U};
    WgcBackgroundSessionCapabilities capabilities{};
    std::shared_ptr<WgcBackgroundResourceLedger> ledger{};
    std::shared_ptr<WgcSessionWindowExclusionState>
        sessionWindowExclusionState{};
    std::shared_ptr<detail::WgcFrameNotification> notification{};
    OwnedBackgroundTexture ownedTexture{};
    std::optional<WgcBackgroundSample> latestBackground{};
    std::optional<bafx::core::MonotonicTime> lastAcceptedTimestamp{};
    std::optional<WindowSize> pendingFramePoolSize{};
    WindowSize poolSize{};
    winrt::event_token frameArrivedToken{};
    winrt::event_token itemClosedToken{};
    std::uint64_t sampleGeneration{0U};
    std::uint64_t pendingFramePoolGeneration{0U};
    bool frameArrivedRegistered{false};
    bool itemClosedRegistered{false};
    bool isRunning{false};
    WgcBackgroundStopDiagnostics stopDiagnostics{};
};

WgcBackgroundSensor::WgcBackgroundSensor(
    ID3D11Device* device,
    const HMONITOR monitor,
    WgcBackgroundSensorOptions options)
{
    if (device == nullptr)
    {
        throw std::invalid_argument("WGC background sensor requires a D3D11 device");
    }
    if (monitor == nullptr)
    {
        throw std::invalid_argument("WGC background sensor requires a monitor");
    }
    try
    {
        implementation_ = std::make_unique<Implementation>(
            device,
            createMonitorItem(monitor),
            options);
    }
    catch (...)
    {
        recordResourceLedgerEvent(
            options.resourceLedger,
            ResourceLedgerEvent::Failure);
        throw;
    }
}

WgcBackgroundSensor::WgcBackgroundSensor(
    ID3D11Device* device,
    const HWND window,
    WgcBackgroundSensorOptions options)
{
    if (device == nullptr)
    {
        throw std::invalid_argument("WGC background sensor requires a D3D11 device");
    }
    if (window == nullptr || !IsWindow(window))
    {
        throw std::invalid_argument("WGC background sensor requires a live window");
    }
    try
    {
        implementation_ = std::make_unique<Implementation>(
            device,
            createWindowItem(window),
            options);
    }
    catch (...)
    {
        recordResourceLedgerEvent(
            options.resourceLedger,
            ResourceLedgerEvent::Failure);
        throw;
    }
}

WgcBackgroundSensor::~WgcBackgroundSensor() = default;

bool WgcBackgroundSensor::isSupported() noexcept
{
    try
    {
        return GraphicsCaptureSession::IsSupported();
    }
    catch (...)
    {
        return false;
    }
}

WgcBackgroundDrainStatus WgcBackgroundSensor::drainLatest(
    ID3D11DeviceContext* context)
{
    return drainLatestDetailed(context).status;
}

WgcBackgroundDrainDiagnostics WgcBackgroundSensor::drainLatestDetailed(
    ID3D11DeviceContext* context)
{
    try
    {
        return implementation_->drainLatestDetailed(context);
    }
    catch (...)
    {
        recordResourceLedgerEvent(
            implementation_->ledger,
            ResourceLedgerEvent::Failure);
        throw;
    }
}

std::optional<WindowSize>
WgcBackgroundSensor::pendingFramePoolSize() const noexcept
{
    return implementation_->pendingFramePoolSize;
}

void WgcBackgroundSensor::recreateFramePool(const WindowSize size)
{
    try
    {
        implementation_->recreateFramePool(size);
    }
    catch (...)
    {
        recordResourceLedgerEvent(
            implementation_->ledger,
            ResourceLedgerEvent::Failure);
        throw;
    }
}

std::optional<WgcBackgroundSample> WgcBackgroundSensor::latestSample() const noexcept
{
    return implementation_->latestBackground;
}

WgcBackgroundTransportSnapshot
WgcBackgroundSensor::transportSnapshot() const noexcept
{
    return implementation_->transportSnapshot();
}

std::uint64_t WgcBackgroundSensor::expectedEpoch() const noexcept
{
    return implementation_->options.epoch;
}

WgcBackgroundSessionCapabilities WgcBackgroundSensor::capabilities() const noexcept
{
    return implementation_->capabilities;
}

WgcProducerCadenceState WgcBackgroundSensor::configureMinimumUpdateInterval(
    const bafx::core::MonotonicTime interval) noexcept
{
    return implementation_->configureMinimumUpdateInterval(interval);
}

WgcBackgroundResourceLedgerSnapshot
WgcBackgroundSensor::resourceLedger() const noexcept
{
    if (implementation_->ledger == nullptr)
    {
        return {};
    }
    return implementation_->ledger->snapshot();
}

WgcBackgroundStopDiagnostics
WgcBackgroundSensor::stopDiagnostics() const noexcept
{
    return implementation_->stopDiagnostics;
}

HANDLE WgcBackgroundSensor::frameAvailableObject() const noexcept
{
    return implementation_->notification->eventObject();
}

bool WgcBackgroundSensor::running() const noexcept
{
    // item.Closed arrives on the capture callback thread. Report it directly
    // so the Host's control poll can enter cleanup even when no frame is drawn.
    return implementation_->isRunning
        && !implementation_->notification->itemClosed();
}

void WgcBackgroundSensor::stop() noexcept
{
    if (implementation_ != nullptr)
    {
        implementation_->stop();
    }
}

std::string_view wgcProducerCadenceStatusName(
    const WgcProducerCadenceStatus status) noexcept
{
    switch (status)
    {
    case WgcProducerCadenceStatus::NotRequested:
        return "not-requested";
    case WgcProducerCadenceStatus::Applied:
        return "applied";
    case WgcProducerCadenceStatus::InterfaceUnavailable:
        return "interface-unavailable";
    case WgcProducerCadenceStatus::Rejected:
        return "rejected";
    }
    return "unknown";
}

}
