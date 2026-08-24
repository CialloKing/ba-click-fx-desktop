#pragma once

#include "bafx/fx/simulation_runtime.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>

namespace bafx::desktop
{

enum class DemoScenario
{
    CenterClick,
    InteriorTrail,
    BoundaryTopLeft,
};

[[nodiscard]] constexpr std::optional<DemoScenario> parseDemoScenario(
    const std::wstring_view name) noexcept
{
    if (name == L"center-click")
    {
        return DemoScenario::CenterClick;
    }
    if (name == L"interior-trail")
    {
        return DemoScenario::InteriorTrail;
    }
    if (name == L"boundary-top-left")
    {
        return DemoScenario::BoundaryTopLeft;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::string_view demoScenarioName(
    const DemoScenario scenario) noexcept
{
    switch (scenario)
    {
    case DemoScenario::CenterClick:
        return "center-click";
    case DemoScenario::InteriorTrail:
        return "interior-trail";
    case DemoScenario::BoundaryTopLeft:
        return "boundary-top-left";
    }
    return "unknown";
}

enum class DemoPointerAction
{
    Down,
    StartTrail,
    Move,
    Up,
};

struct DemoPointerEvent
{
    DemoPointerAction action{DemoPointerAction::Down};
    fx::PointF position{};
    fx::SimulationTime time{};
};

struct DemoScenarioPlan
{
    std::array<DemoPointerEvent, 6U> events{};
    std::size_t eventCount{0U};
};

[[nodiscard]] constexpr std::chrono::milliseconds demoScenarioDuration(
    const DemoScenario scenario) noexcept
{
    return scenario == DemoScenario::InteriorTrail
        ? std::chrono::milliseconds(60)
        : std::chrono::milliseconds(0);
}

[[nodiscard]] constexpr fx::PointF demoPoint(
    const fx::Viewport viewport,
    const float horizontalRatio,
    const float verticalRatio) noexcept
{
    return fx::PointF{
        static_cast<float>(viewport.width) * horizontalRatio,
        static_cast<float>(viewport.height) * verticalRatio};
}

[[nodiscard]] constexpr DemoScenarioPlan makeDemoScenarioPlan(
    const DemoScenario scenario,
    const fx::Viewport viewport,
    const fx::SimulationTime startedAt) noexcept
{
    DemoScenarioPlan plan{};
    switch (scenario)
    {
    case DemoScenario::CenterClick:
        plan.events[0U] = DemoPointerEvent{
            DemoPointerAction::Down,
            demoPoint(viewport, 0.5F, 0.5F),
            startedAt};
        plan.eventCount = 1U;
        break;
    case DemoScenario::InteriorTrail:
        // Keep the deterministic stroke well inside every edge so this case
        // measures ROI work instead of the correctness-first boundary guard.
        plan.events[0U] = DemoPointerEvent{
            DemoPointerAction::StartTrail,
            demoPoint(viewport, 0.40F, 0.45F),
            startedAt};
        plan.events[1U] = DemoPointerEvent{
            DemoPointerAction::Move,
            demoPoint(viewport, 0.45F, 0.475F),
            startedAt + std::chrono::milliseconds(12)};
        plan.events[2U] = DemoPointerEvent{
            DemoPointerAction::Move,
            demoPoint(viewport, 0.50F, 0.50F),
            startedAt + std::chrono::milliseconds(24)};
        plan.events[3U] = DemoPointerEvent{
            DemoPointerAction::Move,
            demoPoint(viewport, 0.55F, 0.525F),
            startedAt + std::chrono::milliseconds(36)};
        plan.events[4U] = DemoPointerEvent{
            DemoPointerAction::Move,
            demoPoint(viewport, 0.60F, 0.55F),
            startedAt + std::chrono::milliseconds(48)};
        plan.events[5U] = DemoPointerEvent{
            DemoPointerAction::Up,
            demoPoint(viewport, 0.60F, 0.55F),
            startedAt + demoScenarioDuration(scenario)};
        plan.eventCount = plan.events.size();
        break;
    case DemoScenario::BoundaryTopLeft:
        plan.events[0U] = DemoPointerEvent{
            DemoPointerAction::Down,
            fx::PointF{0.0F, 0.0F},
            startedAt};
        plan.eventCount = 1U;
        break;
    }
    return plan;
}

inline void executeDemoScenarioPlan(
    fx::SimulationRuntime& simulation,
    const fx::Viewport viewport,
    const DemoScenarioPlan& plan)
{
    for (std::size_t index = 0U; index < plan.eventCount; ++index)
    {
        const DemoPointerEvent& event = plan.events[index];
        switch (event.action)
        {
        case DemoPointerAction::Down:
            simulation.pointerDown(event.position, viewport, event.time);
            break;
        case DemoPointerAction::StartTrail:
            // continuePointerStroke delegates to Simulation::startTrail and
            // intentionally avoids synthesizing a click burst or snapshot.
            simulation.continuePointerStroke(
                event.position,
                viewport,
                event.time,
                event.time);
            break;
        case DemoPointerAction::Move:
            simulation.pointerMove(
                event.position,
                viewport,
                event.time,
                event.time);
            break;
        case DemoPointerAction::Up:
            simulation.pointerUp(event.time);
            break;
        }
    }
}

}
