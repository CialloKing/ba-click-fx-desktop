#pragma once

#include "display_session_manager.hpp"

#include "bafx/fx/simulation.hpp"
#include "bafx/windows/overlay_window.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bafx::desktop
{

using PointerTimestampMapper = bafx::fx::SimulationTime (*)(
    const void* context,
    std::int64_t counter) noexcept;

struct PointerTimestampSource final
{
    const void* context{nullptr};
    PointerTimestampMapper mapper{nullptr};

    [[nodiscard]] bafx::fx::SimulationTime map(
        std::int64_t counter,
        bafx::fx::SimulationTime fallback) const noexcept;
};

struct DisplayPointerRouteResult final
{
    std::vector<bafx::windows::PointerEvent> acceptedDowns{};
    std::size_t displayHandoffs{0U};
    bool pressedSessionActive{false};
};

class DisplayPointerRouter final
{
public:
    [[nodiscard]] DisplayPointerRouteResult consumeFrame(
        DisplaySessionManager& sessions,
        bafx::fx::SimulationTime frameTime,
        std::span<const bafx::windows::PointerEvent> events,
        PointerTimestampSource timestamps);

    void discardFrame(
        std::span<const bafx::windows::PointerEvent> events);
    void cancelAll(
        DisplaySessionManager& sessions,
        bafx::fx::SimulationTime frameTime);

private:
    struct SessionPosition final
    {
        bafx::fx::PointF client{};
        bafx::fx::Viewport viewport{};
        bool inside{false};
    };

    [[nodiscard]] static std::optional<SessionPosition> mapPosition(
        DisplaySession& session,
        POINT screenPosition) noexcept;
    [[nodiscard]] static DisplaySession* resolveSession(
        DisplaySessionManager& sessions,
        const std::optional<DisplayTarget>& target) noexcept;
    static void endAmbientTrails(
        DisplaySessionManager& sessions,
        bafx::fx::SimulationTime frameTime);

    bafx::windows::PointerFrameAdapter frameAdapter_{};
    std::optional<DisplayTarget> pressedTarget_{};
    std::optional<DisplayTarget> ambientTarget_{};
};

}
