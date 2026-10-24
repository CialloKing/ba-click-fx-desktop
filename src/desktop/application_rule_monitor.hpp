#pragma once

#include "bafx/config/config.hpp"
#include "bafx/windows/process_catalog.hpp"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace bafx::desktop
{

enum class ApplicationRuleState
{
    Disabled,
    Unknown,
    Matched,
    Clear,
    Waiting
};

[[nodiscard]] const char* applicationRuleStateName(ApplicationRuleState state) noexcept;

struct ApplicationRulesRuntime final
{
    std::uint64_t revision{0U};
    bool suppressed{false};
    std::string reason{};
    DWORD error{ERROR_SUCCESS};
    std::vector<ApplicationRuleState> states{};
    bool operator==(const ApplicationRulesRuntime&) const = default;
};

struct ApplicationRuleGate final
{
    bool suppressed{false};
    std::uint64_t generation{0U};
};

// Pure state reduction keeps process I/O outside the lock and makes stale
// observations, failed scans and the exit debounce independently testable.
class ApplicationRuleTracker final
{
public:
    void configure(const bafx::config::ApplicationRulesConfig& config);
    void observe(std::uint64_t revision, const bafx::windows::ProcessSnapshot& snapshot);
    [[nodiscard]] const ApplicationRulesRuntime& runtime() const noexcept;
    [[nodiscard]] std::vector<std::wstring> executableNames() const;
    [[nodiscard]] bool active() const noexcept;

private:
    void updateSuppression();
    bafx::config::ApplicationRulesConfig config_{};
    ApplicationRulesRuntime runtime_{};
    std::vector<bool> held_{};
    std::vector<unsigned> clearSamples_{};
};

class ApplicationRuleMonitor final
{
public:
    using Scanner = std::function<bafx::windows::ProcessSnapshot(std::span<const std::wstring>)>;
    explicit ApplicationRuleMonitor(const bafx::config::ApplicationRulesConfig& config,
        Scanner scanner = {});
    ~ApplicationRuleMonitor();
    ApplicationRuleMonitor(const ApplicationRuleMonitor&) = delete;
    ApplicationRuleMonitor& operator=(const ApplicationRuleMonitor&) = delete;
    void configure(const bafx::config::ApplicationRulesConfig& config);
    void stop() noexcept;
    [[nodiscard]] ApplicationRulesRuntime snapshot() const;
    [[nodiscard]] ApplicationRuleGate gate() const noexcept;

private:
    void run(std::uint64_t observedRevision) noexcept;
    Scanner scanner_;
    mutable std::mutex mutex_{};
    std::condition_variable changed_{};
    ApplicationRuleTracker tracker_{};
    bool stopping_{false};
    std::uint64_t generation_{1U};
    std::thread worker_{};
};

}
