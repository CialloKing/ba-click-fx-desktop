#pragma once

#include "demo_scenario.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>

namespace bafx::desktop
{

struct RunOptions
{
    std::optional<std::uint32_t> frameLimit{};
    std::optional<std::uint32_t> demoAgeMilliseconds{};
    std::optional<std::uint32_t> quitAfterMilliseconds{};
    std::optional<std::filesystem::path> supportInfoPath{};
    bool supportInfoOnly{false};
    bool smokeTest{false};
    bool recoveryProbe{false};
    bool framePacingStallProbe{false};
    bool demoClick{false};
    bool disableRawInput{false};
    bool spout2{false};
    std::uint32_t demoDelayMilliseconds{0U};
    bafx::desktop::DemoScenario demoScenario{
        bafx::desktop::DemoScenario::CenterClick};
};

[[nodiscard]] RunOptions parseRunOptions();

// Diagnostic launches must not change the user's login startup registration.
[[nodiscard]] bool productSystemIntegrationEnabled(const RunOptions& options) noexcept;

}
