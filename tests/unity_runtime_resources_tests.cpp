#include "test_support.hpp"

#include <bafx/reference/unity_runtime_resources.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace
{

namespace fs = std::filesystem;
using bafx::reference::UnityRuntimeLocationError;
using bafx::reference::UnityRuntimeLocationResult;
using bafx::reference::UnityRuntimeLocatorOptions;
using bafx::reference::UnityRuntimeResources;
using bafx::reference::UnityRuntimeRootSource;

constexpr std::string_view TextureDirectory =
    "Assets/Imported/FX_Touch/Textures";

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(
              fs::temp_directory_path()
              / ("bafx-unity-runtime-"
                 + std::to_string(
                     std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count())))
    {
        std::error_code error;
        if (!fs::create_directory(path_, error))
        {
            throw std::runtime_error(
                "Unable to create a temporary test directory");
        }
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    [[nodiscard]] const fs::path& path() const
    {
        return path_;
    }

private:
    fs::path path_;
};

void createTextureFiles(
    const fs::path& root,
    const bool includeTrail = true)
{
    const fs::path directory = root / TextureDirectory;
    fs::create_directories(directory);

    const auto createMarker = [&directory](const std::string_view filename)
    {
        // The locator validates paths, not PNG decoding. Empty markers keep the
        // tests independent from copyrighted external resources.
        std::ofstream stream(directory / filename, std::ios::binary);
        if (!stream)
        {
            throw std::runtime_error("Unable to create a test texture marker");
        }
    };

    createMarker("FX_TEX_Circle_01.png");
    createMarker("FX_TEX_Grad_Ring3.png");
    createMarker("FX_TEX_Triangle_02_1.png");
    if (includeTrail)
    {
        createMarker("FX_TEX_Trail_03.png");
    }
}

[[nodiscard]] const UnityRuntimeResources& requireResources(
    const UnityRuntimeLocationResult& result)
{
    BAFX_CHECK(std::holds_alternative<UnityRuntimeResources>(result));
    return std::get<UnityRuntimeResources>(result);
}

}

BAFX_TEST(environment_candidate_has_priority)
{
    TemporaryDirectory temporary;
    const fs::path environmentRoot = temporary.path() / "environment";
    const fs::path fallbackRoot = temporary.path() / "fallback";
    createTextureFiles(environmentRoot);
    createTextureFiles(fallbackRoot);

    const UnityRuntimeLocationResult result =
        bafx::reference::locateUnityRuntimeResources(
            UnityRuntimeLocatorOptions{environmentRoot, fallbackRoot});
    const UnityRuntimeResources& resources = requireResources(result);

    BAFX_CHECK(resources.source == UnityRuntimeRootSource::EnvironmentVariable);
    BAFX_CHECK(resources.root == environmentRoot);
    BAFX_CHECK(resources.circle01.filename() == "FX_TEX_Circle_01.png");
    BAFX_CHECK(resources.gradRing3.filename() == "FX_TEX_Grad_Ring3.png");
    BAFX_CHECK(resources.triangle02_1.filename() == "FX_TEX_Triangle_02_1.png");
    BAFX_CHECK(resources.trail03.filename() == "FX_TEX_Trail_03.png");
}

BAFX_TEST(invalid_environment_candidate_falls_back)
{
    TemporaryDirectory temporary;
    const fs::path environmentRoot = temporary.path() / "environment";
    const fs::path fallbackRoot = temporary.path() / "fallback";
    createTextureFiles(environmentRoot, false);
    createTextureFiles(fallbackRoot);

    const UnityRuntimeLocationResult result =
        bafx::reference::locateUnityRuntimeResources(
            UnityRuntimeLocatorOptions{environmentRoot, fallbackRoot});
    const UnityRuntimeResources& resources = requireResources(result);

    BAFX_CHECK(resources.source == UnityRuntimeRootSource::KnownProjectRoot);
    BAFX_CHECK(resources.root == fallbackRoot);
}

BAFX_TEST(error_lists_every_candidate_and_missing_file)
{
    TemporaryDirectory temporary;
    const fs::path environmentRoot = temporary.path() / "environment";
    const fs::path fallbackRoot = temporary.path() / "fallback";
    createTextureFiles(environmentRoot, false);

    const UnityRuntimeLocationResult result =
        bafx::reference::locateUnityRuntimeResources(
            UnityRuntimeLocatorOptions{environmentRoot, fallbackRoot});

    BAFX_CHECK(std::holds_alternative<UnityRuntimeLocationError>(result));
    const UnityRuntimeLocationError& error =
        std::get<UnityRuntimeLocationError>(result);
    BAFX_CHECK(error.attempts.size() == 2);
    BAFX_CHECK(error.attempts[0].unavailableFiles.size() == 1);
    BAFX_CHECK(
        error.attempts[0].unavailableFiles[0].filename()
        == "FX_TEX_Trail_03.png");
    BAFX_CHECK(error.attempts[1].unavailableFiles.size() == 4);

    const std::string message = error.message();
    BAFX_CHECK(message.find("BAFX_UNITY_RUNTIME_ROOT") != std::string::npos);
    BAFX_CHECK(message.find("FX_TEX_Trail_03.png") != std::string::npos);
    BAFX_CHECK(message.find(fallbackRoot.generic_string()) != std::string::npos);
}
