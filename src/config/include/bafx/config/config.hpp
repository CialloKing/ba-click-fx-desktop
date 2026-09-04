#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bafx::config
{

inline constexpr std::uint32_t currentSchemaVersion = 20U;
inline constexpr std::size_t maximumDisplayOverrides = 64U;
inline constexpr std::size_t maximumDisplayKeyBytes = 4096U;

enum class RenderMode : std::uint8_t
{
    BackgroundAware,
    RecordingCompatible,
    LightBackground
};

enum class BloomQuality : std::uint8_t
{
    Low,
    Medium,
    High,
    Ultra,
    Custom
};

enum class FramePacing : std::uint8_t
{
    MatchDisplay,
    Fixed60,
    Fixed120,
    Fixed144,
    Unlimited
};

// Core keeps the authored click, ring, shard, and trail effects while omitting
// Bloom and background transport. It is a separate performance axis so the
// Unity-faithful Full profile remains the default.
enum class EffectsMode : std::uint8_t
{
    Full,
    Core
};

struct DisplayOverrideConfig final
{
    // The Host derives this opaque key only from authoritative DisplayConfig
    // target paths. GDI names and HMONITOR values are deliberately excluded
    // because they are not stable across boots or topology changes.
    std::string displayKey{};
    bool enabled{true};
    bool hdrEnabled{false};
    FramePacing framePacing{FramePacing::MatchDisplay};
};

struct EffectsConfig
{
    bool enabled{true};
    // Layer switches only affect presentation. The simulation keeps authored
    // state alive so a temporarily hidden layer can resume within its lifetime.
    bool diskLayerEnabled{true};
    bool ringsLayerEnabled{true};
    bool clickShardsLayerEnabled{true};
    bool trailShardsLayerEnabled{true};
    bool trailLayerEnabled{true};
    bool bloomLayerEnabled{true};
    // User-authored sRGB theme color. The renderer maps the Unity palette
    // relative to this value while preserving the default identity path.
    std::string themeColor{"#4ca7ff"};
    float globalScale{1.0F};
    // Opacity is applied to the final FX payload while Unity-authored linear
    // RGB and Bloom emission remain unchanged.
    float opacity{1.0F};
    bool clickEnabled{true};
    bool trailEnabled{true};
    float trailLength{1.0F};
    float trailWidth{1.0F};
    // These independent animation speed controls preserve the extracted
    // Unity timeline exactly when set to one.
    float clickTimeScale{1.0F};
    float trailTimeScale{1.0F};
    // Lifetime is the canonical duration; trailLength is its normalized
    // runtime multiplier and remains synchronized by the configuration API.
    float trailLifetimeMs{300.0F};
    // Spatial values use reference pixels at the 1920x1080 viewport.
    float diskLifetimeMs{200.0F};
    float diskRadius{64.8F};
    std::uint32_t ringsCount{2U};
    float ringsLifetimeMs{600.0F};
    float ringsRadiusMin{68.92571232F};
    float ringsRadiusMax{80.41333104F};
    float ringsAngularVelocityMultiplier{11.170107F};
    float ringsRotationDirection{-1.0F};
    float ringsHdrIntensity{5.992157F};
    float shardsHdrIntensity{5.992157F};
    // Click shard values use 1920x1080 reference-pixel units. Linked Min/Max
    // endpoints may cross; interpolation remains valid in either order and
    // the simulation converts units at its boundary.
    std::uint32_t shardsClickCount{4U};
    float shardsClickLifetimeMinMs{600.0F};
    float shardsClickLifetimeMaxMs{700.0F};
    float shardsClickRadius{49.8769488F};
    float shardsClickSpeedMin{49.8769488F};
    float shardsClickSpeedMax{66.5025984F};
    float shardsSizeMin{16.6256496F};
    float shardsSizeMax{33.2512992F};
    float trailOpacity{1.0F};
    // Unity's serialized Bloom intensity default is 1.7.
    float bloomIntensity{1.7F};
    float bloomDiffusion{7.0F};
    float bloomThreshold{1.0F};
    float bloomSoftKnee{0.0F};
    float bloomClamp{65472.0F};
};

struct BackgroundConfig
{
    // Prefer the background-aware path for new profiles. The host still falls
    // back to FX-only when WGC or capture exclusion cannot be established.
    RenderMode mode{RenderMode::BackgroundAware};
    bool cursorExcluded{true};
    // Keep background-aware rendering usable on systems that require the WGC
    // privacy indicator. Users can still request borderless-only capture.
    bool allowSystemBorder{true};
};

struct DisplayConfig
{
    // Unity-authored color and Bloom always remain linear HDR internally.
    // This flag only opts the final desktop transport into scRGB output.
    bool hdrEnabled{false};
    // A display without a matching entry inherits the global HDR request and
    // performance.framePacing. Entries are complete policies so a partially
    // written override can never silently inherit a different field.
    std::vector<DisplayOverrideConfig> overrides{};
};

struct InputConfig
{
    bool leftClick{true};
    bool rightClick{true};
    bool middleClick{false};
    bool trailOnlyWhilePressed{true};
    // Zero preserves every move sample. Positive values cap accepted input
    // samples in Hz without changing Unity's spatial vertex threshold.
    std::uint32_t samplingRateHz{0U};
};

struct PerformanceConfig
{
    bool idleOptimization{true};
    // Active-FX ROI is opt-in until its constrained production path has been
    // accepted across the supported hardware matrix.
    bool activeFxRoiEnabled{false};
    FramePacing framePacing{FramePacing::MatchDisplay};
    EffectsMode effectsMode{EffectsMode::Full};
};

struct SystemConfig
{
    bool startWithWindows{false};
    bool startMinimized{false};
    bool closeToTray{true};
    // Keep Spout2 opt-in so an upgrade never creates a cross-process sender
    // until the user explicitly enables OBS output.
    bool spout2Enabled{false};
};

enum class HotkeyAction : std::uint8_t
{
    TogglePause,
    ToggleAlwaysOnTrail,
    NextFxProfile,
    Shutdown
};

inline constexpr std::size_t hotkeyActionCount = 4U;
inline constexpr std::array<std::string_view, hotkeyActionCount> hotkeyActionNames{
    "togglePause", "toggleAlwaysOnTrail", "nextFxProfile", "shutdown"};

struct HotkeyBinding final
{
    // Stable modifier bits: alt=1, ctrl=2, shift=4, win=8. NOREPEAT is
    // runtime policy, not a configurable modifier or part of key identity.
    std::uint32_t modifiers{0U};
    std::uint32_t key{0U};
    bool operator==(const HotkeyBinding&) const = default;
};

struct HotkeysConfig final
{
    std::array<std::optional<HotkeyBinding>, hotkeyActionCount> bindings{};
    bool operator==(const HotkeysConfig&) const = default;
};

struct Config
{
    std::uint32_t schemaVersion{currentSchemaVersion};
    EffectsConfig effects{};
    BackgroundConfig background{};
    DisplayConfig display{};
    InputConfig input{};
    PerformanceConfig performance{};
    SystemConfig system{};
    HotkeysConfig hotkeys{};
};

struct ResolvedDisplayPolicy final
{
    bool enabled{true};
    bool hdrEnabled{false};
    FramePacing framePacing{FramePacing::MatchDisplay};
    bool overridden{false};
};

enum class ConfigStatus : std::uint8_t
{
    Ok,
    CreatedDefault,
    IoError,
    ParseError,
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
            || status == ConfigStatus::CreatedDefault;
    }
};

struct EffectsConfigParseResult
{
    // An optional prevents callers from accidentally consuming the default-
    // initialized prefix of an effects object that failed strict parsing.
    std::optional<EffectsConfig> config{};
    ConfigStatus status{ConfigStatus::ParseError};
    std::string message{};

    [[nodiscard]] bool succeeded() const noexcept
    {
        return status == ConfigStatus::Ok && config.has_value();
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

struct ConfigBatchPatchResult
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

[[nodiscard]] bool validHotkeyKey(std::uint32_t key) noexcept;
[[nodiscard]] bool validateHotkeys(
    const HotkeysConfig& hotkeys, std::string* error = nullptr) noexcept;
[[nodiscard]] std::string toJson(const HotkeysConfig& hotkeys);
[[nodiscard]] std::optional<HotkeysConfig> parseHotkeysJson(
    std::string_view json, std::string* error = nullptr) noexcept;

// The effects-only codec owns the exact flat object returned by GetFxConfig.
// It deliberately excludes product settings outside EffectsConfig.
[[nodiscard]] std::string toJson(
    const EffectsConfig& config,
    bool pretty = false);
[[nodiscard]] EffectsConfigParseResult parseEffectsJson(
    std::string_view json) noexcept;

// Parses a single {"path": ..., "value": ...} product-level update. A
// missing path makes recognized=false so callers can fall back to a full
// configuration document without accepting malformed patch objects.
[[nodiscard]] ConfigPatchResult applyPatchJson(
    const Config& base,
    std::string_view json) noexcept;

// Applies one display override by stable key. These commands intentionally do
// not expose vector indices because topology refreshes can reorder sessions.
[[nodiscard]] ConfigPatchResult applyDisplayOverrideJson(
    const Config& base,
    std::string_view json) noexcept;
[[nodiscard]] ConfigPatchResult removeDisplayOverrideJson(
    const Config& base,
    std::string_view json) noexcept;

// Applies one IPC-style patch through the canonical FX path surface. Product
// settings such as input, display, background, and system remain SetConfig-only.
[[nodiscard]] ConfigPatchResult applyFxPatchJson(
    const Config& base,
    std::string_view json) noexcept;

// Applies an object of canonical native dot paths atomically. Every value is
// validated against the same single-patch contract before the result changes.
[[nodiscard]] ConfigBatchPatchResult applyPatchBatchJson(
    const Config& base,
    std::string_view json) noexcept;

// These helpers expose the native EffectsConfig contract to IPC and
// non-Windows callers. Parameter paths always use the effects.* namespace.
[[nodiscard]] ConfigPatchResult setFxParam(
    const Config& base,
    std::string_view path,
    std::string_view valueJson) noexcept;
[[nodiscard]] ConfigBatchPatchResult setFxParams(
    const Config& base,
    std::string_view patchJson) noexcept;
[[nodiscard]] std::string getFxConfig(
    const Config& config,
    bool pretty = false);
[[nodiscard]] Config resetFxConfig() noexcept;

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

[[nodiscard]] const DisplayOverrideConfig* findDisplayOverride(
    const DisplayConfig& display,
    std::string_view displayKey) noexcept;
[[nodiscard]] ResolvedDisplayPolicy resolveDisplayPolicy(
    const Config& config,
    std::string_view displayKey) noexcept;
[[nodiscard]] bool setDisplayOverride(
    Config& config,
    DisplayOverrideConfig overrideConfig,
    std::string* error = nullptr) noexcept;
[[nodiscard]] bool removeDisplayOverride(
    Config& config,
    std::string_view displayKey) noexcept;

[[nodiscard]] std::string_view toString(RenderMode mode) noexcept;
[[nodiscard]] std::string_view toString(BloomQuality quality) noexcept;
[[nodiscard]] std::string_view toString(FramePacing pacing) noexcept;
[[nodiscard]] std::string_view toString(EffectsMode mode) noexcept;

// Maps product quality choices onto the existing Unity Bloom diffusion range.
// High deliberately remains at the extracted game's verified diffusion of 7.
[[nodiscard]] float bloomDiffusionForQuality(BloomQuality quality) noexcept;
// The continuous diffusion value is the source of truth. Product presets are
// a derived Control Center view, with Custom representing every other value.
[[nodiscard]] BloomQuality bloomQualityForDiffusion(float diffusion) noexcept;

}
