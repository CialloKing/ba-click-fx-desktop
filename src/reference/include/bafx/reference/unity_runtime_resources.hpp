#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace bafx::reference
{

inline constexpr char UnityRuntimeRootEnvironmentVariable[] =
    "BAFX_UNITY_RUNTIME_ROOT";

enum class UnityRuntimeRootSource
{
    EnvironmentVariable,
    KnownProjectRoot
};

struct UnityRuntimeResources
{
    UnityRuntimeRootSource source{UnityRuntimeRootSource::KnownProjectRoot};
    std::filesystem::path root;
    std::filesystem::path circle01;
    std::filesystem::path gradRing3;
    std::filesystem::path triangle02_1;
    std::filesystem::path trail03;
};

struct UnityRuntimeLocationAttempt
{
    UnityRuntimeRootSource source{UnityRuntimeRootSource::KnownProjectRoot};
    std::filesystem::path root;
    std::vector<std::filesystem::path> unavailableFiles;
};

struct UnityRuntimeLocationError
{
    std::vector<UnityRuntimeLocationAttempt> attempts;

    [[nodiscard]] std::string message() const;
};

using UnityRuntimeLocationResult =
    std::variant<UnityRuntimeResources, UnityRuntimeLocationError>;

struct UnityRuntimeLocatorOptions
{
    std::optional<std::filesystem::path> environmentRoot;
    std::filesystem::path knownProjectRoot;
};

// This overload accepts explicit candidates so tests and tools do not depend on
// resources installed on the current machine.
[[nodiscard]] UnityRuntimeLocationResult locateUnityRuntimeResources(
    const UnityRuntimeLocatorOptions& options);

// Production discovery reads BAFX_UNITY_RUNTIME_ROOT first, then checks the
// known local Unity reconstruction project without copying any resource.
[[nodiscard]] UnityRuntimeLocationResult locateUnityRuntimeResources();

[[nodiscard]] std::filesystem::path knownUnityRuntimeProjectRoot();

}
