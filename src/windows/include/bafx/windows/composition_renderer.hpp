#pragma once

#include "bafx/windows/overlay_window.hpp"
#include "bafx/windows/unique_handle.hpp"

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <array>
#include <memory>
#include <optional>

namespace bafx::fx
{

struct FrameSnapshot;

}

namespace bafx::windows
{

class FxGpuRenderer;

struct PixelF
{
    float red{0.0F};
    float green{0.0F};
    float blue{0.0F};
    float alpha{0.0F};
};

class CompositionRenderer final
{
public:
    CompositionRenderer(HWND window, WindowSize size);
    ~CompositionRenderer();

    CompositionRenderer(const CompositionRenderer&) = delete;
    CompositionRenderer& operator=(const CompositionRenderer&) = delete;

    void resize(WindowSize size);
    void renderFrame(const bafx::fx::FrameSnapshot& snapshot);
    void setReadbackDiagnostics(bool enabled);

    [[nodiscard]] HANDLE frameLatencyWaitableObject() const noexcept;
    [[nodiscard]] D3D_FEATURE_LEVEL featureLevel() const noexcept;
    [[nodiscard]] std::optional<PixelF> lastCenterPixel() const noexcept;

private:
    void createDevice();
    void createSwapChain(WindowSize size);
    void createComposition(HWND window);
    void createRenderTarget();
    void createDiagnosticStagingTexture();
    void captureCenterPixel();

    Microsoft::WRL::ComPtr<ID3D11Device> device_{};
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_{};
    Microsoft::WRL::ComPtr<IDXGISwapChain2> swapChain_{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer_{};
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget_{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> diagnosticStagingTexture_{};
    Microsoft::WRL::ComPtr<IDCompositionDevice> compositionDevice_{};
    Microsoft::WRL::ComPtr<IDCompositionTarget> compositionTarget_{};
    Microsoft::WRL::ComPtr<IDCompositionVisual> rootVisual_{};
    UniqueHandle frameLatencyHandle_{};
    std::unique_ptr<FxGpuRenderer> fxRenderer_{};
    std::optional<PixelF> lastCenterPixel_{};
    D3D_FEATURE_LEVEL featureLevel_{D3D_FEATURE_LEVEL_11_0};
    WindowSize size_{};
};

}
