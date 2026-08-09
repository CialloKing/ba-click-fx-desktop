#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace bafx::config
{

inline constexpr std::uint32_t currentSchemaVersion = 3U;

enum class CaptureMode : std::uint8_t
{
    FxOnly,
    BackgroundAware,
    RecordingCompatible
};

enum class BloomQuality : std::uint8_t
{
    Low,
    Medium,
    High,
    Ultra
};

enum class FramePacing : std::uint8_t
{
    MatchDisplay,
    Fixed60,
    Fixed120,
    Fixed144
};

struct EffectsConfig
{
    bool enabled{true};
    float globalScale{1.0F};
    bool clickEnabled{true};
    bool trailEnabled{true};
    float trailLength{1.0F};
    float trailWidth{1.0F};
    float bloomIntensity{1.0F};
    BloomQuality bloomQuality{BloomQuality::High};
};

struct BackgroundConfig
{
    CaptureMode mode{CaptureMode::BackgroundAware};
    bool cursorExcluded{true};
};

struct InputConfig
{
    bool leftClick{true};
    bool rightClick{true};
    bool middleClick{false};
    bool trailOnlyWhilePressed{false};
};

struct PerformanceConfig
{
    bool idleOptimization{true};
    FramePacing framePacing{FramePacing::MatchDisplay};
};

struct SystemConfig
{
    bool startWithWindows{false};
    bool startMinimized{false};
    bool closeToTray{true};
};

struct Config
{
    std::uint32_t schemaVersion{currentSchemaVersion};
    EffectsConfig effects{};
    BackgroundConfig background{};
    InputConfig input{};
    PerformanceConfig performance{};
    SystemConfig system{};
};

enum class ConfigStatus : std::uint8_t
{
    Ok,
    CreatedDefault,
    Migrated,
    IoError,
    ParseError,
    UnsupportedSchema,
    ValidationError,
    WriteError
};

struct ConfigLoadResult
{
    Config config{};
    ConfigStatus status{ConfigStatus::Ok};
    std::string message{};

    [[nodiscard]] bool succeeded() const noexcept
    {
        return status == ConfigStatus::Ok
            || status == ConfigStatus::CreatedDefault
            || status == ConfigStatus::Migrated;
    }

    [[nodiscard]] bool migrated() const noexcept
    {
        return status == ConfigStatus::Migrated;
    }
};

struct ConfigPatchResult
{
    Config config{};
    ConfigStatus status{ConfigStatus::ParseError};
    std::string message{};
    bool recognized{false};
    std::optional<std::uint64_t> expectedGeneration{};

    [[nodiscard]] bool succeeded() const noexcept
    {
        return recognized && status == ConfigStatus::Ok;
    }
};

struct ConfigSaveResult
{
    ConfigStatus status{ConfigStatus::Ok};
    std::string message{};

    [[nodiscard]] bool succeeded() const noexcept
    {
        return status == ConfigStatus::Ok;
    }
};

[[nodiscard]] Config defaultConfig() noexcept;

[[nodiscard]] ConfigLoadResult parseJson(std::string_view json) noexcept;

// Parses a single {"path": ..., "value": ...} product-level update. A
// missing path makes recognized=false so callers can fall back to a full
// configuration document without accepting malformed patch objects.
[[nodiscard]] ConfigPatchResult applyPatchJson(
    const Config& base,
    std::string_view json) noexcept;

[[nodiscard]] std::string toJson(
    const Config& config,
    bool pretty = true);

[[nodiscard]] ConfigLoadResult loadConfig(
    const std::filesystem::path& path) noexcept;

[[nodiscard]] ConfigSaveResult saveConfigAtomic(
    const std::filesystem::path& path,
    const Config& config) noexcept;

[[nodiscard]] bool validateConfig(
    const Config& config,
    std::string* error = nullptr) noexcept;

[[nodiscard]] std::string_view toString(CaptureMode mode) noexcept;
[[nodiscard]] std::string_view toString(BloomQuality quality) noexcept;
[[nodiscard]] std::string_view toString(FramePacing pacing) noexcept;

// Maps product quality choices onto the existing Unity Bloom diffusion range.
// High deliberately remains at the extracted game's verified diffusion of 7.
[[nodiscard]] float bloomDiffusionForQuality(BloomQuality quality) noexcept;

}
