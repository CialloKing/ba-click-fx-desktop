#include "bafx/windows/composition_renderer.hpp"

#include "bafx/windows/error.hpp"
#include "bafx/windows/fx_gpu_renderer.hpp"

#include <dxgi1_2.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace bafx::windows
{
namespace
{

constexpr UINT swapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

[[nodiscard]] float halfToFloat(const std::uint16_t value) noexcept
{
    const bool negative = (value & 0x8000U) != 0U;
    const std::uint16_t exponent = static_cast<std::uint16_t>((value >> 10U) & 0x1FU);
    const std::uint16_t mantissa = static_cast<std::uint16_t>(value & 0x03FFU);
    float result = 0.0F;
    if (exponent == 0U)
    {
        result = std::ldexp(static_cast<float>(mantissa), -24);
    }
    else if (exponent == 0x1FU)
    {
        result = mantissa == 0U
            ? std::numeric_limits<float>::infinity()
            : std::numeric_limits<float>::quiet_NaN();
    }
    else
    {
        result = std::ldexp(
            1.0F + static_cast<float>(mantissa) / 1024.0F,
            static_cast<int>(exponent) - 15);
    }
    return negative ? -result : result;
}

}

CompositionRenderer::CompositionRenderer(const HWND window, const WindowSize size)
    : size_(size)
{
    createDevice();
    createSwapChain(size);
    createComposition(window);
    createRenderTarget();
    fxRenderer_ = std::make_unique<FxGpuRenderer>(device_.Get(), context_.Get(), size_);
}

CompositionRenderer::~CompositionRenderer() = default;

void CompositionRenderer::resize(const WindowSize size)
{
    if (size.width == 0U || size.height == 0U
        || (size.width == size_.width && size.height == size_.height))
    {
        return;
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
}

void CompositionRenderer::renderFrame(const bafx::fx::FrameSnapshot& snapshot)
{
    fxRenderer_->render(snapshot, renderTarget_.Get());
    if (diagnosticStagingTexture_ != nullptr)
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

void CompositionRenderer::setReadbackDiagnostics(const bool enabled)
{
    lastCenterPixel_.reset();
    if (enabled)
    {
        createDiagnosticStagingTexture();
    }
    else
    {
        diagnosticStagingTexture_.Reset();
    }
}

HANDLE CompositionRenderer::frameLatencyWaitableObject() const noexcept
{
    return frameLatencyHandle_.get();
}

D3D_FEATURE_LEVEL CompositionRenderer::featureLevel() const noexcept
{
    return featureLevel_;
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
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    HRESULT result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        featureLevels.data(),
        static_cast<UINT>(featureLevels.size()),
        D3D11_SDK_VERSION,
        &device_,
        &featureLevel_,
        &context_);
    if (FAILED(result))
    {
        // WARP keeps the FX-only path usable when a hardware device cannot be created.
        result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            flags,
            featureLevels.data(),
            static_cast<UINT>(featureLevels.size()),
            D3D11_SDK_VERSION,
            &device_,
            &featureLevel_,
            &context_);
    }
    throwIfFailed(result, "D3D11CreateDevice");
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

void CompositionRenderer::createDiagnosticStagingTexture()
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = 1U;
    description.Height = 1U;
    description.MipLevels = 1U;
    description.ArraySize = 1U;
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.SampleDesc = DXGI_SAMPLE_DESC{1U, 0U};
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    throwIfFailed(
        device_->CreateTexture2D(
            &description,
            nullptr,
            &diagnosticStagingTexture_),
        "ID3D11Device::CreateTexture2D(diagnostic staging)");
}

void CompositionRenderer::captureCenterPixel()
{
    const UINT centerX = size_.width / 2U;
    const UINT centerY = size_.height / 2U;
    const D3D11_BOX sourceBox{
        centerX,
        centerY,
        0U,
        centerX + 1U,
        centerY + 1U,
        1U};
    context_->CopySubresourceRegion(
        diagnosticStagingTexture_.Get(),
        0,
        0,
        0,
        0,
        backBuffer_.Get(),
        0,
        &sourceBox);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    throwIfFailed(
        context_->Map(
            diagnosticStagingTexture_.Get(),
            0,
            D3D11_MAP_READ,
            0,
            &mapped),
        "ID3D11DeviceContext::Map(diagnostic staging)");
    const auto* channels = static_cast<const std::uint16_t*>(mapped.pData);
    lastCenterPixel_ = PixelF{
        halfToFloat(channels[0]),
        halfToFloat(channels[1]),
        halfToFloat(channels[2]),
        halfToFloat(channels[3])};
    context_->Unmap(diagnosticStagingTexture_.Get(), 0);
}

}
