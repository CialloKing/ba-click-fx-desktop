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

[[nodiscard]] bool physicalTargetIdentityResolutionImproved(
    const DisplayTarget& previous,
    const DisplayTarget& current) noexcept
{
    if (previous.physicalTargetIdentities.size()
        != current.physicalTargetIdentities.size())
    {
        return false;
    }
    bool improved = false;
    for (std::size_t index = 0U;
         index < previous.physicalTargetIdentities.size();
         ++index)
    {
        const DisplayPhysicalTargetIdentity& oldIdentity =
            previous.physicalTargetIdentities[index];
        const DisplayPhysicalTargetIdentity& newIdentity =
            current.physicalTargetIdentities[index];
        if (!oldIdentity.devicePath.empty()
            && newIdentity.devicePath.empty())
        {
            // Keep the last authoritative path while any cloned target is in
            // a transiently incomplete DisplayConfig snapshot.
            return false;
        }
        if (oldIdentity.devicePath.empty()
            && !newIdentity.devicePath.empty())
        {
            improved = true;
        }
    }
    return improved;
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
      simulationSeed_(options.simulationSeed),
      trailLengthMultiplier_(options.trailLengthMultiplier),
      inputSamplingRateHz_(options.inputSamplingRateHz),
      alwaysOnTrailEnabled_(options.alwaysOnTrailEnabled)
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
        if (sameDisplaySource(coordinator_->target(), observedTarget))
        {
            continue;
        }

        DisplaySession* const existing = findBySource(observedTarget);
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

        const DisplayTarget target = stabilizeDisplayTargetObservation(
            existing->target(),
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

        const bool sameTarget = sameDisplayTarget(existing->target(), target);
        const bool sameSourceIdentity = sameDisplaySourceIdentity(
            existing->target(),
            target);
        const bafx::windows::GraphicsDeviceInfo& deviceInfo =
            existing->renderer().deviceInfo();
        const bool resourceDomainMatches =
            displayTargetResourceAdapterMatches(
                target,
                deviceInfo.adapterLuid,
                deviceInfo.driverType
                    == bafx::windows::GraphicsDriverType::Hardware);
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
                !sameDisplayRuntimeMetadata(existing->target(), target)
                || physicalTargetIdentityResolutionImproved(
                    existing->target(),
                    target);
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
            const bool remove = !targetPresent(snapshot, session->target());
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
        const DisplayTarget* const observed = findDisplayTargetBySource(
            snapshot,
            session->target());
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
            session->target(),
            *observed,
            snapshot.status);
        if (!sameDisplayTarget(session->target(), stabilized)
            || !sameDisplaySourceIdentity(session->target(), stabilized)
            || !sameDisplayRuntimeMetadata(session->target(), stabilized)
            || physicalTargetIdentityResolutionImproved(
                session->target(),
                stabilized))
        {
            return true;
        }
    }

    for (const DisplayTarget& observed : snapshot.displays)
    {
        if (findBySource(observed) == nullptr
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
            const bool remove = session.get() != coordinator_
                && sameDisplaySource(session->target(), coordinator_->target());
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
    const bool alwaysOnTrailEnabled) noexcept
{
    bloomSettings_ = bloomSettings;
    outputPreference_ = outputPreference;
    trailLengthMultiplier_ = trailLengthMultiplier;
    inputSamplingRateHz_ = inputSamplingRateHz;
    alwaysOnTrailEnabled_ = alwaysOnTrailEnabled;
}

DisplaySession* DisplaySessionManager::findBySource(
    const DisplayTarget& target) noexcept
{
    const auto found = std::find_if(
        sessions_.begin(),
        sessions_.end(),
        [&target](const std::unique_ptr<DisplaySession>& session)
        {
            return sameDisplaySource(session->target(), target);
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
            return sameDisplaySource(session->target(), target);
        });
    return found == sessions_.end() ? nullptr : found->get();
}

DisplaySession* DisplaySessionManager::findAtPoint(const POINT point) noexcept
{
    const auto found = std::find_if(
        sessions_.begin(),
        sessions_.end(),
        [point](const std::unique_ptr<DisplaySession>& session)
        {
            return containsPoint(session->target().bounds, point);
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
    auto session = std::make_unique<DisplaySession>(
        DisplaySessionOptions{
            instance_,
            wakeWindow_,
            borderlessAccessAuthority_,
            std::move(target),
            surfaceTitle_,
            bloomSettings_,
            backgroundStopObserver_,
            outputPreference_,
            nextSimulationSeed()});
    session->simulation().setTrailLengthMultiplier(trailLengthMultiplier_);
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
            return sameDisplaySource(target, candidate);
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
