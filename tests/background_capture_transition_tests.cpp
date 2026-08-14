#include "test_support.hpp"

#include "bafx/windows/background_capture_transition.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

using namespace bafx::windows;

namespace
{

constexpr WindowSize resizedOutput{2560U, 1440U};

[[nodiscard]] BackgroundCaptureRequest backgroundAwareRequest(
    const bool cursorExcluded = true,
    const bool allowSystemBorder = true,
    const std::uint64_t retryToken = 0U)
{
    return BackgroundCaptureRequest{
        true,
        FxOverlayProfile::FxOnlyFallback,
        cursorExcluded,
        allowSystemBorder,
        retryToken};
}

[[nodiscard]] BackgroundCaptureRequest recordingRequest(
    const FxOverlayProfile profile = FxOverlayProfile::RecordingCompatible,
    const std::uint64_t retryToken = 0U)
{
    return BackgroundCaptureRequest{false, profile, true, true, retryToken};
}

[[nodiscard]] std::vector<BackgroundCaptureActionKind> completeSuccessfully(
    BackgroundCaptureTransition& transition,
    const std::optional<WindowSize> expectedOutputSize = std::nullopt)
{
    std::vector<BackgroundCaptureActionKind> actions;
    while (const auto action = transition.nextAction())
    {
        BAFX_CHECK(actions.size() < maximumBackgroundCaptureActions);
        actions.push_back(action->kind);
        if (action->kind == BackgroundCaptureActionKind::ResizeOutput)
        {
            BAFX_CHECK(expectedOutputSize.has_value());
            BAFX_CHECK(action->outputSize.width == expectedOutputSize->width);
            BAFX_CHECK(action->outputSize.height == expectedOutputSize->height);
        }
        BAFX_CHECK(transition.applyObservation(*action, true));
    }
    return actions;
}

void checkActions(
    const std::vector<BackgroundCaptureActionKind>& actual,
    const std::initializer_list<BackgroundCaptureActionKind> expected)
{
    BAFX_CHECK(actual == std::vector<BackgroundCaptureActionKind>(expected));
}

}

BAFX_TEST(background_capture_rejects_incoherent_sensor_profiles)
{
    BackgroundCaptureTransition transition;
    BAFX_CHECK(
        transition.beginRequest(BackgroundCaptureRequest{
            true,
            FxOverlayProfile::RecordingCompatible,
            true,
            true,
            0U}) == BackgroundCaptureRequestResult::InvalidRequest);
    BAFX_CHECK(!transition.nextAction().has_value());
}

BAFX_TEST(background_aware_uses_the_transactional_start_order)
{
    BackgroundCaptureTransition transition;
    BAFX_CHECK(
        transition.beginRequest(backgroundAwareRequest())
        == BackgroundCaptureRequestResult::Started);

    checkActions(
        completeSuccessfully(transition),
        {BackgroundCaptureActionKind::StopSensor,
         BackgroundCaptureActionKind::SetAffinityExcluded,
         BackgroundCaptureActionKind::ApplyOverlayProfile,
         BackgroundCaptureActionKind::StartSensor});
    BAFX_CHECK(
        transition.effectivePath()
        == EffectiveBackgroundCapturePath::BackgroundAware);
    BAFX_CHECK(transition.failure() == BackgroundCaptureFailure::None);
}

BAFX_TEST(recording_and_light_modes_stop_include_then_apply_profile)
{
    for (const FxOverlayProfile profile :
         std::array{
             FxOverlayProfile::RecordingCompatible,
             FxOverlayProfile::LightBackground})
    {
        BackgroundCaptureTransition transition;
        BAFX_CHECK(
            transition.beginRequest(recordingRequest(profile))
            == BackgroundCaptureRequestResult::Started);
        checkActions(
            completeSuccessfully(transition),
            {BackgroundCaptureActionKind::StopSensor,
             BackgroundCaptureActionKind::SetAffinityIncluded,
             BackgroundCaptureActionKind::ApplyOverlayProfile});
        BAFX_CHECK(
            transition.effectivePath()
            == EffectiveBackgroundCapturePath::FxOnly);
    }
}

BAFX_TEST(recording_and_light_switch_only_changes_the_profile)
{
    BackgroundCaptureTransition transition;
    BAFX_CHECK(
        transition.beginRequest(recordingRequest())
        == BackgroundCaptureRequestResult::Started);
    static_cast<void>(completeSuccessfully(transition));

    BAFX_CHECK(
        transition.beginRequest(recordingRequest(FxOverlayProfile::LightBackground))
        == BackgroundCaptureRequestResult::Started);
    const auto action = transition.nextAction();
    BAFX_CHECK(action.has_value());
    BAFX_CHECK(action->kind == BackgroundCaptureActionKind::ApplyOverlayProfile);
    BAFX_CHECK(action->overlayProfile == FxOverlayProfile::LightBackground);
    BAFX_CHECK(transition.applyObservation(*action, true));
    BAFX_CHECK(!transition.nextAction().has_value());
}

BAFX_TEST(background_aware_option_change_restarts_once)
{
    BackgroundCaptureTransition transition;
    BAFX_CHECK(
        transition.beginRequest(backgroundAwareRequest())
        == BackgroundCaptureRequestResult::Started);
    static_cast<void>(completeSuccessfully(transition));

    BAFX_CHECK(
        transition.beginRequest(backgroundAwareRequest(false, false))
        == BackgroundCaptureRequestResult::Started);
    checkActions(
        completeSuccessfully(transition),
        {BackgroundCaptureActionKind::StopSensor,
         BackgroundCaptureActionKind::SetAffinityExcluded,
         BackgroundCaptureActionKind::ApplyOverlayProfile,
         BackgroundCaptureActionKind::StartSensor});
}

BAFX_TEST(exclusion_failure_rolls_back_without_starting_the_sensor)
{
    BackgroundCaptureTransition transition;
    BAFX_CHECK(
        transition.beginRequest(backgroundAwareRequest())
        == BackgroundCaptureRequestResult::Started);

    auto action = transition.nextAction();
    BAFX_CHECK(action.has_value());
    BAFX_CHECK(transition.applyObservation(*action, true));
    action = transition.nextAction();
    BAFX_CHECK(action.has_value());
    BAFX_CHECK(action->kind == BackgroundCaptureActionKind::SetAffinityExcluded);
    BAFX_CHECK(transition.applyObservation(*action, false));

    checkActions(
        completeSuccessfully(transition),
        {BackgroundCaptureActionKind::SetAffinityIncluded,
         BackgroundCaptureActionKind::ApplyOverlayProfile});
    BAFX_CHECK(
        transition.effectivePath() == EffectiveBackgroundCapturePath::FxOnly);
    BAFX_CHECK(
        transition.failure()
        == BackgroundCaptureFailure::ExclusionUnconfirmed);
}

BAFX_TEST(sensor_start_failure_stops_partial_state_and_restores_visibility)
{
    BackgroundCaptureTransition transition;
    BAFX_CHECK(
        transition.beginRequest(backgroundAwareRequest())
        == BackgroundCaptureRequestResult::Started);

    for (std::size_t index = 0U; index < 3U; ++index)
    {
        const auto action = transition.nextAction();
        BAFX_CHECK(action.has_value());
        BAFX_CHECK(transition.applyObservation(*action, true));
    }
    const auto start = transition.nextAction();
    BAFX_CHECK(start.has_value());
    BAFX_CHECK(start->kind == BackgroundCaptureActionKind::StartSensor);
    BAFX_CHECK(transition.applyObservation(*start, false));

    const std::vector<BackgroundCaptureActionKind> cleanupActions =
        completeSuccessfully(transition);
    checkActions(
        cleanupActions,
        {BackgroundCaptureActionKind::StopSensor,
         BackgroundCaptureActionKind::SetAffinityIncluded});
    BAFX_CHECK(4U + cleanupActions.size() == maximumBackgroundCaptureActions);
    BAFX_CHECK(
        transition.failure() == BackgroundCaptureFailure::SensorStartFailed);
    BAFX_CHECK(
        transition.effectivePath() == EffectiveBackgroundCapturePath::FxOnly);
}

BAFX_TEST(inclusion_failure_terminates_with_unknown_capture_visibility)
{
    BackgroundCaptureTransition transition;
    BAFX_CHECK(
        transition.beginRequest(recordingRequest())
        == BackgroundCaptureRequestResult::Started);

    auto action = transition.nextAction();
    BAFX_CHECK(action.has_value());
    BAFX_CHECK(transition.applyObservation(*action, true));
    action = transition.nextAction();
    BAFX_CHECK(action.has_value());
    BAFX_CHECK(action->kind == BackgroundCaptureActionKind::SetAffinityIncluded);
    BAFX_CHECK(transition.applyObservation(*action, false));
    static_cast<void>(completeSuccessfully(transition));

    BAFX_CHECK(
        transition.effectivePath()
        == EffectiveBackgroundCapturePath::FxOnlyCaptureVisibilityUnknown);
    BAFX_CHECK(
        transition.failure() == BackgroundCaptureFailure::InclusionUnconfirmed);
}

BAFX_TEST(session_stop_cleans_up_without_retrying_the_same_request)
{
    BackgroundCaptureTransition transition;
    BAFX_CHECK(
        transition.beginRequest(backgroundAwareRequest())
        == BackgroundCaptureRequestResult::Started);
    static_cast<void>(completeSuccessfully(transition));

    BAFX_CHECK(transition.beginSessionStopped());
    checkActions(
        completeSuccessfully(transition),
        {BackgroundCaptureActionKind::StopSensor,
         BackgroundCaptureActionKind::SetAffinityIncluded,
         BackgroundCaptureActionKind::ApplyOverlayProfile});
    BAFX_CHECK(transition.failure() == BackgroundCaptureFailure::SessionStopped);
    BAFX_CHECK(
        transition.beginRequest(backgroundAwareRequest())
        == BackgroundCaptureRequestResult::NoChange);
    BAFX_CHECK(!transition.nextAction().has_value());

    BAFX_CHECK(
        transition.beginRequest(backgroundAwareRequest(true, true, 1U))
        == BackgroundCaptureRequestResult::Started);
}

BAFX_TEST(stable_request_is_idempotent_and_busy_transition_rejects_reentry)
{
    BackgroundCaptureTransition transition;
    const BackgroundCaptureRequest request = recordingRequest();
    BAFX_CHECK(
        transition.beginRequest(request) == BackgroundCaptureRequestResult::Started);
    BAFX_CHECK(
        transition.beginRequest(request) == BackgroundCaptureRequestResult::Busy);
    static_cast<void>(completeSuccessfully(transition));
    BAFX_CHECK(
        transition.beginRequest(request) == BackgroundCaptureRequestResult::NoChange);
}

BAFX_TEST(out_of_order_observation_is_rejected_without_advancing)
{
    BackgroundCaptureTransition transition;
    BAFX_CHECK(
        transition.beginRequest(backgroundAwareRequest())
        == BackgroundCaptureRequestResult::Started);
    const auto expected = transition.nextAction();
    BAFX_CHECK(expected.has_value());

    BackgroundCaptureAction wrong{};
    wrong.kind = BackgroundCaptureActionKind::StartSensor;
    BAFX_CHECK(!transition.applyObservation(wrong, true));
    BAFX_CHECK(transition.nextAction() == expected);
}

BAFX_TEST(background_aware_resize_restarts_in_resize_safe_order)
{
    BackgroundCaptureTransition transition;
    const BackgroundCaptureRequest request = backgroundAwareRequest();
    BAFX_CHECK(
        transition.beginRequest(request) == BackgroundCaptureRequestResult::Started);
    static_cast<void>(completeSuccessfully(transition));

    BAFX_CHECK(
        transition.beginIntent(request, resizedOutput)
        == BackgroundCaptureRequestResult::Started);
    checkActions(
        completeSuccessfully(transition, resizedOutput),
        {BackgroundCaptureActionKind::StopSensor,
         BackgroundCaptureActionKind::ResizeOutput,
         BackgroundCaptureActionKind::SetAffinityExcluded,
         BackgroundCaptureActionKind::StartSensor});
    BAFX_CHECK(
        transition.effectivePath()
        == EffectiveBackgroundCapturePath::BackgroundAware);
    BAFX_CHECK(transition.failure() == BackgroundCaptureFailure::None);
}

BAFX_TEST(fx_only_resize_does_not_restart_a_stable_request)
{
    BackgroundCaptureTransition transition;
    const BackgroundCaptureRequest request = recordingRequest();
    BAFX_CHECK(
        transition.beginRequest(request) == BackgroundCaptureRequestResult::Started);
    static_cast<void>(completeSuccessfully(transition));

    BAFX_CHECK(
        transition.beginIntent(request, resizedOutput)
        == BackgroundCaptureRequestResult::Started);
    const auto resize = transition.nextAction();
    BAFX_CHECK(resize.has_value());
    BAFX_CHECK(resize->kind == BackgroundCaptureActionKind::ResizeOutput);
    BAFX_CHECK(resize->outputSize.width == resizedOutput.width);
    BAFX_CHECK(resize->outputSize.height == resizedOutput.height);
    BAFX_CHECK(!transition.applyObservation(*resize, false));
    BAFX_CHECK(transition.nextAction() == resize);
    BAFX_CHECK(transition.applyObservation(*resize, true));
    BAFX_CHECK(!transition.nextAction().has_value());
    BAFX_CHECK(
        transition.effectivePath() == EffectiveBackgroundCapturePath::FxOnly);
}

BAFX_TEST(failed_background_aware_resize_is_terminal_without_explicit_retry)
{
    BackgroundCaptureTransition transition;
    const BackgroundCaptureRequest request = backgroundAwareRequest();
    BAFX_CHECK(
        transition.beginRequest(request) == BackgroundCaptureRequestResult::Started);
    for (std::size_t index = 0U; index < 3U; ++index)
    {
        const auto action = transition.nextAction();
        BAFX_CHECK(action.has_value());
        BAFX_CHECK(transition.applyObservation(*action, true));
    }
    const auto start = transition.nextAction();
    BAFX_CHECK(start.has_value());
    BAFX_CHECK(start->kind == BackgroundCaptureActionKind::StartSensor);
    BAFX_CHECK(transition.applyObservation(*start, false));
    static_cast<void>(completeSuccessfully(transition));

    BAFX_CHECK(
        transition.beginIntent(request, resizedOutput)
        == BackgroundCaptureRequestResult::Started);
    checkActions(
        completeSuccessfully(transition, resizedOutput),
        {BackgroundCaptureActionKind::ResizeOutput});
    BAFX_CHECK(
        transition.effectivePath() == EffectiveBackgroundCapturePath::FxOnly);
    BAFX_CHECK(
        transition.failure() == BackgroundCaptureFailure::SensorStartFailed);
}

BAFX_TEST(background_aware_configuration_and_resize_share_one_transaction)
{
    BackgroundCaptureTransition transition;
    BAFX_CHECK(
        transition.beginRequest(recordingRequest())
        == BackgroundCaptureRequestResult::Started);
    static_cast<void>(completeSuccessfully(transition));

    BAFX_CHECK(
        transition.beginIntent(backgroundAwareRequest(), resizedOutput)
        == BackgroundCaptureRequestResult::Started);
    checkActions(
        completeSuccessfully(transition, resizedOutput),
        {BackgroundCaptureActionKind::ResizeOutput,
         BackgroundCaptureActionKind::SetAffinityExcluded,
         BackgroundCaptureActionKind::ApplyOverlayProfile,
         BackgroundCaptureActionKind::StartSensor});
}

BAFX_TEST(background_aware_resize_start_failure_stays_within_action_budget)
{
    BackgroundCaptureTransition transition;
    BAFX_CHECK(
        transition.beginRequest(recordingRequest())
        == BackgroundCaptureRequestResult::Started);
    static_cast<void>(completeSuccessfully(transition));
    BAFX_CHECK(
        transition.beginIntent(backgroundAwareRequest(), resizedOutput)
        == BackgroundCaptureRequestResult::Started);

    std::vector<BackgroundCaptureActionKind> actions;
    while (const auto action = transition.nextAction())
    {
        BAFX_CHECK(actions.size() < maximumBackgroundCaptureActions);
        actions.push_back(action->kind);
        const bool succeeded =
            action->kind != BackgroundCaptureActionKind::StartSensor;
        BAFX_CHECK(transition.applyObservation(*action, succeeded));
    }
    checkActions(
        actions,
        {BackgroundCaptureActionKind::ResizeOutput,
         BackgroundCaptureActionKind::SetAffinityExcluded,
         BackgroundCaptureActionKind::ApplyOverlayProfile,
         BackgroundCaptureActionKind::StartSensor,
         BackgroundCaptureActionKind::StopSensor,
         BackgroundCaptureActionKind::SetAffinityIncluded});
    BAFX_CHECK(actions.size() == maximumBackgroundCaptureActions);
    BAFX_CHECK(
        transition.failure() == BackgroundCaptureFailure::SensorStartFailed);
}

BAFX_TEST(explicit_retry_reuses_the_current_profile_during_resize)
{
    BackgroundCaptureTransition transition;
    const BackgroundCaptureRequest request = backgroundAwareRequest();
    BAFX_CHECK(
        transition.beginRequest(request) == BackgroundCaptureRequestResult::Started);
    for (std::size_t index = 0U; index < 3U; ++index)
    {
        const auto action = transition.nextAction();
        BAFX_CHECK(action.has_value());
        BAFX_CHECK(transition.applyObservation(*action, true));
    }
    const auto start = transition.nextAction();
    BAFX_CHECK(start.has_value());
    BAFX_CHECK(transition.applyObservation(*start, false));
    static_cast<void>(completeSuccessfully(transition));

    BAFX_CHECK(
        transition.beginIntent(
            backgroundAwareRequest(true, true, 1U),
            resizedOutput)
        == BackgroundCaptureRequestResult::Started);
    checkActions(
        completeSuccessfully(transition, resizedOutput),
        {BackgroundCaptureActionKind::ResizeOutput,
         BackgroundCaptureActionKind::SetAffinityExcluded,
         BackgroundCaptureActionKind::StartSensor});
    BAFX_CHECK(transition.failure() == BackgroundCaptureFailure::None);
}

BAFX_TEST(leaving_active_background_aware_resizes_after_restoring_visibility)
{
    BackgroundCaptureTransition transition;
    BAFX_CHECK(
        transition.beginRequest(backgroundAwareRequest())
        == BackgroundCaptureRequestResult::Started);
    static_cast<void>(completeSuccessfully(transition));

    BAFX_CHECK(
        transition.beginIntent(recordingRequest(), resizedOutput)
        == BackgroundCaptureRequestResult::Started);
    checkActions(
        completeSuccessfully(transition, resizedOutput),
        {BackgroundCaptureActionKind::StopSensor,
         BackgroundCaptureActionKind::SetAffinityIncluded,
         BackgroundCaptureActionKind::ResizeOutput,
         BackgroundCaptureActionKind::ApplyOverlayProfile});
    BAFX_CHECK(
        transition.effectivePath() == EffectiveBackgroundCapturePath::FxOnly);
}

BAFX_TEST(fx_only_profile_change_and_resize_avoid_capture_operations)
{
    BackgroundCaptureTransition transition;
    BAFX_CHECK(
        transition.beginRequest(recordingRequest())
        == BackgroundCaptureRequestResult::Started);
    static_cast<void>(completeSuccessfully(transition));

    BAFX_CHECK(
        transition.beginIntent(
            recordingRequest(FxOverlayProfile::LightBackground),
            resizedOutput)
        == BackgroundCaptureRequestResult::Started);
    checkActions(
        completeSuccessfully(transition, resizedOutput),
        {BackgroundCaptureActionKind::ResizeOutput,
         BackgroundCaptureActionKind::ApplyOverlayProfile});
}

BAFX_TEST(background_capture_resize_rejects_zero_output_dimensions)
{
    BackgroundCaptureTransition transition;
    BAFX_CHECK(
        transition.beginRequest(recordingRequest())
        == BackgroundCaptureRequestResult::Started);
    static_cast<void>(completeSuccessfully(transition));

    BAFX_CHECK(
        transition.beginIntent(recordingRequest(), WindowSize{0U, 1440U})
        == BackgroundCaptureRequestResult::InvalidRequest);
    BAFX_CHECK(!transition.nextAction().has_value());
}

BAFX_TEST(fx_only_profile_resize_preserves_unknown_capture_visibility)
{
    BackgroundCaptureTransition transition;
    BAFX_CHECK(
        transition.beginRequest(recordingRequest())
        == BackgroundCaptureRequestResult::Started);

    auto action = transition.nextAction();
    BAFX_CHECK(action.has_value());
    BAFX_CHECK(transition.applyObservation(*action, true));
    action = transition.nextAction();
    BAFX_CHECK(action.has_value());
    BAFX_CHECK(
        action->kind == BackgroundCaptureActionKind::SetAffinityIncluded);
    BAFX_CHECK(transition.applyObservation(*action, false));
    static_cast<void>(completeSuccessfully(transition));
    BAFX_CHECK(
        transition.effectivePath()
        == EffectiveBackgroundCapturePath::FxOnlyCaptureVisibilityUnknown);

    BAFX_CHECK(
        transition.beginIntent(
            recordingRequest(FxOverlayProfile::LightBackground),
            resizedOutput)
        == BackgroundCaptureRequestResult::Started);
    checkActions(
        completeSuccessfully(transition, resizedOutput),
        {BackgroundCaptureActionKind::ResizeOutput,
         BackgroundCaptureActionKind::ApplyOverlayProfile});
    BAFX_CHECK(
        transition.effectivePath()
        == EffectiveBackgroundCapturePath::FxOnlyCaptureVisibilityUnknown);
    BAFX_CHECK(
        transition.failure() == BackgroundCaptureFailure::InclusionUnconfirmed);
}

BAFX_TEST(frame_pool_recreate_keeps_the_active_background_path)
{
    BackgroundCaptureTransition transition;
    const BackgroundCaptureRequest request = backgroundAwareRequest();
    BAFX_CHECK(
        transition.beginRequest(request) == BackgroundCaptureRequestResult::Started);
    static_cast<void>(completeSuccessfully(transition));

    BAFX_CHECK(transition.beginFramePoolRecreate(resizedOutput));
    const auto recreate = transition.nextAction();
    BAFX_CHECK(recreate.has_value());
    BAFX_CHECK(
        recreate->kind == BackgroundCaptureActionKind::RecreateFramePool);
    BAFX_CHECK(recreate->captureSize.width == resizedOutput.width);
    BAFX_CHECK(recreate->captureSize.height == resizedOutput.height);
    BAFX_CHECK(transition.applyObservation(*recreate, true));
    BAFX_CHECK(!transition.nextAction().has_value());
    BAFX_CHECK(
        transition.effectivePath()
        == EffectiveBackgroundCapturePath::BackgroundAware);
    BAFX_CHECK(transition.failure() == BackgroundCaptureFailure::None);
}

BAFX_TEST(frame_pool_recreate_failure_cleans_up_once_without_retry)
{
    BackgroundCaptureTransition transition;
    const BackgroundCaptureRequest request = backgroundAwareRequest();
    BAFX_CHECK(
        transition.beginRequest(request) == BackgroundCaptureRequestResult::Started);
    static_cast<void>(completeSuccessfully(transition));

    BAFX_CHECK(transition.beginFramePoolRecreate(resizedOutput));
    const auto recreate = transition.nextAction();
    BAFX_CHECK(recreate.has_value());
    BAFX_CHECK(transition.applyObservation(*recreate, false));
    checkActions(
        completeSuccessfully(transition),
        {BackgroundCaptureActionKind::StopSensor,
         BackgroundCaptureActionKind::SetAffinityIncluded,
         BackgroundCaptureActionKind::ApplyOverlayProfile});
    BAFX_CHECK(
        transition.failure()
        == BackgroundCaptureFailure::FramePoolRecreateFailed);
    BAFX_CHECK(
        transition.effectivePath() == EffectiveBackgroundCapturePath::FxOnly);
    BAFX_CHECK(
        transition.beginRequest(request) == BackgroundCaptureRequestResult::NoChange);
}

BAFX_TEST(frame_pool_recreate_rejects_invalid_or_inactive_sessions)
{
    BackgroundCaptureTransition transition;
    BAFX_CHECK(!transition.beginFramePoolRecreate(resizedOutput));

    BAFX_CHECK(
        transition.beginRequest(recordingRequest())
        == BackgroundCaptureRequestResult::Started);
    static_cast<void>(completeSuccessfully(transition));
    BAFX_CHECK(!transition.beginFramePoolRecreate(resizedOutput));

    BAFX_CHECK(
        transition.beginRequest(backgroundAwareRequest())
        == BackgroundCaptureRequestResult::Started);
    static_cast<void>(completeSuccessfully(transition));
    BAFX_CHECK(
        !transition.beginFramePoolRecreate(WindowSize{resizedOutput.width, 0U}));
}
