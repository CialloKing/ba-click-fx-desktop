#pragma once

#include "bafx/fx/simulation.hpp"
#include "bafx/core/types.hpp"
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
#include <string_view>
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

enum class FxActiveRoiActualPath : std::uint8_t
{
    Disabled,
    Idle,
    FullScreen,
    RoiWarmup,
    RoiPrefilter,
    Unavailable
};

[[nodiscard]] constexpr std::string_view fxActiveRoiActualPathName(
    const FxActiveRoiActualPath path) noexcept
{
    switch (path)
    {
    case FxActiveRoiActualPath::Disabled:
        return "disabled";
    case FxActiveRoiActualPath::Idle:
        return "idle";
    case FxActiveRoiActualPath::FullScreen:
        return "full-screen";
    case FxActiveRoiActualPath::RoiWarmup:
        return "roi-warmup";
    case FxActiveRoiActualPath::RoiPrefilter:
        return "roi-prefilter";
    case FxActiveRoiActualPath::Unavailable:
        return "unavailable";
    }
    return "unavailable";
}

enum class FxActiveRoiDecisionReason : std::uint8_t
{
    Disabled,
    NoContent,
    BackgroundDifferentialBloom,
    Context1Unavailable,
    SharedTargetFullWrite,
    AreaTooLarge,
    BenefitTooSmall,
    Applied,
    RendererFallback
};

[[nodiscard]] constexpr std::string_view fxActiveRoiDecisionReasonName(
    const FxActiveRoiDecisionReason reason) noexcept
{
    switch (reason)
    {
    case FxActiveRoiDecisionReason::Disabled:
        return "disabled";
    case FxActiveRoiDecisionReason::NoContent:
        return "no-content";
    case FxActiveRoiDecisionReason::BackgroundDifferentialBloom:
        return "background-differential-bloom";
    case FxActiveRoiDecisionReason::Context1Unavailable:
        return "context1-unavailable";
    case FxActiveRoiDecisionReason::SharedTargetFullWrite:
        return "shared-target-full-write";
    case FxActiveRoiDecisionReason::AreaTooLarge:
        return "area-too-large";
    case FxActiveRoiDecisionReason::BenefitTooSmall:
        return "benefit-too-small";
    case FxActiveRoiDecisionReason::Applied:
        return "applied";
    case FxActiveRoiDecisionReason::RendererFallback:
        return "renderer-fallback";
    }
    return "renderer-fallback";
}

struct FxActiveRoiPassDiagnostics
{
    bool requested{false};
    bool eligible{false};
    bool executed{false};
    bool warmup{false};
    FxActiveRoiActualPath actualPath{FxActiveRoiActualPath::Disabled};
    FxActiveRoiDecisionReason decisionReason{
        FxActiveRoiDecisionReason::Disabled};
    std::uint64_t fullPixels{0U};
    std::uint64_t drawnPixels{0U};
    std::uint64_t clearedPixels{0U};
};

struct FxRenderCpuDiagnostics
{
    std::chrono::nanoseconds totalSubmit{};
    std::chrono::nanoseconds materialsSubmit{};
    std::chrono::nanoseconds bloomAndCompositeSubmit{};
    bool visualContent{false};
    FxActiveRoiPassDiagnostics primaryActiveFxRoi{};
    FxActiveRoiPassDiagnostics recordingRebuildActiveFxRoi{};
    // Retain the aggregate fields while existing log/report consumers migrate
    // to the two real execution paths above.
    bool activeFxRoiApplied{false};
    std::uint64_t activeFxRoiPixels{0U};
};

struct FxActiveRoi final
{
    // Full-resolution, phase-aligned work domain. The renderer maps it onto
    // the first Bloom target while preserving the original full-screen UVs.
    bafx::core::RectI alignedWork{};
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
    void resetActiveFxRoiState() noexcept;
    void setThemeColor(std::string_view themeColor);
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
        GpuTimestampProfiler* gpuTimestampProfiler = nullptr,
        ID3D11RenderTargetView* recordingDestination = nullptr,
        std::optional<FxActiveRoi> activeRoi = std::nullopt);
    [[nodiscard]] FxGpuFrameCapture renderAndCapture(
        const bafx::fx::FrameSnapshot& snapshot,
        ID3D11RenderTargetView* destination);

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}
