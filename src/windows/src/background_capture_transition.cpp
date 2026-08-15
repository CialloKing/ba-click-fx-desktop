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

[[nodiscard]] BackgroundCaptureAction resizeAction(
    const WindowSize outputSize) noexcept
{
    BackgroundCaptureAction action{};
    action.kind = BackgroundCaptureActionKind::ResizeOutput;
    action.outputSize = outputSize;
    return action;
}

[[nodiscard]] BackgroundCaptureAction recreateFramePoolAction(
    const WindowSize captureSize) noexcept
{
    BackgroundCaptureAction action{};
    action.kind = BackgroundCaptureActionKind::RecreateFramePool;
    action.captureSize = captureSize;
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

[[nodiscard]] BackgroundCaptureAction borderlessAccessAction() noexcept
{
    BackgroundCaptureAction action{};
    action.kind = BackgroundCaptureActionKind::RequestBorderlessAccess;
    action.allowSystemBorder = false;
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

bool BackgroundCaptureAction::operator==(
    const BackgroundCaptureAction& other) const noexcept
{
    return kind == other.kind
        && overlayProfile == other.overlayProfile
        && outputSize.width == other.outputSize.width
        && outputSize.height == other.outputSize.height
        && captureSize.width == other.captureSize.width
        && captureSize.height == other.captureSize.height
        && cursorExcluded == other.cursorExcluded
        && allowSystemBorder == other.allowSystemBorder;
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
    return beginIntent(request, std::nullopt);
}

BackgroundCaptureRequestResult BackgroundCaptureTransition::beginIntent(
    const BackgroundCaptureRequest request,
    const std::optional<WindowSize> outputSize) noexcept
{
    if (!isValidBackgroundCaptureRequest(request))
    {
        return BackgroundCaptureRequestResult::InvalidRequest;
    }
    if (outputSize.has_value()
        && (outputSize->width == 0U || outputSize->height == 0U))
    {
        return BackgroundCaptureRequestResult::InvalidRequest;
    }
    if (transitioning())
    {
        return BackgroundCaptureRequestResult::Busy;
    }
    if (sensorRestartBlocked_ && request.sensorRequired)
    {
        // A failed WinRT stop leaves resource ownership uncertain. Resizing
        // the FX-only transport is safe, but this process must not create a
        // replacement capture session on top of possibly live resources.
        request_ = request;
        if (!outputSize.has_value())
        {
            return BackgroundCaptureRequestResult::NoChange;
        }
        beginResizeOnly(*outputSize);
        return BackgroundCaptureRequestResult::Started;
    }
    const bool stableRequest = request_.has_value()
        && equivalentStableRequest(*request_, request);
    if (stableRequest && !outputSize.has_value())
    {
        request_ = request;
        return BackgroundCaptureRequestResult::NoChange;
    }

    const bool profileOnly = request_.has_value()
        && !request_->sensorRequired
        && !request.sensorRequired
        && request_->retryToken == request.retryToken;
    const bool activeBackgroundAware =
        effectivePath_ == EffectiveBackgroundCapturePath::BackgroundAware;
    const bool applyProfile = !appliedOverlayProfile_.has_value()
        || *appliedOverlayProfile_ != request.overlayProfile;
    request_ = request;
    if (!outputSize.has_value())
    {
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

    if (stableRequest)
    {
        if (activeBackgroundAware)
        {
            beginBackgroundAwareResize(
                request,
                *outputSize,
                true,
                false);
        }
        else
        {
            // A failed stable request is terminal. Resize its output without
            // turning an unrelated window event into an implicit WGC retry.
            beginResizeOnly(*outputSize);
        }
    }
    else if (request.sensorRequired)
    {
        // An active sensor already uses the only valid sensor profile. Avoid a
        // redundant profile action so Stop+Included rollback still fits six.
        beginBackgroundAwareResize(
            request,
            *outputSize,
            activeBackgroundAware,
            !activeBackgroundAware && applyProfile);
    }
    else
    {
        beginFxOnlyResize(
            request,
            *outputSize,
            activeBackgroundAware,
            profileOnly);
    }
    return BackgroundCaptureRequestResult::Started;
}

bool BackgroundCaptureTransition::beginFramePoolRecreate(
    const WindowSize captureSize) noexcept
{
    if (transitioning()
        || captureSize.width == 0U
        || captureSize.height == 0U
        || !request_.has_value()
        || !request_->sensorRequired
        || effectivePath_ != EffectiveBackgroundCapturePath::BackgroundAware)
    {
        return false;
    }

    actionCount_ = 0U;
    actionIndex_ = 0U;
    pendingFailure_ = BackgroundCaptureFailure::None;
    completionPath_ = EffectiveBackgroundCapturePath::BackgroundAware;
    completionVisibilityUnknown_ = false;
    appendAction(recreateFramePoolAction(captureSize));
    return true;
}

bool BackgroundCaptureTransition::beginSessionStopped() noexcept
{
    return beginActiveSensorFailure(BackgroundCaptureFailure::SessionStopped);
}

bool BackgroundCaptureTransition::beginCaptureExclusionLost() noexcept
{
    return beginActiveSensorFailure(
        BackgroundCaptureFailure::ExclusionUnconfirmed);
}

bool BackgroundCaptureTransition::beginActiveSensorFailure(
    const BackgroundCaptureFailure failure) noexcept
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
    pendingFailure_ = failure;
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
    return applyObservation(
        action,
        succeeded
            ? BackgroundCaptureActionObservation::Succeeded
            : BackgroundCaptureActionObservation::Failed);
}

bool BackgroundCaptureTransition::applyObservation(
    const BackgroundCaptureAction& action,
    const BackgroundCaptureActionObservation observation) noexcept
{
    const std::optional<BackgroundCaptureAction> expected = nextAction();
    if (!expected.has_value() || action != *expected)
    {
        return false;
    }
    if (observation == BackgroundCaptureActionObservation::Pending)
    {
        // Permission completion is delivered asynchronously. All owner-side
        // actions are synchronous and must either succeed or fail immediately.
        return action.kind
            == BackgroundCaptureActionKind::RequestBorderlessAccess;
    }
    if (observation != BackgroundCaptureActionObservation::Succeeded
        && observation != BackgroundCaptureActionObservation::Failed
        && observation != BackgroundCaptureActionObservation::Canceled)
    {
        return false;
    }
    if (observation == BackgroundCaptureActionObservation::Canceled
        && action.kind
            != BackgroundCaptureActionKind::RequestBorderlessAccess)
    {
        return false;
    }

    const bool succeeded =
        observation == BackgroundCaptureActionObservation::Succeeded;
    if (!succeeded
        && (action.kind == BackgroundCaptureActionKind::ResizeOutput
            || action.kind == BackgroundCaptureActionKind::ApplyOverlayProfile))
    {
        return false;
    }

    ++actionIndex_;
    switch (action.kind)
    {
    case BackgroundCaptureActionKind::RequestBorderlessAccess:
        if (!succeeded)
        {
            const bool ownerCanceled =
                observation == BackgroundCaptureActionObservation::Canceled;
            std::optional<BackgroundCaptureAction> pendingResize{};
            for (std::size_t index = actionIndex_; index < actionCount_; ++index)
            {
                if (actions_[index].kind
                    == BackgroundCaptureActionKind::ResizeOutput)
                {
                    pendingResize = actions_[index];
                    break;
                }
            }

            pendingFailure_ = ownerCanceled
                ? BackgroundCaptureFailure::BorderlessAccessCanceled
                : BackgroundCaptureFailure::BorderlessAccessFailed;
            completionPath_ = EffectiveBackgroundCapturePath::FxOnly;
            completionVisibilityUnknown_ = false;
            discardRemainingActions();
            // Permission is decided before ownership or visibility changes.
            // A denial therefore runs one explicit FX-only rollback and keeps
            // a coalesced output resize from being lost with the start plan.
            appendAction(simpleAction(BackgroundCaptureActionKind::StopSensor));
            appendAction(simpleAction(
                BackgroundCaptureActionKind::SetAffinityIncluded));
            if (pendingResize.has_value())
            {
                appendAction(*pendingResize);
            }
            if (!appliedOverlayProfile_.has_value()
                || *appliedOverlayProfile_ != FxOverlayProfile::FxOnlyFallback)
            {
                appendAction(profileAction(FxOverlayProfile::FxOnlyFallback));
            }
            if (ownerCanceled)
            {
                // A superseding owner intent must be allowed to submit the
                // same capture identity under its new generation. Broker
                // denial remains terminal until an explicit retry token.
                request_.reset();
            }
        }
        break;
    case BackgroundCaptureActionKind::StopSensor:
        if (!succeeded)
        {
            pendingFailure_ = BackgroundCaptureFailure::SensorStopFailed;
            completionPath_ = EffectiveBackgroundCapturePath::FxOnly;
            completionVisibilityUnknown_ = false;
            sensorRestartBlocked_ = true;
            discardRemainingActions();
            appendAction(simpleAction(
                BackgroundCaptureActionKind::SetAffinityIncluded));
            if (!appliedOverlayProfile_.has_value()
                || *appliedOverlayProfile_ != FxOverlayProfile::FxOnlyFallback)
            {
                appendAction(profileAction(FxOverlayProfile::FxOnlyFallback));
            }
        }
        break;
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
    case BackgroundCaptureActionKind::RecreateFramePool:
        if (!succeeded)
        {
            pendingFailure_ =
                BackgroundCaptureFailure::FramePoolRecreateFailed;
            completionPath_ = EffectiveBackgroundCapturePath::FxOnly;
            discardRemainingActions();
            appendAction(simpleAction(BackgroundCaptureActionKind::StopSensor));
            appendAction(simpleAction(
                BackgroundCaptureActionKind::SetAffinityIncluded));
            appendAction(profileAction(FxOverlayProfile::FxOnlyFallback));
        }
        break;
    case BackgroundCaptureActionKind::ApplyOverlayProfile:
        appliedOverlayProfile_ = action.overlayProfile;
        break;
    case BackgroundCaptureActionKind::ResizeOutput:
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
    pendingFailure_ = sensorRestartBlocked_
        ? BackgroundCaptureFailure::SensorStopFailed
        : BackgroundCaptureFailure::None;
    completionPath_ = request.sensorRequired
        ? EffectiveBackgroundCapturePath::BackgroundAware
        : EffectiveBackgroundCapturePath::FxOnly;
    completionVisibilityUnknown_ = false;

    appendBorderlessAccessRequestIfRequired(request);
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

void BackgroundCaptureTransition::beginResizeOnly(
    const WindowSize outputSize) noexcept
{
    actionCount_ = 0U;
    actionIndex_ = 0U;
    pendingFailure_ = failure_;
    completionPath_ = effectivePath_;
    completionVisibilityUnknown_ =
        effectivePath_ == EffectiveBackgroundCapturePath::
            FxOnlyCaptureVisibilityUnknown;
    appendAction(resizeAction(outputSize));
}

void BackgroundCaptureTransition::beginBackgroundAwareResize(
    const BackgroundCaptureRequest& request,
    const WindowSize outputSize,
    const bool stopActiveSensor,
    const bool applyProfile) noexcept
{
    actionCount_ = 0U;
    actionIndex_ = 0U;
    pendingFailure_ = BackgroundCaptureFailure::None;
    completionPath_ = EffectiveBackgroundCapturePath::BackgroundAware;
    completionVisibilityUnknown_ = false;

    appendBorderlessAccessRequestIfRequired(request);
    if (stopActiveSensor)
    {
        appendAction(simpleAction(BackgroundCaptureActionKind::StopSensor));
    }
    appendAction(resizeAction(outputSize));
    appendAction(simpleAction(BackgroundCaptureActionKind::SetAffinityExcluded));
    if (applyProfile)
    {
        appendAction(profileAction(request.overlayProfile));
    }
    appendAction(startAction(request));
}

void BackgroundCaptureTransition::beginFxOnlyResize(
    const BackgroundCaptureRequest& request,
    const WindowSize outputSize,
    const bool stopActiveSensor,
    const bool profileOnly) noexcept
{
    actionCount_ = 0U;
    actionIndex_ = 0U;
    if (profileOnly)
    {
        // A profile-only change cannot repair an earlier failure to restore
        // capture visibility. Resize must preserve that diagnostic contract.
        pendingFailure_ = failure_;
        completionPath_ = effectivePath_;
        completionVisibilityUnknown_ =
            effectivePath_ == EffectiveBackgroundCapturePath::
                FxOnlyCaptureVisibilityUnknown;
    }
    else
    {
        pendingFailure_ = sensorRestartBlocked_
            ? BackgroundCaptureFailure::SensorStopFailed
            : BackgroundCaptureFailure::None;
        completionPath_ = EffectiveBackgroundCapturePath::FxOnly;
        completionVisibilityUnknown_ = false;
    }

    if (stopActiveSensor || !profileOnly)
    {
        appendAction(simpleAction(BackgroundCaptureActionKind::StopSensor));
        appendAction(simpleAction(BackgroundCaptureActionKind::SetAffinityIncluded));
    }
    appendAction(resizeAction(outputSize));
    appendAction(profileAction(request.overlayProfile));
}

void BackgroundCaptureTransition::appendBorderlessAccessRequestIfRequired(
    const BackgroundCaptureRequest& request) noexcept
{
    if (request.sensorRequired && !request.allowSystemBorder)
    {
        // Permission must resolve before stopping an existing producer or
        // changing WDA/profile state, so denial has no speculative side effect.
        appendAction(borderlessAccessAction());
    }
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
