#pragma once

#include <windows.h>

#include <span>
#include <string>
#include <string_view>

namespace bafx::control_center
{

// Compare against the real control, including optimistic user edits, instead
// of caching the last value that the Host sent to the view.
// preserveReadPosition is for multiline diagnostic edits that refresh while
// the user is selecting or scrolling through their contents.
void setControlText(HWND control, std::wstring_view text, bool preserveReadPosition = false);
void setControlEnabled(HWND control, BOOL enabled) noexcept;
void setComboSelection(HWND control, LRESULT selected) noexcept;

struct ComboItem final
{
    std::wstring text;
    LPARAM data{0};
};

// Preserve selection, the open dropdown and its scroll position when the
// catalog is unchanged. The caller resolves selection after a catalog change.
[[nodiscard]] bool updateComboItems(HWND control, std::span<const ComboItem> items);

}
