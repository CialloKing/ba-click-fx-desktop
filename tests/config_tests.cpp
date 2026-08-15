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
    BAFX_CHECK(
        defaults.background.mode
        == bafx::config::RenderMode::BackgroundAware);
    BAFX_CHECK(defaults.background.cursorExcluded);
    BAFX_CHECK(defaults.background.allowSystemBorder);
    BAFX_CHECK(defaults.input.trailOnlyWhilePressed);
    BAFX_CHECK(defaults.input.samplingRateHz == 0U);
    BAFX_CHECK_NEAR(defaults.effects.globalScale, 1.0F, 0.00001F);
    BAFX_CHECK_NEAR(defaults.effects.bloomIntensity, 1.0F, 0.00001F);

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
        parsed.config.effects.bloomIntensity,
        defaults.effects.bloomIntensity,
        0.00001F);
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

    const auto retiredAlias = bafx::config::applyPatchJson(
        base,
        R"json({"path":"background.mode","value":"classic"})json");
    BAFX_CHECK(!retiredAlias.succeeded());

    const auto lightBackground = bafx::config::applyPatchJson(
        base,
        R"json({"path":"background.mode","value":"light-background"})json");
    BAFX_CHECK(lightBackground.succeeded());
    BAFX_CHECK(lightBackground.config.background.mode
        == bafx::config::RenderMode::LightBackground);

    const auto retiredMode = bafx::config::applyPatchJson(
        base,
        R"json({"path":"background.mode","value":"fx-only"})json");
    BAFX_CHECK(!retiredMode.succeeded());
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
    for (std::uint32_t version = 1U;
         version < bafx::config::currentSchemaVersion;
         ++version)
    {
        const auto result = bafx::config::parseJson(
            std::string("{\"schemaVersion\":")
            + std::to_string(version)
            + "}");
        BAFX_CHECK(!result.succeeded());
        BAFX_CHECK(result.status == bafx::config::ConfigStatus::UnsupportedSchema);
    }
}

BAFX_TEST(config_current_schema_requires_every_section_and_field)
{
    const auto missingSection = bafx::config::parseJson(
        R"json({"schemaVersion":8})json");
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
}

BAFX_TEST(config_parser_rejects_invalid_documents_and_values)
{
    const auto missingVersion = bafx::config::parseJson("{}");
    BAFX_CHECK(!missingVersion.succeeded());
    BAFX_CHECK(missingVersion.status == bafx::config::ConfigStatus::ValidationError);

    const auto futureVersion = bafx::config::parseJson(
        R"json({"schemaVersion":99})json");
    BAFX_CHECK(futureVersion.status == bafx::config::ConfigStatus::UnsupportedSchema);

    const auto duplicateKey = bafx::config::parseJson(
        R"json({"schemaVersion":8,"schemaVersion":8})json");
    BAFX_CHECK(duplicateKey.status == bafx::config::ConfigStatus::ParseError);

    bafx::config::Config invalidConfig = bafx::config::defaultConfig();
    invalidConfig.effects.globalScale = 9.0F;
    const auto invalidScale = bafx::config::parseJson(
        bafx::config::toJson(invalidConfig, false));
    BAFX_CHECK(invalidScale.status == bafx::config::ConfigStatus::ValidationError);

    const auto malformed = bafx::config::parseJson(
        R"json({"schemaVersion":8,"effects":{"enabled":tru}})json");
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

    value.background.mode = static_cast<bafx::config::RenderMode>(255U);
    const auto invalid = bafx::config::saveConfigAtomic(path, value);
    BAFX_CHECK(invalid.status == bafx::config::ConfigStatus::ValidationError);

    removeTestTree(path);
}
