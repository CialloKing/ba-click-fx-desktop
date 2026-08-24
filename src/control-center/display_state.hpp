#pragma once

#include "bafx/config/config.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bafx::control_center
{

struct DisplayRefreshState final
{
    std::uint32_t numerator{0U};
    std::uint32_t denominator{0U};
};

enum class DisplayDriverState : std::uint8_t
{
    Hardware,
    Warp,
    Unknown
};

enum class DisplayOutputState : std::uint8_t
{
    ConservativeSdr,
    LinearScRgb,
    Unknown
};

enum class DisplayColorState : std::uint8_t
{
    Sdr,
    WideColorGamut,
    Hdr,
    Unknown
};

enum class DisplayTopologyState : std::uint8_t
{
    Complete,
    Incomplete,
    NoActiveDisplays,
    QueryFailed
};

enum class DisplayColorMonitorState : std::uint8_t
{
    Active,
    InvalidTarget,
    Unsupported,
    Failed
};

enum class DisplayColorSnapshotState : std::uint8_t
{
    Fresh,
    RetainedTransaction,
    RetainedLastKnown,
    Unavailable
};

enum class DisplayCadenceFallbackState : std::uint8_t
{
    None,
    NoPhysicalTargets,
    PhysicalTargetUnavailable,
    DrrPhysicalRefreshRateUnavailable,
    InvalidEffectiveRefreshRate,
    MixedCloneRefreshRates
};

enum class DisplayOutputFallbackState : std::uint8_t
{
    None,
    ConservativeSdr
};

enum class DisplayCaptureCadenceState : std::uint8_t
{
    Inactive,
    WrongMonitor,
    TargetRate,
    ConservativeFallback
};

enum class DisplayProducerCadenceState : std::uint8_t
{
    NotRequested,
    Applied,
    InterfaceUnavailable,
    Rejected
};

enum class DisplayOutputMappingState : std::uint8_t
{
    ConservativeSdr,
    AdvancedColorScRgb,
    HdrSceneReferredScRgb,
    Unknown
};

struct DisplayPhysicalCadenceState final
{
    std::optional<DisplayRefreshState> virtualRefresh{};
    std::optional<DisplayRefreshState> physicalRefresh{};
    std::optional<DisplayRefreshState> captureRefresh{};
    bool drrBoosted{false};
    bool available{false};
};

enum class ActiveFxRoiPathState : std::uint8_t
{
    Disabled,
    Idle,
    FullScreen,
    RoiWarmup,
    RoiPrefilter,
    RoiPyramid,
    Unavailable
};

enum class ActiveFxRoiReasonState : std::uint8_t
{
    Disabled,
    NoContent,
    BackgroundDifferentialBloom,
    Context1Unavailable,
    SharedTargetFullWrite,
    AreaTooLarge,
    BenefitTooSmall,
    Applied,
    RendererFallback,
    BloomDisabled,
    CoreMode,
    TouchesBoundary,
    Unavailable,
    Count
};

inline constexpr std::size_t activeFxRoiReasonStateCount =
    static_cast<std::size_t>(ActiveFxRoiReasonState::Count);

struct ActiveFxRoiRectState final
{
    std::int32_t left{0};
    std::int32_t top{0};
    std::int32_t right{0};
    std::int32_t bottom{0};
};

struct ActiveFxRoiGpuPercentileState final
{
    std::optional<double> p50Microseconds{};
    std::optional<double> p95Microseconds{};
};

struct ActiveFxRoiGpuState final
{
    ActiveFxRoiGpuPercentileState prefilter{};
    ActiveFxRoiGpuPercentileState pyramid{};
    ActiveFxRoiGpuPercentileState finalComposite{};
};

struct ActiveFxRoiStageState final
{
    std::uint64_t fullPixels{0U};
    std::uint64_t candidatePixels{0U};
    std::uint64_t drawnPixels{0U};
    std::uint64_t clearedPixels{0U};
};

struct ActiveFxRoiStagesState final
{
    ActiveFxRoiStageState prefilter{};
    ActiveFxRoiStageState downsample{};
    ActiveFxRoiStageState upsample{};
    ActiveFxRoiStageState resolve{};
};

struct ActiveFxRoiPathRuntimeState final
{
    bool requested{false};
    bool executed{false};
    bool eligible{false};
    bool warmup{false};
    ActiveFxRoiPathState actualPath{ActiveFxRoiPathState::Unavailable};
    ActiveFxRoiReasonState decisionReason{
        ActiveFxRoiReasonState::Unavailable};
    std::uint64_t observedFrames{0U};
    std::uint64_t requestedFrames{0U};
    std::uint64_t eligibleFrames{0U};
    std::uint64_t appliedFrames{0U};
    std::uint64_t warmupFrames{0U};
    std::uint64_t fallbackFrames{0U};
    std::uint64_t fullPixels{0U};
    std::uint64_t candidatePixels{0U};
    std::uint64_t drawnPixels{0U};
    std::uint64_t clearedPixels{0U};
    std::uint32_t guardX{0U};
    std::uint32_t guardY{0U};
    std::uint32_t phase{0U};
    std::optional<ActiveFxRoiRectState> dirtyRect{};
    std::optional<ActiveFxRoiRectState> alignedRect{};
    ActiveFxRoiStagesState stages{};
    ActiveFxRoiGpuState gpu{};
    std::array<std::uint64_t, activeFxRoiReasonStateCount> reasonCounts{};
};

struct ActiveFxRoiRuntimeState final
{
    bool enabled{false};
    std::uint32_t sampleWindowMs{0U};
    std::uint32_t sampleAgeMs{0U};
    std::uint64_t lastFrameId{0U};
    ActiveFxRoiPathRuntimeState primary{};
    ActiveFxRoiPathRuntimeState recordingRebuild{};
};

inline constexpr std::uint32_t activeFxRoiStaleThresholdMilliseconds = 3'000U;

[[nodiscard]] constexpr bool activeFxRoiSampleIsStale(
    const ActiveFxRoiRuntimeState& state) noexcept
{
    return state.sampleAgeMs > activeFxRoiStaleThresholdMilliseconds;
}

struct DisplaySessionState final
{
    std::string monitor{};
    std::string device{};
    std::optional<std::string> displayKey{};
    bool coordinator{false};
    bool primary{false};
    bool effectsEnabled{true};
    bool hdrEnabled{false};
    bafx::config::FramePacing framePacing{
        bafx::config::FramePacing::MatchDisplay};
    bool sourceAdapterResolved{false};
    bool sourceIdentityResolved{false};
    std::optional<std::uint32_t> sourceId{};
    std::uint64_t physicalTargetCount{0U};
    std::int32_t left{0};
    std::int32_t top{0};
    std::int32_t right{0};
    std::int32_t bottom{0};
    std::uint32_t targetDpiX{0U};
    std::uint32_t targetDpiY{0U};
    std::uint32_t windowDpi{0U};
    std::optional<DisplayRefreshState> displayRefresh{};
    std::optional<DisplayRefreshState> captureRefresh{};
    DisplayCaptureCadenceState captureCadenceStatus{
        DisplayCaptureCadenceState::Inactive};
    std::optional<DisplayRefreshState> producerPolicyRefresh{};
    std::optional<DisplayRefreshState> freshnessPolicyRefresh{};
    std::uint64_t freshnessPeriodUs{0U};
    DisplayProducerCadenceState producerCadenceStatus{
        DisplayProducerCadenceState::NotRequested};
    std::uint64_t producerRequestedPeriodUs{0U};
    std::uint64_t producerAppliedPeriodUs{0U};
    std::int32_t producerResult{0};
    std::string adapter{};
    DisplayDriverState driver{DisplayDriverState::Unknown};
    DisplayOutputState requestedOutput{DisplayOutputState::Unknown};
    DisplayOutputState resolvedOutput{DisplayOutputState::Unknown};
    DisplayOutputState actualOutput{DisplayOutputState::Unknown};
    bool outputPolicySatisfied{false};
    DisplayOutputMappingState resolvedOutputMapping{
        DisplayOutputMappingState::Unknown};
    DisplayOutputMappingState actualOutputMapping{
        DisplayOutputMappingState::Unknown};
    DisplayOutputFallbackState outputFallback{
        DisplayOutputFallbackState::None};
    std::int32_t outputFallbackResult{0};
    DisplayColorState colorMode{DisplayColorState::Unknown};
    std::optional<bool> hdrSupported{};
    std::optional<bool> hdrActive{};
    DisplayColorMonitorState colorMonitorStatus{
        DisplayColorMonitorState::Failed};
    std::int32_t colorMonitorHresult{0};
    std::uint64_t colorMonitorGeneration{0U};
    std::uint64_t colorQueryGeneration{0U};
    DisplayColorSnapshotState colorSnapshotDisposition{
        DisplayColorSnapshotState::Unavailable};
    bool colorSnapshotComplete{false};
    std::optional<std::int32_t> advancedColorQueryResult{};
    std::optional<bool> advancedColorLimitedByPolicy{};
    std::optional<bool> hdrUserEnabled{};
    std::optional<std::int32_t> sdrWhiteLevelQueryResult{};
    std::optional<float> sdrWhiteLevelNits{};
    std::optional<bool> sdrWhiteLevelRetained{};
    std::optional<bool> sdrWhiteLevelConsistent{};
    std::uint32_t colorRefreshRetriesRemaining{0U};
    DisplayCadenceFallbackState cadenceFallbackReason{
        DisplayCadenceFallbackState::NoPhysicalTargets};
    std::vector<DisplayPhysicalCadenceState> physicalCadence{};
    bool backgroundCaptureActive{false};
    bool backgroundCaptureRestartAllowed{false};
    std::string backgroundCaptureFailure{};
    bool renderFaulted{false};
    bool outputContractFaulted{false};
    ActiveFxRoiRuntimeState activeFxRoi{};
};

struct DisplayState final
{
    std::uint32_t schemaVersion{0U};
    std::uint64_t runtimeGeneration{0U};
    std::uint64_t configGeneration{0U};
    std::uint64_t appliedConfigGeneration{0U};
    DisplayTopologyState topologyStatus{DisplayTopologyState::QueryFailed};
    std::uint32_t topologyError{0U};
    bool offlineOverridesAuthoritative{false};
    std::vector<bafx::config::DisplayOverrideConfig> offlineOverrides{};
    std::vector<DisplaySessionState> sessions{};
};

struct DisplayStateParseResult final
{
    std::optional<DisplayState> state{};
    std::string error{};

    [[nodiscard]] bool succeeded() const noexcept
    {
        return state.has_value();
    }
};

// GetDisplayState is a bounded product protocol, not a general JSON surface.
// Rejecting shape drift keeps the Control Center from presenting stale fields
// as current runtime facts.
[[nodiscard]] DisplayStateParseResult parseDisplayState(
    std::string_view json) noexcept;

}
