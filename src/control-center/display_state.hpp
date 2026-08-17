#pragma once

#include "bafx/config/config.hpp"

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
