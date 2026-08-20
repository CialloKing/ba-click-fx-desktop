#include "test_support.hpp"

#include "obs_spout_plugin_probe.hpp"

#include <array>
#include <span>

using namespace bafx::control_center;

BAFX_TEST(obs_spout_plugin_probe_classifies_install_and_load_states)
{
    BAFX_CHECK(
        classifyObsSpoutPlugin(false, {})
        == ObsSpoutPluginState::Missing);
    BAFX_CHECK(
        classifyObsSpoutPlugin(true, {})
        == ObsSpoutPluginState::InstalledObsNotRunning);

    constexpr std::array loaded{
        ObsProcessModuleEvidence{true, false},
        ObsProcessModuleEvidence{true, true}};
    BAFX_CHECK(
        classifyObsSpoutPlugin(true, loaded)
        == ObsSpoutPluginState::Loaded);

    constexpr std::array notLoaded{
        ObsProcessModuleEvidence{true, false},
        ObsProcessModuleEvidence{true, false}};
    BAFX_CHECK(
        classifyObsSpoutPlugin(true, notLoaded)
        == ObsSpoutPluginState::InstalledNotLoaded);
}

BAFX_TEST(obs_spout_plugin_probe_does_not_misreport_access_denied)
{
    constexpr std::array unavailable{
        ObsProcessModuleEvidence{true, false},
        ObsProcessModuleEvidence{false, false}};
    BAFX_CHECK(
        classifyObsSpoutPlugin(true, unavailable)
        == ObsSpoutPluginState::InspectionUnavailable);
    BAFX_CHECK(
        classifyObsSpoutPlugin(false, unavailable)
        == ObsSpoutPluginState::InspectionUnavailable);
}
