#include "test_support.hpp"

#include "bafx/config/config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

namespace
{

namespace fs = std::filesystem;

[[nodiscard]] fs::path testPath()
{
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path()
        / ("bafx-config-tests-" + std::to_string(ticks))
        / "settings.json";
}

[[nodiscard]] std::string readFile(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

void removeTestTree(const fs::path& path) noexcept
{
    std::error_code error;
    fs::remove_all(path.parent_path(), error);
}

}

BAFX_TEST(config_defaults_round_trip_through_versioned_json)
{
    const bafx::config::Config defaults = bafx::config::defaultConfig();
    BAFX_CHECK(defaults.schemaVersion == bafx::config::currentSchemaVersion);
    BAFX_CHECK(defaults.effects.enabled);
    BAFX_CHECK(defaults.effects.diskLayerEnabled);
    BAFX_CHECK(defaults.effects.ringsLayerEnabled);
    BAFX_CHECK(defaults.effects.clickShardsLayerEnabled);
    BAFX_CHECK(defaults.effects.trailShardsLayerEnabled);
    BAFX_CHECK(defaults.effects.trailLayerEnabled);
    BAFX_CHECK(defaults.effects.bloomLayerEnabled);
    BAFX_CHECK(defaults.effects.themeColor == "#4ca7ff");
    BAFX_CHECK(
        defaults.background.mode
        == bafx::config::RenderMode::BackgroundAware);
    BAFX_CHECK(defaults.background.cursorExcluded);
    BAFX_CHECK(defaults.background.allowSystemBorder);
    BAFX_CHECK(defaults.input.trailOnlyWhilePressed);
    BAFX_CHECK(defaults.input.samplingRateHz == 0U);
    BAFX_CHECK(
        defaults.performance.effectsMode
        == bafx::config::EffectsMode::Full);
    BAFX_CHECK(!defaults.performance.activeFxRoiEnabled);
    BAFX_CHECK(!defaults.system.spout2Enabled);
    BAFX_CHECK_NEAR(defaults.effects.globalScale, 1.0F, 0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.opacity, 1.0F, 0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.clickTimeScale, 1.0F, 0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.trailTimeScale, 1.0F, 0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.trailLifetimeMs, 300.0F, 0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.diskLifetimeMs, 200.0F, 0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.diskRadius, 64.8F, 0.00001F);
    BAFX_CHECK(defaults.effects.ringsCount == 2U);
    BAFX_CHECK_NEAR(defaults.effects.ringsLifetimeMs, 600.0F, 0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.ringsRadiusMin, 68.92571232F, 0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.ringsRadiusMax, 80.41333104F, 0.00001F);
    BAFX_CHECK_NEAR(
        defaults.effects.ringsAngularVelocityMultiplier,
        11.170107F,
        0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.ringsRotationDirection, -1.0F, 0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.ringsHdrIntensity, 5.992157F, 0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.shardsHdrIntensity, 5.992157F, 0.00001F);
    BAFX_CHECK(defaults.effects.shardsClickCount == 4U);
    BAFX_CHECK_NEAR(
        defaults.effects.shardsClickLifetimeMinMs,
        600.0F,
        0.00001F);
    BAFX_CHECK_NEAR(
        defaults.effects.shardsClickLifetimeMaxMs,
        700.0F,
        0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.shardsClickRadius, 49.8769488F, 0.00001F);
    BAFX_CHECK_NEAR(
        defaults.effects.shardsClickSpeedMin,
        49.8769488F,
        0.00001F);
    BAFX_CHECK_NEAR(
        defaults.effects.shardsClickSpeedMax,
        66.5025984F,
        0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.shardsSizeMin, 16.6256496F, 0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.shardsSizeMax, 33.2512992F, 0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.trailOpacity, 1.0F, 0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.bloomIntensity, 1.7F, 0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.bloomDiffusion, 7.0F, 0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.bloomThreshold, 1.0F, 0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.bloomSoftKnee, 0.0F, 0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.bloomClamp, 65472.0F, 0.00001F);

    const std::string document = bafx::config::toJson(defaults);
    const auto parsed = bafx::config::parseJson(document);
    BAFX_CHECK(parsed.succeeded());
    BAFX_CHECK(parsed.status == bafx::config::ConfigStatus::Ok);
    BAFX_CHECK(parsed.config.schemaVersion == defaults.schemaVersion);
    BAFX_CHECK(parsed.config.effects.themeColor == defaults.effects.themeColor);
    BAFX_CHECK(
        parsed.config.system.spout2Enabled
        == defaults.system.spout2Enabled);
    BAFX_CHECK(parsed.config.background.mode == defaults.background.mode);
    BAFX_CHECK(
        parsed.config.background.cursorExcluded
        == defaults.background.cursorExcluded);
    BAFX_CHECK(
        parsed.config.background.allowSystemBorder
        == defaults.background.allowSystemBorder);
    BAFX_CHECK(
        parsed.config.input.trailOnlyWhilePressed
        == defaults.input.trailOnlyWhilePressed);
    BAFX_CHECK(parsed.config.input.samplingRateHz == defaults.input.samplingRateHz);
    BAFX_CHECK_NEAR(
        parsed.config.effects.globalScale,
        defaults.effects.globalScale,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.opacity,
        defaults.effects.opacity,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.clickTimeScale,
        defaults.effects.clickTimeScale,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.trailTimeScale,
        defaults.effects.trailTimeScale,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.trailLifetimeMs,
        defaults.effects.trailLifetimeMs,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.diskLifetimeMs,
        defaults.effects.diskLifetimeMs,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.diskRadius,
        defaults.effects.diskRadius,
        0.00001F);
    BAFX_CHECK(parsed.config.effects.ringsCount == defaults.effects.ringsCount);
    BAFX_CHECK_NEAR(
        parsed.config.effects.ringsLifetimeMs,
        defaults.effects.ringsLifetimeMs,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.ringsRadiusMin,
        defaults.effects.ringsRadiusMin,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.ringsRadiusMax,
        defaults.effects.ringsRadiusMax,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.ringsAngularVelocityMultiplier,
        defaults.effects.ringsAngularVelocityMultiplier,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.ringsRotationDirection,
        defaults.effects.ringsRotationDirection,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.ringsHdrIntensity,
        defaults.effects.ringsHdrIntensity,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.shardsHdrIntensity,
        defaults.effects.shardsHdrIntensity,
        0.00001F);
    BAFX_CHECK(
        parsed.config.effects.shardsClickCount
        == defaults.effects.shardsClickCount);
    BAFX_CHECK_NEAR(
        parsed.config.effects.shardsClickLifetimeMinMs,
        defaults.effects.shardsClickLifetimeMinMs,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.shardsClickLifetimeMaxMs,
        defaults.effects.shardsClickLifetimeMaxMs,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.shardsClickRadius,
        defaults.effects.shardsClickRadius,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.shardsClickSpeedMin,
        defaults.effects.shardsClickSpeedMin,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.shardsClickSpeedMax,
        defaults.effects.shardsClickSpeedMax,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.shardsSizeMin,
        defaults.effects.shardsSizeMin,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.shardsSizeMax,
        defaults.effects.shardsSizeMax,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.trailOpacity,
        defaults.effects.trailOpacity,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.bloomIntensity,
        defaults.effects.bloomIntensity,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.bloomDiffusion,
        defaults.effects.bloomDiffusion,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.bloomThreshold,
        defaults.effects.bloomThreshold,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.bloomSoftKnee,
        defaults.effects.bloomSoftKnee,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.bloomClamp,
        defaults.effects.bloomClamp,
        0.00001F);
    BAFX_CHECK(defaults.display.overrides.empty());
    BAFX_CHECK(parsed.config.display.overrides.empty());
}

BAFX_TEST(config_display_overrides_resolve_complete_per_display_policies)
{
    bafx::config::Config config = bafx::config::defaultConfig();
    config.display.hdrEnabled = true;
    config.performance.framePacing = bafx::config::FramePacing::Fixed60;

    const auto inherited = bafx::config::resolveDisplayPolicy(
        config,
        "displayconfig:unconfigured");
    BAFX_CHECK(inherited.enabled);
    BAFX_CHECK(inherited.hdrEnabled);
    BAFX_CHECK(
        inherited.framePacing == bafx::config::FramePacing::Fixed60);
    BAFX_CHECK(!inherited.overridden);

    std::string error;
    BAFX_CHECK(bafx::config::setDisplayOverride(
        config,
        bafx::config::DisplayOverrideConfig{
            "displayconfig:target-b",
            false,
            false,
            bafx::config::FramePacing::Fixed144},
        &error));
    BAFX_CHECK(error.empty());
    BAFX_CHECK(bafx::config::setDisplayOverride(
        config,
        bafx::config::DisplayOverrideConfig{
            "displayconfig:target-a",
            true,
            true,
            bafx::config::FramePacing::Fixed120},
        &error));
    BAFX_CHECK(config.display.overrides.size() == 2U);
    BAFX_CHECK(
        config.display.overrides[0].displayKey
        == "displayconfig:target-a");
    BAFX_CHECK(
        config.display.overrides[1].displayKey
        == "displayconfig:target-b");

    const auto overridden = bafx::config::resolveDisplayPolicy(
        config,
        "displayconfig:target-b");
    BAFX_CHECK(!overridden.enabled);
    BAFX_CHECK(!overridden.hdrEnabled);
    BAFX_CHECK(
        overridden.framePacing == bafx::config::FramePacing::Fixed144);
    BAFX_CHECK(overridden.overridden);

    BAFX_CHECK(bafx::config::setDisplayOverride(
        config,
        bafx::config::DisplayOverrideConfig{
            "displayconfig:target-b",
            true,
            true,
            bafx::config::FramePacing::MatchDisplay},
        &error));
    BAFX_CHECK(config.display.overrides.size() == 2U);
    BAFX_CHECK(
        bafx::config::findDisplayOverride(
            config.display,
            "displayconfig:target-b")->enabled);

    BAFX_CHECK(bafx::config::removeDisplayOverride(
        config,
        "displayconfig:target-a"));
    BAFX_CHECK(!bafx::config::removeDisplayOverride(
        config,
        "displayconfig:target-a"));
    BAFX_CHECK(config.display.overrides.size() == 1U);

    bafx::config::DisplayOverrideConfig invalid{};
    invalid.displayKey.assign(1U, '\x1f');
    const bafx::config::Config beforeInvalid = config;
    BAFX_CHECK(!bafx::config::setDisplayOverride(config, invalid, &error));
    BAFX_CHECK(!error.empty());
    BAFX_CHECK(
        config.display.overrides.size()
        == beforeInvalid.display.overrides.size());
}

BAFX_TEST(config_display_override_patch_is_atomic_and_canonical)
{
    const bafx::config::Config base = bafx::config::defaultConfig();
    const auto patched = bafx::config::applyPatchJson(
        base,
        R"json({"generation":9,"path":"display.overrides","value":[{"displayKey":"displayconfig:target-b","enabled":false,"hdrEnabled":true,"framePacing":"144"},{"displayKey":"displayconfig:target-a","enabled":true,"hdrEnabled":false,"framePacing":"60"}]})json");
    BAFX_CHECK(patched.succeeded());
    BAFX_CHECK(patched.expectedGeneration.has_value());
    BAFX_CHECK(*patched.expectedGeneration == 9U);
    BAFX_CHECK(patched.config.display.overrides.size() == 2U);
    BAFX_CHECK(
        patched.config.display.overrides[0].displayKey
        == "displayconfig:target-a");
    BAFX_CHECK(
        patched.config.display.overrides[1].displayKey
        == "displayconfig:target-b");

    const auto roundTrip = bafx::config::parseJson(
        bafx::config::toJson(patched.config, false));
    BAFX_CHECK(roundTrip.succeeded());
    BAFX_CHECK(roundTrip.config.display.overrides.size() == 2U);
    BAFX_CHECK(
        roundTrip.config.display.overrides[1].framePacing
        == bafx::config::FramePacing::Fixed144);

    for (const std::string_view value : {
             R"json([{"displayKey":"displayconfig:duplicate","enabled":true,"hdrEnabled":false,"framePacing":"match-display"},{"displayKey":"displayconfig:duplicate","enabled":false,"hdrEnabled":true,"framePacing":"120"}])json",
             R"json([{"displayKey":"displayconfig:partial","enabled":true,"hdrEnabled":false}])json",
             R"json({"displayKey":"displayconfig:not-an-array"})json"})
    {
        const auto rejected = bafx::config::applyPatchJson(
            base,
            std::string("{\"path\":\"display.overrides\",\"value\":")
                + std::string(value)
                + "}");
        BAFX_CHECK(!rejected.succeeded());
        BAFX_CHECK(rejected.recognized);
        BAFX_CHECK(rejected.config.display.overrides.empty());
    }

    bafx::config::Config unsorted = base;
    unsorted.display.overrides = {
        bafx::config::DisplayOverrideConfig{
            "displayconfig:z",
            true,
            false,
            bafx::config::FramePacing::MatchDisplay},
        bafx::config::DisplayOverrideConfig{
            "displayconfig:a",
            true,
            false,
            bafx::config::FramePacing::MatchDisplay}};
    std::string validationError;
    BAFX_CHECK(!bafx::config::validateConfig(unsorted, &validationError));
    BAFX_CHECK(!validationError.empty());
}

BAFX_TEST(config_display_override_commands_mutate_only_the_addressed_key)
{
    bafx::config::Config base = bafx::config::defaultConfig();
    std::string error;
    BAFX_CHECK(bafx::config::setDisplayOverride(
        base,
        bafx::config::DisplayOverrideConfig{
            "displayconfig-v1-sha256:other",
            true,
            false,
            bafx::config::FramePacing::Fixed60},
        &error));

    const bafx::config::ConfigPatchResult set =
        bafx::config::applyDisplayOverrideJson(
            base,
            R"json({"generation":41,"displayKey":"displayconfig-v1-sha256:selected","enabled":false,"hdrEnabled":true,"framePacing":"120"})json");
    BAFX_CHECK(set.succeeded());
    BAFX_CHECK(set.expectedGeneration.has_value());
    BAFX_CHECK(*set.expectedGeneration == 41U);
    BAFX_CHECK(set.config.display.overrides.size() == 2U);
    const bafx::config::DisplayOverrideConfig* selected =
        bafx::config::findDisplayOverride(
            set.config.display,
            "displayconfig-v1-sha256:selected");
    BAFX_CHECK(selected != nullptr);
    BAFX_CHECK(!selected->enabled);
    BAFX_CHECK(selected->hdrEnabled);
    BAFX_CHECK(
        selected->framePacing == bafx::config::FramePacing::Fixed120);
    BAFX_CHECK(bafx::config::findDisplayOverride(
        set.config.display,
        "displayconfig-v1-sha256:other") != nullptr);

    const bafx::config::ConfigPatchResult removed =
        bafx::config::removeDisplayOverrideJson(
            set.config,
            R"json({"generation":42,"displayKey":"displayconfig-v1-sha256:selected"})json");
    BAFX_CHECK(removed.succeeded());
    BAFX_CHECK(removed.expectedGeneration.has_value());
    BAFX_CHECK(*removed.expectedGeneration == 42U);
    BAFX_CHECK(bafx::config::findDisplayOverride(
        removed.config.display,
        "displayconfig-v1-sha256:selected") == nullptr);
    BAFX_CHECK(bafx::config::findDisplayOverride(
        removed.config.display,
        "displayconfig-v1-sha256:other") != nullptr);
}

BAFX_TEST(config_display_override_commands_reject_partial_or_ambiguous_payloads)
{
    const bafx::config::Config base = bafx::config::defaultConfig();
    for (const std::string_view payload : {
             R"json({"displayKey":"displayconfig-v1-sha256:x","enabled":true,"hdrEnabled":false})json",
             R"json({"displayKey":"displayconfig-v1-sha256:x","enabled":true,"hdrEnabled":false,"framePacing":"adaptive"})json",
             R"json({"displayKey":"displayconfig-v1-sha256:x","enabled":true,"hdrEnabled":false,"framePacing":"60","extra":1})json",
             R"json({"displayKey":"\u001f","enabled":true,"hdrEnabled":false,"framePacing":"60"})json"})
    {
        const bafx::config::ConfigPatchResult rejected =
            bafx::config::applyDisplayOverrideJson(base, payload);
        BAFX_CHECK(!rejected.succeeded());
        BAFX_CHECK(rejected.config.display.overrides.empty());
    }

    const bafx::config::ConfigPatchResult invalidRemoval =
        bafx::config::removeDisplayOverrideJson(
            base,
            R"json({"displayKey":"displayconfig-v1-sha256:x","enabled":true})json");
    BAFX_CHECK(!invalidRemoval.succeeded());
    BAFX_CHECK(invalidRemoval.config.display.overrides.empty());
}

BAFX_TEST(config_current_effect_fields_round_trip_through_file)
{
    const fs::path path = testPath();
    const fs::path root = path.parent_path();
    std::error_code cleanupError;
    fs::remove_all(root, cleanupError);

    bafx::config::Config value = bafx::config::defaultConfig();
    value.effects.themeColor = "#FF6969";
    value.effects.opacity = 0.35F;
    value.effects.clickTimeScale = 0.25F;
    value.effects.trailTimeScale = 3.5F;
    value.effects.trailLifetimeMs = 450.0F;
    value.effects.trailLength = 1.5F;
    value.effects.trailWidth = 1.25F;
    value.effects.diskLifetimeMs = 350.0F;
    value.effects.diskRadius = 48.0F;
    value.effects.ringsCount = 5U;
    value.effects.ringsLifetimeMs = 900.0F;
    value.effects.ringsRadiusMin = 45.0F;
    value.effects.ringsRadiusMax = 95.0F;
    value.effects.ringsAngularVelocityMultiplier = 14.5F;
    value.effects.ringsRotationDirection = 0.5F;
    value.effects.ringsHdrIntensity = 4.0F;
    value.effects.shardsHdrIntensity = 8.0F;
    value.effects.shardsClickCount = 9U;
    value.effects.shardsClickLifetimeMinMs = 250.0F;
    value.effects.shardsClickLifetimeMaxMs = 850.0F;
    value.effects.shardsClickRadius = 72.5F;
    value.effects.shardsClickSpeedMin = 25.0F;
    value.effects.shardsClickSpeedMax = 125.0F;
    value.effects.shardsSizeMin = 12.0F;
    value.effects.shardsSizeMax = 44.0F;
    value.effects.trailOpacity = 0.55F;
    value.effects.bloomIntensity = 3.4F;
    value.effects.bloomDiffusion = 8.5F;
    value.effects.bloomThreshold = 0.75F;
    value.effects.bloomSoftKnee = 0.4F;
    value.effects.bloomClamp = 4096.0F;

    const std::string serialized = bafx::config::toJson(value, false);
    BAFX_CHECK(serialized.find("\"effectsMode\":\"full\"") != std::string::npos);
    BAFX_CHECK(serialized.find("\"themeColor\":\"#ff6969\"") != std::string::npos);
    for (const std::string_view field : {
             "opacity",
             "clickTimeScale",
             "trailTimeScale",
             "trailLifetimeMs",
             "diskLifetimeMs",
             "diskRadius",
             "ringsCount",
             "ringsLifetimeMs",
             "ringsRadiusMin",
             "ringsRadiusMax",
             "ringsAngularVelocityMultiplier",
             "ringsRotationDirection",
             "ringsHdrIntensity",
             "shardsHdrIntensity",
             "shardsClickCount",
             "shardsClickLifetimeMinMs",
             "shardsClickLifetimeMaxMs",
             "shardsClickRadius",
             "shardsClickSpeedMin",
             "shardsClickSpeedMax",
             "shardsSizeMin",
             "shardsSizeMax",
             "trailOpacity",
             "bloomDiffusion",
             "bloomThreshold",
             "bloomSoftKnee",
             "bloomClamp"})
    {
        BAFX_CHECK(serialized.find(std::string("\"") + std::string(field) + "\"")
            != std::string::npos);
    }

    BAFX_CHECK(bafx::config::saveConfigAtomic(path, value).succeeded());
    const auto loaded = bafx::config::loadConfig(path);
    BAFX_CHECK(loaded.status == bafx::config::ConfigStatus::Ok);
    BAFX_CHECK(loaded.config.schemaVersion == bafx::config::currentSchemaVersion);
    BAFX_CHECK(loaded.config.effects.themeColor == "#ff6969");
    BAFX_CHECK_NEAR(loaded.config.effects.opacity, 0.35F, 0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.clickTimeScale, 0.25F, 0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.trailTimeScale, 3.5F, 0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.trailLifetimeMs, 450.0F, 0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.trailLength, 1.5F, 0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.trailWidth, 1.25F, 0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.diskLifetimeMs, 350.0F, 0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.diskRadius, 48.0F, 0.00001F);
    BAFX_CHECK(loaded.config.effects.ringsCount == 5U);
    BAFX_CHECK_NEAR(loaded.config.effects.ringsLifetimeMs, 900.0F, 0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.ringsRadiusMin, 45.0F, 0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.ringsRadiusMax, 95.0F, 0.00001F);
    BAFX_CHECK_NEAR(
        loaded.config.effects.ringsAngularVelocityMultiplier,
        14.5F,
        0.00001F);
    BAFX_CHECK_NEAR(
        loaded.config.effects.ringsRotationDirection,
        0.5F,
        0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.ringsHdrIntensity, 4.0F, 0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.shardsHdrIntensity, 8.0F, 0.00001F);
    BAFX_CHECK(loaded.config.effects.shardsClickCount == 9U);
    BAFX_CHECK_NEAR(
        loaded.config.effects.shardsClickLifetimeMinMs,
        250.0F,
        0.00001F);
    BAFX_CHECK_NEAR(
        loaded.config.effects.shardsClickLifetimeMaxMs,
        850.0F,
        0.00001F);
    BAFX_CHECK_NEAR(
        loaded.config.effects.shardsClickRadius,
        72.5F,
        0.00001F);
    BAFX_CHECK_NEAR(
        loaded.config.effects.shardsClickSpeedMin,
        25.0F,
        0.00001F);
    BAFX_CHECK_NEAR(
        loaded.config.effects.shardsClickSpeedMax,
        125.0F,
        0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.shardsSizeMin, 12.0F, 0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.shardsSizeMax, 44.0F, 0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.trailOpacity, 0.55F, 0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.bloomIntensity, 3.4F, 0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.bloomDiffusion, 8.5F, 0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.bloomThreshold, 0.75F, 0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.bloomSoftKnee, 0.4F, 0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.bloomClamp, 4096.0F, 0.00001F);

    const std::string fxConfig = bafx::config::getFxConfig(loaded.config, false);
    for (const std::string_view fragment : {
             "\"diskLifetimeMs\":350",
             "\"ringsCount\":5",
             "\"ringsRadiusMin\":45",
             "\"ringsRadiusMax\":95",
             "\"ringsAngularVelocityMultiplier\":14.5",
             "\"ringsRotationDirection\":0.5",
             "\"shardsClickCount\":9",
             "\"shardsClickLifetimeMinMs\":250",
             "\"shardsClickLifetimeMaxMs\":850",
             "\"shardsClickRadius\":72.5",
             "\"shardsClickSpeedMin\":25",
             "\"shardsClickSpeedMax\":125",
             "\"shardsSizeMin\":12",
             "\"shardsSizeMax\":44"})
    {
        BAFX_CHECK(fxConfig.find(fragment) != std::string::npos);
    }
    BAFX_CHECK(fxConfig.find("\"effects\"") == std::string::npos);
    BAFX_CHECK(fxConfig.find("\"samplingRateHz\"") == std::string::npos);
    BAFX_CHECK(
        fxConfig.find("\"trailOnlyWhilePressed\"")
        == std::string::npos);

    removeTestTree(path);
}

BAFX_TEST(config_effects_json_codec_round_trips_complete_flat_object)
{
    bafx::config::EffectsConfig value =
        bafx::config::defaultConfig().effects;
    value.enabled = false;
    value.diskLayerEnabled = false;
    value.ringsLayerEnabled = true;
    value.clickShardsLayerEnabled = false;
    value.trailShardsLayerEnabled = true;
    value.trailLayerEnabled = false;
    value.bloomLayerEnabled = true;
    value.themeColor = "#FF6969";
    value.globalScale = 1.75F;
    value.opacity = 0.35F;
    value.clickEnabled = false;
    value.trailEnabled = true;
    value.trailLength = 1.5F;
    value.trailWidth = 1.25F;
    value.clickTimeScale = 0.25F;
    value.trailTimeScale = 3.5F;
    value.trailLifetimeMs = 450.0F;
    value.diskLifetimeMs = 350.0F;
    value.diskRadius = 48.0F;
    value.ringsCount = 5U;
    value.ringsLifetimeMs = 900.0F;
    value.ringsRadiusMin = 45.0F;
    value.ringsRadiusMax = 95.0F;
    value.ringsAngularVelocityMultiplier = 14.5F;
    value.ringsRotationDirection = 0.5F;
    value.ringsHdrIntensity = 4.0F;
    value.shardsHdrIntensity = 8.0F;
    value.shardsClickCount = 9U;
    value.shardsClickLifetimeMinMs = 250.0F;
    value.shardsClickLifetimeMaxMs = 850.0F;
    value.shardsClickRadius = 72.5F;
    value.shardsClickSpeedMin = 25.0F;
    value.shardsClickSpeedMax = 125.0F;
    value.shardsSizeMin = 12.0F;
    value.shardsSizeMax = 44.0F;
    value.trailOpacity = 0.55F;
    value.bloomIntensity = 3.4F;
    value.bloomDiffusion = 8.5F;
    value.bloomThreshold = 0.75F;
    value.bloomSoftKnee = 0.4F;
    value.bloomClamp = 4096.0F;

    const std::string serialized = bafx::config::toJson(value, false);
    BAFX_CHECK(!serialized.empty());
    BAFX_CHECK(serialized.front() == '{');
    BAFX_CHECK(serialized.back() == '}');
    BAFX_CHECK(serialized.find("\"schemaVersion\"") == std::string::npos);
    BAFX_CHECK(serialized.find("\"effects\"") == std::string::npos);
    BAFX_CHECK(serialized.find("\"background\"") == std::string::npos);
    BAFX_CHECK(serialized.find("\"themeColor\":\"#ff6969\"")
        != std::string::npos);

    bafx::config::Config wrapper = bafx::config::defaultConfig();
    wrapper.effects = value;
    BAFX_CHECK(bafx::config::getFxConfig(wrapper, false) == serialized);

    const bafx::config::EffectsConfigParseResult parsed =
        bafx::config::parseEffectsJson(serialized);
    BAFX_CHECK(parsed.succeeded());
    BAFX_CHECK(parsed.status == bafx::config::ConfigStatus::Ok);
    BAFX_CHECK(parsed.config.has_value());
    BAFX_CHECK(!parsed.config->enabled);
    BAFX_CHECK(!parsed.config->diskLayerEnabled);
    BAFX_CHECK(parsed.config->ringsLayerEnabled);
    BAFX_CHECK(!parsed.config->clickShardsLayerEnabled);
    BAFX_CHECK(parsed.config->trailShardsLayerEnabled);
    BAFX_CHECK(!parsed.config->trailLayerEnabled);
    BAFX_CHECK(parsed.config->bloomLayerEnabled);
    BAFX_CHECK(parsed.config->themeColor == "#ff6969");
    BAFX_CHECK(!parsed.config->clickEnabled);
    BAFX_CHECK(parsed.config->trailEnabled);
    // Deterministic reserialization verifies every numeric member without
    // duplicating the production field table in this test.
    BAFX_CHECK(bafx::config::toJson(*parsed.config, false) == serialized);

    const std::string pretty = bafx::config::toJson(value, true);
    BAFX_CHECK(!pretty.empty());
    BAFX_CHECK(pretty.back() == '\n');
    BAFX_CHECK(pretty.find("\n  \"bloomClamp\"") != std::string::npos);
}

BAFX_TEST(config_effects_json_codec_rejects_non_strict_documents_atomically)
{
    bafx::config::EffectsConfig value =
        bafx::config::defaultConfig().effects;
    value.ringsCount = 5U;
    value.globalScale = 1.75F;
    const std::string valid = bafx::config::toJson(value, false);

    std::string missing = valid;
    const std::string requiredField = "\"ringsCount\":5,";
    const std::size_t requiredPosition = missing.find(requiredField);
    BAFX_CHECK(requiredPosition != std::string::npos);
    missing.erase(requiredPosition, requiredField.size());
    const auto missingResult = bafx::config::parseEffectsJson(missing);
    BAFX_CHECK(!missingResult.succeeded());
    BAFX_CHECK(!missingResult.config.has_value());
    BAFX_CHECK(
        missingResult.status == bafx::config::ConfigStatus::ValidationError);
    BAFX_CHECK(
        missingResult.message.find("effects.ringsCount")
        != std::string::npos);

    std::string unknown = valid;
    unknown.insert(unknown.size() - 1U, ",\"metadata\":true");
    const auto unknownResult = bafx::config::parseEffectsJson(unknown);
    BAFX_CHECK(!unknownResult.succeeded());
    BAFX_CHECK(!unknownResult.config.has_value());
    BAFX_CHECK(
        unknownResult.status == bafx::config::ConfigStatus::ValidationError);
    BAFX_CHECK(
        unknownResult.message.find("effects.metadata")
        != std::string::npos);

    std::string wrongType = valid;
    const std::string enabledField = "\"enabled\":true";
    const std::size_t enabledPosition = wrongType.find(enabledField);
    BAFX_CHECK(enabledPosition != std::string::npos);
    wrongType.replace(
        enabledPosition,
        enabledField.size(),
        "\"enabled\":\"true\"");
    const auto wrongTypeResult = bafx::config::parseEffectsJson(wrongType);
    BAFX_CHECK(!wrongTypeResult.succeeded());
    BAFX_CHECK(!wrongTypeResult.config.has_value());
    BAFX_CHECK(
        wrongTypeResult.status
        == bafx::config::ConfigStatus::ValidationError);

    std::string invalidRange = valid;
    const std::string scaleField = "\"globalScale\":1.75";
    const std::size_t scalePosition = invalidRange.find(scaleField);
    BAFX_CHECK(scalePosition != std::string::npos);
    invalidRange.replace(
        scalePosition,
        scaleField.size(),
        "\"globalScale\":9");
    const auto invalidRangeResult =
        bafx::config::parseEffectsJson(invalidRange);
    BAFX_CHECK(!invalidRangeResult.succeeded());
    BAFX_CHECK(!invalidRangeResult.config.has_value());
    BAFX_CHECK(
        invalidRangeResult.status
        == bafx::config::ConfigStatus::ValidationError);

    const auto arrayResult = bafx::config::parseEffectsJson("[]");
    BAFX_CHECK(!arrayResult.succeeded());
    BAFX_CHECK(!arrayResult.config.has_value());
    BAFX_CHECK(arrayResult.status == bafx::config::ConfigStatus::ParseError);

    const auto duplicateResult = bafx::config::parseEffectsJson(
        R"json({"enabled":true,"enabled":false})json");
    BAFX_CHECK(!duplicateResult.succeeded());
    BAFX_CHECK(!duplicateResult.config.has_value());
    BAFX_CHECK(
        duplicateResult.status == bafx::config::ConfigStatus::ParseError);
}

BAFX_TEST(config_fx_parameter_boundaries_use_native_paths)
{
    const bafx::config::Config base = bafx::config::defaultConfig();

    const auto disabled = bafx::config::setFxParam(
        base,
        "effects.enabled",
        "false");
    BAFX_CHECK(disabled.succeeded());
    BAFX_CHECK(!disabled.config.effects.enabled);

    const auto themeColor = bafx::config::setFxParam(
        base,
        "effects.themeColor",
        "\"#FF6969\"");
    BAFX_CHECK(themeColor.succeeded());
    BAFX_CHECK(themeColor.config.effects.themeColor == "#ff6969");

    const auto invalidThemeColor = bafx::config::setFxParam(
        base,
        "effects.themeColor",
        "\"#ff6969cc\"");
    BAFX_CHECK(!invalidThemeColor.succeeded());
    BAFX_CHECK(invalidThemeColor.recognized);
    BAFX_CHECK(
        invalidThemeColor.config.effects.themeColor
        == base.effects.themeColor);

    const auto globalScale = bafx::config::setFxParam(
        base,
        "effects.globalScale",
        "2");
    BAFX_CHECK(globalScale.succeeded());
    BAFX_CHECK_NEAR(globalScale.config.effects.globalScale, 2.0F, 0.00001F);

    const auto trailLength = bafx::config::setFxParam(
        base,
        "effects.trailLength",
        "2");
    BAFX_CHECK(trailLength.succeeded());
    BAFX_CHECK_NEAR(trailLength.config.effects.trailLength, 2.0F, 0.00001F);
    BAFX_CHECK_NEAR(
        trailLength.config.effects.trailLifetimeMs,
        600.0F,
        0.00001F);

    const auto opacityMinimum = bafx::config::setFxParam(
        base,
        "effects.opacity",
        "0");
    BAFX_CHECK(opacityMinimum.succeeded());
    BAFX_CHECK_NEAR(opacityMinimum.config.effects.opacity, 0.0F, 0.00001F);

    const auto opacityMaximum = bafx::config::setFxParam(
        base,
        "effects.opacity",
        "1");
    BAFX_CHECK(opacityMaximum.succeeded());
    BAFX_CHECK_NEAR(opacityMaximum.config.effects.opacity, 1.0F, 0.00001F);

    const auto clickMinimum = bafx::config::setFxParam(
        base,
        "effects.clickTimeScale",
        "0.01");
    BAFX_CHECK(clickMinimum.succeeded());
    BAFX_CHECK_NEAR(clickMinimum.config.effects.clickTimeScale, 0.01F, 0.00001F);

    const auto trailMaximum = bafx::config::setFxParam(
        base,
        "effects.trailTimeScale",
        "4");
    BAFX_CHECK(trailMaximum.succeeded());
    BAFX_CHECK_NEAR(trailMaximum.config.effects.trailTimeScale, 4.0F, 0.00001F);

    const auto lifetime = bafx::config::setFxParam(
        base,
        "effects.trailLifetimeMs",
        "10000");
    BAFX_CHECK(lifetime.succeeded());
    BAFX_CHECK_NEAR(lifetime.config.effects.trailLifetimeMs, 10000.0F, 0.00001F);
    BAFX_CHECK_NEAR(
        lifetime.config.effects.trailLength,
        10000.0F / 300.0F,
        0.00001F);

    const auto width = bafx::config::setFxParam(
        base,
        "effects.trailWidth",
        "4");
    BAFX_CHECK(width.succeeded());
    BAFX_CHECK_NEAR(width.config.effects.trailWidth, 4.0F, 0.00001F);

    const auto bloomIntensity = bafx::config::setFxParam(
        base,
        "effects.bloomIntensity",
        "3.4");
    BAFX_CHECK(bloomIntensity.succeeded());
    BAFX_CHECK_NEAR(bloomIntensity.config.effects.bloomIntensity, 3.4F, 0.00001F);

    const auto diskRadius = bafx::config::setFxParam(
        base,
        "effects.diskRadius",
        "48");
    BAFX_CHECK(diskRadius.succeeded());
    BAFX_CHECK_NEAR(diskRadius.config.effects.diskRadius, 48.0F, 0.00001F);

    const auto diskLifetime = bafx::config::setFxParam(
        base,
        "effects.diskLifetimeMs",
        "500");
    BAFX_CHECK(diskLifetime.succeeded());
    BAFX_CHECK_NEAR(
        diskLifetime.config.effects.diskLifetimeMs,
        500.0F,
        0.00001F);

    const auto ringsCount = bafx::config::setFxParam(
        base,
        "effects.ringsCount",
        "64");
    BAFX_CHECK(ringsCount.succeeded());
    BAFX_CHECK(ringsCount.config.effects.ringsCount == 64U);

    const auto ringsLifetime = bafx::config::setFxParam(
        base,
        "effects.ringsLifetimeMs",
        "2000");
    BAFX_CHECK(ringsLifetime.succeeded());
    BAFX_CHECK_NEAR(
        ringsLifetime.config.effects.ringsLifetimeMs,
        2000.0F,
        0.00001F);

    const auto ringsRadiusMin = bafx::config::setFxParam(
        base,
        "effects.ringsRadiusMin",
        "100");
    BAFX_CHECK(ringsRadiusMin.succeeded());
    BAFX_CHECK_NEAR(
        ringsRadiusMin.config.effects.ringsRadiusMin,
        100.0F,
        0.00001F);

    const auto ringsRadiusMax = bafx::config::setFxParam(
        base,
        "effects.ringsRadiusMax",
        "30");
    BAFX_CHECK(ringsRadiusMax.succeeded());
    BAFX_CHECK_NEAR(
        ringsRadiusMax.config.effects.ringsRadiusMax,
        30.0F,
        0.00001F);

    const auto angularVelocity = bafx::config::setFxParam(
        base,
        "effects.ringsAngularVelocityMultiplier",
        "100");
    BAFX_CHECK(angularVelocity.succeeded());
    BAFX_CHECK_NEAR(
        angularVelocity.config.effects.ringsAngularVelocityMultiplier,
        100.0F,
        0.00001F);

    const auto rotationDirection = bafx::config::setFxParam(
        base,
        "effects.ringsRotationDirection",
        "0.5");
    BAFX_CHECK(rotationDirection.succeeded());
    BAFX_CHECK_NEAR(
        rotationDirection.config.effects.ringsRotationDirection,
        0.5F,
        0.00001F);

    const auto ringIntensity = bafx::config::setFxParam(
        base,
        "effects.ringsHdrIntensity",
        "4.5");
    BAFX_CHECK(ringIntensity.succeeded());
    BAFX_CHECK_NEAR(
        ringIntensity.config.effects.ringsHdrIntensity,
        4.5F,
        0.00001F);

    const auto shardIntensity = bafx::config::setFxParam(
        base,
        "effects.shardsHdrIntensity",
        "7.5");
    BAFX_CHECK(shardIntensity.succeeded());
    BAFX_CHECK_NEAR(
        shardIntensity.config.effects.shardsHdrIntensity,
        7.5F,
        0.00001F);

    const auto shardCount = bafx::config::setFxParam(
        base,
        "effects.shardsClickCount",
        "1000");
    BAFX_CHECK(shardCount.succeeded());
    BAFX_CHECK(shardCount.config.effects.shardsClickCount == 1000U);

    const auto shardLifetimeMin = bafx::config::setFxParam(
        base,
        "effects.shardsClickLifetimeMinMs",
        "100");
    BAFX_CHECK(shardLifetimeMin.succeeded());
    BAFX_CHECK_NEAR(
        shardLifetimeMin.config.effects.shardsClickLifetimeMinMs,
        100.0F,
        0.00001F);

    const auto shardLifetimeMax = bafx::config::setFxParam(
        base,
        "effects.shardsClickLifetimeMaxMs",
        "10000");
    BAFX_CHECK(shardLifetimeMax.succeeded());
    BAFX_CHECK_NEAR(
        shardLifetimeMax.config.effects.shardsClickLifetimeMaxMs,
        10000.0F,
        0.00001F);

    const auto shardRadius = bafx::config::setFxParam(
        base,
        "effects.shardsClickRadius",
        "5000");
    BAFX_CHECK(shardRadius.succeeded());
    BAFX_CHECK_NEAR(
        shardRadius.config.effects.shardsClickRadius,
        5000.0F,
        0.00001F);

    const auto shardSpeedMin = bafx::config::setFxParam(
        base,
        "effects.shardsClickSpeedMin",
        "0");
    BAFX_CHECK(shardSpeedMin.succeeded());
    BAFX_CHECK_NEAR(
        shardSpeedMin.config.effects.shardsClickSpeedMin,
        0.0F,
        0.00001F);

    const auto shardSpeedMax = bafx::config::setFxParam(
        base,
        "effects.shardsClickSpeedMax",
        "5000");
    BAFX_CHECK(shardSpeedMax.succeeded());
    BAFX_CHECK_NEAR(
        shardSpeedMax.config.effects.shardsClickSpeedMax,
        5000.0F,
        0.00001F);

    const auto shardSizeMin = bafx::config::setFxParam(
        base,
        "effects.shardsSizeMin",
        "0");
    BAFX_CHECK(shardSizeMin.succeeded());
    BAFX_CHECK_NEAR(
        shardSizeMin.config.effects.shardsSizeMin,
        0.0F,
        0.00001F);

    const auto shardSizeMax = bafx::config::setFxParam(
        base,
        "effects.shardsSizeMax",
        "2000");
    BAFX_CHECK(shardSizeMax.succeeded());
    BAFX_CHECK_NEAR(
        shardSizeMax.config.effects.shardsSizeMax,
        2000.0F,
        0.00001F);

    const auto trailOpacity = bafx::config::setFxParam(
        base,
        "effects.trailOpacity",
        "0.4");
    BAFX_CHECK(trailOpacity.succeeded());
    BAFX_CHECK_NEAR(
        trailOpacity.config.effects.trailOpacity,
        0.4F,
        0.00001F);

    const auto diffusionMinimum = bafx::config::setFxParam(
        base,
        "effects.bloomDiffusion",
        "0");
    BAFX_CHECK(diffusionMinimum.succeeded());
    BAFX_CHECK_NEAR(diffusionMinimum.config.effects.bloomDiffusion, 0.0F, 0.00001F);

    const auto clampMaximum = bafx::config::setFxParam(
        base,
        "effects.bloomClamp",
        "65504");
    BAFX_CHECK(clampMaximum.succeeded());
    BAFX_CHECK_NEAR(clampMaximum.config.effects.bloomClamp, 65504.0F, 0.00001F);

    for (const auto& invalid : {
             std::pair{"effects.opacity", "-0.01"},
             std::pair{"effects.opacity", "1.01"},
             std::pair{"effects.clickTimeScale", "0.009"},
             std::pair{"effects.trailTimeScale", "4.01"},
             std::pair{"effects.trailLifetimeMs", "10000.01"},
             std::pair{"effects.diskLifetimeMs", "0"},
             std::pair{"effects.diskLifetimeMs", "10001"},
             std::pair{"effects.ringsCount", "65"},
             std::pair{"effects.ringsCount", "2.5"},
             std::pair{"effects.ringsLifetimeMs", "0"},
             std::pair{"effects.ringsRadiusMin", "-0.01"},
             std::pair{"effects.ringsRadiusMax", "2000.01"},
             std::pair{"effects.ringsAngularVelocityMultiplier", "100.01"},
             std::pair{"effects.ringsRotationDirection", "-1.01"},
             std::pair{"effects.ringsRotationDirection", "1.01"},
             std::pair{"effects.shardsClickCount", "1001"},
             std::pair{"effects.shardsClickCount", "2.5"},
             std::pair{"effects.shardsClickLifetimeMinMs", "0"},
             std::pair{"effects.shardsClickLifetimeMaxMs", "10001"},
             std::pair{"effects.shardsClickRadius", "5000.01"},
             std::pair{"effects.shardsClickSpeedMin", "5000.01"},
             std::pair{"effects.shardsClickSpeedMax", "-0.01"},
             std::pair{"effects.shardsSizeMin", "2000.01"},
             std::pair{"effects.shardsSizeMax", "-0.01"},
             std::pair{"effects.bloomSoftKnee", "1.01"},
             std::pair{"effects.bloomClamp", "-0.01"}})
    {
        const auto result = bafx::config::setFxParam(
            base,
            invalid.first,
            invalid.second);
        BAFX_CHECK(!result.succeeded());
        BAFX_CHECK(result.config.effects.opacity == base.effects.opacity);
        BAFX_CHECK(result.config.effects.clickTimeScale == base.effects.clickTimeScale);
        BAFX_CHECK(result.config.effects.bloomClamp == base.effects.bloomClamp);
    }

    for (const std::string_view nonFxPath : {
             "background.mode",
             "display.hdrEnabled",
             "input.samplingRateHz",
             "performance.idleOptimization",
             "system.startWithWindows"})
    {
        const auto result = bafx::config::setFxParam(base, nonFxPath, "true");
        BAFX_CHECK(!result.succeeded());
        BAFX_CHECK(result.status == bafx::config::ConfigStatus::ValidationError);
    }

    for (const std::string_view retiredWebPath : {
             "opacity",
             "scale",
             "disk.radius",
             "rings.count",
             "shards.clickCount",
             "trail.lifetimeMs",
             "trail.width",
             "bloom.intensity"})
    {
        const auto result = bafx::config::setFxParam(
            base,
            retiredWebPath,
            "1");
        BAFX_CHECK(!result.succeeded());
        BAFX_CHECK(result.status == bafx::config::ConfigStatus::ValidationError);
    }

    const auto retiredGeneralPath = bafx::config::applyPatchJson(
        base,
        R"json({"path":"opacity","value":0.5})json");
    BAFX_CHECK(!retiredGeneralPath.succeeded());
    BAFX_CHECK(
        retiredGeneralPath.status
        == bafx::config::ConfigStatus::ValidationError);
}

BAFX_TEST(config_fx_parameter_batch_is_atomic_and_preserves_generation)
{
    const bafx::config::Config base = bafx::config::defaultConfig();
    const auto batch = bafx::config::setFxParams(
        base,
        R"json({"generation":7,"patch":{"effects.themeColor":"#FF6969","effects.opacity":0.25,"effects.clickTimeScale":2,"effects.trailLifetimeMs":600,"effects.diskLifetimeMs":350,"effects.ringsCount":4,"effects.ringsLifetimeMs":900,"effects.ringsRadiusMin":45,"effects.ringsRadiusMax":95,"effects.ringsAngularVelocityMultiplier":14.5,"effects.ringsRotationDirection":0.5,"effects.shardsClickCount":7,"effects.shardsClickLifetimeMinMs":100,"effects.shardsClickLifetimeMaxMs":200,"effects.shardsClickRadius":75,"effects.shardsClickSpeedMin":10,"effects.shardsClickSpeedMax":20,"effects.shardsSizeMin":1,"effects.shardsSizeMax":2,"effects.bloomIntensity":4.2}})json");
    BAFX_CHECK(batch.succeeded());
    BAFX_CHECK(batch.expectedGeneration.has_value());
    BAFX_CHECK(*batch.expectedGeneration == 7U);
    BAFX_CHECK(batch.config.effects.themeColor == "#ff6969");
    BAFX_CHECK_NEAR(batch.config.effects.opacity, 0.25F, 0.00001F);
    BAFX_CHECK_NEAR(batch.config.effects.clickTimeScale, 2.0F, 0.00001F);
    BAFX_CHECK_NEAR(batch.config.effects.trailLifetimeMs, 600.0F, 0.00001F);
    BAFX_CHECK_NEAR(batch.config.effects.trailLength, 2.0F, 0.00001F);
    BAFX_CHECK_NEAR(batch.config.effects.diskLifetimeMs, 350.0F, 0.00001F);
    BAFX_CHECK(batch.config.effects.ringsCount == 4U);
    BAFX_CHECK_NEAR(batch.config.effects.ringsLifetimeMs, 900.0F, 0.00001F);
    BAFX_CHECK_NEAR(batch.config.effects.ringsRadiusMin, 45.0F, 0.00001F);
    BAFX_CHECK_NEAR(batch.config.effects.ringsRadiusMax, 95.0F, 0.00001F);
    BAFX_CHECK_NEAR(
        batch.config.effects.ringsAngularVelocityMultiplier,
        14.5F,
        0.00001F);
    BAFX_CHECK_NEAR(
        batch.config.effects.ringsRotationDirection,
        0.5F,
        0.00001F);
    BAFX_CHECK(batch.config.effects.shardsClickCount == 7U);
    BAFX_CHECK_NEAR(
        batch.config.effects.shardsClickLifetimeMinMs,
        100.0F,
        0.00001F);
    BAFX_CHECK_NEAR(
        batch.config.effects.shardsClickLifetimeMaxMs,
        200.0F,
        0.00001F);
    BAFX_CHECK_NEAR(
        batch.config.effects.shardsClickRadius,
        75.0F,
        0.00001F);
    BAFX_CHECK_NEAR(
        batch.config.effects.shardsClickSpeedMin,
        10.0F,
        0.00001F);
    BAFX_CHECK_NEAR(
        batch.config.effects.shardsClickSpeedMax,
        20.0F,
        0.00001F);
    BAFX_CHECK_NEAR(batch.config.effects.shardsSizeMin, 1.0F, 0.00001F);
    BAFX_CHECK_NEAR(batch.config.effects.shardsSizeMax, 2.0F, 0.00001F);
    BAFX_CHECK_NEAR(batch.config.effects.bloomIntensity, 4.2F, 0.00001F);

    const auto rejected = bafx::config::setFxParams(
        base,
        R"json({"generation":7,"patch":{"effects.opacity":0.25,"effects.themeColor":"red"}})json");
    BAFX_CHECK(!rejected.succeeded());
    BAFX_CHECK(rejected.config.effects.opacity == base.effects.opacity);
    BAFX_CHECK(rejected.config.effects.themeColor == base.effects.themeColor);

    const auto rejectedProductPath = bafx::config::setFxParams(
        base,
        R"json({"generation":7,"patch":{"effects.opacity":0.25,"display.hdrEnabled":true}})json");
    BAFX_CHECK(!rejectedProductPath.succeeded());
    BAFX_CHECK(rejectedProductPath.expectedGeneration.has_value());
    BAFX_CHECK(*rejectedProductPath.expectedGeneration == 7U);
    BAFX_CHECK(rejectedProductPath.config.effects.opacity == base.effects.opacity);
    BAFX_CHECK(rejectedProductPath.config.display.hdrEnabled == base.display.hdrEnabled);

    const auto reversedRanges = bafx::config::setFxParams(
        base,
        R"json({"patch":{"effects.shardsClickLifetimeMinMs":900,"effects.shardsClickLifetimeMaxMs":800,"effects.shardsClickSpeedMin":90,"effects.shardsClickSpeedMax":80,"effects.shardsSizeMin":50,"effects.shardsSizeMax":40}})json");
    BAFX_CHECK(reversedRanges.succeeded());
    BAFX_CHECK_NEAR(
        reversedRanges.config.effects.shardsClickLifetimeMinMs,
        900.0F,
        0.00001F);
    BAFX_CHECK_NEAR(
        reversedRanges.config.effects.shardsClickLifetimeMaxMs,
        800.0F,
        0.00001F);
    BAFX_CHECK_NEAR(
        reversedRanges.config.effects.shardsClickSpeedMin,
        90.0F,
        0.00001F);
    BAFX_CHECK_NEAR(
        reversedRanges.config.effects.shardsClickSpeedMax,
        80.0F,
        0.00001F);
    BAFX_CHECK_NEAR(
        reversedRanges.config.effects.shardsSizeMin,
        50.0F,
        0.00001F);
    BAFX_CHECK_NEAR(
        reversedRanges.config.effects.shardsSizeMax,
        40.0F,
        0.00001F);
}

BAFX_TEST(config_bloom_quality_is_derived_from_continuous_diffusion)
{
    const bafx::config::Config base = bafx::config::defaultConfig();
    BAFX_CHECK(
        bafx::config::bloomQualityForDiffusion(base.effects.bloomDiffusion)
        == bafx::config::BloomQuality::High);

    const auto continuous = bafx::config::setFxParam(
        base,
        "effects.bloomDiffusion",
        "8.5");
    BAFX_CHECK(continuous.succeeded());
    BAFX_CHECK(
        bafx::config::bloomQualityForDiffusion(
            continuous.config.effects.bloomDiffusion)
        == bafx::config::BloomQuality::Custom);
    BAFX_CHECK(
        bafx::config::toJson(continuous.config, false).find("bloomQuality")
        == std::string::npos);

    const auto preset = bafx::config::applyPatchJson(
        continuous.config,
        R"json({"path":"effects.bloomQuality","value":"low"})json");
    BAFX_CHECK(preset.succeeded());
    BAFX_CHECK_NEAR(preset.config.effects.bloomDiffusion, 4.0F, 0.00001F);
    BAFX_CHECK(
        bafx::config::bloomQualityForDiffusion(
            preset.config.effects.bloomDiffusion)
        == bafx::config::BloomQuality::Low);
}

BAFX_TEST(config_render_modes_use_canonical_wire_values)
{
    const bafx::config::Config base = bafx::config::defaultConfig();

    BAFX_CHECK(
        bafx::config::toString(bafx::config::RenderMode::BackgroundAware)
        == "background-aware");
    BAFX_CHECK(
        bafx::config::toString(bafx::config::RenderMode::RecordingCompatible)
        == "recording-compatible");
    BAFX_CHECK(
        bafx::config::toString(bafx::config::RenderMode::LightBackground)
        == "light-background");

    const auto backgroundAware = bafx::config::applyPatchJson(
        base,
        R"json({"path":"background.mode","value":"background-aware"})json");
    BAFX_CHECK(backgroundAware.succeeded());
    BAFX_CHECK(backgroundAware.config.background.mode
        == bafx::config::RenderMode::BackgroundAware);

    const auto recordingCompatible = bafx::config::applyPatchJson(
        base,
        R"json({"path":"background.mode","value":"recording-compatible"})json");
    BAFX_CHECK(recordingCompatible.succeeded());
    BAFX_CHECK(recordingCompatible.config.background.mode
        == bafx::config::RenderMode::RecordingCompatible);

    const auto unknownMode = bafx::config::applyPatchJson(
        base,
        R"json({"path":"background.mode","value":"not-a-render-mode"})json");
    BAFX_CHECK(!unknownMode.succeeded());

    const auto lightBackground = bafx::config::applyPatchJson(
        base,
        R"json({"path":"background.mode","value":"light-background"})json");
    BAFX_CHECK(lightBackground.succeeded());
    BAFX_CHECK(lightBackground.config.background.mode
        == bafx::config::RenderMode::LightBackground);

}

BAFX_TEST(config_patch_controls_cursor_capture_policy)
{
    const bafx::config::Config base = bafx::config::defaultConfig();
    const auto included = bafx::config::applyPatchJson(
        base,
        R"json({"path":"background.cursorExcluded","value":false})json");
    BAFX_CHECK(included.succeeded());
    BAFX_CHECK(!included.config.background.cursorExcluded);

    const auto excluded = bafx::config::applyPatchJson(
        included.config,
        R"json({"path":"background.cursorExcluded","value":true})json");
    BAFX_CHECK(excluded.succeeded());
    BAFX_CHECK(excluded.config.background.cursorExcluded);
}

BAFX_TEST(config_patch_controls_system_capture_border_policy)
{
    const bafx::config::Config base = bafx::config::defaultConfig();
    const auto allowed = bafx::config::applyPatchJson(
        base,
        R"json({"path":"background.allowSystemBorder","value":true})json");
    BAFX_CHECK(allowed.succeeded());
    BAFX_CHECK(allowed.config.background.allowSystemBorder);

    const auto hidden = bafx::config::applyPatchJson(
        allowed.config,
        R"json({"path":"background.allowSystemBorder","value":false})json");
    BAFX_CHECK(hidden.succeeded());
    BAFX_CHECK(!hidden.config.background.allowSystemBorder);
}

BAFX_TEST(config_patch_controls_always_on_trail_policy)
{
    const bafx::config::Config base = bafx::config::defaultConfig();
    const auto alwaysOn = bafx::config::applyPatchJson(
        base,
        R"json({"path":"input.trailOnlyWhilePressed","value":false})json");
    BAFX_CHECK(alwaysOn.succeeded());
    BAFX_CHECK(!alwaysOn.config.input.trailOnlyWhilePressed);

    const auto pressedOnly = bafx::config::applyPatchJson(
        alwaysOn.config,
        R"json({"path":"input.trailOnlyWhilePressed","value":true})json");
    BAFX_CHECK(pressedOnly.succeeded());
    BAFX_CHECK(pressedOnly.config.input.trailOnlyWhilePressed);

    const auto invalid = bafx::config::applyPatchJson(
        base,
        R"json({"path":"input.trailOnlyWhilePressed","value":"yes"})json");
    BAFX_CHECK(!invalid.succeeded());
    BAFX_CHECK(invalid.status == bafx::config::ConfigStatus::ValidationError);
}

BAFX_TEST(config_patch_controls_input_sampling_rate)
{
    const bafx::config::Config base = bafx::config::defaultConfig();
    for (const std::uint32_t rate : {0U, 15U, 30U, 1000U})
    {
        const auto result = bafx::config::applyPatchJson(
            base,
            std::string("{\"path\":\"input.samplingRateHz\",\"value\":")
                + std::to_string(rate)
                + "}");
        BAFX_CHECK(result.succeeded());
        BAFX_CHECK(result.config.input.samplingRateHz == rate);
    }

    for (const std::string_view value : {"-1", "0.5", "1001", "\"30\""})
    {
        const auto result = bafx::config::applyPatchJson(
            base,
            std::string("{\"path\":\"input.samplingRateHz\",\"value\":")
                + std::string(value)
                + "}");
        BAFX_CHECK(!result.succeeded());
        BAFX_CHECK(result.status == bafx::config::ConfigStatus::ValidationError);
    }
}

BAFX_TEST(config_bloom_quality_preserves_the_unity_default_at_high)
{
    BAFX_CHECK_NEAR(
        bafx::config::bloomDiffusionForQuality(bafx::config::BloomQuality::Low),
        4.0F,
        0.00001F);
    BAFX_CHECK_NEAR(
        bafx::config::bloomDiffusionForQuality(bafx::config::BloomQuality::Medium),
        6.0F,
        0.00001F);
    BAFX_CHECK_NEAR(
        bafx::config::bloomDiffusionForQuality(bafx::config::BloomQuality::High),
        7.0F,
        0.00001F);
    BAFX_CHECK_NEAR(
        bafx::config::bloomDiffusionForQuality(bafx::config::BloomQuality::Ultra),
        10.0F,
        0.00001F);
}

BAFX_TEST(config_parser_rejects_non_current_schemas)
{
    const auto future = bafx::config::parseJson(
        std::string("{\"schemaVersion\":")
        + std::to_string(bafx::config::currentSchemaVersion + 1U)
        + "}");
    BAFX_CHECK(!future.succeeded());
    BAFX_CHECK(future.status == bafx::config::ConfigStatus::ValidationError);
    BAFX_CHECK(
        future.message.find("schemaVersion must equal")
        != std::string::npos);

    bafx::config::Config legacy = bafx::config::defaultConfig();
    legacy.performance.effectsMode = bafx::config::EffectsMode::Full;
    std::string legacyJson = bafx::config::toJson(legacy, false);
    const std::string currentVersionField = "\"schemaVersion\":"
        + std::to_string(bafx::config::currentSchemaVersion);
    const std::size_t versionPosition = legacyJson.find(currentVersionField);
    BAFX_CHECK(versionPosition != std::string::npos);
    legacyJson.replace(
        versionPosition,
        currentVersionField.size(),
        "\"schemaVersion\":14");
    const std::string effectsModeField = "\"effectsMode\":\"full\",";
    const std::size_t effectsModePosition = legacyJson.find(effectsModeField);
    BAFX_CHECK(effectsModePosition != std::string::npos);
    legacyJson.erase(effectsModePosition, effectsModeField.size());
    const std::string themeColorField = R"json("themeColor":"#4ca7ff",)json";
    const std::size_t themeColorPosition = legacyJson.find(themeColorField);
    BAFX_CHECK(themeColorPosition != std::string::npos);
    legacyJson.erase(themeColorPosition, themeColorField.size());
    const auto migrated = bafx::config::parseJson(legacyJson);
    bafx::test::check(migrated.succeeded(), migrated.message);
    BAFX_CHECK(migrated.config.schemaVersion == bafx::config::currentSchemaVersion);
    BAFX_CHECK(
        migrated.config.performance.effectsMode
        == bafx::config::EffectsMode::Full);
    BAFX_CHECK(migrated.config.effects.themeColor == "#4ca7ff");

    std::string schema15Json = bafx::config::toJson(legacy, false);
    const std::size_t schema15VersionPosition = schema15Json.find(currentVersionField);
    BAFX_CHECK(schema15VersionPosition != std::string::npos);
    schema15Json.replace(
        schema15VersionPosition,
        currentVersionField.size(),
        "\"schemaVersion\":15");
    const std::size_t schema15ThemeColorPosition = schema15Json.find(themeColorField);
    BAFX_CHECK(schema15ThemeColorPosition != std::string::npos);
    schema15Json.erase(schema15ThemeColorPosition, themeColorField.size());
    const auto migratedSchema15 = bafx::config::parseJson(schema15Json);
    bafx::test::check(migratedSchema15.succeeded(), migratedSchema15.message);
    BAFX_CHECK(
        migratedSchema15.config.schemaVersion
        == bafx::config::currentSchemaVersion);
    BAFX_CHECK(migratedSchema15.config.effects.themeColor == "#4ca7ff");

    std::string schema16Json = bafx::config::toJson(legacy, false);
    const std::size_t schema16VersionPosition = schema16Json.find(currentVersionField);
    BAFX_CHECK(schema16VersionPosition != std::string::npos);
    schema16Json.replace(
        schema16VersionPosition,
        currentVersionField.size(),
        "\"schemaVersion\":16");
    const std::string spout2EnabledField = R"json(,"spout2Enabled":false)json";
    const std::size_t spout2EnabledPosition = schema16Json.find(spout2EnabledField);
    BAFX_CHECK(spout2EnabledPosition != std::string::npos);
    schema16Json.erase(spout2EnabledPosition, spout2EnabledField.size());
    const auto migratedSchema16 = bafx::config::parseJson(schema16Json);
    bafx::test::check(migratedSchema16.succeeded(), migratedSchema16.message);
    BAFX_CHECK(
        migratedSchema16.config.schemaVersion
        == bafx::config::currentSchemaVersion);
    BAFX_CHECK(!migratedSchema16.config.system.spout2Enabled);

    std::string schema17Json = bafx::config::toJson(legacy, false);
    const std::size_t schema17VersionPosition = schema17Json.find(
        currentVersionField);
    BAFX_CHECK(schema17VersionPosition != std::string::npos);
    schema17Json.replace(
        schema17VersionPosition,
        currentVersionField.size(),
        "\"schemaVersion\":17");
    static constexpr std::string_view layerFields[] = {
        "bloomLayerEnabled",
        "clickShardsLayerEnabled",
        "diskLayerEnabled",
        "ringsLayerEnabled",
        "trailLayerEnabled",
        "trailShardsLayerEnabled"};
    for (const std::string_view field : layerFields)
    {
        const std::string serialized = "\"" + std::string(field)
            + "\":true,";
        const std::size_t position = schema17Json.find(serialized);
        BAFX_CHECK(position != std::string::npos);
        schema17Json.erase(position, serialized.size());
    }
    const auto migratedSchema17 = bafx::config::parseJson(schema17Json);
    bafx::test::check(migratedSchema17.succeeded(), migratedSchema17.message);
    BAFX_CHECK(migratedSchema17.config.effects.diskLayerEnabled);
    BAFX_CHECK(migratedSchema17.config.effects.ringsLayerEnabled);
    BAFX_CHECK(migratedSchema17.config.effects.clickShardsLayerEnabled);
    BAFX_CHECK(migratedSchema17.config.effects.trailShardsLayerEnabled);
    BAFX_CHECK(migratedSchema17.config.effects.trailLayerEnabled);
    BAFX_CHECK(migratedSchema17.config.effects.bloomLayerEnabled);

    std::string schema18Json = bafx::config::toJson(legacy, false);
    const std::size_t schema18VersionPosition = schema18Json.find(
        currentVersionField);
    BAFX_CHECK(schema18VersionPosition != std::string::npos);
    schema18Json.replace(
        schema18VersionPosition,
        currentVersionField.size(),
        "\"schemaVersion\":18");
    const std::string activeFxRoiField =
        R"json("activeFxRoiEnabled":false,)json";
    const std::size_t activeFxRoiPosition = schema18Json.find(
        activeFxRoiField);
    BAFX_CHECK(activeFxRoiPosition != std::string::npos);
    schema18Json.erase(activeFxRoiPosition, activeFxRoiField.size());
    const auto migratedSchema18 = bafx::config::parseJson(schema18Json);
    bafx::test::check(migratedSchema18.succeeded(), migratedSchema18.message);
    BAFX_CHECK(!migratedSchema18.config.performance.activeFxRoiEnabled);
}

BAFX_TEST(config_fx_patch_controls_each_effect_layer_atomically)
{
    const bafx::config::Config base = bafx::config::defaultConfig();
    const auto result = bafx::config::setFxParams(
        base,
        R"json({"effects.diskLayerEnabled":false,"effects.ringsLayerEnabled":false,"effects.clickShardsLayerEnabled":false,"effects.trailShardsLayerEnabled":false,"effects.trailLayerEnabled":false,"effects.bloomLayerEnabled":false})json");
    bafx::test::check(result.succeeded(), result.message);
    BAFX_CHECK(!result.config.effects.diskLayerEnabled);
    BAFX_CHECK(!result.config.effects.ringsLayerEnabled);
    BAFX_CHECK(!result.config.effects.clickShardsLayerEnabled);
    BAFX_CHECK(!result.config.effects.trailShardsLayerEnabled);
    BAFX_CHECK(!result.config.effects.trailLayerEnabled);
    BAFX_CHECK(!result.config.effects.bloomLayerEnabled);

    const auto rejected = bafx::config::setFxParams(
        result.config,
        R"json({"effects.diskLayerEnabled":true,"effects.ringsLayerEnabled":"false"})json");
    BAFX_CHECK(!rejected.succeeded());
    BAFX_CHECK(!rejected.config.effects.diskLayerEnabled);
    BAFX_CHECK(!rejected.config.effects.ringsLayerEnabled);
}

BAFX_TEST(config_patch_updates_spout2_switch_atomically)
{
    const bafx::config::Config base = bafx::config::defaultConfig();
    const auto enabled = bafx::config::applyPatchJson(
        base,
        R"json({"generation":3,"path":"system.spout2Enabled","value":true})json");
    BAFX_CHECK(enabled.succeeded());
    BAFX_CHECK(enabled.config.system.spout2Enabled);

    const auto rejected = bafx::config::applyPatchJson(
        enabled.config,
        R"json({"path":"system.spout2Enabled","value":"true"})json");
    BAFX_CHECK(!rejected.succeeded());
    BAFX_CHECK(rejected.config.system.spout2Enabled);
}

BAFX_TEST(config_patch_controls_active_fx_roi_switch)
{
    const bafx::config::Config base = bafx::config::defaultConfig();
    const auto enabled = bafx::config::applyPatchJson(
        base,
        R"json({"generation":2,"path":"performance.activeFxRoiEnabled","value":true})json");
    bafx::test::check(enabled.succeeded(), enabled.message);
    BAFX_CHECK(enabled.config.performance.activeFxRoiEnabled);

    const auto rejected = bafx::config::applyPatchJson(
        enabled.config,
        R"json({"path":"performance.activeFxRoiEnabled","value":"true"})json");
    BAFX_CHECK(!rejected.succeeded());
    BAFX_CHECK(rejected.config.performance.activeFxRoiEnabled);
}

BAFX_TEST(config_patch_controls_effects_mode)
{
    const bafx::config::Config base = bafx::config::defaultConfig();
    const auto result = bafx::config::applyPatchJson(
        base,
        "{\"generation\":1,\"path\":\"performance.effectsMode\",\"value\":\"core\"}");
    BAFX_CHECK(result.succeeded());
    BAFX_CHECK(
        result.config.performance.effectsMode
        == bafx::config::EffectsMode::Core);
    BAFX_CHECK(
        bafx::config::toJson(result.config, false).find(
            "\"effectsMode\":\"core\"")
        != std::string::npos);
}

BAFX_TEST(config_patch_round_trips_unlimited_frame_pacing)
{
    const bafx::config::Config base = bafx::config::defaultConfig();
    const auto result = bafx::config::applyPatchJson(
        base,
        R"json({"generation":2,"path":"performance.framePacing","value":"unlimited"})json");
    BAFX_CHECK(result.succeeded());
    BAFX_CHECK(
        result.config.performance.framePacing
        == bafx::config::FramePacing::Unlimited);

    std::string error;
    bafx::config::Config withOverride = result.config;
    BAFX_CHECK(bafx::config::setDisplayOverride(
        withOverride,
        bafx::config::DisplayOverrideConfig{
            "displayconfig-v1-sha256:unlimited",
            true,
            false,
            bafx::config::FramePacing::Unlimited},
        &error));
    const std::string serialized = bafx::config::toJson(withOverride, false);
    BAFX_CHECK(
        serialized.find("\"framePacing\":\"unlimited\"")
        != std::string::npos);

    const auto roundTrip = bafx::config::parseJson(serialized);
    BAFX_CHECK(roundTrip.succeeded());
    BAFX_CHECK(
        roundTrip.config.performance.framePacing
        == bafx::config::FramePacing::Unlimited);
    BAFX_CHECK(
        roundTrip.config.display.overrides.front().framePacing
        == bafx::config::FramePacing::Unlimited);
}

BAFX_TEST(config_current_schema_requires_every_section_and_field)
{
    const std::string currentSchema = std::to_string(
        bafx::config::currentSchemaVersion);
    const auto missingSection = bafx::config::parseJson(
        std::string("{\"schemaVersion\":") + currentSchema + "}");
    BAFX_CHECK(
        missingSection.status == bafx::config::ConfigStatus::ValidationError);
    BAFX_CHECK(
        missingSection.message.find("config section 'effects' is required")
        != std::string::npos);

    bafx::config::Config config = bafx::config::defaultConfig();
    config.background.allowSystemBorder = false;
    std::string document = bafx::config::toJson(config, false);
    const std::string field = R"json("hdrEnabled":false,)json";
    const std::size_t fieldPosition = document.find(field);
    BAFX_CHECK(fieldPosition != std::string::npos);
    document.erase(fieldPosition, field.size());

    const auto missingField = bafx::config::parseJson(document);
    BAFX_CHECK(
        missingField.status == bafx::config::ConfigStatus::ValidationError);
    BAFX_CHECK(
        missingField.message.find("config field 'display.hdrEnabled' is required")
        != std::string::npos);

    document = bafx::config::toJson(config, false);
    const std::string overridesField = R"json(,"overrides":[])json";
    const std::size_t overridesPosition = document.find(overridesField);
    BAFX_CHECK(overridesPosition != std::string::npos);
    document.erase(overridesPosition, overridesField.size());

    const auto missingOverrides = bafx::config::parseJson(document);
    BAFX_CHECK(
        missingOverrides.status
        == bafx::config::ConfigStatus::ValidationError);
    BAFX_CHECK(
        missingOverrides.message.find(
            "config field 'display.overrides' is required")
        != std::string::npos);

    document = bafx::config::toJson(config, false);
    const std::string ringsCountField = R"json("ringsCount":2,)json";
    const std::size_t ringsCountPosition = document.find(ringsCountField);
    BAFX_CHECK(ringsCountPosition != std::string::npos);
    document.erase(ringsCountPosition, ringsCountField.size());

    const auto missingCurrentEffectField = bafx::config::parseJson(document);
    BAFX_CHECK(
        missingCurrentEffectField.status
        == bafx::config::ConfigStatus::ValidationError);
    BAFX_CHECK(
        missingCurrentEffectField.message.find(
            "config field 'effects.ringsCount' is required")
        != std::string::npos);

    document = bafx::config::toJson(config, false);
    const std::string themeColorField = R"json("themeColor":"#4ca7ff",)json";
    const std::size_t themeColorPosition = document.find(themeColorField);
    BAFX_CHECK(themeColorPosition != std::string::npos);
    document.erase(themeColorPosition, themeColorField.size());

    const auto missingThemeColor = bafx::config::parseJson(document);
    BAFX_CHECK(
        missingThemeColor.status
        == bafx::config::ConfigStatus::ValidationError);
    BAFX_CHECK(
        missingThemeColor.message.find(
            "config field 'effects.themeColor' is required")
        != std::string::npos);

    document = bafx::config::toJson(config, false);
    const std::string spout2EnabledField = R"json(,"spout2Enabled":false)json";
    const std::size_t spout2EnabledPosition = document.find(spout2EnabledField);
    BAFX_CHECK(spout2EnabledPosition != std::string::npos);
    document.erase(spout2EnabledPosition, spout2EnabledField.size());

    const auto missingSpout2Enabled = bafx::config::parseJson(document);
    BAFX_CHECK(
        missingSpout2Enabled.status
        == bafx::config::ConfigStatus::ValidationError);
    BAFX_CHECK(
        missingSpout2Enabled.message.find(
            "config field 'system.spout2Enabled' is required")
        != std::string::npos);

    document = bafx::config::toJson(config, false);
    const std::string shardCountField = R"json("shardsClickCount":4,)json";
    const std::size_t shardCountPosition = document.find(shardCountField);
    BAFX_CHECK(shardCountPosition != std::string::npos);
    document.erase(shardCountPosition, shardCountField.size());

    const auto missingShardField = bafx::config::parseJson(document);
    BAFX_CHECK(
        missingShardField.status == bafx::config::ConfigStatus::ValidationError);
    BAFX_CHECK(
        missingShardField.message.find(
            "config field 'effects.shardsClickCount' is required")
        != std::string::npos);
}

BAFX_TEST(config_parser_rejects_invalid_documents_and_values)
{
    const std::string currentSchema = std::to_string(
        bafx::config::currentSchemaVersion);
    const auto missingVersion = bafx::config::parseJson("{}");
    BAFX_CHECK(!missingVersion.succeeded());
    BAFX_CHECK(missingVersion.status == bafx::config::ConfigStatus::ValidationError);

    const auto duplicateKey = bafx::config::parseJson(
        std::string("{\"schemaVersion\":")
            + currentSchema
            + ",\"schemaVersion\":"
            + currentSchema
            + "}");
    BAFX_CHECK(duplicateKey.status == bafx::config::ConfigStatus::ParseError);

    bafx::config::Config invalidConfig = bafx::config::defaultConfig();
    invalidConfig.effects.globalScale = 9.0F;
    const auto invalidScale = bafx::config::parseJson(
        bafx::config::toJson(invalidConfig, false));
    BAFX_CHECK(invalidScale.status == bafx::config::ConfigStatus::ValidationError);

    const auto malformed = bafx::config::parseJson(
        std::string("{\"schemaVersion\":")
            + currentSchema
            + ",\"effects\":{\"enabled\":tru}}");
    BAFX_CHECK(malformed.status == bafx::config::ConfigStatus::ParseError);
}

BAFX_TEST(config_atomic_save_load_and_failure_preserves_previous_file)
{
    const fs::path path = testPath();
    const fs::path root = path.parent_path();
    std::error_code cleanupError;
    fs::remove_all(root, cleanupError);

    bafx::config::Config value = bafx::config::defaultConfig();
    value.effects.globalScale = 1.75F;
    value.effects.bloomIntensity = 0.6F;
    value.input.trailOnlyWhilePressed = false;
    value.input.samplingRateHz = 30U;
    const auto saved = bafx::config::saveConfigAtomic(path, value);
    BAFX_CHECK(saved.succeeded());
    BAFX_CHECK(fs::exists(path));

    const auto loaded = bafx::config::loadConfig(path);
    BAFX_CHECK(loaded.status == bafx::config::ConfigStatus::Ok);
    BAFX_CHECK_NEAR(loaded.config.effects.globalScale, 1.75F, 0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.bloomIntensity, 0.6F, 0.00001F);
    BAFX_CHECK(!loaded.config.input.trailOnlyWhilePressed);
    BAFX_CHECK(loaded.config.input.samplingRateHz == 30U);

    const std::string beforeInvalidSave = readFile(path);
    value.effects.bloomIntensity = 10.1F;
    const auto invalidSave = bafx::config::saveConfigAtomic(path, value);
    BAFX_CHECK(invalidSave.status == bafx::config::ConfigStatus::ValidationError);
    BAFX_CHECK(readFile(path) == beforeInvalidSave);

    const fs::path missing = root / "missing.json";
    const auto defaults = bafx::config::loadConfig(missing);
    BAFX_CHECK(defaults.status == bafx::config::ConfigStatus::CreatedDefault);
    BAFX_CHECK(defaults.config.schemaVersion == bafx::config::currentSchemaVersion);

    removeTestTree(path);
}

BAFX_TEST(config_patch_updates_whitelisted_field_and_checks_generation)
{
    const bafx::config::Config base = bafx::config::defaultConfig();
    const auto patched = bafx::config::applyPatchJson(
        base,
        R"json({"generation":7,"path":"effects.globalScale","value":1.75})json");
    BAFX_CHECK(patched.recognized);
    BAFX_CHECK(patched.succeeded());
    BAFX_CHECK(patched.expectedGeneration.has_value());
    BAFX_CHECK(*patched.expectedGeneration == 7U);
    BAFX_CHECK_NEAR(patched.config.effects.globalScale, 1.75F, 0.00001F);

    const auto unknown = bafx::config::applyPatchJson(
        base,
        R"json({"path":"renderer.device","value":"warp"})json");
    BAFX_CHECK(unknown.recognized);
    BAFX_CHECK(!unknown.succeeded());

    const auto fullDocument = bafx::config::applyPatchJson(
        base,
        bafx::config::toJson(base, false));
    BAFX_CHECK(!fullDocument.recognized);

    const auto patchWithExtraField = bafx::config::applyPatchJson(
        base,
        R"json({"metadata":true,"path":"effects.enabled","value":false})json");
    BAFX_CHECK(patchWithExtraField.recognized);
    BAFX_CHECK(!patchWithExtraField.succeeded());
    BAFX_CHECK(
        patchWithExtraField.status
        == bafx::config::ConfigStatus::ValidationError);
    BAFX_CHECK(
        patchWithExtraField.config.effects.enabled
        == base.effects.enabled);
}

BAFX_TEST(config_preserves_unicode_paths_and_rejects_unknown_enum_values)
{
    const fs::path path = testPath().parent_path() / L"配置.json";
    const fs::path root = path.parent_path();
    std::error_code cleanupError;
    fs::remove_all(root, cleanupError);

    bafx::config::Config value = bafx::config::defaultConfig();
    BAFX_CHECK(bafx::config::saveConfigAtomic(path, value).succeeded());
    const auto loaded = bafx::config::loadConfig(path);
    BAFX_CHECK(loaded.status == bafx::config::ConfigStatus::Ok);

    value.background.mode = static_cast<bafx::config::RenderMode>(255U);
    const auto invalid = bafx::config::saveConfigAtomic(path, value);
    BAFX_CHECK(invalid.status == bafx::config::ConfigStatus::ValidationError);

    removeTestTree(path);
}
