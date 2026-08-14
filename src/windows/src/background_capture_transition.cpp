#include "bafx/windows/background_capture_transition.hpp"

#include <cassert>

namespace bafx::windows
{
namespace
{

[[nodiscard]] BackgroundCaptureAction simpleAction(
    const BackgroundCaptureActionKind kind) noexcept
{
    BackgroundCaptureAction action{};
    action.kind = kind;
    return action;
}

[[nodiscard]] BackgroundCaptureAction profileAction(
    const FxOverlayProfile profile) noexcept
{
    BackgroundCaptureAction action{};
    action.kind = BackgroundCaptureActionKind::ApplyOverlayProfile;
    action.overlayProfile = profile;
    return action;
}

[[nodiscard]] BackgroundCaptureAction startAction(
    const BackgroundCaptureRequest& request) noexcept
{
    BackgroundCaptureAction action{};
    action.kind = BackgroundCaptureActionKind::StartSensor;
    action.cursorExcluded = request.cursorExcluded;
    action.allowSystemBorder = request.allowSystemBorder;
    return action;
}

[[nodiscard]] bool equivalentStableRequest(
    const BackgroundCaptureRequest& left,
    const BackgroundCaptureRequest& right) noexcept
{
    if (left.sensorRequired != right.sensorRequired
        || left.overlayProfile != right.overlayProfile
        || left.retryToken != right.retryToken)
    {
        return false;
    }
    if (!left.sensorRequired)
    {
        // Cursor and border options have no effect while WGC is disabled.
        return true;
    }
    return left.cursorExcluded == right.cursorExcluded
        && left.allowSystemBorder == right.allowSystemBorder;
}

}

bool isValidBackgroundCaptureRequest(
    const BackgroundCaptureRequest& request) noexcept
{
    if (request.sensorRequired)
    {
        return request.overlayProfile == FxOverlayProfile::FxOnlyFallback;
    }
    return request.overlayProfile != FxOverlayProfile::FxOnlyFallback;
}

BackgroundCaptureRequestResult BackgroundCaptureTransition::beginRequest(
    const BackgroundCaptureRequest request) noexcept
{
    if (!isValidBackgroundCaptureRequest(request))
    {
        return BackgroundCaptureRequestResult::InvalidRequest;
    }
    if (transitioning())
    {
        return BackgroundCaptureRequestResult::Busy;
    }
    if (request_.has_value()
        && equivalentStableRequest(*request_, request))
    {
        request_ = request;
        return BackgroundCaptureRequestResult::NoChange;
    }

    const bool profileOnly = request_.has_value()
        && !request_->sensorRequired
        && !request.sensorRequired
        && request_->retryToken == request.retryToken;
    request_ = request;
    if (profileOnly)
    {
        beginProfileOnlyRequest(request);
    }
    else
    {
        beginFullRequest(request);
    }
    return BackgroundCaptureRequestResult::Started;
}

bool BackgroundCaptureTransition::beginSessionStopped() noexcept
{
    if (transitioning()
        || !request_.has_value()
        || !request_->sensorRequired
        || effectivePath_ != EffectiveBackgroundCapturePath::BackgroundAware)
    {
        return false;
    }

    actionCount_ = 0U;
    actionIndex_ = 0U;
    pendingFailure_ = BackgroundCaptureFailure::SessionStopped;
    completionPath_ = EffectiveBackgroundCapturePath::FxOnly;
    completionVisibilityUnknown_ = false;
    appendAction(simpleAction(BackgroundCaptureActionKind::StopSensor));
    appendAction(simpleAction(BackgroundCaptureActionKind::SetAffinityIncluded));
    appendAction(profileAction(FxOverlayProfile::FxOnlyFallback));
    return true;
}

std::optional<BackgroundCaptureAction>
BackgroundCaptureTransition::nextAction() const noexcept
{
    if (!transitioning())
    {
        return std::nullopt;
    }
    return actions_[actionIndex_];
}

bool BackgroundCaptureTransition::applyObservation(
    const BackgroundCaptureAction& action,
    const bool succeeded) noexcept
{
    const std::optional<BackgroundCaptureAction> expected = nextAction();
    if (!expected.has_value() || action != *expected)
    {
        return false;
    }
    if (!succeeded
        && (action.kind == BackgroundCaptureActionKind::StopSensor
            || action.kind == BackgroundCaptureActionKind::ApplyOverlayProfile))
    {
        return false;
    }

    ++actionIndex_;
    switch (action.kind)
    {
    case BackgroundCaptureActionKind::SetAffinityExcluded:
        if (!succeeded)
        {
            pendingFailure_ = BackgroundCaptureFailure::ExclusionUnconfirmed;
            completionPath_ = EffectiveBackgroundCapturePath::FxOnly;
            discardRemainingActions();
            appendAction(simpleAction(
                BackgroundCaptureActionKind::SetAffinityIncluded));
            appendAction(profileAction(FxOverlayProfile::FxOnlyFallback));
        }
        break;
    case BackgroundCaptureActionKind::SetAffinityIncluded:
        if (!succeeded)
        {
            if (pendingFailure_ == BackgroundCaptureFailure::None)
            {
                pendingFailure_ = BackgroundCaptureFailure::InclusionUnconfirmed;
            }
            completionVisibilityUnknown_ = true;
        }
        break;
    case BackgroundCaptureActionKind::StartSensor:
        if (!succeeded)
        {
            pendingFailure_ = BackgroundCaptureFailure::SensorStartFailed;
            completionPath_ = EffectiveBackgroundCapturePath::FxOnly;
            discardRemainingActions();
            appendAction(simpleAction(BackgroundCaptureActionKind::StopSensor));
            appendAction(simpleAction(
                BackgroundCaptureActionKind::SetAffinityIncluded));
        }
        break;
    case BackgroundCaptureActionKind::StopSensor:
    case BackgroundCaptureActionKind::ApplyOverlayProfile:
        break;
    }

    finishIfComplete();
    return true;
}

bool BackgroundCaptureTransition::transitioning() const noexcept
{
    return actionIndex_ < actionCount_;
}

EffectiveBackgroundCapturePath
BackgroundCaptureTransition::effectivePath() const noexcept
{
    return effectivePath_;
}

BackgroundCaptureFailure BackgroundCaptureTransition::failure() const noexcept
{
    return failure_;
}

std::optional<BackgroundCaptureRequest>
BackgroundCaptureTransition::request() const noexcept
{
    return request_;
}

void BackgroundCaptureTransition::beginFullRequest(
    const BackgroundCaptureRequest& request) noexcept
{
    actionCount_ = 0U;
    actionIndex_ = 0U;
    pendingFailure_ = BackgroundCaptureFailure::None;
    completionPath_ = request.sensorRequired
        ? EffectiveBackgroundCapturePath::BackgroundAware
        : EffectiveBackgroundCapturePath::FxOnly;
    completionVisibilityUnknown_ = false;

    appendAction(simpleAction(BackgroundCaptureActionKind::StopSensor));
    if (request.sensorRequired)
    {
        appendAction(simpleAction(
            BackgroundCaptureActionKind::SetAffinityExcluded));
        appendAction(profileAction(request.overlayProfile));
        appendAction(startAction(request));
    }
    else
    {
        appendAction(simpleAction(
            BackgroundCaptureActionKind::SetAffinityIncluded));
        appendAction(profileAction(request.overlayProfile));
    }
}

void BackgroundCaptureTransition::beginProfileOnlyRequest(
    const BackgroundCaptureRequest& request) noexcept
{
    actionCount_ = 0U;
    actionIndex_ = 0U;
    pendingFailure_ = failure_;
    completionPath_ = effectivePath_;
    completionVisibilityUnknown_ =
        effectivePath_ == EffectiveBackgroundCapturePath::
            FxOnlyCaptureVisibilityUnknown;
    appendAction(profileAction(request.overlayProfile));
}

void BackgroundCaptureTransition::appendAction(
    const BackgroundCaptureAction action) noexcept
{
    assert(actionCount_ < actions_.size());
    if (actionCount_ < actions_.size())
    {
        actions_[actionCount_] = action;
        ++actionCount_;
    }
}

void BackgroundCaptureTransition::discardRemainingActions() noexcept
{
    actionCount_ = actionIndex_;
}

void BackgroundCaptureTransition::finishIfComplete() noexcept
{
    if (transitioning())
    {
        return;
    }
    effectivePath_ = completionVisibilityUnknown_
        ? EffectiveBackgroundCapturePath::FxOnlyCaptureVisibilityUnknown
        : completionPath_;
    failure_ = pendingFailure_;
}

}
