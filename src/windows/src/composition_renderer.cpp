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
constexpr std::uint64_t maximumSessionWindowExclusionRejectedFrames = 8U;
constexpr CompositionOutputState scRgbOutputState{
    DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
    CompositionOutputTransfer::LinearScRgb,
    CompositionOutputFallback::None,
    S_OK,
    compositionOutputPolicyFor(
        CompositionOutputPreference::PreferLinearScRgb).mapping,
    true};
constexpr CompositionOutputState requestedSdrOutputState{
    DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
    CompositionOutputTransfer::SdrGamma22,
    CompositionOutputFallback::None,
    S_OK,
    compositionOutputPolicyFor(
        CompositionOutputPreference::ConservativeSdr).mapping,
    false};
constexpr CompositionOutputState fallbackSdrOutputState{
    DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
    CompositionOutputTransfer::SdrGamma22,
    CompositionOutputFallback::ConservativeSdr,
    S_OK,
    compositionOutputPolicyFor(
        CompositionOutputPreference::ConservativeSdr).mapping,
    false};

struct SwapChainCandidateSpecification final
{
    CompositionOutputState output{};
    std::string_view createOperation{};
    std::string_view checkColorSpaceOperation{};
    std::string_view setColorSpaceOperation{};
};

struct CreatedCompositionSwapChain final
{
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain{};
    UniqueHandle frameLatencyHandle{};
    CompositionOutputState output{};
};

[[nodiscard]] bool hasValidBackgroundReferenceWhiteContract(
    const CompositionOutputMapping& mapping) noexcept
{
    if (!mapping.backgroundReferenceWhiteValid)
    {
        return mapping.backgroundReferenceWhiteNits == 0.0F;
    }
    return std::isfinite(mapping.backgroundReferenceWhiteNits)
        && mapping.backgroundReferenceWhiteNits > 0.0F;
}

[[nodiscard]] bool backgroundReferenceWhiteUnavailable(
    const CompositionOutputMapping& mapping) noexcept
{
    return mapping.backgroundReferenceWhiteRequired
        && !mapping.backgroundReferenceWhiteValid;
}

constexpr std::string_view backgroundReferenceWhiteUnavailableFailure =
    "WGC background reference white is required but unavailable";

constexpr SwapChainCandidateSpecification scRgbSwapChainCandidate{
    scRgbOutputState,
    "IDXGIFactory2::CreateSwapChainForComposition(scRGB)",
    "IDXGISwapChain3::CheckColorSpaceSupport(scRGB present)",
    "IDXGISwapChain3::SetColorSpace1(scRGB)"};
constexpr SwapChainCandidateSpecification requestedSdrSwapChainCandidate{
    requestedSdrOutputState,
    "IDXGIFactory2::CreateSwapChainForComposition(BGRA8 SDR)",
    "IDXGISwapChain3::CheckColorSpaceSupport(BGRA8 SDR present)",
    "IDXGISwapChain3::SetColorSpace1(BGRA8 SDR)"};
constexpr SwapChainCandidateSpecification fallbackSdrSwapChainCandidate{
    fallbackSdrOutputState,
    "IDXGIFactory2::CreateSwapChainForComposition(BGRA8 SDR)",
    "IDXGISwapChain3::CheckColorSpaceSupport(BGRA8 SDR present)",
    "IDXGISwapChain3::SetColorSpace1(BGRA8 SDR)"};

[[nodiscard]] CreatedCompositionSwapChain createCompositionSwapChainCandidate(
    IDXGIFactory2* const factory,
    ID3D11Device* const device,
    const WindowSize size,
    const SwapChainCandidateSpecification& candidate)
{
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = size.width;
    description.Height = size.height;
    description.Format = candidate.output.format;
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
            device,
            &description,
            nullptr,
            &swapChain),
        candidate.createOperation);

    CreatedCompositionSwapChain created{};
    throwIfFailed(
        swapChain.As(&created.swapChain),
        "IDXGISwapChain1::QueryInterface(IDXGISwapChain3)");

    UINT colorSpaceSupport = 0U;
    throwIfFailed(
        created.swapChain->CheckColorSpaceSupport(
            candidate.output.colorSpace,
            &colorSpaceSupport),
        candidate.checkColorSpaceOperation);
    if ((colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0U)
    {
        // Never let DXGI silently reinterpret the final shader's transfer.
        throwIfFailed(DXGI_ERROR_UNSUPPORTED, candidate.checkColorSpaceOperation);
    }
    throwIfFailed(
        created.swapChain->SetColorSpace1(candidate.output.colorSpace),
        candidate.setColorSpaceOperation);
    throwIfFailed(
        created.swapChain->SetMaximumFrameLatency(1),
        "IDXGISwapChain2::SetMaximumFrameLatency");

    created.frameLatencyHandle.reset(
        created.swapChain->GetFrameLatencyWaitableObject());
    if (created.frameLatencyHandle.get() == nullptr)
    {
        throwLastError("IDXGISwapChain2::GetFrameLatencyWaitableObject");
    }
    created.output = candidate.output;
    return created;
}

[[nodiscard]] CreatedCompositionSwapChain createPreferredCompositionSwapChain(
    IDXGIFactory2* const factory,
    ID3D11Device* const device,
    const WindowSize size,
    const CompositionOutputPolicy& policy)
{
    if (policy.preference == CompositionOutputPreference::ConservativeSdr)
    {
        if (policy.mapping.mode
                != CompositionOutputMappingMode::ConservativeSdr
            || policy.mapping.intensitySemantics
                != requestedSdrOutputState.mapping.intensitySemantics
            || policy.mapping.referenceWhiteValid
            || policy.mapping.referenceWhiteNits != 0.0F
            || !hasValidBackgroundReferenceWhiteContract(policy.mapping))
        {
            throw std::invalid_argument(
                "Conservative SDR output policy has a non-SDR mapping");
        }
        CreatedCompositionSwapChain created = createCompositionSwapChainCandidate(
            factory,
            device,
            size,
            requestedSdrSwapChainCandidate);
        created.output.mapping = policy.mapping;
        return created;
    }
    if (policy.preference != CompositionOutputPreference::PreferLinearScRgb
        || policy.mapping.mode == CompositionOutputMappingMode::ConservativeSdr
        || !hasValidBackgroundReferenceWhiteContract(policy.mapping))
    {
        throw std::invalid_argument("Composition output policy is invalid");
    }
    if (policy.mapping.mode
            == CompositionOutputMappingMode::HdrSceneReferredScRgb
        && (!policy.mapping.referenceWhiteValid
            || !std::isfinite(policy.mapping.referenceWhiteNits)
            || policy.mapping.referenceWhiteNits <= 0.0F))
    {
        // An unverified white level cannot be represented as a verified HDR
        // scene-referred contract. Production should have resolved this to SDR.
        throw std::invalid_argument(
            "HDR output policy requires a verified reference white");
    }

    try
    {
        CreatedCompositionSwapChain created = createCompositionSwapChainCandidate(
            factory,
            device,
            size,
            scRgbSwapChainCandidate);
        created.output.mapping = policy.mapping;
        return created;
    }
    catch (const HResultError& error)
    {
        if (isDeviceLostResult(error.result()))
        {
            throw;
        }
        // Runtime capability decides the transport. The binary retains both
        // paths regardless of the SDK or Windows version used to compile it.
        CreatedCompositionSwapChain created = createCompositionSwapChainCandidate(
            factory,
            device,
            size,
            fallbackSdrSwapChainCandidate);
        created.output.fallbackResult = error.result();
        created.output.mapping.backgroundReferenceWhiteNits =
            policy.mapping.backgroundReferenceWhiteNits;
        created.output.mapping.backgroundReferenceWhiteValid =
            policy.mapping.backgroundReferenceWhiteValid;
        created.output.mapping.backgroundReferenceWhiteRequired =
            policy.mapping.backgroundReferenceWhiteRequired;
        return created;
    }
}

[[nodiscard]] OutputRenegotiationStatus classifyOutputRenegotiation(
    const CompositionOutputState& previous,
    const CompositionOutputState& current) noexcept
{
    if (previous == current)
    {
        return OutputRenegotiationStatus::RecreatedSameContract;
    }
    if (previous.transfer == current.transfer)
    {
        // Reference white and HDR mapping can change without changing the
        // scRGB transport. Report that distinction instead of claiming that
        // an already-linear swap chain changed to linear scRGB.
        return OutputRenegotiationStatus::ChangedWithinTransfer;
    }
    return current.transfer == CompositionOutputTransfer::LinearScRgb
        ? OutputRenegotiationStatus::ChangedToLinearScRgb
        : OutputRenegotiationStatus::ChangedToSdr;
}

constexpr bafx::core::MonotonicTime minimumBackgroundCadencePeriod =
    std::chrono::nanoseconds(16'666'667);
constexpr DisplayRefreshRate conservativeBackgroundRefreshRate{
    60U,
    1U,
    DisplayRefreshRateSource::ConservativeFallback};
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

struct BackgroundCadencePolicy final
{
    BackgroundCadenceRefreshStatus status{
        BackgroundCadenceRefreshStatus::ConservativeFallback};
    DisplayRefreshRate producerRefreshRate{
        conservativeBackgroundRefreshRate};
    DisplayRefreshRate freshnessRefreshRate{
        conservativeBackgroundRefreshRate};
    bafx::core::MonotonicTime producerPeriod{
        minimumBackgroundCadencePeriod};
    bafx::core::MonotonicTime freshnessPeriod{
        minimumBackgroundCadencePeriod};
};

[[nodiscard]] BackgroundCadencePolicy resolveBackgroundCadencePolicy(
    const std::optional<DisplayRefreshRate>& refreshRate) noexcept
{
    if (!refreshRate.has_value())
    {
        return {};
    }
    const std::optional<bafx::core::MonotonicTime> targetPeriod =
        refreshPeriod(*refreshRate);
    if (!targetPeriod.has_value())
    {
        return {};
    }

    BackgroundCadencePolicy policy{};
    policy.status = BackgroundCadenceRefreshStatus::TargetRate;
    policy.producerRefreshRate = *refreshRate;
    policy.producerPeriod = *targetPeriod;
    const bool noFasterThanFreshnessCeiling =
        static_cast<std::uint64_t>(refreshRate->numerator)
        <= static_cast<std::uint64_t>(
            conservativeBackgroundRefreshRate.numerator)
            * refreshRate->denominator;
    if (noFasterThanFreshnessCeiling)
    {
        policy.freshnessRefreshRate = *refreshRate;
        policy.freshnessPeriod = std::max(
            *targetPeriod,
            minimumBackgroundCadencePeriod);
    }
    // WGC delivery jitter can still be near 60 Hz on a high-refresh display.
    // A looser freshness window must not be reported as the producer policy.
    return policy;
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
            bloomSettings.intensity});
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
    const std::optional<LUID> requestedAdapterLuid,
    const CompositionOutputPolicy outputPolicy)
    : window_(window)
    , bloomSettings_(bloomSettings)
    , size_(size)
    , backgroundResourceLedger_(
          std::make_shared<WgcBackgroundResourceLedger>())
    , backgroundStopObserver_(backgroundStopObserver)
    , requestedAdapterLuid_(requestedAdapterLuid)
    , outputPolicy_(outputPolicy)
{
    // Create the optional sender after all members have been initialized in
    // declaration order, so device-resource setup never observes a partial
    // sender state and MSVC does not report an initialization-order warning.
    spout2Sender_ = std::make_unique<Spout2Sender>();
    createDeviceResources();
}

CompositionRenderer::CompositionRenderer(
    const HWND window,
    const WindowSize size,
    const FxBloomSettings bloomSettings,
    const WgcBackgroundStopObserver backgroundStopObserver,
    const std::optional<LUID> requestedAdapterLuid,
    const CompositionOutputPreference outputPreference)
    : CompositionRenderer(
          window,
          size,
          bloomSettings,
          backgroundStopObserver,
          requestedAdapterLuid,
          compositionOutputPolicyFor(outputPreference))
{
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
    // A WARP fallback did not satisfy an explicit adapter request. Keep that
    // request retryable when a later topology transaction observes the GPU.
    if (sameRequestedAdapter(requestedAdapterLuid_, requestedAdapterLuid)
        && deviceInfo_.requestedAdapterMatched)
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

OutputRenegotiationResult CompositionRenderer::renegotiateOutput(
    const CompositionOutputPolicy policy)
{
    try
    {
        return renegotiateOutputOnce(policy);
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
    }

    // Recovery has a one-shot budget. A second device-loss exception escapes
    // directly instead of re-entering an open-ended retry loop.
    OutputRenegotiationResult result = renegotiateOutputOnce(policy);
    result.deviceRecovered = true;
    return result;
}

OutputRenegotiationResult CompositionRenderer::renegotiateOutput(
    const CompositionOutputPreference preference)
{
    return renegotiateOutput(compositionOutputPolicyFor(preference));
}

OutputRenegotiationResult CompositionRenderer::renegotiateOutputOnce(
    const CompositionOutputPolicy policy)
{
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    throwIfFailed(
        device_.As(&dxgiDevice),
        "ID3D11Device::QueryInterface(IDXGIDevice)");

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    throwIfFailed(
        dxgiDevice->GetAdapter(&adapter),
        "IDXGIDevice::GetAdapter");

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    throwIfFailed(
        adapter->GetParent(IID_PPV_ARGS(&factory)),
        "IDXGIAdapter::GetParent");

    CreatedCompositionSwapChain created = createPreferredCompositionSwapChain(
        factory.Get(),
        device_.Get(),
        size_,
        policy);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> replacementBackBuffer;
    throwIfFailed(
        created.swapChain->GetBuffer(
            0U,
            IID_PPV_ARGS(&replacementBackBuffer)),
        "IDXGISwapChain::GetBuffer(output renegotiation)");

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> replacementRenderTarget;
    throwIfFailed(
        device_->CreateRenderTargetView(
            replacementBackBuffer.Get(),
            nullptr,
            &replacementRenderTarget),
        "ID3D11Device::CreateRenderTargetView(output renegotiation)");

    auto replacementFxRenderer = std::make_unique<FxGpuRenderer>(
        device_.Get(),
        context_.Get(),
        size_,
        bloomSettings_,
        created.output.mapping);
    replacementFxRenderer->setOverlayProfile(overlayProfile_);
    replacementFxRenderer->setThemeColor(themeColor_);

    throwIfFailed(
        rootVisual_->SetContent(created.swapChain.Get()),
        "IDCompositionVisual::SetContent(output renegotiation)");
    const HRESULT commitResult = compositionDevice_->Commit();
    if (FAILED(commitResult))
    {
        // SetContent mutates the retained visual before Commit. Restore the old
        // content explicitly so a non-device failure leaves a usable renderer.
        const HRESULT restoreContentResult = rootVisual_->SetContent(
            swapChain_.Get());
        const HRESULT restoreCommitResult = SUCCEEDED(restoreContentResult)
            ? compositionDevice_->Commit()
            : restoreContentResult;
        if (FAILED(restoreCommitResult))
        {
            throwIfFailed(
                restoreCommitResult,
                "IDCompositionDevice::Commit(output renegotiation rollback)");
        }
        throwIfFailed(
            commitResult,
            "IDCompositionDevice::Commit(output renegotiation)");
    }

    const CompositionOutputPolicy previousPolicy = outputPolicy_;
    const CompositionOutputState previousOutput = deviceInfo_.output;
    context_->OMSetRenderTargets(0U, nullptr, nullptr);
    renderTarget_ = std::move(replacementRenderTarget);
    backBuffer_ = std::move(replacementBackBuffer);
    swapChain_ = std::move(created.swapChain);
    frameLatencyHandle_ = std::move(created.frameLatencyHandle);
    fxRenderer_ = std::move(replacementFxRenderer);
    deviceInfo_.output = created.output;
    outputPolicy_ = policy;
    deviceInfo_.outputPreference = outputPolicy_.preference;
    deviceInfo_.outputPolicy = outputPolicy_;
    lastCenterPixel_.reset();
    backgroundPathLatch_.reset();
    previousVisualBounds_.reset();

    return OutputRenegotiationResult{
        classifyOutputRenegotiation(previousOutput, deviceInfo_.output),
        previousPolicy.preference,
        outputPolicy_.preference,
        previousOutput,
        deviceInfo_.output,
        false};
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

bool CompositionRenderer::deviceRecoveryBudgetConsumed() const noexcept
{
    return deviceRecoveryAttempted_;
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
            lastCenterPixel_.reset();
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
            recordingRenderTarget_.Reset();
            recordingTexture_.Reset();
            createSpout2RecordingTarget();
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

void CompositionRenderer::setThemeColor(const std::string_view themeColor)
{
    fxRenderer_->setThemeColor(themeColor);
    themeColor_ = std::string(themeColor);
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
    recordingRenderTarget_.Reset();
    recordingTexture_.Reset();
    if (spout2Sender_ != nullptr)
    {
        spout2Sender_->reset();
    }
    backBuffer_.Reset();
    rootVisual_.Reset();
    compositionTarget_.Reset();
    compositionDevice_.Reset();
    frameLatencyHandle_.reset();
    swapChain_.Reset();
    lastCenterPixel_.reset();
    context_.Reset();
    device_.Reset();
    deviceInfo_.output = CompositionOutputState{};
}

void CompositionRenderer::setOverlayProfile(const FxOverlayProfile profile)
{
    fxRenderer_->setOverlayProfile(profile);
    overlayProfile_ = profile;
}

void CompositionRenderer::setSpout2Enabled(const bool enabled)
{
    // Configuration snapshots are reapplied for unrelated controls as well.
    // Keep the shared texture and sender handle stable unless the switch
    // actually changes or device recovery left the enabled output uncreated.
    if (spout2Enabled_ == enabled
        && (!enabled || recordingTexture_ != nullptr))
    {
        return;
    }
    spout2Enabled_ = enabled;
    spout2Sender_->setEnabled(enabled);
    recordingRenderTarget_.Reset();
    recordingTexture_.Reset();
    if (!enabled)
    {
        return;
    }
    createSpout2RecordingTarget();
}

bool CompositionRenderer::spout2Enabled() const noexcept
{
    return spout2Enabled_;
}

std::string_view CompositionRenderer::spout2SenderName() const noexcept
{
    return spout2Sender_->senderName();
}

Spout2SenderStatus CompositionRenderer::spout2Status() const noexcept
{
    return spout2Sender_->status();
}

std::string_view CompositionRenderer::spout2Error() const noexcept
{
    return spout2Sender_->error();
}

bool CompositionRenderer::sendSpout2Heartbeat() noexcept
{
    if (!spout2Enabled_ || recordingTexture_ == nullptr)
    {
        return false;
    }
    return spout2Sender_->send(
        device_.Get(),
        recordingTexture_.Get(),
        size_.width,
        size_.height);
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
    // Spout2 is a complete desktop output, so it must keep the WGC snapshot
    // alive during idle periods even though the transparent overlay is empty.
    const bool needsBackgroundFrame = hasDrawableContent || spout2Enabled_;
    const bool referenceWhiteUnavailable = backgroundSensor_ != nullptr
        && backgroundReferenceWhiteUnavailable(deviceInfo_.output.mapping);
    std::optional<BackgroundRenderInput> background;
    std::optional<WgcBackgroundSample> backgroundSample;
    bafx::core::BackgroundUsageDecision acquireUsage{};
    bafx::core::BackgroundUsageDecision retainUsage{};
    if (referenceWhiteUnavailable)
    {
        // This cause must win the single-slot diagnostic mailbox even if the
        // visible batch happens to end on the same frame.
        resetBackgroundSnapshot(
            BackgroundSnapshotInvalidationReason::ReferenceWhiteUnavailable,
            diagnostics.frameId);
    }
    else if (!needsBackgroundFrame)
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
    if (referenceWhiteUnavailable)
    {
        // Keep the producer warm so a later display-policy refresh can recover
        // without synchronously creating WGC on the first visible frame. Only
        // the unsafe consumer path and any old-scale snapshot are disabled.
        backgroundPathLatch_.forceFxOnly();
        backgroundCompositeStatus_ = BackgroundCompositeStatus::InvalidPolicy;
        setBackgroundCaptureFailure(
            backgroundReferenceWhiteUnavailableFailure);
    }
    else if (backgroundCaptureFailure()
        == backgroundReferenceWhiteUnavailableFailure)
    {
        // Do not erase an unrelated WGC failure. This exact renderer-owned
        // reason is cleared when a new output contract supplies the white.
        setBackgroundCaptureFailure({});
    }
    const bafx::core::MonotonicTime effectiveWallTime =
        resolveMonotonicTime(wallTime);
    const BackgroundSensorMaintenanceDiagnostics maintenance =
        drainBackgroundSensor(
            needsBackgroundFrame,
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
        && !referenceWhiteUnavailable
        && needsBackgroundFrame
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
            needsBackgroundFrame,
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
        gpuTimestampFrame.recorder(),
        spout2Enabled_ && recordingRenderTarget_ != nullptr
            ? recordingRenderTarget_.Get()
            : nullptr);
    if (spout2Enabled_ && recordingTexture_ != nullptr)
    {
        static_cast<void>(spout2Sender_->send(
            device_.Get(),
            recordingTexture_.Get(),
            size_.width,
            size_.height));
    }
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
        else if (backgroundRequireSessionWindowExclusion_
            && diagnostics.wgc.configurationIterationRejectedFrames > 0U
            && backgroundSensor_->capabilities()
                    .sessionWindowExclusion.consecutiveRejectedFrameCount
                >= maximumSessionWindowExclusionRejectedFrames)
        {
            // A short run of old frames is expected after SetWindowExclusionList.
            // Repeated mismatch means the runtime cannot establish a stable
            // configuration-to-frame relation, so fail closed and let the
            // transition state machine choose LegacyGlobal or FX-only.
            stopFailedBackgroundCapture(
                "WGC Session-local exclusion configuration iteration did not stabilize");
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
    if (deviceInfo_.output.transfer != CompositionOutputTransfer::LinearScRgb)
    {
        // ClearRenderTargetView bypasses the final composite shader. On BGRA8
        // it cannot exercise the FP16 extended-premultiplied probe contract.
        throw std::logic_error(
            "Composition probe requires linear scRGB output");
    }
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
    const bool borderlessAccessConfirmed,
    const BackgroundCaptureRequest::ExclusionMode exclusionMode,
    const std::optional<DisplayRefreshRate>& refreshRate) noexcept
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
    backgroundRequireSessionWindowExclusion_ = exclusionMode
        == BackgroundCaptureRequest::ExclusionMode::SessionLocal;
    backgroundCaptureRequested_ = exclusionConfirmed
        && monitor != nullptr
        && deviceInfo_.driverType == GraphicsDriverType::Hardware
        && backgroundStopMailbox_.restartAllowed()
        && backgroundCaptureAfterRecoveryAllowed_
        && (allowSystemBorder || borderlessAccessConfirmed);
    backgroundMonitor_ = backgroundCaptureRequested_ ? monitor : nullptr;
    backgroundTargetRefreshRate_ = backgroundCaptureRequested_
        ? refreshRate
        : std::nullopt;
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

    backgroundSessionWindowExclusionState_ =
        std::make_shared<WgcSessionWindowExclusionState>();
    return tryCreateBackgroundSensor(refreshRate);
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
    backgroundRequireSessionWindowExclusion_ = false;
    backgroundMonitor_ = nullptr;
    backgroundTargetRefreshRate_.reset();
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

WgcProducerCadenceState
CompositionRenderer::backgroundCaptureProducerCadence() const noexcept
{
    return backgroundSensor_ != nullptr
        ? backgroundSensor_->capabilities().producerCadence
        : WgcProducerCadenceState{};
}

BackgroundCadenceRefreshResult
CompositionRenderer::backgroundCaptureCadence() const noexcept
{
    if (!backgroundCaptureActive())
    {
        return BackgroundCadenceRefreshResult{
            BackgroundCadenceRefreshStatus::Inactive,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            backgroundRefreshPeriod_};
    }

    const BackgroundCadencePolicy policy = resolveBackgroundCadencePolicy(
        backgroundTargetRefreshRate_);
    return BackgroundCadenceRefreshResult{
        policy.status,
        backgroundTargetRefreshRate_,
        policy.producerRefreshRate,
        policy.freshnessRefreshRate,
        backgroundRefreshPeriod_,
        backgroundCaptureProducerCadence()};
}

BackgroundCadenceRefreshResult CompositionRenderer::refreshBackgroundCadence(
    const HMONITOR monitor,
    const std::optional<DisplayRefreshRate>& refreshRate) noexcept
{
    if (!backgroundCaptureRequested_ || backgroundSensor_ == nullptr)
    {
        return BackgroundCadenceRefreshResult{
            BackgroundCadenceRefreshStatus::Inactive,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            backgroundRefreshPeriod_};
    }
    if (monitor == nullptr || monitor != backgroundMonitor_)
    {
        return BackgroundCadenceRefreshResult{
            BackgroundCadenceRefreshStatus::WrongMonitor,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            backgroundRefreshPeriod_};
    }

    backgroundTargetRefreshRate_ = refreshRate;
    const BackgroundCadencePolicy policy = resolveBackgroundCadencePolicy(
        backgroundTargetRefreshRate_);
    backgroundRefreshPeriod_ = policy.freshnessPeriod;
    const WgcProducerCadenceState producerCadence =
        backgroundSensor_->configureMinimumUpdateInterval(
            policy.producerPeriod);
    return BackgroundCadenceRefreshResult{
        policy.status,
        refreshRate,
        policy.producerRefreshRate,
        policy.freshnessRefreshRate,
        backgroundRefreshPeriod_,
        producerCadence};
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

bool CompositionRenderer::tryCreateBackgroundSensor(
    const std::optional<DisplayRefreshRate>& requestedRefreshRate) noexcept
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

    const BackgroundCadencePolicy cadence = resolveBackgroundCadencePolicy(
        requestedRefreshRate);

    try
    {
        setBackgroundCaptureFailure({});
        WgcBackgroundSensorOptions sensorOptions{};
        sensorOptions.epoch = backgroundEpoch_;
        sensorOptions.excludesOwnOverlay = true;
        sensorOptions.cursorExcluded = backgroundCursorExcluded_;
        sensorOptions.allowSystemBorder = backgroundSystemBorderAllowed_;
        sensorOptions.requireSessionWindowExclusion =
            backgroundRequireSessionWindowExclusion_;
        sensorOptions.excludedWindow = window_;
        sensorOptions.sessionWindowExclusionState =
            backgroundSessionWindowExclusionState_;
        // Producer cadence follows an authoritative target rate. The separate
        // freshness window below remains no tighter than 60 Hz so normal WGC
        // delivery jitter does not destabilize the background path.
        sensorOptions.minimumUpdateInterval = cadence.producerPeriod;
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
        backgroundTargetRefreshRate_ = requestedRefreshRate;
        backgroundRefreshPeriod_ = cadence.freshnessPeriod;
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

WgcSessionWindowExclusionState
CompositionRenderer::backgroundSessionWindowExclusion() const noexcept
{
    if (backgroundSensor_ != nullptr)
    {
        return backgroundSensor_->capabilities().sessionWindowExclusion;
    }
    return backgroundSessionWindowExclusionState_ != nullptr
        ? *backgroundSessionWindowExclusionState_
        : WgcSessionWindowExclusionState{};
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

WindowSize CompositionRenderer::outputSize() const noexcept
{
    return size_;
}

D3D_FEATURE_LEVEL CompositionRenderer::featureLevel() const noexcept
{
    return featureLevel_;
}

const GraphicsDeviceInfo& CompositionRenderer::deviceInfo() const noexcept
{
    return deviceInfo_;
}

bool CompositionRenderer::requestedAdapterPresent() const noexcept
{
    if (!requestedAdapterLuid_.has_value())
    {
        return true;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        return false;
    }
    for (UINT index = 0U;; ++index)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const HRESULT result = factory->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND)
        {
            return false;
        }
        if (FAILED(result))
        {
            return false;
        }

        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)))
        {
            return false;
        }
        if (description.AdapterLuid.HighPart
                == requestedAdapterLuid_->HighPart
            && description.AdapterLuid.LowPart
                == requestedAdapterLuid_->LowPart)
        {
            return true;
        }
    }
}

const CompositionOutputState& CompositionRenderer::outputState() const noexcept
{
    return deviceInfo_.output;
}

CompositionOutputPreference CompositionRenderer::outputPreference() const noexcept
{
    return outputPolicy_.preference;
}

const CompositionOutputPolicy& CompositionRenderer::outputPolicy() const noexcept
{
    return outputPolicy_;
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
        bloomSettings_,
        deviceInfo_.output.mapping);
    fxRenderer_->setThemeColor(themeColor_);
    fxRenderer_->setOverlayProfile(overlayProfile_);
    createSpout2RecordingTarget();
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

    CreatedCompositionSwapChain created = createPreferredCompositionSwapChain(
        factory.Get(),
        device_.Get(),
        size,
        outputPolicy_);

    // Publish only a fully configured candidate so shaders and diagnostics
    // cannot observe a partially initialized output transport.
    swapChain_ = std::move(created.swapChain);
    frameLatencyHandle_ = std::move(created.frameLatencyHandle);
    deviceInfo_.output = created.output;
    deviceInfo_.outputPreference = outputPolicy_.preference;
    deviceInfo_.outputPolicy = outputPolicy_;
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

void CompositionRenderer::createSpout2RecordingTarget()
{
    if (!spout2Enabled_)
    {
        return;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = size_.width;
    description.Height = size_.height;
    description.MipLevels = 1U;
    description.ArraySize = 1U;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc = DXGI_SAMPLE_DESC{1U, 0U};
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    throwIfFailed(
        device_->CreateTexture2D(
            &description,
            nullptr,
            &recordingTexture_),
        "ID3D11Device::CreateTexture2D(Spout2 recording)");
    throwIfFailed(
        device_->CreateRenderTargetView(
            recordingTexture_.Get(),
            nullptr,
            &recordingRenderTarget_),
        "ID3D11Device::CreateRenderTargetView(Spout2 recording)");
    // The first heartbeat can occur before the display swap chain presents a
    // frame. Initialize the shared output to a valid opaque black image so
    // OBS never receives undefined GPU memory.
    constexpr float clearColor[4]{0.0F, 0.0F, 0.0F, 1.0F};
    context_->ClearRenderTargetView(recordingRenderTarget_.Get(), clearColor);
    context_->Flush();
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
    D3D11_TEXTURE2D_DESC description{};
    backBuffer_->GetDesc(&description);
    if (description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
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
        return;
    }
    if (description.Format == DXGI_FORMAT_B8G8R8A8_UNORM)
    {
        const Bgra8UnormPixel pixel = readbackBgra8UnormPixel(
            context_.Get(),
            backBuffer_.Get(),
            centerX,
            centerY);
        constexpr float unormScale = 1.0F / 255.0F;
        lastCenterPixel_ = PixelF{
            static_cast<float>(pixel.red) * unormScale,
            static_cast<float>(pixel.green) * unormScale,
            static_cast<float>(pixel.blue) * unormScale,
            static_cast<float>(pixel.alpha) * unormScale};
        return;
    }
    throw std::logic_error(
        "Composition center-pixel readback has an unsupported output format");
}

}
