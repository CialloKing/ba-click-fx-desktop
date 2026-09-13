#include "test_support.hpp"
#include "localization.hpp"
#include "language_preferences.hpp"

#include <fstream>

using namespace bafx::control_center;

BAFX_TEST(ui_language_resolution_and_catalog)
{
    BAFX_CHECK(parseUiLanguage(" en-US\r\n") == UiLanguage::English);
    BAFX_CHECK(parseUiLanguage("zh-CN") == UiLanguage::SimplifiedChinese);
    BAFX_CHECK(parseUiLanguage("invalid") == UiLanguage::System);
    BAFX_CHECK(resolveUiLanguage(UiLanguage::System, 0x0404U) == UiLanguage::SimplifiedChinese);
    BAFX_CHECK(resolveUiLanguage(UiLanguage::System, 0x040CU) == UiLanguage::English);
    BAFX_CHECK(resolveUiLanguage(UiLanguage::English, 0x0804U) == UiLanguage::English);
    for (std::size_t index = 0U; index < static_cast<std::size_t>(TextId::Count); ++index)
    {
        const auto id = static_cast<TextId>(index);
        const std::wstring_view english = translatedText(id, UiLanguage::English);
        const std::wstring_view chinese = translatedText(id, UiLanguage::SimplifiedChinese);
        BAFX_CHECK(!english.empty() && !chinese.empty());
        for (int argument = 0; argument < 10; ++argument)
        {
            const auto placeholder = L"{" + std::to_wstring(argument) + L"}";
            BAFX_CHECK((english.find(placeholder) != english.npos) == (chinese.find(placeholder) != chinese.npos));
        }
    }
    const UiMessage message(TextId::LanguageSaveFailed, {std::wstring(L"5")});
    BAFX_CHECK(message.render(UiLanguage::English).find(L"Win32: 5") != std::wstring::npos);
    BAFX_CHECK(message.render(UiLanguage::SimplifiedChinese).find(L"Win32：5") != std::wstring::npos);
    // User names, including names that resemble a translated built-in, stay opaque.
    setUiLanguage(UiLanguage::English);
    BAFX_CHECK(profileDisplayName("Unity 原版", true) == L"Unity Original");
    BAFX_CHECK(profileDisplayName("Unity 原版", false) == L"Unity 原版");
    BAFX_CHECK(profileDisplayName("我的预设", false) == L"我的预设");
    BAFX_CHECK(UiMessage(L"raw {0}").render() == L"raw {0}");
}

BAFX_TEST(ui_language_preference_round_trip_and_failure)
{
    const auto root = std::filesystem::temp_directory_path()
        / (L"bafx-language-" + std::to_wstring(GetCurrentProcessId()));
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } cleanup{root};
    const auto path = root / L"界面.language";
    BAFX_CHECK(loadLanguagePreference(path) == UiLanguage::System);
    for (const auto language : {UiLanguage::English, UiLanguage::SimplifiedChinese, UiLanguage::System})
    {
        BAFX_CHECK(saveLanguagePreference(path, language) == ERROR_SUCCESS);
        BAFX_CHECK(loadLanguagePreference(path) == language);
    }
    BAFX_CHECK(saveLanguagePreference(path, UiLanguage::English) == ERROR_SUCCESS);
    const HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    BAFX_CHECK(locked != INVALID_HANDLE_VALUE);
    const DWORD failure = saveLanguagePreference(path, UiLanguage::SimplifiedChinese);
    CloseHandle(locked);
    BAFX_CHECK(failure != ERROR_SUCCESS);
    BAFX_CHECK(loadLanguagePreference(path) == UiLanguage::English);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << "invalid";
    BAFX_CHECK(loadLanguagePreference(path) == UiLanguage::System);
    BAFX_CHECK(std::filesystem::file_size(path) == 7U);
    BAFX_CHECK(languagePreferencePath(root) == root / L"BAFX.ControlCenter.language");
}
