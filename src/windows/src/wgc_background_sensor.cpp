#include "bafx/windows/wgc_background_sensor.hpp"

#include "bafx/windows/error.hpp"
#include "bafx/windows/unique_handle.hpp"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/base.h>

#include <atomic>
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
constexpr DirectXPixelFormat capturePixelFormat =
    DirectXPixelFormat::R16G16B16A16Float;
constexpr DXGI_FORMAT captureDxgiFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

struct OwnedBackgroundTexture
{
    ComPtr<ID3D11Texture2D> texture{};
    ComPtr<ID3D11ShaderResourceView> shaderResource{};
    WindowSize size{};
};

class NotificationState final
{
public:
    NotificationState()
        : event(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {
        if (event.get() == nullptr)
        {
            throwLastError("CreateEventW(WGC frame available)");
        }
    }

    UniqueHandle event{};
    std::atomic<std::uint64_t> generation{0U};
    std::atomic_bool itemClosed{false};
    std::atomic_bool stopping{false};
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

void closeFrame(Direct3D11CaptureFrame& frame) noexcept
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
    }
    frame = nullptr;
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
    const Direct3D11CaptureFrame& frame) noexcept
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

struct WgcBackgroundSensor::Implementation
{
    Implementation(
        ID3D11Device* sourceDevice,
        const HMONITOR monitor,
        const WgcBackgroundSensorOptions sourceOptions)
        : device(sourceDevice)
        , direct3dDevice(createWinrtDevice(sourceDevice))
        , item(createMonitorItem(monitor))
        , options(sourceOptions)
        , notification(std::make_shared<NotificationState>())
    {
        if (sourceDevice == nullptr)
        {
            throw std::invalid_argument("WGC background sensor requires a D3D11 device");
        }
        if (monitor == nullptr)
        {
            throw std::invalid_argument("WGC background sensor requires a monitor");
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

            const std::shared_ptr<NotificationState> callbackState = notification;
            frameArrivedToken = framePool.FrameArrived(
                [callbackState](const auto&, const auto&) noexcept
                {
                    if (callbackState->stopping.load(std::memory_order_acquire))
                    {
                        return;
                    }
                    callbackState->generation.fetch_add(
                        1U,
                        std::memory_order_release);
                    SetEvent(callbackState->event.get());
                });
            frameArrivedRegistered = true;

            itemClosedToken = item.Closed(
                [callbackState](const auto&, const auto&) noexcept
                {
                    callbackState->itemClosed.store(true, std::memory_order_release);
                    SetEvent(callbackState->event.get());
                });
            itemClosedRegistered = true;

            session = framePool.CreateCaptureSession(item);
            const auto cursorSession = session.try_as<
                winrt::Windows::Graphics::Capture::IGraphicsCaptureSession2>();
            if (!cursorSession)
            {
                throw HResultError(
                    E_NOINTERFACE,
                    "GraphicsCaptureSession cursor exclusion");
            }
            // A captured cursor would be mistaken for the desktop beneath the FX.
            cursorSession.IsCursorCaptureEnabled(false);
            session.StartCapture();
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

    [[nodiscard]] WgcBackgroundDrainStatus drainLatest(
        ID3D11DeviceContext* context)
    {
        if (!isRunning)
        {
            return WgcBackgroundDrainStatus::Stopped;
        }
        if (context == nullptr)
        {
            throw std::invalid_argument("WGC drain requires a D3D11 device context");
        }
        if (notification->itemClosed.load(std::memory_order_acquire))
        {
            stop();
            return WgcBackgroundDrainStatus::Stopped;
        }

        const std::uint64_t observedGeneration =
            notification->generation.load(std::memory_order_acquire);
        Direct3D11CaptureFrame latest = framePool.TryGetNextFrame();
        if (!latest)
        {
            resetNotification(observedGeneration);
            return WgcBackgroundDrainStatus::NoFrame;
        }

        try
        {
            while (Direct3D11CaptureFrame next = framePool.TryGetNextFrame())
            {
                closeFrame(latest);
                latest = std::move(next);
            }

            const SizeInt32 contentSize = latest.ContentSize();
            const WindowSize checkedContentSize = checkedSize(contentSize);
            if (checkedContentSize.width != poolSize.width
                || checkedContentSize.height != poolSize.height)
            {
                OwnedBackgroundTexture resizedTexture = createOwnedTexture(
                    device.Get(),
                    checkedContentSize);
                closeFrame(latest);
                framePool.Recreate(
                    direct3dDevice,
                    capturePixelFormat,
                    captureBufferCount,
                    contentSize);
                ownedTexture = std::move(resizedTexture);
                poolSize = checkedContentSize;
                options.epoch = nextGeneration(options.epoch);
                latestBackground.reset();
                resetNotification(observedGeneration);
                return WgcBackgroundDrainStatus::Reconfigured;
            }

            const bafx::core::MonotonicTime timestamp = captureTime(latest);
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
            context->CopySubresourceRegion(
                ownedTexture.texture.Get(),
                0U,
                0U,
                0U,
                0U,
                sourceTexture.Get(),
                0U,
                &sourceBox);

            closeFrame(latest);
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
            resetNotification(observedGeneration);
            return WgcBackgroundDrainStatus::Updated;
        }
        catch (...)
        {
            closeFrame(latest);
            throw;
        }
    }

    void resetNotification(const std::uint64_t observedGeneration)
    {
        if (!ResetEvent(notification->event.get()))
        {
            throwLastError("ResetEvent(WGC frame available)");
        }
        if (notification->itemClosed.load(std::memory_order_acquire)
            || notification->generation.load(std::memory_order_acquire)
                != observedGeneration)
        {
            SetEvent(notification->event.get());
        }
    }

    void stop() noexcept
    {
        notification->stopping.store(true, std::memory_order_release);
        if (frameArrivedRegistered && framePool)
        {
            try
            {
                framePool.FrameArrived(frameArrivedToken);
            }
            catch (...)
            {
                // The pool may already be torn down after a device loss.
            }
            frameArrivedRegistered = false;
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
            }
            itemClosedRegistered = false;
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
            }
            session = nullptr;
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
            }
            framePool = nullptr;
        }
        item = nullptr;
        direct3dDevice = nullptr;
        latestBackground.reset();
        ownedTexture = {};
        isRunning = false;
        SetEvent(notification->event.get());
    }

    ComPtr<ID3D11Device> device{};
    IDirect3DDevice direct3dDevice{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool framePool{nullptr};
    GraphicsCaptureSession session{nullptr};
    WgcBackgroundSensorOptions options{};
    std::shared_ptr<NotificationState> notification{};
    OwnedBackgroundTexture ownedTexture{};
    std::optional<WgcBackgroundSample> latestBackground{};
    WindowSize poolSize{};
    winrt::event_token frameArrivedToken{};
    winrt::event_token itemClosedToken{};
    std::uint64_t sampleGeneration{0U};
    bool frameArrivedRegistered{false};
    bool itemClosedRegistered{false};
    bool isRunning{false};
};

WgcBackgroundSensor::WgcBackgroundSensor(
    ID3D11Device* device,
    const HMONITOR monitor,
    const WgcBackgroundSensorOptions options)
{
    if (device == nullptr)
    {
        throw std::invalid_argument("WGC background sensor requires a D3D11 device");
    }
    if (monitor == nullptr)
    {
        throw std::invalid_argument("WGC background sensor requires a monitor");
    }
    implementation_ = std::make_unique<Implementation>(device, monitor, options);
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
    return implementation_->drainLatest(context);
}

std::optional<WgcBackgroundSample> WgcBackgroundSensor::latestSample() const noexcept
{
    return implementation_->latestBackground;
}

HANDLE WgcBackgroundSensor::frameAvailableObject() const noexcept
{
    return implementation_->notification->event.get();
}

bool WgcBackgroundSensor::running() const noexcept
{
    return implementation_->isRunning;
}

void WgcBackgroundSensor::stop() noexcept
{
    if (implementation_ != nullptr)
    {
        implementation_->stop();
    }
}

}
