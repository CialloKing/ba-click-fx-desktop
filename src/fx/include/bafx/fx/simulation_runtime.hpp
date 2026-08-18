#pragma once

#include "bafx/fx/simulation.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace bafx::fx
{

class SimulationRuntime final
{
public:
    explicit SimulationRuntime(std::uint64_t seed = 20260716U);

    void pointerDown(PointF screenPosition, Viewport viewport, SimulationTime time);
    void pointerDown(
        PointF screenPosition,
        Viewport viewport,
        SimulationTime simulationTime,
        SimulationTime inputTime);
    // Continues a physical press after its screen-local coordinate domain
    // changes. The new instance starts from an anchor without a click burst or
    // a synthetic line from the previous display.
    void continuePointerStroke(
        PointF screenPosition,
        Viewport viewport,
        SimulationTime simulationTime,
        SimulationTime inputTime);
    void pointerMove(PointF screenPosition, Viewport viewport, SimulationTime time);
    void pointerMove(
        PointF screenPosition,
        Viewport viewport,
        SimulationTime simulationTime,
        SimulationTime inputTime);
    void pointerUp(SimulationTime time);
    void pointerCancel(SimulationTime time);
    // Removes drawable state immediately when a display stops owning effects.
    // Unlike pointerCancel, this deliberately does not preserve natural fade.
    void discardActiveEffects() noexcept;
    void endAlwaysOnTrail(SimulationTime time);
    void advance(SimulationTime time);
    void onFrameRendered(SimulationTime time);
    void updateUnityTrailTimeScale(float timeScale);
    // Timestamped overloads preserve active-object history across hot updates.
    void setClickTimeScale(float timeScale) noexcept;
    void setClickTimeScale(float timeScale, SimulationTime time) noexcept;
    void setTrailTimeScale(float timeScale) noexcept;
    void setTrailTimeScale(float timeScale, SimulationTime time) noexcept;
    void setClickParticleSettings(ClickParticleSettings settings) noexcept;
    void setClickParticleSettings(
        ClickParticleSettings settings,
        SimulationTime time) noexcept;
    void setShardParticleSettings(ShardParticleSettings settings) noexcept;
    void setTrailLengthMultiplier(float multiplier) noexcept;
    void setInputSamplingRateHz(std::uint32_t rateHz) noexcept;
    void setAlwaysOnTrailEnabled(bool enabled, SimulationTime time);
    void setEffectsMode(SimulationEffectsMode mode, SimulationTime time) noexcept;
    [[nodiscard]] SimulationEffectsMode effectsMode() const noexcept;

    [[nodiscard]] FrameSnapshot snapshot(Viewport viewport, SimulationTime time) const;
    // Pressed FX instances retain Unity's presentation-bound cleanup. The
    // ambient instance may stay allocated only as a future Move anchor, so it
    // requires frames only while it still owns visible geometry.
    [[nodiscard]] bool renderingRequired(SimulationTime time) const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool pointerHeld() const noexcept;
    [[nodiscard]] bool alwaysOnTrailEnabled() const noexcept;
    [[nodiscard]] std::size_t instanceCount() const noexcept;
    [[nodiscard]] std::size_t pooledInstanceCount() const noexcept;

private:
    struct RuntimeInstance
    {
        Simulation simulation;
        bool returnsToUnityPool{false};
    };

    [[nodiscard]] std::uint64_t nextUnitySeed() noexcept;
    [[nodiscard]] std::uint64_t nextAmbientSeed() noexcept;
    [[nodiscard]] Simulation& acquirePressedInstance(
        SimulationTime simulationTime);
    [[nodiscard]] bool acceptInputSample(SimulationTime inputTime) noexcept;
    void resetInputSamplingPhase() noexcept;
    void retireAlwaysOnTrail(SimulationTime time);

    std::uint64_t baseSeed_{0U};
    std::uint64_t unityActivationCount_{0U};
    std::uint64_t ambientActivationCount_{0U};
    bool pointerActive_{false};
    bool alwaysOnTrailEnabled_{false};
    SimulationEffectsMode effectsMode_{SimulationEffectsMode::Full};
    float trailLengthMultiplier_{1.0F};
    float clickTimeScale_{1.0F};
    float trailTimeScale_{1.0F};
    ClickParticleSettings clickParticleSettings_{};
    ShardParticleSettings shardParticleSettings_{};
    std::uint32_t inputSamplingRateHz_{0U};
    std::optional<SimulationTime> lastInputSampleAt_{};
    std::vector<RuntimeInstance> instances_{};
    std::deque<Simulation> unityPool_{};
    std::optional<Simulation> alwaysOnTrail_{};
};

}
