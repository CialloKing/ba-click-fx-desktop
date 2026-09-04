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
constexpr std::array actionLabels{L"暂停／恢复特效", L"切换常驻拖尾", L"下一个特效预设", L"退出 Host"};

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
        return L"未绑定";
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

std::wstring registrationSummary(
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
    return L"已保存 " + std::to_wstring(configured) + L" 项；已注册 "
        + std::to_wstring(registered) + L" 项；注册失败 "
        + std::to_wstring(configured - registered) + L" 项。逐项状态见列表。";
}
}

bool ControlCenterWindow::createHotkeyControls()
{
    const auto child = [this](const wchar_t* type, const wchar_t* text, const DWORD style, const int id)
    {
        const HWND window = createChild(type, text, style, static_cast<ControlId>(id));
        hotkeyControls_.push_back(window);
        return window;
    };
    hotkeyHint_ = child(L"STATIC",
        L"支持单键或 Ctrl / Alt / Shift / Win + 一个主键，F12 不可用。\r\n"
        L"全局热键可能影响前台软件原有操作；Win 组合为系统保留，不保证可用。\r\n"
        L"单普通键容易误触，退出键尤其需要谨慎。录制完成后请保存全部。",
        SS_LEFT | SS_NOPREFIX, 0);
    for (std::size_t index = 0U; index < actionLabels.size(); ++index)
    {
        hotkeyLabels_[index] = child(L"STATIC", actionLabels[index], SS_LEFT | SS_NOPREFIX, 0);
        hotkeyValues_[index] = child(L"STATIC", L"未绑定", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0);
        hotkeyStatuses_[index] = child(L"STATIC", L"尚未连接 Host", SS_LEFT | SS_NOPREFIX, 0);
        hotkeyRecord_[index] = child(L"BUTTON", L"录制", BS_PUSHBUTTON | WS_TABSTOP,
            recordFirst + static_cast<int>(index) * 2);
        hotkeyClear_[index] = child(L"BUTTON", L"清除", BS_PUSHBUTTON | WS_TABSTOP,
            recordFirst + static_cast<int>(index) * 2 + 1);
    }
    hotkeySave_ = child(L"BUTTON", L"保存全部", BS_PUSHBUTTON | WS_TABSTOP, saveId);
    hotkeyRevert_ = child(L"BUTTON", L"撤销修改", BS_PUSHBUTTON | WS_TABSTOP, revertId);
    hotkeyRetry_ = child(L"BUTTON", L"重试注册", BS_PUSHBUTTON | WS_TABSTOP, retryId);
    hotkeyCancel_ = child(L"BUTTON", L"取消录制", BS_PUSHBUTTON | WS_TABSTOP, cancelId);
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
    const auto move = [](const HWND control, const int x, const int y, const int w, const int h)
    {
        SetWindowPos(control, nullptr, x, y, w, h,
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
    };
    move(hotkeyHint_, left, scale(165), available, scale(72));
    for (std::size_t index = 0U; index < actionLabels.size(); ++index)
    {
        const int top = scale(244 + static_cast<int>(index) * 54);
        move(hotkeyLabels_[index], left, top, scale(150), scale(25));
        move(hotkeyValues_[index], left + scale(160), top,
            (std::max)(scale(100), available - scale(330)), scale(25));
        move(hotkeyRecord_[index], width - left - scale(155), top, scale(72), scale(30));
        move(hotkeyClear_[index], width - left - scale(75), top, scale(72), scale(30));
        move(hotkeyStatuses_[index], left + scale(160), top + scale(27),
            (std::max)(scale(200), available - scale(165)), scale(25));
    }
    const std::array buttons{hotkeySave_, hotkeyRevert_, hotkeyRetry_, hotkeyCancel_};
    for (std::size_t index = 0U; index < buttons.size(); ++index)
    {
        move(buttons[index], left + scale(static_cast<int>(index) * 120), scale(480), scale(110), scale(34));
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
            text = hotkeyAwaitRelease_ ? L"请先松开所有按键…" : L"请按下快捷键…";
            if (hotkeyCandidate_.has_value())
            {
                text = bindingText(hotkeyCandidate_) + L"（松开主键完成）";
            }
            status = L"录制期间不执行本程序快捷键，最多 30 秒";
        }
        else if (!connected_ || !hotkeyStateKnown_)
        {
            status = L"注册状态不可用";
        }
        else if (hotkeyDraft_.bindings[index] != config_.hotkeys.bindings[index])
        {
            status = L"未保存";
        }
        else if (!hotkeyDraft_.bindings[index].has_value())
        {
            status = L"未绑定";
        }
        else if ((hotkeyState_.hotkeyRegisteredMask & (1ULL << index)) != 0U)
        {
            status = L"已注册";
        }
        else
        {
            status = L"被占用／注册失败，Win32=" + std::to_wstring(hotkeyState_.hotkeyErrors[index]);
        }
        for (std::size_t other = 0U; other < actionLabels.size(); ++other)
        {
            if (index != other && hotkeyDraft_.bindings[index].has_value()
                && hotkeyDraft_.bindings[index] == hotkeyDraft_.bindings[other])
            {
                status = std::wstring(L"与“") + actionLabels[other] + L"”重复，请修改";
            }
        }
        SetWindowTextW(hotkeyValues_[index], text.c_str());
        SetWindowTextW(hotkeyStatuses_[index], status.c_str());
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
        setError(response.succeeded() ? L"快捷键状态无效，请重新连接 Host。" : describeResponse(response));
        updateHotkeyControls();
        return false;
    }
    const auto saved = bafx::config::parseHotkeysJson(*parsed.state->hotkeysJson);
    if (!saved.has_value())
    {
        clearHotkeyCaptureLocally();
        hotkeyStateKnown_ = false;
        setError(L"Host 返回了无效的快捷键配置。");
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
        setError(L"Host 快捷键已被其他客户端修改。草稿已保留，请撤销修改后重新录制，避免覆盖较新的绑定。");
    }
    config_.hotkeys = *saved;
    const bool cleanupErrorChanged = displayedHotkeyCleanupError_
        != hotkeyState_.hotkeyCleanupError;
    if (cleanupErrorChanged)
    {
        displayedHotkeyCleanupError_ = hotkeyState_.hotkeyCleanupError;
        if (displayedHotkeyCleanupError_ != 0U)
        {
            setError(L"旧快捷键注册清理失败，请重启 Host。");
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
            setInfo(L"录制已结束", L"录制超时或 Host 会话已变化，请重新录制。");
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
    setInfo(L"录制快捷键", L"先按住修饰键，再按一个主键；无法录到系统或其他软件占用的组合时，请换一组。");
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
            setError(L"录制结束请求未确认；Host 最迟在会话停止续期 5 秒后自动恢复快捷键。");
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
    setInfo(L"录制完成，尚未保存", L"请点击“保存全部”；只有注册成功的组合才会生效。");
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
            setError(L"只支持一个非修饰主键，且 F12 不可用；请松开按键后重试。");
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
            setError(L"Host 未连接，无法保存快捷键。请重新连接，或在关闭时选择丢弃草稿。");
        }
        return false;
    }
    if (hotkeyDraftConflicted_)
    {
        setError(L"Host 快捷键已变化。请先撤销草稿，再基于最新绑定重新录制。");
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
            ? L"存在重复的快捷键组合，请修改后再保存。"
            : L"快捷键配置无效，请清除对应项目后重新录制。");
        updateHotkeyControls();
        return false;
    }
    const auto response = client_.transact("SetHotkeys " + std::to_string(hotkeyDraftGeneration_)
        + " " + bafx::config::toJson(hotkeyDraft_));
    if (!response.succeeded())
    {
        const std::wstring responseError = describeResponse(response);
        if (response.errorCode == "hotkey_activation_unconfirmed"
            || response.errorCode == "hotkey_cleanup_failed")
        {
            if (refreshHotkeys() && !hotkeyDraftDirty_)
            {
                setInfo(L"快捷键已保存，需重启 Host",
                    L"配置已写入，但当前注册状态未能完全确认。重启 Host 后将按已保存绑定重新注册。");
                return true;
            }
        }
        if (response.errorCode == "generation_conflict")
        {
            if (refreshHotkeys() && !hotkeyDraftDirty_)
            {
                setInfo(L"快捷键已保存", L"已从 Host 确认全部绑定，无需再次保存。");
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
    setInfo(L"快捷键已保存", L"全部绑定已更新。清除的组合已释放，未绑定动作不会响应键盘。");
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
        const int offlineChoice = MessageBoxW(window_,
            L"Host 已断开，当前快捷键草稿无法保存。\n\n"
            L"选择“确定”丢弃草稿并继续，或选择“取消”返回。",
            L"无法保存快捷键", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
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
    const int choice = MessageBoxW(window_, L"快捷键有未保存的修改。是否保存？\n选择“否”丢弃，或“取消”返回。",
        L"未保存的快捷键", MB_YESNOCANCEL | MB_ICONQUESTION);
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
            const std::wstring retryError = response.succeeded()
                ? std::wstring{}
                : describeResponse(response);
            if (refreshHotkeys())
            {
                const std::wstring summary = registrationSummary(config_.hotkeys, hotkeyState_);
                if (!response.succeeded())
                {
                    setError(retryError + L"\r\n" + summary);
                }
                else if (hotkeyState_.hotkeyCleanupError != 0U)
                {
                    setError(L"旧快捷键注册清理失败，请重启 Host。\r\n" + summary);
                }
                else if (!hotkeyState_.hotkeyActionError.empty())
                {
                    setError(utf8ToWide(hotkeyState_.hotkeyActionError) + L"\r\n" + summary);
                }
                else
                {
                    setInfo(L"已重试保存的快捷键", summary);
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
