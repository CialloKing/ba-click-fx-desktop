#include "control_center_window.hpp"
#include "control_updates.hpp"
#include "host_snapshot_poller.hpp"

namespace bafx::control_center
{

void ControlCenterWindow::invalidateHostRefresh() noexcept
{
    hostSnapshotCurrent_ = false;
    displayStateCurrent_ = false;
    if (window_ != nullptr)
    {
        KillTimer(window_, hostRefreshTimerId);
    }
    if (hostSnapshotPoller_ != nullptr)
    {
        hostSnapshotPoller_->invalidate();
    }
    invalidateDisplayStateRefresh();
}

bool ControlCenterWindow::requireCurrentSnapshot()
{
    if (connected_ && hostSnapshotCurrent_)
    {
        return true;
    }
    setInfo(connected_ ? TextId::HostRefreshing : TextId::HostDisconnected,
        connected_ ? TextId::HostRefreshingHint : TextId::StartHostAndRefresh);
    return false;
}

void ControlCenterWindow::requestHostRefresh()
{
    if (hostShutdownPending_ || window_ == nullptr)
    {
        return;
    }
    hostVersionBlocked_ = false;
    const bool mutexPresent = hostMutexPresent();
    if (!mutexPresent)
    {
        hostRunning_ = hostStartPending_;
        setConnected(false);
        setText(hostVersionText_, hostStartPending_
            ? TextId::HostVersionStarting : TextId::HostVersionStopped);
        if (!hostStartPending_)
        {
            setText(statusText_, TextId::HostNotRunning);
            setInfo(TextId::HostNotRunning, TextId::StartHostHint);
        }
        if (std::exchange(trayMenuPending_, false))
        {
            openTrayMenu();
        }
        return;
    }
    hostRunning_ = true;
    hostSnapshotCurrent_ = false;
    displayStateCurrent_ = false;
    invalidateDisplayStateRefresh();
    // These operations derive a compound mutation from a complete snapshot.
    // Keep independent edits, navigation and language changes available.
    for (const HWND control : {applyFxProfileButton_, saveFxProfileButton_,
        deleteFxProfileButton_, displayIndependent_, displayEffectsEnabled_,
        displayHdrEnabled_, displayFramePacingLabel_, displayFramePacing_,
        hotkeySave_, resetDefaultsButton_})
    {
        setControlEnabled(control, FALSE);
    }
    try
    {
        if (hostSnapshotPoller_ == nullptr)
        {
            hostSnapshotPoller_ = std::make_unique<HostSnapshotPoller>(controlCenterIpcOptions());
        }
        if (hostSnapshotPoller_->request(generation_))
        {
            refreshInfoRevision_ = infoRevision_;
        }
        if (SetTimer(window_, hostRefreshTimerId, 50U, nullptr) == 0U)
        {
            invalidateHostRefresh();
            setError(TextId::HostConfigReadFailed);
        }
    }
    catch (...)
    {
        invalidateHostRefresh();
        setError(TextId::HostConfigReadFailed);
    }
}

void ControlCenterWindow::pollHostRefresh()
{
    if (hostSnapshotPoller_ == nullptr)
    {
        return;
    }
    if (hostShutdownPending_)
    {
        invalidateHostRefresh();
        return;
    }
    if (auto result = hostSnapshotPoller_->takeResult(); result.has_value())
    {
        if (result->generation != generation_)
        {
            // Hotkey confirmation may advance the UI generation during a read.
            // An older coherent snapshot must not roll that confirmation back.
            invalidateHostRefresh();
            requestHostRefresh();
            return;
        }
        acceptHostSnapshot(std::move(*result));
        if (std::exchange(trayMenuPending_, false))
        {
            openTrayMenu();
        }
    }
    if (!hostSnapshotPoller_->busy())
    {
        KillTimer(window_, hostRefreshTimerId);
    }
}

void ControlCenterWindow::acceptHostSnapshot(HostSnapshotResult result)
{
    if (result.state.has_value())
    {
        updateHostVersionText(*result.state);
    }
    if (result.status == HostSnapshotStatus::Succeeded)
    {
        const bool preserveInfo = infoRevision_ != refreshInfoRevision_;
        updateControls(*result.state, result.config, preserveInfo);
        KillTimer(window_, hostRetryTimerId);
        hostRetryAttempts_ = 0U;
        hostStartPending_ = false;
        updateHostLifecycleButton();
        return;
    }
    setConnected(false);
    switch (result.status)
    {
    case HostSnapshotStatus::StateReadFailed:
        setText(hostVersionText_, TextId::HostVersionUnreadable);
        if (!hostStartPending_)
        {
            setText(statusText_, TextId::HostServiceUnavailable);
            setInfo(TextId::HostNotReady, TextId::HostNotReadyHint);
        }
        break;
    case HostSnapshotStatus::StateInvalid:
        setText(hostVersionText_, TextId::HostVersionInvalidState);
        setText(statusText_, TextId::HostInvalidState);
        setError(utf8ToWide(result.error));
        break;
    case HostSnapshotStatus::Incompatible:
        rejectIncompatibleHostVersion(*result.state);
        KillTimer(window_, hostRetryTimerId);
        hostRetryAttempts_ = 0U;
        break;
    case HostSnapshotStatus::ConfigReadFailed:
        setText(statusText_, TextId::HostConfigReadFailed);
        setError(describeResponse(result.response));
        break;
    case HostSnapshotStatus::ConfigInvalid:
        setText(statusText_, TextId::HostInvalidConfig);
        setError(utf8ToWide(result.error));
        break;
    case HostSnapshotStatus::RecheckReadFailed:
    case HostSnapshotStatus::RecheckInvalid:
        setText(hostVersionText_, result.status == HostSnapshotStatus::RecheckInvalid
            ? TextId::HostVersionInvalidRecheck : TextId::HostVersionRecheckFailed);
        setText(statusText_, TextId::HostStateRecheckFailed);
        setError(result.status == HostSnapshotStatus::RecheckInvalid
            ? UiMessage(utf8ToWide(result.error)) : describeResponse(result.response));
        break;
    case HostSnapshotStatus::GenerationChanged:
        setInfo(TextId::HostStateChanging, TextId::HostStateChangingHint);
        break;
    case HostSnapshotStatus::Succeeded:
        break;
    }
}

void ControlCenterWindow::confirmHostMutation()
{
    // Mutation replies do not all include a generation. Confirm only that small
    // state here so consecutive writes retain their optimistic concurrency
    // guard. Full configuration I/O and parsing belong to the background reader.
    const auto response = client_.transact("GetState");
    const auto state = response.succeeded()
        ? parseHostState(response.payload) : HostStateParseResult{};
    if (!state.succeeded())
    {
        setConnected(false);
        return;
    }
    if (!state.state->settingsCompatible())
    {
        rejectIncompatibleHostVersion(*state.state);
        return;
    }
    generation_ = state.state->generation;
    paused_ = state.state->paused;
}

}
