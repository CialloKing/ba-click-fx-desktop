#include "capture_artifact_writer.hpp"

#include "bafx/core/unity_bloom.hpp"
#include "bafx/fx/simulation.hpp"
#include "bafx/windows/error.hpp"
#include "bafx/windows/fx_gpu_renderer.hpp"

#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

constexpr bafx::windows::WindowSize captureSize{1950U, 1097U};
constexpr bafx::fx::PointF captureCenter{975.0F, 548.5F};
constexpr std::uint64_t captureSeed = 20260716U;
constexpr std::uint32_t dragTrailAgeMilliseconds = 140U;
constexpr std::uint32_t dragTrailMovementPixels = 432U;
constexpr std::uint32_t dragTrailMovementSteps = 12U;
constexpr std::uint32_t trailOnlyDiagnosticPixels = 20U;
constexpr std::array defaultAges{
    50U,
    100U,
    110U,
    120U,
    130U,
    140U,
    150U,
    180U,
    250U,
    450U};

enum class CaptureCase
{
    Click,
    DragTrail
};

class ComApartment final
{
public:
    ComApartment()
    {
        bafx::windows::throwIfFailed(
            CoInitializeEx(nullptr, COINIT_MULTITHREADED),
            "CoInitializeEx(GPU capture)");
        initialized_ = true;
    }

    ~ComApartment()
    {
        if (initialized_)
        {
            CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool initialized_{false};
};

struct CaptureOptions
{
    std::filesystem::path outputDirectory{
        L"artifacts\\local\\gpu-captures\\native"};
    std::vector<std::uint32_t> agesMilliseconds{};
    std::string revision{"unrecorded"};
    CaptureCase captureCase{CaptureCase::Click};
    bool allLayers{false};
    bool help{false};
};

struct CaptureDevice
{
    ComPtr<ID3D11Device> device{};
    ComPtr<ID3D11DeviceContext> context{};
    D3D_FEATURE_LEVEL featureLevel{D3D_FEATURE_LEVEL_11_0};
};

struct RenderTarget
{
    ComPtr<ID3D11Texture2D> texture{};
    ComPtr<ID3D11RenderTargetView> view{};
};

struct ManifestLayer
{
    std::string name{};
    bafx::capture::LayerArtifact artifact{};
};

struct ManifestAge
{
    std::uint32_t ageMilliseconds{0U};
    std::vector<ManifestLayer> layers{};
    std::vector<ManifestLayer> comparisonFrames{};
};

[[nodiscard]] std::optional<std::uint32_t> parseUnsigned(
    const std::wstring_view value)
{
    if (value.empty())
    {
        return std::nullopt;
    }
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(value.data(), &end, 10);
    if (end == value.data() || *end != L'\0')
    {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(parsed);
}

[[nodiscard]] bool isRevisionCharacter(const wchar_t character) noexcept
{
    return (character >= L'0' && character <= L'9')
        || (character >= L'A' && character <= L'Z')
        || (character >= L'a' && character <= L'z')
        || character == L'.'
        || character == L'_'
        || character == L'-';
}

[[nodiscard]] CaptureOptions parseOptions(const int argumentCount, wchar_t** arguments)
{
    CaptureOptions options{};
    bool customAges = false;
    for (int index = 1; index < argumentCount; ++index)
    {
        const std::wstring_view argument(arguments[index]);
        if (argument == L"--help")
        {
            options.help = true;
        }
        else if (argument == L"--all-layers")
        {
            options.allLayers = true;
        }
        else if (argument.starts_with(L"--output="))
        {
            const std::wstring_view value = argument.substr(9U);
            if (value.empty())
            {
                throw std::invalid_argument("--output requires a directory");
            }
            options.outputDirectory = std::wstring(value);
        }
        else if (argument.starts_with(L"--age-ms="))
        {
            const std::optional<std::uint32_t> age = parseUnsigned(argument.substr(9U));
            if (!age.has_value())
            {
                throw std::invalid_argument("--age-ms requires an unsigned integer");
            }
            if (!customAges)
            {
                options.agesMilliseconds.clear();
                customAges = true;
            }
            options.agesMilliseconds.push_back(*age);
        }
        else if (argument.starts_with(L"--revision="))
        {
            const std::wstring_view value = argument.substr(11U);
            options.revision.clear();
            options.revision.reserve(value.size());
            for (const wchar_t character : value)
            {
                if (!isRevisionCharacter(character))
                {
                    throw std::invalid_argument(
                        "--revision contains a character that is unsafe for JSON");
                }
                options.revision.push_back(static_cast<char>(character));
            }
        }
        else if (argument == L"--case=drag-trail")
        {
            options.captureCase = CaptureCase::DragTrail;
        }
        else if (argument.starts_with(L"--case="))
        {
            throw std::invalid_argument("--case only supports drag-trail");
        }
        else
        {
            throw std::invalid_argument("Unknown GPU capture option");
        }
    }

    if (options.captureCase == CaptureCase::DragTrail)
    {
        if (customAges
            && (options.agesMilliseconds.size() != 1U
                || options.agesMilliseconds.front() != dragTrailAgeMilliseconds))
        {
            throw std::invalid_argument("drag-trail capture requires age 140 ms");
        }
        options.agesMilliseconds.assign(1U, dragTrailAgeMilliseconds);
    }
    else if (!customAges)
    {
        options.agesMilliseconds.assign(defaultAges.begin(), defaultAges.end());
    }
    return options;
}

void printUsage()
{
    std::wcout
        << L"Usage: ba-click-fx-gpu-capture [--output=DIR] [--age-ms=N ...] "
        << L"[--all-layers] [--revision=GIT] [--case=drag-trail]\n";
}

[[nodiscard]] CaptureDevice createWarpDevice()
{
    constexpr std::array featureLevels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    CaptureDevice graphics{};
    bafx::windows::throwIfFailed(
        D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels.data(),
            static_cast<UINT>(featureLevels.size()),
            D3D11_SDK_VERSION,
            &graphics.device,
            &graphics.featureLevel,
            &graphics.context),
        "D3D11CreateDevice(WARP capture)");
    return graphics;
}

[[nodiscard]] RenderTarget createRenderTarget(ID3D11Device* device)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = captureSize.width;
    description.Height = captureSize.height;
    description.MipLevels = 1U;
    description.ArraySize = 1U;
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.SampleDesc = DXGI_SAMPLE_DESC{1U, 0U};
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;

    RenderTarget target{};
    bafx::windows::throwIfFailed(
        device->CreateTexture2D(&description, nullptr, &target.texture),
        "ID3D11Device::CreateTexture2D(capture destination)");
    bafx::windows::throwIfFailed(
        device->CreateRenderTargetView(target.texture.Get(), nullptr, &target.view),
        "ID3D11Device::CreateRenderTargetView(capture destination)");
    return target;
}

[[nodiscard]] std::wstring ageDirectoryName(const std::uint32_t ageMilliseconds)
{
    std::wostringstream stream;
    stream << std::setfill(L'0') << std::setw(4) << ageMilliseconds << L"ms";
    return stream.str();
}

void appendLayer(
    ManifestAge& manifest,
    const std::filesystem::path& directory,
    const std::string& name,
    const bafx::windows::Rgba16FloatImage& image)
{
    manifest.layers.push_back(ManifestLayer{
        name,
        bafx::capture::writeLayerArtifact(
            directory,
            std::filesystem::path(name),
            image)});
}

[[nodiscard]] ManifestAge writeCapture(
    const std::filesystem::path& root,
    const std::uint32_t ageMilliseconds,
    const bool allLayers,
    const bafx::windows::FxGpuFrameCapture& capture)
{
    if (!capture.intermediateLayersValid)
    {
        throw std::runtime_error("GPU capture did not produce valid intermediate layers");
    }

    ManifestAge manifest{ageMilliseconds, {}, {}};
    const std::filesystem::path directory = root / ageDirectoryName(ageMilliseconds);
    if (allLayers)
    {
        appendLayer(manifest, directory, "DirectSurface", capture.directSurface);
        appendLayer(manifest, directory, "BloomSeed", capture.bloomSeed);
        for (std::size_t index = 0U; index < capture.bloomDown.size(); ++index)
        {
            std::ostringstream name;
            if (index == 0U)
            {
                name << "Prefilter_Down00";
            }
            else
            {
                name << "Down" << std::setfill('0') << std::setw(2) << index;
            }
            appendLayer(manifest, directory, name.str(), capture.bloomDown[index]);
        }
        for (std::size_t index = 0U; index < capture.bloomUp.size(); ++index)
        {
            std::ostringstream name;
            name << "Up" << std::setfill('0') << std::setw(2) << index;
            appendLayer(manifest, directory, name.str(), capture.bloomUp[index]);
        }
    }
    appendLayer(manifest, directory, "FinalOverlay", capture.finalOverlay);
    return manifest;
}

void appendComparisonFrame(
    ManifestAge& manifest,
    const std::filesystem::path& root,
    const std::string& name,
    const bafx::windows::Rgba16FloatImage& image)
{
    const std::filesystem::path directory =
        root / ageDirectoryName(manifest.ageMilliseconds);
    manifest.comparisonFrames.push_back(ManifestLayer{
        name,
        bafx::capture::writeLayerArtifact(
            directory,
            std::filesystem::path(name),
            image)});
}

[[nodiscard]] constexpr bafx::fx::SimulationTime dragTrailStepTime(
    const std::uint32_t completedSteps) noexcept
{
    constexpr std::uint32_t totalSteps = dragTrailMovementSteps + 1U;
    constexpr std::int64_t totalNanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::milliseconds(dragTrailAgeMilliseconds))
            .count();

    // Unity accumulates a float step. Round each boundary independently so
    // truncation cannot bias every simulated particle age one nanosecond low.
    return std::chrono::nanoseconds{
        (totalNanoseconds * static_cast<std::int64_t>(completedSteps)
            + static_cast<std::int64_t>(totalSteps / 2U))
        / static_cast<std::int64_t>(totalSteps)};
}

static_assert(dragTrailStepTime(1U) == 10'769'231ns);
static_assert(dragTrailStepTime(2U) == 21'538'462ns);
static_assert(dragTrailStepTime(13U) == 140ms);

[[nodiscard]] bafx::fx::FrameSnapshot makeDragTrailSnapshot()
{
    constexpr float halfMovement =
        static_cast<float>(dragTrailMovementPixels) * 0.5F;
    constexpr bafx::fx::PointF start{
        captureCenter.x - halfMovement,
        captureCenter.y};
    constexpr bafx::fx::PointF end{
        captureCenter.x + halfMovement,
        captureCenter.y};
    constexpr bafx::fx::Viewport viewport{
        captureSize.width,
        captureSize.height};

    bafx::fx::Simulation simulation(captureSeed);
    simulation.pointerDown(start, viewport, 0ns);
    simulation.advance(dragTrailStepTime(1U));
    // Anchor the input sample after Unity's stationary first step. Otherwise
    // the first moving segment incorrectly spreads births across two steps.
    simulation.pointerMove(start, viewport, dragTrailStepTime(1U));
    for (std::uint32_t step = 1U; step <= dragTrailMovementSteps; ++step)
    {
        const float progress = static_cast<float>(step)
            / static_cast<float>(dragTrailMovementSteps);
        const bafx::fx::PointF position{
            start.x + (end.x - start.x) * progress,
            start.y + (end.y - start.y) * progress};
        const bafx::fx::SimulationTime time = dragTrailStepTime(step + 1U);
        simulation.pointerMove(position, viewport, time);
        simulation.advance(time);
    }

    bafx::fx::FrameSnapshot snapshot = simulation.snapshot(
        viewport,
        std::chrono::milliseconds(dragTrailAgeMilliseconds));
    // Unity's editor diagnostic freezes TrailRenderer as two endpoints because
    // manual ParticleSystem.Simulate does not advance editor TrailRenderer time.
    snapshot.trail = {
        bafx::fx::TrailPoint{start, 1.0F},
        bafx::fx::TrailPoint{end, 0.0F}};
    snapshot.trailStrokes.clear();

    std::size_t triangleCount = 0U;
    for (const bafx::fx::Sprite& sprite : snapshot.sprites)
    {
        if (sprite.kind == bafx::fx::SpriteKind::Triangle)
        {
            ++triangleCount;
        }
    }
    if (snapshot.sprites.size() != 10U || triangleCount != 7U)
    {
        throw std::runtime_error(
            "drag-trail fixture must contain 3 non-triangle and 7 triangle sprites");
    }
    if (snapshot.trail.size() != 2U
        || snapshot.trailWidthPixels <= 0.0F
        || snapshot.trail.front().positionPixels.x != start.x
        || snapshot.trail.front().positionPixels.y != start.y
        || snapshot.trail.back().positionPixels.x != end.x
        || snapshot.trail.back().positionPixels.y != end.y)
    {
        throw std::runtime_error(
            "drag-trail fixture must contain the fixed two-endpoint trail");
    }
    return snapshot;
}

[[nodiscard]] bafx::fx::FrameSnapshot makeTrailOnlySnapshot()
{
    constexpr float halfLength =
        static_cast<float>(trailOnlyDiagnosticPixels) * 0.5F;
    bafx::fx::FrameSnapshot snapshot{};
    snapshot.trail = {
        bafx::fx::TrailPoint{
            bafx::fx::PointF{captureCenter.x - halfLength, captureCenter.y},
            1.0F},
        bafx::fx::TrailPoint{
            bafx::fx::PointF{captureCenter.x + halfLength, captureCenter.y},
            0.0F}};
    // Use the same world-to-pixel mapping as the simulation and Unity camera.
    snapshot.trailWidthPixels = 0.005F * (captureSize.height * 0.5F);
    return snapshot;
}

void writeManifest(
    const CaptureOptions& options,
    const D3D_FEATURE_LEVEL featureLevel,
    const std::vector<ManifestAge>& ages)
{
    const bafx::core::UnityBloomPlanResult bloom = bafx::core::planUnityBloom(
        bafx::core::BloomExtent{
            static_cast<std::int32_t>(captureSize.width),
            static_cast<std::int32_t>(captureSize.height)});
    if (bloom.status != bafx::core::UnityBloomStatus::Ok)
    {
        throw std::runtime_error("Bloom planner rejected the fixed capture viewport");
    }

    std::ofstream stream(options.outputDirectory / "manifest.json", std::ios::trunc);
    if (!stream)
    {
        throw std::runtime_error("Unable to create capture manifest");
    }
    stream << std::setprecision(9);
    stream << "{\n"
           << "  \"schemaVersion\": 1,\n"
           << "  \"applicationVersion\": \"" << BAFX_CAPTURE_VERSION << "\",\n"
           << "  \"revision\": \"" << options.revision << "\",\n"
           << "  \"driver\": \"WARP\",\n"
           << "  \"featureLevel\": " << static_cast<unsigned int>(featureLevel) << ",\n"
           << "  \"viewport\": {\"width\": " << captureSize.width
           << ", \"height\": " << captureSize.height << "},\n"
           << "  \"seed\": " << captureSeed << ",\n";
    if (options.captureCase == CaptureCase::DragTrail)
    {
        stream
            << "  \"case\": {\"name\": \"drag-trail\", "
            << "\"movementPixels\": " << dragTrailMovementPixels << ", "
            << "\"movementSteps\": " << dragTrailMovementSteps << ", "
            << "\"trailOnlyPixels\": " << trailOnlyDiagnosticPixels << ", "
            << "\"trailFixture\": \"two-endpoint-unity-editor-diagnostic\", "
            << "\"unityReference\": {"
            << "\"withTrail\": \"Reference/Diagnostics/Interaction/"
            << "FX_Touch_0140ms_Move_0432px_WithTrail_Age0140ms.png\", "
            << "\"noTrail\": \"Reference/Diagnostics/Interaction/"
            << "FX_Touch_0140ms_Move_0432px_NoTrail_Age0140ms.png\", "
            << "\"trailOnly\": \"Reference/Diagnostics/Trail/"
            << "FX_Touch_0140ms_TrailOnly_20px.png\"}},\n";
    }
    stream << "  \"rowOrigin\": \"top-left\",\n"
           << "  \"rawFormat\": \"DXGI_FORMAT_R16G16B16A16_FLOAT little-endian RGBA\",\n"
           << "  \"rawColorEncoding\": \"linear extended-premultiplied\",\n"
           << "  \"pngPreview\": \"linear-to-sRGB clamp over opaque black; no unpremultiply\",\n"
           << "  \"allLayers\": " << (options.allLayers ? "true" : "false") << ",\n"
           << "  \"bloom\": {\"mipCount\": "
           << static_cast<unsigned int>(bloom.plan.mipCount)
           << ", \"sampleScale\": " << bloom.plan.sampleScale
           << ", \"exposureGain\": " << bloom.plan.exposureGain << "},\n"
           << "  \"ages\": [\n";
    for (std::size_t ageIndex = 0U; ageIndex < ages.size(); ++ageIndex)
    {
        const ManifestAge& age = ages[ageIndex];
        stream << "    {\"ageMs\": " << age.ageMilliseconds << ", \"layers\": [\n";
        for (std::size_t layerIndex = 0U; layerIndex < age.layers.size(); ++layerIndex)
        {
            const ManifestLayer& layer = age.layers[layerIndex];
            stream << "      {\"name\": \"" << layer.name
                   << "\", \"width\": " << layer.artifact.width
                   << ", \"height\": " << layer.artifact.height
                   << ", \"rawBytes\": " << layer.artifact.rawBytes << "}";
            stream << (layerIndex + 1U < age.layers.size() ? ",\n" : "\n");
        }
        stream << "    ]";
        if (!age.comparisonFrames.empty())
        {
            stream << ", \"comparisonFrames\": [\n";
            for (std::size_t frameIndex = 0U;
                 frameIndex < age.comparisonFrames.size();
                 ++frameIndex)
            {
                const ManifestLayer& frame = age.comparisonFrames[frameIndex];
                stream << "      {\"name\": \"" << frame.name
                       << "\", \"width\": " << frame.artifact.width
                       << ", \"height\": " << frame.artifact.height
                       << ", \"rawBytes\": " << frame.artifact.rawBytes << "}";
                stream << (frameIndex + 1U < age.comparisonFrames.size()
                    ? ",\n"
                    : "\n");
            }
            stream << "    ]";
        }
        stream << "}" << (ageIndex + 1U < ages.size() ? ",\n" : "\n");
    }
    stream << "  ]\n}\n";
    if (!stream)
    {
        throw std::runtime_error("Unable to write capture manifest");
    }
}

int run(const CaptureOptions& options)
{
    std::error_code error;
    std::filesystem::create_directories(options.outputDirectory, error);
    if (error)
    {
        throw std::runtime_error("Unable to create capture output directory");
    }

    ComApartment apartment;
    const CaptureDevice graphics = createWarpDevice();
    const RenderTarget target = createRenderTarget(graphics.device.Get());
    bafx::windows::FxGpuRenderer renderer(
        graphics.device.Get(),
        graphics.context.Get(),
        captureSize);

    std::vector<ManifestAge> manifests;
    manifests.reserve(options.agesMilliseconds.size());
    if (options.captureCase == CaptureCase::DragTrail)
    {
        bafx::fx::FrameSnapshot withTrail = makeDragTrailSnapshot();
        const bafx::windows::FxGpuFrameCapture withTrailCapture =
            renderer.renderAndCapture(withTrail, target.view.Get());
        ManifestAge manifest = writeCapture(
            options.outputDirectory,
            dragTrailAgeMilliseconds,
            options.allLayers,
            withTrailCapture);

        bafx::fx::FrameSnapshot noTrail = std::move(withTrail);
        noTrail.trail.clear();
        noTrail.trailStrokes.clear();
        noTrail.trailWidthPixels = 0.0F;
        const bafx::windows::FxGpuFrameCapture noTrailCapture =
            renderer.renderAndCapture(noTrail, target.view.Get());
        appendComparisonFrame(
            manifest,
            options.outputDirectory,
            "FinalOverlay_NoTrail",
            noTrailCapture.finalOverlay);
        const bafx::windows::FxGpuFrameCapture trailOnlyCapture =
            renderer.renderAndCapture(makeTrailOnlySnapshot(), target.view.Get());
        appendComparisonFrame(
            manifest,
            options.outputDirectory,
            "FinalOverlay_TrailOnly20px",
            trailOnlyCapture.finalOverlay);
        manifests.push_back(std::move(manifest));
        writeManifest(options, graphics.featureLevel, manifests);
        return 0;
    }

    for (const std::uint32_t ageMilliseconds : options.agesMilliseconds)
    {
        bafx::fx::Simulation simulation(captureSeed);
        simulation.pointerDown(
            captureCenter,
            bafx::fx::Viewport{captureSize.width, captureSize.height},
            0ns);
        const bafx::fx::SimulationTime age =
            std::chrono::milliseconds(ageMilliseconds);
        simulation.advance(age);
        const bafx::fx::FrameSnapshot snapshot = simulation.snapshot(
            bafx::fx::Viewport{captureSize.width, captureSize.height},
            age);
        const bafx::windows::FxGpuFrameCapture capture = renderer.renderAndCapture(
            snapshot,
            target.view.Get());
        manifests.push_back(writeCapture(
            options.outputDirectory,
            ageMilliseconds,
            options.allLayers,
            capture));
    }
    writeManifest(options, graphics.featureLevel, manifests);
    return 0;
}

}

int wmain(const int argumentCount, wchar_t** arguments)
{
    try
    {
        const CaptureOptions options = parseOptions(argumentCount, arguments);
        if (options.help)
        {
            printUsage();
            return 0;
        }
        return run(options);
    }
    catch (const std::exception& error)
    {
        std::cerr << "ba-click-fx-gpu-capture: " << error.what() << '\n';
        return 1;
    }
}
