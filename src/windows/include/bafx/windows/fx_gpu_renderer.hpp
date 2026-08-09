#pragma once

#include "bafx/fx/simulation.hpp"
#include "bafx/windows/gpu_texture_readback.hpp"
#include "bafx/windows/overlay_window.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstddef>
#include <memory>
#include <vector>

namespace bafx::windows
{

struct FxGpuFrameCapture
{
    Rgba16FloatImage directSurface{};
    Rgba16FloatImage bloomSeed{};
    std::vector<Rgba16FloatImage> bloomDown{};
    std::vector<Rgba16FloatImage> bloomUp{};
    Rgba16FloatImage finalOverlay{};
    bool intermediateLayersValid{false};
};

class FxGpuRenderer final
{
public:
    FxGpuRenderer(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        WindowSize size);
    ~FxGpuRenderer();

    FxGpuRenderer(const FxGpuRenderer&) = delete;
    FxGpuRenderer& operator=(const FxGpuRenderer&) = delete;

    void resize(WindowSize size);
    void render(
        const bafx::fx::FrameSnapshot& snapshot,
        ID3D11RenderTargetView* destination);
    [[nodiscard]] FxGpuFrameCapture renderAndCapture(
        const bafx::fx::FrameSnapshot& snapshot,
        ID3D11RenderTargetView* destination);

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}
