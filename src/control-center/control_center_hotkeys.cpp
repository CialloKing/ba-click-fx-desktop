#include "control_center_window.hpp"

#include <algorithm>

namespace bafx::control_center
{
namespace
{
constexpr int recordFirst = 1000;
constexpr int saveId = 1010;
constexpr int revertId = 1011;
constexpr int retryId = 1012;
constexpr int cancelId = 1013;
constexpr std::array actionLabels{TextId::HotkeyPause, TextId::HotkeyTrail, TextId::HotkeyNextProfile, TextId::HotkeyExit};

bool modifierKey(const WPARAM key)
{
    return key == VK_CONTROL || key == VK_MENU || key == VK_SHIFT
        || key == VK_LWIN || key == VK_RWIN || (key >= VK_LSHIFT && key <= VK_RMENU);
}

bool anyKeyHeld()
{
    for (int key = 8; key < 255; ++key)
    {
        if ((GetAsyncKeyState(key) & 0x8000) != 0)
        {
            return true;
        }
    }
    return false;
}

std::wstring bindingText(const std::optional<bafx::config::HotkeyBinding>& binding)
{
    if (!binding.has_value())
    {
        return tr(TextId::Unbound);
    }
    std::wstring text;
    for (const auto& [name, bit] : std::array{std::pair{L"Ctrl+", 2U}, std::pair{L"Alt+", 1U},
        std::pair{L"Shift+", 4U}, std::pair{L"Win+", 8U}})
    {
        if ((binding->modifiers & bit) != 0U)
        {
            text += name;
        }
    }
    UINT scan = MapVirtualKeyW(binding->key, MAPVK_VK_TO_VSC_EX);
    LONG keyData = static_cast<LONG>((scan & 0xFFU) << 16U);
    if ((scan & 0xFF00U) != 0U)
    {
        keyData |= 1L << 24U;
    }
    wchar_t name[96]{};
    if (GetKeyNameTextW(keyData, name, 96) > 0)
    {
        text += name;
    }
    else
    {
        text += L"VK " + std::to_wstring(binding->key);
    }
    return text;
}

bool hasDuplicateBindings(const bafx::config::HotkeysConfig& hotkeys) noexcept
{
    for (std::size_t index = 0U; index < hotkeys.bindings.size(); ++index)
    {
        if (!hotkeys.bindings[index].has_value())
        {
            continue;
        }
        for (std::size_t previous = 0U; previous < index; ++previous)
        {
            if (hotkeys.bindings[index] == hotkeys.bindings[previous])
            {
                return true;
            }
        }
    }
    return false;
}

UiMessage registrationSummary(
    const bafx::config::HotkeysConfig& saved,
    const HostState& state)
{
    std::size_t configured = 0U;
    std::size_t registered = 0U;
    for (std::size_t index = 0U; index < saved.bindings.size(); ++index)
    {
        if (!saved.bindings[index].has_value())
        {
            continue;
        }
        ++configured;
        if ((state.hotkeyRegisteredMask & (1ULL << index)) != 0U)
        {
            ++registered;
        }
    }
    return UiMessage(TextId::HotkeySummary, {std::to_wstring(configured),
        std::to_wstring(registered), std::to_wstring(configured - registered)});
}
}

bool ControlCenterWindow::createHotkeyControls()
{
    const auto child = [this](const wchar_t* type, const TextId text, const DWORD style, const int id)
    {
        const HWND window = createChild(type, text, style, static_cast<ControlId>(id));
        hotkeyControls_.push_back(window);
        return window;
    };
    hotkeyHint_ = child(L"STATIC",
        TextId::HotkeyHint,
        SS_LEFT | SS_NOPREFIX, 0);
    for (std::size_t index = 0U; index < actionLabels.size(); ++index)
    {
        hotkeyLabels_[index] = child(L"STATIC", actionLabels[index], SS_LEFT | SS_NOPREFIX, 0);
        hotkeyValues_[index] = child(L"STATIC", TextId::Unbound, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0);
        hotkeyStatuses_[index] = child(L"STATIC", TextId::HostNotYetConnected,
            SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0);
        hotkeyRecord_[index] = child(L"BUTTON", TextId::Record, BS_PUSHBUTTON | WS_TABSTOP,
            recordFirst + static_cast<int>(index) * 2);
        hotkeyClear_[index] = child(L"BUTTON", TextId::Clear, BS_PUSHBUTTON | WS_TABSTOP,
            recordFirst + static_cast<int>(index) * 2 + 1);
    }
    hotkeySave_ = child(L"BUTTON", TextId::SaveAll, BS_PUSHBUTTON | WS_TABSTOP, saveId);
    hotkeyRevert_ = child(L"BUTTON", TextId::RevertChanges, BS_PUSHBUTTON | WS_TABSTOP, revertId);
    hotkeyRetry_ = child(L"BUTTON", TextId::RetryRegistration, BS_PUSHBUTTON | WS_TABSTOP, retryId);
    hotkeyCancel_ = child(L"BUTTON", TextId::CancelRecording, BS_PUSHBUTTON | WS_TABSTOP, cancelId);
    return hotkeysPageButton_ != nullptr && std::all_of(hotkeyControls_.begin(), hotkeyControls_.end(),
        [](const HWND control)
        {
            return control != nullptr;
        });
}

void ControlCenterWindow::layoutHotkeyControls(const int width, const int height) const noexcept
{
    const int left = scale(24);
    const int available = width - left * 2;
    const int labelWidth = scale(150);
    const int columnGap = scale(12);
    const int bindingWidth = (std::clamp)(available / 4, scale(180), scale(230));
    const int itemButtonWidth = scale(84);
    const int itemButtonGap = scale(8);
    const int itemActionsWidth = itemButtonWidth * 2 + itemButtonGap;
    const int itemActionsX = width - left - itemActionsWidth;
    const int bindingX = left + labelWidth + columnGap;
    const int statusX = bindingX + bindingWidth + columnGap;
    const int statusWidth = (std::max)(scale(120), itemActionsX - columnGap - statusX);
    const auto move = [](const HWND control, const int x, const int y, const int w, const int h)
    {
        SetWindowPos(control, nullptr, x, y, w, h,
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
    };
    move(hotkeyHint_, left, scale(160), available, scale(72));
    for (std::size_t index = 0U; index < actionLabels.size(); ++index)
    {
        const int top = scale(244 + static_cast<int>(index) * 50);
        move(hotkeyLabels_[index], left, top, labelWidth, scale(36));
        move(hotkeyValues_[index], bindingX, top, bindingWidth, scale(36));
        move(hotkeyStatuses_[index], statusX, top, statusWidth, scale(36));
        move(hotkeyRecord_[index], itemActionsX, top, itemButtonWidth, scale(36));
        move(hotkeyClear_[index], itemActionsX + itemButtonWidth + itemButtonGap,
            top, itemButtonWidth, scale(36));
    }
    const std::array buttons{hotkeySave_, hotkeyRevert_, hotkeyRetry_, hotkeyCancel_};
    const int buttonGap = scale(10);
    const int buttonWidth = (available - buttonGap * 3) / 4;
    for (std::size_t index = 0U; index < buttons.size(); ++index)
    {
        move(buttons[index], left + static_cast<int>(index) * (buttonWidth + buttonGap),
            scale(454), buttonWidth, scale(38));
    }
    const std::array footer{pauseButton_, refreshButton_, hostLifecycleButton_, resetDefaultsButton_};
    const int footerWidth = (available - scale(30)) / 4;
    for (std::size_t index = 0U; index < footer.size(); ++index)
    {
        move(footer[index], left + static_cast<int>(index) * (footerWidth + scale(10)),
            (std::max)(scale(528), height - scale(62)), footerWidth, scale(38));
    }
}

void ControlCenterWindow::updateHotkeyControls()
{
    const bool recording = hotkeyRecording_.has_value();
    const bool duplicateBindings = hasDuplicateBindings(hotkeyDraft_);
    for (std::size_t index = 0U; index < actionLabels.size(); ++index)
    {
        std::wstring text = bindingText(hotkeyDraft_.bindings[index]);
        std::wstring status;
        if (recording && *hotkeyRecording_ == index)
        {
            text = hotkeyAwaitRelease_ ? tr(TextId::ReleaseAllKeys) : tr(TextId::PressHotkey);
            if (hotkeyCandidate_.has_value())
            {
                text = bindingText(hotkeyCandidate_) + tr(TextId::ReleaseMainKeySuffix);
            }
            status = tr(TextId::RecordingStatus);
        }
        else if (!connected_ || !hotkeyStateKnown_)
        {
            status = tr(TextId::RegistrationUnavailable);
        }
        else if (hotkeyDraft_.bindings[index] != config_.hotkeys.bindings[index])
        {
            status = tr(TextId::Unsaved);
        }
        else if (!hotkeyDraft_.bindings[index].has_value())
        {
            status.clear();
        }
        else if ((hotkeyState_.hotkeyRegisteredMask & (1ULL << index)) != 0U)
        {
            status = tr(TextId::Registered);
        }
        else
        {
            status = tr(TextId::RegistrationFailedPrefix) + std::to_wstring(hotkeyState_.hotkeyErrors[index]);
        }
        for (std::size_t other = 0U; other < actionLabels.size(); ++other)
        {
            if (index != other && hotkeyDraft_.bindings[index].has_value()
                && hotkeyDraft_.bindings[index] == hotkeyDraft_.bindings[other])
            {
                status = formatText(TextId::HotkeyDuplicate, {actionLabels[other]});
            }
        }
        setText(hotkeyValues_[index], text.c_str());
        setText(hotkeyStatuses_[index], status.c_str());
        EnableWindow(hotkeyRecord_[index], connected_ && !recording
            && !hotkeyDraftConflicted_);
        EnableWindow(hotkeyClear_[index], connected_ && !recording
            && !hotkeyDraftConflicted_);
    }
    EnableWindow(hotkeySave_, connected_ && hotkeyDraftDirty_ && !recording
        && !duplicateBindings && !hotkeyDraftConflicted_);
    EnableWindow(hotkeyRevert_, connected_ && hotkeyDraftDirty_ && !recording);
    EnableWindow(hotkeyRetry_, connected_ && !hotkeyDraftDirty_ && !recording);
    EnableWindow(hotkeyCancel_, recording);
}

bool ControlCenterWindow::refreshHotkeys(
    const std::string_view command)
{
    const auto response = client_.transact(command);
    const auto parsed = response.succeeded() ? parseHostState(response.payload) : HostStateParseResult{};
    if (!parsed.succeeded() || !parsed.state->settingsCompatible() || !parsed.state->hotkeysJson.has_value())
    {
        clearHotkeyCaptureLocally();
        hotkeyStateKnown_ = false;
        setError(response.succeeded() ? UiMessage(TextId::InvalidHotkeyState) : describeResponse(response));
        updateHotkeyControls();
        return false;
    }
    const auto saved = bafx::config::parseHotkeysJson(*parsed.state->hotkeysJson);
    if (!saved.has_value())
    {
        clearHotkeyCaptureLocally();
        hotkeyStateKnown_ = false;
        setError(TextId::InvalidHostHotkeys);
        updateHotkeyControls();
        return false;
    }
    hotkeyState_ = *parsed.state;
    hotkeyStateKnown_ = true;
    if (!hotkeyDraftDirty_)
    {
        hotkeyDraft_ = *saved;
        hotkeyBaseline_ = *saved;
        hotkeyDraftGeneration_ = hotkeyState_.generation;
        hotkeyDraftConflicted_ = false;
    }
    else if (hotkeyDraft_ == *saved)
    {
        // SetHotkeys can report a post-commit activation or cleanup failure.
        // Matching Host state proves persistence and prevents a stale retry.
        hotkeyDraft_ = *saved;
        hotkeyBaseline_ = *saved;
        hotkeyDraftGeneration_ = hotkeyState_.generation;
        generation_ = hotkeyState_.generation;
        hotkeyDraftDirty_ = false;
        hotkeyDraftConflicted_ = false;
    }
    else if (hotkeyBaseline_ == *saved)
    {
        // Unrelated UI/shortcut mutations can advance the global generation.
        // Refresh it only while the binding baseline is still unchanged.
        hotkeyDraftGeneration_ = hotkeyState_.generation;
        hotkeyDraftConflicted_ = false;
    }
    else
    {
        hotkeyDraftConflicted_ = true;
        setError(TextId::HotkeyDraftConflict);
    }
    config_.hotkeys = *saved;
    const bool cleanupErrorChanged = displayedHotkeyCleanupError_
        != hotkeyState_.hotkeyCleanupError;
    if (cleanupErrorChanged)
    {
        displayedHotkeyCleanupError_ = hotkeyState_.hotkeyCleanupError;
        if (displayedHotkeyCleanupError_ != 0U)
        {
            setError(TextId::HotkeyCleanupFailed);
        }
    }
    if ((!cleanupErrorChanged || displayedHotkeyCleanupError_ == 0U)
        && displayedHotkeyActionError_ != hotkeyState_.hotkeyActionError)
    {
        displayedHotkeyActionError_ = hotkeyState_.hotkeyActionError;
        if (!displayedHotkeyActionError_.empty())
        {
            setError(utf8ToWide(displayedHotkeyActionError_));
        }
    }
    if (hotkeyRecording_.has_value())
    {
        if (hotkeyState_.hotkeyCaptureToken != hotkeyCaptureToken_)
        {
            clearHotkeyCaptureLocally();
            setInfo(TextId::RecordingEnded, TextId::RecordingExpired);
        }
        else if (!hotkeyAwaitRelease_ && !hotkeyCaptureInvalid_
            && hotkeyState_.hotkeyCaptureKey != 0U)
        {
            hotkeyCandidate_ = bafx::config::HotkeyBinding{
                static_cast<std::uint32_t>(hotkeyState_.hotkeyCaptureModifiers),
                static_cast<std::uint32_t>(hotkeyState_.hotkeyCaptureKey)};
            if ((GetAsyncKeyState(static_cast<int>(hotkeyCandidate_->key)) & 0x8000) == 0)
            {
                acceptHotkeyCandidate(*hotkeyCandidate_);
            }
        }
    }
    if (activePage_ == Page::Hotkeys && connected_ && !hotkeyRecording_.has_value())
    {
        SetTimer(window_, hotkeyTimerId, 1'000U, nullptr);
    }
    updateHotkeyControls();
    return true;
}

void ControlCenterWindow::beginHotkeyCapture(const std::size_t index)
{
    if (!commitPendingPatch())
    {
        return;
    }
    if (!connected_)
    {
        return;
    }
    static_cast<void>(refreshHotkeys("BeginHotkeyCapture"));
    if (!hotkeyStateKnown_ || hotkeyState_.hotkeyCaptureToken == 0U)
    {
        return;
    }
    hotkeyRecording_ = index;
    hotkeyCaptureToken_ = hotkeyState_.hotkeyCaptureToken;
    hotkeyCandidate_.reset();
    hotkeyCaptureInvalid_ = false;
    hotkeyAwaitRelease_ = anyKeyHeld();
    SetTimer(window_, hotkeyTimerId, 200U, nullptr);
    setInfo(TextId::RecordHotkey, TextId::RecordHotkeyHint);
    updateHotkeyControls();
}

void ControlCenterWindow::endHotkeyCapture()
{
    const auto token = hotkeyCaptureToken_;
    clearHotkeyCaptureLocally();
    if (token != 0U && connected_)
    {
        const auto response = client_.transact("EndHotkeyCapture " + std::to_string(token));
        if (!response.succeeded())
        {
            setError(TextId::RecordingEndUnconfirmed);
        }
    }
    if (activePage_ == Page::Hotkeys && connected_)
    {
        SetTimer(window_, hotkeyTimerId, 1'000U, nullptr);
    }
    updateHotkeyControls();
}

void ControlCenterWindow::clearHotkeyCaptureLocally() noexcept
{
    KillTimer(window_, hotkeyTimerId);
    hotkeyCaptureToken_ = 0U;
    hotkeyRecording_.reset();
    hotkeyCandidate_.reset();
    hotkeyCaptureInvalid_ = false;
    hotkeyAwaitRelease_ = false;
}

void ControlCenterWindow::acceptHotkeyCandidate(const bafx::config::HotkeyBinding binding)
{
    if (!hotkeyRecording_.has_value() || !bafx::config::validHotkeyKey(binding.key))
    {
        return;
    }
    hotkeyDraft_.bindings[*hotkeyRecording_] = binding;
    hotkeyDraftDirty_ = hotkeyDraft_ != config_.hotkeys;
    endHotkeyCapture();
    setInfo(TextId::RecordingDone, TextId::RecordingDoneHint);
}

bool ControlCenterWindow::captureHotkeyMessage(const MSG& message)
{
    if (!hotkeyRecording_.has_value()
        || (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN
            && message.message != WM_KEYUP && message.message != WM_SYSKEYUP))
    {
        return false;
    }
    if (hotkeyAwaitRelease_)
    {
        hotkeyAwaitRelease_ = anyKeyHeld();
        updateHotkeyControls();
        return true;
    }
    const bool down = message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
    if (down && (message.lParam & (1LL << 30U)) == 0 && !modifierKey(message.wParam))
    {
        if (!bafx::config::validHotkeyKey(static_cast<std::uint32_t>(message.wParam))
            || (hotkeyCandidate_.has_value() && hotkeyCandidate_->key != message.wParam))
        {
            hotkeyCaptureInvalid_ = true;
            setError(TextId::InvalidHotkeyCandidate);
        }
        else
        {
            std::uint32_t modifiers = 0U;
            for (const auto& [key, bit] : std::array{std::pair{VK_CONTROL, 2U}, std::pair{VK_MENU, 1U},
                std::pair{VK_SHIFT, 4U}, std::pair{VK_LWIN, 8U}, std::pair{VK_RWIN, 8U}})
            {
                if ((GetKeyState(key) & 0x8000) != 0)
                {
                    modifiers |= bit;
                }
            }
            hotkeyCandidate_ = bafx::config::HotkeyBinding{modifiers, static_cast<std::uint32_t>(message.wParam)};
        }
    }
    if (!down && !hotkeyCaptureInvalid_ && hotkeyCandidate_.has_value()
        && hotkeyCandidate_->key == message.wParam)
    {
        acceptHotkeyCandidate(*hotkeyCandidate_);
    }
    else if (hotkeyCaptureInvalid_ && !anyKeyHeld())
    {
        hotkeyCaptureInvalid_ = false;
        hotkeyCandidate_.reset();
    }
    updateHotkeyControls();
    return true;
}

bool ControlCenterWindow::saveHotkeys()
{
    if (!connected_ || hotkeyRecording_.has_value())
    {
        if (!connected_)
        {
            setError(TextId::CannotSaveHotkeysOffline);
        }
        return false;
    }
    if (hotkeyDraftConflicted_)
    {
        setError(TextId::HotkeyBindingsChanged);
        return false;
    }
    if (!commitPendingPatch())
    {
        return false;
    }
    std::string error;
    if (!bafx::config::validateHotkeys(hotkeyDraft_, &error))
    {
        setError(hasDuplicateBindings(hotkeyDraft_)
            ? TextId::DuplicateHotkeys
            : TextId::InvalidHotkeyConfig);
        updateHotkeyControls();
        return false;
    }
    const auto response = client_.transact("SetHotkeys " + std::to_string(hotkeyDraftGeneration_)
        + " " + bafx::config::toJson(hotkeyDraft_));
    if (!response.succeeded())
    {
        const UiMessage responseError = describeResponse(response);
        if (response.errorCode == "hotkey_activation_unconfirmed"
            || response.errorCode == "hotkey_cleanup_failed")
        {
            if (refreshHotkeys() && !hotkeyDraftDirty_)
            {
                setInfo(TextId::HotkeysSavedRestart,
                    TextId::HotkeysSavedRestartHint);
                return true;
            }
        }
        if (response.errorCode == "generation_conflict")
        {
            if (refreshHotkeys() && !hotkeyDraftDirty_)
            {
                setInfo(TextId::HotkeysSaved, TextId::HotkeysAlreadyConfirmed);
                return true;
            }
        }
        setError(responseError);
        return false;
    }
    if (!refreshHotkeys() || hotkeyDraftDirty_)
    {
        return false;
    }
    setInfo(TextId::HotkeysSaved, TextId::HotkeysUpdatedHint);
    return true;
}

bool ControlCenterWindow::confirmHotkeyDraft()
{
    endHotkeyCapture();
    if (!hotkeyDraftDirty_)
    {
        return true;
    }
    if (!connected_)
    {
        const int offlineChoice = localizedMessageBox(window_,
            TextId::DiscardOfflineDraftQuestion,
            TextId::CannotSaveHotkeys, MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
        if (offlineChoice != IDOK)
        {
            return false;
        }
        hotkeyDraftDirty_ = false;
        hotkeyDraftConflicted_ = false;
        hotkeyDraft_ = config_.hotkeys;
        hotkeyBaseline_ = config_.hotkeys;
        hotkeyDraftGeneration_ = generation_;
        updateHotkeyControls();
        return true;
    }
    const int choice = localizedMessageBox(window_, TextId::SaveHotkeyDraftQuestion,
        TextId::UnsavedHotkeys, MB_YESNOCANCEL | MB_ICONQUESTION);
    if (choice == IDYES)
    {
        return saveHotkeys();
    }
    if (choice == IDNO)
    {
        hotkeyDraftDirty_ = false;
        hotkeyDraftConflicted_ = false;
        hotkeyDraft_ = config_.hotkeys;
        hotkeyBaseline_ = config_.hotkeys;
        hotkeyDraftGeneration_ = generation_;
        updateHotkeyControls();
        return true;
    }
    return false;
}

bool ControlCenterWindow::onHotkeyCommand(const int id)
{
    if (id >= recordFirst && id < recordFirst + 8)
    {
        const auto index = static_cast<std::size_t>((id - recordFirst) / 2);
        if ((id - recordFirst) % 2 == 0)
        {
            beginHotkeyCapture(index);
        }
        else
        {
            hotkeyDraft_.bindings[index].reset();
            hotkeyDraftDirty_ = hotkeyDraft_ != config_.hotkeys;
            updateHotkeyControls();
        }
        return true;
    }
    switch (id)
    {
    case saveId:
        static_cast<void>(saveHotkeys());
        return true;
    case revertId:
        hotkeyDraftDirty_ = false;
        hotkeyDraftConflicted_ = false;
        static_cast<void>(refreshHotkeys());
        return true;
    case retryId:
        if (!hotkeyDraftDirty_)
        {
            const auto response = client_.transact("RetryHotkeys");
            const UiMessage retryError = response.succeeded()
                ? std::wstring{}
                : describeResponse(response);
            if (refreshHotkeys())
            {
                const UiMessage summary = registrationSummary(config_.hotkeys, hotkeyState_);
                if (!response.succeeded())
                {
                    setError(retryError + L"\r\n" + summary);
                }
                else if (hotkeyState_.hotkeyCleanupError != 0U)
                {
                    setError(UiMessage(TextId::HotkeyCleanupFailedLine) + summary);
                }
                else if (!hotkeyState_.hotkeyActionError.empty())
                {
                    setError(utf8ToWide(hotkeyState_.hotkeyActionError) + L"\r\n" + summary);
                }
                else
                {
                    setInfo(TextId::HotkeysRetried, summary);
                }
            }
            else if (!response.succeeded())
            {
                setError(retryError);
            }
        }
        return true;
    case cancelId:
        endHotkeyCapture();
        return true;
    default:
        return false;
    }
}

}
