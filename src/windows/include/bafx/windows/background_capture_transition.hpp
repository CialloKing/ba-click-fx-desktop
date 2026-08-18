#pragma once

#include "bafx/windows/fx_gpu_renderer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace bafx::windows
{

inline constexpr std::size_t maximumBackgroundCaptureActions = 10U;

struct BackgroundCaptureRequest
{
    bool sensorRequired{false};
    FxOverlayProfile overlayProfile{FxOverlayProfile::RecordingCompatible};
    bool cursorExcluded{true};
    bool allowSystemBorder{true};
    // A stable request is terminal after failure. Increment this only for an
    // explicit retry so the render loop cannot restart WGC indefinitely.
    std::uint64_t retryToken{0U};
    // Legacy background-aware capture hides the overlay globally. The test
    // path keeps WDA_NONE and asks only its own WGC session to exclude it.
    enum class ExclusionMode : std::uint8_t
    {
        LegacyGlobal,
        SessionLocal
    };
    ExclusionMode exclusionMode{ExclusionMode::LegacyGlobal};

    [[nodiscard]] bool operator==(
        const BackgroundCaptureRequest&) const noexcept = default;
};

enum class BackgroundCaptureActionKind : std::uint8_t
{
    RequestBorderlessAccess,
    StopSensor,
    ResizeOutput,
    RecreateFramePool,
    SetAffinityExcluded,
    SetAffinityIncluded,
    ApplyOverlayProfile,
    StartSensor
};

enum class BackgroundCaptureActionObservation : std::uint8_t
{
    Pending,
    Succeeded,
    Failed,
    // Preserve an already-coalesced resize unless a newer geometry intent
    // explicitly supersedes the whole transaction.
    Canceled,
    CanceledSupersededIntent
};

struct BackgroundCaptureAction
{
    BackgroundCaptureActionKind kind{BackgroundCaptureActionKind::StopSensor};
    FxOverlayProfile overlayProfile{FxOverlayProfile::FxOnlyFallback};
    WindowSize outputSize{};
    WindowSize captureSize{};
    bool cursorExcluded{true};
    bool allowSystemBorder{true};
    BackgroundCaptureRequest::ExclusionMode exclusionMode{
        BackgroundCaptureRequest::ExclusionMode::LegacyGlobal};

    [[nodiscard]] bool operator==(
        const BackgroundCaptureAction& other) const noexcept;
};

enum class EffectiveBackgroundCapturePath : std::uint8_t
{
    BackgroundAware,
    SessionLocalExclusion,
    FxOnly,
    FxOnlyCaptureVisibilityUnknown
};

[[nodiscard]] constexpr bool isActiveBackgroundCapturePath(
    const EffectiveBackgroundCapturePath path) noexcept
{
    return path == EffectiveBackgroundCapturePath::BackgroundAware
        || path == EffectiveBackgroundCapturePath::SessionLocalExclusion;
}

enum class BackgroundCaptureFailure : std::uint8_t
{
    None,
    SensorStopFailed,
    BorderlessAccessFailed,
    BorderlessAccessCanceled,
    ExclusionUnconfirmed,
    SensorStartFailed,
    SessionExclusionFailed,
    FramePoolRecreateFailed,
    InclusionUnconfirmed,
    SessionStopped,
    CaptureSizeMismatch
};

enum class BackgroundCaptureRequestResult : std::uint8_t
{
    Started,
    NoChange,
    Busy,
    InvalidRequest
};

[[nodiscard]] bool isValidBackgroundCaptureRequest(
    const BackgroundCaptureRequest& request) noexcept;

class BackgroundCaptureTransition final
{
public:
    [[nodiscard]] BackgroundCaptureRequestResult beginIntent(
        BackgroundCaptureRequest request,
        std::optional<WindowSize> outputSize) noexcept;
    [[nodiscard]] BackgroundCaptureRequestResult beginRequest(
        BackgroundCaptureRequest request) noexcept;
    // Display power loss is an expected lifecycle boundary rather than a WGC
    // failure. Preserve the requested capture contract while parking the
    // effective path in FX-only until an explicit recovery advances its token.
    [[nodiscard]] BackgroundCaptureRequestResult beginPowerSuspension(
        BackgroundCaptureRequest request,
        std::optional<WindowSize> outputSize = std::nullopt) noexcept;
    [[nodiscard]] bool beginFramePoolRecreate(WindowSize captureSize) noexcept;
    [[nodiscard]] bool beginSessionStopped() noexcept;
    [[nodiscard]] bool beginCaptureExclusionLost() noexcept;
    [[nodiscard]] bool beginBorderlessAccessLost() noexcept;
    [[nodiscard]] bool beginCaptureSizeMismatch() noexcept;

    [[nodiscard]] std::optional<BackgroundCaptureAction> nextAction() const noexcept;
    // Observation must match nextAction(). Only an asynchronous borderless
    // access request may remain pending without advancing the transaction.
    [[nodiscard]] bool applyObservation(
        const BackgroundCaptureAction& action,
        BackgroundCaptureActionObservation observation) noexcept;
    // Preserve the synchronous owner contract while permission acquisition
    // uses the tri-state overload above.
    [[nodiscard]] bool applyObservation(
        const BackgroundCaptureAction& action,
        bool succeeded) noexcept;

    [[nodiscard]] bool transitioning() const noexcept;
    [[nodiscard]] EffectiveBackgroundCapturePath effectivePath() const noexcept;
    [[nodiscard]] BackgroundCaptureFailure failure() const noexcept;
    [[nodiscard]] std::optional<BackgroundCaptureRequest> request() const noexcept;

private:
    void beginFullRequest(const BackgroundCaptureRequest& request) noexcept;
    void beginProfileOnlyRequest(const BackgroundCaptureRequest& request) noexcept;
    void beginResizeOnly(WindowSize outputSize) noexcept;
    void beginBackgroundAwareResize(
        const BackgroundCaptureRequest& request,
        WindowSize outputSize,
        bool stopActiveSensor,
        bool applyProfile) noexcept;
    void beginFxOnlyResize(
        const BackgroundCaptureRequest& request,
        WindowSize outputSize,
        bool stopActiveSensor,
        bool profileOnly) noexcept;
    [[nodiscard]] bool beginActiveSensorFailure(
        BackgroundCaptureFailure failure) noexcept;
    void appendBorderlessAccessRequestIfRequired(
        const BackgroundCaptureRequest& request) noexcept;
    void appendAction(BackgroundCaptureAction action) noexcept;
    void discardRemainingActions() noexcept;
    void finishIfComplete() noexcept;

    std::array<BackgroundCaptureAction, maximumBackgroundCaptureActions> actions_{};
    std::size_t actionCount_{0U};
    std::size_t actionIndex_{0U};
    std::optional<BackgroundCaptureRequest> request_{};
    EffectiveBackgroundCapturePath effectivePath_{
        EffectiveBackgroundCapturePath::FxOnly};
    EffectiveBackgroundCapturePath completionPath_{
        EffectiveBackgroundCapturePath::FxOnly};
    BackgroundCaptureFailure failure_{BackgroundCaptureFailure::None};
    BackgroundCaptureFailure pendingFailure_{BackgroundCaptureFailure::None};
    std::optional<FxOverlayProfile> appliedOverlayProfile_{};
    bool completionVisibilityUnknown_{false};
    bool sensorRestartBlocked_{false};
};

}
