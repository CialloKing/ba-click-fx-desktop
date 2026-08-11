#pragma once

#include "bafx/fx/simulation.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bafx::fx
{

class SimulationRuntime final
{
public:
    explicit SimulationRuntime(std::uint64_t seed = 20260716U);

    void pointerDown(PointF screenPosition, Viewport viewport, SimulationTime time);
    void pointerMove(PointF screenPosition, Viewport viewport, SimulationTime time);
    void pointerUp(SimulationTime time);
    void pointerCancel(SimulationTime time);
    void endAlwaysOnTrail(SimulationTime time);
    void advance(SimulationTime time);
    void onFrameRendered();
    void setTrailLengthMultiplier(float multiplier) noexcept;
    void setAlwaysOnTrailEnabled(bool enabled, SimulationTime time);

    [[nodiscard]] FrameSnapshot snapshot(Viewport viewport, SimulationTime time) const;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool pointerHeld() const noexcept;
    [[nodiscard]] bool alwaysOnTrailEnabled() const noexcept;
    [[nodiscard]] std::size_t instanceCount() const noexcept;

private:
    [[nodiscard]] std::uint64_t nextSeed() noexcept;
    void retireAlwaysOnTrail(SimulationTime time);

    std::uint64_t baseSeed_{0U};
    std::uint64_t activationCount_{0U};
    bool pointerActive_{false};
    bool alwaysOnTrailEnabled_{false};
    float trailLengthMultiplier_{1.0F};
    std::vector<Simulation> instances_{};
    std::optional<Simulation> alwaysOnTrail_{};
};

}
