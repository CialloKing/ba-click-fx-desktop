#include "bafx/fx/simulation_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace bafx::fx
{
namespace
{

constexpr std::uint64_t randomStreamStep = 0x9E3779B97F4A7C15ULL;
constexpr std::uint64_t ambientRandomStream = 0xD1B54A32D192ED03ULL;
constexpr float minimumTrailLengthMultiplier = 0.0F;
constexpr float maximumTrailLengthMultiplier = 3.0F;
constexpr std::uint32_t maximumInputSamplingRateHz = 1000U;

[[nodiscard]] float normalizeTrailLengthMultiplier(const float multiplier) noexcept
{
    if (!std::isfinite(multiplier))
    {
        return 1.0F;
    }

    return std::clamp(
        multiplier,
        minimumTrailLengthMultiplier,
        maximumTrailLengthMultiplier);
}

}

SimulationRuntime::SimulationRuntime(const std::uint64_t seed)
    : baseSeed_(seed)
{
    instances_.reserve(8U);
    // TouchEffectCreater initializes SyncComponentPool<FXTouch> with one item.
    // Materialize it here so the first activation also follows the FIFO path.
    unityPool_.emplace_back(baseSeed_);
}

void SimulationRuntime::pointerDown(
    const PointF screenPosition,
    const Viewport viewport,
    const SimulationTime time)
{
    pointerDown(screenPosition, viewport, time, time);
}

void SimulationRuntime::pointerDown(
    const PointF screenPosition,
    const Viewport viewport,
    const SimulationTime simulationTime,
    const SimulationTime inputTime)
{
    // Unity keeps one object for the currently held pointer, so duplicate down
    // notifications must not replace that object or restart its particles.
    if (pointerActive_)
    {
        return;
    }

    Simulation& instance = acquirePressedInstance(simulationTime);
    instance.pointerDown(screenPosition, viewport, simulationTime);
    pointerActive_ = true;
    if (inputSamplingRateHz_ > 0U)
    {
        lastInputSampleAt_ = inputTime;
    }
}

void SimulationRuntime::continuePointerStroke(
    const PointF screenPosition,
    const Viewport viewport,
    const SimulationTime simulationTime,
    const SimulationTime inputTime)
{
    if (pointerActive_)
    {
        return;
    }

    Simulation& instance = acquirePressedInstance(simulationTime);
    // startTrail deliberately omits click particles and treats this position
    // as an anchor. This preserves the physical press without connecting two
    // unrelated monitor-local coordinate systems.
    instance.startTrail(screenPosition, viewport, simulationTime);
    pointerActive_ = true;
    if (inputSamplingRateHz_ > 0U)
    {
        lastInputSampleAt_ = inputTime;
    }
}

void SimulationRuntime::pointerMove(
    const PointF screenPosition,
    const Viewport viewport,
    const SimulationTime time)
{
    pointerMove(screenPosition, viewport, time, time);
}

void SimulationRuntime::pointerMove(
    const PointF screenPosition,
    const Viewport viewport,
    const SimulationTime simulationTime,
    const SimulationTime inputTime)
{
    if (pointerActive_ && !instances_.empty())
    {
        Simulation& instance = instances_.back().simulation;
        const bool firstAdvancePending = instance.firstAdvancePending();
        if (firstAdvancePending)
        {
            // Unity samples the final Input.mousePosition in the same Update
            // that creates FX_Touch. Preserve that final sample even when an
            // optional host-side rate would reject its short raw interval.
            if (inputSamplingRateHz_ > 0U
                && (!lastInputSampleAt_.has_value()
                    || inputTime > *lastInputSampleAt_))
            {
                lastInputSampleAt_ = inputTime;
            }
            instance.pointerMove(
                screenPosition,
                viewport,
                simulationTime);
        }
        else if (acceptInputSample(inputTime))
        {
            instance.pointerMove(
                screenPosition,
                viewport,
                simulationTime);
        }
        return;
    }

    if (!alwaysOnTrailEnabled_)
    {
        return;
    }
    if (!acceptInputSample(inputTime))
    {
        return;
    }
    if (!alwaysOnTrail_.has_value())
    {
        // The first free-move sample is only an anchor. Connecting it to a
        // pre-toggle or off-screen coordinate would draw a false long segment.
        alwaysOnTrail_.emplace(nextAmbientSeed());
        alwaysOnTrail_->setTrailLengthMultiplier(trailLengthMultiplier_);
        alwaysOnTrail_->setClickTimeScale(clickTimeScale_);
        alwaysOnTrail_->setTrailTimeScale(trailTimeScale_);
        alwaysOnTrail_->startTrail(screenPosition, viewport, simulationTime);
        return;
    }
    alwaysOnTrail_->pointerMove(screenPosition, viewport, simulationTime);
}

void SimulationRuntime::pointerUp(const SimulationTime time)
{
    if (!pointerActive_ || instances_.empty())
    {
        return;
    }

    instances_.back().simulation.pointerUp(time);
    pointerActive_ = false;
    resetInputSamplingPhase();
}

void SimulationRuntime::pointerCancel(const SimulationTime time)
{
    if (pointerActive_ && !instances_.empty())
    {
        instances_.back().simulation.pointerCancel(time);
        pointerActive_ = false;
    }

    retireAlwaysOnTrail(time);
}

void SimulationRuntime::endAlwaysOnTrail(const SimulationTime time)
{
    retireAlwaysOnTrail(time);
}

void SimulationRuntime::advance(const SimulationTime time)
{
    for (RuntimeInstance& runtimeInstance : instances_)
    {
        runtimeInstance.simulation.advance(time);
    }
    if (alwaysOnTrail_.has_value())
    {
        alwaysOnTrail_->advance(time);
    }
}

void SimulationRuntime::onFrameRendered(const SimulationTime time)
{
    for (RuntimeInstance& runtimeInstance : instances_)
    {
        runtimeInstance.simulation.onFrameRendered(time);
    }
    if (alwaysOnTrail_.has_value())
    {
        alwaysOnTrail_->onFrameRendered(time);
    }

    for (auto instance = instances_.begin(); instance != instances_.end();)
    {
        if (instance->simulation.active())
        {
            ++instance;
            continue;
        }

        if (instance->returnsToUnityPool)
        {
            // ComponentPool<T>.AddObject enqueues at the tail. Iterating in
            // activation order preserves the game's FIFO return order when
            // several coroutines complete on the same presented frame.
            unityPool_.push_back(std::move(instance->simulation));
        }
        instance = instances_.erase(instance);
    }
}

void SimulationRuntime::updateUnityTrailTimeScale(const float timeScale)
{
    for (RuntimeInstance& runtimeInstance : instances_)
    {
        runtimeInstance.simulation.updateUnityTrailTimeScale(timeScale);
    }
    if (alwaysOnTrail_.has_value())
    {
        alwaysOnTrail_->updateUnityTrailTimeScale(timeScale);
    }
}

void SimulationRuntime::setClickTimeScale(const float timeScale) noexcept
{
    clickTimeScale_ = std::isfinite(timeScale)
        ? std::clamp(timeScale, 0.01F, 4.0F)
        : 1.0F;
    for (RuntimeInstance& runtimeInstance : instances_)
    {
        runtimeInstance.simulation.setClickTimeScale(clickTimeScale_);
    }
    if (alwaysOnTrail_.has_value())
    {
        alwaysOnTrail_->setClickTimeScale(clickTimeScale_);
    }
}

void SimulationRuntime::setTrailTimeScale(const float timeScale) noexcept
{
    trailTimeScale_ = std::isfinite(timeScale)
        ? std::clamp(timeScale, 0.01F, 4.0F)
        : 1.0F;
    for (RuntimeInstance& runtimeInstance : instances_)
    {
        runtimeInstance.simulation.setTrailTimeScale(trailTimeScale_);
    }
    if (alwaysOnTrail_.has_value())
    {
        alwaysOnTrail_->setTrailTimeScale(trailTimeScale_);
    }
}

void SimulationRuntime::setTrailLengthMultiplier(const float multiplier) noexcept
{
    trailLengthMultiplier_ = normalizeTrailLengthMultiplier(multiplier);
    for (RuntimeInstance& runtimeInstance : instances_)
    {
        runtimeInstance.simulation.setTrailLengthMultiplier(trailLengthMultiplier_);
    }
    if (alwaysOnTrail_.has_value())
    {
        alwaysOnTrail_->setTrailLengthMultiplier(trailLengthMultiplier_);
    }
}

void SimulationRuntime::setInputSamplingRateHz(const std::uint32_t rateHz) noexcept
{
    const std::uint32_t normalized = std::min(
        rateHz,
        maximumInputSamplingRateHz);
    if (normalized == inputSamplingRateHz_)
    {
        return;
    }

    inputSamplingRateHz_ = normalized;
    // A new rate starts with the next Move. Carrying the old phase across two
    // intervals makes the control appear unresponsive near a boundary.
    resetInputSamplingPhase();
}

void SimulationRuntime::setAlwaysOnTrailEnabled(
    const bool enabled,
    const SimulationTime time)
{
    alwaysOnTrailEnabled_ = enabled;
    if (!alwaysOnTrailEnabled_)
    {
        retireAlwaysOnTrail(time);
    }
}

FrameSnapshot SimulationRuntime::snapshot(
    const Viewport viewport,
    const SimulationTime time) const
{
    FrameSnapshot combined{};
    for (const RuntimeInstance& runtimeInstance : instances_)
    {
        FrameSnapshot current = runtimeInstance.simulation.snapshot(viewport, time);
        combined.active = combined.active || current.active;
        combined.pointerHeld = combined.pointerHeld || current.pointerHeld;
        combined.sprites.insert(
            combined.sprites.end(),
            std::make_move_iterator(current.sprites.begin()),
            std::make_move_iterator(current.sprites.end()));

        if (!current.trail.empty())
        {
            combined.trailStrokes.push_back(TrailStroke{
                std::move(current.trail),
                current.trailWidthPixels});
        }
    }
    if (alwaysOnTrail_.has_value())
    {
        FrameSnapshot current = alwaysOnTrail_->snapshot(viewport, time);
        combined.active = combined.active || current.active;
        combined.sprites.insert(
            combined.sprites.end(),
            std::make_move_iterator(current.sprites.begin()),
            std::make_move_iterator(current.sprites.end()));
        if (!current.trail.empty())
        {
            combined.trailStrokes.push_back(TrailStroke{
                std::move(current.trail),
                current.trailWidthPixels});
        }
    }

    // Keep the newest stroke in the single-stroke fields while renderers move
    // to the lossless collection; joining strokes would draw a false bridge.
    if (!combined.trailStrokes.empty())
    {
        const TrailStroke& newest = combined.trailStrokes.back();
        combined.trail = newest.points;
        combined.trailWidthPixels = newest.widthPixels;
    }

    std::stable_sort(
        combined.sprites.begin(),
        combined.sprites.end(),
        [](const Sprite& lhs, const Sprite& rhs)
        {
            return lhs.renderQueue < rhs.renderQueue;
        });
    return combined;
}

bool SimulationRuntime::active() const noexcept
{
    return !instances_.empty() || alwaysOnTrail_.has_value();
}

bool SimulationRuntime::pointerHeld() const noexcept
{
    return pointerActive_;
}

bool SimulationRuntime::alwaysOnTrailEnabled() const noexcept
{
    return alwaysOnTrailEnabled_;
}

std::size_t SimulationRuntime::instanceCount() const noexcept
{
    return instances_.size() + (alwaysOnTrail_.has_value() ? 1U : 0U);
}

std::size_t SimulationRuntime::pooledInstanceCount() const noexcept
{
    return unityPool_.size();
}

std::uint64_t SimulationRuntime::nextUnitySeed() noexcept
{
    // Assign every pooled activation a distinct deterministic stream base.
    const std::uint64_t seed =
        baseSeed_ + unityActivationCount_ * randomStreamStep;
    ++unityActivationCount_;
    return seed;
}

std::uint64_t SimulationRuntime::nextAmbientSeed() noexcept
{
    // Ambient trails are a desktop-only enhancement. Their independent domain
    // prevents free movement from perturbing the strict Unity click sequence.
    const std::uint64_t seed = (baseSeed_ ^ ambientRandomStream)
        + ambientActivationCount_ * randomStreamStep;
    ++ambientActivationCount_;
    return seed;
}

Simulation& SimulationRuntime::acquirePressedInstance(
    const SimulationTime simulationTime)
{
    // A physical press owns the live stroke until release. Retiring the
    // movement-only stroke prevents duplicate geometry while preserving fade.
    retireAlwaysOnTrail(simulationTime);
    if (unityPool_.empty())
    {
        instances_.push_back(RuntimeInstance{Simulation(nextUnitySeed()), true});
    }
    else
    {
        instances_.push_back(RuntimeInstance{
            std::move(unityPool_.front()),
            true});
        unityPool_.pop_front();
        instances_.back().simulation.preparePooledActivation(nextUnitySeed());
    }

    Simulation& instance = instances_.back().simulation;
    instance.setTrailLengthMultiplier(trailLengthMultiplier_);
    instance.setClickTimeScale(clickTimeScale_);
    instance.setTrailTimeScale(trailTimeScale_);
    return instance;
}

bool SimulationRuntime::acceptInputSample(const SimulationTime inputTime) noexcept
{
    if (inputSamplingRateHz_ == 0U)
    {
        return true;
    }
    if (!lastInputSampleAt_.has_value())
    {
        lastInputSampleAt_ = inputTime;
        return true;
    }

    const std::chrono::duration<double> elapsed = inputTime - *lastInputSampleAt_;
    const double intervalSeconds = 1.0
        / static_cast<double>(inputSamplingRateHz_);
    if (elapsed.count() < intervalSeconds)
    {
        return false;
    }

    // Advance the time phase before Unity's independent spatial threshold.
    // A retained but spatially tiny Move still consumes one input sample.
    lastInputSampleAt_ = inputTime;
    return true;
}

void SimulationRuntime::resetInputSamplingPhase() noexcept
{
    lastInputSampleAt_.reset();
}

void SimulationRuntime::retireAlwaysOnTrail(const SimulationTime time)
{
    resetInputSamplingPhase();
    if (!alwaysOnTrail_.has_value())
    {
        return;
    }

    alwaysOnTrail_->pointerCancel(time);
    // Always-on trail is a desktop enhancement, not an FXTouch acquired from
    // the game's SyncComponentPool. Retain its fade without pooling it.
    instances_.push_back(RuntimeInstance{
        std::move(*alwaysOnTrail_),
        false});
    alwaysOnTrail_.reset();
}

}
