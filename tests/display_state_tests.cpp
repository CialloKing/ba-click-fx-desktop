#include "test_support.hpp"

#include "display_state.hpp"

#include <string>
#include <string_view>

namespace
{

constexpr std::string_view validEmptyDisplayState =
    R"({"schemaVersion":2,"runtimeGeneration":7,"configGeneration":11,"appliedConfigGeneration":10,"topologyStatus":"complete","topologyError":0,"offlineOverridesAuthoritative":true,"offlineOverrides":[],"sessions":[]})";

}

BAFX_TEST(display_state_schema_two_accepts_a_complete_empty_snapshot)
{
    const bafx::control_center::DisplayStateParseResult result =
        bafx::control_center::parseDisplayState(validEmptyDisplayState);

    BAFX_CHECK(result.succeeded());
    BAFX_CHECK(result.state->schemaVersion == 2U);
    BAFX_CHECK(result.state->runtimeGeneration == 7U);
    BAFX_CHECK(result.state->configGeneration == 11U);
    BAFX_CHECK(result.state->appliedConfigGeneration == 10U);
    BAFX_CHECK(result.state->offlineOverridesAuthoritative);
}

BAFX_TEST(display_state_schema_two_rejects_old_unknown_and_duplicate_fields)
{
    std::string oldSchema(validEmptyDisplayState);
    oldSchema.replace(oldSchema.find("\"schemaVersion\":2"), 17U,
        "\"schemaVersion\":1");
    BAFX_CHECK(!bafx::control_center::parseDisplayState(oldSchema).succeeded());

    std::string unknownField(validEmptyDisplayState);
    unknownField.insert(unknownField.size() - 1U, ",\"futureField\":true");
    BAFX_CHECK(
        !bafx::control_center::parseDisplayState(unknownField).succeeded());

    std::string duplicateField(validEmptyDisplayState);
    duplicateField.insert(
        duplicateField.find("\"runtimeGeneration\":"),
        "\"runtimeGeneration\":6,");
    BAFX_CHECK(
        !bafx::control_center::parseDisplayState(duplicateField).succeeded());
}

BAFX_TEST(display_state_schema_two_requires_authoritative_offline_topology)
{
    std::string contradictory(validEmptyDisplayState);
    contradictory.replace(
        contradictory.find("\"topologyStatus\":\"complete\""),
        27U,
        "\"topologyStatus\":\"incomplete\"");

    BAFX_CHECK(
        !bafx::control_center::parseDisplayState(contradictory).succeeded());
}
