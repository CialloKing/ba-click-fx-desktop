#pragma once

#include "bafx/windows/runtime_diagnostics.hpp"

namespace bafx::desktop
{

class DisplaySession;
class DisplaySessionManager;

// Observe every session on the render-owner thread at one timestamp. The caller
// publishes this same value to diagnostics and IPC after collection completes.
[[nodiscard]] bafx::windows::DisplayRuntimeSummary collectDisplayRuntimeSummary(
    const DisplaySessionManager& displaySessions,
    const DisplaySession& displaySession,
    bafx::windows::DisplayTopologyStatus latestDisplayTopologyStatus,
    LONG latestDisplayTopologyError,
    bafx::core::MonotonicTime runtimeObservedAt);

}
