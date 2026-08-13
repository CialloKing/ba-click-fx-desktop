#include "pointer_frame_dispatch.hpp"

namespace bafx::desktop
{

void applyPointerFrame(
    bafx::fx::SimulationRuntime& runtime,
    const bafx::fx::Viewport viewport,
    const bafx::fx::SimulationTime frameTime,
    const PointerFrameDispatch& dispatch)
{
    for (const PointerFrameTransition& transition : dispatch.transitions)
    {
        switch (transition.kind)
        {
        case PointerFrameTransitionKind::Down:
            if (transition.acceptDown && dispatch.position.has_value())
            {
                runtime.pointerDown(
                    dispatch.position->clientPosition,
                    viewport,
                    frameTime,
                    transition.inputTime);
            }
            else
            {
                // A rejected press partitions ambient movement just like a
                // real press, without fabricating an off-overlay click.
                runtime.endAlwaysOnTrail(frameTime);
            }
            break;

        case PointerFrameTransitionKind::Up:
            // Release must survive a failed coordinate conversion.
            runtime.pointerUp(frameTime);
            break;

        case PointerFrameTransitionKind::Cancel:
            // Cancellation also retires any ambient stroke in the runtime.
            runtime.pointerCancel(frameTime);
            break;
        }
    }

    switch (dispatch.positionUse)
    {
    case PointerFramePositionUse::None:
        break;

    case PointerFramePositionUse::Held:
        if (runtime.pointerHeld() && dispatch.position.has_value())
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
}

}
