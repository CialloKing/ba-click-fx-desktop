#include "test_support.hpp"

#include "bafx/config/config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

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
    BAFX_CHECK(defaults.background.mode == bafx::config::CaptureMode::FxOnly);
    BAFX_CHECK_NEAR(defaults.effects.globalScale, 1.0F, 0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.bloomIntensity, 1.0F, 0.00001F);

    const std::string document = bafx::config::toJson(defaults);
    const auto parsed = bafx::config::parseJson(document);
    BAFX_CHECK(parsed.succeeded());
    BAFX_CHECK(parsed.status == bafx::config::ConfigStatus::Ok);
    BAFX_CHECK(parsed.config.schemaVersion == defaults.schemaVersion);
    BAFX_CHECK(parsed.config.background.mode == defaults.background.mode);
    BAFX_CHECK_NEAR(
        parsed.config.effects.globalScale,
        defaults.effects.globalScale,
        0.00001F);
    BAFX_CHECK_NEAR(
        parsed.config.effects.bloomIntensity,
        defaults.effects.bloomIntensity,
        0.00001F);
}

BAFX_TEST(config_capture_modes_use_canonical_wire_values)
{
    const bafx::config::Config base = bafx::config::defaultConfig();

    BAFX_CHECK(bafx::config::toString(bafx::config::CaptureMode::FxOnly)
        == "fx-only");
    BAFX_CHECK(
        bafx::config::toString(bafx::config::CaptureMode::BackgroundAware)
        == "background-aware");
    BAFX_CHECK(
        bafx::config::toString(bafx::config::CaptureMode::RecordingCompatible)
        == "recording-compatible");

    const auto fxOnly = bafx::config::applyPatchJson(
        base,
        R"json({"path":"background.mode","value":"fx-only"})json");
    BAFX_CHECK(fxOnly.succeeded());
    BAFX_CHECK(fxOnly.config.background.mode
        == bafx::config::CaptureMode::FxOnly);

    const auto backgroundAware = bafx::config::applyPatchJson(
        base,
        R"json({"path":"background.mode","value":"background-aware"})json");
    BAFX_CHECK(backgroundAware.succeeded());
    BAFX_CHECK(backgroundAware.config.background.mode
        == bafx::config::CaptureMode::BackgroundAware);

    const auto recordingCompatible = bafx::config::applyPatchJson(
        base,
        R"json({"path":"background.mode","value":"recording-compatible"})json");
    BAFX_CHECK(recordingCompatible.succeeded());
    BAFX_CHECK(recordingCompatible.config.background.mode
        == bafx::config::CaptureMode::RecordingCompatible);

    // Uppercase enum names were emitted by an early Control Center build. Keep
    // them rejected so every writer converges on the canonical wire contract.
    const auto legacyUppercase = bafx::config::applyPatchJson(
        base,
        R"json({"path":"background.mode","value":"BACKGROUND_AWARE"})json");
    BAFX_CHECK(!legacyUppercase.succeeded());
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

BAFX_TEST(config_migration_maps_legacy_keys)
{
    const auto result = bafx::config::parseJson(R"json(
        {
            "schemaVersion": 1,
            "enabled": false,
            "scale": 1.75,
            "trail": false,
            "trailLength": 2.0,
            "trailWidth": 1.5,
            "bloom": 0.35,
            "backgroundMode": "recording-compatible"
        }
    )json");
    BAFX_CHECK(result.status == bafx::config::ConfigStatus::Migrated);
    BAFX_CHECK(result.migrated());
    BAFX_CHECK(result.config.schemaVersion == bafx::config::currentSchemaVersion);
    BAFX_CHECK(!result.config.effects.enabled);
    BAFX_CHECK(!result.config.effects.trailEnabled);
    BAFX_CHECK(result.config.background.mode
        == bafx::config::CaptureMode::RecordingCompatible);
    BAFX_CHECK_NEAR(result.config.effects.globalScale, 1.75F, 0.00001F);
    BAFX_CHECK_NEAR(result.config.effects.bloomIntensity, 0.35F, 0.00001F);
}

BAFX_TEST(config_parser_rejects_invalid_documents_and_values)
{
    const auto missingVersion = bafx::config::parseJson("{}");
    BAFX_CHECK(missingVersion.succeeded());
    BAFX_CHECK(missingVersion.status == bafx::config::ConfigStatus::Migrated);

    const auto futureVersion = bafx::config::parseJson(
        R"json({"schemaVersion":99})json");
    BAFX_CHECK(futureVersion.status == bafx::config::ConfigStatus::UnsupportedSchema);

    const auto duplicateKey = bafx::config::parseJson(
        R"json({"schemaVersion":3,"schemaVersion":3})json");
    BAFX_CHECK(duplicateKey.status == bafx::config::ConfigStatus::ParseError);

    const auto invalidScale = bafx::config::parseJson(
        R"json({"schemaVersion":3,"effects":{"globalScale":9.0}})json");
    BAFX_CHECK(invalidScale.status == bafx::config::ConfigStatus::ValidationError);

    const auto malformed = bafx::config::parseJson(
        R"json({"schemaVersion":3,"effects":{"enabled":tru}})json");
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
    const auto saved = bafx::config::saveConfigAtomic(path, value);
    BAFX_CHECK(saved.succeeded());
    BAFX_CHECK(fs::exists(path));

    const auto loaded = bafx::config::loadConfig(path);
    BAFX_CHECK(loaded.status == bafx::config::ConfigStatus::Ok);
    BAFX_CHECK_NEAR(loaded.config.effects.globalScale, 1.75F, 0.00001F);
    BAFX_CHECK_NEAR(loaded.config.effects.bloomIntensity, 0.6F, 0.00001F);

    const std::string beforeInvalidSave = readFile(path);
    value.effects.bloomIntensity = 9.0F;
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

    value.background.mode = static_cast<bafx::config::CaptureMode>(255U);
    const auto invalid = bafx::config::saveConfigAtomic(path, value);
    BAFX_CHECK(invalid.status == bafx::config::ConfigStatus::ValidationError);

    removeTestTree(path);
}
