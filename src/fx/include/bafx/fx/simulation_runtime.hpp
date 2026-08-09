#pragma once

#include "bafx/fx/simulation.hpp"

#include <cstddef>
#include <cstdint>
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
    void advance(SimulationTime time);
    void onFrameRendered();

    [[nodiscard]] FrameSnapshot snapshot(Viewport viewport, SimulationTime time) const;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool pointerHeld() const noexcept;
    [[nodiscard]] std::size_t instanceCount() const noexcept;

private:
    [[nodiscard]] std::uint64_t nextSeed() noexcept;

    std::uint64_t baseSeed_{0U};
    std::uint64_t activationCount_{0U};
    bool pointerActive_{false};
    std::vector<Simulation> instances_{};
};

}
