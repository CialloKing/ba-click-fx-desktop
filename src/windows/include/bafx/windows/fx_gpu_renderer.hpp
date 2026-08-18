#pragma once

#include "bafx/fx/simulation.hpp"
#include "bafx/windows/composition_output.hpp"
#include "bafx/windows/fx_bloom_settings.hpp"
#include "bafx/windows/gpu_texture_readback.hpp"
#include "bafx/windows/overlay_window.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace bafx::windows
{

class GpuTimestampProfiler;

struct FxGpuFrameCapture
{
    Rgba16FloatImage directSurface{};
    Rgba16FloatImage bloomSeed{};
    std::vector<Rgba16FloatImage> bloomDown{};
    std::vector<Rgba16FloatImage> bloomUp{};
    Rgba16FloatImage bloomResult{};
    Rgba16FloatImage finalOverlay{};
    bool intermediateLayersValid{false};
};

struct BackgroundRenderInput
{
    ID3D11ShaderResourceView* shaderResource{nullptr};
};

struct FxRenderCpuDiagnostics
{
    std::chrono::nanoseconds totalSubmit{};
    std::chrono::nanoseconds materialsSubmit{};
    std::chrono::nanoseconds bloomAndCompositeSubmit{};
    bool visualContent{false};
};

enum class FxOverlayProfile : std::uint8_t
{
    FxOnlyFallback,
    RecordingCompatible,
    LightBackground,
    Core
};

class FxGpuRenderer final
{
public:
    FxGpuRenderer(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        WindowSize size,
        FxBloomSettings bloomSettings = {},
        CompositionOutputMapping outputMapping = compositionOutputPolicyFor(
            CompositionOutputPreference::PreferLinearScRgb).mapping);
    ~FxGpuRenderer();

    FxGpuRenderer(const FxGpuRenderer&) = delete;
    FxGpuRenderer& operator=(const FxGpuRenderer&) = delete;

    void resize(WindowSize size);
    void setBloomSettings(FxBloomSettings settings);
    void setOverlayProfile(FxOverlayProfile profile);
    // WGC and DComp run on independent clocks. Filter accepted captures before
    // they feed both Differential Bloom and the final source-over transport so
    // one FP16 rounding step cannot modulate a visible effect.
    void stabilizeBackgroundFrame(
        ID3D11ShaderResourceView* previous,
        ID3D11ShaderResourceView* current,
        ID3D11RenderTargetView* destination);
    FxRenderCpuDiagnostics render(
        const bafx::fx::FrameSnapshot& snapshot,
        ID3D11RenderTargetView* destination,
        std::optional<BackgroundRenderInput> background = std::nullopt,
        GpuTimestampProfiler* gpuTimestampProfiler = nullptr);
    [[nodiscard]] FxGpuFrameCapture renderAndCapture(
        const bafx::fx::FrameSnapshot& snapshot,
        ID3D11RenderTargetView* destination);

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}
