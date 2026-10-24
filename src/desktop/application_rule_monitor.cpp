#include "application_rule_monitor.hpp"

#include <algorithm>
#include <chrono>

namespace bafx::desktop
{
namespace
{

std::string basename(const std::string_view target)
{
    const auto separator = target.find_last_of("/\\");
    return std::string(target.substr(separator == std::string_view::npos ? 0U : separator + 1U));
}

}

const char* applicationRuleStateName(const ApplicationRuleState state) noexcept
{
    switch (state)
    {
    case ApplicationRuleState::Disabled: return "disabled";
    case ApplicationRuleState::Unknown: return "unknown";
    case ApplicationRuleState::Matched: return "matched";
    case ApplicationRuleState::Clear: return "clear";
    case ApplicationRuleState::Waiting: return "waiting";
    }
    return "unknown";
}

void ApplicationRuleTracker::configure(const bafx::config::ApplicationRulesConfig& config)
{
    if (runtime_.revision != 0U && config_ == config)
    {
        return;
    }
    std::vector<bool> held(config.rules.size(), false);
    if (config.enabled && config_.enabled)
    {
        for (std::size_t i = 0U; i < config.rules.size(); ++i)
        {
            const auto& rule = config.rules[i];
            if (!rule.enabled)
            {
                continue;
            }
            for (std::size_t old = 0U; old < config_.rules.size(); ++old)
            {
                if (config_.rules[old].matchMode == rule.matchMode
                    && bafx::config::applicationTargetKey(config_.rules[old].target)
                        == bafx::config::applicationTargetKey(rule.target))
                {
                    held[i] = held_[old];
                }
            }
        }
    }
    config_ = config;
    held_ = std::move(held);
    clearSamples_.assign(config.rules.size(), 0U);
    ++runtime_.revision;
    runtime_.error = ERROR_SUCCESS;
    runtime_.states.clear();
    for (const auto& rule : config.rules)
    {
        runtime_.states.push_back(config.enabled && rule.enabled
            ? ApplicationRuleState::Unknown : ApplicationRuleState::Disabled);
    }
    // Removing a held rule withdraws that reason immediately; a new rule must
    // earn its own match instead of inheriting another program's pause.
    updateSuppression();
}

void ApplicationRuleTracker::observe(
    const std::uint64_t revision, const bafx::windows::ProcessSnapshot& snapshot)
{
    if (revision != runtime_.revision || !active())
    {
        return;
    }
    runtime_.error = snapshot.error;
    for (std::size_t i = 0U; i < config_.rules.size(); ++i)
    {
        const auto& rule = config_.rules[i];
        if (!rule.enabled)
        {
            continue;
        }
        const auto target = bafx::config::applicationTargetKey(rule.target);
        const auto name = basename(target);
        bool matched = false;
        bool unknown = !snapshot.complete;
        for (const auto& process : snapshot.processes)
        {
            if (bafx::config::applicationTargetKey(
                bafx::windows::processTextUtf8(process.executableName)) != name)
            {
                continue;
            }
            if (!process.sessionKnown)
            {
                unknown = true;
                continue;
            }
            if (rule.matchMode == bafx::config::ApplicationMatchMode::ExecutableName)
            {
                matched = true;
            }
            else if (process.executablePath.empty())
            {
                unknown = true;
                runtime_.error = process.pathError;
            }
            else if (bafx::config::applicationTargetKey(
                bafx::windows::processTextUtf8(process.executablePath.native())) == target)
            {
                matched = true;
            }
        }
        if (matched)
        {
            held_[i] = true;
            clearSamples_[i] = 0U;
            runtime_.states[i] = ApplicationRuleState::Matched;
        }
        else if (unknown)
        {
            clearSamples_[i] = 0U;
            runtime_.states[i] = ApplicationRuleState::Unknown;
        }
        else
        {
            if (held_[i] && ++clearSamples_[i] >= 2U)
            {
                held_[i] = false;
            }
            runtime_.states[i] = held_[i] ? ApplicationRuleState::Waiting : ApplicationRuleState::Clear;
        }
    }
    updateSuppression();
}

void ApplicationRuleTracker::updateSuppression()
{
    runtime_.suppressed = false;
    runtime_.reason.clear();
    for (std::size_t i = 0U; i < held_.size(); ++i)
    {
        if (held_[i])
        {
            runtime_.suppressed = true;
            if (!runtime_.reason.empty())
            {
                runtime_.reason += ", ";
            }
            runtime_.reason += basename(config_.rules[i].target);
        }
    }
}

const ApplicationRulesRuntime& ApplicationRuleTracker::runtime() const noexcept
{
    return runtime_;
}

bool ApplicationRuleTracker::active() const noexcept
{
    return config_.enabled && std::any_of(config_.rules.begin(), config_.rules.end(), [](const auto& rule)
    {
        return rule.enabled;
    });
}

std::vector<std::wstring> ApplicationRuleTracker::executableNames() const
{
    std::vector<std::wstring> result;
    if (active())
    {
        for (const auto& rule : config_.rules)
        {
            if (rule.enabled)
            {
                result.push_back(bafx::windows::processTextWide(basename(rule.target)));
            }
        }
    }
    return result;
}

ApplicationRuleMonitor::ApplicationRuleMonitor(
    const bafx::config::ApplicationRulesConfig& config, Scanner scanner)
    : scanner_(scanner ? std::move(scanner) : Scanner([](const auto names)
    {
        return bafx::windows::queryProcessSnapshot(names);
    }))
{
    tracker_.configure(config);
    // This first observation happens before the Host creates visible surfaces
    // or starts WGC. Subsequent enumerations never run on the render thread.
    if (tracker_.active())
    {
        try
        {
            auto observed = tracker_;
            observed.observe(observed.runtime().revision, scanner_(observed.executableNames()));
            tracker_ = std::move(observed);
        }
        catch (...)
        {
            // Startup follows the same unknown-result policy as later scans;
            // a scanner failure must not prevent the user's Host from starting.
            tracker_.observe(tracker_.runtime().revision, {{}, ERROR_GEN_FAILURE, false});
        }
    }
    worker_ = std::thread([this, revision = tracker_.runtime().revision]()
    {
        run(revision);
    });
}

ApplicationRuleMonitor::~ApplicationRuleMonitor()
{
    stop();
}

void ApplicationRuleMonitor::stop() noexcept
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
}

void ApplicationRuleMonitor::configure(const bafx::config::ApplicationRulesConfig& config)
{
    {
        std::lock_guard lock(mutex_);
        const auto revision = tracker_.runtime().revision;
        tracker_.configure(config);
        if (revision != tracker_.runtime().revision)
        {
            ++generation_;
        }
    }
    changed_.notify_all();
}

ApplicationRulesRuntime ApplicationRuleMonitor::snapshot() const
{
    std::lock_guard lock(mutex_);
    return tracker_.runtime();
}

ApplicationRuleGate ApplicationRuleMonitor::gate() const noexcept
{
    std::lock_guard lock(mutex_);
    return {tracker_.runtime().suppressed, generation_};
}

void ApplicationRuleMonitor::run(std::uint64_t observedRevision) noexcept
{
    std::unique_lock lock(mutex_);
    while (!stopping_)
    {
        const auto changed = [this, observedRevision]()
        {
            return stopping_ || tracker_.runtime().revision != observedRevision;
        };
        if (!tracker_.active())
        {
            changed_.wait(lock, changed);
        }
        else if (tracker_.runtime().revision == observedRevision)
        {
            changed_.wait_for(lock, std::chrono::seconds(1), changed);
        }
        if (stopping_)
        {
            break;
        }
        observedRevision = tracker_.runtime().revision;
        if (!tracker_.active())
        {
            continue;
        }
        try
        {
            const auto names = tracker_.executableNames();
            lock.unlock();
            const auto observation = scanner_(names);
            lock.lock();
            const auto previous = tracker_.runtime();
            // Publish a whole observation: a conversion failure midway through
            // a scan must not leave only some rules using the new sample.
            auto observed = tracker_;
            observed.observe(observedRevision, observation);
            tracker_ = std::move(observed);
            if (previous != tracker_.runtime())
            {
                ++generation_;
            }
        }
        catch (...)
        {
            if (!lock.owns_lock())
            {
                lock.lock();
            }
            // A failed scanner never counts as an empty successful scan.
            try
            {
                const auto previous = tracker_.runtime();
                tracker_.observe(observedRevision, {{}, ERROR_GEN_FAILURE, false});
                if (previous != tracker_.runtime())
                {
                    ++generation_;
                }
            }
            catch (...)
            {
                // Retain the last valid state if even error publication fails.
            }
        }
    }
}

}
