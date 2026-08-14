#pragma once

#include "bafx/windows/fx_gpu_renderer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace bafx::windows
{

inline constexpr std::size_t maximumBackgroundCaptureActions = 6U;

struct BackgroundCaptureRequest
{
    bool sensorRequired{false};
    FxOverlayProfile overlayProfile{FxOverlayProfile::RecordingCompatible};
    bool cursorExcluded{true};
    bool allowSystemBorder{true};
    // A stable request is terminal after failure. Increment this only for an
    // explicit retry so the render loop cannot restart WGC indefinitely.
    std::uint64_t retryToken{0U};

    [[nodiscard]] bool operator==(
        const BackgroundCaptureRequest&) const noexcept = default;
};

enum class BackgroundCaptureActionKind : std::uint8_t
{
    StopSensor,
    SetAffinityExcluded,
    SetAffinityIncluded,
    ApplyOverlayProfile,
    StartSensor
};

struct BackgroundCaptureAction
{
    BackgroundCaptureActionKind kind{BackgroundCaptureActionKind::StopSensor};
    FxOverlayProfile overlayProfile{FxOverlayProfile::FxOnlyFallback};
    bool cursorExcluded{true};
    bool allowSystemBorder{true};

    [[nodiscard]] bool operator==(
        const BackgroundCaptureAction&) const noexcept = default;
};

enum class EffectiveBackgroundCapturePath : std::uint8_t
{
    BackgroundAware,
    FxOnly,
    FxOnlyCaptureVisibilityUnknown
};

enum class BackgroundCaptureFailure : std::uint8_t
{
    None,
    ExclusionUnconfirmed,
    SensorStartFailed,
    InclusionUnconfirmed,
    SessionStopped
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
    [[nodiscard]] BackgroundCaptureRequestResult beginRequest(
        BackgroundCaptureRequest request) noexcept;
    [[nodiscard]] bool beginSessionStopped() noexcept;

    [[nodiscard]] std::optional<BackgroundCaptureAction> nextAction() const noexcept;
    // Observation must match nextAction(). Stop/Profile are infallible owner
    // operations and therefore reject a false observation without advancing.
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
    bool completionVisibilityUnknown_{false};
};

}
