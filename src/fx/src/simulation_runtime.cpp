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
constexpr float minimumTrailLengthMultiplier = 0.0F;
constexpr float maximumTrailLengthMultiplier = 3.0F;

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
}

void SimulationRuntime::pointerDown(
    const PointF screenPosition,
    const Viewport viewport,
    const SimulationTime time)
{
    // Unity keeps one object for the currently held pointer, so duplicate down
    // notifications must not replace that object or restart its particles.
    if (pointerActive_)
    {
        return;
    }

    // A physical press owns the live stroke until release. Retiring the
    // movement-only stroke prevents duplicate geometry while preserving its fade.
    retireAlwaysOnTrail(time);
    instances_.emplace_back(nextSeed());
    instances_.back().setTrailLengthMultiplier(trailLengthMultiplier_);
    instances_.back().pointerDown(screenPosition, viewport, time);
    pointerActive_ = true;
}

void SimulationRuntime::pointerMove(
    const PointF screenPosition,
    const Viewport viewport,
    const SimulationTime time)
{
    if (pointerActive_ && !instances_.empty())
    {
        instances_.back().pointerMove(screenPosition, viewport, time);
        return;
    }

    if (!alwaysOnTrailEnabled_)
    {
        return;
    }
    if (!alwaysOnTrail_.has_value())
    {
        // The first free-move sample is only an anchor. Connecting it to a
        // pre-toggle or off-screen coordinate would draw a false long segment.
        alwaysOnTrail_.emplace(nextSeed());
        alwaysOnTrail_->setTrailLengthMultiplier(trailLengthMultiplier_);
        alwaysOnTrail_->startTrail(screenPosition, viewport, time);
        return;
    }
    alwaysOnTrail_->pointerMove(screenPosition, viewport, time);
}

void SimulationRuntime::pointerUp(const SimulationTime time)
{
    if (!pointerActive_ || instances_.empty())
    {
        return;
    }

    instances_.back().pointerUp(time);
    pointerActive_ = false;
}

void SimulationRuntime::pointerCancel(const SimulationTime time)
{
    if (pointerActive_ && !instances_.empty())
    {
        instances_.back().pointerCancel(time);
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
    for (Simulation& instance : instances_)
    {
        instance.advance(time);
    }
    if (alwaysOnTrail_.has_value())
    {
        alwaysOnTrail_->advance(time);
    }
}

void SimulationRuntime::onFrameRendered()
{
    for (Simulation& instance : instances_)
    {
        instance.onFrameRendered();
    }
    if (alwaysOnTrail_.has_value())
    {
        alwaysOnTrail_->onFrameRendered();
    }

    const auto inactiveBegin = std::remove_if(
        instances_.begin(),
        instances_.end(),
        [](const Simulation& instance)
        {
            return !instance.active();
        });
    instances_.erase(inactiveBegin, instances_.end());
}

void SimulationRuntime::setTrailLengthMultiplier(const float multiplier) noexcept
{
    trailLengthMultiplier_ = normalizeTrailLengthMultiplier(multiplier);
    for (Simulation& instance : instances_)
    {
        instance.setTrailLengthMultiplier(trailLengthMultiplier_);
    }
    if (alwaysOnTrail_.has_value())
    {
        alwaysOnTrail_->setTrailLengthMultiplier(trailLengthMultiplier_);
    }
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
    for (const Simulation& instance : instances_)
    {
        FrameSnapshot current = instance.snapshot(viewport, time);
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

std::uint64_t SimulationRuntime::nextSeed() noexcept
{
    // Match repeated activation of one Simulation while giving each retained
    // instance an independent deterministic random stream.
    const std::uint64_t seed = baseSeed_ + activationCount_ * randomStreamStep;
    ++activationCount_;
    return seed;
}

void SimulationRuntime::retireAlwaysOnTrail(const SimulationTime time)
{
    if (!alwaysOnTrail_.has_value())
    {
        return;
    }

    alwaysOnTrail_->pointerCancel(time);
    instances_.push_back(std::move(*alwaysOnTrail_));
    alwaysOnTrail_.reset();
}

}
