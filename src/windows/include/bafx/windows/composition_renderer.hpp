#pragma once

#include "bafx/core/background_freshness.hpp"
#include "bafx/windows/fx_bloom_settings.hpp"
#include "bafx/windows/overlay_window.hpp"
#include "bafx/windows/unique_handle.hpp"

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace bafx::fx
{

struct FrameSnapshot;

}

namespace bafx::windows
{

class FxGpuRenderer;
class WgcBackgroundSensor;

struct PixelF
{
    float red{0.0F};
    float green{0.0F};
    float blue{0.0F};
    float alpha{0.0F};
};

enum class GraphicsDriverType : std::uint8_t
{
    Hardware,
    Warp
};

struct GraphicsDeviceInfo
{
    GraphicsDriverType driverType{GraphicsDriverType::Hardware};
    std::wstring adapterDescription{};
    LUID adapterLuid{};
    std::uint32_t vendorId{0U};
    std::uint32_t deviceId{0U};
    std::uint32_t subsystemId{0U};
    std::uint32_t revision{0U};
    std::uint64_t dedicatedVideoMemory{0U};
    std::uint64_t dedicatedSystemMemory{0U};
    std::uint64_t sharedSystemMemory{0U};
    std::optional<std::uint64_t> driverVersion{};
    HRESULT hardwareCreateResult{S_OK};
    D3D_FEATURE_LEVEL featureLevel{D3D_FEATURE_LEVEL_11_0};
};

class CompositionRenderer final
{
public:
    CompositionRenderer(
        HWND window,
        WindowSize size,
        FxBloomSettings bloomSettings = {});
    ~CompositionRenderer();

    CompositionRenderer(const CompositionRenderer&) = delete;
    CompositionRenderer& operator=(const CompositionRenderer&) = delete;

    void resize(WindowSize size);
    void setBloomSettings(FxBloomSettings settings);
    void renderFrame(
        const bafx::fx::FrameSnapshot& snapshot,
        bafx::core::MonotonicTime wallTime = bafx::core::MonotonicTime::zero());
    [[nodiscard]] bool tryEnableBackgroundCapture(
        HMONITOR monitor,
        bool exclusionConfirmed) noexcept;
    void disableBackgroundCapture() noexcept;
    [[nodiscard]] bool backgroundCaptureActive() const noexcept;
    [[nodiscard]] std::string_view backgroundCaptureFailure() const noexcept;
    void setReadbackDiagnostics(bool enabled);

    [[nodiscard]] HANDLE frameLatencyWaitableObject() const noexcept;
    [[nodiscard]] D3D_FEATURE_LEVEL featureLevel() const noexcept;
    [[nodiscard]] const GraphicsDeviceInfo& deviceInfo() const noexcept;
    [[nodiscard]] std::optional<PixelF> lastCenterPixel() const noexcept;

private:
    void createDevice();
    void collectDeviceInfo();
    void createSwapChain(WindowSize size);
    void createComposition(HWND window);
    void createRenderTarget();
    void captureCenterPixel();
    [[nodiscard]] bool tryCreateBackgroundSensor() noexcept;

    Microsoft::WRL::ComPtr<ID3D11Device> device_{};
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_{};
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain_{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer_{};
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget_{};
    Microsoft::WRL::ComPtr<IDCompositionDevice> compositionDevice_{};
    Microsoft::WRL::ComPtr<IDCompositionTarget> compositionTarget_{};
    Microsoft::WRL::ComPtr<IDCompositionVisual> rootVisual_{};
    UniqueHandle frameLatencyHandle_{};
    std::unique_ptr<FxGpuRenderer> fxRenderer_{};
    std::unique_ptr<WgcBackgroundSensor> backgroundSensor_{};
    std::optional<PixelF> lastCenterPixel_{};
    bafx::core::MonotonicTime backgroundRefreshPeriod_{};
    HMONITOR backgroundMonitor_{nullptr};
    std::uint64_t backgroundEpoch_{1U};
    bool backgroundCaptureRequested_{false};
    std::string backgroundCaptureFailure_{};
    bool readbackDiagnosticsEnabled_{false};
    D3D_FEATURE_LEVEL featureLevel_{D3D_FEATURE_LEVEL_11_0};
    GraphicsDeviceInfo deviceInfo_{};
    WindowSize size_{};
};

}
