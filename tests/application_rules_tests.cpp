#include "test_support.hpp"
#include "application_rule_monitor.hpp"

using bafx::config::ApplicationMatchMode;
using bafx::config::ApplicationRulesConfig;
using bafx::desktop::ApplicationRuleState;
using bafx::desktop::ApplicationRuleTracker;
using bafx::windows::ProcessSnapshot;

BAFX_TEST(application_rules_match_all_instances_and_debounce_only_complete_scans)
{
    ApplicationRuleTracker tracker;
    tracker.configure({true, {{true, ApplicationMatchMode::ExecutablePath, "D:\\Games\\Game.exe"}}});
    const auto revision = tracker.runtime().revision;
    ProcessSnapshot running{{{10U, L"game.EXE", L"d:\\games\\GAME.exe"},
        {11U, L"Game.exe", L"D:\\Games\\Game.exe"}}, ERROR_SUCCESS, true};
    tracker.observe(revision, running);
    BAFX_CHECK(tracker.runtime().suppressed);
    running.processes.erase(running.processes.begin());
    tracker.observe(revision, running);
    BAFX_CHECK(tracker.runtime().suppressed);
    tracker.observe(revision, {{}, ERROR_SUCCESS, true});
    BAFX_CHECK(tracker.runtime().states[0] == ApplicationRuleState::Waiting);
    tracker.observe(revision, {{}, ERROR_ACCESS_DENIED, false});
    tracker.observe(revision, {{}, ERROR_SUCCESS, true});
    BAFX_CHECK(tracker.runtime().suppressed);
    tracker.observe(revision, {{}, ERROR_SUCCESS, true});
    BAFX_CHECK(!tracker.runtime().suppressed);
}

BAFX_TEST(application_rules_path_failure_never_broadens_match_or_proves_exit)
{
    ApplicationRuleTracker tracker;
    tracker.configure({true, {{true, ApplicationMatchMode::ExecutablePath, "D:\\Game.exe"}}});
    const auto revision = tracker.runtime().revision;
    const ProcessSnapshot inaccessible{{{10U, L"Game.exe", {}, ERROR_ACCESS_DENIED}}, ERROR_SUCCESS, true};
    tracker.observe(revision, inaccessible);
    BAFX_CHECK(!tracker.runtime().suppressed);
    BAFX_CHECK(tracker.runtime().states[0] == ApplicationRuleState::Unknown);
    tracker.observe(revision, {{{10U, L"Game.exe", L"D:\\Game.exe"}}, ERROR_SUCCESS, true});
    tracker.observe(revision, inaccessible);
    BAFX_CHECK(tracker.runtime().suppressed);
    tracker.configure({true, {{true, ApplicationMatchMode::ExecutableName, "game.exe"}}});
    tracker.observe(tracker.runtime().revision, inaccessible);
    BAFX_CHECK(tracker.runtime().suppressed);
}

BAFX_TEST(application_rules_edits_remove_only_their_own_holds_and_ignore_stale_results)
{
    ApplicationRuleTracker tracker;
    ApplicationRulesConfig config{true, {
        {true, ApplicationMatchMode::ExecutableName, "a.exe"},
        {true, ApplicationMatchMode::ExecutableName, "b.exe"}}};
    tracker.configure(config);
    const auto oldRevision = tracker.runtime().revision;
    const ProcessSnapshot both{{{1U, L"a.exe"}, {2U, L"b.exe"}}, ERROR_SUCCESS, true};
    tracker.observe(oldRevision, both);
    config.rules.erase(config.rules.begin());
    tracker.configure(config);
    BAFX_CHECK(tracker.runtime().suppressed);
    BAFX_CHECK(tracker.runtime().reason == "b.exe");
    config.enabled = false;
    tracker.configure(config);
    tracker.observe(oldRevision, both);
    BAFX_CHECK(!tracker.runtime().suppressed);
    BAFX_CHECK(!tracker.active());
    config.enabled = true;
    tracker.configure(config);
    tracker.observe(oldRevision, both);
    BAFX_CHECK(!tracker.runtime().suppressed);
    tracker.observe(tracker.runtime().revision, both);
    BAFX_CHECK(tracker.runtime().suppressed);
    config.rules.clear();
    tracker.configure(config);
    BAFX_CHECK(!tracker.runtime().suppressed);
}

BAFX_TEST(application_rules_initial_scan_and_current_session_catalog)
{
    const auto path = bafx::windows::queryProcessExecutable(GetCurrentProcessId());
    BAFX_CHECK(!path.empty());
    const auto name = path.filename().native();
    const std::array<std::wstring, 1> names{name};
    const auto processes = bafx::windows::queryProcessSnapshot(names);
    BAFX_CHECK(processes.complete);
    BAFX_CHECK(std::any_of(processes.processes.begin(), processes.processes.end(), [](const auto& process)
    {
        return process.id == GetCurrentProcessId() && process.sessionKnown && !process.executablePath.empty();
    }));
    bafx::desktop::ApplicationRuleMonitor monitor({true, {{true,
        ApplicationMatchMode::ExecutablePath, bafx::windows::processTextUtf8(path.native())}}});
    BAFX_CHECK(monitor.snapshot().suppressed);
    monitor.configure({});
    BAFX_CHECK(!monitor.snapshot().suppressed);
    monitor.stop();
}
