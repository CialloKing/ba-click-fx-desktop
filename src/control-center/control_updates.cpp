#include "control_updates.hpp"

#include <commctrl.h>

namespace bafx::control_center
{

void setControlText(const HWND control, const std::wstring_view text)
{
    if (control == nullptr)
    {
        return;
    }
    const int length = GetWindowTextLengthW(control);
    if (length >= 0 && static_cast<std::size_t>(length) == text.size())
    {
        std::wstring current(text.size() + 1U, L'\0');
        const int copied = GetWindowTextW(control, current.data(), static_cast<int>(current.size()));
        current.resize(static_cast<std::size_t>(copied));
        if (current == text)
        {
            return;
        }
    }
    SetWindowTextW(control, std::wstring(text).c_str());
}

void setControlEnabled(const HWND control, const BOOL enabled) noexcept
{
    if (control != nullptr && (IsWindowEnabled(control) != FALSE) != (enabled != FALSE))
    {
        EnableWindow(control, enabled);
    }
}

void setComboSelection(const HWND control, const LRESULT selected) noexcept
{
    if (control != nullptr && SendMessageW(control, CB_GETCURSEL, 0U, 0) != selected)
    {
        SendMessageW(control, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
    }
}

bool updateComboItems(const HWND control, const std::span<const ComboItem> items)
{
    if (control == nullptr)
    {
        return false;
    }
    bool unchanged = SendMessageW(control, CB_GETCOUNT, 0U, 0)
        == static_cast<LRESULT>(items.size());
    for (std::size_t index = 0U; unchanged && index < items.size(); ++index)
    {
        const auto length = SendMessageW(control, CB_GETLBTEXTLEN, index, 0);
        if (length < 0 || static_cast<std::size_t>(length) != items[index].text.size())
        {
            unchanged = false;
            break;
        }
        std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
        const auto copied = SendMessageW(control, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text.data()));
        unchanged = copied == length
            && std::wstring_view(text.data(), static_cast<std::size_t>(length)) == items[index].text
            && SendMessageW(control, CB_GETITEMDATA, index, 0) == items[index].data;
    }
    if (unchanged)
    {
        return true;
    }
    SendMessageW(control, CB_RESETCONTENT, 0U, 0);
    for (const auto& item : items)
    {
        const auto index = SendMessageW(control, CB_ADDSTRING, 0U, reinterpret_cast<LPARAM>(item.text.c_str()));
        if (index == CB_ERR || index == CB_ERRSPACE
            || SendMessageW(control, CB_SETITEMDATA, static_cast<WPARAM>(index), item.data) == CB_ERR)
        {
            SendMessageW(control, CB_RESETCONTENT, 0U, 0);
            return false;
        }
    }
    return true;
}

}
