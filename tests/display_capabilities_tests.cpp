#include "test_support.hpp"

#include "bafx/windows/display_capabilities.hpp"

namespace
{

[[nodiscard]] bafx::windows::ActiveDisplayMonitor completeColorPath()
{
    bafx::windows::ActiveDisplayMonitor display{};
    display.sourceIdentityResolved = true;
    display.displayConfigColorPathComplete = true;
    display.physicalTargets.push_back(
        bafx::windows::DisplayPhysicalTarget{});
    return display;
}

}

BAFX_TEST(incomplete_global_topology_can_use_complete_monitor_color_path)
{
    bafx::windows::DisplayTopologySnapshot topology{};
    topology.status = bafx::windows::DisplayTopologyStatus::Incomplete;
    topology.error = ERROR_NOT_FOUND;
    const bafx::windows::ActiveDisplayMonitor display = completeColorPath();

    BAFX_CHECK(bafx::windows::canQueryDisplayConfigColorState(
        topology,
        &display));
}

BAFX_TEST(display_color_query_rejects_incomplete_monitor_evidence)
{
    bafx::windows::DisplayTopologySnapshot topology{};
    topology.status = bafx::windows::DisplayTopologyStatus::Incomplete;
    topology.error = ERROR_NOT_FOUND;

    bafx::windows::ActiveDisplayMonitor display = completeColorPath();
    display.displayConfigColorPathComplete = false;
    BAFX_CHECK(!bafx::windows::canQueryDisplayConfigColorState(
        topology,
        &display));

    display = completeColorPath();
    display.sourceIdentityResolved = false;
    BAFX_CHECK(!bafx::windows::canQueryDisplayConfigColorState(
        topology,
        &display));

    display = completeColorPath();
    display.physicalTargets.clear();
    BAFX_CHECK(!bafx::windows::canQueryDisplayConfigColorState(
        topology,
        &display));
}

BAFX_TEST(display_color_query_rejects_unusable_global_topology)
{
    bafx::windows::DisplayTopologySnapshot topology{};
    const bafx::windows::ActiveDisplayMonitor display = completeColorPath();

    topology.status = bafx::windows::DisplayTopologyStatus::QueryFailed;
    BAFX_CHECK(!bafx::windows::canQueryDisplayConfigColorState(
        topology,
        &display));

    topology.status = bafx::windows::DisplayTopologyStatus::NoActiveDisplays;
    BAFX_CHECK(!bafx::windows::canQueryDisplayConfigColorState(
        topology,
        &display));
}
