#include "bafx/windows/composition_renderer.hpp"

#include "bafx/windows/error.hpp"
#include "bafx/windows/fx_gpu_renderer.hpp"

#include <dxgi1_2.h>

#include <array>

namespace bafx::windows
{
namespace
{

constexpr UINT swapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

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
    fxRenderer_->render(snapshot, backBuffer_.Get());

    const HRESULT result = swapChain_->Present(1, 0);
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
    {
        throwIfFailed(device_->GetDeviceRemovedReason(), "D3D11 device removed");
    }
    throwIfFailed(result, "IDXGISwapChain::Present");
}

HANDLE CompositionRenderer::frameLatencyWaitableObject() const noexcept
{
    return frameLatencyHandle_.get();
}

D3D_FEATURE_LEVEL CompositionRenderer::featureLevel() const noexcept
{
    return featureLevel_;
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

}
