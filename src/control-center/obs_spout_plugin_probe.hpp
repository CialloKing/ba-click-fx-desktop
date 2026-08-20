#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace bafx::control_center
{

enum class ObsSpoutPluginState : std::uint8_t
{
    Missing,
    InstalledObsNotRunning,
    Loaded,
    InstalledNotLoaded,
    InspectionUnavailable
};

struct ObsProcessModuleEvidence final
{
    bool inspectionSucceeded{false};
    bool pluginLoaded{false};
};

struct ObsSpoutPluginProbeResult final
{
    ObsSpoutPluginState state{ObsSpoutPluginState::Missing};
    bool obsRunning{false};
    std::filesystem::path obsExecutable{};
    std::filesystem::path pluginPath{};
    std::string pluginVersion{};
    std::string pluginArchitecture{};
};

[[nodiscard]] ObsSpoutPluginState classifyObsSpoutPlugin(
    bool installed,
    std::span<const ObsProcessModuleEvidence> processEvidence) noexcept;

[[nodiscard]] ObsSpoutPluginProbeResult probeObsSpoutPlugin() noexcept;

}
