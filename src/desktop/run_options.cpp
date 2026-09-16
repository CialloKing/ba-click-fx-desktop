#include "run_options.hpp"

#include <cstdlib>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bafx::desktop
{

[[nodiscard]] bool productSystemIntegrationEnabled(
    const RunOptions& options) noexcept
{
    return !options.frameLimit.has_value()
        && !options.demoAgeMilliseconds.has_value()
        && !options.quitAfterMilliseconds.has_value()
        && !options.supportInfoOnly
        && !options.smokeTest
        && !options.recoveryProbe
        && !options.framePacingStallProbe
        && !options.demoClick
        && !options.disableRawInput
        && !options.spout2;
}

[[nodiscard]] RunOptions parseRunOptions()
{
    RunOptions options{};
    bool demoScenarioSpecified = false;
    const int argumentCount = __argc;
    for (int index = 1; index < argumentCount; ++index)
    {
        const std::wstring_view argument(__wargv[index]);
        if (argument == L"--smoke-test")
        {
            options.smokeTest = true;
            options.demoClick = true;
            options.demoAgeMilliseconds = 130U;
            options.frameLimit = 3U;
        }
        else if (argument == L"--device-recovery-probe")
        {
            options.recoveryProbe = true;
            options.smokeTest = true;
            options.demoClick = true;
            options.demoAgeMilliseconds = 130U;
            options.frameLimit = 2U;
        }
        else if (argument == L"--frame-pacing-stall-probe")
        {
            // This internal probe replaces the DXGI latency handle with a
            // permanently unsignaled event to verify bounded Host shutdown.
            options.framePacingStallProbe = true;
            options.disableRawInput = true;
        }
        else if (argument == L"--support-info")
        {
            options.supportInfoPath = std::filesystem::path(L"ba-click-fx-support.txt");
            options.supportInfoOnly = true;
        }
        else if (argument.starts_with(L"--support-info="))
        {
            const std::wstring_view value = argument.substr(15);
            options.supportInfoPath = value.empty()
                ? std::filesystem::path(L"ba-click-fx-support.txt")
                : std::filesystem::path(std::wstring(value));
            options.supportInfoOnly = true;
        }
        else if (argument == L"--demo-click")
        {
            options.demoClick = true;
        }
        else if (argument.starts_with(L"--demo-scenario="))
        {
            if (demoScenarioSpecified)
            {
                throw std::invalid_argument(
                    "--demo-scenario may be specified only once");
            }
            const std::optional<bafx::desktop::DemoScenario> scenario =
                bafx::desktop::parseDemoScenario(argument.substr(16U));
            if (!scenario.has_value())
            {
                throw std::invalid_argument(
                    "--demo-scenario requires center-click, interior-trail, "
                    "or boundary-top-left");
            }
            demoScenarioSpecified = true;
            options.demoClick = true;
            options.demoScenario = *scenario;
        }
        else if (argument == L"--disable-raw-input")
        {
            // Deterministic renderer baselines provide their own harmless
            // message pressure and must not depend on operator mouse activity.
            options.disableRawInput = true;
        }
        else if (argument == L"--spout2")
        {
            // Spout2 is an explicit capture output and must not silently
            // alter the user's startup integration while it is being tested.
            options.spout2 = true;
        }
        else if (argument.starts_with(L"--frames="))
        {
            const std::wstring_view value = argument.substr(9);
            wchar_t* end = nullptr;
            const unsigned long parsed = std::wcstoul(value.data(), &end, 10);
            if (end != value.data() && *end == L'\0' && parsed > 0UL)
            {
                options.frameLimit = static_cast<std::uint32_t>(parsed);
            }
        }
        else if (argument.starts_with(L"--demo-age-ms="))
        {
            const std::wstring_view value = argument.substr(14);
            wchar_t* end = nullptr;
            const unsigned long parsed = std::wcstoul(value.data(), &end, 10);
            if (end != value.data() && *end == L'\0')
            {
                options.demoClick = true;
                options.demoAgeMilliseconds = static_cast<std::uint32_t>(parsed);
            }
        }
        else if (argument.starts_with(L"--demo-delay-ms="))
        {
            const std::wstring_view value = argument.substr(16);
            wchar_t* end = nullptr;
            const unsigned long parsed = std::wcstoul(value.data(), &end, 10);
            if (end != value.data() && *end == L'\0')
            {
                options.demoClick = true;
                options.demoDelayMilliseconds =
                    static_cast<std::uint32_t>(parsed);
            }
        }
        else if (argument.starts_with(L"--quit-after-ms="))
        {
            const std::wstring_view value = argument.substr(16);
            wchar_t* end = nullptr;
            const unsigned long parsed = std::wcstoul(value.data(), &end, 10);
            if (end != value.data() && *end == L'\0' && parsed > 0UL)
            {
                options.quitAfterMilliseconds = static_cast<std::uint32_t>(parsed);
            }
        }
    }
    const std::uint32_t scenarioDurationMilliseconds =
        static_cast<std::uint32_t>(
            bafx::desktop::demoScenarioDuration(options.demoScenario).count());
    if (options.demoAgeMilliseconds.has_value()
        && *options.demoAgeMilliseconds < scenarioDurationMilliseconds)
    {
        throw std::invalid_argument(
            "--demo-age-ms must be at least "
            + std::to_string(scenarioDurationMilliseconds)
            + " for "
            + std::string(
                bafx::desktop::demoScenarioName(options.demoScenario)));
    }
    return options;
}

}
