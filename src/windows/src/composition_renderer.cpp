#include "bafx/windows/composition_renderer.hpp"

#include "bafx/windows/error.hpp"
#include "bafx/windows/fx_gpu_renderer.hpp"
#include "bafx/windows/gpu_texture_readback.hpp"
#include "bafx/windows/wgc_background_sensor.hpp"

#include <dwmapi.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>

namespace bafx::windows
{
namespace
{

constexpr UINT swapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
constexpr DXGI_COLOR_SPACE_TYPE swapChainColorSpace =
    DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;

[[nodiscard]] std::optional<bafx::core::MonotonicTime>
primaryRefreshPeriod() noexcept
{
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &timing))
        || timing.rateRefresh.uiNumerator == 0U
        || timing.rateRefresh.uiDenominator == 0U)
    {
        return std::nullopt;
    }

    const double seconds = static_cast<double>(timing.rateRefresh.uiDenominator)
        / static_cast<double>(timing.rateRefresh.uiNumerator);
    if (!std::isfinite(seconds) || seconds <= 0.0 || seconds > 1.0)
    {
        return std::nullopt;
    }
    const auto period = std::chrono::duration_cast<bafx::core::MonotonicTime>(
        std::chrono::duration<double>(seconds));
    if (period <= bafx::core::MonotonicTime::zero())
    {
        return std::nullopt;
    }
    return period;
}

[[nodiscard]] std::uint64_t nextEpoch(const std::uint64_t epoch) noexcept
{
    return epoch == std::numeric_limits<std::uint64_t>::max()
        ? 1U
        : epoch + 1U;
}

}

CompositionRenderer::CompositionRenderer(
    const HWND window,
    const WindowSize size,
    const FxBloomSettings bloomSettings)
    : size_(size)
{
    createDevice();
    createSwapChain(size);
    createComposition(window);
    createRenderTarget();
    fxRenderer_ = std::make_unique<FxGpuRenderer>(
        device_.Get(),
        context_.Get(),
        size_,
        bloomSettings);
}

CompositionRenderer::~CompositionRenderer() = default;

void CompositionRenderer::resize(const WindowSize size)
{
    if (size.width == 0U || size.height == 0U
        || (size.width == size_.width && size.height == size_.height))
    {
        return;
    }

    if (backgroundSensor_ != nullptr)
    {
        // Capture teardown shares the render owner's immediate-context domain;
        // finish it before swap-chain and size-dependent resources are replaced.
        backgroundSensor_->stop();
        backgroundSensor_.reset();
    }

    context_->OMSetRenderTargets(0, nullptr, nullptr);
    renderTarget_.Reset();
    backBuffer_.Reset();
    throwIfFailed(
        swapChain_->ResizeBuffers(
            0,
            size.width,
            size.height,
            DXGI_FORMAT_UNKNOWN,
            swapChainFlags),
        "IDXGISwapChain::ResizeBuffers");
    size_ = size;
    createRenderTarget();
    fxRenderer_->resize(size);
    static_cast<void>(tryCreateBackgroundSensor());
}

void CompositionRenderer::setBloomSettings(const FxBloomSettings settings)
{
    fxRenderer_->setBloomSettings(settings);
}

void CompositionRenderer::renderFrame(
    const bafx::fx::FrameSnapshot& snapshot,
    const bafx::core::MonotonicTime wallTime)
{
    std::optional<BackgroundRenderInput> background;
    bafx::core::MonotonicTime effectiveWallTime = wallTime;
    if (effectiveWallTime == bafx::core::MonotonicTime::zero())
    {
        LARGE_INTEGER counter{};
        if (QueryPerformanceCounter(&counter))
        {
            LARGE_INTEGER frequency{};
            if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
            {
                const auto seconds = counter.QuadPart / frequency.QuadPart;
                const auto remainder = counter.QuadPart % frequency.QuadPart;
                // Keep the legacy overload usable for diagnostics while the
                // desktop host passes its calibrated QPC time explicitly.
                const auto now = std::chrono::seconds(seconds)
                    + std::chrono::nanoseconds(
                        remainder * 1'000'000'000LL / frequency.QuadPart);
                effectiveWallTime = std::chrono::duration_cast<
                    bafx::core::MonotonicTime>(now);
            }
        }
    }
    if (backgroundSensor_ != nullptr)
    {
        try
        {
            const WgcBackgroundDrainStatus drainStatus =
                backgroundSensor_->drainLatest(context_.Get());
            if (drainStatus == WgcBackgroundDrainStatus::Stopped)
            {
                backgroundSensor_.reset();
                backgroundRefreshPeriod_ = bafx::core::MonotonicTime::zero();
            }
            else
            {
                const std::optional<WgcBackgroundSample> sample =
                    backgroundSensor_->latestSample();
                if (sample.has_value()
                    && sample->size.width == size_.width
                    && sample->size.height == size_.height
                    && backgroundRefreshPeriod_ > bafx::core::MonotonicTime::zero())
                {
                    const bafx::core::BackgroundFreshnessResult freshness =
                        bafx::core::evaluateBackgroundFreshness(
                            sample->stamp,
                            effectiveWallTime,
                            bafx::core::BackgroundFreshnessPolicy{
                                backgroundRefreshPeriod_,
                                backgroundRefreshPeriod_ * 3,
                                std::min(
                                    std::chrono::duration_cast<
                                        bafx::core::MonotonicTime>(
                                        std::chrono::milliseconds(2)),
                                    backgroundRefreshPeriod_ / 2),
                                backgroundSensor_->expectedEpoch()});
                    if (freshness.freshness
                            == bafx::core::BackgroundFreshness::Fresh
                        || freshness.freshness
                            == bafx::core::BackgroundFreshness::Fading)
                    {
                        background = BackgroundRenderInput{
                            sample->texture,
                            freshness.weight};
                    }
                }
            }
        }
        catch (...)
        {
            // Background sensing is optional. A capture/device failure must
            // never stop the FX-only interaction path.
            backgroundSensor_->stop();
            backgroundSensor_.reset();
            backgroundRefreshPeriod_ = bafx::core::MonotonicTime::zero();
        }
    }

    fxRenderer_->render(snapshot, renderTarget_.Get(), background);
    if (readbackDiagnosticsEnabled_)
    {
        captureCenterPixel();
    }

    const HRESULT result = swapChain_->Present(1, 0);
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
    {
        throwIfFailed(device_->GetDeviceRemovedReason(), "D3D11 device removed");
    }
    throwIfFailed(result, "IDXGISwapChain::Present");
}

bool CompositionRenderer::tryEnableBackgroundCapture(
    const HMONITOR monitor,
    const bool exclusionConfirmed) noexcept
{
    backgroundCaptureFailure_.clear();
    if (backgroundSensor_ != nullptr)
    {
        backgroundSensor_->stop();
        backgroundSensor_.reset();
    }
    backgroundCaptureRequested_ = exclusionConfirmed
        && monitor != nullptr
        && deviceInfo_.driverType == GraphicsDriverType::Hardware;
    backgroundMonitor_ = backgroundCaptureRequested_ ? monitor : nullptr;
    backgroundRefreshPeriod_ = bafx::core::MonotonicTime::zero();
    if (!backgroundCaptureRequested_)
    {
        if (!exclusionConfirmed)
        {
            backgroundCaptureFailure_ = "capture exclusion was not confirmed";
        }
        else if (monitor == nullptr)
        {
            backgroundCaptureFailure_ = "target monitor is unavailable";
        }
        else if (deviceInfo_.driverType != GraphicsDriverType::Hardware)
        {
            backgroundCaptureFailure_ =
                "WGC requires a hardware D3D11 device";
        }
        return false;
    }

    return tryCreateBackgroundSensor();
}

void CompositionRenderer::disableBackgroundCapture() noexcept
{
    backgroundCaptureRequested_ = false;
    backgroundMonitor_ = nullptr;
    backgroundRefreshPeriod_ = bafx::core::MonotonicTime::zero();
    backgroundCaptureFailure_.clear();
    if (backgroundSensor_ != nullptr)
    {
        backgroundSensor_->stop();
        backgroundSensor_.reset();
    }
    backgroundEpoch_ = nextEpoch(backgroundEpoch_);
}

bool CompositionRenderer::backgroundCaptureActive() const noexcept
{
    return backgroundSensor_ != nullptr && backgroundSensor_->running();
}

bool CompositionRenderer::tryCreateBackgroundSensor() noexcept
{
    if (!backgroundCaptureRequested_ || backgroundMonitor_ == nullptr)
    {
        if (backgroundCaptureRequested_ && backgroundMonitor_ == nullptr)
        {
            backgroundCaptureFailure_ = "target monitor is unavailable";
        }
        return false;
    }
    if (!WgcBackgroundSensor::isSupported())
    {
        backgroundCaptureFailure_ =
            "Windows Graphics Capture is not supported";
        return false;
    }

    const std::optional<bafx::core::MonotonicTime> refreshPeriod =
        primaryRefreshPeriod();
    if (!refreshPeriod.has_value())
    {
        backgroundCaptureFailure_ =
            "display refresh period could not be determined";
        return false;
    }

    try
    {
        backgroundCaptureFailure_.clear();
        backgroundSensor_ = std::make_unique<WgcBackgroundSensor>(
            device_.Get(),
            backgroundMonitor_,
            WgcBackgroundSensorOptions{backgroundEpoch_, true});
        backgroundEpoch_ = nextEpoch(backgroundEpoch_);
        backgroundRefreshPeriod_ = *refreshPeriod;
        return true;
    }
    catch (...)
    {
        backgroundSensor_.reset();
        backgroundRefreshPeriod_ = bafx::core::MonotonicTime::zero();
        try
        {
            throw;
        }
        catch (const std::exception& error)
        {
            backgroundCaptureFailure_ = error.what();
        }
        catch (...)
        {
            backgroundCaptureFailure_ = "unknown WGC initialization failure";
        }
        return false;
    }
}

std::string_view CompositionRenderer::backgroundCaptureFailure() const noexcept
{
    return backgroundCaptureFailure_;
}

void CompositionRenderer::setReadbackDiagnostics(const bool enabled)
{
    lastCenterPixel_.reset();
    readbackDiagnosticsEnabled_ = enabled;
}

HANDLE CompositionRenderer::frameLatencyWaitableObject() const noexcept
{
    return frameLatencyHandle_.get();
}

D3D_FEATURE_LEVEL CompositionRenderer::featureLevel() const noexcept
{
    return featureLevel_;
}

const GraphicsDeviceInfo& CompositionRenderer::deviceInfo() const noexcept
{
    return deviceInfo_;
}

std::optional<PixelF> CompositionRenderer::lastCenterPixel() const noexcept
{
    return lastCenterPixel_;
}

void CompositionRenderer::createDevice()
{
    constexpr std::array featureLevels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    constexpr UINT baseFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const auto create = [this, &featureLevels](
                            const D3D_DRIVER_TYPE driver,
                            const UINT flags)
    {
        device_.Reset();
        context_.Reset();
        return D3D11CreateDevice(
            nullptr,
            driver,
            nullptr,
            flags,
            featureLevels.data(),
            static_cast<UINT>(featureLevels.size()),
            D3D11_SDK_VERSION,
            &device_,
            &featureLevel_,
            &context_);
    };

#if defined(_DEBUG)
    HRESULT result = create(
        D3D_DRIVER_TYPE_HARDWARE,
        baseFlags | D3D11_CREATE_DEVICE_DEBUG);
    if (result == DXGI_ERROR_SDK_COMPONENT_MISSING)
    {
        // End-user machines often omit Graphics Tools; diagnostics must remain optional.
        result = create(D3D_DRIVER_TYPE_HARDWARE, baseFlags);
    }
#else
    HRESULT result = create(D3D_DRIVER_TYPE_HARDWARE, baseFlags);
#endif
    const HRESULT hardwareCreateResult = result;
    if (FAILED(result))
    {
        // WARP keeps the FX-only path usable when a hardware device cannot be created.
        result = create(D3D_DRIVER_TYPE_WARP, baseFlags);
        deviceInfo_.driverType = GraphicsDriverType::Warp;
    }
    throwIfFailed(result, "D3D11CreateDevice");
    deviceInfo_.hardwareCreateResult = hardwareCreateResult;
    deviceInfo_.featureLevel = featureLevel_;
    collectDeviceInfo();
}

void CompositionRenderer::collectDeviceInfo()
{
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    throwIfFailed(device_.As(&dxgiDevice), "ID3D11Device::QueryInterface(IDXGIDevice)");

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    throwIfFailed(dxgiDevice->GetAdapter(&adapter), "IDXGIDevice::GetAdapter");

    DXGI_ADAPTER_DESC description{};
    throwIfFailed(adapter->GetDesc(&description), "IDXGIAdapter::GetDesc");
    deviceInfo_.adapterDescription = description.Description;
    deviceInfo_.adapterLuid = description.AdapterLuid;
    deviceInfo_.vendorId = description.VendorId;
    deviceInfo_.deviceId = description.DeviceId;
    deviceInfo_.subsystemId = description.SubSysId;
    deviceInfo_.revision = description.Revision;
    deviceInfo_.dedicatedVideoMemory = description.DedicatedVideoMemory;
    deviceInfo_.dedicatedSystemMemory = description.DedicatedSystemMemory;
    deviceInfo_.sharedSystemMemory = description.SharedSystemMemory;

    LARGE_INTEGER driverVersion{};
    const HRESULT driverResult = adapter->CheckInterfaceSupport(
        __uuidof(IDXGIDevice),
        &driverVersion);
    if (SUCCEEDED(driverResult))
    {
        deviceInfo_.driverVersion = static_cast<std::uint64_t>(driverVersion.QuadPart);
    }
}

void CompositionRenderer::createSwapChain(const WindowSize size)
{
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    throwIfFailed(device_.As(&dxgiDevice), "ID3D11Device::QueryInterface(IDXGIDevice)");

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    throwIfFailed(dxgiDevice->GetAdapter(&adapter), "IDXGIDevice::GetAdapter");

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    throwIfFailed(adapter->GetParent(IID_PPV_ARGS(&factory)), "IDXGIAdapter::GetParent");

    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = size.width;
    description.Height = size.height;
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.Stereo = FALSE;
    description.SampleDesc = DXGI_SAMPLE_DESC{1, 0};
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    description.Flags = swapChainFlags;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
    throwIfFailed(
        factory->CreateSwapChainForComposition(
            device_.Get(),
            &description,
            nullptr,
            &swapChain),
        "IDXGIFactory2::CreateSwapChainForComposition");
    throwIfFailed(swapChain.As(&swapChain_), "IDXGISwapChain1::QueryInterface");

    UINT colorSpaceSupport = 0U;
    throwIfFailed(
        swapChain_->CheckColorSpaceSupport(swapChainColorSpace, &colorSpaceSupport),
        "IDXGISwapChain3::CheckColorSpaceSupport(scRGB)");
    if ((colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0U)
    {
        // The final shader emits linear premultiplied values. Continuing with
        // DXGI's default gamma interpretation would silently change their hue.
        throwIfFailed(
            DXGI_ERROR_UNSUPPORTED,
            "IDXGISwapChain3::CheckColorSpaceSupport(scRGB present)");
    }
    throwIfFailed(
        swapChain_->SetColorSpace1(swapChainColorSpace),
        "IDXGISwapChain3::SetColorSpace1(scRGB)");
    throwIfFailed(swapChain_->SetMaximumFrameLatency(1), "IDXGISwapChain2::SetMaximumFrameLatency");

    frameLatencyHandle_.reset(swapChain_->GetFrameLatencyWaitableObject());
    if (frameLatencyHandle_.get() == nullptr)
    {
        throwLastError("IDXGISwapChain2::GetFrameLatencyWaitableObject");
    }

}

void CompositionRenderer::createComposition(const HWND window)
{
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    throwIfFailed(device_.As(&dxgiDevice), "ID3D11Device::QueryInterface(IDXGIDevice)");
    throwIfFailed(
        DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&compositionDevice_)),
        "DCompositionCreateDevice");
    throwIfFailed(
        compositionDevice_->CreateTargetForHwnd(window, TRUE, &compositionTarget_),
        "IDCompositionDevice::CreateTargetForHwnd");
    throwIfFailed(
        compositionDevice_->CreateVisual(&rootVisual_),
        "IDCompositionDevice::CreateVisual");
    throwIfFailed(rootVisual_->SetContent(swapChain_.Get()), "IDCompositionVisual::SetContent");
    throwIfFailed(compositionTarget_->SetRoot(rootVisual_.Get()), "IDCompositionTarget::SetRoot");
    throwIfFailed(compositionDevice_->Commit(), "IDCompositionDevice::Commit");
}

void CompositionRenderer::createRenderTarget()
{
    throwIfFailed(
        swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer_)),
        "IDXGISwapChain::GetBuffer");
    throwIfFailed(
        device_->CreateRenderTargetView(backBuffer_.Get(), nullptr, &renderTarget_),
        "ID3D11Device::CreateRenderTargetView");
}

void CompositionRenderer::captureCenterPixel()
{
    const UINT centerX = size_.width / 2U;
    const UINT centerY = size_.height / 2U;
    // The one-pixel region keeps the interactive smoke test from synchronizing
    // and copying the full monitor-sized swap-chain buffer.
    const Rgba16FloatImage image = readbackRgba16FloatTexture(
        context_.Get(),
        backBuffer_.Get(),
        TextureReadbackRegion{centerX, centerY, 1U, 1U});
    const Rgba16FloatPixel pixel = image.pixels.front();
    lastCenterPixel_ = PixelF{
        halfToFloat(pixel.red),
        halfToFloat(pixel.green),
        halfToFloat(pixel.blue),
        halfToFloat(pixel.alpha)};
}

}
