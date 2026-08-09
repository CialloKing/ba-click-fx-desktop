#include "bafx/fx/simulation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace bafx::fx
{
namespace
{

constexpr float triangleLocalScale = 0.3078824F;
constexpr float clickShapeRadiusWorld = 0.3F * triangleLocalScale;
constexpr float dragShapeRadiusWorld = 0.15F * triangleLocalScale;
constexpr float dragEmissionStepWorld = 1.0F / 5.0F;
constexpr float trailPointStepWorld = 0.01F;
constexpr float trailLifetimeSeconds = 0.3F;
constexpr float trailWidthWorld = 0.005F;
constexpr float ringLifetimeSeconds = 0.6F;
constexpr float ringAngularVelocityMultiplier = 11.170107F;
constexpr std::uint32_t maximumDragParticles = 50U;
constexpr std::uint32_t releaseFrameCount = 60U;

struct CurveKey
{
    float time{0.0F};
    float value{0.0F};
    float inSlope{0.0F};
    float outSlope{0.0F};
};

struct ColorKey
{
    float time{0.0F};
    ColorF color{};
};

[[nodiscard]] float clampUnit(const float value) noexcept
{
    return std::clamp(value, 0.0F, 1.0F);
}

template<std::size_t keyCount>
[[nodiscard]] float evaluateHermiteCurve(
    const std::array<CurveKey, keyCount>& keys,
    const float time) noexcept
{
    static_assert(keyCount > 0U);
    if (time <= keys.front().time)
    {
        return keys.front().value;
    }
    if (time >= keys.back().time)
    {
        return keys.back().value;
    }

    for (std::size_t index = 1U; index < keys.size(); ++index)
    {
        const CurveKey& right = keys[index];
        if (time > right.time)
        {
            continue;
        }

        const CurveKey& left = keys[index - 1U];
        const float duration = right.time - left.time;
        const float t = (time - left.time) / duration;
        const float tSquared = t * t;
        const float tCubed = tSquared * t;
        const float h00 = 2.0F * tCubed - 3.0F * tSquared + 1.0F;
        const float h10 = tCubed - 2.0F * tSquared + t;
        const float h01 = -2.0F * tCubed + 3.0F * tSquared;
        const float h11 = tCubed - tSquared;
        return h00 * left.value
            + h10 * duration * left.outSlope
            + h01 * right.value
            + h11 * duration * right.inSlope;
    }

    return keys.back().value;
}

template<std::size_t keyCount>
[[nodiscard]] float integrateHermiteCurve(
    const std::array<CurveKey, keyCount>& keys,
    const float time) noexcept
{
    static_assert(keyCount > 0U);
    const float t = clampUnit(time);
    float integral = keys.front().value * std::min(t, keys.front().time);
    if (t <= keys.front().time)
    {
        return integral;
    }

    for (std::size_t index = 1U; index < keys.size(); ++index)
    {
        const CurveKey& left = keys[index - 1U];
        const CurveKey& right = keys[index];
        const float duration = right.time - left.time;
        if (duration <= 0.0F)
        {
            continue;
        }

        const float segmentEnd = std::min(t, right.time);
        const float u = clampUnit((segmentEnd - left.time) / duration);
        const float uSquared = u * u;
        const float uCubed = uSquared * u;
        const float uFourth = uCubed * u;
        const float h00Integral = 0.5F * uFourth - uCubed + u;
        const float h10Integral = 0.25F * uFourth
            - (2.0F / 3.0F) * uCubed
            + 0.5F * uSquared;
        const float h01Integral = -0.5F * uFourth + uCubed;
        const float h11Integral = 0.25F * uFourth
            - (1.0F / 3.0F) * uCubed;
        integral += duration * (
            h00Integral * left.value
            + h10Integral * duration * left.outSlope
            + h01Integral * right.value
            + h11Integral * duration * right.inSlope);
        if (t <= right.time)
        {
            return integral;
        }
    }

    return integral + (t - keys.back().time) * keys.back().value;
}

[[nodiscard]] float ringRotationDelta(
    const float angularBlend,
    const float normalizedAge) noexcept
{
    constexpr std::array minimumKeys{
        CurveKey{0.14903903F, 1.0F, 0.0F, 0.0F},
        CurveKey{1.0F, 0.45561826F, 0.0F, 0.0F}};
    constexpr std::array maximumKeys{
        CurveKey{0.15865384F, 0.79881656F, 0.0F, 0.0F},
        CurveKey{1.0F, -0.06509134F, 0.0F, 0.0F}};
    const float minimumIntegral = integrateHermiteCurve(
        minimumKeys,
        normalizedAge);
    const float maximumIntegral = integrateHermiteCurve(
        maximumKeys,
        normalizedAge);
    const float blend = clampUnit(angularBlend);
    const float blendedIntegral = minimumIntegral
        + (maximumIntegral - minimumIntegral) * blend;

    // Unity stores angular velocity over normalized lifetime, so integrate in
    // curve space and multiply by the particle lifetime to recover radians.
    return blendedIntegral
        * ringAngularVelocityMultiplier
        * ringLifetimeSeconds;
}

[[nodiscard]] float length(const PointF value) noexcept
{
    return std::sqrt(value.x * value.x + value.y * value.y);
}

[[nodiscard]] PointF add(const PointF lhs, const PointF rhs) noexcept
{
    return PointF{lhs.x + rhs.x, lhs.y + rhs.y};
}

[[nodiscard]] PointF subtract(const PointF lhs, const PointF rhs) noexcept
{
    return PointF{lhs.x - rhs.x, lhs.y - rhs.y};
}

[[nodiscard]] PointF multiply(const PointF value, const float scale) noexcept
{
    return PointF{value.x * scale, value.y * scale};
}

[[nodiscard]] PointF direction(const float angle) noexcept
{
    return PointF{std::cos(angle), std::sin(angle)};
}

[[nodiscard]] ColorF lerpColor(
    const ColorF from,
    const ColorF to,
    const float amount) noexcept
{
    const float t = clampUnit(amount);
    return ColorF{
        from.r + (to.r - from.r) * t,
        from.g + (to.g - from.g) * t,
        from.b + (to.b - from.b) * t,
        from.a + (to.a - from.a) * t};
}

template<std::size_t keyCount>
[[nodiscard]] ColorF evaluateColorGradient(
    const std::array<ColorKey, keyCount>& keys,
    const float time) noexcept
{
    static_assert(keyCount > 0U);
    if (time <= keys.front().time)
    {
        return keys.front().color;
    }
    for (std::size_t index = 1U; index < keys.size(); ++index)
    {
        if (time <= keys[index].time)
        {
            const float amount = (time - keys[index - 1U].time)
                / (keys[index].time - keys[index - 1U].time);
            return lerpColor(keys[index - 1U].color, keys[index].color, amount);
        }
    }
    return keys.back().color;
}

[[nodiscard]] float diskSizeCurve(const float normalizedAge) noexcept
{
    constexpr std::array keys{
        CurveKey{0.0F, 0.32583582F, 2.4004734F, 2.4004734F},
        CurveKey{0.21392822F, 0.7159773F, 0.9115745F, 0.9115745F},
        CurveKey{1.0F, 1.0F, 0.0F, 0.0F}};
    return 2.0F * evaluateHermiteCurve(keys, normalizedAge);
}

[[nodiscard]] float ringSizeCurve(const float normalizedAge) noexcept
{
    constexpr std::array keys{
        CurveKey{0.007209778F, 0.42050898F, 2.4004734F, 2.4004734F},
        CurveKey{0.21392822F, 0.7159773F, 0.9115745F, 0.9115745F},
        CurveKey{1.0F, 1.0F, 0.0F, 0.0F}};
    return evaluateHermiteCurve(keys, normalizedAge);
}

[[nodiscard]] float triangleSizeCurve(const float normalizedAge) noexcept
{
    constexpr std::array keys{
        CurveKey{0.0F, 0.0F, 0.0F, 0.0F},
        CurveKey{0.15445095F, 1.0F, 0.0F, 0.0F},
        CurveKey{1.0F, 0.0F, -2.1621501F, -2.1621501F}};
    return evaluateHermiteCurve(keys, normalizedAge);
}

[[nodiscard]] float triangleAlphaCurve(const float normalizedAge) noexcept
{
    constexpr std::array times{
        0.288242924F,
        0.364705882F,
        0.470588235F,
        0.573525597F,
        0.667643244F,
        0.755886168F,
        0.852948806F,
        1.0F};
    constexpr std::array values{1.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F};

    if (normalizedAge <= times.front())
    {
        return values.front();
    }
    for (std::size_t index = 1; index < times.size(); ++index)
    {
        if (normalizedAge <= times[index])
        {
            const float t = (normalizedAge - times[index - 1])
                / (times[index] - times[index - 1]);
            return values[index - 1] + (values[index] - values[index - 1]) * t;
        }
    }
    return values.back();
}

[[nodiscard]] ColorF diskColor(const float normalizedAge) noexcept
{
    const ColorF white{1.0F, 1.0F, 1.0F, 1.0F};
    const ColorF blue{0.24056602F, 0.39061815F, 1.0F, 1.0F};
    const float colorT = normalizedAge / 0.120592050F;
    ColorF color = lerpColor(white, blue, colorT);
    const float fadeStart = 0.108827344F;
    color.a = normalizedAge <= fadeStart
        ? 1.0F
        : 1.0F - (normalizedAge - fadeStart) / (1.0F - fadeStart);
    color.a = clampUnit(color.a);
    return color;
}

[[nodiscard]] ColorF ringColor(const float normalizedAge) noexcept
{
    const ColorF white{1.0F, 1.0F, 1.0F, 1.0F};
    const ColorF blue{0.2971698F, 0.6532865F, 1.0F, 1.0F};
    if (normalizedAge <= 0.111772335F)
    {
        return white;
    }
    return lerpColor(
        white,
        blue,
        (normalizedAge - 0.111772335F) / (0.500007630F - 0.111772335F));
}

[[nodiscard]] ColorF triangleColor(const float normalizedAge) noexcept
{
    constexpr ColorF startColor{0.5377358F, 0.5377358F, 0.5377358F, 1.0F};
    constexpr std::array keys{
        ColorKey{0.182360571F, ColorF{1.0F, 1.0F, 1.0F, 1.0F}},
        ColorKey{0.282352941F, ColorF{0.3726415F, 0.7731873F, 1.0F, 1.0F}},
        ColorKey{0.461768521F, ColorF{0.37254903F, 0.7725491F, 1.0F, 1.0F}},
        ColorKey{0.661768521F, ColorF{0.3529412F, 0.7294118F, 0.9450981F, 1.0F}},
        ColorKey{0.826474403F, ColorF{0.37254903F, 0.7725491F, 1.0F, 1.0F}}};
    const ColorF lifetimeColor = evaluateColorGradient(keys, normalizedAge);
    ColorF color{
        startColor.r * lifetimeColor.r,
        startColor.g * lifetimeColor.g,
        startColor.b * lifetimeColor.b,
        startColor.a * lifetimeColor.a};
    color.a *= triangleAlphaCurve(normalizedAge);
    return color;
}

[[nodiscard]] float dissolveThreshold(const float normalizedAge) noexcept
{
    constexpr std::array keys{
        CurveKey{0.0F, 1.0F, 0.0F, 0.0F},
        CurveKey{0.2F, 0.0F, 0.0F, 2.4249368F},
        CurveKey{1.0F, 1.0F, 0.27735636F, 0.27735636F}};
    return evaluateHermiteCurve(keys, normalizedAge);
}

}

Simulation::Random::Random(const std::uint64_t seed) noexcept
    : state_(seed == 0U ? 0x9E3779B97F4A7C15ULL : seed)
{
}

float Simulation::Random::unit() noexcept
{
    // SplitMix64 provides deterministic independent streams without platform library drift.
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t value = state_;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    value ^= value >> 31U;
    const std::uint32_t high = static_cast<std::uint32_t>(value >> 40U);
    return static_cast<float>(high) / static_cast<float>(1U << 24U);
}

float Simulation::Random::range(const float minimum, const float maximum) noexcept
{
    return minimum + (maximum - minimum) * unit();
}

Simulation::Simulation(const std::uint64_t seed)
    : baseSeed_(seed)
    , random_(seed)
{
    rings_.reserve(2);
    triangles_.reserve(4U + maximumDragParticles);
    trail_.reserve(128);
}

void Simulation::pointerDown(
    const PointF screenPosition,
    const Viewport viewport,
    const SimulationTime time)
{
    reset(screenToWorld(screenPosition, viewport), time);
}

void Simulation::pointerMove(
    const PointF screenPosition,
    const Viewport viewport,
    const SimulationTime time)
{
    if (!active_ || !pointerHeld_ || time < lastAdvancedAt_)
    {
        return;
    }

    const PointF nextWorld = screenToWorld(screenPosition, viewport);
    emitAlongDrag(pointerWorld_, nextWorld, time);
    pointerWorld_ = nextWorld;
    appendTrailPoint(pointerWorld_, time);
}

void Simulation::pointerUp(const SimulationTime)
{
    if (!active_ || !pointerHeld_)
    {
        return;
    }
    pointerHeld_ = false;
    releasedFrames_ = 0;
}

void Simulation::advance(const SimulationTime time)
{
    if (!active_ || time < lastAdvancedAt_)
    {
        return;
    }

    lastAdvancedAt_ = time;
    const auto trailEnd = std::remove_if(
        trail_.begin(),
        trail_.end(),
        [time](const StoredTrailPoint& point)
        {
            return ageSeconds(time, point.createdAt) > trailLifetimeSeconds;
        });
    trail_.erase(trailEnd, trail_.end());

    const auto particleEnd = std::remove_if(
        triangles_.begin(),
        triangles_.end(),
        [time](const MovingParticle& particle)
        {
            return ageSeconds(time, particle.bornAt) > particle.lifetimeSeconds;
        });
    triangles_.erase(particleEnd, triangles_.end());
}

void Simulation::onFrameRendered()
{
    if (!active_ || pointerHeld_)
    {
        return;
    }

    ++releasedFrames_;
    if (releasedFrames_ >= releaseFrameCount)
    {
        // Unity waits until the 60th frame has rendered before Stop clears the pooled effect.
        active_ = false;
        rings_.clear();
        triangles_.clear();
        trail_.clear();
    }
}

FrameSnapshot Simulation::snapshot(const Viewport viewport, const SimulationTime time) const
{
    FrameSnapshot frame{};
    frame.active = active_;
    frame.pointerHeld = pointerHeld_;
    frame.trailWidthPixels = trailWidthWorld * worldToPixels(viewport);
    if (!active_ || viewport.width == 0U || viewport.height == 0U)
    {
        return frame;
    }

    const double effectAge = ageSeconds(time, startedAt_);
    if (effectAge >= 0.0 && effectAge <= 0.2)
    {
        const float normalizedAge = static_cast<float>(effectAge / 0.2);
        frame.sprites.push_back(Sprite{
            SpriteKind::CenterDisk,
            worldToScreen(effectOriginWorld_, viewport),
            0.12F * diskSizeCurve(normalizedAge) * worldToPixels(viewport),
            0.0F,
            diskColor(normalizedAge),
            2.0F,
            0.0F,
            0U,
            4499,
            true});
    }

    if (effectAge >= 0.0 && effectAge <= ringLifetimeSeconds)
    {
        const float normalizedAge = static_cast<float>(
            effectAge / ringLifetimeSeconds);
        for (const RingParticle& ring : rings_)
        {
            // The reconstructed Cylinder002 AABB has a full extent of 2 * 1.0636685.
            const float size = ring.startSizeWorld
                * ringSizeCurve(normalizedAge)
                * 2.127337F
                * worldToPixels(viewport);
            frame.sprites.push_back(Sprite{
                SpriteKind::DissolveRing,
                worldToScreen(effectOriginWorld_, viewport),
                size,
                ring.initialRotationRadians
                    + ringRotationDelta(ring.angularBlend, normalizedAge),
                ringColor(normalizedAge),
                5.992157F,
                dissolveThreshold(normalizedAge),
                0U,
                4499,
                true});
        }
    }

    for (const MovingParticle& particle : triangles_)
    {
        const double age = ageSeconds(time, particle.bornAt);
        if (age < 0.0 || age > particle.lifetimeSeconds)
        {
            continue;
        }

        const float normalizedAge = static_cast<float>(age / particle.lifetimeSeconds);
        const PointF worldPosition = add(
            particle.originWorld,
            multiply(particle.velocityWorld, static_cast<float>(age)));
        frame.sprites.push_back(Sprite{
            SpriteKind::Triangle,
            worldToScreen(worldPosition, viewport),
            particle.startSizeWorld
                * triangleSizeCurve(normalizedAge)
                * worldToPixels(viewport),
            0.0F,
            triangleColor(normalizedAge),
            5.992157F,
            0.0F,
            particle.atlasFrame,
            4550,
            false});
    }

    for (const StoredTrailPoint& point : trail_)
    {
        const float normalizedAge = clampUnit(
            static_cast<float>(ageSeconds(time, point.createdAt) / trailLifetimeSeconds));
        frame.trail.push_back(TrailPoint{worldToScreen(point.world, viewport), normalizedAge});
    }

    std::stable_sort(
        frame.sprites.begin(),
        frame.sprites.end(),
        [](const Sprite& lhs, const Sprite& rhs)
        {
            return lhs.renderQueue < rhs.renderQueue;
        });
    return frame;
}

bool Simulation::active() const noexcept
{
    return active_;
}

PointF Simulation::screenToWorld(const PointF screen, const Viewport viewport) noexcept
{
    if (viewport.width == 0U || viewport.height == 0U)
    {
        return PointF{};
    }
    const float width = static_cast<float>(viewport.width);
    const float height = static_cast<float>(viewport.height);
    const float aspect = width / height;
    return PointF{
        (screen.x / width * 2.0F - 1.0F) * aspect,
        1.0F - screen.y / height * 2.0F};
}

PointF Simulation::worldToScreen(const PointF world, const Viewport viewport) noexcept
{
    if (viewport.width == 0U || viewport.height == 0U)
    {
        return PointF{};
    }
    const float width = static_cast<float>(viewport.width);
    const float height = static_cast<float>(viewport.height);
    const float aspect = width / height;
    return PointF{
        (world.x / aspect + 1.0F) * 0.5F * width,
        (1.0F - world.y) * 0.5F * height};
}

float Simulation::worldToPixels(const Viewport viewport) noexcept
{
    return static_cast<float>(viewport.height) * 0.5F;
}

double Simulation::ageSeconds(const SimulationTime now, const SimulationTime then) noexcept
{
    return std::chrono::duration<double>(now - then).count();
}

void Simulation::reset(const PointF worldPosition, const SimulationTime time)
{
    ++activationCount_;
    random_ = Random(baseSeed_ + activationCount_ * 0x9E3779B97F4A7C15ULL);
    active_ = true;
    pointerHeld_ = true;
    startedAt_ = time;
    lastAdvancedAt_ = time;
    releasedFrames_ = 0;
    effectOriginWorld_ = worldPosition;
    pointerWorld_ = worldPosition;
    lastEmissionWorld_ = worldPosition;
    dragDistanceRemainderWorld_ = 0.0F;
    rings_.clear();
    triangles_.clear();
    trail_.clear();

    for (std::uint32_t index = 0; index < 2U; ++index)
    {
        rings_.push_back(RingParticle{
            random_.range(0.12F, 0.14F),
            random_.range(0.0F, 2.0F * std::numbers::pi_v<float>),
            random_.unit()});
    }
    emitClickTriangles(time);
    appendTrailPoint(worldPosition, time);
}

void Simulation::emitClickTriangles(const SimulationTime time)
{
    for (std::uint32_t index = 0; index < 4U; ++index)
    {
        // Unity's Circle arc is Random: burst particles sample independent positions.
        const float angle = random_.range(0.0F, 2.0F * std::numbers::pi_v<float>);
        const PointF radial = direction(angle);
        triangles_.push_back(MovingParticle{
            add(effectOriginWorld_, multiply(radial, clickShapeRadiusWorld)),
            multiply(radial, random_.range(0.3F, 0.4F) * triangleLocalScale),
            time,
            random_.range(0.6F, 0.7F),
            random_.range(0.1F, 0.2F) * triangleLocalScale,
            index % 2U,
            false});
    }
}

void Simulation::emitDragTriangle(const PointF worldPosition, const SimulationTime time)
{
    const auto dragCount = static_cast<std::uint32_t>(std::count_if(
        triangles_.begin(),
        triangles_.end(),
        [](const MovingParticle& particle)
        {
            return particle.dragParticle;
        }));
    if (dragCount >= maximumDragParticles)
    {
        return;
    }

    const float angle = random_.range(0.0F, 2.0F * std::numbers::pi_v<float>);
    const PointF radial = direction(angle);
    triangles_.push_back(MovingParticle{
        add(worldPosition, multiply(radial, dragShapeRadiusWorld)),
        multiply(radial, random_.range(0.2F, 0.3F) * triangleLocalScale),
        time,
        random_.range(0.2F, 0.4F),
        random_.range(0.1F, 0.2F) * triangleLocalScale,
        static_cast<std::uint32_t>(random_.unit() * 2.0F) % 2U,
        true});
}

void Simulation::appendTrailPoint(const PointF worldPosition, const SimulationTime time)
{
    if (trail_.empty()
        || length(subtract(worldPosition, trail_.back().world)) >= trailPointStepWorld)
    {
        trail_.push_back(StoredTrailPoint{worldPosition, time});
    }
}

void Simulation::emitAlongDrag(
    const PointF from,
    const PointF to,
    const SimulationTime time)
{
    const PointF segment = subtract(to, from);
    const float segmentLength = length(segment);
    if (segmentLength <= std::numeric_limits<float>::epsilon())
    {
        return;
    }

    float consumed = 0.0F;
    float distanceUntilEmission = dragEmissionStepWorld - dragDistanceRemainderWorld_;
    while (consumed + distanceUntilEmission <= segmentLength)
    {
        consumed += distanceUntilEmission;
        const float interpolation = consumed / segmentLength;
        const PointF position = add(from, multiply(segment, interpolation));
        emitDragTriangle(position, time);
        lastEmissionWorld_ = position;
        dragDistanceRemainderWorld_ = 0.0F;
        distanceUntilEmission = dragEmissionStepWorld;
    }
    dragDistanceRemainderWorld_ += segmentLength - consumed;
}

}
