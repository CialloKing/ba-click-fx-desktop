#include "bafx/windows/composition_renderer.hpp"

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
constexpr CompositionOutputState scRgbOutputState{
    DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
    CompositionOutputTransfer::LinearScRgb,
    CompositionOutputFallback::None,
    true};
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
refreshPeriod(const DisplayRefreshRate& refreshRate) noexcept
{
    const double seconds = static_cast<double>(refreshRate.denominator)
        / static_cast<double>(refreshRate.numerator);
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

[[nodiscard]] std::optional<bafx::core::MonotonicTime>
displayRefreshPeriod(const HMONITOR monitor) noexcept
{
    const std::optional<DisplayRefreshRate> refreshRate =
        queryDisplayRefreshRate(monitor);
    return refreshRate.has_value()
        ? refreshPeriod(*refreshRate)
        : std::nullopt;
}

[[nodiscard]] bafx::core::MonotonicTime resolveMonotonicTime(
    const bafx::core::MonotonicTime supplied) noexcept
{
    if (supplied != bafx::core::MonotonicTime::zero())
    {
        return supplied;
    }

    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceCounter(&counter)
        || !QueryPerformanceFrequency(&frequency)
        || frequency.QuadPart <= 0)
    {
        return supplied;
    }

    const auto seconds = counter.QuadPart / frequency.QuadPart;
    const auto remainder = counter.QuadPart % frequency.QuadPart;
    // Keep diagnostic callers on the same monotonic domain as the Host even
    // when they use the legacy zero-time overload.
    return std::chrono::duration_cast<bafx::core::MonotonicTime>(
        std::chrono::seconds(seconds)
        + std::chrono::nanoseconds(
            remainder * 1'000'000'000LL / frequency.QuadPart));
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
    const FxBloomSettings bloomSettings,
    const WgcBackgroundStopObserver backgroundStopObserver,
    const std::optional<LUID> requestedAdapterLuid)
    : window_(window)
    , bloomSettings_(bloomSettings)
    , size_(size)
    , backgroundResourceLedger_(
          std::make_shared<WgcBackgroundResourceLedger>())
    , backgroundStopObserver_(backgroundStopObserver)
    , requestedAdapterLuid_(requestedAdapterLuid)
{
    createDeviceResources();
}

CompositionRenderer::~CompositionRenderer()
{
    // Preserve the original member-destruction order: WGC retains the device
    // and must release its capture objects before the renderer drops D3D.
    stopBackgroundSensor();
    releaseDeviceResources();
}

bool CompositionRenderer::tryRecoverDevice() noexcept
{
    setDeviceRecoveryFailure({});
    deviceRecoveryDiagnostics_ = DeviceRecoveryDiagnostics{};
    if (deviceRecoveryAttempted_)
    {
        setDeviceRecoveryFailure("device recovery budget exhausted");
        return false;
    }
    deviceRecoveryAttempted_ = true;
    const auto recoveryStartedAt = std::chrono::steady_clock::now();
    GraphicsDeviceInfo previousDeviceInfo{};
    try
    {
        previousDeviceInfo = deviceInfo_;
        // WGC textures and the temporal snapshot belong to the old device;
        // invalidate them before releasing the swap-chain resource domain.
        const auto backgroundStopStartedAt = std::chrono::steady_clock::now();
        resetBackgroundSnapshot(
            BackgroundSnapshotInvalidationReason::DeviceResourcesReleased,
            0U);
        disableBackgroundCapture();
        deviceRecoveryDiagnostics_.backgroundStop =
            std::chrono::steady_clock::now() - backgroundStopStartedAt;
        previousVisualBounds_.reset();
        lastCenterPixel_.reset();
        backgroundCompositeStatus_ = BackgroundCompositeStatus::Inactive;
        releaseDeviceResources();
        deviceInfo_ = GraphicsDeviceInfo{};
        featureLevel_ = D3D_FEATURE_LEVEL_11_0;

        createDeviceResources();
        backgroundCaptureAfterRecoveryAllowed_ =
            previousDeviceInfo.adapterLuid.LowPart
                == deviceInfo_.adapterLuid.LowPart
            && previousDeviceInfo.adapterLuid.HighPart
                == deviceInfo_.adapterLuid.HighPart
            && deviceInfo_.driverType == GraphicsDriverType::Hardware;
        deviceRecoveryDiagnostics_.total =
            std::chrono::steady_clock::now() - recoveryStartedAt;
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
        deviceInfo_ = std::move(previousDeviceInfo);
        // Preserve the failed adapter context, but never describe the released
        // swap chain as an active output transport.
        deviceInfo_.output = CompositionOutputState{};
        deviceRecoveryDiagnostics_.total =
            std::chrono::steady_clock::now() - recoveryStartedAt;
        return false;
    }
}

OutputAdapterRetargetStatus CompositionRenderer::retargetOutputAdapter(
    const std::optional<LUID> requestedAdapterLuid)
{
    const auto sameRequestedAdapter = [](const std::optional<LUID>& left,
                                         const std::optional<LUID>& right)
    {
        if (left.has_value() != right.has_value())
        {
            return false;
        }
        return !left.has_value()
            || (left->HighPart == right->HighPart
                && left->LowPart == right->LowPart);
    };
    if (sameRequestedAdapter(requestedAdapterLuid_, requestedAdapterLuid))
    {
        return OutputAdapterRetargetStatus::Unchanged;
    }
    if (backgroundSensor_ != nullptr || backgroundCaptureRequested_)
    {
        throw std::logic_error(
            "Output adapter retarget requires a completed capture-stop transaction");
    }

    const std::optional<LUID> previousRequestedAdapter =
        requestedAdapterLuid_;
    const bool previousBackgroundRestartAllowed =
        backgroundCaptureAfterRecoveryAllowed_;
    const bool previousDeviceRecoveryAttempted = deviceRecoveryAttempted_;

    previousVisualBounds_.reset();
    lastCenterPixel_.reset();
    backgroundPathLatch_.reset();
    backgroundCompositeStatus_ = BackgroundCompositeStatus::Inactive;
    releaseDeviceResources();
    requestedAdapterLuid_ = requestedAdapterLuid;
    deviceInfo_ = GraphicsDeviceInfo{};
    featureLevel_ = D3D_FEATURE_LEVEL_11_0;
    try
    {
        createDeviceResources();
        deviceRecoveryAttempted_ = false;
        backgroundCaptureAfterRecoveryAllowed_ =
            deviceInfo_.driverType == GraphicsDriverType::Hardware
            && deviceInfo_.requestedAdapterMatched;
        return deviceInfo_.driverType == GraphicsDriverType::Hardware
            ? OutputAdapterRetargetStatus::RecreatedHardware
            : OutputAdapterRetargetStatus::RecreatedWarpFallback;
    }
    catch (...)
    {
        const std::exception_ptr retargetFailure = std::current_exception();
        // Preserve a usable old resource domain when a newly attached adapter
        // disappears between topology enumeration and device creation.
        releaseDeviceResources();
        requestedAdapterLuid_ = previousRequestedAdapter;
        deviceInfo_ = GraphicsDeviceInfo{};
        featureLevel_ = D3D_FEATURE_LEVEL_11_0;
        createDeviceResources();
        backgroundCaptureAfterRecoveryAllowed_ =
            previousBackgroundRestartAllowed;
        deviceRecoveryAttempted_ = previousDeviceRecoveryAttempted;
        std::rethrow_exception(retargetFailure);
    }
}

std::string_view CompositionRenderer::deviceRecoveryFailure() const noexcept
{
    return std::string_view(
        deviceRecoveryFailure_.data(),
        deviceRecoveryFailureLength_);
}

DeviceRecoveryDiagnostics
CompositionRenderer::deviceRecoveryDiagnostics() const noexcept
{
    return deviceRecoveryDiagnostics_;
}

OutputResizeStatus CompositionRenderer::resizeOutput(const WindowSize size)
{
    if (size.width == 0U || size.height == 0U
        || (size.width == size_.width && size.height == size_.height))
    {
        return OutputResizeStatus::Unchanged;
    }

    if (backgroundSensor_ != nullptr || backgroundCaptureRequested_)
    {
        throw std::logic_error(
            "Output resize requires a completed capture-stop transaction");
    }

    bool recovered = false;
    for (;;)
    {
        if (size.width == size_.width && size.height == size_.height)
        {
            return recovered
                ? OutputResizeStatus::DeviceRecovered
                : OutputResizeStatus::Unchanged;
        }
        try
        {
            // A resized swap chain starts a new capture contract. Do not carry
            // the previous visible batch's path into the new session while its
            // first correctly sized WGC frame is still pending.
            backgroundPathLatch_.reset();
            releaseBackgroundSnapshotResources(
                BackgroundSnapshotInvalidationReason::OutputResize);
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
            return recovered
                ? OutputResizeStatus::DeviceRecovered
                : OutputResizeStatus::Resized;
        }
        catch (const HResultError& error)
        {
            if (recovered || !isDeviceLostResult(error.result()))
            {
                throw;
            }
            recovered = true;
            if (!tryRecoverDevice())
            {
                throw;
            }
        }
    }
}

bool CompositionRenderer::setBloomSettings(const FxBloomSettings settings)
{
    bloomSettings_ = settings;
    try
    {
        fxRenderer_->setBloomSettings(settings);
        return false;
    }
    catch (const HResultError& error)
    {
        if (!isDeviceLostResult(error.result()))
        {
            throw;
        }
        if (!tryRecoverDevice())
        {
            throw;
        }
        // tryRecoverDevice constructs the replacement renderer from the
        // already-updated bloomSettings_ value.
        return true;
    }
}

void CompositionRenderer::releaseDeviceResources() noexcept
{
    unregisterDeviceRemovedNotification();
    if (context_ != nullptr)
    {
        context_->OMSetRenderTargets(0U, nullptr, nullptr);
    }
    // Snapshot views are created on the same device as the swap chain. Keep
    // this helper self-contained so an exceptional recovery path cannot leave
    // old-device views alive behind the new resource domain.
    releaseBackgroundSnapshotResources(
        BackgroundSnapshotInvalidationReason::DeviceResourcesReleased);
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
    deviceInfo_.output = CompositionOutputState{};
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
        resetBackgroundSnapshot(
            BackgroundSnapshotInvalidationReason::VisibleBatchEnded,
            diagnostics.frameId);
    }
    backgroundParticipatedInLastFrame_ = false;
    backgroundCompositeStatus_ = backgroundSensor_ != nullptr
        ? BackgroundCompositeStatus::WaitingForFrame
        : BackgroundCompositeStatus::Inactive;
    const bafx::core::MonotonicTime effectiveWallTime =
        resolveMonotonicTime(wallTime);
    const BackgroundSensorMaintenanceDiagnostics maintenance =
        drainBackgroundSensor(
            hasDrawableContent,
            effectiveWallTime,
            diagnostics.frameId);
    diagnostics.wgc = maintenance.wgc;
    diagnostics.wgcDrainInclusiveCpu = maintenance.wgcDrainInclusiveCpu;
    diagnostics.wgcActive = maintenance.wgcActive;
    diagnostics.wgcDrainAttempted = maintenance.wgcDrainAttempted;
    diagnostics.wgcIdleDrainAttempted = maintenance.wgcIdleDrainAttempted;
    diagnostics.wgcIdleDrainSkipped = maintenance.wgcIdleDrainSkipped;
    const bool drainCanFeedVisibleFrame = maintenance.wgc.status
            == WgcBackgroundDrainStatus::NoFrame
        || maintenance.wgc.status == WgcBackgroundDrainStatus::Updated;
    if (backgroundSensor_ != nullptr
        && hasDrawableContent
        && maintenance.wgcDrainAttempted
        && drainCanFeedVisibleFrame)
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
                backgroundSample->texture,
                diagnostics.frameId);
            diagnostics.backgroundSnapshotSubmitCpu =
                std::chrono::steady_clock::now() - snapshotStartedAt;
        }
        if (snapshotRefreshed)
        {
            backgroundSnapshotValid_ = true;
            backgroundSnapshotEpoch_ = backgroundSample->stamp.epoch;
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
        resetBackgroundSnapshot(
            BackgroundSnapshotInvalidationReason::FxOnlyPathSelected,
            diagnostics.frameId);
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
    if (diagnostics.backgroundParticipated)
    {
        diagnostics.backgroundSnapshotEpoch = backgroundSnapshotEpoch_;
        diagnostics.backgroundSnapshotGeneration =
            backgroundSnapshotGeneration_;
    }
    diagnostics.frameTotalCpu =
        std::chrono::steady_clock::now() - frameStartedAt;
    return diagnostics;
}

BackgroundSensorMaintenanceDiagnostics
CompositionRenderer::serviceBackgroundCapture(
    const bafx::core::MonotonicTime wallTime) noexcept
{
    return drainBackgroundSensor(
        false,
        resolveMonotonicTime(wallTime),
        0U);
}

BackgroundSensorMaintenanceDiagnostics
CompositionRenderer::drainBackgroundSensor(
    const bool hasDrawableContent,
    const bafx::core::MonotonicTime wallTime,
    const std::uint64_t frameId) noexcept
{
    BackgroundSensorMaintenanceDiagnostics diagnostics{};
    if (backgroundSensor_ == nullptr)
    {
        return diagnostics;
    }

    diagnostics.wgcActive = true;
    const WgcBackgroundTransportSnapshot transport =
        backgroundSensor_->transportSnapshot();
    diagnostics.wgc.epoch = transport.epoch;
    diagnostics.wgc.frameArrivedCallbacksTotal =
        transport.frameArrivedCallbacksTotal;
    diagnostics.wgc.acceptedGeneration = transport.acceptedGeneration;
    const detail::WgcDrainPolicyDecision drainDecision =
        detail::decideWgcDrain(
            hasDrawableContent,
            transport.epoch,
            wallTime,
            wgcDrainPolicyState_);
    wgcDrainPolicyState_ = drainDecision.nextState;
    diagnostics.wgcIdleDrainAttempted = drainDecision.action
        == detail::WgcDrainPolicyAction::IdleAttempt;
    diagnostics.wgcIdleDrainSkipped = drainDecision.action
        == detail::WgcDrainPolicyAction::IdleThrottled;
    if (diagnostics.wgcIdleDrainSkipped)
    {
        return diagnostics;
    }

    diagnostics.wgcDrainAttempted = true;
    const auto stopFailedBackgroundCapture =
        [this, frameId](const std::string_view failure) noexcept
    {
        setBackgroundCaptureFailure(failure);
        // The producer is gone before the control transaction observes it.
        // Marking the request inactive keeps same-turn resize transactional.
        backgroundCaptureRequested_ = false;
        backgroundRefreshPeriod_ = bafx::core::MonotonicTime::zero();
        backgroundCompositeStatus_ = BackgroundCompositeStatus::CaptureFailed;
        backgroundPathLatch_.reset();
        // Capture the producer identity before stop releases its diagnostics.
        resetBackgroundSnapshot(
            BackgroundSnapshotInvalidationReason::WgcDrainFailed,
            frameId);
        stopBackgroundSensor();
    };
    try
    {
        const auto drainStartedAt = std::chrono::steady_clock::now();
        diagnostics.wgc = backgroundSensor_->drainLatestDetailed(
            context_.Get());
        diagnostics.wgcDrainInclusiveCpu =
            std::chrono::steady_clock::now() - drainStartedAt;
        if (diagnostics.wgc.status == WgcBackgroundDrainStatus::Stopped)
        {
            // item.Closed is terminal even when observed between presentations.
            backgroundCaptureRequested_ = false;
            backgroundRefreshPeriod_ = bafx::core::MonotonicTime::zero();
            backgroundCompositeStatus_ = BackgroundCompositeStatus::CaptureFailed;
            backgroundPathLatch_.reset();
            resetBackgroundSnapshot(
                BackgroundSnapshotInvalidationReason::WgcSessionStopped,
                frameId);
            stopBackgroundSensor();
        }
        else if (diagnostics.wgc.status
            == WgcBackgroundDrainStatus::ReconfigureRequired)
        {
            // The owner performs Recreate in its explicit lifecycle transaction.
            backgroundCompositeStatus_ = BackgroundCompositeStatus::WaitingForFrame;
            backgroundPathLatch_.reset();
            resetBackgroundSnapshot(
                BackgroundSnapshotInvalidationReason::
                    FramePoolReconfigureRequired,
                frameId);
        }
    }
    catch (const std::exception& error)
    {
        // WGC is optional. Preserve the FX-only interaction path and expose the
        // original failure to the owner before releasing the failed session.
        stopFailedBackgroundCapture(error.what());
    }
    catch (...)
    {
        stopFailedBackgroundCapture("unknown WGC drain failure");
    }
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
    const bool allowSystemBorder,
    const bool borderlessAccessConfirmed) noexcept
{
    setBackgroundCaptureFailure({});
    // Re-enabling capture replaces the producer and therefore starts a new
    // visible-batch decision, even when the monitor and options are unchanged.
    backgroundPathLatch_.reset();
    releaseBackgroundSnapshotResources(
        BackgroundSnapshotInvalidationReason::CaptureSessionReplaced);
    stopBackgroundSensor();
    backgroundCursorExcluded_ = cursorExcluded;
    backgroundSystemBorderAllowed_ = allowSystemBorder;
    backgroundCaptureRequested_ = exclusionConfirmed
        && monitor != nullptr
        && deviceInfo_.driverType == GraphicsDriverType::Hardware
        && backgroundStopMailbox_.restartAllowed()
        && backgroundCaptureAfterRecoveryAllowed_
        && (allowSystemBorder || borderlessAccessConfirmed);
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
        else if (!backgroundStopMailbox_.restartAllowed())
        {
            setBackgroundCaptureFailure(
                "WGC restart requires a process restart after capture stop failure");
        }
        else if (!backgroundCaptureAfterRecoveryAllowed_)
        {
            setBackgroundCaptureFailure(
                "WGC restart requires a process restart after graphics adapter change");
        }
        else if (!allowSystemBorder && !borderlessAccessConfirmed)
        {
            // Permission is an owner-thread preflight action. Sensor creation
            // must never synchronously invoke the broker as a hidden fallback.
            setBackgroundCaptureFailure(
                "borderless capture access was not confirmed");
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

BackgroundFramePoolRecreateStatus
CompositionRenderer::tryRecreateBackgroundFramePool(
    const WindowSize size) noexcept
{
    if (backgroundSensor_ == nullptr)
    {
        setBackgroundCaptureFailure(
            "WGC frame pool recreate requires an active sensor");
        return BackgroundFramePoolRecreateStatus::Failed;
    }

    try
    {
        backgroundSensor_->recreateFramePool(size);
        setBackgroundCaptureFailure({});
        return BackgroundFramePoolRecreateStatus::Recreated;
    }
    catch (const HResultError& error)
    {
        if (!isDeviceLostResult(error.result()))
        {
            setBackgroundCaptureFailure(error.what());
            return BackgroundFramePoolRecreateStatus::Failed;
        }
        const bool recovered = tryRecoverDevice();
        setBackgroundCaptureFailure(error.what());
        return recovered
            ? BackgroundFramePoolRecreateStatus::DeviceRecovered
            : BackgroundFramePoolRecreateStatus::DeviceRecoveryFailed;
    }
    catch (const std::exception& error)
    {
        setBackgroundCaptureFailure(error.what());
        return BackgroundFramePoolRecreateStatus::Failed;
    }
    catch (...)
    {
        setBackgroundCaptureFailure(
            "unknown WGC frame pool recreate failure");
        return BackgroundFramePoolRecreateStatus::Failed;
    }
}

void CompositionRenderer::disableBackgroundCapture() noexcept
{
    // Disabling capture invalidates any latched Background-aware path before
    // the next FX-only frame is presented.
    backgroundPathLatch_.reset();
    releaseBackgroundSnapshotResources(
        BackgroundSnapshotInvalidationReason::CaptureDisabled);
    backgroundCaptureRequested_ = false;
    backgroundMonitor_ = nullptr;
    backgroundSystemBorderAllowed_ = false;
    backgroundRefreshPeriod_ = bafx::core::MonotonicTime::zero();
    setBackgroundCaptureFailure({});
    stopBackgroundSensor();
}

bool CompositionRenderer::backgroundCaptureActive() const noexcept
{
    return backgroundSensor_ != nullptr && backgroundSensor_->running();
}

bool CompositionRenderer::backgroundCaptureRestartAllowed() const noexcept
{
    return backgroundStopMailbox_.restartAllowed()
        && backgroundCaptureAfterRecoveryAllowed_;
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

BackgroundCadenceRefreshResult CompositionRenderer::refreshBackgroundCadence(
    const HMONITOR monitor) noexcept
{
    if (!backgroundCaptureRequested_ || backgroundSensor_ == nullptr)
    {
        return BackgroundCadenceRefreshResult{
            BackgroundCadenceRefreshStatus::Inactive,
            std::nullopt,
            backgroundRefreshPeriod_};
    }
    if (monitor == nullptr || monitor != backgroundMonitor_)
    {
        return BackgroundCadenceRefreshResult{
            BackgroundCadenceRefreshStatus::WrongMonitor,
            std::nullopt,
            backgroundRefreshPeriod_};
    }

    const std::optional<DisplayRefreshRate> refreshRate =
        queryDisplayRefreshRate(monitor);
    const std::optional<bafx::core::MonotonicTime> targetPeriod =
        refreshRate.has_value()
            ? refreshPeriod(*refreshRate)
            : std::nullopt;
    backgroundRefreshPeriod_ = targetPeriod.has_value()
        ? std::max(*targetPeriod, minimumBackgroundCadencePeriod)
        : minimumBackgroundCadencePeriod;
    return BackgroundCadenceRefreshResult{
        targetPeriod.has_value()
            ? BackgroundCadenceRefreshStatus::TargetRate
            : BackgroundCadenceRefreshStatus::ConservativeFallback,
        refreshRate,
        backgroundRefreshPeriod_};
}

WgcBackgroundResourceLedgerSnapshot
CompositionRenderer::backgroundResourceLedger() const noexcept
{
    return backgroundResourceLedger_ != nullptr
        ? backgroundResourceLedger_->snapshot()
        : WgcBackgroundResourceLedgerSnapshot{};
}

WgcBackgroundStopDiagnostics
CompositionRenderer::takeBackgroundStopDiagnostics() noexcept
{
    return backgroundStopMailbox_.take();
}

std::optional<BackgroundSnapshotInvalidation>
CompositionRenderer::takeBackgroundSnapshotInvalidation() noexcept
{
    return backgroundSnapshotInvalidationMailbox_.take();
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

    const std::optional<bafx::core::MonotonicTime> refreshPeriod =
        displayRefreshPeriod(backgroundMonitor_);

    try
    {
        setBackgroundCaptureFailure({});
        WgcBackgroundSensorOptions sensorOptions{};
        sensorOptions.epoch = backgroundEpoch_;
        sensorOptions.excludesOwnOverlay = true;
        sensorOptions.cursorExcluded = backgroundCursorExcluded_;
        sensorOptions.allowSystemBorder = backgroundSystemBorderAllowed_;
        sensorOptions.resourceLedger = backgroundResourceLedger_;
        sensorOptions.stopObserver = backgroundStopObserver_;
        sensorOptions.stopResultObserver =
            backgroundStopMailbox_.resultObserver();
        backgroundSensor_ = std::make_unique<WgcBackgroundSensor>(
            device_.Get(),
            backgroundMonitor_,
            sensorOptions);
        backgroundEpoch_ = nextEpoch(backgroundEpoch_);
        // Capture and presentation have independent cadence. On high-refresh
        // displays WGC can still arrive near 60 Hz, so using a 170/240 Hz
        // present period directly would make normal jitter toggle transport.
        // Unknown and mixed clone cadence uses a conservative 60 Hz freshness
        // budget. A diagnostic uncertainty must not disable an otherwise valid
        // capture session on a secondary display.
        backgroundRefreshPeriod_ = refreshPeriod.has_value()
            ? std::max(*refreshPeriod, minimumBackgroundCadencePeriod)
            : minimumBackgroundCadencePeriod;
        return true;
    }
    catch (...)
    {
        backgroundSensor_.reset();
        backgroundRefreshPeriod_ = bafx::core::MonotonicTime::zero();
        // Sensor construction can fail after allocating part of a session;
        // clear the latch so a later retry cannot inherit that partial state.
        backgroundPathLatch_.reset();
        resetBackgroundSnapshot(
            BackgroundSnapshotInvalidationReason::SensorStartFailed,
            0U);
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

HANDLE CompositionRenderer::deviceRemovedWaitableObject() const noexcept
{
    return deviceRemovedNotificationRegistered_
        ? deviceRemovedHandle_.get()
        : nullptr;
}

HRESULT CompositionRenderer::deviceRemovedNotificationResult() const noexcept
{
    return deviceRemovedNotificationResult_;
}

HRESULT CompositionRenderer::deviceRemovedReason() const noexcept
{
    return device_ != nullptr
        ? device_->GetDeviceRemovedReason()
        : E_POINTER;
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

const CompositionOutputState& CompositionRenderer::outputState() const noexcept
{
    return deviceInfo_.output;
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
    deviceInfo_.requestedAdapterLuid = requestedAdapterLuid_;
    deviceInfo_.requestedAdapterFound = !requestedAdapterLuid_.has_value();
    deviceInfo_.requestedAdapterMatched = false;

    Microsoft::WRL::ComPtr<IDXGIAdapter1> requestedAdapter;
    if (requestedAdapterLuid_.has_value())
    {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        throwIfFailed(
            CreateDXGIFactory1(IID_PPV_ARGS(&factory)),
            "CreateDXGIFactory1(target adapter)");
        for (UINT index = 0U;; ++index)
        {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            const HRESULT result = factory->EnumAdapters1(index, &adapter);
            if (result == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            throwIfFailed(result, "IDXGIFactory1::EnumAdapters1(target adapter)");

            DXGI_ADAPTER_DESC1 description{};
            throwIfFailed(
                adapter->GetDesc1(&description),
                "IDXGIAdapter1::GetDesc1(target adapter)");
            if (description.AdapterLuid.HighPart
                    == requestedAdapterLuid_->HighPart
                && description.AdapterLuid.LowPart
                    == requestedAdapterLuid_->LowPart)
            {
                requestedAdapter = std::move(adapter);
                deviceInfo_.requestedAdapterFound = true;
                break;
            }
        }
    }

    const auto create = [this, &featureLevels](
                             IDXGIAdapter* const adapter,
                             const D3D_DRIVER_TYPE driver,
                             const UINT flags)
    {
        device_.Reset();
        context_.Reset();
        return D3D11CreateDevice(
            adapter,
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

    const auto createHardware = [&](const UINT flags)
    {
        if (requestedAdapterLuid_.has_value()
            && requestedAdapter == nullptr)
        {
            return DXGI_ERROR_NOT_FOUND;
        }
        return create(
            requestedAdapter.Get(),
            requestedAdapterLuid_.has_value()
                ? D3D_DRIVER_TYPE_UNKNOWN
                : D3D_DRIVER_TYPE_HARDWARE,
            flags);
    };

#if defined(_DEBUG)
    HRESULT result = createHardware(baseFlags | D3D11_CREATE_DEVICE_DEBUG);
    if (result == DXGI_ERROR_SDK_COMPONENT_MISSING)
    {
        // End-user machines often omit Graphics Tools; diagnostics must remain optional.
        result = createHardware(baseFlags);
    }
#else
    HRESULT result = createHardware(baseFlags);
#endif
    const HRESULT hardwareCreateResult = result;
    if (FAILED(result))
    {
        // WARP keeps the FX-only path usable when a hardware device cannot be created.
        result = create(nullptr, D3D_DRIVER_TYPE_WARP, baseFlags);
        deviceInfo_.driverType = GraphicsDriverType::Warp;
    }
    throwIfFailed(result, "D3D11CreateDevice");
    deviceInfo_.hardwareCreateResult = hardwareCreateResult;
    deviceInfo_.featureLevel = featureLevel_;
    collectDeviceInfo();
    deviceInfo_.requestedAdapterMatched =
        !requestedAdapterLuid_.has_value()
        || (deviceInfo_.driverType == GraphicsDriverType::Hardware
            && deviceInfo_.adapterLuid.HighPart
                == requestedAdapterLuid_->HighPart
            && deviceInfo_.adapterLuid.LowPart
                == requestedAdapterLuid_->LowPart);
}

void CompositionRenderer::createDeviceResources()
{
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
    registerDeviceRemovedNotification();
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
    description.Format = scRgbOutputState.format;
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
        swapChain_->CheckColorSpaceSupport(
            scRgbOutputState.colorSpace,
            &colorSpaceSupport),
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
        swapChain_->SetColorSpace1(scRgbOutputState.colorSpace),
        "IDXGISwapChain3::SetColorSpace1(scRGB)");
    throwIfFailed(swapChain_->SetMaximumFrameLatency(1), "IDXGISwapChain2::SetMaximumFrameLatency");

    frameLatencyHandle_.reset(swapChain_->GetFrameLatencyWaitableObject());
    if (frameLatencyHandle_.get() == nullptr)
    {
        throwLastError("IDXGISwapChain2::GetFrameLatencyWaitableObject");
    }

    // Publish only a fully configured swap chain. A later SDR fallback can use
    // the same boundary without exposing a format whose shader contract failed.
    deviceInfo_.output = scRgbOutputState;
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

void CompositionRenderer::registerDeviceRemovedNotification() noexcept
{
    unregisterDeviceRemovedNotification();
    deviceRemovedNotificationResult_ = device_.As(&device4_);
    if (FAILED(deviceRemovedNotificationResult_))
    {
        return;
    }

    deviceRemovedHandle_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (deviceRemovedHandle_.get() == nullptr)
    {
        deviceRemovedNotificationResult_ = HRESULT_FROM_WIN32(GetLastError());
        device4_.Reset();
        return;
    }

    DWORD cookie = 0U;
    deviceRemovedNotificationResult_ = device4_->RegisterDeviceRemovedEvent(
        deviceRemovedHandle_.get(),
        &cookie);
    if (FAILED(deviceRemovedNotificationResult_))
    {
        deviceRemovedHandle_.reset();
        device4_.Reset();
        return;
    }
    deviceRemovedCookie_ = cookie;
    deviceRemovedNotificationRegistered_ = true;
}

void CompositionRenderer::unregisterDeviceRemovedNotification() noexcept
{
    if (deviceRemovedNotificationRegistered_ && device4_ != nullptr)
    {
        device4_->UnregisterDeviceRemoved(deviceRemovedCookie_);
    }
    deviceRemovedNotificationRegistered_ = false;
    deviceRemovedCookie_ = 0U;
    deviceRemovedHandle_.reset();
    device4_.Reset();
}

void CompositionRenderer::resetBackgroundSnapshot(
    const BackgroundSnapshotInvalidationReason reason,
    const std::uint64_t frameId) noexcept
{
    if (backgroundSnapshotValid_)
    {
        std::uint64_t wgcEpoch = backgroundSnapshotEpoch_;
        std::uint64_t wgcGeneration = backgroundSnapshotGeneration_;
        if (backgroundSensor_ != nullptr)
        {
            const WgcBackgroundTransportSnapshot transport =
                backgroundSensor_->transportSnapshot();
            wgcEpoch = transport.epoch;
            wgcGeneration = transport.acceptedGeneration;
        }
        backgroundSnapshotInvalidationMailbox_.record(
            BackgroundSnapshotInvalidation{
                reason,
                frameId,
                wgcEpoch,
                wgcGeneration,
                backgroundSnapshotEpoch_,
                backgroundSnapshotGeneration_});
    }

    // Invalidate the batch without releasing the monitor-sized allocations;
    // repeated clicks should seed the existing ping-pong pair instead of
    // stalling D3D11 on two fresh full-screen textures every time.
    backgroundSnapshotEpoch_ = 0U;
    backgroundSnapshotGeneration_ = 0U;
    backgroundSnapshotValid_ = false;
}

void CompositionRenderer::releaseBackgroundSnapshotResources(
    const BackgroundSnapshotInvalidationReason reason) noexcept
{
    resetBackgroundSnapshot(reason, 0U);
    backgroundSnapshotShaderResource_.Reset();
    backgroundSnapshotRenderTarget_.Reset();
    backgroundSnapshotTexture_.Reset();
    backgroundCandidateShaderResource_.Reset();
    backgroundCandidateRenderTarget_.Reset();
    backgroundCandidateTexture_.Reset();
    backgroundSnapshotSize_ = WindowSize{};
}

void CompositionRenderer::stopBackgroundSensor() noexcept
{
    // A replacement session must receive an immediate idle drain even when it
    // starts within 50 ms of the previous producer.
    wgcDrainPolicyState_ = detail::WgcDrainPolicyState{};
    if (backgroundSensor_ == nullptr)
    {
        backgroundStopMailbox_.recordNoSensor();
        return;
    }

    // FramePool::Recreate advances the sensor-owned epoch. Derive the next
    // session from that final value so a later Start cannot reuse an old stamp.
    backgroundEpoch_ = nextEpoch(backgroundSensor_->expectedEpoch());
    backgroundSensor_->stop();
    // stop() publishes the aggregate before reset, including constructor
    // rollback paths where the sensor never reaches this member.
    backgroundSensor_.reset();
}

bool CompositionRenderer::captureBackgroundSnapshot(
    ID3D11ShaderResourceView* const source,
    const std::uint64_t frameId) noexcept
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
            resetBackgroundSnapshot(
                BackgroundSnapshotInvalidationReason::
                    SnapshotResourcesRecreated,
                frameId);
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
