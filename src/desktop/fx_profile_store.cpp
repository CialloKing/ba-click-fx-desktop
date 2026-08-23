#include "fx_profile_store.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <utility>

namespace bafx::desktop
{
namespace
{

constexpr std::string_view customProfileLabel = "自定义";
constexpr std::size_t maximumProfileDocumentBytes = 512U * 1024U;

[[nodiscard]] std::wstring utf8ToWide(const std::string_view value)
{
    if (value.empty()
        || value.size() > static_cast<std::size_t>(
            (std::numeric_limits<int>::max)()))
    {
        return {};
    }
    const int byteCount = static_cast<int>(value.size());
    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        byteCount,
        nullptr,
        0);
    if (required <= 0)
    {
        return {};
    }
    std::wstring converted(static_cast<std::size_t>(required), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        byteCount,
        converted.data(),
        required);
    return written == required ? converted : std::wstring{};
}

[[nodiscard]] std::string wideToUtf8(const std::wstring_view value)
{
    if (value.empty()
        || value.size() > static_cast<std::size_t>(
            (std::numeric_limits<int>::max)()))
    {
        return {};
    }
    const int characterCount = static_cast<int>(value.size());
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        characterCount,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0)
    {
        return {};
    }
    std::string converted(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        characterCount,
        converted.data(),
        required,
        nullptr,
        nullptr);
    return written == required ? converted : std::string{};
}

[[nodiscard]] bool profileNamesEqual(
    const std::string_view left,
    const std::string_view right) noexcept
{
    if (left == right)
    {
        return true;
    }
    try
    {
        const std::wstring wideLeft = utf8ToWide(left);
        const std::wstring wideRight = utf8ToWide(right);
        if (wideLeft.empty() || wideRight.empty())
        {
            return false;
        }
        return CompareStringOrdinal(
                   wideLeft.data(),
                   static_cast<int>(wideLeft.size()),
                   wideRight.data(),
                   static_cast<int>(wideRight.size()),
                   TRUE)
            == CSTR_EQUAL;
    }
    catch (...)
    {
        return false;
    }
}

[[nodiscard]] bool builtInName(const std::string_view name) noexcept
{
    static constexpr std::array names{
        std::string_view{"Unity 原版"},
        std::string_view{"轻量"},
        std::string_view{"纯点击"},
        std::string_view{"纯拖尾"}};
    return std::ranges::any_of(
        names,
        [name](const std::string_view builtIn)
        {
            return profileNamesEqual(name, builtIn);
        });
}

[[nodiscard]] bool hasJsonExtension(
    const std::filesystem::path& path)
{
    const std::wstring extension = path.extension().wstring();
    return CompareStringOrdinal(
               extension.c_str(),
               -1,
               L".json",
               -1,
               TRUE)
        == CSTR_EQUAL;
}

[[nodiscard]] bool reservedDosName(const std::wstring_view name)
{
    const std::size_t dot = name.find(L'.');
    std::wstring base(name.substr(0U, dot));
    std::ranges::transform(
        base,
        base.begin(),
        [](const wchar_t value)
        {
            return static_cast<wchar_t>(std::towupper(value));
        });
    if (base == L"CON"
        || base == L"PRN"
        || base == L"AUX"
        || base == L"NUL")
    {
        return true;
    }
    if (base.size() == 4U
        && (base.starts_with(L"COM") || base.starts_with(L"LPT"))
        && base[3] >= L'1'
        && base[3] <= L'9')
    {
        return true;
    }
    return false;
}

[[nodiscard]] bool effectsValid(
    const bafx::config::EffectsConfig& effects,
    std::string& error)
{
    bafx::config::Config candidate = bafx::config::defaultConfig();
    candidate.effects = effects;
    return bafx::config::validateConfig(candidate, &error);
}

[[nodiscard]] bool effectsEqual(
    const bafx::config::EffectsConfig& left,
    const bafx::config::EffectsConfig& right)
{
    return bafx::config::toJson(left, false)
        == bafx::config::toJson(right, false);
}

[[nodiscard]] FxProfileStoreResult failure(
    const FxProfileStoreStatus status,
    std::string message)
{
    return FxProfileStoreResult{status, std::move(message)};
}

}

FxProfileStore::FxProfileStore(
    std::filesystem::path directory,
    const FxProfileStoreReadProbe readProbe)
    : directory_(std::move(directory))
    , readProbe_(readProbe)
{
    addBuiltInProfiles();
    loadCustomProfiles();
}

const std::vector<FxProfile>& FxProfileStore::profiles() const noexcept
{
    return profiles_;
}

std::vector<FxProfileSummary> FxProfileStore::summaries() const
{
    if (readProbe_.summariesMaterialized != nullptr)
    {
        readProbe_.summariesMaterialized(readProbe_.context);
    }
    std::vector<FxProfileSummary> result;
    result.reserve(profiles_.size());
    for (const FxProfile& profile : profiles_)
    {
        result.push_back(FxProfileSummary{profile.name, profile.builtIn});
    }
    return result;
}

const FxProfile* FxProfileStore::find(const std::string_view name) const noexcept
{
    const auto profile = std::ranges::find_if(
        profiles_,
        [name](const FxProfile& candidate)
        {
            return profileNamesEqual(candidate.name, name);
        });
    return profile == profiles_.end() ? nullptr : &*profile;
}

std::string FxProfileStore::activeProfileName(
    const bafx::config::EffectsConfig& effects) const
{
    if (readProbe_.activeProfileResolved != nullptr)
    {
        readProbe_.activeProfileResolved(readProbe_.context);
    }
    const std::string current = bafx::config::toJson(effects, false);
    for (const FxProfile& profile : profiles_)
    {
        if (bafx::config::toJson(profile.effects, false) == current)
        {
            return profile.name;
        }
    }
    return std::string(customProfileLabel);
}

const std::string& FxProfileStore::loadWarning() const noexcept
{
    return loadWarning_;
}

FxProfileStoreResult FxProfileStore::save(
    const std::string_view name,
    const bafx::config::EffectsConfig& effects) noexcept
{
    try
    {
        std::string error;
        if (!validCustomName(name, &error))
        {
            return failure(FxProfileStoreStatus::InvalidName, std::move(error));
        }
        if (!effectsValid(effects, error))
        {
            return failure(
                FxProfileStoreStatus::InvalidEffects,
                error.empty() ? "effects profile is invalid" : std::move(error));
        }

        const FxProfile* existing = find(name);
        for (const FxProfile& profile : profiles_)
        {
            if (&profile != existing && effectsEqual(profile.effects, effects))
            {
                return failure(
                    FxProfileStoreStatus::DuplicateEffects,
                    "effects already match profile: " + profile.name);
            }
        }
        if (existing == nullptr
            && profiles_.size() >= 4U + maximumCustomFxProfiles)
        {
            return failure(
                FxProfileStoreStatus::TooManyProfiles,
                "custom effects profile limit reached");
        }

        const std::string storedName = existing == nullptr
            ? std::string(name)
            : existing->name;
        std::vector<FxProfile> candidateProfiles = profiles_;
        if (existing == nullptr)
        {
            candidateProfiles.push_back(FxProfile{storedName, false, effects});
            std::sort(
                candidateProfiles.begin() + 4,
                candidateProfiles.end(),
                [](const FxProfile& left, const FxProfile& right)
                {
                    return left.name < right.name;
                });
        }
        else
        {
            const std::size_t existingIndex = static_cast<std::size_t>(
                existing - profiles_.data());
            candidateProfiles[existingIndex].effects = effects;
        }

        std::error_code directoryError;
        std::filesystem::create_directories(directory_, directoryError);
        if (directoryError)
        {
            return failure(
                FxProfileStoreStatus::IoError,
                "effects profile directory could not be created");
        }

        // Preserve the first spelling of a profile name. Windows resolves
        // Foo.json and foo.json to the same file, so publishing a second
        // in-memory entry would make disk and catalog state disagree.
        const std::filesystem::path target = customPath(storedName);
        const std::filesystem::path temporary = target.wstring() + L".tmp";
        {
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::trunc);
            if (!output)
            {
                return failure(
                    FxProfileStoreStatus::IoError,
                    "effects profile temporary file could not be opened");
            }
            output << bafx::config::toJson(effects, true);
            output.flush();
            if (!output)
            {
                output.close();
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return failure(
                    FxProfileStoreStatus::IoError,
                    "effects profile temporary file could not be written");
            }
        }
        if (MoveFileExW(
                temporary.c_str(),
                target.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE)
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return failure(
                FxProfileStoreStatus::IoError,
                "effects profile could not be replaced atomically");
        }

        // All allocating work completed before the file commit. swap() is the
        // no-throw publication step after MoveFileExW succeeds.
        profiles_.swap(candidateProfiles);
        return FxProfileStoreResult{};
    }
    catch (...)
    {
        return failure(
            FxProfileStoreStatus::IoError,
            "effects profile save failed");
    }
}

FxProfileStoreResult FxProfileStore::remove(
    const std::string_view name) noexcept
{
    try
    {
        if (builtInName(name))
        {
            return failure(
                FxProfileStoreStatus::BuiltIn,
                "built-in effects profiles cannot be deleted");
        }
        std::string error;
        if (!validCustomName(name, &error))
        {
            return failure(FxProfileStoreStatus::InvalidName, std::move(error));
        }
        auto profile = std::ranges::find_if(
            profiles_,
            [name](const FxProfile& candidate)
            {
                return profileNamesEqual(candidate.name, name);
            });
        if (profile == profiles_.end() || profile->builtIn)
        {
            return failure(
                FxProfileStoreStatus::NotFound,
                "effects profile does not exist");
        }

        const std::string storedName = profile->name;
        const std::size_t profileIndex = static_cast<std::size_t>(
            profile - profiles_.begin());
        std::vector<FxProfile> candidateProfiles = profiles_;
        candidateProfiles.erase(candidateProfiles.begin() + profileIndex);

        std::error_code removeError;
        const bool removed = std::filesystem::remove(
            customPath(storedName),
            removeError);
        if (removeError || !removed)
        {
            return failure(
                FxProfileStoreStatus::IoError,
                "effects profile file could not be deleted");
        }
        profiles_.swap(candidateProfiles);
        return FxProfileStoreResult{};
    }
    catch (...)
    {
        return failure(
            FxProfileStoreStatus::IoError,
            "effects profile deletion failed");
    }
}

bool FxProfileStore::validCustomName(
    const std::string_view name,
    std::string* const error) noexcept
{
    const auto reject = [error](const std::string_view message)
    {
        if (error != nullptr)
        {
            *error = message;
        }
        return false;
    };
    if (name.empty() || name.size() > maximumFxProfileNameBytes)
    {
        return reject("effects profile name must contain 1 to 160 UTF-8 bytes");
    }
    if (name == customProfileLabel || builtInName(name))
    {
        return reject("effects profile name is reserved");
    }
    const std::wstring wide = utf8ToWide(name);
    if (wide.empty() || wide.size() > maximumFxProfileNameCharacters)
    {
        return reject("effects profile name must be valid UTF-8 and at most 40 characters");
    }
    if (wide.front() == L' '
        || wide.back() == L' '
        || wide.back() == L'.')
    {
        return reject("effects profile name cannot start or end with space or dot");
    }
    static constexpr std::wstring_view invalid = L"<>:\"/\\|?*";
    for (const wchar_t character : wide)
    {
        if (character < 0x20 || invalid.find(character) != std::wstring::npos)
        {
            return reject("effects profile name contains a reserved character");
        }
    }
    if (wide == L"." || wide == L".." || reservedDosName(wide))
    {
        return reject("effects profile name is reserved by Windows");
    }
    if (error != nullptr)
    {
        error->clear();
    }
    return true;
}

void FxProfileStore::addBuiltInProfiles()
{
    const bafx::config::EffectsConfig unity =
        bafx::config::defaultConfig().effects;

    bafx::config::EffectsConfig light = unity;
    light.clickShardsLayerEnabled = false;
    light.trailShardsLayerEnabled = false;
    light.bloomLayerEnabled = false;

    bafx::config::EffectsConfig click = unity;
    click.trailEnabled = false;
    click.trailShardsLayerEnabled = false;
    click.trailLayerEnabled = false;

    bafx::config::EffectsConfig trail = unity;
    trail.clickEnabled = false;
    trail.diskLayerEnabled = false;
    trail.ringsLayerEnabled = false;
    trail.clickShardsLayerEnabled = false;

    profiles_.push_back(FxProfile{"Unity 原版", true, unity});
    profiles_.push_back(FxProfile{"轻量", true, light});
    profiles_.push_back(FxProfile{"纯点击", true, click});
    profiles_.push_back(FxProfile{"纯拖尾", true, trail});
}

void FxProfileStore::loadCustomProfiles() noexcept
{
    try
    {
        std::error_code iteratorError;
        if (!std::filesystem::exists(directory_, iteratorError))
        {
            if (iteratorError)
            {
                loadWarning_ = "effects profile directory could not be read";
            }
            return;
        }

        std::vector<std::filesystem::path> documents;
        for (std::filesystem::directory_iterator iterator(
                 directory_,
                 iteratorError), end;
             iterator != end && !iteratorError;
             iterator.increment(iteratorError))
        {
            std::error_code typeError;
            const bool regular = iterator->is_regular_file(typeError);
            if (typeError)
            {
                loadWarning_ = "one or more effects profile files could not be inspected";
                continue;
            }
            if (!regular || !hasJsonExtension(iterator->path()))
            {
                continue;
            }
            documents.push_back(iterator->path());
        }
        if (iteratorError)
        {
            loadWarning_ = "effects profile directory enumeration failed";
        }

        // directory_iterator order is unspecified. A stable order makes the
        // surviving identity deterministic when manually copied files have
        // conflicting names or duplicate effects.
        std::sort(
            documents.begin(),
            documents.end(),
            [](const std::filesystem::path& left,
               const std::filesystem::path& right)
            {
                return left.filename().wstring() < right.filename().wstring();
            });

        for (const std::filesystem::path& document : documents)
        {
            const std::string name = wideToUtf8(document.stem().wstring());
            if (!validCustomName(name))
            {
                loadWarning_ = "one or more effects profile names were invalid or reserved";
                continue;
            }
            if (profiles_.size() >= 4U + maximumCustomFxProfiles)
            {
                loadWarning_ = "one or more effects profiles exceeded the profile limit";
                continue;
            }
            std::ifstream input(document, std::ios::binary);
            if (!input)
            {
                loadWarning_ = "one or more effects profiles could not be opened";
                continue;
            }
            input.seekg(0, std::ios::end);
            const std::streamoff length = input.tellg();
            if (length < 0
                || static_cast<std::uint64_t>(length)
                    > maximumProfileDocumentBytes)
            {
                loadWarning_ = "one or more effects profiles exceeded the size limit";
                continue;
            }
            input.seekg(0, std::ios::beg);
            const std::string json{
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
            const bafx::config::EffectsConfigParseResult parsed =
                bafx::config::parseEffectsJson(json);
            if (!parsed.succeeded())
            {
                loadWarning_ = "one or more effects profiles were invalid";
                continue;
            }
            if (find(name) != nullptr)
            {
                loadWarning_ = "one or more effects profile names conflicted";
                continue;
            }
            const bool duplicateEffects = std::ranges::any_of(
                profiles_,
                [&parsed](const FxProfile& profile)
                {
                    return effectsEqual(profile.effects, *parsed.config);
                });
            if (duplicateEffects)
            {
                loadWarning_ = "one or more effects profiles duplicated existing values";
                continue;
            }
            profiles_.push_back(FxProfile{name, false, *parsed.config});
        }
        sortCustomProfiles();
    }
    catch (...)
    {
        loadWarning_ = "effects profile loading failed";
    }
}

void FxProfileStore::sortCustomProfiles()
{
    if (profiles_.size() <= 4U)
    {
        return;
    }
    std::sort(
        profiles_.begin() + 4,
        profiles_.end(),
        [](const FxProfile& left, const FxProfile& right)
        {
            return left.name < right.name;
        });
}

std::filesystem::path FxProfileStore::customPath(
    const std::string_view name) const
{
    return directory_ / (utf8ToWide(name) + L".json");
}

}
