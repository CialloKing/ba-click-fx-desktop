#include "control_center_window.hpp"
#include "product/version.hpp"

#include <commctrl.h>
#include <shellapi.h>

#include <array>

namespace bafx::control_center
{
namespace
{
void translateCombo(const HWND control, const std::initializer_list<TextId> items)
{
    const LRESULT selected = SendMessageW(control, CB_GETCURSEL, 0U, 0);
    SendMessageW(control, CB_RESETCONTENT, 0U, 0);
    for (const TextId item : items)
    {
        SendMessageW(control, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(tr(item)));
    }
    SendMessageW(control, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
}
}

HWND ControlCenterWindow::createChild(const wchar_t* className, const TextId text,
    const DWORD style, const ControlId id) const
{
    const HWND control = createChild(className, tr(text), style, id);
    setText(control, text);
    return control;
}

void ControlCenterWindow::setText(const HWND control, const UiMessage& message) const
{
    if (control == nullptr)
    {
        return;
    }
    // Editable contents are user data, not translated captions. Replaying an
    // earlier config snapshot here would erase an unfinished color/name draft.
    if (control != themeColorEdit_ && control != fxProfileNameEdit_)
    {
        localizedTexts_.insert_or_assign(control, message);
    }
    const std::wstring text = message.render();
    SetWindowTextW(control, text.c_str());
}

void ControlCenterWindow::changeLanguage()
{
    const LRESULT selected = SendMessageW(languageSelector_, CB_GETCURSEL, 0U, 0);
    if (selected < 0 || selected > 2)
    {
        return;
    }
    const auto preference = static_cast<UiLanguage>(selected);
    if (preference == languagePreference_)
    {
        return;
    }
    const DWORD status = saveLanguagePreference(languagePath_, preference);
    if (status != ERROR_SUCCESS)
    {
        SendMessageW(languageSelector_, CB_SETCURSEL, static_cast<WPARAM>(languagePreference_), 0);
        setError(UiMessage(TextId::LanguageSaveFailed, {std::to_wstring(status)}));
        return;
    }
    languagePreference_ = preference;
    setUiLanguage(preference);
    retranslateUi();
}

void ControlCenterWindow::retranslateUi()
{
    if (titleText_ == nullptr)
    {
        return;
    }
    const bool wasUpdating = updatingControls_;
    updatingControls_ = true;
    const bool wasVisible = IsWindowVisible(window_) != FALSE;
    if (wasVisible)
    {
        SendMessageW(window_, WM_SETREDRAW, FALSE, 0);
    }
    // Refresh presentation only. In particular, updateControls() would discard
    // partially edited colors and can publish Host configuration generations.
    for (const auto& [control, message] : localizedTexts_)
    {
        const std::wstring text = message.render();
        SetWindowTextW(control, text.c_str());
    }
    translateCombo(effectsMode_, {TextId::FullEffects, TextId::CoreEffects});
    translateCombo(backgroundMode_, {TextId::BackgroundAware, TextId::RecordingCompatible, TextId::LightBackground});
    translateCombo(bloomQuality_, {TextId::Compact, TextId::Moderate, TextId::Original, TextId::ExtraWide, TextId::Custom});
    SendMessageW(fxProfileNameEdit_, EM_SETCUEBANNER, TRUE,
        reinterpret_cast<LPARAM>(tr(TextId::ProfileName)));
    for (const HWND control : {framePacing_, displayFramePacing_})
    {
        translateCombo(control, {TextId::MatchDisplay, TextId::Fixed60, TextId::Fixed120, TextId::Fixed144, TextId::UnlimitedFps});
    }
    const LRESULT selectedProfile = SendMessageW(fxProfileSelector_, CB_GETCURSEL, 0U, 0);
    SendMessageW(fxProfileSelector_, CB_RESETCONTENT, 0U, 0);
    SendMessageW(fxProfileSelector_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(tr(TextId::Custom)));
    for (const auto& profile : fxProfiles_)
    {
        const auto name = profileDisplayName(profile.name, profile.builtIn);
        SendMessageW(fxProfileSelector_, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(name.c_str()));
    }
    SendMessageW(fxProfileSelector_, CB_SETCURSEL, static_cast<WPARAM>(selectedProfile), 0);
    updateVersionPresentation();
    updateDisplayControls(config_);
    updateHotkeyControls();
    updateHostLifecycleButton();
#if defined(BAFX_ENABLE_SPOUT2)
    if (connected_)
    {
        updateSpout2Status(presentationState_);
    }
    updateObsPluginPresentation();
#endif
    const std::wstring info = infoTitle_.render() + L"\r\n" + infoMessage_.render();
    SetWindowTextW(messageText_, info.c_str());
    if (trayIconAdded_)
    {
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = window_;
        icon.uID = 1U;
        icon.uFlags = NIF_TIP;
        const auto tip = formatText(TextId::TrayTooltip, {utf8ToWide(bafx::product::version)});
        wcsncpy_s(icon.szTip, tip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &icon);
    }
    RECT client{};
    GetClientRect(window_, &client);
    layoutControls(client.right, client.bottom);
    if (wasVisible)
    {
        SendMessageW(window_, WM_SETREDRAW, TRUE, 0);
    }
    updatingControls_ = wasUpdating;
    redrawWindowTree();
}

}
