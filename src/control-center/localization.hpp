#pragma once

#include <windows.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bafx::control_center
{

enum class UiLanguage : std::uint8_t
{
    System,
    SimplifiedChinese,
    English
};

enum class TextId : std::uint16_t
{
#define BAFX_TEXT(id, chinese, english) id,
#include "localized_strings.inc"
#undef BAFX_TEXT
    Count
};

[[nodiscard]] UiLanguage parseUiLanguage(std::string_view value) noexcept;
[[nodiscard]] std::string_view uiLanguageToken(UiLanguage language) noexcept;
[[nodiscard]] UiLanguage resolveUiLanguage(UiLanguage preference, LANGID userLanguage) noexcept;
// The native control center owns all presentation on one UI thread.
void setUiLanguage(UiLanguage preference) noexcept;
[[nodiscard]] UiLanguage currentUiLanguage() noexcept;
[[nodiscard]] const wchar_t* translatedText(TextId id, UiLanguage language) noexcept;
[[nodiscard]] const wchar_t* tr(TextId id) noexcept;

// Keep IDs and raw parameters, so a visible message can be rendered again
// without repeating the operation that originally produced it.
struct UiMessage final
{
    using Argument = std::variant<std::wstring, TextId>;
    TextId id{TextId::Raw};
    std::vector<Argument> arguments{std::wstring{}};

    UiMessage() = default;
    UiMessage(TextId text, std::initializer_list<Argument> values = {});
    UiMessage(std::wstring value);
    UiMessage(std::wstring_view value);
    UiMessage(const wchar_t* value);
    [[nodiscard]] std::wstring render(UiLanguage language = currentUiLanguage()) const;
};

[[nodiscard]] std::wstring formatText(TextId id, std::initializer_list<UiMessage::Argument> values);
[[nodiscard]] std::wstring profileDisplayName(std::string_view name, bool builtIn);
[[nodiscard]] int localizedMessageBox(HWND owner, const UiMessage& message,
    const UiMessage& title, UINT flags);

}
