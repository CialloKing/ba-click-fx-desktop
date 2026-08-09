#include <bafx/reference/unity_runtime_resources.hpp>

#include <array>
#include <cstdlib>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace bafx::reference
{
namespace
{

namespace fs = std::filesystem;

constexpr std::string_view TextureDirectory =
    "Assets/Imported/FX_Touch/Textures";

struct TextureResource
{
    std::string_view filename;
    fs::path UnityRuntimeResources::*member;
};

constexpr std::array<TextureResource, 4> TextureResources{
    TextureResource{"FX_TEX_Circle_01.png", &UnityRuntimeResources::circle01},
    TextureResource{"FX_TEX_Grad_Ring3.png", &UnityRuntimeResources::gradRing3},
    TextureResource{
        "FX_TEX_Triangle_02_1.png",
        &UnityRuntimeResources::triangle02_1},
    TextureResource{"FX_TEX_Trail_03.png", &UnityRuntimeResources::trail03},
};

[[nodiscard]] std::optional<fs::path> environmentRoot()
{
#ifdef _WIN32
    const DWORD requiredSize = GetEnvironmentVariableW(
        L"BAFX_UNITY_RUNTIME_ROOT",
        nullptr,
        0);
    if (requiredSize == 0)
    {
        return std::nullopt;
    }

    std::wstring value(requiredSize, L'\0');
    const DWORD copiedSize = GetEnvironmentVariableW(
        L"BAFX_UNITY_RUNTIME_ROOT",
        value.data(),
        requiredSize);
    if (copiedSize == 0 || copiedSize >= requiredSize)
    {
        return std::nullopt;
    }

    value.resize(copiedSize);
    return fs::path{value};
#else
    const char* const value = std::getenv(UnityRuntimeRootEnvironmentVariable);
    if (value == nullptr || value[0] == '\0')
    {
        return std::nullopt;
    }

    return fs::path{value};
#endif
}

[[nodiscard]] UnityRuntimeLocationAttempt inspectCandidate(
    const UnityRuntimeRootSource source,
    const fs::path& root,
    UnityRuntimeResources& resources)
{
    UnityRuntimeLocationAttempt attempt;
    attempt.source = source;
    attempt.root = root.lexically_normal();

    resources.source = source;
    resources.root = attempt.root;

    for (const TextureResource& resource : TextureResources)
    {
        const fs::path path =
            attempt.root / fs::path{TextureDirectory} / resource.filename;
        resources.*(resource.member) = path;

        std::error_code error;
        if (!fs::is_regular_file(path, error))
        {
            // Treat inaccessible paths like missing files. The aggregated error
            // still identifies the exact candidate path that needs attention.
            attempt.unavailableFiles.push_back(path);
        }
    }

    return attempt;
}

[[nodiscard]] std::string sourceName(const UnityRuntimeRootSource source)
{
    if (source == UnityRuntimeRootSource::EnvironmentVariable)
    {
        return UnityRuntimeRootEnvironmentVariable;
    }

    return "known Unity project root";
}

[[nodiscard]] std::string pathText(const fs::path& path)
{
    // u8string keeps diagnostics stable even when the known path contains
    // Chinese characters and the process uses a different ANSI code page.
    const std::u8string value = path.generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(value.data()),
        value.size());
}

}

std::string UnityRuntimeLocationError::message() const
{
    std::ostringstream stream;
    stream << "Unable to locate the four Unity FX_Touch textures.";

    for (const UnityRuntimeLocationAttempt& attempt : attempts)
    {
        stream << " Checked " << sourceName(attempt.source) << " at '"
               << pathText(attempt.root) << "'; missing or inaccessible:";
        for (const fs::path& path : attempt.unavailableFiles)
        {
            stream << " '" << pathText(path) << "'";
        }
        stream << '.';
    }

    stream << " Set " << UnityRuntimeRootEnvironmentVariable
           << " to the Unity project root containing " << TextureDirectory
           << '.';
    return stream.str();
}

UnityRuntimeLocationResult locateUnityRuntimeResources(
    const UnityRuntimeLocatorOptions& options)
{
    UnityRuntimeLocationError locationError;

    const auto tryCandidate = [&locationError](
                                  const UnityRuntimeRootSource source,
                                  const fs::path& root)
        -> std::optional<UnityRuntimeResources>
    {
        UnityRuntimeResources resources;
        UnityRuntimeLocationAttempt attempt =
            inspectCandidate(source, root, resources);
        if (attempt.unavailableFiles.empty())
        {
            return resources;
        }

        locationError.attempts.push_back(std::move(attempt));
        return std::nullopt;
    };

    // An explicit environment path is an override candidate, not a fatal
    // requirement: a stale value may still fall back to the known project.
    if (options.environmentRoot.has_value()
        && !options.environmentRoot->empty())
    {
        if (auto resources = tryCandidate(
                UnityRuntimeRootSource::EnvironmentVariable,
                *options.environmentRoot))
        {
            return std::move(*resources);
        }
    }

    if (auto resources = tryCandidate(
            UnityRuntimeRootSource::KnownProjectRoot,
            options.knownProjectRoot))
    {
        return std::move(*resources);
    }

    return locationError;
}

UnityRuntimeLocationResult locateUnityRuntimeResources()
{
    return locateUnityRuntimeResources(UnityRuntimeLocatorOptions{
        environmentRoot(),
        knownUnityRuntimeProjectRoot(),
    });
}

fs::path knownUnityRuntimeProjectRoot()
{
#ifdef _WIN32
    // A wide literal preserves the Chinese path independently of the process
    // code page used by the desktop host.
    return fs::path{
        LR"(D:\WebProjects\BA鼠标输入与点击特效系统\UnityMouseFxLab\UnityMouseFxLab)"};
#else
    return fs::path{
        u8"D:/WebProjects/BA鼠标输入与点击特效系统/UnityMouseFxLab/UnityMouseFxLab"};
#endif
}

}
