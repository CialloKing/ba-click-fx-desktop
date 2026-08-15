#pragma once

#include "bafx/core/background_freshness.hpp"
#include "bafx/core/roi.hpp"
#include "bafx/fx/frame_bounds.hpp"
#include "bafx/windows/background_snapshot_diagnostics.hpp"
#include "bafx/windows/detail/wgc_idle_drain_policy.hpp"
#include "bafx/windows/fx_bloom_settings.hpp"
#include "bafx/windows/fx_gpu_renderer.hpp"
#include "bafx/windows/gpu_timestamp_profiler.hpp"
#include "bafx/windows/overlay_window.hpp"
#include "bafx/windows/unique_handle.hpp"
#include "bafx/windows/wgc_background_sensor.hpp"

#include <d3d11_4.h>
#include <dcomp.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace bafx::fx
{

struct FrameSnapshot;

}

namespace bafx::windows
{

enum class FxOverlayProfile : std::uint8_t;

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

enum class BackgroundCompositeStatus : std::uint8_t
{
    Inactive,
    WaitingForFrame,
    SizeMismatch,
    Stale,
    FutureTimestamp,
    WrongEpoch,
    InvalidContract,
    InvalidPolicy,
    CaptureFailed,
    LatchedFxOnly,
    Participating
};

enum class OutputResizeStatus : std::uint8_t
{
    Unchanged,
    Resized,
    DeviceRecovered
};

enum class BackgroundFramePoolRecreateStatus : std::uint8_t
{
    Recreated,
    Failed,
    DeviceRecovered,
    DeviceRecoveryFailed
};

struct DeviceRecoveryDiagnostics
{
    std::chrono::nanoseconds total{};
    std::chrono::nanoseconds backgroundStop{};
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

struct RoiFrameDiagnostics
{
    bafx::fx::FrameBoundsStatus visualBoundsStatus{
        bafx::fx::FrameBoundsStatus::Empty};
    bafx::core::RoiStatus planStatus{bafx::core::RoiStatus::Empty};
    bafx::core::RectI currentVisualBounds{};
    bafx::core::RectI dirtyRect{};
    bafx::core::RectI bloomOutput{};
    bafx::core::RectI alignedWork{};
    std::uint32_t guardX{0U};
    std::uint32_t guardY{0U};
    std::uint32_t phasePeriod{0U};
    std::uint64_t fullScreenPixels{0U};
    std::uint64_t bloomOutputPixels{0U};
    std::uint64_t alignedWorkPixels{0U};
    bool currentVisualBoundsAvailable{false};
    bool dirtyRectAvailable{false};
    bool planAvailable{false};
    // ROI remains an observation-only plan until ADR-006 and VAL-ROI pass.
    bool productionFullScreenFallback{true};
};

struct CompositionFrameDiagnostics
{
    std::uint64_t frameId{0U};
    RoiFrameDiagnostics roi{};
    WgcBackgroundDrainDiagnostics wgc{};
    FxRenderCpuDiagnostics fx{};
    BackgroundCompositeStatus backgroundStatus{
        BackgroundCompositeStatus::Inactive};
    std::uint64_t backgroundSnapshotEpoch{0U};
    std::uint64_t backgroundSnapshotGeneration{0U};
    std::chrono::nanoseconds frameTotalCpu{};
    std::chrono::nanoseconds wgcDrainInclusiveCpu{};
    std::chrono::nanoseconds backgroundSnapshotSubmitCpu{};
    std::chrono::nanoseconds diagnosticReadbackCpu{};
    std::chrono::nanoseconds presentCallCpu{};
    std::chrono::nanoseconds backgroundSampleAge{};
    std::int64_t presentReturnedQpc{0};
    std::uint32_t presentReturnedTickMilliseconds{0U};
    bool wgcActive{false};
    bool wgcDrainAttempted{false};
    bool wgcIdleDrainAttempted{false};
    bool wgcIdleDrainSkipped{false};
    bool backgroundSampleAgeValid{false};
    bool backgroundSnapshotRefreshAttempted{false};
    bool backgroundSnapshotRefreshed{false};
    bool backgroundParticipated{false};
    GpuTimestampPollResult gpuTimestampPoll{};
    GpuTimestampBeginStatus gpuTimestampBegin{
        GpuTimestampBeginStatus::Unavailable};
    GpuTimestampEndStatus gpuTimestampEnd{
        GpuTimestampEndStatus::NoActiveFrame};
    HRESULT gpuTimestampInitializationResult{E_FAIL};
    std::size_t gpuTimestampPendingFrames{0U};
    bool gpuTimestampProfilerAvailable{false};
    bool gpuTimestampCheckpointFailure{false};
};

struct BackgroundSensorMaintenanceDiagnostics
{
    WgcBackgroundDrainDiagnostics wgc{};
    std::chrono::nanoseconds wgcDrainInclusiveCpu{};
    bool wgcActive{false};
    bool wgcDrainAttempted{false};
    bool wgcIdleDrainAttempted{false};
    bool wgcIdleDrainSkipped{false};
};

class CompositionRenderer final
{
public:
    CompositionRenderer(
        HWND window,
        WindowSize size,
        FxBloomSettings bloomSettings = {},
        WgcBackgroundStopObserver backgroundStopObserver = {});
    ~CompositionRenderer();

    CompositionRenderer(const CompositionRenderer&) = delete;
    CompositionRenderer& operator=(const CompositionRenderer&) = delete;

    // Capture lifecycle is owned by BackgroundCaptureTransition. Callers must
    // stop any active sensor before replacing output-size resources.
    [[nodiscard]] OutputResizeStatus resizeOutput(WindowSize size);
    // Rebuild the D3D/DComp resource domain once after device removal. WGC is
    // stopped first so no old-device frame or snapshot can cross the boundary.
    [[nodiscard]] bool tryRecoverDevice() noexcept;
    [[nodiscard]] std::string_view deviceRecoveryFailure() const noexcept;
    [[nodiscard]] DeviceRecoveryDiagnostics
        deviceRecoveryDiagnostics() const noexcept;
    [[nodiscard]] bool setBloomSettings(FxBloomSettings settings);
    void setOverlayProfile(FxOverlayProfile profile);
    CompositionFrameDiagnostics renderFrame(
        const bafx::fx::FrameSnapshot& snapshot,
        bafx::core::MonotonicTime wallTime = bafx::core::MonotonicTime::zero(),
        bool requireCurrentBackground = false);
    // Keeps the bounded WGC queue fresh while the Host intentionally skips
    // rendering. This never creates a batch snapshot or presents a frame.
    [[nodiscard]] BackgroundSensorMaintenanceDiagnostics
        serviceBackgroundCapture(
            bafx::core::MonotonicTime wallTime =
                bafx::core::MonotonicTime::zero()) noexcept;
    // SPK-001 must exercise the production FP16 swap chain and DComp target
    // without reconstructing them in a second renderer. The returned pixel is
    // captured before Present so DWM observation can be compared independently.
    [[nodiscard]] PixelF presentCompositionProbeColor(PixelF color);
    [[nodiscard]] bool tryEnableBackgroundCapture(
        HMONITOR monitor,
        bool exclusionConfirmed,
        bool cursorExcluded = true,
        bool allowSystemBorder = false,
        bool borderlessAccessConfirmed = false) noexcept;
    [[nodiscard]] std::optional<WindowSize>
        pendingBackgroundFramePoolSize() const noexcept;
    [[nodiscard]] BackgroundFramePoolRecreateStatus
        tryRecreateBackgroundFramePool(WindowSize size) noexcept;
    void disableBackgroundCapture() noexcept;
    [[nodiscard]] bool backgroundCaptureActive() const noexcept;
    [[nodiscard]] bool backgroundCaptureRestartAllowed() const noexcept;
    [[nodiscard]] bool backgroundCaptureBorderHidden() const noexcept;
    [[nodiscard]] bool backgroundCaptureCursorExcluded() const noexcept;
    [[nodiscard]] WgcBackgroundResourceLedgerSnapshot
        backgroundResourceLedger() const noexcept;
    [[nodiscard]] WgcBackgroundStopDiagnostics
        takeBackgroundStopDiagnostics() noexcept;
    [[nodiscard]] std::optional<BackgroundSnapshotInvalidation>
        takeBackgroundSnapshotInvalidation() noexcept;
    [[nodiscard]] bool backgroundParticipatedInLastFrame() const noexcept;
    [[nodiscard]] BackgroundCompositeStatus backgroundCompositeStatus() const noexcept;
    [[nodiscard]] std::string_view backgroundCaptureFailure() const noexcept;
    void setReadbackDiagnostics(bool enabled);

    [[nodiscard]] HANDLE frameLatencyWaitableObject() const noexcept;
    [[nodiscard]] HANDLE deviceRemovedWaitableObject() const noexcept;
    [[nodiscard]] HRESULT deviceRemovedNotificationResult() const noexcept;
    [[nodiscard]] HRESULT deviceRemovedReason() const noexcept;
    [[nodiscard]] HANDLE backgroundFrameAvailableObject() const noexcept;
    [[nodiscard]] D3D_FEATURE_LEVEL featureLevel() const noexcept;
    [[nodiscard]] const GraphicsDeviceInfo& deviceInfo() const noexcept;
    [[nodiscard]] std::optional<PixelF> lastCenterPixel() const noexcept;

private:
    void createDevice();
    void collectDeviceInfo();
    void createSwapChain(WindowSize size);
    void createComposition(HWND window);
    void createRenderTarget();
    void registerDeviceRemovedNotification() noexcept;
    void unregisterDeviceRemovedNotification() noexcept;
    void presentSwapChain();
    void releaseDeviceResources() noexcept;
    void resetBackgroundSnapshot(
        BackgroundSnapshotInvalidationReason reason,
        std::uint64_t frameId) noexcept;
    void releaseBackgroundSnapshotResources(
        BackgroundSnapshotInvalidationReason reason) noexcept;
    void stopBackgroundSensor() noexcept;
    [[nodiscard]] BackgroundSensorMaintenanceDiagnostics
        drainBackgroundSensor(
            bool hasDrawableContent,
            bafx::core::MonotonicTime wallTime,
            std::uint64_t frameId) noexcept;
    [[nodiscard]] bool captureBackgroundSnapshot(
        ID3D11ShaderResourceView* source,
        std::uint64_t frameId) noexcept;
    void captureCenterPixel();
    void setBackgroundCaptureFailure(std::string_view message) noexcept;
    void setDeviceRecoveryFailure(std::string_view message) noexcept;
    [[nodiscard]] bool tryCreateBackgroundSensor() noexcept;

    HWND window_{nullptr};
    Microsoft::WRL::ComPtr<ID3D11Device> device_{};
    Microsoft::WRL::ComPtr<ID3D11Device4> device4_{};
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_{};
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain_{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer_{};
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget_{};
    // WGC owns and reuses its latest texture. Keep a ping-pong pair of owned
    // copies: one is the previous accepted sample and the other receives the
    // temporal filter output before the pair is swapped.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backgroundSnapshotTexture_{};
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> backgroundSnapshotRenderTarget_{};
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> backgroundSnapshotShaderResource_{};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backgroundCandidateTexture_{};
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> backgroundCandidateRenderTarget_{};
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> backgroundCandidateShaderResource_{};
    WindowSize backgroundSnapshotSize_{};
    std::uint64_t backgroundSnapshotEpoch_{0U};
    std::uint64_t backgroundSnapshotGeneration_{0U};
    bool backgroundSnapshotValid_{false};
    Microsoft::WRL::ComPtr<IDCompositionDevice> compositionDevice_{};
    Microsoft::WRL::ComPtr<IDCompositionTarget> compositionTarget_{};
    Microsoft::WRL::ComPtr<IDCompositionVisual> rootVisual_{};
    UniqueHandle frameLatencyHandle_{};
    UniqueHandle deviceRemovedHandle_{};
    DWORD deviceRemovedCookie_{0U};
    HRESULT deviceRemovedNotificationResult_{E_NOINTERFACE};
    bool deviceRemovedNotificationRegistered_{false};
    std::unique_ptr<GpuTimestampProfiler> gpuTimestampProfiler_{};
    std::unique_ptr<FxGpuRenderer> fxRenderer_{};
    std::unique_ptr<WgcBackgroundSensor> backgroundSensor_{};
    std::optional<PixelF> lastCenterPixel_{};
    bafx::core::MonotonicTime backgroundRefreshPeriod_{};
    HMONITOR backgroundMonitor_{nullptr};
    std::uint64_t backgroundEpoch_{1U};
    bool backgroundCaptureRequested_{false};
    bool backgroundCursorExcluded_{true};
    bool backgroundSystemBorderAllowed_{false};
    bafx::core::BackgroundPathLatch backgroundPathLatch_{};
    detail::WgcDrainPolicyState wgcDrainPolicyState_{};
    bool backgroundParticipatedInLastFrame_{false};
    BackgroundCompositeStatus backgroundCompositeStatus_{
        BackgroundCompositeStatus::Inactive};
    // Failure reporting is used by noexcept fallback paths. Keep it inline so
    // a diagnostic allocation can never terminate the render process.
    std::array<char, 512U> backgroundCaptureFailure_{};
    std::size_t backgroundCaptureFailureLength_{0U};
    std::array<char, 512U> deviceRecoveryFailure_{};
    std::size_t deviceRecoveryFailureLength_{0U};
    bool readbackDiagnosticsEnabled_{false};
    D3D_FEATURE_LEVEL featureLevel_{D3D_FEATURE_LEVEL_11_0};
    GraphicsDeviceInfo deviceInfo_{};
    FxBloomSettings bloomSettings_{};
    FxOverlayProfile overlayProfile_{FxOverlayProfile::FxOnlyFallback};
    std::optional<bafx::core::RectI> previousVisualBounds_{};
    WindowSize size_{};
    std::shared_ptr<WgcBackgroundResourceLedger> backgroundResourceLedger_{};
    WgcBackgroundStopObserver backgroundStopObserver_{};
    detail::WgcBackgroundStopMailbox backgroundStopMailbox_{};
    detail::BackgroundSnapshotInvalidationMailbox
        backgroundSnapshotInvalidationMailbox_{};
    std::uint64_t frameId_{0U};
    bool deviceRecoveryAttempted_{false};
    bool backgroundCaptureAfterRecoveryAllowed_{true};
    DeviceRecoveryDiagnostics deviceRecoveryDiagnostics_{};
};

}
