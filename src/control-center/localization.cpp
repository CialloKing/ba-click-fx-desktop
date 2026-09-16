#include "localization.hpp"

#include <commctrl.h>

#include <array>

namespace bafx::control_center
{

std::string_view textIdName(const TextId id) noexcept
{
    switch (id)
    {
#define BAFX_TEXT(name, chinese, english) case TextId::name: return #name;
#include "localized_strings.inc"
#undef BAFX_TEXT
    case TextId::Count: return "Count";
    }
    return "Unknown";
}

namespace
{
struct Translation
{
    const wchar_t* chinese;
    const wchar_t* english;
};
constexpr std::array translations{
#define BAFX_TEXT(id, chinese, english) Translation{chinese, english},
#include "localized_strings.inc"
#undef BAFX_TEXT
};
static_assert(translations.size() == static_cast<std::size_t>(TextId::Count));
thread_local UiLanguage activeLanguage = UiLanguage::SimplifiedChinese;
}

UiLanguage parseUiLanguage(std::string_view value) noexcept
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
    {
        return UiLanguage::System;
    }
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1U);
    if (value == "zh-CN")
    {
        return UiLanguage::SimplifiedChinese;
    }
    if (value == "en-US")
    {
        return UiLanguage::English;
    }
    return UiLanguage::System;
}

std::string_view uiLanguageToken(const UiLanguage language) noexcept
{
    switch (language)
    {
    case UiLanguage::SimplifiedChinese:
        return "zh-CN";
    case UiLanguage::English:
        return "en-US";
    default:
        return "auto";
    }
}

UiLanguage resolveUiLanguage(const UiLanguage preference, const LANGID userLanguage) noexcept
{
    if (preference != UiLanguage::System)
    {
        return preference;
    }
    return PRIMARYLANGID(userLanguage) == LANG_CHINESE
        ? UiLanguage::SimplifiedChinese : UiLanguage::English;
}

void setUiLanguage(const UiLanguage preference) noexcept
{
    activeLanguage = resolveUiLanguage(preference, GetUserDefaultUILanguage());
}

UiLanguage currentUiLanguage() noexcept
{
    return activeLanguage;
}

const wchar_t* translatedText(const TextId id, const UiLanguage language) noexcept
{
    const auto index = static_cast<std::size_t>(id);
    if (index >= translations.size())
    {
        return L"";
    }
    return language == UiLanguage::SimplifiedChinese
        ? translations[index].chinese : translations[index].english;
}

const wchar_t* tr(const TextId id) noexcept
{
    return translatedText(id, currentUiLanguage());
}

UiMessage::UiMessage(const TextId text, const std::initializer_list<Argument> values)
    : id(text), arguments(values)
{
}

UiMessage::UiMessage(std::wstring value) : arguments{std::move(value)}
{
}

UiMessage::UiMessage(const std::wstring_view value) : UiMessage(std::wstring(value))
{
}

UiMessage::UiMessage(const wchar_t* value) : UiMessage(std::wstring(value))
{
}

std::wstring UiMessage::render(const UiLanguage language) const
{
    const std::wstring_view pattern = translatedText(id, language);
    std::wstring result;
    for (std::size_t index = 0U; index < pattern.size(); ++index)
    {
        if (pattern[index] == L'{' && index + 2U < pattern.size()
            && pattern[index + 1U] >= L'0' && pattern[index + 1U] <= L'9'
            && pattern[index + 2U] == L'}')
        {
            const auto argumentIndex = static_cast<std::size_t>(pattern[index + 1U] - L'0');
            if (argumentIndex < arguments.size())
            {
                const Argument& argument = arguments[argumentIndex];
                if (const auto* text = std::get_if<std::wstring>(&argument))
                {
                    result += *text;
                }
                else
                {
                    result += translatedText(std::get<TextId>(argument), language);
                }
            }
            index += 2U;
        }
        else
        {
            result += pattern[index];
        }
    }
    for (const auto& suffix : suffixes)
    {
        result += suffix.render(language);
    }
    return result;
}

bool UiMessage::empty() const
{
    return render().empty();
}

void UiMessage::clear()
{
    *this = UiMessage{};
}

UiMessage operator+(UiMessage left, const UiMessage& right)
{
    left.suffixes.push_back(right);
    return left;
}

UiMessage& UiMessage::operator+=(const UiMessage& right)
{
    suffixes.push_back(right);
    return *this;
}

std::wstring formatText(const TextId id, const std::initializer_list<UiMessage::Argument> values)
{
    return UiMessage(id, values).render();
}

std::wstring profileDisplayName(const std::string_view name, const bool builtIn)
{
    if (builtIn)
    {
        constexpr std::array names{
            std::pair{"Unity 原版", TextId::UnityOriginal},
            std::pair{"轻量", TextId::Lightweight},
            std::pair{"纯点击", TextId::ClickOnly},
            std::pair{"纯拖尾", TextId::TrailOnly}};
        for (const auto& [internalName, text] : names)
        {
            if (name == internalName)
            {
                return tr(text);
            }
        }
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        name.data(), static_cast<int>(name.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (count > 0)
    {
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            name.data(), static_cast<int>(name.size()), result.data(), count);
    }
    return result;
}

int localizedMessageBox(const HWND owner, const UiMessage& message,
    const UiMessage& title, const UINT flags)
{
    const std::wstring body = message.render();
    const std::wstring caption = title.render();
    std::vector<TASKDIALOG_BUTTON> buttons;
    const UINT type = flags & MB_TYPEMASK;
    if (type == MB_YESNO || type == MB_YESNOCANCEL)
    {
        buttons.push_back({IDYES, tr(TextId::Yes)});
        buttons.push_back({IDNO, tr(TextId::No)});
    }
    else
    {
        buttons.push_back({IDOK, tr(TextId::Ok)});
    }
    if (type == MB_OKCANCEL || type == MB_YESNOCANCEL)
    {
        buttons.push_back({IDCANCEL, tr(TextId::Cancel)});
    }
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof(dialog);
    dialog.hwndParent = owner;
    dialog.dwFlags = TDF_SIZE_TO_CONTENT | TDF_POSITION_RELATIVE_TO_WINDOW;
    dialog.pszWindowTitle = caption.c_str();
    dialog.pszContent = body.c_str();
    dialog.cButtons = static_cast<UINT>(buttons.size());
    dialog.pButtons = buttons.data();
    dialog.nDefaultButton = buttons[(flags & MB_DEFMASK) == MB_DEFBUTTON2 ? 1U : 0U].nButtonID;
    dialog.pszMainIcon = (flags & MB_ICONMASK) == MB_ICONERROR ? TD_ERROR_ICON
        : ((flags & MB_ICONMASK) == MB_ICONWARNING ? TD_WARNING_ICON : TD_INFORMATION_ICON);
    int result = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&dialog, &result, nullptr, nullptr)))
    {
        // Even common-control initialization failures need a visible error.
        // Never turn a dialog creation failure into approval of a mutation.
        MessageBoxW(owner, body.c_str(), caption.c_str(), MB_OK | MB_ICONERROR);
        return IDCANCEL;
    }
    return result;
}

}
