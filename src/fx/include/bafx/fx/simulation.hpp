#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

namespace bafx::fx
{

using SimulationTime = std::chrono::nanoseconds;

struct PointF
{
    float x{0.0F};
    float y{0.0F};
};

struct ColorF
{
    float r{0.0F};
    float g{0.0F};
    float b{0.0F};
    float a{0.0F};
};

struct Viewport
{
    std::uint32_t width{0};
    std::uint32_t height{0};
};

enum class SpriteKind : std::uint8_t
{
    CenterDisk,
    DissolveRing,
    Triangle
};

struct Sprite
{
    SpriteKind kind{SpriteKind::CenterDisk};
    PointF centerPixels{};
    float sizePixels{0.0F};
    float rotationRadians{0.0F};
    ColorF color{};
    float artisticIntensity{1.0F};
    float dissolveThreshold{0.0F};
    std::uint32_t atlasFrame{0};
    std::int32_t renderQueue{0};
    bool contributesBloom{false};
    PointF globalScalePivotPixels{};
    bool scaleCenterWithGlobalScale{false};
};

struct TrailPoint
{
    PointF positionPixels{};
    float normalizedAge{0.0F};
};

struct TrailStroke
{
    std::vector<TrailPoint> points{};
    float widthPixels{0.0F};
    float opacity{1.0F};
};

struct FrameSnapshot
{
    std::vector<Sprite> sprites{};
    std::vector<TrailPoint> trail{};
    float trailWidthPixels{0.0F};
    float trailOpacity{1.0F};
    std::vector<TrailStroke> trailStrokes{};
    // Host opacity is evaluated after the Unity material. Keeping it separate
    // prevents opacity changes from moving Dissolve clip boundaries.
    float globalOpacity{1.0F};
    bool active{false};
    bool pointerHeld{false};

    [[nodiscard]] bool hasDrawableContent() const noexcept
    {
        if (!sprites.empty())
        {
            return true;
        }
        if (trailWidthPixels > 0.0F && trail.size() >= 2U)
        {
            return true;
        }
        for (const TrailStroke& stroke : trailStrokes)
        {
            if (stroke.widthPixels > 0.0F && stroke.points.size() >= 2U)
            {
                return true;
            }
        }
        return false;
    }
};

// Public values use the native 1080p reference-pixel contract. The extracted
// Unity defaults remain the identity configuration, while the simulation
// converts reference pixels to world units only when a ring particle is born.
struct ClickParticleSettings
{
    float diskLifetimeMs{200.0F};
    std::uint32_t ringsCount{2U};
    float ringsLifetimeMs{600.0F};
    float ringsRadiusMin{68.92571232F};
    float ringsRadiusMax{80.41333104F};
    float ringsAngularVelocityMultiplier{11.170107F};
    float ringsRotationDirection{-1.0F};
};

// Values use the native 1080p reference-pixel contract. Each click shard locks
// these spawn inputs when it is created, so later changes cannot rewrite an
// existing particle's trajectory or lifetime.
struct ShardParticleSettings
{
    std::uint32_t clickCount{4U};
    float clickLifetimeMinMs{600.0F};
    float clickLifetimeMaxMs{700.0F};
    float clickRadius{49.8769488F};
    float clickSpeedMin{49.8769488F};
    float clickSpeedMax{66.5025984F};
    float sizeMin{16.6256496F};
    float sizeMax{33.2512992F};
};

// Moving particles must scale around their own emission pivot; changing only
// their quad size detaches click shards from the disk and ring.
void applyGlobalScale(FrameSnapshot& snapshot, float scale) noexcept;

class Simulation final
{
public:
    explicit Simulation(std::uint64_t seed = 20260716U);

    void pointerDown(PointF screenPosition, Viewport viewport, SimulationTime time);
    // Starts a movement-only instance. It deliberately omits the click burst
    // so an always-on trail never fabricates a mouse click.
    void startTrail(PointF screenPosition, Viewport viewport, SimulationTime time);
    void pointerMove(PointF screenPosition, Viewport viewport, SimulationTime time);
    void pointerUp(SimulationTime time);
    void pointerCancel(SimulationTime time);
    void advance(SimulationTime time);
    void onFrameRendered(SimulationTime time);

    // Runtime pool reuse retains Unity component state while assigning a new
    // deterministic native random stream to this activation.
    void preparePooledActivation(std::uint64_t seed) noexcept;

    // Mirrors FxTrailTimeScale.Update. This is an explicit game-time-scale
    // input and is intentionally separate from the desktop pause timeline.
    void updateUnityTrailTimeScale(float timeScale);

    // Animation controls scale particle/trail age, not the host clock, so
    // input timestamps and pause semantics stay intact.
    // Active simulations use the timestamped overload to settle the preceding
    // source-time interval before the new multiplier becomes effective. The
    // overload without a timestamp is intentionally initialization-only.
    void setClickTimeScale(float timeScale) noexcept;
    void setClickTimeScale(float timeScale, SimulationTime time) noexcept;
    void setTrailTimeScale(float timeScale) noexcept;
    void setTrailTimeScale(float timeScale, SimulationTime time) noexcept;

    // Count and radius bounds are spawn-time inputs. Disk/ring lifetime and
    // angular motion affect live particles. A configured visible child may
    // extend retention beyond Unity's one-second post-release floor, within the
    // validated lifetime/time-scale ceiling. The timestamped overload settles
    // rotation at the old parameters before installing a new motion segment.
    void setClickParticleSettings(ClickParticleSettings settings) noexcept;
    void setClickParticleSettings(
        ClickParticleSettings settings,
        SimulationTime time) noexcept;
    void setShardParticleSettings(ShardParticleSettings settings) noexcept;

    // Product settings may change during an active stroke. Retain the
    // existing points and apply the new lifetime on the next simulation step.
    void setTrailLengthMultiplier(float multiplier) noexcept;

    [[nodiscard]] FrameSnapshot snapshot(Viewport viewport, SimulationTime time) const;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool pointerHeld() const noexcept;
    [[nodiscard]] bool firstAdvancePending() const noexcept;

private:
    struct MovingParticle
    {
        PointF originWorld{};
        PointF velocityWorld{};
        SimulationTime bornAt{};
        float lifetimeSeconds{0.0F};
        float startSizeWorld{0.0F};
        std::uint32_t atlasFrame{0};
        bool dragParticle{false};
        PointF globalScalePivotWorld{};
    };

    struct RingParticle
    {
        float startSizeWorld{0.0F};
        float initialRotationRadians{0.0F};
        float angularBlend{0.0F};
        float settledRotationRadians{0.0F};
        float rotationAnchorAgeSeconds{0.0F};
    };

    struct StoredTrailPoint
    {
        PointF world{};
        SimulationTime createdAt{};
    };

    struct ParticleStepState
    {
        float particleAgeSeconds{0.0F};
        float customDataAgeSeconds{0.0F};
        bool burstEmitted{false};
    };

    struct ClickParticleStepStates
    {
        // Unity advances every child ParticleSystem independently, even when
        // their zero-delay bursts are observed in the same rendered frame.
        ParticleStepState centerDisk{};
        ParticleStepState dissolveRings{};
        ParticleStepState clickTriangles{};
    };

    class Random final
    {
    public:
        explicit Random(std::uint64_t seed) noexcept;
        [[nodiscard]] float unit() noexcept;
        [[nodiscard]] float range(float minimum, float maximum) noexcept;

    private:
        std::uint64_t state_;
    };

    [[nodiscard]] static PointF screenToWorld(PointF screen, Viewport viewport) noexcept;
    [[nodiscard]] static PointF worldToScreen(PointF world, Viewport viewport) noexcept;
    [[nodiscard]] static float worldToPixels(Viewport viewport) noexcept;
    [[nodiscard]] static double ageSeconds(SimulationTime now, SimulationTime then) noexcept;
    [[nodiscard]] static SimulationTime scaledDuration(
        SimulationTime duration,
        float timeScale) noexcept;

    void reset(PointF worldPosition, SimulationTime time);
    void resetState(PointF worldPosition, SimulationTime time);
    void relocatePendingClick(
        PointF worldPosition,
        SimulationTime time,
        SimulationTime trailTime);
    void emitClickTriangles(SimulationTime clickTime);
    void emitDragTriangle(PointF worldPosition, SimulationTime trailTime);
    void appendTrailPoint(PointF worldPosition, SimulationTime trailTime);
    void initTrailNormalMode();
    void initTrailParkingMode();
    void stepTrailParkingSequence();
    void finishTrailParkingSequence();
    void emitAlongDrag(
        PointF from,
        PointF to,
        SimulationTime fromTrailTime,
        SimulationTime toTrailTime);
    void accumulateClickTime(SimulationTime time) noexcept;
    void synchronizeTrailTime(SimulationTime time) noexcept;
    [[nodiscard]] SimulationTime trailTimeAt(SimulationTime time) const noexcept;
    static void advanceParticleStepState(
        ParticleStepState& state,
        SimulationTime elapsed) noexcept;
    static void advanceClickParticleStepStates(
        ClickParticleStepStates& states,
        SimulationTime elapsed) noexcept;
    [[nodiscard]] ClickParticleStepStates particleStepStatesAt(
        SimulationTime time) const noexcept;
    [[nodiscard]] bool hasVisibleSystemsAfterFrame(
        SimulationTime time) const noexcept;
    void settleRingRotation(float particleAgeSeconds) noexcept;

    std::uint64_t baseSeed_{0};
    std::uint64_t activationCount_{0};
    Random random_;
    Random atlasRandom_;
    bool active_{false};
    bool pointerHeld_{false};
    bool clickEffectEnabled_{false};
    bool firstAdvancePending_{false};
    SimulationTime startedAt_{};
    SimulationTime lastAdvancedAt_{};
    SimulationTime releasedAt_{};
    SimulationTime clickTimeSourceAt_{};
    SimulationTime pendingClickTime_{};
    SimulationTime trailTimeSourceAt_{};
    SimulationTime trailTime_{};
    SimulationTime pointerTrailSampleAt_{};
    PointF effectOriginWorld_{};
    PointF pointerWorld_{};
    SimulationTime pointerSampleAt_{};
    PointF lastEmissionWorld_{};
    float dragDistanceRemainderWorld_{0.0F};
    float trailLengthMultiplier_{1.0F};
    float clickTimeScale_{1.0F};
    float trailTimeScale_{1.0F};
    ClickParticleSettings clickParticleSettings_{};
    ShardParticleSettings shardParticleSettings_{};
    bool trailParkingMode_{false};
    bool trailRendererEnabled_{true};
    ClickParticleStepStates particleStepStates_{};
    std::vector<RingParticle> rings_{};
    std::vector<MovingParticle> triangles_{};
    std::vector<StoredTrailPoint> trail_{};
    std::vector<StoredTrailPoint> trailParkingPoints_{};
};

}
