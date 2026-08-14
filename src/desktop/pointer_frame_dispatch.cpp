#include "pointer_frame_dispatch.hpp"

namespace bafx::desktop
{

void mergePointerFrameTransition(
    PointerFrameButtons& buttons,
    const PointerFrameTransition& transition) noexcept
{
    switch (transition.kind)
    {
    case PointerFrameTransitionKind::Down:
        buttons.down = true;
        if (transition.acceptDown && !buttons.acceptDown)
        {
            // Legacy Input exposes one Down flag. Keep the first native sample
            // that proves this overlay owned any Down in the frame.
            buttons.acceptDown = true;
            buttons.downInputTime = transition.inputTime;
        }
        break;

    case PointerFrameTransitionKind::Up:
        buttons.up = true;
        break;

    case PointerFrameTransitionKind::Cancel:
        buttons.cancel = true;
        break;
    }
}

void applyPointerFrame(
    bafx::fx::SimulationRuntime& runtime,
    const bafx::fx::Viewport viewport,
    const bafx::fx::SimulationTime frameTime,
    const PointerFrameDispatch& dispatch)
{
    if (dispatch.buttons.down)
    {
        if (dispatch.buttons.acceptDown && dispatch.position.has_value())
        {
            runtime.pointerDown(
                dispatch.position->clientPosition,
                viewport,
                frameTime,
                dispatch.buttons.downInputTime);
        }
        else
        {
            // A rejected press partitions ambient movement just like a real
            // press, without fabricating an off-overlay click.
            runtime.endAlwaysOnTrail(frameTime);
        }
    }

    switch (dispatch.positionUse)
    {
    case PointerFramePositionUse::None:
        break;

    case PointerFramePositionUse::Held:
        if (dispatch.buttons.held
            && runtime.pointerHeld()
            && dispatch.position.has_value())
        {
            runtime.pointerMove(
                dispatch.position->clientPosition,
                viewport,
                frameTime,
                dispatch.position->inputTime);
        }
        break;

    case PointerFramePositionUse::Free:
        if (dispatch.position.has_value()
            && dispatch.position->insideClient)
        {
            runtime.pointerMove(
                dispatch.position->clientPosition,
                viewport,
                frameTime,
                dispatch.position->inputTime);
        }
        else
        {
            // Do not clamp a free cursor to an overlay edge; re-entry needs a
            // fresh anchor so it cannot draw a long cross-boundary segment.
            runtime.endAlwaysOnTrail(frameTime);
        }
        break;
    }

    if (dispatch.buttons.up)
    {
        // TouchEffectCreater queries Up after Held. Release must survive a
        // failed coordinate conversion.
        runtime.pointerUp(frameTime);
    }
    if (dispatch.buttons.cancel)
    {
        // Cancel is a native final hard boundary, not a Unity Legacy flag.
        runtime.pointerCancel(frameTime);
    }
}

}
