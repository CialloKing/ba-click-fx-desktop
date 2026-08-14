#include "bafx/windows/wgc_background_sensor.hpp"

#include "bafx/windows/detail/wgc_frame_notification.hpp"
#include "bafx/windows/error.hpp"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/base.h>

#include <chrono>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace bafx::windows
{
namespace
{

using Microsoft::WRL::ComPtr;
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
        , ledger(sourceOptions.resourceLedger)
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
            winrt::hresult borderQueryResult{};
            const auto borderSession = session.try_as_with_reason<
                winrt::Windows::Graphics::Capture::IGraphicsCaptureSession3>(
                    borderQueryResult);
            if (!options.allowSystemBorder)
            {
                if (!borderSession)
                {
                    throw HResultError(
                        SUCCEEDED(borderQueryResult)
                            ? E_NOINTERFACE
                            : static_cast<HRESULT>(borderQueryResult),
                        "borderless WGC session is unavailable");
                }
                try
                {
                    borderSession.IsBorderRequired(false);
                    capabilities.borderHidden = !borderSession.IsBorderRequired();
                }
                catch (const winrt::hresult_error& error)
                {
                    throw HResultError(
                        error.code(),
                        "IGraphicsCaptureSession3::IsBorderRequired(false)");
                }
                if (!capabilities.borderHidden)
                {
                    throw HResultError(
                        E_ACCESSDENIED,
                        "Windows kept the WGC system capture border enabled");
                }
            }
            else if (borderSession)
            {
                try
                {
                    capabilities.borderHidden = !borderSession.IsBorderRequired();
                }
                catch (const winrt::hresult_error&)
                {
                    // The visible border is explicitly allowed, so an
                    // unavailable capability query does not block capture.
                    capabilities.borderHidden = false;
                }
            }

            if (options.cursorExcluded
                || options.cursorCaptureEnabledOverride.has_value())
            {
                winrt::hresult cursorQueryResult{};
                const auto cursorSession = session.try_as_with_reason<
                    winrt::Windows::Graphics::Capture::IGraphicsCaptureSession2>(
                        cursorQueryResult);
                if (!cursorSession)
                {
                    throw HResultError(
                        SUCCEEDED(cursorQueryResult)
                            ? E_NOINTERFACE
                            : static_cast<HRESULT>(cursorQueryResult),
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
                try
                {
                    cursorSession.IsCursorCaptureEnabled(
                        requestedCursorCaptureEnabled);
                    capabilities.cursorCaptureEnabled =
                        cursorSession.IsCursorCaptureEnabled();
                    capabilities.cursorExcluded =
                        !capabilities.cursorCaptureEnabled;
                    capabilities.cursorControlConfirmed =
                        capabilities.cursorCaptureEnabled
                        == requestedCursorCaptureEnabled;
                }
                catch (const winrt::hresult_error& error)
                {
                    throw HResultError(
                        error.code(),
                        "IGraphicsCaptureSession2::IsCursorCaptureEnabled(write/readback)");
                }
                if (!capabilities.cursorControlConfirmed)
                {
                    throw HResultError(
                        E_FAIL,
                        "IGraphicsCaptureSession2 cursor readback mismatched the request");
                }
            }
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
        notification->beginStop();
        if (frameArrivedRegistered && framePool)
        {
            try
            {
                framePool.FrameArrived(frameArrivedToken);
            }
            catch (...)
            {
                // The pool may already be torn down after a device loss.
                recordResourceLedgerEvent(ledger, ResourceLedgerEvent::Failure);
            }
            frameArrivedRegistered = false;
            recordResourceLedgerEvent(
                ledger,
                ResourceLedgerEvent::FrameArrivedUnregistered);
        }
        if (itemClosedRegistered && item)
        {
            try
            {
                item.Closed(itemClosedToken);
            }
            catch (...)
            {
                // The item can close itself before the owner reaches shutdown.
                recordResourceLedgerEvent(ledger, ResourceLedgerEvent::Failure);
            }
            itemClosedRegistered = false;
            recordResourceLedgerEvent(
                ledger,
                ResourceLedgerEvent::ItemClosedUnregistered);
        }
        if (session)
        {
            try
            {
                session.Close();
            }
            catch (...)
            {
                // Shutdown must not replace an earlier rendering failure.
                recordResourceLedgerEvent(ledger, ResourceLedgerEvent::Failure);
            }
            session = nullptr;
            recordResourceLedgerEvent(
                ledger,
                ResourceLedgerEvent::SessionClosed);
        }
        if (framePool)
        {
            try
            {
                framePool.Close();
            }
            catch (...)
            {
                // The device may already be removed during process shutdown.
                recordResourceLedgerEvent(ledger, ResourceLedgerEvent::Failure);
            }
            framePool = nullptr;
            recordResourceLedgerEvent(
                ledger,
                ResourceLedgerEvent::FramePoolClosed);
        }
        item = nullptr;
        direct3dDevice = nullptr;
        latestBackground.reset();
        lastAcceptedTimestamp.reset();
        pendingFramePoolSize.reset();
        ownedTexture = {};
        isRunning = false;
    }

    ComPtr<ID3D11Device> device{};
    IDirect3DDevice direct3dDevice{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool framePool{nullptr};
    GraphicsCaptureSession session{nullptr};
    WgcBackgroundSensorOptions options{};
    WgcBackgroundSessionCapabilities capabilities{};
    std::shared_ptr<WgcBackgroundResourceLedger> ledger{};
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

std::uint64_t WgcBackgroundSensor::expectedEpoch() const noexcept
{
    return implementation_->options.epoch;
}

WgcBackgroundSessionCapabilities WgcBackgroundSensor::capabilities() const noexcept
{
    return implementation_->capabilities;
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

}
