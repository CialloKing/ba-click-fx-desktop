#include "bafx/fx/simulation_timeline.hpp"

#include <algorithm>

namespace bafx::fx
{

void SimulationTimeline::setPaused(
    const bool paused,
    const SimulationTime wallTime) noexcept
{
    if (paused == pausedAtWallTime_.has_value())
    {
        return;
    }

    if (paused)
    {
        pausedAtWallTime_ = wallTime;
        return;
    }

    // A pause freezes simulation time. Removing the whole wall-clock interval
    // on resume prevents particles from expiring while no frames were drawn.
    pausedDuration_ += std::max(
        wallTime - *pausedAtWallTime_,
        SimulationTime::zero());
    pausedAtWallTime_.reset();
}

SimulationTime SimulationTimeline::fromWallTime(
    const SimulationTime wallTime) const noexcept
{
    const SimulationTime effectiveWallTime = pausedAtWallTime_.has_value()
        ? std::min(wallTime, *pausedAtWallTime_)
        : wallTime;
    return effectiveWallTime - pausedDuration_;
}

bool SimulationTimeline::paused() const noexcept
{
    return pausedAtWallTime_.has_value();
}

}
