#pragma once

#include "bafx/fx/simulation.hpp"

#include <optional>

namespace bafx::fx
{

class SimulationTimeline final
{
public:
    void setPaused(bool paused, SimulationTime wallTime) noexcept;

    [[nodiscard]] SimulationTime fromWallTime(
        SimulationTime wallTime) const noexcept;
    [[nodiscard]] bool paused() const noexcept;

private:
    SimulationTime pausedDuration_{};
    std::optional<SimulationTime> pausedAtWallTime_{};
};

}
