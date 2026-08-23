#include "test_support.hpp"

#include "fx_profile_store.hpp"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{

class TemporaryFxProfileDirectory final
{
public:
    TemporaryFxProfileDirectory()
    {
        const auto nonce = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = std::filesystem::temp_directory_path()
            / (L"bafx-fx-profile-tests-"
               + std::to_wstring(GetCurrentProcessId())
               + L"-"
               + std::to_wstring(nonce));
    }

    ~TemporaryFxProfileDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_{};
};

void writeEffectsProfile(
    const std::filesystem::path& path,
    const bafx::config::EffectsConfig& effects)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bafx::config::toJson(effects, true);
}

}

BAFX_TEST(fx_profile_store_round_trips_unicode_effects_only_documents)
{
    const TemporaryFxProfileDirectory temporary;
    bafx::desktop::FxProfileStore store(temporary.path());
    BAFX_CHECK(store.profiles().size() == 4U);
    BAFX_CHECK(store.profiles().front().builtIn);
    BAFX_CHECK(store.activeProfileName(
        bafx::config::defaultConfig().effects) == "Unity 原版");

    bafx::config::EffectsConfig effects =
        bafx::config::defaultConfig().effects;
    effects.opacity = 0.42F;
    effects.bloomLayerEnabled = false;
    const bafx::desktop::FxProfileStoreResult saved = store.save(
        "夜间 柔和",
        effects);
    BAFX_CHECK(saved.succeeded());
    BAFX_CHECK(std::filesystem::is_regular_file(
        temporary.path() / L"夜间 柔和.json"));
    BAFX_CHECK(store.activeProfileName(effects) == "夜间 柔和");

    bafx::desktop::FxProfileStore reloaded(temporary.path());
    const bafx::desktop::FxProfile* profile = reloaded.find("夜间 柔和");
    BAFX_CHECK(profile != nullptr);
    BAFX_CHECK(!profile->builtIn);
    BAFX_CHECK(
        bafx::config::toJson(profile->effects, false)
        == bafx::config::toJson(effects, false));
    BAFX_CHECK(reloaded.loadWarning().empty());

    const bafx::desktop::FxProfileStoreResult removed = reloaded.remove(
        "夜间 柔和");
    BAFX_CHECK(removed.succeeded());
    BAFX_CHECK(reloaded.find("夜间 柔和") == nullptr);
    BAFX_CHECK(!std::filesystem::exists(
        temporary.path() / L"夜间 柔和.json"));
}

BAFX_TEST(fx_profile_store_rejects_reserved_names_and_invalid_documents)
{
    const TemporaryFxProfileDirectory temporary;
    bafx::desktop::FxProfileStore store(temporary.path());
    const bafx::config::EffectsConfig effects =
        bafx::config::defaultConfig().effects;

    BAFX_CHECK(!store.save("Unity 原版", effects).succeeded());
    const bafx::desktop::FxProfileStoreResult duplicateEffects = store.save(
        "默认副本",
        effects);
    BAFX_CHECK(!duplicateEffects.succeeded());
    BAFX_CHECK(
        duplicateEffects.status
        == bafx::desktop::FxProfileStoreStatus::DuplicateEffects);
    BAFX_CHECK(!store.save("CON", effects).succeeded());
    BAFX_CHECK(!store.save("bad/name", effects).succeeded());
    BAFX_CHECK(!store.remove("轻量").succeeded());

    std::filesystem::create_directories(temporary.path());
    {
        std::ofstream corrupt(
            temporary.path() / L"损坏.json",
            std::ios::binary | std::ios::trunc);
        corrupt << "{\"opacity\":0.5}";
    }
    bafx::desktop::FxProfileStore reloaded(temporary.path());
    BAFX_CHECK(reloaded.find("损坏") == nullptr);
    BAFX_CHECK(!reloaded.loadWarning().empty());
    BAFX_CHECK(reloaded.profiles().size() == 4U);
}

BAFX_TEST(fx_profile_store_uses_windows_case_insensitive_name_identity)
{
    const TemporaryFxProfileDirectory temporary;
    bafx::desktop::FxProfileStore store(temporary.path());
    bafx::config::EffectsConfig first =
        bafx::config::defaultConfig().effects;
    first.opacity = 0.25F;
    bafx::config::EffectsConfig replacement = first;
    replacement.opacity = 0.75F;

    BAFX_CHECK(store.save("Foo", first).succeeded());
    BAFX_CHECK(store.save("foo", replacement).succeeded());
    BAFX_CHECK(store.profiles().size() == 5U);
    const bafx::desktop::FxProfile* found = store.find("FOO");
    BAFX_CHECK(found != nullptr);
    BAFX_CHECK(found->name == "Foo");
    BAFX_CHECK_NEAR(found->effects.opacity, 0.75F, 0.00001F);
    BAFX_CHECK(std::filesystem::is_regular_file(
        temporary.path() / L"Foo.json"));
    BAFX_CHECK(!store.save("unity 原版", first).succeeded());

    bafx::desktop::FxProfileStore reloaded(temporary.path());
    BAFX_CHECK(reloaded.profiles().size() == 5U);
    found = reloaded.find("fOo");
    BAFX_CHECK(found != nullptr);
    BAFX_CHECK_NEAR(found->effects.opacity, 0.75F, 0.00001F);
    BAFX_CHECK(reloaded.remove("fOO").succeeded());
    BAFX_CHECK(reloaded.find("Foo") == nullptr);
    BAFX_CHECK(!std::filesystem::exists(
        temporary.path() / L"Foo.json"));
}

BAFX_TEST(fx_profile_store_loads_duplicate_documents_deterministically)
{
    const TemporaryFxProfileDirectory temporary;
    std::filesystem::create_directories(temporary.path());
    bafx::config::EffectsConfig effects =
        bafx::config::defaultConfig().effects;
    effects.opacity = 0.33F;

    // File creation order intentionally opposes lexical order. Loading must
    // not inherit the unspecified directory enumeration order.
    writeEffectsProfile(temporary.path() / L"Zulu.JSON", effects);
    writeEffectsProfile(temporary.path() / L"Alpha.json", effects);
    writeEffectsProfile(temporary.path() / L"Unity 原版.json", effects);

    bafx::desktop::FxProfileStore store(temporary.path());
    BAFX_CHECK(store.find("Alpha") != nullptr);
    BAFX_CHECK(store.find("Zulu") == nullptr);
    BAFX_CHECK(store.profiles().size() == 5U);
    BAFX_CHECK(store.activeProfileName(effects) == "Alpha");
    BAFX_CHECK(!store.loadWarning().empty());
}
