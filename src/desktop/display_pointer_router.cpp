#include "display_pointer_router.hpp"

#include <windows.h>

#include <algorithm>

namespace bafx::desktop
{

bafx::fx::SimulationTime PointerTimestampSource::map(
    const std::int64_t counter,
    const bafx::fx::SimulationTime fallback) const noexcept
{
    return mapper != nullptr && counter > 0
        ? mapper(context, counter)
        : fallback;
}

DisplayPointerRouteResult DisplayPointerRouter::consumeFrame(
    DisplaySessionManager& sessions,
    const bafx::fx::SimulationTime frameTime,
    const std::span<const bafx::windows::PointerEvent> events,
    const PointerTimestampSource timestamps)
{
    using bafx::windows::PointerEvent;
    using bafx::windows::PointerEventKind;

    const bafx::windows::PointerFrameSnapshot frame =
        frameAdapter_.consume(events);
    DisplayPointerRouteResult result{};
    result.acceptedDowns.reserve(frame.edges.size());

    bool down = false;
    bool up = false;
    bool cancel = false;
    DisplaySession* acceptedDownSession = nullptr;
    std::optional<PointerEvent> acceptedDown{};
    for (const bafx::windows::PointerFrameEdge& edge : frame.edges)
    {
        switch (edge.kind)
        {
        case PointerEventKind::LeftButtonDown:
        {
            down = true;
            DisplaySession* const candidate = sessions.findAtPoint(
                edge.trigger.screenPosition);
            if (candidate != nullptr)
            {
                result.acceptedDowns.push_back(edge.trigger);
                if (acceptedDownSession == nullptr)
                {
                    acceptedDownSession = candidate;
                    acceptedDown = edge.trigger;
                }
            }
            break;
        }

        case PointerEventKind::LeftButtonUp:
            up = true;
            break;

        case PointerEventKind::Cancel:
            cancel = true;
            break;

        case PointerEventKind::Move:
            break;
        }
    }

    if (down)
    {
        // One physical press partitions every ambient trail, including a
        // different monitor's last free-move anchor.
        endAmbientTrails(sessions, frameTime);
        ambientTarget_.reset();
    }

    POINT finalScreenPosition{};
    bool finalPositionAvailable = GetCursorPos(&finalScreenPosition) != FALSE;
    if (!finalPositionAvailable && frame.latestNonCancelSample.has_value())
    {
        finalScreenPosition = frame.latestNonCancelSample->screenPosition;
        finalPositionAvailable = true;
    }
    const PointerEvent* finalInputSample = nullptr;
    if (frame.latestMoveSample.has_value())
    {
        finalInputSample = &*frame.latestMoveSample;
    }
    else if (frame.latestNonCancelSample.has_value())
    {
        finalInputSample = &*frame.latestNonCancelSample;
    }
    const bafx::fx::SimulationTime finalInputTime = finalInputSample != nullptr
        ? timestamps.map(finalInputSample->qpcTimestamp, frameTime)
        : frameTime;

    DisplaySession* pressedSession = resolveSession(sessions, pressedTarget_);
    if (pressedSession == nullptr)
    {
        pressedTarget_.reset();
    }

    if (down
        && pressedSession == nullptr
        && acceptedDownSession != nullptr
        && acceptedDown.has_value())
    {
        const POINT downPosition = finalPositionAvailable
            ? finalScreenPosition
            : acceptedDown->screenPosition;
        const std::optional<SessionPosition> mapped = mapPosition(
            *acceptedDownSession,
            downPosition);
        if (mapped.has_value())
        {
            acceptedDownSession->simulation().pointerDown(
                mapped->client,
                mapped->viewport,
                frameTime,
                timestamps.map(acceptedDown->qpcTimestamp, frameTime));
            pressedTarget_ = acceptedDownSession->target();
            pressedSession = acceptedDownSession;
        }
    }

    const bool heldPositionRequired = frame.heldAfter
        && (frame.hasFinalHeldMove || acceptedDown.has_value());
    if (heldPositionRequired && finalPositionAvailable)
    {
        DisplaySession* const finalSession = sessions.findAtPoint(
            finalScreenPosition);
        if (pressedSession != nullptr
            && finalSession != nullptr
            && sameDisplaySource(
                pressedSession->target(),
                finalSession->target()))
        {
            const std::optional<SessionPosition> mapped = mapPosition(
                *pressedSession,
                finalScreenPosition);
            if (mapped.has_value())
            {
                pressedSession->simulation().pointerMove(
                    mapped->client,
                    mapped->viewport,
                    frameTime,
                    finalInputTime);
            }
        }
        else if (pressedSession != nullptr)
        {
            pressedSession->simulation().pointerUp(frameTime);
            pressedTarget_.reset();
            pressedSession = nullptr;
            if (finalSession != nullptr)
            {
                const std::optional<SessionPosition> mapped = mapPosition(
                    *finalSession,
                    finalScreenPosition);
                if (mapped.has_value())
                {
                    finalSession->simulation().continuePointerStroke(
                        mapped->client,
                        mapped->viewport,
                        frameTime,
                        finalInputTime);
                    pressedTarget_ = finalSession->target();
                    pressedSession = finalSession;
                    ++result.displayHandoffs;
                }
            }
        }
    }

    if (!frame.heldAfter
        && frame.edges.empty()
        && frame.hasFinalFreeMove
        && finalPositionAvailable)
    {
        DisplaySession* ambientSession = resolveSession(
            sessions,
            ambientTarget_);
        DisplaySession* const finalSession = sessions.findAtPoint(
            finalScreenPosition);
        if (ambientSession != nullptr
            && (finalSession == nullptr
                || !sameDisplaySource(
                    ambientSession->target(),
                    finalSession->target())))
        {
            ambientSession->simulation().endAlwaysOnTrail(frameTime);
            ambientSession = nullptr;
            ambientTarget_.reset();
        }
        if (finalSession != nullptr)
        {
            const std::optional<SessionPosition> mapped = mapPosition(
                *finalSession,
                finalScreenPosition);
            if (mapped.has_value() && mapped->inside)
            {
                finalSession->simulation().pointerMove(
                    mapped->client,
                    mapped->viewport,
                    frameTime,
                    finalInputTime);
                ambientTarget_ = finalSession->target();
            }
        }
    }

    if (up && pressedSession != nullptr)
    {
        pressedSession->simulation().pointerUp(frameTime);
        pressedTarget_.reset();
        pressedSession = nullptr;
    }
    if (cancel)
    {
        cancelAll(sessions, frameTime);
        pressedSession = nullptr;
    }

    result.pressedSessionActive = pressedSession != nullptr;
    return result;
}

void DisplayPointerRouter::discardFrame(
    const std::span<const bafx::windows::PointerEvent> events)
{
    static_cast<void>(frameAdapter_.consume(events));
}

void DisplayPointerRouter::cancelAll(
    DisplaySessionManager& sessions,
    const bafx::fx::SimulationTime frameTime)
{
    for (const auto& session : sessions.sessions())
    {
        session->simulation().pointerCancel(frameTime);
    }
    pressedTarget_.reset();
    ambientTarget_.reset();
}

std::optional<DisplayPointerRouter::SessionPosition>
DisplayPointerRouter::mapPosition(
    DisplaySession& session,
    const POINT screenPosition) noexcept
{
    POINT clientPosition = screenPosition;
    if (ScreenToClient(session.window().handle(), &clientPosition) == FALSE)
    {
        return std::nullopt;
    }

    const bafx::windows::WindowSize size = session.window().size();
    if (size.width == 0U || size.height == 0U)
    {
        return std::nullopt;
    }
    const bool inside = clientPosition.x >= 0
        && clientPosition.y >= 0
        && static_cast<std::uint32_t>(clientPosition.x) < size.width
        && static_cast<std::uint32_t>(clientPosition.y) < size.height;
    const LONG maximumX = static_cast<LONG>(size.width - 1U);
    const LONG maximumY = static_cast<LONG>(size.height - 1U);
    clientPosition.x = std::clamp(clientPosition.x, 0L, maximumX);
    clientPosition.y = std::clamp(clientPosition.y, 0L, maximumY);
    return SessionPosition{
        bafx::fx::PointF{
            static_cast<float>(clientPosition.x),
            static_cast<float>(clientPosition.y)},
        bafx::fx::Viewport{size.width, size.height},
        inside};
}

DisplaySession* DisplayPointerRouter::resolveSession(
    DisplaySessionManager& sessions,
    const std::optional<DisplayTarget>& target) noexcept
{
    return target.has_value()
        ? sessions.findBySource(*target)
        : nullptr;
}

void DisplayPointerRouter::endAmbientTrails(
    DisplaySessionManager& sessions,
    const bafx::fx::SimulationTime frameTime)
{
    for (const auto& session : sessions.sessions())
    {
        session->simulation().endAlwaysOnTrail(frameTime);
    }
}

}
