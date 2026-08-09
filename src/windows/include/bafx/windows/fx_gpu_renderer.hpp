#pragma once

#include "bafx/fx/simulation.hpp"
#include "bafx/windows/overlay_window.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstddef>
#include <memory>

namespace bafx::windows
{

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
    void render(const bafx::fx::FrameSnapshot& snapshot, ID3D11Texture2D* destination);

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}
