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
};

struct FrameSnapshot
{
    std::vector<Sprite> sprites{};
    std::vector<TrailPoint> trail{};
    float trailWidthPixels{0.0F};
    std::vector<TrailStroke> trailStrokes{};
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

class Simulation final
{
public:
    explicit Simulation(std::uint64_t seed = 20260716U);

    void pointerDown(PointF screenPosition, Viewport viewport, SimulationTime time);
    void pointerMove(PointF screenPosition, Viewport viewport, SimulationTime time);
    void pointerUp(SimulationTime time);
    void pointerCancel(SimulationTime time);
    void advance(SimulationTime time);
    void onFrameRendered();

    // Product settings may change during an active stroke. Retain the
    // existing points and apply the new lifetime on the next simulation step.
    void setTrailLengthMultiplier(float multiplier) noexcept;

    [[nodiscard]] FrameSnapshot snapshot(Viewport viewport, SimulationTime time) const;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool pointerHeld() const noexcept;

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
    };

    struct RingParticle
    {
        float startSizeWorld{0.0F};
        float initialRotationRadians{0.0F};
        float angularBlend{0.0F};
    };

    struct StoredTrailPoint
    {
        PointF world{};
        SimulationTime createdAt{};
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

    void reset(PointF worldPosition, SimulationTime time);
    void emitClickTriangles(SimulationTime time);
    void emitDragTriangle(PointF worldPosition, SimulationTime time);
    void appendTrailPoint(PointF worldPosition, SimulationTime time);
    void emitAlongDrag(
        PointF from,
        PointF to,
        SimulationTime fromTime,
        SimulationTime toTime);

    std::uint64_t baseSeed_{0};
    std::uint64_t activationCount_{0};
    Random random_;
    Random atlasRandom_;
    bool active_{false};
    bool pointerHeld_{false};
    SimulationTime startedAt_{};
    SimulationTime lastAdvancedAt_{};
    std::uint32_t releasedFrames_{0};
    PointF effectOriginWorld_{};
    PointF pointerWorld_{};
    SimulationTime pointerSampleAt_{};
    PointF lastEmissionWorld_{};
    float dragDistanceRemainderWorld_{0.0F};
    float trailLengthMultiplier_{1.0F};
    std::vector<RingParticle> rings_{};
    std::vector<MovingParticle> triangles_{};
    std::vector<StoredTrailPoint> trail_{};
};

}
