#include "control_center_window.hpp"
#include "control_updates.hpp"
#include "product/version.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <sstream>

namespace bafx::control_center
{

bool ControlCenterWindow::createApplicationRuleControls()
{
    applicationRulesEnabled_ = createChild(L"BUTTON", TextId::ApplicationRulesEnabled,
        BS_AUTOCHECKBOX | WS_TABSTOP, ControlId::ApplicationRulesEnabled);
    applicationRulesHint_ = createChild(L"STATIC", TextId::ApplicationRulesHint, SS_LEFT | SS_NOPREFIX);
    applicationRuleList_ = createChild(L"LISTBOX", L"",
        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_BORDER | WS_VSCROLL | WS_TABSTOP,
        ControlId::ApplicationRuleList);
    applicationRuleTarget_ = createChild(L"EDIT", L"",
        ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP);
    applicationRuleToggle_ = createChild(L"BUTTON", TextId::ApplicationRuleToggle,
        BS_PUSHBUTTON | WS_TABSTOP, ControlId::ApplicationRuleToggle);
    applicationRuleDelete_ = createChild(L"BUTTON", TextId::Delete,
        BS_PUSHBUTTON | WS_TABSTOP, ControlId::ApplicationRuleDelete);
    applicationRuleModeLabel_ = createChild(L"STATIC", TextId::ApplicationRuleMode, SS_LEFT);
    applicationRuleMode_ = createChild(WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | WS_TABSTOP, ControlId::ApplicationRuleMode);
    applicationRuleBrowse_ = createChild(L"BUTTON", TextId::ApplicationRuleBrowse,
        BS_PUSHBUTTON | WS_TABSTOP, ControlId::ApplicationRuleBrowse);
    applicationRuleProcess_ = createChild(WC_COMBOBOXW, L"",
        CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ControlId::ApplicationRuleProcess);
    applicationRuleRefresh_ = createChild(L"BUTTON", TextId::ApplicationRuleRefresh,
        BS_PUSHBUTTON | WS_TABSTOP, ControlId::ApplicationRuleRefresh);
    applicationRuleAdd_ = createChild(L"BUTTON", TextId::ApplicationRuleAdd,
        BS_PUSHBUTTON | WS_TABSTOP, ControlId::ApplicationRuleAdd);
    updateApplicationRuleControls();
    return applicationRulesEnabled_ != nullptr && applicationRuleList_ != nullptr;
}

void ControlCenterWindow::layoutApplicationRuleControls(const int width, const int height) const noexcept
{
    const int x = scale(24);
    const int w = width - x * 2;
    const auto place = [this](const HWND control, const int left, const int top,
        const int controlWidth, const int controlHeight)
    {
        MoveWindow(control, left, top, controlWidth, controlHeight, TRUE);
    };
    place(applicationRulesEnabled_, x, scale(158), w, scale(28));
    place(applicationRulesHint_, x, scale(194), w, scale(42));
    const int footer = (std::max)(scale(400), height - scale(200));
    place(applicationRuleList_, x, scale(240), w, footer - scale(248));
    place(applicationRuleTarget_, x, footer, w - scale(260), scale(46));
    place(applicationRuleToggle_, width - x - scale(250), footer, scale(150), scale(30));
    place(applicationRuleDelete_, width - x - scale(92), footer, scale(92), scale(30));
    place(applicationRuleModeLabel_, x, footer + scale(56), scale(150), scale(28));
    place(applicationRuleMode_, x + scale(156), footer + scale(52), scale(240), scale(140));
    place(applicationRuleBrowse_, width - x - scale(240), footer + scale(52), scale(240), scale(30));
    place(applicationRuleProcess_, x, footer + scale(92), w - scale(280), scale(260));
    place(applicationRuleRefresh_, width - x - scale(270), footer + scale(92), scale(130), scale(30));
    place(applicationRuleAdd_, width - x - scale(132), footer + scale(92), scale(132), scale(30));
    const int gap = scale(10);
    const int buttonWidth = (w - gap * 3) / 4;
    const HWND actions[] = {pauseButton_, refreshButton_, hostLifecycleButton_, resetDefaultsButton_};
    for (int index = 0; index < 4; ++index)
    {
        const int left = x + index * (buttonWidth + gap);
        const int actionWidth = index == 3 ? width - x - left : buttonWidth;
        place(actions[index], left, height - x - scale(38), actionWidth, scale(38));
    }
}

void ControlCenterWindow::updateApplicationRuleControls()
{
    setChecked(applicationRulesEnabled_, config_.applicationRules.enabled);
    const ComboItem modes[] = {{tr(TextId::ApplicationRulePath), 0}, {tr(TextId::ApplicationRuleName), 1}};
    const auto mode = SendMessageW(applicationRuleMode_, CB_GETCURSEL, 0U, 0);
    if (updateComboItems(applicationRuleMode_, modes))
    {
        setComboSelection(applicationRuleMode_, mode < 0 ? 0 : mode);
    }
    std::istringstream statuses(presentationState_.applicationRuleStates);
    std::vector<std::wstring> rows;
    for (const auto& rule : config_.applicationRules.rules)
    {
        std::string status;
        std::getline(statuses, status);
        TextId statusText = TextId::ApplicationRuleUnknown;
        if (!rule.enabled || !config_.applicationRules.enabled)
        {
            statusText = TextId::ApplicationRuleInactive;
        }
        else if (status == "matched")
        {
            statusText = TextId::ApplicationRuleMatched;
        }
        else if (status == "clear")
        {
            statusText = TextId::ApplicationRuleClear;
        }
        else if (status == "waiting")
        {
            statusText = TextId::ApplicationRuleWaiting;
        }
        const auto name = rule.target.substr(rule.target.find_last_of("/\\") + 1U);
        rows.push_back(std::wstring(tr(rule.enabled ? TextId::ApplicationRuleOn : TextId::ApplicationRuleOff)) + L"  " + utf8ToWide(name) + L"  |  "
            + tr(rule.matchMode == bafx::config::ApplicationMatchMode::ExecutablePath
                ? TextId::ApplicationRulePath : TextId::ApplicationRuleName)
            + L"  |  " + tr(statusText));
    }
    if (rows != applicationRuleRows_)
    {
        const auto selected = SendMessageW(applicationRuleList_, LB_GETCURSEL, 0U, 0);
        const auto top = SendMessageW(applicationRuleList_, LB_GETTOPINDEX, 0U, 0);
        const bool visible = (GetWindowLongPtrW(applicationRuleList_, GWL_STYLE) & WS_VISIBLE) != 0;
        if (visible)
        {
            SendMessageW(applicationRuleList_, WM_SETREDRAW, FALSE, 0);
        }
        SendMessageW(applicationRuleList_, LB_RESETCONTENT, 0U, 0);
        for (const auto& row : rows)
        {
            SendMessageW(applicationRuleList_, LB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(row.c_str()));
        }
        if (!rows.empty())
        {
            SendMessageW(applicationRuleList_, LB_SETCURSEL,
                static_cast<WPARAM>((std::clamp)(selected, LRESULT{0}, static_cast<LRESULT>(rows.size() - 1U))), 0);
        }
        SendMessageW(applicationRuleList_, LB_SETTOPINDEX, static_cast<WPARAM>(top), 0);
        if (visible)
        {
            SendMessageW(applicationRuleList_, WM_SETREDRAW, TRUE, 0);
        }
        InvalidateRect(applicationRuleList_, nullptr, TRUE);
        applicationRuleRows_ = std::move(rows);
    }
    updateApplicationRuleSelection();
}

void ControlCenterWindow::updateApplicationRuleSelection()
{
    const auto selected = SendMessageW(applicationRuleList_, LB_GETCURSEL, 0U, 0);
    const bool valid = selected >= 0 && static_cast<std::size_t>(selected) < config_.applicationRules.rules.size();
    setControlEnabled(applicationRuleToggle_, connected_ && valid);
    setControlEnabled(applicationRuleDelete_, connected_ && valid);
    setText(applicationRuleTarget_, valid
        ? UiMessage(utf8ToWide(config_.applicationRules.rules[static_cast<std::size_t>(selected)].target))
        : UiMessage(TextId::ApplicationRulesEmpty));
}

void ControlCenterWindow::saveApplicationRules(bafx::config::ApplicationRulesConfig rules)
{
    if (!requireCurrentSnapshot())
    {
        return;
    }
    if (!bafx::config::validateApplicationRules(rules))
    {
        setError(TextId::ApplicationRuleInvalid);
        updateApplicationRuleControls();
        return;
    }
    static_cast<void>(applyPatch("applicationRules", bafx::config::toJson(rules)));
}

void ControlCenterWindow::addApplicationRule(std::wstring target)
{
    const bool byName = SendMessageW(applicationRuleMode_, CB_GETCURSEL, 0U, 0) == 1;
    if (byName)
    {
        target = std::filesystem::path(target).filename().wstring();
    }
    auto rules = config_.applicationRules;
    rules.rules.push_back({true, byName ? bafx::config::ApplicationMatchMode::ExecutableName
        : bafx::config::ApplicationMatchMode::ExecutablePath, wideToUtf8(target)});
    saveApplicationRules(std::move(rules));
}

bool ControlCenterWindow::onApplicationRuleCommand(const int id, const int notification)
{
    const auto command = static_cast<ControlId>(id);
    if (command == ControlId::ApplicationRuleList && notification == LBN_SELCHANGE)
    {
        updateApplicationRuleSelection();
        return true;
    }
    if (notification != BN_CLICKED)
    {
        return false;
    }
    switch (command)
    {
    case ControlId::ApplicationRuleRefresh:
        refreshApplicationProcesses();
        return true;
    case ControlId::ApplicationRulesEnabled:
    {
        auto rules = config_.applicationRules;
        rules.enabled = isChecked(applicationRulesEnabled_);
        saveApplicationRules(std::move(rules));
        return true;
    }
    case ControlId::ApplicationRuleToggle:
    case ControlId::ApplicationRuleDelete:
    {
        const auto selected = SendMessageW(applicationRuleList_, LB_GETCURSEL, 0U, 0);
        auto rules = config_.applicationRules;
        if (selected >= 0 && static_cast<std::size_t>(selected) < rules.rules.size())
        {
            if (command == ControlId::ApplicationRuleDelete)
            {
                rules.rules.erase(rules.rules.begin() + selected);
            }
            else
            {
                auto& rule = rules.rules[static_cast<std::size_t>(selected)];
                rule.enabled = !rule.enabled;
            }
            saveApplicationRules(std::move(rules));
        }
        return true;
    }
    case ControlId::ApplicationRuleBrowse:
    {
        std::wstring filename(32'768U, L'\0');
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"Windows EXE (*.exe)\0*.exe\0\0";
        dialog.lpstrFile = filename.data();
        dialog.nMaxFile = static_cast<DWORD>(filename.size());
        dialog.lpstrTitle = tr(TextId::ApplicationRuleBrowse);
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&dialog) != FALSE)
        {
            filename.resize(wcslen(filename.c_str()));
            addApplicationRule(std::move(filename));
        }
        return true;
    }
    case ControlId::ApplicationRuleAdd:
    {
        const auto selected = SendMessageW(applicationRuleProcess_, CB_GETCURSEL, 0U, 0);
        if (selected < 0 || static_cast<std::size_t>(selected) >= applicationProcesses_.size())
        {
            setError(TextId::ApplicationRuleChooseProcess);
            return true;
        }
        const auto& process = applicationProcesses_[static_cast<std::size_t>(selected)];
        if (process.executablePath.empty()
            && SendMessageW(applicationRuleMode_, CB_GETCURSEL, 0U, 0) != 1)
        {
            setError(TextId::ApplicationRulePathUnavailable);
            return true;
        }
        addApplicationRule(process.executablePath.empty()
            ? process.executableName : process.executablePath.wstring());
        return true;
    }
    default:
        return false;
    }
}

void ControlCenterWindow::refreshApplicationProcesses()
{
    if (applicationProcessesFuture_.valid())
    {
        return;
    }
    // Enumerating protected/elevated processes may be slow. The UI only polls
    // completion; the worker owns no HWND and never modifies configuration.
    applicationProcessesFuture_ = std::async(std::launch::async, []()
    {
        return bafx::windows::queryProcessSnapshot();
    });
    setControlEnabled(applicationRuleRefresh_, FALSE);
    SetTimer(window_, applicationProcessTimerId, 50U, nullptr);
}

void ControlCenterWindow::pollApplicationProcesses()
{
    if (!applicationProcessesFuture_.valid()
        || applicationProcessesFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        return;
    }
    KillTimer(window_, applicationProcessTimerId);
    auto snapshot = applicationProcessesFuture_.get();
    setControlEnabled(applicationRuleRefresh_, TRUE);
    if (!snapshot.complete)
    {
        setError(TextId::ApplicationRuleScanFailed);
        return;
    }
    std::erase_if(snapshot.processes, [](const auto& process)
    {
        return !process.sessionKnown;
    });
    std::ranges::sort(snapshot.processes, {}, &bafx::windows::ProcessInfo::executableName);
    applicationProcesses_ = std::move(snapshot.processes);
    std::vector<ComboItem> items;
    for (const auto& process : applicationProcesses_)
    {
        items.push_back({process.executableName + L" (" + std::to_wstring(process.id) + L")  "
            + (process.executablePath.empty() ? tr(TextId::ApplicationRulePathUnknown)
                : process.executablePath.wstring()), static_cast<LPARAM>(process.id)});
    }
    static_cast<void>(updateComboItems(applicationRuleProcess_, items));
    setComboSelection(applicationRuleProcess_, items.empty() ? -1 : 0);
    SendMessageW(applicationRuleProcess_, CB_SETDROPPEDWIDTH, scale(740), 0);
}

void ControlCenterWindow::updateApplicationRuleStatus()
{
    if (connected_ && presentationState_.automaticSuppressed)
    {
        setText(statusText_, UiMessage(TextId::ApplicationRulesSuppressed,
            {utf8ToWide(presentationState_.applicationRuleReason)}));
    }
    else if (connected_ && presentationState_.applicationRulesError != 0U)
    {
        setText(statusText_, TextId::ApplicationRuleScanFailed);
    }
    else if (connected_)
    {
        setText(statusText_, UiMessage(TextId::ConnectedStatus,
            {paused_ ? TextId::Paused : TextId::Running,
                presentationState_.backgroundCapture == "active" ? UiMessage::Argument(TextId::BackgroundActive)
                    : (presentationState_.backgroundCapture == "fallback-fx-only"
                        ? UiMessage::Argument(TextId::BackgroundFallback)
                        : UiMessage::Argument(utf8ToWide(presentationState_.backgroundCapture)))}));
    }
    if (trayIconAdded_)
    {
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = window_;
        icon.uID = 1U;
        icon.uFlags = NIF_TIP;
        const auto tip = connected_ && presentationState_.automaticSuppressed
            ? formatText(TextId::ApplicationRulesSuppressed, {utf8ToWide(presentationState_.applicationRuleReason)})
            : formatText(TextId::TrayTooltip, {utf8ToWide(bafx::product::version)});
        wcsncpy_s(icon.szTip, tip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &icon);
    }
}

}
