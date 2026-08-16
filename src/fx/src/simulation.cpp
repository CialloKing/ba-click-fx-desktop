#include "bafx/fx/simulation.hpp"

#include "bafx/core/color_space.hpp"

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
constexpr double trailLifetimeSeconds = 0.3;
// The script default is 0.3333, but the shipped FX_Touch Trail instance
// serializes killUnderTimeScale as 0.19. The instance override is the value
// observed by the game and therefore owns this native simulation contract.
constexpr float trailParkingTimeScaleThreshold = 0.19F;
constexpr float minimumTrailLengthMultiplier = 0.0F;
constexpr float maximumTrailLengthMultiplier = 3.0F;
constexpr float minimumTimeScale = 0.01F;
constexpr float maximumTimeScale = 4.0F;
constexpr float trailWidthWorld = 0.005F;
constexpr float referenceWorldToPixels = 540.0F;
constexpr float webRingMeshOuterRadius = 1.0636684F;
constexpr float minimumParticleLifetimeMs = 1.0F;
constexpr float maximumParticleLifetimeMs = 10000.0F;
constexpr std::uint32_t maximumRingCount = 64U;
constexpr float maximumRingRadius = 2000.0F;
constexpr float maximumRingAngularVelocityMultiplier = 100.0F;
constexpr float maximumParticleTimestepSeconds = 0.03F;
constexpr std::uint32_t maximumDragParticles = 50U;
constexpr double releaseLifetimeSeconds = 1.0;
constexpr std::uint64_t atlasRandomStream = 0xD1B54A32D192ED03ULL;

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

[[nodiscard]] float normalizeTimeScale(const float timeScale) noexcept
{
    if (!std::isfinite(timeScale))
    {
        return 1.0F;
    }

    return std::clamp(timeScale, minimumTimeScale, maximumTimeScale);
}

[[nodiscard]] float normalizeFiniteRange(
    const float value,
    const float fallback,
    const float minimum,
    const float maximum) noexcept
{
    if (!std::isfinite(value))
    {
        return fallback;
    }
    return std::clamp(value, minimum, maximum);
}

[[nodiscard]] ClickParticleSettings normalizeClickParticleSettings(
    ClickParticleSettings settings) noexcept
{
    const ClickParticleSettings defaults{};
    settings.diskLifetimeMs = normalizeFiniteRange(
        settings.diskLifetimeMs,
        defaults.diskLifetimeMs,
        minimumParticleLifetimeMs,
        maximumParticleLifetimeMs);
    settings.ringsCount = std::min(settings.ringsCount, maximumRingCount);
    settings.ringsLifetimeMs = normalizeFiniteRange(
        settings.ringsLifetimeMs,
        defaults.ringsLifetimeMs,
        minimumParticleLifetimeMs,
        maximumParticleLifetimeMs);
    settings.ringsRadiusMin = normalizeFiniteRange(
        settings.ringsRadiusMin,
        defaults.ringsRadiusMin,
        0.0F,
        maximumRingRadius);
    settings.ringsRadiusMax = normalizeFiniteRange(
        settings.ringsRadiusMax,
        defaults.ringsRadiusMax,
        0.0F,
        maximumRingRadius);
    settings.ringsAngularVelocityMultiplier = normalizeFiniteRange(
        settings.ringsAngularVelocityMultiplier,
        defaults.ringsAngularVelocityMultiplier,
        0.0F,
        maximumRingAngularVelocityMultiplier);
    settings.ringsRotationDirection = normalizeFiniteRange(
        settings.ringsRotationDirection,
        defaults.ringsRotationDirection,
        -1.0F,
        1.0F);
    return settings;
}

[[nodiscard]] float millisecondsToSeconds(const float milliseconds) noexcept
{
    return milliseconds * 0.001F;
}

[[nodiscard]] float referenceRadiusToStartSizeWorld(
    const float radiusPixels) noexcept
{
    return radiusPixels / (referenceWorldToPixels * webRingMeshOuterRadius);
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
    const float fromAgeSeconds,
    const float toAgeSeconds,
    const ClickParticleSettings& settings) noexcept
{
    if (toAgeSeconds <= fromAgeSeconds)
    {
        return 0.0F;
    }

    constexpr std::array minimumKeys{
        CurveKey{0.14903903F, 1.0F, 0.0F, 0.0F},
        CurveKey{1.0F, 0.45561826F, 0.0F, 0.0F}};
    constexpr std::array maximumKeys{
        CurveKey{0.15865384F, 0.79881656F, 0.0F, 0.0F},
        CurveKey{1.0F, -0.06509134F, 0.0F, 0.0F}};
    const float lifetimeSeconds = millisecondsToSeconds(
        settings.ringsLifetimeMs);
    const float fromNormalizedAge = fromAgeSeconds / lifetimeSeconds;
    const float toNormalizedAge = toAgeSeconds / lifetimeSeconds;
    const float minimumIntegralFrom = integrateHermiteCurve(
        minimumKeys,
        fromNormalizedAge);
    const float minimumIntegralTo = integrateHermiteCurve(
        minimumKeys,
        toNormalizedAge);
    const float maximumIntegralFrom = integrateHermiteCurve(
        maximumKeys,
        fromNormalizedAge);
    const float maximumIntegralTo = integrateHermiteCurve(
        maximumKeys,
        toNormalizedAge);
    const float blend = clampUnit(angularBlend);
    const float blendedIntegralFrom = minimumIntegralFrom
        + (maximumIntegralFrom - minimumIntegralFrom) * blend;
    const float blendedIntegralTo = minimumIntegralTo
        + (maximumIntegralTo - minimumIntegralTo) * blend;

    // Unity stores angular velocity over normalized lifetime, so integrate in
    // curve space and multiply by the particle lifetime to recover radians.
    // Web Canvas positive rotation is screen-clockwise; native mesh rotation
    // is screen-counterclockwise, so the public direction changes sign here.
    return (blendedIntegralTo - blendedIntegralFrom)
        * settings.ringsAngularVelocityMultiplier
        * lifetimeSeconds
        * -settings.ringsRotationDirection;
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

[[nodiscard]] SimulationTime interpolateTime(
    const SimulationTime from,
    const SimulationTime to,
    const float amount) noexcept
{
    const auto elapsedNanoseconds = static_cast<SimulationTime::rep>(
        static_cast<double>((to - from).count()) * static_cast<double>(amount));
    return from + SimulationTime{elapsedNanoseconds};
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

[[nodiscard]] ColorF applyUnityActiveColorSpace(const ColorF color) noexcept
{
    // ParticleSystemRenderer converts the evaluated gamma color once before upload.
    const bafx::core::Float3 linear = bafx::core::srgbToLinear(
        bafx::core::Float3{color.r, color.g, color.b});
    return ColorF{linear.r, linear.g, linear.b, color.a};
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
    constexpr ColorF white{1.0F, 1.0F, 1.0F, 1.0F};
    constexpr ColorF blue{0.24056602F, 0.39061815F, 1.0F, 1.0F};
    const float colorT = normalizedAge / 0.120592050F;
    ColorF color = lerpColor(white, blue, colorT);
    const float fadeStart = 0.108827344F;
    color.a = normalizedAge <= fadeStart
        ? 1.0F
        : 1.0F - (normalizedAge - fadeStart) / (1.0F - fadeStart);
    color.a = clampUnit(color.a);
    return applyUnityActiveColorSpace(color);
}

[[nodiscard]] ColorF ringColor(const float normalizedAge) noexcept
{
    constexpr ColorF white{1.0F, 1.0F, 1.0F, 1.0F};
    constexpr ColorF blue{0.2971698F, 0.6532865F, 1.0F, 1.0F};
    if (normalizedAge <= 0.111772335F)
    {
        return applyUnityActiveColorSpace(white);
    }
    const ColorF color = lerpColor(
        white,
        blue,
        (normalizedAge - 0.111772335F) / (0.500007630F - 0.111772335F));
    return applyUnityActiveColorSpace(color);
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
    return applyUnityActiveColorSpace(color);
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

void applyGlobalScale(FrameSnapshot& snapshot, const float scale) noexcept
{
    const float safeScale = std::isfinite(scale) && scale > 0.0F
        ? scale
        : 1.0F;
    for (Sprite& sprite : snapshot.sprites)
    {
        sprite.sizePixels *= safeScale;
        if (sprite.scaleCenterWithGlobalScale)
        {
            const PointF offset = subtract(
                sprite.centerPixels,
                sprite.globalScalePivotPixels);
            sprite.centerPixels = add(
                sprite.globalScalePivotPixels,
                multiply(offset, safeScale));
        }
    }
    snapshot.trailWidthPixels *= safeScale;
    for (TrailStroke& stroke : snapshot.trailStrokes)
    {
        stroke.widthPixels *= safeScale;
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
    , atlasRandom_(seed ^ atlasRandomStream)
{
    rings_.reserve(2);
    triangles_.reserve(4U + maximumDragParticles);
    trail_.reserve(128);
    trailParkingPoints_.reserve(128);
}

void Simulation::preparePooledActivation(const std::uint64_t seed) noexcept
{
    baseSeed_ = seed;
    activationCount_ = 0U;
    random_ = Random(seed);
    atlasRandom_ = Random(seed ^ atlasRandomStream);
    // FxTrailTimeScale mode, cached points and renderer state intentionally
    // survive. FXTouch.Stop does not reset that sibling component in Unity.
}

void Simulation::setTrailLengthMultiplier(const float multiplier) noexcept
{
    trailLengthMultiplier_ = normalizeTrailLengthMultiplier(multiplier);
    if (trailLengthMultiplier_ == 0.0F)
    {
        // A zero-length product setting must remove retained geometry now,
        // otherwise a paused renderer could present one stale trail frame.
        trail_.clear();
        trailParkingPoints_.clear();
    }
}

void Simulation::updateUnityTrailTimeScale(const float timeScale)
{
    const float normalized = std::isfinite(timeScale)
        ? std::clamp(timeScale, 0.0F, maximumTimeScale)
        : timeScale;
    // C# floating-point comparison sends NaN and negative infinity through
    // the low-scale branch; replacing either with a default changes parking.
    if (normalized > trailParkingTimeScaleThreshold)
    {
        if (trailParkingMode_)
        {
            trailParkingMode_ = false;
            initTrailNormalMode();
        }
        return;
    }

    if (!trailParkingMode_)
    {
        trailParkingMode_ = true;
        initTrailParkingMode();
        return;
    }

    if (trailRendererEnabled_)
    {
        stepTrailParkingSequence();
    }
}

void Simulation::setClickTimeScale(const float timeScale) noexcept
{
    if (active_)
    {
        // There is no honest boundary at which to split elapsed source time.
        // Ignore ambiguous live updates instead of rewriting particle history.
        return;
    }
    setClickTimeScale(timeScale, clickTimeSourceAt_);
}

void Simulation::setClickTimeScale(
    const float timeScale,
    const SimulationTime time) noexcept
{
    // Match the Web clock contract: the old multiplier owns all source time
    // through the configuration boundary; only later intervals use the new one.
    accumulateClickTime(time);
    clickTimeScale_ = normalizeTimeScale(timeScale);
}

void Simulation::setTrailTimeScale(const float timeScale) noexcept
{
    if (active_)
    {
        // Live callers must provide the source timestamp that owns the change.
        return;
    }
    setTrailTimeScale(timeScale, trailTimeSourceAt_);
}

void Simulation::setTrailTimeScale(
    const float timeScale,
    const SimulationTime time) noexcept
{
    // Trail vertices and drag particles share this virtual clock. Settling it
    // first prevents an active stroke from jumping when the multiplier changes.
    synchronizeTrailTime(time);
    trailTimeScale_ = normalizeTimeScale(timeScale);
}

void Simulation::setClickParticleSettings(
    const ClickParticleSettings settings) noexcept
{
    if (active_)
    {
        // Motion changes need a source-time boundary to preserve prior turns.
        return;
    }
    clickParticleSettings_ = normalizeClickParticleSettings(settings);
}

void Simulation::setClickParticleSettings(
    const ClickParticleSettings settings,
    const SimulationTime time) noexcept
{
    const ClickParticleSettings normalized = normalizeClickParticleSettings(
        settings);
    const bool motionChanged =
        normalized.ringsLifetimeMs
            != clickParticleSettings_.ringsLifetimeMs
        || normalized.ringsAngularVelocityMultiplier
            != clickParticleSettings_.ringsAngularVelocityMultiplier
        || normalized.ringsRotationDirection
            != clickParticleSettings_.ringsRotationDirection;
    if (motionChanged && active_ && clickEffectEnabled_ && !rings_.empty())
    {
        // The old motion parameters own every click-time interval through this
        // boundary. Only later motion may observe the replacement settings.
        accumulateClickTime(time);
        settleRingRotation(
            particleStepStatesAt(time).dissolveRings.particleAgeSeconds);
    }
    clickParticleSettings_ = normalized;
}

void Simulation::pointerDown(
    const PointF screenPosition,
    const Viewport viewport,
    const SimulationTime time)
{
    reset(screenToWorld(screenPosition, viewport), time);
}

void Simulation::startTrail(
    const PointF screenPosition,
    const Viewport viewport,
    const SimulationTime time)
{
    const PointF worldPosition = screenToWorld(screenPosition, viewport);
    resetState(worldPosition, time);
    appendTrailPoint(worldPosition, trailTime_);
}

void Simulation::pointerMove(
    const PointF screenPosition,
    const Viewport viewport,
    const SimulationTime time)
{
    if (!active_ || !pointerHeld_)
    {
        return;
    }

    // Input can be drained after the compositor has already advanced past its
    // QPC sample. Preserve that sample while preventing pointer-time rollback.
    const SimulationTime sampleTime = std::max(time, pointerSampleAt_);
    synchronizeTrailTime(sampleTime);
    const SimulationTime sampleTrailTime = trailTime_;
    const PointF nextWorld = screenToWorld(screenPosition, viewport);
    if (clickEffectEnabled_ && firstAdvancePending_)
    {
        // TouchEffectCreater reads Input.mousePosition for both CreateEffect
        // and SetDragPosition in one Update. Before Unity's first particle
        // step, only the final transform exists; no travelled segment exists.
        relocatePendingClick(nextWorld, sampleTime, sampleTrailTime);
        return;
    }

    emitAlongDrag(
        pointerWorld_,
        nextWorld,
        pointerTrailSampleAt_,
        sampleTrailTime);
    pointerWorld_ = nextWorld;
    pointerSampleAt_ = sampleTime;
    pointerTrailSampleAt_ = sampleTrailTime;
    appendTrailPoint(pointerWorld_, sampleTrailTime);
}

void Simulation::pointerUp(const SimulationTime time)
{
    if (!active_ || !pointerHeld_)
    {
        return;
    }
    pointerHeld_ = false;
    pointerSampleAt_ = std::max(pointerSampleAt_, time);
    synchronizeTrailTime(pointerSampleAt_);
    pointerTrailSampleAt_ = trailTime_;
    releasedAt_ = pointerSampleAt_;
}

void Simulation::pointerCancel(const SimulationTime time)
{
    if (!active_ || !pointerHeld_)
    {
        return;
    }

    pointerHeld_ = false;
    pointerSampleAt_ = std::max(pointerSampleAt_, time);
    synchronizeTrailTime(pointerSampleAt_);
    pointerTrailSampleAt_ = trailTime_;
    releasedAt_ = pointerSampleAt_;
    dragDistanceRemainderWorld_ = 0.0F;
    // Unity routes Canceled and Ended through the same delayed Stop path, so
    // the existing TrailRenderer geometry must decay instead of disappearing.
}

void Simulation::advance(const SimulationTime time)
{
    if (!active_ || time < lastAdvancedAt_)
    {
        return;
    }

    if (pointerHeld_)
    {
        // Unity observes the emitter Transform on every particle update even
        // when no OS move arrives. Confirm the stationary interval here so a
        // later jump cannot distribute distance emissions across idle time.
        pointerSampleAt_ = std::max(pointerSampleAt_, time);
    }
    firstAdvancePending_ = false;
    accumulateClickTime(time);
    synchronizeTrailTime(time);
    if (pointerHeld_)
    {
        pointerTrailSampleAt_ = trailTime_;
    }
    if (clickEffectEnabled_)
    {
        advanceClickParticleStepStates(
            particleStepStates_,
            pendingClickTime_);
    }
    pendingClickTime_ = SimulationTime::zero();

    lastAdvancedAt_ = time;
    const double effectiveTrailLifetime = trailLifetimeSeconds
        * static_cast<double>(trailLengthMultiplier_);
    if (!trailParkingMode_)
    {
        const auto trailEnd = std::remove_if(
            trail_.begin(),
            trail_.end(),
            [this, effectiveTrailLifetime](const StoredTrailPoint& point)
            {
                return effectiveTrailLifetime <= 0.0
                    || ageSeconds(trailTime_, point.createdAt)
                        >= effectiveTrailLifetime;
            });
        trail_.erase(trailEnd, trail_.end());
    }

    const float clickTriangleAge =
        particleStepStates_.clickTriangles.particleAgeSeconds;
    const bool clickTrianglesEmitted =
        particleStepStates_.clickTriangles.burstEmitted;
    const auto particleEnd = std::remove_if(
        triangles_.begin(),
        triangles_.end(),
        [this, clickTriangleAge, clickTrianglesEmitted](
            const MovingParticle& particle)
        {
            if (particle.dragParticle)
            {
                return ageSeconds(trailTime_, particle.bornAt)
                    > particle.lifetimeSeconds;
            }

            return clickTrianglesEmitted
                && clickTriangleAge > particle.lifetimeSeconds;
        });
    triangles_.erase(particleEnd, triangles_.end());
}

void Simulation::onFrameRendered(const SimulationTime time)
{
    if (!active_ || pointerHeld_)
    {
        return;
    }

    if (ageSeconds(time, releasedAt_) >= releaseLifetimeSeconds)
    {
        // The source converts the one-second root duration to 60 UI frames.
        // Preserve that visual lifetime across desktop refresh rates; cleanup
        // remains post-Present so the boundary frame is still drawable.
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

    const ClickParticleStepStates particleStates = particleStepStatesAt(time);
    const SimulationTime snapshotTrailTime = trailTimeAt(time);
    const ParticleStepState& centerDiskState = particleStates.centerDisk;
    const float diskLifetimeSeconds = millisecondsToSeconds(
        clickParticleSettings_.diskLifetimeMs);
    if (clickEffectEnabled_
        && centerDiskState.burstEmitted
        && centerDiskState.particleAgeSeconds <= diskLifetimeSeconds)
    {
        const float normalizedAge =
            centerDiskState.particleAgeSeconds / diskLifetimeSeconds;
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

    const ParticleStepState& dissolveRingState =
        particleStates.dissolveRings;
    const float ringLifetimeSeconds = millisecondsToSeconds(
        clickParticleSettings_.ringsLifetimeMs);
    if (dissolveRingState.burstEmitted
        && dissolveRingState.particleAgeSeconds <= ringLifetimeSeconds)
    {
        const float normalizedAge = dissolveRingState.particleAgeSeconds
            / ringLifetimeSeconds;
        const float customNormalizedAge = dissolveRingState.customDataAgeSeconds
            / ringLifetimeSeconds;
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
                    + ring.settledRotationRadians
                    + ringRotationDelta(
                        ring.angularBlend,
                        ring.rotationAnchorAgeSeconds,
                        dissolveRingState.particleAgeSeconds,
                        clickParticleSettings_),
                ringColor(normalizedAge),
                5.992157F,
                dissolveThreshold(customNormalizedAge),
                0U,
                4499,
                true});
        }
    }

    for (const MovingParticle& particle : triangles_)
    {
        double age = 0.0;
        if (particle.dragParticle)
        {
            age = ageSeconds(snapshotTrailTime, particle.bornAt);
            if (age <= 0.0)
            {
                continue;
            }
        }
        else
        {
            const ParticleStepState& clickTriangleState =
                particleStates.clickTriangles;
            if (!clickTriangleState.burstEmitted)
            {
                continue;
            }
            age = clickTriangleState.particleAgeSeconds;
        }

        if (age > particle.lifetimeSeconds)
        {
            continue;
        }

        const float normalizedAge = static_cast<float>(age / particle.lifetimeSeconds);
        const PointF worldPosition = add(
            particle.originWorld,
            multiply(particle.velocityWorld, static_cast<float>(age)));
        // Tri2 uses FX_SHADER_Additive_0; its HDR output is included in the
        // game's full-scene Bloom pass after the UI buffer is rendered.
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
            true,
            worldToScreen(particle.globalScalePivotWorld, viewport),
            true});
    }

    const double effectiveTrailLifetime = trailLifetimeSeconds
        * static_cast<double>(trailLengthMultiplier_);
    for (const StoredTrailPoint& point : trail_)
    {
        const float normalizedAge = clampUnit(
            static_cast<float>(
                ageSeconds(snapshotTrailTime, point.createdAt)
                    / effectiveTrailLifetime));
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

bool Simulation::pointerHeld() const noexcept
{
    return pointerHeld_;
}

bool Simulation::firstAdvancePending() const noexcept
{
    return firstAdvancePending_;
}

void Simulation::advanceParticleStepState(
    ParticleStepState& state,
    const SimulationTime elapsed) noexcept
{
    const float elapsedSeconds = std::chrono::duration<float>(elapsed).count();
    if (elapsedSeconds <= 0.0F)
    {
        return;
    }

    const auto stepCount = static_cast<std::uint32_t>(std::ceil(
        elapsedSeconds / maximumParticleTimestepSeconds));
    const float stepSeconds = elapsedSeconds / static_cast<float>(stepCount);
    for (std::uint32_t step = 0U; step < stepCount; ++step)
    {
        if (!state.burstEmitted)
        {
            // Unity's zero-delay Burst is born at the end of the first
            // particle update and therefore starts with age zero.
            state.burstEmitted = true;
            continue;
        }

        // Custom1 is evaluated from the age entering this particle step; the
        // renderer observes it after the particle age itself has advanced.
        state.customDataAgeSeconds = state.particleAgeSeconds;
        state.particleAgeSeconds += stepSeconds;
    }
}

void Simulation::advanceClickParticleStepStates(
    ClickParticleStepStates& states,
    const SimulationTime elapsed) noexcept
{
    advanceParticleStepState(states.centerDisk, elapsed);
    advanceParticleStepState(states.dissolveRings, elapsed);
    advanceParticleStepState(states.clickTriangles, elapsed);
}

Simulation::ClickParticleStepStates Simulation::particleStepStatesAt(
    const SimulationTime time) const noexcept
{
    ClickParticleStepStates states = particleStepStates_;
    SimulationTime elapsed = pendingClickTime_;
    if (time > clickTimeSourceAt_)
    {
        elapsed += scaledDuration(
            time - clickTimeSourceAt_,
            clickTimeScale_);
    }
    if (clickEffectEnabled_ && elapsed > SimulationTime::zero())
    {
        // Snapshot is intentionally read-only. Capture tools query arbitrary
        // future ages, so complete the pending virtual interval on a copy.
        advanceClickParticleStepStates(states, elapsed);
    }
    return states;
}

void Simulation::settleRingRotation(const float particleAgeSeconds) noexcept
{
    for (RingParticle& ring : rings_)
    {
        ring.settledRotationRadians += ringRotationDelta(
            ring.angularBlend,
            ring.rotationAnchorAgeSeconds,
            particleAgeSeconds,
            clickParticleSettings_);
        ring.rotationAnchorAgeSeconds = particleAgeSeconds;
    }
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

SimulationTime Simulation::scaledDuration(
    const SimulationTime duration,
    const float timeScale) noexcept
{
    if (duration <= SimulationTime::zero())
    {
        return SimulationTime::zero();
    }

    return SimulationTime{static_cast<SimulationTime::rep>(
        static_cast<double>(duration.count())
            * static_cast<double>(timeScale))};
}

void Simulation::accumulateClickTime(const SimulationTime time) noexcept
{
    if (time <= clickTimeSourceAt_)
    {
        return;
    }

    pendingClickTime_ += scaledDuration(
        time - clickTimeSourceAt_,
        clickTimeScale_);
    clickTimeSourceAt_ = time;
}

void Simulation::synchronizeTrailTime(const SimulationTime time) noexcept
{
    if (time <= trailTimeSourceAt_)
    {
        return;
    }

    trailTime_ += scaledDuration(
        time - trailTimeSourceAt_,
        trailTimeScale_);
    trailTimeSourceAt_ = time;
}

SimulationTime Simulation::trailTimeAt(const SimulationTime time) const noexcept
{
    if (time <= trailTimeSourceAt_)
    {
        return trailTime_;
    }

    return trailTime_ + scaledDuration(
        time - trailTimeSourceAt_,
        trailTimeScale_);
}

void Simulation::reset(const PointF worldPosition, const SimulationTime time)
{
    resetState(worldPosition, time);
    clickEffectEnabled_ = true;

    const float radiusMinimumWorld = referenceRadiusToStartSizeWorld(
        clickParticleSettings_.ringsRadiusMin);
    const float radiusMaximumWorld = referenceRadiusToStartSizeWorld(
        clickParticleSettings_.ringsRadiusMax);
    for (std::uint32_t index = 0;
         index < clickParticleSettings_.ringsCount;
         ++index)
    {
        rings_.push_back(RingParticle{
            random_.range(radiusMinimumWorld, radiusMaximumWorld),
            random_.range(0.0F, 2.0F * std::numbers::pi_v<float>),
            random_.unit(),
            0.0F,
            0.0F});
    }
    emitClickTriangles(SimulationTime::zero());
    appendTrailPoint(worldPosition, trailTime_);
}

void Simulation::resetState(
    const PointF worldPosition,
    const SimulationTime time)
{
    ++activationCount_;
    const std::uint64_t activationSeed =
        baseSeed_ + activationCount_ * 0x9E3779B97F4A7C15ULL;
    random_ = Random(activationSeed);
    // Unity samples texture-sheet animation per particle. A separate stream
    // keeps frame selection from perturbing already calibrated trajectories.
    atlasRandom_ = Random(activationSeed ^ atlasRandomStream);
    active_ = true;
    pointerHeld_ = true;
    clickEffectEnabled_ = false;
    firstAdvancePending_ = true;
    startedAt_ = time;
    lastAdvancedAt_ = time;
    releasedAt_ = time;
    clickTimeSourceAt_ = time;
    pendingClickTime_ = SimulationTime::zero();
    // A pooled instance is inactive between activations. Keep its virtual
    // trail position, but anchor the next interval at the new source time.
    trailTimeSourceAt_ = time;
    pointerTrailSampleAt_ = trailTime_;
    effectOriginWorld_ = worldPosition;
    pointerWorld_ = worldPosition;
    pointerSampleAt_ = time;
    lastEmissionWorld_ = worldPosition;
    dragDistanceRemainderWorld_ = 0.0F;
    particleStepStates_ = ClickParticleStepStates{};
    rings_.clear();
    triangles_.clear();
    trail_.clear();
    // FXTouch.Stop clears TrailRenderer geometry, but the sibling
    // FxTrailTimeScale state survives pool reuse until its next Update.
    // Preserve the cached suffix, mode and renderer state across activation.
}

void Simulation::relocatePendingClick(
    const PointF worldPosition,
    const SimulationTime time,
    const SimulationTime trailTime)
{
    const PointF offset = subtract(worldPosition, effectOriginWorld_);
    effectOriginWorld_ = worldPosition;
    pointerWorld_ = worldPosition;
    pointerSampleAt_ = time;
    pointerTrailSampleAt_ = trailTime;
    lastEmissionWorld_ = worldPosition;
    dragDistanceRemainderWorld_ = 0.0F;

    // World-space Burst particles have not been emitted yet, so moving the
    // pooled object changes their eventual spawn position without a trail.
    for (MovingParticle& particle : triangles_)
    {
        if (!particle.dragParticle)
        {
            particle.originWorld = add(particle.originWorld, offset);
            particle.globalScalePivotWorld = add(
                particle.globalScalePivotWorld,
                offset);
        }
    }

    if (!trail_.empty())
    {
        trail_.front() = StoredTrailPoint{worldPosition, trailTime};
        trail_.resize(1U);
    }
}

void Simulation::emitClickTriangles(const SimulationTime clickTime)
{
    for (std::uint32_t index = 0; index < 4U; ++index)
    {
        // Unity's Circle arc is Random: burst particles sample independent positions.
        const float angle = random_.range(0.0F, 2.0F * std::numbers::pi_v<float>);
        const PointF radial = direction(angle);
        triangles_.push_back(MovingParticle{
            add(effectOriginWorld_, multiply(radial, clickShapeRadiusWorld)),
            multiply(radial, random_.range(0.3F, 0.4F) * triangleLocalScale),
            clickTime,
            random_.range(0.6F, 0.7F),
            random_.range(0.1F, 0.2F) * triangleLocalScale,
            atlasRandom_.unit() < 0.5F ? 0U : 1U,
            false,
            effectOriginWorld_});
    }
}

void Simulation::emitDragTriangle(
    const PointF worldPosition,
    const SimulationTime trailTime)
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
        trailTime,
        random_.range(0.2F, 0.4F),
        random_.range(0.1F, 0.2F) * triangleLocalScale,
        atlasRandom_.unit() < 0.5F ? 0U : 1U,
        true,
        worldPosition});
}

void Simulation::appendTrailPoint(
    const PointF worldPosition,
    const SimulationTime trailTime)
{
    if (trailLengthMultiplier_ <= 0.0F
        || trailParkingMode_
        || !trailRendererEnabled_)
    {
        return;
    }

    if (trail_.empty())
    {
        trail_.push_back(StoredTrailPoint{worldPosition, trailTime});
        return;
    }

    const double effectiveTrailLifetime = trailLifetimeSeconds
        * static_cast<double>(trailLengthMultiplier_);
    if (ageSeconds(trailTime, trail_.back().createdAt)
        >= effectiveTrailLifetime)
    {
        // An expired anchor must not connect movement across an idle interval.
        trail_.clear();
        trail_.push_back(StoredTrailPoint{worldPosition, trailTime});
        return;
    }

    if (length(subtract(worldPosition, trail_.back().world))
        < trailPointStepWorld)
    {
        return;
    }

    // Unity's TrailRenderer samples the transform once per rendered update.
    // MinVertexDistance filters samples; it does not tessellate a frame jump.
    trail_.push_back(StoredTrailPoint{worldPosition, trailTime});
}

void Simulation::initTrailNormalMode()
{
    trailParkingPoints_.clear();
    trailRendererEnabled_ = true;
}

void Simulation::initTrailParkingMode()
{
    trailParkingPoints_.clear();
    if (trail_.empty())
    {
        finishTrailParkingSequence();
        return;
    }

    if (trail_.size() == 1U)
    {
        // The game leaves a one-point renderer enabled on the transition
        // Update; StepParkingSequence finishes it on the following Update.
        return;
    }

    // The game deliberately skips position zero. The first low-time-scale
    // Update only captures this suffix; visible contraction starts next Update.
    trailParkingPoints_.assign(trail_.begin() + 1, trail_.end());
}

void Simulation::stepTrailParkingSequence()
{
    if (trailParkingPoints_.size() <= 1U)
    {
        finishTrailParkingSequence();
        return;
    }

    // FxTrailTimeScale calls SetPositions before removing the next head, so
    // the final one-point state is never presented.
    trail_ = trailParkingPoints_;
    trailParkingPoints_.erase(trailParkingPoints_.begin());
}

void Simulation::finishTrailParkingSequence()
{
    trailParkingPoints_.clear();
    trail_.clear();
    trailRendererEnabled_ = false;
}

void Simulation::emitAlongDrag(
    const PointF from,
    const PointF to,
    const SimulationTime fromTrailTime,
    const SimulationTime toTrailTime)
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
        emitDragTriangle(
            position,
            interpolateTime(fromTrailTime, toTrailTime, interpolation));
        lastEmissionWorld_ = position;
        dragDistanceRemainderWorld_ = 0.0F;
        distanceUntilEmission = dragEmissionStepWorld;
    }
    dragDistanceRemainderWorld_ += segmentLength - consumed;
}

}
