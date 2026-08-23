#pragma once

#include "bafx/config/config.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bafx::desktop
{

inline constexpr std::size_t maximumCustomFxProfiles = 64U;
inline constexpr std::size_t maximumFxProfileNameBytes = 160U;
inline constexpr std::size_t maximumFxProfileNameCharacters = 40U;

struct FxProfile final
{
    std::string name{};
    bool builtIn{false};
    bafx::config::EffectsConfig effects{};
};

struct FxProfileSummary final
{
    std::string name{};
    bool builtIn{false};
};

enum class FxProfileStoreStatus
{
    Ok,
    InvalidName,
    BuiltIn,
    NotFound,
    TooManyProfiles,
    DuplicateEffects,
    InvalidEffects,
    IoError
};

struct FxProfileStoreResult final
{
    FxProfileStoreStatus status{FxProfileStoreStatus::Ok};
    std::string message{};

    [[nodiscard]] bool succeeded() const noexcept
    {
        return status == FxProfileStoreStatus::Ok;
    }
};

// This optional probe proves the cold Profile-state materialization boundary;
// HostControlPlane::runtimeSnapshot neither checks nor invokes it. Host
// callbacks run while its control mutex is held and must not re-enter it.
struct FxProfileStoreReadProbe final
{
    using Notify = void (*)(void* context) noexcept;

    void* context{nullptr};
    Notify summariesMaterialized{nullptr};
    Notify activeProfileResolved{nullptr};
};

// One effects-only JSON document per profile keeps corruption and atomic
// replacement local to that profile. The Host is the sole writer.
class FxProfileStore final
{
public:
    explicit FxProfileStore(
        std::filesystem::path directory,
        FxProfileStoreReadProbe readProbe = {});

    [[nodiscard]] const std::vector<FxProfile>& profiles() const noexcept;
    [[nodiscard]] std::vector<FxProfileSummary> summaries() const;
    [[nodiscard]] const FxProfile* find(std::string_view name) const noexcept;
    [[nodiscard]] std::string activeProfileName(
        const bafx::config::EffectsConfig& effects) const;
    [[nodiscard]] const std::string& loadWarning() const noexcept;

    [[nodiscard]] FxProfileStoreResult save(
        std::string_view name,
        const bafx::config::EffectsConfig& effects) noexcept;
    [[nodiscard]] FxProfileStoreResult remove(
        std::string_view name) noexcept;

    [[nodiscard]] static bool validCustomName(
        std::string_view name,
        std::string* error = nullptr) noexcept;

private:
    void addBuiltInProfiles();
    void loadCustomProfiles() noexcept;
    void sortCustomProfiles();
    [[nodiscard]] std::filesystem::path customPath(
        std::string_view name) const;

    std::filesystem::path directory_{};
    std::vector<FxProfile> profiles_{};
    std::string loadWarning_{};
    FxProfileStoreReadProbe readProbe_{};
};

}
