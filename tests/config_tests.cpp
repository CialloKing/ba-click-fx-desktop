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
    BAFX_CHECK(
        defaults.background.mode
        == bafx::config::RenderMode::BackgroundAware);
    BAFX_CHECK(defaults.background.cursorExcluded);
    BAFX_CHECK(defaults.background.allowSystemBorder);
    BAFX_CHECK(defaults.input.trailOnlyWhilePressed);
    BAFX_CHECK(defaults.input.samplingRateHz == 0U);
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
}

BAFX_TEST(config_current_effect_fields_round_trip_through_file)
{
    const fs::path path = testPath();
    const fs::path root = path.parent_path();
    std::error_code cleanupError;
    fs::remove_all(root, cleanupError);

    bafx::config::Config value = bafx::config::defaultConfig();
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
             "\"lifetimeMs\":350",
             "\"count\":5",
             "\"radiusMin\":45",
             "\"radiusMax\":95",
             "\"angularVelocityMultiplier\":14.5",
             "\"rotationDirection\":0.5",
             "\"clickCount\":9",
             "\"clickLifetimeMinMs\":250",
             "\"clickLifetimeMaxMs\":850",
             "\"clickRadius\":72.5",
             "\"clickSpeedMin\":25",
             "\"clickSpeedMax\":125",
             "\"sizeMin\":12",
             "\"sizeMax\":44"})
    {
        BAFX_CHECK(fxConfig.find(fragment) != std::string::npos);
    }

    removeTestTree(path);
}

BAFX_TEST(config_fx_parameter_boundaries_normalize_web_units)
{
    const bafx::config::Config base = bafx::config::defaultConfig();

    const auto opacityMinimum = bafx::config::setFxParam(
        base,
        "opacity",
        "0");
    BAFX_CHECK(opacityMinimum.succeeded());
    BAFX_CHECK_NEAR(opacityMinimum.config.effects.opacity, 0.0F, 0.00001F);

    const auto opacityMaximum = bafx::config::setFxParam(
        base,
        "opacity",
        "1");
    BAFX_CHECK(opacityMaximum.succeeded());
    BAFX_CHECK_NEAR(opacityMaximum.config.effects.opacity, 1.0F, 0.00001F);

    const auto clickMinimum = bafx::config::setFxParam(
        base,
        "clickTimeScale",
        "0.01");
    BAFX_CHECK(clickMinimum.succeeded());
    BAFX_CHECK_NEAR(clickMinimum.config.effects.clickTimeScale, 0.01F, 0.00001F);

    const auto trailMaximum = bafx::config::setFxParam(
        base,
        "trailTimeScale",
        "4");
    BAFX_CHECK(trailMaximum.succeeded());
    BAFX_CHECK_NEAR(trailMaximum.config.effects.trailTimeScale, 4.0F, 0.00001F);

    const auto lifetime = bafx::config::setFxParam(
        base,
        "trail.lifetimeMs",
        "900");
    BAFX_CHECK(lifetime.succeeded());
    BAFX_CHECK_NEAR(lifetime.config.effects.trailLifetimeMs, 900.0F, 0.00001F);
    BAFX_CHECK_NEAR(lifetime.config.effects.trailLength, 3.0F, 0.00001F);

    const auto width = bafx::config::setFxParam(
        base,
        "trail.width",
        "10.8");
    BAFX_CHECK(width.succeeded());
    BAFX_CHECK_NEAR(width.config.effects.trailWidth, 4.0F, 0.00001F);

    const auto bloomIntensity = bafx::config::setFxParam(
        base,
        "bloom.intensity",
        "3.4");
    BAFX_CHECK(bloomIntensity.succeeded());
    BAFX_CHECK_NEAR(bloomIntensity.config.effects.bloomIntensity, 3.4F, 0.00001F);

    const auto diskRadius = bafx::config::setFxParam(
        base,
        "disk.radius",
        "48");
    BAFX_CHECK(diskRadius.succeeded());
    BAFX_CHECK_NEAR(diskRadius.config.effects.diskRadius, 48.0F, 0.00001F);

    const auto diskLifetime = bafx::config::setFxParam(
        base,
        "disk.lifetimeMs",
        "500");
    BAFX_CHECK(diskLifetime.succeeded());
    BAFX_CHECK_NEAR(
        diskLifetime.config.effects.diskLifetimeMs,
        500.0F,
        0.00001F);

    const auto ringsCount = bafx::config::setFxParam(
        base,
        "rings.count",
        "64");
    BAFX_CHECK(ringsCount.succeeded());
    BAFX_CHECK(ringsCount.config.effects.ringsCount == 64U);

    const auto ringsLifetime = bafx::config::setFxParam(
        base,
        "rings.lifetimeMs",
        "2000");
    BAFX_CHECK(ringsLifetime.succeeded());
    BAFX_CHECK_NEAR(
        ringsLifetime.config.effects.ringsLifetimeMs,
        2000.0F,
        0.00001F);

    const auto ringsRadiusMin = bafx::config::setFxParam(
        base,
        "rings.radiusMin",
        "100");
    BAFX_CHECK(ringsRadiusMin.succeeded());
    BAFX_CHECK_NEAR(
        ringsRadiusMin.config.effects.ringsRadiusMin,
        100.0F,
        0.00001F);

    const auto ringsRadiusMax = bafx::config::setFxParam(
        base,
        "rings.radiusMax",
        "30");
    BAFX_CHECK(ringsRadiusMax.succeeded());
    BAFX_CHECK_NEAR(
        ringsRadiusMax.config.effects.ringsRadiusMax,
        30.0F,
        0.00001F);

    const auto angularVelocity = bafx::config::setFxParam(
        base,
        "rings.angularVelocityMultiplier",
        "100");
    BAFX_CHECK(angularVelocity.succeeded());
    BAFX_CHECK_NEAR(
        angularVelocity.config.effects.ringsAngularVelocityMultiplier,
        100.0F,
        0.00001F);

    const auto rotationDirection = bafx::config::setFxParam(
        base,
        "rings.rotationDirection",
        "0.5");
    BAFX_CHECK(rotationDirection.succeeded());
    BAFX_CHECK_NEAR(
        rotationDirection.config.effects.ringsRotationDirection,
        0.5F,
        0.00001F);

    const auto ringIntensity = bafx::config::setFxParam(
        base,
        "rings.hdrIntensity",
        "4.5");
    BAFX_CHECK(ringIntensity.succeeded());
    BAFX_CHECK_NEAR(
        ringIntensity.config.effects.ringsHdrIntensity,
        4.5F,
        0.00001F);

    const auto shardIntensity = bafx::config::setFxParam(
        base,
        "shards.hdrIntensity",
        "7.5");
    BAFX_CHECK(shardIntensity.succeeded());
    BAFX_CHECK_NEAR(
        shardIntensity.config.effects.shardsHdrIntensity,
        7.5F,
        0.00001F);

    const auto shardCount = bafx::config::setFxParam(
        base,
        "shards.clickCount",
        "1000");
    BAFX_CHECK(shardCount.succeeded());
    BAFX_CHECK(shardCount.config.effects.shardsClickCount == 1000U);

    const auto shardLifetimeMin = bafx::config::setFxParam(
        base,
        "shards.clickLifetimeMinMs",
        "100");
    BAFX_CHECK(shardLifetimeMin.succeeded());
    BAFX_CHECK_NEAR(
        shardLifetimeMin.config.effects.shardsClickLifetimeMinMs,
        100.0F,
        0.00001F);

    const auto shardLifetimeMax = bafx::config::setFxParam(
        base,
        "shards.clickLifetimeMaxMs",
        "10000");
    BAFX_CHECK(shardLifetimeMax.succeeded());
    BAFX_CHECK_NEAR(
        shardLifetimeMax.config.effects.shardsClickLifetimeMaxMs,
        10000.0F,
        0.00001F);

    const auto shardRadius = bafx::config::setFxParam(
        base,
        "shards.clickRadius",
        "5000");
    BAFX_CHECK(shardRadius.succeeded());
    BAFX_CHECK_NEAR(
        shardRadius.config.effects.shardsClickRadius,
        5000.0F,
        0.00001F);

    const auto shardSpeedMin = bafx::config::setFxParam(
        base,
        "shards.clickSpeedMin",
        "0");
    BAFX_CHECK(shardSpeedMin.succeeded());
    BAFX_CHECK_NEAR(
        shardSpeedMin.config.effects.shardsClickSpeedMin,
        0.0F,
        0.00001F);

    const auto shardSpeedMax = bafx::config::setFxParam(
        base,
        "shards.clickSpeedMax",
        "5000");
    BAFX_CHECK(shardSpeedMax.succeeded());
    BAFX_CHECK_NEAR(
        shardSpeedMax.config.effects.shardsClickSpeedMax,
        5000.0F,
        0.00001F);

    const auto shardSizeMin = bafx::config::setFxParam(
        base,
        "shards.sizeMin",
        "0");
    BAFX_CHECK(shardSizeMin.succeeded());
    BAFX_CHECK_NEAR(
        shardSizeMin.config.effects.shardsSizeMin,
        0.0F,
        0.00001F);

    const auto shardSizeMax = bafx::config::setFxParam(
        base,
        "shards.sizeMax",
        "2000");
    BAFX_CHECK(shardSizeMax.succeeded());
    BAFX_CHECK_NEAR(
        shardSizeMax.config.effects.shardsSizeMax,
        2000.0F,
        0.00001F);

    const auto trailOpacity = bafx::config::setFxParam(
        base,
        "trail.trailOpacity",
        "0.4");
    BAFX_CHECK(trailOpacity.succeeded());
    BAFX_CHECK_NEAR(
        trailOpacity.config.effects.trailOpacity,
        0.4F,
        0.00001F);

    const auto diffusionMinimum = bafx::config::setFxParam(
        base,
        "bloom.diffusion",
        "0");
    BAFX_CHECK(diffusionMinimum.succeeded());
    BAFX_CHECK_NEAR(diffusionMinimum.config.effects.bloomDiffusion, 0.0F, 0.00001F);

    const auto clampMaximum = bafx::config::setFxParam(
        base,
        "bloom.clamp",
        "65504");
    BAFX_CHECK(clampMaximum.succeeded());
    BAFX_CHECK_NEAR(clampMaximum.config.effects.bloomClamp, 65504.0F, 0.00001F);

    for (const auto& invalid : {
             std::pair{"opacity", "-0.01"},
             std::pair{"opacity", "1.01"},
             std::pair{"clickTimeScale", "0.009"},
             std::pair{"trailTimeScale", "4.01"},
             std::pair{"disk.lifetimeMs", "0"},
             std::pair{"disk.lifetimeMs", "10001"},
             std::pair{"rings.count", "65"},
             std::pair{"rings.count", "2.5"},
             std::pair{"rings.lifetimeMs", "0"},
             std::pair{"rings.radiusMin", "-0.01"},
             std::pair{"rings.radiusMax", "2000.01"},
             std::pair{"rings.angularVelocityMultiplier", "100.01"},
             std::pair{"rings.rotationDirection", "-1.01"},
             std::pair{"rings.rotationDirection", "1.01"},
             std::pair{"shards.clickCount", "1001"},
             std::pair{"shards.clickCount", "2.5"},
             std::pair{"shards.clickLifetimeMinMs", "0"},
             std::pair{"shards.clickLifetimeMaxMs", "10001"},
             std::pair{"shards.clickLifetimeMinMs", "701"},
             std::pair{"shards.clickLifetimeMaxMs", "599"},
             std::pair{"shards.clickRadius", "5000.01"},
             std::pair{"shards.clickSpeedMin", "5000.01"},
             std::pair{"shards.clickSpeedMax", "-0.01"},
             std::pair{"shards.clickSpeedMin", "66.51"},
             std::pair{"shards.clickSpeedMax", "49.87"},
             std::pair{"shards.sizeMin", "2000.01"},
             std::pair{"shards.sizeMax", "-0.01"},
             std::pair{"shards.sizeMin", "33.26"},
             std::pair{"shards.sizeMax", "16.62"},
             std::pair{"bloom.softKnee", "1.01"},
             std::pair{"bloom.clamp", "-0.01"}})
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
}

BAFX_TEST(config_fx_parameter_batch_is_atomic_and_preserves_generation)
{
    const bafx::config::Config base = bafx::config::defaultConfig();
    const auto batch = bafx::config::setFxParams(
        base,
        R"json({"generation":7,"patch":{"opacity":0.25,"clickTimeScale":2,"trail.lifetimeMs":600,"disk.lifetimeMs":350,"rings.count":4,"rings.lifetimeMs":900,"rings.radiusMin":45,"rings.radiusMax":95,"rings.angularVelocityMultiplier":14.5,"rings.rotationDirection":0.5,"shards.clickCount":7,"shards.clickLifetimeMinMs":100,"shards.clickLifetimeMaxMs":200,"shards.clickRadius":75,"shards.clickSpeedMin":10,"shards.clickSpeedMax":20,"shards.sizeMin":1,"shards.sizeMax":2,"bloom.intensity":4.2}})json");
    BAFX_CHECK(batch.succeeded());
    BAFX_CHECK(batch.expectedGeneration.has_value());
    BAFX_CHECK(*batch.expectedGeneration == 7U);
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
        R"json({"generation":7,"patch":{"opacity":0.25,"bloom.softKnee":2}})json");
    BAFX_CHECK(!rejected.succeeded());
    BAFX_CHECK(rejected.config.effects.opacity == base.effects.opacity);
    BAFX_CHECK(rejected.config.effects.bloomSoftKnee == base.effects.bloomSoftKnee);

    const auto rejectedRange = bafx::config::setFxParams(
        base,
        R"json({"patch":{"opacity":0.25,"shards.clickLifetimeMinMs":900,"shards.clickLifetimeMaxMs":800}})json");
    BAFX_CHECK(!rejectedRange.succeeded());
    BAFX_CHECK(rejectedRange.config.effects.opacity == base.effects.opacity);
    BAFX_CHECK_NEAR(
        rejectedRange.config.effects.shardsClickLifetimeMinMs,
        base.effects.shardsClickLifetimeMinMs,
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
        "bloom.diffusion",
        "8.5");
    BAFX_CHECK(continuous.succeeded());
    BAFX_CHECK(
        bafx::config::bloomQualityForDiffusion(
            continuous.config.effects.bloomDiffusion)
        == bafx::config::BloomQuality::Custom);
    BAFX_CHECK(
        bafx::config::toJson(continuous.config, false).find("bloomQuality")
        == std::string::npos);

    const auto preset = bafx::config::setFxParam(
        continuous.config,
        "effects.bloomQuality",
        "\"low\"");
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
    for (const std::uint32_t version :
         {
             bafx::config::currentSchemaVersion - 1U,
             bafx::config::currentSchemaVersion + 1U
         })
    {
        const auto result = bafx::config::parseJson(
            std::string("{\"schemaVersion\":")
            + std::to_string(version)
            + "}");
        BAFX_CHECK(!result.succeeded());
        BAFX_CHECK(result.status == bafx::config::ConfigStatus::ValidationError);
        BAFX_CHECK(
            result.message.find("schemaVersion must equal")
            != std::string::npos);
    }
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
    const std::string field = R"json("hdrEnabled":false)json";
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
