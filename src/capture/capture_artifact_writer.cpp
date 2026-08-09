#include "capture_artifact_writer.hpp"

#include "bafx/windows/error.hpp"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <vector>

namespace bafx::capture
{
namespace
{

using Microsoft::WRL::ComPtr;
using bafx::windows::Rgba16FloatImage;
using bafx::windows::Rgba16FloatPixel;

static_assert(sizeof(Rgba16FloatPixel) == sizeof(std::uint16_t) * 4U);
static_assert(std::is_trivially_copyable_v<Rgba16FloatPixel>);

[[nodiscard]] std::size_t checkedPixelCount(const Rgba16FloatImage& image)
{
    if (image.width == 0U || image.height == 0U)
    {
        throw std::invalid_argument("Capture layer dimensions must be nonzero");
    }
    const std::size_t width = image.width;
    const std::size_t height = image.height;
    if (height > std::numeric_limits<std::size_t>::max() / width)
    {
        throw std::overflow_error("Capture layer pixel count exceeds address space");
    }
    const std::size_t count = width * height;
    if (image.pixels.size() != count)
    {
        throw std::invalid_argument("Capture layer storage does not match its dimensions");
    }
    return count;
}

[[nodiscard]] std::uint8_t linearToSrgbByte(const float linear)
{
    if (!std::isfinite(linear))
    {
        throw std::runtime_error("Capture layer contains a non-finite RGB value");
    }

    const float clamped = std::clamp(linear, 0.0F, 1.0F);
    const float srgb = clamped <= 0.0031308F
        ? clamped * 12.92F
        : 1.055F * std::pow(clamped, 1.0F / 2.4F) - 0.055F;
    return static_cast<std::uint8_t>(std::lround(srgb * 255.0F));
}

[[nodiscard]] std::vector<std::uint8_t> makeOpaqueBlackSrgbPreview(
    const Rgba16FloatImage& image)
{
    const std::size_t pixelCount = checkedPixelCount(image);
    if (pixelCount > std::numeric_limits<std::size_t>::max() / 4U)
    {
        throw std::overflow_error("Capture PNG buffer exceeds address space");
    }

    std::vector<std::uint8_t> bgra(pixelCount * 4U);
    for (std::size_t index = 0U; index < pixelCount; ++index)
    {
        const Rgba16FloatPixel pixel = image.pixels[index];
        const std::size_t destination = index * 4U;
        bgra[destination] = linearToSrgbByte(
            bafx::windows::halfToFloat(pixel.blue));
        bgra[destination + 1U] = linearToSrgbByte(
            bafx::windows::halfToFloat(pixel.green));
        bgra[destination + 2U] = linearToSrgbByte(
            bafx::windows::halfToFloat(pixel.red));
        // Unity Golden is captured over an opaque black camera attachment.
        // Keeping RGB premultiplied and forcing opaque alpha gives the same comparison basis.
        bgra[destination + 3U] = 255U;
    }
    return bgra;
}

void writeRawHalf(
    const std::filesystem::path& path,
    const Rgba16FloatImage& image)
{
    (void)checkedPixelCount(image);
    const std::size_t byteCountValue =
        image.pixels.size() * sizeof(Rgba16FloatPixel);
    if (byteCountValue > static_cast<std::size_t>(
            std::numeric_limits<std::streamsize>::max()))
    {
        throw std::overflow_error("Capture raw layer exceeds stream limits");
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        throw std::runtime_error("Unable to create capture raw file");
    }
    const auto byteCount = static_cast<std::streamsize>(byteCountValue);
    stream.write(
        reinterpret_cast<const char*>(image.pixels.data()),
        byteCount);
    if (!stream)
    {
        throw std::runtime_error("Unable to write capture raw file");
    }
}

void writePng(
    const std::filesystem::path& path,
    const Rgba16FloatImage& image)
{
    const std::vector<std::uint8_t> pixels = makeOpaqueBlackSrgbPreview(image);
    if (image.width > std::numeric_limits<UINT>::max() / 4U
        || pixels.size() > std::numeric_limits<UINT>::max())
    {
        throw std::overflow_error("Capture PNG exceeds WIC buffer limits");
    }

    ComPtr<IWICImagingFactory> factory;
    bafx::windows::throwIfFailed(
        CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory)),
        "CoCreateInstance(WIC capture factory)");
    ComPtr<IWICStream> stream;
    bafx::windows::throwIfFailed(
        factory->CreateStream(&stream),
        "IWICImagingFactory::CreateStream(capture)");
    bafx::windows::throwIfFailed(
        stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE),
        "IWICStream::InitializeFromFilename(capture)");

    ComPtr<IWICBitmapEncoder> encoder;
    bafx::windows::throwIfFailed(
        factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder),
        "IWICImagingFactory::CreateEncoder(capture PNG)");
    bafx::windows::throwIfFailed(
        encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache),
        "IWICBitmapEncoder::Initialize(capture PNG)");

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    bafx::windows::throwIfFailed(
        encoder->CreateNewFrame(&frame, &properties),
        "IWICBitmapEncoder::CreateNewFrame(capture PNG)");
    bafx::windows::throwIfFailed(
        frame->Initialize(properties.Get()),
        "IWICBitmapFrameEncode::Initialize(capture PNG)");
    bafx::windows::throwIfFailed(
        frame->SetSize(image.width, image.height),
        "IWICBitmapFrameEncode::SetSize(capture PNG)");
    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
    bafx::windows::throwIfFailed(
        frame->SetPixelFormat(&pixelFormat),
        "IWICBitmapFrameEncode::SetPixelFormat(capture PNG)");
    if (!IsEqualGUID(pixelFormat, GUID_WICPixelFormat32bppBGRA))
    {
        throw std::runtime_error("WIC capture encoder changed the requested pixel format");
    }

    const UINT stride = image.width * 4U;
    bafx::windows::throwIfFailed(
        frame->WritePixels(
            image.height,
            stride,
            static_cast<UINT>(pixels.size()),
            const_cast<BYTE*>(pixels.data())),
        "IWICBitmapFrameEncode::WritePixels(capture PNG)");
    bafx::windows::throwIfFailed(
        frame->Commit(),
        "IWICBitmapFrameEncode::Commit(capture PNG)");
    bafx::windows::throwIfFailed(
        encoder->Commit(),
        "IWICBitmapEncoder::Commit(capture PNG)");
}

}

LayerArtifact writeLayerArtifact(
    const std::filesystem::path& directory,
    const std::filesystem::path& stem,
    const Rgba16FloatImage& image)
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
    {
        throw std::runtime_error("Unable to create capture artifact directory");
    }

    writeRawHalf(directory / (stem.native() + L".rgba16f"), image);
    writePng(directory / (stem.native() + L".png"), image);
    return LayerArtifact{
        image.width,
        image.height,
        static_cast<std::uint64_t>(image.pixels.size())
            * sizeof(Rgba16FloatPixel)};
}

}
