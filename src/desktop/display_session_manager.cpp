#include "display_session_manager.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace bafx::desktop
{
namespace
{

constexpr std::uint64_t sessionSeedStep = 0x9E3779B97F4A7C15ULL;

[[nodiscard]] bool containsPoint(
    const RECT& bounds,
    const POINT point) noexcept
{
    return point.x >= bounds.left
        && point.y >= bounds.top
        && point.x < bounds.right
        && point.y < bounds.bottom;
}

}

DisplaySessionManager::DisplaySessionManager(
    DisplaySessionManagerOptions options)
    : instance_(options.instance),
      wakeWindow_(options.wakeWindow),
      borderlessAccessAuthority_(options.borderlessAccessAuthority),
      surfaceTitle_(options.surfaceTitle),
      bloomSettings_(options.bloomSettings),
      backgroundStopObserver_(options.backgroundStopObserver),
      outputPreference_(options.outputPreference),
      policyResolver_(std::move(options.policyResolver)),
      simulationSeed_(options.simulationSeed),
      trailLengthMultiplier_(options.trailLengthMultiplier),
      inputSamplingRateHz_(options.inputSamplingRateHz),
      alwaysOnTrailEnabled_(options.alwaysOnTrailEnabled),
      clickTimeScale_(options.clickTimeScale),
      trailTimeScale_(options.trailTimeScale),
      clickParticleSettings_(options.clickParticleSettings),
      shardParticleSettings_(options.shardParticleSettings)
{
    if (borderlessAccessAuthority_ == nullptr)
    {
        throw std::invalid_argument(
            "Display manager requires the process access authority");
    }
}

DisplaySession& DisplaySessionManager::createCoordinator(DisplayTarget target)
{
    if (coordinator_ != nullptr)
    {
        throw std::logic_error("Display coordinator already exists");
    }

    std::unique_ptr<DisplaySession> session = createSession(std::move(target));
    sessions_.push_back(std::move(session));
    coordinator_ = sessions_.back().get();
    return *coordinator_;
}

DisplaySession& DisplaySessionManager::coordinator()
{
    if (coordinator_ == nullptr)
    {
        throw std::logic_error("Display coordinator has not been created");
    }
    return *coordinator_;
}

const DisplaySession& DisplaySessionManager::coordinator() const
{
    if (coordinator_ == nullptr)
    {
        throw std::logic_error("Display coordinator has not been created");
    }
    return *coordinator_;
}

DisplaySessionReconcileResult DisplaySessionManager::reconcileSecondaries(
    const DisplayTargetSnapshot& snapshot)
{
    DisplaySessionReconcileResult result{};
    result.topologyStatus = snapshot.status;
    result.topologyError = snapshot.error;
    if (coordinator_ == nullptr)
    {
        result.failures.push_back(DisplaySessionFailure{
            {},
            "reconcile",
            "display coordinator has not been created"});
        return result;
    }

    for (const DisplayTarget& observedTarget : snapshot.displays)
    {
        if (sameDisplaySource(coordinator_->target(), observedTarget)
            || sameDisplayLogicalSlot(
                coordinator_->target(),
                observedTarget))
        {
            continue;
        }

        DisplaySession* const existing = findForReconciliation(
            observedTarget);
        if (existing == nullptr)
        {
            if (!observedTarget.sourceAdapterResolved)
            {
                // A new target without either DisplayConfig or unique DXGI
                // adapter evidence has no safe resource domain. Wait instead
                // of creating its renderer on the process-default GPU.
                continue;
            }
            try
            {
                std::unique_ptr<DisplaySession> session =
                    createSession(observedTarget);
                session->show();
                sessions_.push_back(std::move(session));
                ++result.added;
            }
            catch (const std::exception& error)
            {
                result.failures.push_back(DisplaySessionFailure{
                    observedTarget,
                    "create",
                    error.what()});
            }
            continue;
        }

        const DisplayTarget& reconciliationTarget =
            existing->reconciliationTarget();
        const DisplayTarget target = stabilizeDisplayTargetObservation(
            reconciliationTarget,
            observedTarget,
            snapshot.status);

        if (existing->renderFaulted())
        {
            if (snapshot.status
                != bafx::windows::DisplayTopologyStatus::Complete)
            {
                // A partial QueryDisplayConfig result cannot safely choose a
                // replacement adapter. Retain the faulted session until a
                // later authoritative topology notification.
                continue;
            }
            try
            {
                std::unique_ptr<DisplaySession> replacement =
                    createSession(target);
                replacement->show();
                const auto slot = std::find_if(
                    sessions_.begin(),
                    sessions_.end(),
                    [existing](const std::unique_ptr<DisplaySession>& session)
                    {
                        return session.get() == existing;
                    });
                if (slot == sessions_.end())
                {
                    throw std::logic_error(
                        "Faulted display session lost its ownership slot");
                }
                *slot = std::move(replacement);
                ++result.recreated;
            }
            catch (const std::exception& error)
            {
                result.failures.push_back(DisplaySessionFailure{
                    target,
                    "recreate",
                    error.what()});
            }
            continue;
        }

        if (existing->retargetPendingFor(target))
        {
            // The pending WGC transaction already owns this exact placement
            // and source. Merge DPI/DRR metadata without canceling a
            // long-running permission request.
            const bool metadataChanged =
                displayTargetMetadataChanged(reconciliationTarget, target);
            if (metadataChanged
                && existing->updatePendingTargetMetadata(target))
            {
                ++result.updated;
            }
            continue;
        }

        const bool sameTarget = sameDisplayTarget(existing->target(), target);
        const bool sameSourceIdentity = sameDisplaySourceIdentity(
            existing->target(),
            target);
        const bool resourceDomainMatches =
            existing->resourceDomainReadyForTarget(target);
        if (sameTarget && sameSourceIdentity && resourceDomainMatches)
        {
            bool boundsCorrected = false;
            try
            {
                if (!sameDisplayBounds(
                        existing->window().bounds(),
                        target.bounds))
                {
                    // WM_DPICHANGED and external placement can alter only the
                    // HWND while rcMonitor and the GPU domain stay stable.
                    existing->window().setBounds(target.bounds);
                    boundsCorrected = true;
                }
            }
            catch (const std::exception& error)
            {
                result.failures.push_back(DisplaySessionFailure{
                    target,
                    "restore-bounds",
                    error.what()});
                continue;
            }
            const bool metadataChanged =
                displayTargetMetadataChanged(existing->target(), target);
            if (metadataChanged)
            {
                static_cast<void>(existing->updateTargetMetadata(target));
            }
            if (boundsCorrected || metadataChanged)
            {
                ++result.updated;
            }
            continue;
        }

        try
        {
            static_cast<void>(existing->retargetSecondary(target, wakeWindow_));
            ++result.updated;
        }
        catch (const std::exception& error)
        {
            result.failures.push_back(DisplaySessionFailure{
                target,
                "retarget",
                error.what()});
        }
    }

    const bool topologyAuthoritative =
        snapshot.status == bafx::windows::DisplayTopologyStatus::Complete;
    if (!topologyAuthoritative)
    {
        // QueryDisplayConfig can race hot-plug. Keep the last complete set
        // until a later authoritative snapshot prevents accidental teardown.
        result.removalsDeferred = true;
        return result;
    }

    const auto firstRemoved = std::remove_if(
        sessions_.begin(),
        sessions_.end(),
        [&](const std::unique_ptr<DisplaySession>& session)
        {
            if (session.get() == coordinator_)
            {
                return false;
            }
            const bool intentPresent = targetPresent(
                snapshot,
                session->reconciliationTarget());
            const bool appliedTargetPresent = targetPresent(
                snapshot,
                session->target());
            // A failed cancellation can leave a stale pending target while
            // the applied monitor is still connected. Retain that live owner
            // so the next bounded topology poll can retry the rollback.
            const bool remove = !intentPresent && !appliedTargetPresent;
            if (remove)
            {
                ++result.removed;
            }
            return remove;
        });
    sessions_.erase(firstRemoved, sessions_.end());
    return result;
}

bool DisplaySessionManager::topologyDiffers(
    const DisplayTargetSnapshot& snapshot) const noexcept
{
    if (coordinator_ == nullptr
        || snapshot.status
            == bafx::windows::DisplayTopologyStatus::QueryFailed
        || snapshot.status
            == bafx::windows::DisplayTopologyStatus::NoActiveDisplays)
    {
        return false;
    }

    for (const std::unique_ptr<DisplaySession>& session : sessions_)
    {
        const DisplayTarget& expected = session->reconciliationTarget();
        const DisplayTarget* observed = findDisplayTargetBySource(
            snapshot,
            expected);
        if (observed == nullptr)
        {
            observed = findDisplayTargetByLogicalSlot(snapshot, expected);
        }
        if (observed == nullptr)
        {
            if (snapshot.status
                == bafx::windows::DisplayTopologyStatus::Complete)
            {
                return true;
            }
            continue;
        }

        const DisplayTarget stabilized = stabilizeDisplayTargetObservation(
            expected,
            *observed,
            snapshot.status);
        if (session->retargetPendingFor(stabilized))
        {
            if (displayTargetMetadataChanged(expected, stabilized))
            {
                return true;
            }
            continue;
        }
        if (!sameDisplayTarget(expected, stabilized)
            || !sameDisplaySourceIdentity(expected, stabilized)
            || !sameDisplayRuntimeMetadata(expected, stabilized)
            || !session->resourceDomainReadyForTarget(stabilized)
            || displayPhysicalTargetIdentityResolutionImproved(
                expected,
                stabilized))
        {
            return true;
        }
    }

    for (const DisplayTarget& observed : snapshot.displays)
    {
        const bool coordinatorTarget = sameDisplaySource(
                coordinator_->target(),
                observed)
            || sameDisplayLogicalSlot(
                coordinator_->target(),
                observed);
        if (!coordinatorTarget
            && findForReconciliation(observed) == nullptr
            && observed.sourceAdapterResolved)
        {
            return true;
        }
    }
    return false;
}

std::size_t DisplaySessionManager::pruneCoordinatorDuplicates() noexcept
{
    if (coordinator_ == nullptr)
    {
        return 0U;
    }

    std::size_t removed = 0U;
    const auto firstRemoved = std::remove_if(
        sessions_.begin(),
        sessions_.end(),
        [&](const std::unique_ptr<DisplaySession>& session)
        {
            const bool intentDuplicatesCoordinator = sameDisplaySource(
                    session->reconciliationTarget(),
                    coordinator_->target())
                || sameDisplayLogicalSlot(
                    session->reconciliationTarget(),
                    coordinator_->target());
            const bool appliedTargetDuplicatesCoordinator = sameDisplaySource(
                    session->target(),
                    coordinator_->target())
                || sameDisplayLogicalSlot(
                    session->target(),
                    coordinator_->target());
            // A secondary HWND remains on its applied target until an
            // asynchronous retarget commits. Once the coordinator owns that
            // display, retaining either identity would overlap two surfaces.
            const bool remove = session.get() != coordinator_
                && (intentDuplicatesCoordinator
                    || appliedTargetDuplicatesCoordinator);
            if (remove)
            {
                ++removed;
            }
            return remove;
        });
    sessions_.erase(firstRemoved, sessions_.end());
    return removed;
}

void DisplaySessionManager::updateCreationSettings(
    const bafx::windows::FxBloomSettings bloomSettings,
    const bafx::windows::CompositionOutputPreference outputPreference,
    const float trailLengthMultiplier,
    const std::uint32_t inputSamplingRateHz,
    const bool alwaysOnTrailEnabled,
    const float clickTimeScale,
    const float trailTimeScale,
    const bafx::fx::ClickParticleSettings clickParticleSettings,
    const bafx::fx::ShardParticleSettings shardParticleSettings) noexcept
{
    bloomSettings_ = bloomSettings;
    outputPreference_ = outputPreference;
    trailLengthMultiplier_ = trailLengthMultiplier;
    inputSamplingRateHz_ = inputSamplingRateHz;
    alwaysOnTrailEnabled_ = alwaysOnTrailEnabled;
    clickTimeScale_ = clickTimeScale;
    trailTimeScale_ = trailTimeScale;
    clickParticleSettings_ = clickParticleSettings;
    shardParticleSettings_ = shardParticleSettings;
}

DisplaySessionPolicyChange DisplaySessionManager::refreshRuntimePolicies()
{
    DisplaySessionPolicyChange aggregate{};
    for (const std::unique_ptr<DisplaySession>& session : sessions_)
    {
        const DisplaySessionRuntimePolicy policy = policyResolver_
            ? policyResolver_(session->target())
            : DisplaySessionRuntimePolicy{
                true,
                outputPreference_,
                bafx::config::FramePacing::MatchDisplay,
                bafx::core::MonotonicTime::zero()};
        const DisplaySessionPolicyChange change =
            session->applyRuntimePolicy(policy);
        aggregate.effectsEnabledChanged = aggregate.effectsEnabledChanged
            || change.effectsEnabledChanged;
        aggregate.outputPreferenceChanged = aggregate.outputPreferenceChanged
            || change.outputPreferenceChanged;
        aggregate.framePacingChanged = aggregate.framePacingChanged
            || change.framePacingChanged;
    }
    return aggregate;
}

DisplaySession* DisplaySessionManager::findBySource(
    const DisplayTarget& target) noexcept
{
    const auto found = std::find_if(
        sessions_.begin(),
        sessions_.end(),
        [&target](const std::unique_ptr<DisplaySession>& session)
        {
            return session->effectsEnabled()
                && sameDisplaySource(session->target(), target);
        });
    return found == sessions_.end() ? nullptr : found->get();
}

const DisplaySession* DisplaySessionManager::findBySource(
    const DisplayTarget& target) const noexcept
{
    const auto found = std::find_if(
        sessions_.begin(),
        sessions_.end(),
        [&target](const std::unique_ptr<DisplaySession>& session)
        {
            return session->effectsEnabled()
                && sameDisplaySource(session->target(), target);
        });
    return found == sessions_.end() ? nullptr : found->get();
}

DisplaySession* DisplaySessionManager::findForReconciliation(
    const DisplayTarget& target) noexcept
{
    const auto exactSource = std::find_if(
        sessions_.begin(),
        sessions_.end(),
        [this, &target](const std::unique_ptr<DisplaySession>& session)
        {
            return session.get() != coordinator_
                && sameDisplaySource(
                    session->reconciliationTarget(),
                    target);
        });
    if (exactSource != sessions_.end())
    {
        return exactSource->get();
    }

    const auto reconciliationSlot = std::find_if(
        sessions_.begin(),
        sessions_.end(),
        [this, &target](const std::unique_ptr<DisplaySession>& session)
        {
            return session.get() != coordinator_
                && sameDisplayLogicalSlot(
                    session->reconciliationTarget(),
                    target);
        });
    if (reconciliationSlot != sessions_.end())
    {
        return reconciliationSlot->get();
    }

    // A target can disappear while its asynchronous retarget owns the
    // reconciliation identity. Match the still-applied source afterwards so
    // the owner can cancel that stale intent instead of deleting a live surface.
    const auto appliedSource = std::find_if(
        sessions_.begin(),
        sessions_.end(),
        [this, &target](const std::unique_ptr<DisplaySession>& session)
        {
            return session.get() != coordinator_
                && sameDisplaySource(session->target(), target);
        });
    if (appliedSource != sessions_.end())
    {
        return appliedSource->get();
    }

    const auto appliedSlot = std::find_if(
        sessions_.begin(),
        sessions_.end(),
        [this, &target](const std::unique_ptr<DisplaySession>& session)
        {
            return session.get() != coordinator_
                && sameDisplayLogicalSlot(session->target(), target);
        });
    return appliedSlot == sessions_.end() ? nullptr : appliedSlot->get();
}

const DisplaySession* DisplaySessionManager::findForReconciliation(
    const DisplayTarget& target) const noexcept
{
    const auto exactSource = std::find_if(
        sessions_.begin(),
        sessions_.end(),
        [this, &target](const std::unique_ptr<DisplaySession>& session)
        {
            return session.get() != coordinator_
                && sameDisplaySource(
                    session->reconciliationTarget(),
                    target);
        });
    if (exactSource != sessions_.end())
    {
        return exactSource->get();
    }

    const auto reconciliationSlot = std::find_if(
        sessions_.begin(),
        sessions_.end(),
        [this, &target](const std::unique_ptr<DisplaySession>& session)
        {
            return session.get() != coordinator_
                && sameDisplayLogicalSlot(
                    session->reconciliationTarget(),
                    target);
        });
    if (reconciliationSlot != sessions_.end())
    {
        return reconciliationSlot->get();
    }

    const auto appliedSource = std::find_if(
        sessions_.begin(),
        sessions_.end(),
        [this, &target](const std::unique_ptr<DisplaySession>& session)
        {
            return session.get() != coordinator_
                && sameDisplaySource(session->target(), target);
        });
    if (appliedSource != sessions_.end())
    {
        return appliedSource->get();
    }

    const auto appliedSlot = std::find_if(
        sessions_.begin(),
        sessions_.end(),
        [this, &target](const std::unique_ptr<DisplaySession>& session)
        {
            return session.get() != coordinator_
                && sameDisplayLogicalSlot(session->target(), target);
        });
    return appliedSlot == sessions_.end() ? nullptr : appliedSlot->get();
}

DisplaySession* DisplaySessionManager::findAtPoint(const POINT point) noexcept
{
    const HMONITOR currentMonitor = MonitorFromPoint(
        point,
        MONITOR_DEFAULTTONULL);
    if (currentMonitor != nullptr)
    {
        const auto exactMonitor = std::find_if(
            sessions_.begin(),
            sessions_.end(),
            [point, currentMonitor](
                const std::unique_ptr<DisplaySession>& session)
            {
                return session->effectsEnabled()
                    && session->target().monitor == currentMonitor
                    && containsPoint(session->target().bounds, point);
            });
        if (exactMonitor != sessions_.end())
        {
            // A removed coordinator can overlap its replacement until the WGC
            // retarget commits. Prefer the monitor Windows currently owns so
            // input remains on the visible session during that transaction.
            return exactMonitor->get();
        }
    }

    const auto found = std::find_if(
        sessions_.begin(),
        sessions_.end(),
        [point](const std::unique_ptr<DisplaySession>& session)
        {
            return session->effectsEnabled()
                && containsPoint(session->target().bounds, point);
        });
    return found == sessions_.end() ? nullptr : found->get();
}

const std::vector<std::unique_ptr<DisplaySession>>&
DisplaySessionManager::sessions() const noexcept
{
    return sessions_;
}

std::unique_ptr<DisplaySession> DisplaySessionManager::createSession(
    DisplayTarget target)
{
    const DisplaySessionRuntimePolicy policy = policyResolver_
        ? policyResolver_(target)
        : DisplaySessionRuntimePolicy{
            true,
            outputPreference_,
            bafx::config::FramePacing::MatchDisplay,
            bafx::core::MonotonicTime::zero()};
    auto session = std::make_unique<DisplaySession>(
        DisplaySessionOptions{
            instance_,
            wakeWindow_,
            borderlessAccessAuthority_,
            std::move(target),
            surfaceTitle_,
            bloomSettings_,
            backgroundStopObserver_,
            policy,
            nextSimulationSeed()});
    session->simulation().setTrailLengthMultiplier(trailLengthMultiplier_);
    session->simulation().setClickTimeScale(clickTimeScale_);
    session->simulation().setTrailTimeScale(trailTimeScale_);
    session->simulation().setClickParticleSettings(clickParticleSettings_);
    session->simulation().setShardParticleSettings(shardParticleSettings_);
    session->simulation().setInputSamplingRateHz(inputSamplingRateHz_);
    session->simulation().setAlwaysOnTrailEnabled(
        alwaysOnTrailEnabled_,
        bafx::fx::SimulationTime{});
    return session;
}

bool DisplaySessionManager::targetPresent(
    const DisplayTargetSnapshot& snapshot,
    const DisplayTarget& target) noexcept
{
    return std::any_of(
        snapshot.displays.begin(),
        snapshot.displays.end(),
        [&target](const DisplayTarget& candidate)
        {
            return sameDisplaySource(target, candidate)
                || sameDisplayLogicalSlot(target, candidate);
        });
}

std::uint64_t DisplaySessionManager::nextSimulationSeed() noexcept
{
    const std::uint64_t seed = simulationSeed_
        + sessionSequence_ * sessionSeedStep;
    ++sessionSequence_;
    return seed;
}

}
