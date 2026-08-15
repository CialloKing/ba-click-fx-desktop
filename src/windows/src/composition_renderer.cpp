#include "bafx/windows/composition_renderer.hpp"

#include "bafx/windows/borderless_capture_access.hpp"
#include "bafx/windows/display_capabilities.hpp"
#include "bafx/windows/error.hpp"
#include "bafx/windows/fx_gpu_renderer.hpp"
#include "bafx/windows/gpu_texture_readback.hpp"
#include "bafx/windows/wgc_background_sensor.hpp"

#include <dxgi1_2.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace bafx::windows
{
namespace
{

constexpr UINT swapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
constexpr DXGI_COLOR_SPACE_TYPE swapChainColorSpace =
    DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
constexpr bafx::core::MonotonicTime minimumBackgroundCadencePeriod =
    std::chrono::nanoseconds(16'666'667);
constexpr bafx::core::MonotonicTime minimumBackgroundAcquireLifetime =
    std::chrono::milliseconds(100);
constexpr bafx::core::MonotonicTime minimumBackgroundRetainLifetime =
    std::chrono::milliseconds(250);
constexpr std::int64_t backgroundAcquirePeriodCount = 6;
constexpr std::int64_t backgroundRetainPeriodCount = 12;
constexpr std::int64_t backgroundFuturePeriodCount = 3;

class GpuTimestampFrameScope final
{
public:
    GpuTimestampFrameScope(
        GpuTimestampProfiler& profiler,
        const std::uint64_t frameId,
        CompositionFrameDiagnostics& diagnostics) noexcept
        : profiler_(profiler), diagnostics_(diagnostics)
    {
        diagnostics_.gpuTimestampProfilerAvailable = profiler_.available();
        diagnostics_.gpuTimestampInitializationResult =
            profiler_.initializationResult();
        diagnostics_.gpuTimestampPoll = profiler_.poll(frameId);
        diagnostics_.gpuTimestampBegin = profiler_.beginFrame(frameId);
        active_ = diagnostics_.gpuTimestampBegin
            == GpuTimestampBeginStatus::Started;
    }

    ~GpuTimestampFrameScope()
    {
        if (active_)
        {
            // An exception must close the disjoint query so one bad frame
            // cannot leave the fixed ring permanently occupied.
            (void)profiler_.cancelFrame();
        }
    }

    GpuTimestampFrameScope(const GpuTimestampFrameScope&) = delete;
    GpuTimestampFrameScope& operator=(const GpuTimestampFrameScope&) = delete;

    void checkpoint(const GpuTimestampCheckpoint checkpoint) noexcept
    {
        if (active_
            && profiler_.checkpoint(checkpoint)
                != GpuTimestampCheckpointStatus::Recorded)
        {
            diagnostics_.gpuTimestampCheckpointFailure = true;
        }
    }

    [[nodiscard]] GpuTimestampProfiler* recorder() noexcept
    {
        return active_ ? &profiler_ : nullptr;
    }

    void complete(const GpuTimestampFrameUsage usage) noexcept
    {
        if (active_)
        {
            diagnostics_.gpuTimestampEnd = profiler_.endFrame(usage);
            active_ = false;
        }
        diagnostics_.gpuTimestampPendingFrames =
            profiler_.pendingFrameCount();
    }

private:
    GpuTimestampProfiler& profiler_;
    CompositionFrameDiagnostics& diagnostics_;
    bool active_{false};
};

[[nodiscard]] std::optional<bafx::core::MonotonicTime>
primaryRefreshPeriod() noexcept
{
    const std::optional<DisplayRefreshRate> refreshRate =
        queryPrimaryCompositionRefreshRate();
    if (!refreshRate.has_value())
    {
        return std::nullopt;
    }

    const double seconds = static_cast<double>(refreshRate->denominator)
        / static_cast<double>(refreshRate->numerator);
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

[[nodiscard]] bafx::core::BackgroundUsagePolicy backgroundUsagePolicy(
    const bafx::core::MonotonicTime refreshPeriod,
    const std::uint64_t expectedEpoch,
    const bool retain,
    const bool requireCurrentBackground) noexcept
{
    if (requireCurrentBackground)
    {
        return bafx::core::BackgroundUsagePolicy{
            refreshPeriod,
            refreshPeriod,
            expectedEpoch};
    }

    return bafx::core::BackgroundUsagePolicy{
        std::max(
            refreshPeriod * (retain
                ? backgroundRetainPeriodCount
                : backgroundAcquirePeriodCount),
            retain
                ? minimumBackgroundRetainLifetime
                : minimumBackgroundAcquireLifetime),
        refreshPeriod * backgroundFuturePeriodCount,
        expectedEpoch};
}

[[nodiscard]] BackgroundCompositeStatus compositeStatus(
    const bafx::core::BackgroundUsageStatus status) noexcept
{
    switch (status)
    {
    case bafx::core::BackgroundUsageStatus::Stale:
        return BackgroundCompositeStatus::Stale;
    case bafx::core::BackgroundUsageStatus::FutureTimestamp:
        return BackgroundCompositeStatus::FutureTimestamp;
    case bafx::core::BackgroundUsageStatus::WrongEpoch:
        return BackgroundCompositeStatus::WrongEpoch;
    case bafx::core::BackgroundUsageStatus::InvalidContract:
        return BackgroundCompositeStatus::InvalidContract;
    case bafx::core::BackgroundUsageStatus::InvalidPolicy:
        return BackgroundCompositeStatus::InvalidPolicy;
    case bafx::core::BackgroundUsageStatus::Usable:
    case bafx::core::BackgroundUsageStatus::Missing:
        return BackgroundCompositeStatus::WaitingForFrame;
    }
    return BackgroundCompositeStatus::WaitingForFrame;
}

[[nodiscard]] std::uint64_t rectArea(const bafx::core::RectI rect) noexcept
{
    if (rect.left >= rect.right || rect.top >= rect.bottom)
    {
        return 0U;
    }

    const std::uint64_t width = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(rect.right)
        - static_cast<std::int64_t>(rect.left));
    const std::uint64_t height = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(rect.bottom)
        - static_cast<std::int64_t>(rect.top));
    if (height != 0U
        && width > std::numeric_limits<std::uint64_t>::max() / height)
    {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return width * height;
}

[[nodiscard]] std::optional<bafx::core::RectI> monitorRect(
    const WindowSize size) noexcept
{
    if (size.width == 0U || size.height == 0U
        || size.width > static_cast<std::uint32_t>(
            std::numeric_limits<std::int32_t>::max())
        || size.height > static_cast<std::uint32_t>(
            std::numeric_limits<std::int32_t>::max()))
    {
        return std::nullopt;
    }
    return bafx::core::RectI{
        0,
        0,
        static_cast<std::int32_t>(size.width),
        static_cast<std::int32_t>(size.height)};
}

void populateRoiDiagnostics(
    CompositionFrameDiagnostics& diagnostics,
    const bafx::fx::FrameSnapshot& snapshot,
    const WindowSize size,
    const FxBloomSettings bloomSettings,
    std::optional<bafx::core::RectI>& previousVisualBounds) noexcept
{
    diagnostics.roi.fullScreenPixels =
        static_cast<std::uint64_t>(size.width)
        * static_cast<std::uint64_t>(size.height);

    const bafx::fx::FrameVisualBoundsResult bounds =
        bafx::fx::visualBounds(snapshot);
    diagnostics.roi.visualBoundsStatus = bounds.status;
    if (bounds.status == bafx::fx::FrameBoundsStatus::Ok)
    {
        diagnostics.roi.currentVisualBounds = bounds.bounds;
        diagnostics.roi.currentVisualBoundsAvailable = true;
    }

    const std::optional<bafx::core::RectI> currentBounds =
        bounds.status == bafx::fx::FrameBoundsStatus::Ok
        ? std::optional<bafx::core::RectI>(bounds.bounds)
        : std::nullopt;
    const std::optional<bafx::core::RectI> dirtyRect =
        bafx::fx::uniteVisualBounds(previousVisualBounds, currentBounds);
    if (dirtyRect.has_value())
    {
        diagnostics.roi.dirtyRect = *dirtyRect;
        diagnostics.roi.dirtyRectAvailable = true;
    }

    if (bounds.status == bafx::fx::FrameBoundsStatus::Ok)
    {
        previousVisualBounds = currentBounds;
    }
    else if (bounds.status == bafx::fx::FrameBoundsStatus::Empty)
    {
        previousVisualBounds.reset();
    }
    else
    {
        // An invalid frame cannot safely seed a later ROI. Drop the previous
        // range so a recovered frame starts a fresh conservative batch.
        previousVisualBounds.reset();
    }

    if (bounds.status != bafx::fx::FrameBoundsStatus::Ok
        && bounds.status != bafx::fx::FrameBoundsStatus::Empty)
    {
        diagnostics.roi.planStatus = bafx::core::RoiStatus::InvalidRect;
        return;
    }

    const std::optional<bafx::core::RectI> fullMonitor = monitorRect(size);
    if (!dirtyRect.has_value() || !fullMonitor.has_value())
    {
        diagnostics.roi.planStatus = dirtyRect.has_value()
            ? bafx::core::RoiStatus::InvalidRect
            : bafx::core::RoiStatus::Empty;
        return;
    }

    const bafx::core::UnityBloomPlanResult bloom = bafx::core::planUnityBloom(
        bafx::core::BloomExtent{
            fullMonitor->right,
            fullMonitor->bottom},
        bafx::core::UnityBloomSettings{
            bloomSettings.diffusion,
            0.0F,
            1.7F});
    if (bloom.status != bafx::core::UnityBloomStatus::Ok)
    {
        diagnostics.roi.planStatus = bafx::core::RoiStatus::InvalidFootprint;
        return;
    }

    const bafx::core::BloomRoiPlanResult plan =
        bafx::core::planUnityBloomRoi(
            *dirtyRect,
            *fullMonitor,
            bloom.plan);
    diagnostics.roi.planStatus = plan.status;
    if (plan.status != bafx::core::RoiStatus::Ok)
    {
        return;
    }

    diagnostics.roi.guardX = plan.plan.guardX;
    diagnostics.roi.guardY = plan.plan.guardY;
    diagnostics.roi.phasePeriod = plan.plan.phasePeriod;
    diagnostics.roi.bloomOutput = plan.plan.bloomOutput;
    diagnostics.roi.alignedWork = plan.plan.alignedWork;
    diagnostics.roi.bloomOutputPixels = rectArea(plan.plan.bloomOutput);
    diagnostics.roi.alignedWorkPixels = rectArea(plan.plan.alignedWork);
    diagnostics.roi.planAvailable = true;
}

}

CompositionRenderer::CompositionRenderer(
    const HWND window,
    const WindowSize size,
    const FxBloomSettings bloomSettings)
    : window_(window)
    , bloomSettings_(bloomSettings)
    , size_(size)
    , backgroundResourceLedger_(
          std::make_shared<WgcBackgroundResourceLedger>())
{
    createDevice();
    createSwapChain(size);
    createComposition(window);
    createRenderTarget();
    gpuTimestampProfiler_ = std::make_unique<GpuTimestampProfiler>(
        device_.Get(),
        context_.Get());
    fxRenderer_ = std::make_unique<FxGpuRenderer>(
        device_.Get(),
        context_.Get(),
        size_,
        bloomSettings);
}

CompositionRenderer::~CompositionRenderer() = default;

bool CompositionRenderer::tryRecoverDevice() noexcept
{
    setDeviceRecoveryFailure({});
    try
    {
        // WGC textures and the temporal snapshot belong to the old device;
        // invalidate them before releasing the swap-chain resource domain.
        disableBackgroundCapture();
        previousVisualBounds_.reset();
        lastCenterPixel_.reset();
        backgroundCompositeStatus_ = BackgroundCompositeStatus::Inactive;
        releaseDeviceResources();
        featureLevel_ = D3D_FEATURE_LEVEL_11_0;

        createDevice();
        createSwapChain(size_);
        createComposition(window_);
        createRenderTarget();
        gpuTimestampProfiler_ = std::make_unique<GpuTimestampProfiler>(
            device_.Get(),
            context_.Get());
        fxRenderer_ = std::make_unique<FxGpuRenderer>(
            device_.Get(),
            context_.Get(),
            size_,
            bloomSettings_);
        fxRenderer_->setOverlayProfile(overlayProfile_);
        setReadbackDiagnostics(readbackDiagnosticsEnabled_);
        return true;
    }
    catch (...)
    {
        try
        {
            throw;
        }
        catch (const std::exception& error)
        {
            setDeviceRecoveryFailure(error.what());
        }
        catch (...)
        {
            setDeviceRecoveryFailure("unknown device recovery failure");
        }
        releaseDeviceResources();
        return false;
    }
}

std::string_view CompositionRenderer::deviceRecoveryFailure() const noexcept
{
    return std::string_view(
        deviceRecoveryFailure_.data(),
        deviceRecoveryFailureLength_);
}

void CompositionRenderer::resizeOutput(const WindowSize size)
{
    if (size.width == 0U || size.height == 0U
        || (size.width == size_.width && size.height == size_.height))
    {
        return;
    }

    if (backgroundSensor_ != nullptr || backgroundCaptureRequested_)
    {
        throw std::logic_error(
            "Output resize requires a completed capture-stop transaction");
    }

    // A resized swap chain starts a new capture contract. Do not carry the
    // previous visible batch's path into the new session while its first
    // correctly sized WGC frame is still pending.
    backgroundPathLatch_.reset();
    releaseBackgroundSnapshotResources();
    previousVisualBounds_.reset();

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

void CompositionRenderer::setBloomSettings(const FxBloomSettings settings)
{
    bloomSettings_ = settings;
    fxRenderer_->setBloomSettings(settings);
}

void CompositionRenderer::releaseDeviceResources() noexcept
{
    if (context_ != nullptr)
    {
        context_->OMSetRenderTargets(0U, nullptr, nullptr);
    }
    // Snapshot views are created on the same device as the swap chain. Keep
    // this helper self-contained so an exceptional recovery path cannot leave
    // old-device views alive behind the new resource domain.
    releaseBackgroundSnapshotResources();
    gpuTimestampProfiler_.reset();
    fxRenderer_.reset();
    renderTarget_.Reset();
    backBuffer_.Reset();
    rootVisual_.Reset();
    compositionTarget_.Reset();
    compositionDevice_.Reset();
    frameLatencyHandle_.reset();
    swapChain_.Reset();
    context_.Reset();
    device_.Reset();
}

void CompositionRenderer::setOverlayProfile(const FxOverlayProfile profile)
{
    fxRenderer_->setOverlayProfile(profile);
    overlayProfile_ = profile;
}

CompositionFrameDiagnostics CompositionRenderer::renderFrame(
    const bafx::fx::FrameSnapshot& snapshot,
    const bafx::core::MonotonicTime wallTime,
    const bool requireCurrentBackground)
{
    CompositionFrameDiagnostics diagnostics{};
    diagnostics.frameId = ++frameId_;
    populateRoiDiagnostics(
        diagnostics,
        snapshot,
        size_,
        bloomSettings_,
        previousVisualBounds_);
    GpuTimestampFrameScope gpuTimestampFrame(
        *gpuTimestampProfiler_,
        diagnostics.frameId,
        diagnostics);
    const auto frameStartedAt = std::chrono::steady_clock::now();
    const bool hasDrawableContent = snapshot.hasDrawableContent();
    std::optional<BackgroundRenderInput> background;
    std::optional<WgcBackgroundSample> backgroundSample;
    bafx::core::BackgroundUsageDecision acquireUsage{};
    bafx::core::BackgroundUsageDecision retainUsage{};
    if (!hasDrawableContent)
    {
        // A new visible batch gets a fresh desktop reference. Keeping the
        // previous copy across an idle frame would make a later click inherit
        // an unrelated background and reintroduce a bright-surface pulse.
        resetBackgroundSnapshot();
    }
    backgroundParticipatedInLastFrame_ = false;
    backgroundCompositeStatus_ = backgroundSensor_ != nullptr
        ? BackgroundCompositeStatus::WaitingForFrame
        : BackgroundCompositeStatus::Inactive;
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
        diagnostics.wgcActive = true;
        const WgcBackgroundTransportSnapshot transport =
            backgroundSensor_->transportSnapshot();
        diagnostics.wgc.epoch = transport.epoch;
        diagnostics.wgc.frameArrivedCallbacksTotal =
            transport.frameArrivedCallbacksTotal;
        diagnostics.wgc.acceptedGeneration = transport.acceptedGeneration;
    }
    if (backgroundSensor_ != nullptr && hasDrawableContent)
    {
        diagnostics.wgcDrainAttempted = true;
        const auto stopFailedBackgroundCapture =
            [this](const std::string_view failure) noexcept
        {
            setBackgroundCaptureFailure(failure);
            backgroundSensor_->stop();
            backgroundSensor_.reset();
            // The producer is gone before the control transaction observes
            // it.  Mark the request inactive now so a same-turn resize can
            // rebuild the output instead of tripping the stop-before-resize
            // guard on a dead sensor.
            backgroundCaptureRequested_ = false;
            backgroundRefreshPeriod_ = bafx::core::MonotonicTime::zero();
            backgroundCompositeStatus_ = BackgroundCompositeStatus::CaptureFailed;
            // A failed session cannot continue the previous path safely: its
            // resource and epoch are no longer part of the active contract.
            backgroundPathLatch_.reset();
            resetBackgroundSnapshot();
        };
        try
        {
            const auto drainStartedAt = std::chrono::steady_clock::now();
            diagnostics.wgc = backgroundSensor_->drainLatestDetailed(
                context_.Get());
            diagnostics.wgcDrainInclusiveCpu =
                std::chrono::steady_clock::now() - drainStartedAt;
            const WgcBackgroundDrainStatus drainStatus = diagnostics.wgc.status;
            if (drainStatus == WgcBackgroundDrainStatus::Stopped)
            {
                backgroundSensor_.reset();
                // item.Closed is an asynchronous terminal event.  Do not
                // let a resize arriving before the next control poll treat
                // the already-destroyed producer as active.
                backgroundCaptureRequested_ = false;
                backgroundRefreshPeriod_ = bafx::core::MonotonicTime::zero();
                // The capture session ended between visible frames. The next
                // session must make an independent acquire decision.
                backgroundPathLatch_.reset();
                resetBackgroundSnapshot();
            }
            else if (drainStatus
                == WgcBackgroundDrainStatus::ReconfigureRequired)
            {
                // The owner will execute Recreate through the capture
                // transaction after this frame has selected FX-only.
                backgroundPathLatch_.reset();
                resetBackgroundSnapshot();
            }
            else
            {
                const std::optional<WgcBackgroundSample> sample =
                    backgroundSensor_->latestSample();
                if (sample.has_value()
                    && sample->texture != nullptr
                    && sample->size.width == size_.width
                    && sample->size.height == size_.height
                    && backgroundRefreshPeriod_ > bafx::core::MonotonicTime::zero())
                {
                    backgroundSample = sample;
                    if (effectiveWallTime >= sample->stamp.capturedAt)
                    {
                        diagnostics.backgroundSampleAge = effectiveWallTime
                            - sample->stamp.capturedAt;
                        diagnostics.backgroundSampleAgeValid = true;
                    }
                    acquireUsage = bafx::core::evaluateBackgroundUsage(
                        sample->stamp,
                        effectiveWallTime,
                        backgroundUsagePolicy(
                            backgroundRefreshPeriod_,
                            backgroundSensor_->expectedEpoch(),
                            false,
                            requireCurrentBackground));
                    retainUsage = bafx::core::evaluateBackgroundUsage(
                        sample->stamp,
                        effectiveWallTime,
                        backgroundUsagePolicy(
                            backgroundRefreshPeriod_,
                            backgroundSensor_->expectedEpoch(),
                            true,
                            requireCurrentBackground));
                    backgroundCompositeStatus_ = compositeStatus(acquireUsage.status);
                }
                else if (sample.has_value())
                {
                    backgroundCompositeStatus_ = sample->texture == nullptr
                        ? BackgroundCompositeStatus::InvalidContract
                        : BackgroundCompositeStatus::SizeMismatch;
                }
            }
        }
        catch (const std::exception& error)
        {
            // Background sensing is optional. A capture/device failure must
            // never stop the FX-only interaction path. Preserve the original
            // failure before destroying the sensor so Host cleanup can log it.
            stopFailedBackgroundCapture(error.what());
        }
        catch (...)
        {
            stopFailedBackgroundCapture("unknown WGC drain failure");
        }
    }
    else if (backgroundSensor_ != nullptr)
    {
        // The callback can continue coalescing producer progress while idle.
        // The next visible frame drains the existing bounded queue once.
        diagnostics.wgcIdleDrainSkipped = true;
    }
    gpuTimestampFrame.checkpoint(
        GpuTimestampCheckpoint::WgcDrainAndCopyComplete);

    // The render path remains stable for the visible batch, but its owned
    // background copy follows each accepted WGC generation. Freezing the first
    // image would bake stale light UI into every later source-over payload.
    const bool retainedBackgroundAvailable = backgroundSnapshotValid_
        || (backgroundSample.has_value() && retainUsage.enabled);
    const bafx::core::BackgroundRenderPath renderPath =
        backgroundPathLatch_.select(
            hasDrawableContent,
            backgroundSample.has_value() && acquireUsage.enabled,
            retainedBackgroundAvailable);
    if (renderPath == bafx::core::BackgroundRenderPath::BackgroundAware)
    {
        const bool refreshSnapshot = backgroundSample.has_value()
            && retainUsage.enabled
            && (!backgroundSnapshotValid_
                || backgroundSample->generation
                    != backgroundSnapshotGeneration_);
        diagnostics.backgroundSnapshotRefreshAttempted = refreshSnapshot;
        bool snapshotRefreshed = false;
        if (refreshSnapshot)
        {
            const auto snapshotStartedAt = std::chrono::steady_clock::now();
            snapshotRefreshed = captureBackgroundSnapshot(
                backgroundSample->texture);
            diagnostics.backgroundSnapshotSubmitCpu =
                std::chrono::steady_clock::now() - snapshotStartedAt;
        }
        if (snapshotRefreshed)
        {
            backgroundSnapshotValid_ = true;
            backgroundSnapshotGeneration_ = backgroundSample->generation;
            diagnostics.backgroundSnapshotRefreshed = true;
        }
        if (backgroundSnapshotValid_)
        {
            background = BackgroundRenderInput{
                backgroundSnapshotShaderResource_.Get()};
            backgroundParticipatedInLastFrame_ = true;
            backgroundCompositeStatus_ = BackgroundCompositeStatus::Participating;
        }
        else
        {
            // Optional WGC transport must never prevent the stable FX-only
            // path when a snapshot allocation or copy is unavailable. Keep
            // this visible batch latched so a later frame cannot upgrade.
            backgroundPathLatch_.forceFxOnly();
            backgroundCompositeStatus_ = BackgroundCompositeStatus::CaptureFailed;
        }
    }
    else
    {
        // A new FX-only batch must not inherit a previous batch's snapshot.
        resetBackgroundSnapshot();
        if (hasDrawableContent
            && backgroundSample.has_value()
            && acquireUsage.enabled)
        {
            backgroundCompositeStatus_ = BackgroundCompositeStatus::LatchedFxOnly;
        }
    }

    gpuTimestampFrame.checkpoint(
        GpuTimestampCheckpoint::BackgroundSnapshotComplete);

    diagnostics.fx = fxRenderer_->render(
        snapshot,
        renderTarget_.Get(),
        background,
        gpuTimestampFrame.recorder());
    gpuTimestampFrame.complete(GpuTimestampFrameUsage{
        diagnostics.wgcDrainAttempted,
        diagnostics.backgroundSnapshotRefreshAttempted,
        diagnostics.fx.visualContent});
    if (readbackDiagnosticsEnabled_)
    {
        const auto readbackStartedAt = std::chrono::steady_clock::now();
        captureCenterPixel();
        diagnostics.diagnosticReadbackCpu =
            std::chrono::steady_clock::now() - readbackStartedAt;
    }

    const auto presentStartedAt = std::chrono::steady_clock::now();
    presentSwapChain();
    diagnostics.presentCallCpu =
        std::chrono::steady_clock::now() - presentStartedAt;
    LARGE_INTEGER presentReturned{};
    if (QueryPerformanceCounter(&presentReturned))
    {
        diagnostics.presentReturnedQpc = presentReturned.QuadPart;
    }
    diagnostics.presentReturnedTickMilliseconds = GetTickCount();
    diagnostics.backgroundStatus = backgroundCompositeStatus_;
    diagnostics.backgroundParticipated = backgroundParticipatedInLastFrame_;
    diagnostics.frameTotalCpu =
        std::chrono::steady_clock::now() - frameStartedAt;
    return diagnostics;
}

PixelF CompositionRenderer::presentCompositionProbeColor(const PixelF color)
{
    if (!std::isfinite(color.red)
        || !std::isfinite(color.green)
        || !std::isfinite(color.blue)
        || !std::isfinite(color.alpha)
        || color.alpha < 0.0F
        || color.alpha > 1.0F)
    {
        throw std::invalid_argument(
            "Composition probe color requires finite RGB and Alpha in [0, 1]");
    }

    const std::array<float, 4> clearColor{
        color.red,
        color.green,
        color.blue,
        color.alpha};
    context_->ClearRenderTargetView(renderTarget_.Get(), clearColor.data());
    captureCenterPixel();
    presentSwapChain();
    if (!lastCenterPixel_.has_value())
    {
        throw std::runtime_error(
            "Composition probe could not read the pre-Present pixel");
    }
    return *lastCenterPixel_;
}

void CompositionRenderer::presentSwapChain()
{
    // Frame cadence is gated before rendering by the frame-latency waitable
    // object. A synchronous interval here can block the render owner behind
    // DWM/GPU back-pressure and prevent the same thread from dispatching
    // WM_INPUT, so composition receives the frame without an extra v-sync wait.
    const HRESULT result = swapChain_->Present(0, 0);
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
    {
        throwIfFailed(device_->GetDeviceRemovedReason(), "D3D11 device removed");
    }
    throwIfFailed(result, "IDXGISwapChain::Present");
}

bool CompositionRenderer::tryEnableBackgroundCapture(
    const HMONITOR monitor,
    const bool exclusionConfirmed,
    const bool cursorExcluded,
    const bool allowSystemBorder) noexcept
{
    setBackgroundCaptureFailure({});
    // Re-enabling capture replaces the producer and therefore starts a new
    // visible-batch decision, even when the monitor and options are unchanged.
    backgroundPathLatch_.reset();
    releaseBackgroundSnapshotResources();
    if (backgroundSensor_ != nullptr)
    {
        backgroundSensor_->stop();
        backgroundSensor_.reset();
    }
    backgroundCursorExcluded_ = cursorExcluded;
    backgroundSystemBorderAllowed_ = allowSystemBorder;
    backgroundCaptureRequested_ = exclusionConfirmed
        && monitor != nullptr
        && deviceInfo_.driverType == GraphicsDriverType::Hardware;
    backgroundMonitor_ = backgroundCaptureRequested_ ? monitor : nullptr;
    backgroundRefreshPeriod_ = bafx::core::MonotonicTime::zero();
    if (!backgroundCaptureRequested_)
    {
        if (!exclusionConfirmed)
        {
            setBackgroundCaptureFailure("capture exclusion was not confirmed");
        }
        else if (monitor == nullptr)
        {
            setBackgroundCaptureFailure("target monitor is unavailable");
        }
        else if (deviceInfo_.driverType != GraphicsDriverType::Hardware)
        {
            setBackgroundCaptureFailure("WGC requires a hardware D3D11 device");
        }
        return false;
    }

    return tryCreateBackgroundSensor();
}

std::optional<WindowSize>
CompositionRenderer::pendingBackgroundFramePoolSize() const noexcept
{
    if (backgroundSensor_ == nullptr)
    {
        return std::nullopt;
    }
    return backgroundSensor_->pendingFramePoolSize();
}

bool CompositionRenderer::tryRecreateBackgroundFramePool(
    const WindowSize size) noexcept
{
    if (backgroundSensor_ == nullptr)
    {
        setBackgroundCaptureFailure(
            "WGC frame pool recreate requires an active sensor");
        return false;
    }

    try
    {
        backgroundSensor_->recreateFramePool(size);
        setBackgroundCaptureFailure({});
        return true;
    }
    catch (...)
    {
        try
        {
            throw;
        }
        catch (const std::exception& error)
        {
            setBackgroundCaptureFailure(error.what());
        }
        catch (...)
        {
            setBackgroundCaptureFailure(
                "unknown WGC frame pool recreate failure");
        }
        return false;
    }
}

void CompositionRenderer::disableBackgroundCapture() noexcept
{
    // Disabling capture invalidates any latched Background-aware path before
    // the next FX-only frame is presented.
    backgroundPathLatch_.reset();
    releaseBackgroundSnapshotResources();
    backgroundCaptureRequested_ = false;
    backgroundMonitor_ = nullptr;
    backgroundSystemBorderAllowed_ = false;
    backgroundRefreshPeriod_ = bafx::core::MonotonicTime::zero();
    setBackgroundCaptureFailure({});
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

bool CompositionRenderer::backgroundCaptureBorderHidden() const noexcept
{
    return backgroundSensor_ != nullptr
        && backgroundSensor_->capabilities().borderHidden;
}

bool CompositionRenderer::backgroundCaptureCursorExcluded() const noexcept
{
    return backgroundSensor_ != nullptr
        && backgroundSensor_->capabilities().cursorExcluded;
}

WgcBackgroundResourceLedgerSnapshot
CompositionRenderer::backgroundResourceLedger() const noexcept
{
    return backgroundResourceLedger_ != nullptr
        ? backgroundResourceLedger_->snapshot()
        : WgcBackgroundResourceLedgerSnapshot{};
}

bool CompositionRenderer::backgroundParticipatedInLastFrame() const noexcept
{
    return backgroundParticipatedInLastFrame_;
}

BackgroundCompositeStatus CompositionRenderer::backgroundCompositeStatus() const noexcept
{
    return backgroundCompositeStatus_;
}

bool CompositionRenderer::tryCreateBackgroundSensor() noexcept
{
    if (!backgroundCaptureRequested_ || backgroundMonitor_ == nullptr)
    {
        if (backgroundCaptureRequested_ && backgroundMonitor_ == nullptr)
        {
            setBackgroundCaptureFailure("target monitor is unavailable");
        }
        return false;
    }
    if (!WgcBackgroundSensor::isSupported())
    {
        setBackgroundCaptureFailure("Windows Graphics Capture is not supported");
        return false;
    }

    if (!backgroundSystemBorderAllowed_)
    {
        const BorderlessCaptureAccessResult access =
            requestBorderlessCaptureAccess();
        if (!borderlessCaptureAccessAllowed(access))
        {
            setBackgroundCaptureFailure(borderlessCaptureAccessDiagnostic(access));
            return false;
        }
    }

    const std::optional<bafx::core::MonotonicTime> refreshPeriod =
        primaryRefreshPeriod();
    if (!refreshPeriod.has_value())
    {
        setBackgroundCaptureFailure("display refresh period could not be determined");
        return false;
    }

    try
    {
        setBackgroundCaptureFailure({});
        backgroundSensor_ = std::make_unique<WgcBackgroundSensor>(
            device_.Get(),
            backgroundMonitor_,
            WgcBackgroundSensorOptions{
                backgroundEpoch_,
                true,
                backgroundCursorExcluded_,
                backgroundSystemBorderAllowed_,
                backgroundResourceLedger_});
        backgroundEpoch_ = nextEpoch(backgroundEpoch_);
        // Capture and presentation have independent cadence. On high-refresh
        // displays WGC can still arrive near 60 Hz, so using a 170/240 Hz
        // present period directly would make normal jitter toggle transport.
        backgroundRefreshPeriod_ = std::max(
            *refreshPeriod,
            minimumBackgroundCadencePeriod);
        return true;
    }
    catch (...)
    {
        backgroundSensor_.reset();
        backgroundRefreshPeriod_ = bafx::core::MonotonicTime::zero();
        // Sensor construction can fail after allocating part of a session;
        // clear the latch so a later retry cannot inherit that partial state.
        backgroundPathLatch_.reset();
        resetBackgroundSnapshot();
        try
        {
            throw;
        }
        catch (const std::exception& error)
        {
            setBackgroundCaptureFailure(error.what());
        }
        catch (...)
        {
            setBackgroundCaptureFailure("unknown WGC initialization failure");
        }
        return false;
    }
}

std::string_view CompositionRenderer::backgroundCaptureFailure() const noexcept
{
    return std::string_view(
        backgroundCaptureFailure_.data(),
        backgroundCaptureFailureLength_);
}

void CompositionRenderer::setDeviceRecoveryFailure(
    const std::string_view message) noexcept
{
    const std::size_t length = std::min(
        message.size(),
        deviceRecoveryFailure_.size());
    if (length > 0U)
    {
        std::copy_n(
            message.data(),
            length,
            deviceRecoveryFailure_.data());
    }
    deviceRecoveryFailureLength_ = length;
}

void CompositionRenderer::setBackgroundCaptureFailure(
    const std::string_view message) noexcept
{
    const std::size_t length = std::min(
        message.size(),
        backgroundCaptureFailure_.size());
    if (length > 0U)
    {
        std::copy_n(
            message.data(),
            length,
            backgroundCaptureFailure_.data());
    }
    backgroundCaptureFailureLength_ = length;
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

HANDLE CompositionRenderer::backgroundFrameAvailableObject() const noexcept
{
    return backgroundSensor_ != nullptr
        ? backgroundSensor_->frameAvailableObject()
        : nullptr;
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
    // A recovery attempt may follow a WARP fallback; reset the diagnostic
    // default before probing hardware again so stale driver labels do not
    // survive a successful rebuild.
    deviceInfo_.driverType = GraphicsDriverType::Hardware;
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

void CompositionRenderer::resetBackgroundSnapshot() noexcept
{
    // Invalidate the batch without releasing the monitor-sized allocations;
    // repeated clicks should seed the existing ping-pong pair instead of
    // stalling D3D11 on two fresh full-screen textures every time.
    backgroundSnapshotGeneration_ = 0U;
    backgroundSnapshotValid_ = false;
}

void CompositionRenderer::releaseBackgroundSnapshotResources() noexcept
{
    backgroundSnapshotShaderResource_.Reset();
    backgroundSnapshotRenderTarget_.Reset();
    backgroundSnapshotTexture_.Reset();
    backgroundCandidateShaderResource_.Reset();
    backgroundCandidateRenderTarget_.Reset();
    backgroundCandidateTexture_.Reset();
    backgroundSnapshotSize_ = WindowSize{};
    backgroundSnapshotGeneration_ = 0U;
    backgroundSnapshotValid_ = false;
}

bool CompositionRenderer::captureBackgroundSnapshot(
    ID3D11ShaderResourceView* const source) noexcept
{
    if (source == nullptr || device_ == nullptr || context_ == nullptr)
    {
        return false;
    }

    try
    {
        Microsoft::WRL::ComPtr<ID3D11Resource> sourceResource;
        source->GetResource(&sourceResource);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> sourceTexture;
        throwIfFailed(
            sourceResource.As(&sourceTexture),
            "WGC background snapshot source texture");

        D3D11_TEXTURE2D_DESC sourceDescription{};
        sourceTexture->GetDesc(&sourceDescription);
        if (sourceDescription.Width != size_.width
            || sourceDescription.Height != size_.height
            || sourceDescription.MipLevels != 1U
            || sourceDescription.ArraySize != 1U
            || sourceDescription.Format != DXGI_FORMAT_R16G16B16A16_FLOAT
            || sourceDescription.SampleDesc.Count != 1U
            || sourceDescription.SampleDesc.Quality != 0U)
        {
            return false;
        }

        if (backgroundSnapshotTexture_ == nullptr
            || backgroundCandidateTexture_ == nullptr
            || backgroundSnapshotSize_.width != size_.width
            || backgroundSnapshotSize_.height != size_.height)
        {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> replacementTexture;
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> replacementRenderTarget;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> replacementShaderResource;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> replacementCandidateTexture;
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> replacementCandidateRenderTarget;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> replacementCandidateShaderResource;
            D3D11_TEXTURE2D_DESC destinationDescription{};
            destinationDescription.Width = size_.width;
            destinationDescription.Height = size_.height;
            destinationDescription.MipLevels = 1U;
            destinationDescription.ArraySize = 1U;
            destinationDescription.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            destinationDescription.SampleDesc = DXGI_SAMPLE_DESC{1U, 0U};
            destinationDescription.Usage = D3D11_USAGE_DEFAULT;
            destinationDescription.BindFlags = D3D11_BIND_RENDER_TARGET
                | D3D11_BIND_SHADER_RESOURCE;
            throwIfFailed(
                device_->CreateTexture2D(
                    &destinationDescription,
                    nullptr,
                    &replacementTexture),
                "ID3D11Device::CreateTexture2D(WGC background snapshot)");
            throwIfFailed(
                device_->CreateShaderResourceView(
                    replacementTexture.Get(),
                    nullptr,
                    &replacementShaderResource),
                "ID3D11Device::CreateShaderResourceView(WGC background snapshot)");
            throwIfFailed(
                device_->CreateRenderTargetView(
                    replacementTexture.Get(),
                    nullptr,
                    &replacementRenderTarget),
                "ID3D11Device::CreateRenderTargetView(WGC background snapshot)");
            throwIfFailed(
                device_->CreateTexture2D(
                    &destinationDescription,
                    nullptr,
                    &replacementCandidateTexture),
                "ID3D11Device::CreateTexture2D(WGC background candidate)");
            throwIfFailed(
                device_->CreateShaderResourceView(
                    replacementCandidateTexture.Get(),
                    nullptr,
                    &replacementCandidateShaderResource),
                "ID3D11Device::CreateShaderResourceView(WGC background candidate)");
            throwIfFailed(
                device_->CreateRenderTargetView(
                    replacementCandidateTexture.Get(),
                    nullptr,
                    &replacementCandidateRenderTarget),
                "ID3D11Device::CreateRenderTargetView(WGC background candidate)");
            backgroundSnapshotTexture_ = std::move(replacementTexture);
            backgroundSnapshotRenderTarget_ = std::move(replacementRenderTarget);
            backgroundSnapshotShaderResource_ =
                std::move(replacementShaderResource);
            backgroundCandidateTexture_ = std::move(replacementCandidateTexture);
            backgroundCandidateRenderTarget_ =
                std::move(replacementCandidateRenderTarget);
            backgroundCandidateShaderResource_ =
                std::move(replacementCandidateShaderResource);
            backgroundSnapshotSize_ = size_;
            backgroundSnapshotValid_ = false;
        }

        if (!backgroundSnapshotValid_)
        {
            context_->CopyResource(
                backgroundSnapshotTexture_.Get(),
                sourceTexture.Get());
            return true;
        }

        // Only the accepted WGC generation reaches this pass. Keeping the
        // previous and candidate textures separate avoids an SRV/RTV hazard
        // and makes Differential Bloom and Final read one coherent result.
        fxRenderer_->stabilizeBackgroundFrame(
            backgroundSnapshotShaderResource_.Get(),
            source,
            backgroundCandidateRenderTarget_.Get());
        std::swap(backgroundSnapshotTexture_, backgroundCandidateTexture_);
        std::swap(
            backgroundSnapshotRenderTarget_,
            backgroundCandidateRenderTarget_);
        std::swap(
            backgroundSnapshotShaderResource_,
            backgroundCandidateShaderResource_);
        return true;
    }
    catch (...)
    {
        // A failed refresh must not destroy the last coherent background.
        // Session, epoch and resize failures explicitly reset it at the caller.
        return false;
    }
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
