#include "startup_config.hpp"

#include "package_activation.hpp"

namespace bafx::control_center
{

std::filesystem::path startupConfigPath(
    const std::filesystem::path& executableDirectory)
{
    const PackageActivationIdentityResult installState =
        readPackageActivationState(executableDirectory);
    if (installState.succeeded())
    {
        return executableDirectory / L"data" / L"BAFX.config.json";
    }
    return executableDirectory / L"BAFX.config.json";
}

bafx::config::Config loadStartupConfig(
    const std::filesystem::path& executableDirectory)
{
    const bafx::config::ConfigLoadResult loaded = bafx::config::loadConfig(
        startupConfigPath(executableDirectory));
    if (loaded.status == bafx::config::ConfigStatus::Ok)
    {
        return loaded.config;
    }

    bafx::config::Config fallback = bafx::config::defaultConfig();
    // An unavailable Host cannot provide a recovery entry. Do not hide the
    // only visible window unless a complete persisted config explicitly asks.
    fallback.system.closeToTray = false;
    return fallback;
}

}
