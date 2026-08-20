#include <SpoutDX/SpoutDX.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{

constexpr std::string_view defaultSenderName = "ba-click-fx-desktop";

struct Options final
{
    std::string senderName{defaultSenderName};
    std::filesystem::path outputPath{};
    std::filesystem::path captureOutputPath{};
    std::uint32_t durationMilliseconds{6500U};
    std::uint32_t intervalMilliseconds{100U};
};

struct Pixel final
{
    std::uint8_t blue{0U};
    std::uint8_t green{0U};
    std::uint8_t red{0U};
    std::uint8_t alpha{0U};
};

static_assert(sizeof(Pixel) == 4U);

struct PixelSummary final
{
    std::uint8_t maximumRgb{0U};
    std::uint8_t maximumAlpha{0U};
    std::uint64_t nonzeroRgbPixels{0U};
    std::uint64_t nonzeroAlphaPixels{0U};
    std::uint64_t extendedPremultipliedPixels{0U};
    std::uint64_t zeroAlphaEmissionPixels{0U};
};

struct Sample final
{
    std::uint64_t elapsedMilliseconds{0U};
    bool connected{false};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    std::uintptr_t sharedHandle{0U};
    PixelSummary pixels{};
};

struct CapturedFrame final
{
    std::uint64_t elapsedMilliseconds{0U};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
};

struct Collection final
{
    std::vector<Sample> samples{};
    std::optional<CapturedFrame> capturedFrame{};
};

[[nodiscard]] std::uint32_t parsePositiveInteger(
    const std::string_view value,
    const std::string_view optionName)
{
    std::uint32_t parsed = 0U;
    const std::from_chars_result result = std::from_chars(
        value.data(),
        value.data() + value.size(),
        parsed);
    if (result.ec != std::errc{}
        || result.ptr != value.data() + value.size()
        || parsed == 0U)
    {
        throw std::invalid_argument(
            std::string(optionName) + " requires a positive integer");
    }
    return parsed;
}

[[nodiscard]] Options parseOptions(const int argc, char** argv)
{
    Options options{};
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        if (argument.starts_with("--sender="))
        {
            options.senderName = argument.substr(9U);
        }
        else if (argument.starts_with("--output="))
        {
            options.outputPath = std::string(argument.substr(9U));
        }
        else if (argument.starts_with("--duration-ms="))
        {
            options.durationMilliseconds = parsePositiveInteger(
                argument.substr(14U),
                "--duration-ms");
        }
        else if (argument.starts_with("--capture-output="))
        {
            options.captureOutputPath = std::string(argument.substr(17U));
        }
        else if (argument.starts_with("--interval-ms="))
        {
            options.intervalMilliseconds = parsePositiveInteger(
                argument.substr(14U),
                "--interval-ms");
        }
        else
        {
            throw std::invalid_argument(
                "unknown receiver probe argument: " + std::string(argument));
        }
    }

    if (options.senderName.empty())
    {
        throw std::invalid_argument("--sender must not be empty");
    }
    if (options.outputPath.empty())
    {
        throw std::invalid_argument("--output is required");
    }
    if (options.intervalMilliseconds > options.durationMilliseconds)
    {
        throw std::invalid_argument(
            "--interval-ms must not exceed --duration-ms");
    }
    return options;
}

[[nodiscard]] std::string jsonEscape(const std::string_view value)
{
    std::ostringstream output;
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '\"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20U)
            {
                output << "\\u"
                       << std::hex
                       << std::setw(4)
                       << std::setfill('0')
                       << static_cast<unsigned int>(character)
                       << std::dec;
            }
            else
            {
                output << static_cast<char>(character);
            }
            break;
        }
    }
    return output.str();
}

[[nodiscard]] std::string handleString(const std::uintptr_t value)
{
    std::ostringstream output;
    output << "0x" << std::hex << value;
    return output.str();
}

class TextureScanner final
{
public:
    explicit TextureScanner(ID3D11DeviceContext* context)
        : context_(context)
    {
        if (context_ == nullptr)
        {
            throw std::invalid_argument("Spout2 receiver has no D3D11 context");
        }
    }

    [[nodiscard]] PixelSummary scan(ID3D11Texture2D* source)
    {
        if (source == nullptr)
        {
            throw std::invalid_argument("Spout2 receiver has no texture");
        }

        D3D11_TEXTURE2D_DESC description{};
        source->GetDesc(&description);
        if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM
            || description.SampleDesc.Count != 1U
            || description.Width == 0U
            || description.Height == 0U)
        {
            throw std::runtime_error(
                "Spout2 receiver texture is not single-sample BGRA8 UNORM");
        }
        ensureStaging(source, description);

        context_->CopyResource(staging_.Get(), source);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT mappedResult = context_->Map(
            staging_.Get(),
            0U,
            D3D11_MAP_READ,
            0U,
            &mapped);
        if (FAILED(mappedResult))
        {
            throw std::runtime_error("Spout2 receiver could not map its staging texture");
        }

        PixelSummary summary{};
        try
        {
            const std::size_t tightRowBytes =
                static_cast<std::size_t>(description.Width) * sizeof(Pixel);
            if (mapped.pData == nullptr || mapped.RowPitch < tightRowBytes)
            {
                throw std::runtime_error(
                    "Spout2 receiver mapped row is smaller than the texture");
            }
            for (std::uint32_t y = 0U; y < description.Height; ++y)
            {
                const auto* row = reinterpret_cast<const Pixel*>(
                    static_cast<const std::uint8_t*>(mapped.pData)
                    + static_cast<std::size_t>(y) * mapped.RowPitch);
                for (std::uint32_t x = 0U; x < description.Width; ++x)
                {
                    const Pixel pixel = row[x];
                    summary.maximumRgb = std::max(
                        summary.maximumRgb,
                        std::max(pixel.red, std::max(pixel.green, pixel.blue)));
                    summary.maximumAlpha = std::max(
                        summary.maximumAlpha,
                        pixel.alpha);
                    if (pixel.alpha != 0U)
                    {
                        ++summary.nonzeroAlphaPixels;
                    }
                    if (pixel.red != 0U
                        || pixel.green != 0U
                        || pixel.blue != 0U)
                    {
                        ++summary.nonzeroRgbPixels;
                        if (pixel.alpha == 0U)
                        {
                            ++summary.zeroAlphaEmissionPixels;
                        }
                    }
                    if (pixel.red > pixel.alpha
                        || pixel.green > pixel.alpha
                        || pixel.blue > pixel.alpha)
                    {
                        ++summary.extendedPremultipliedPixels;
                    }
                }
            }
        }
        catch (...)
        {
            context_->Unmap(staging_.Get(), 0U);
            throw;
        }
        context_->Unmap(staging_.Get(), 0U);
        return summary;
    }

    void writeCurrentFrame(const std::filesystem::path& outputPath)
    {
        if (staging_ == nullptr || width_ == 0U || height_ == 0U)
        {
            throw std::runtime_error("Spout2 receiver has no frame to capture");
        }
        const std::filesystem::path parent = outputPath.parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent);
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT mappedResult = context_->Map(
            staging_.Get(),
            0U,
            D3D11_MAP_READ,
            0U,
            &mapped);
        if (FAILED(mappedResult))
        {
            throw std::runtime_error(
                "Spout2 receiver could not map its captured frame");
        }
        try
        {
            const std::size_t tightRowBytes =
                static_cast<std::size_t>(width_) * sizeof(Pixel);
            if (mapped.pData == nullptr || mapped.RowPitch < tightRowBytes)
            {
                throw std::runtime_error(
                    "Spout2 captured frame row is smaller than expected");
            }
            std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                throw std::runtime_error(
                    "Spout2 receiver could not create its captured frame");
            }
            for (std::uint32_t y = 0U; y < height_; ++y)
            {
                const auto* row = static_cast<const char*>(mapped.pData)
                    + static_cast<std::size_t>(y) * mapped.RowPitch;
                output.write(row, static_cast<std::streamsize>(tightRowBytes));
            }
            if (!output)
            {
                throw std::runtime_error(
                    "Spout2 receiver could not finish its captured frame");
            }
        }
        catch (...)
        {
            context_->Unmap(staging_.Get(), 0U);
            throw;
        }
        context_->Unmap(staging_.Get(), 0U);
    }

private:
    void ensureStaging(
        ID3D11Texture2D* source,
        const D3D11_TEXTURE2D_DESC& sourceDescription)
    {
        if (staging_ != nullptr
            && width_ == sourceDescription.Width
            && height_ == sourceDescription.Height
            && format_ == sourceDescription.Format)
        {
            return;
        }

        ComPtr<ID3D11Device> sourceDevice;
        ComPtr<ID3D11Device> contextDevice;
        source->GetDevice(&sourceDevice);
        context_->GetDevice(&contextDevice);
        if (sourceDevice.Get() != contextDevice.Get())
        {
            throw std::runtime_error(
                "Spout2 receiver texture and context use different devices");
        }

        D3D11_TEXTURE2D_DESC stagingDescription{};
        stagingDescription.Width = sourceDescription.Width;
        stagingDescription.Height = sourceDescription.Height;
        stagingDescription.MipLevels = 1U;
        stagingDescription.ArraySize = 1U;
        stagingDescription.Format = sourceDescription.Format;
        stagingDescription.SampleDesc = DXGI_SAMPLE_DESC{1U, 0U};
        stagingDescription.Usage = D3D11_USAGE_STAGING;
        stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        staging_.Reset();
        const HRESULT createResult = sourceDevice->CreateTexture2D(
            &stagingDescription,
            nullptr,
            &staging_);
        if (FAILED(createResult))
        {
            throw std::runtime_error(
                "Spout2 receiver could not create a staging texture");
        }
        width_ = sourceDescription.Width;
        height_ = sourceDescription.Height;
        format_ = sourceDescription.Format;
    }

    ComPtr<ID3D11DeviceContext> context_{};
    ComPtr<ID3D11Texture2D> staging_{};
    std::uint32_t width_{0U};
    std::uint32_t height_{0U};
    DXGI_FORMAT format_{DXGI_FORMAT_UNKNOWN};
};

void writeResult(
    const Options& options,
    const Collection& collection)
{
    const std::filesystem::path parent = options.outputPath.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent);
    }
    std::ofstream output(options.outputPath, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        throw std::runtime_error("could not create receiver probe output");
    }

    output << "{\n"
           << "  \"schemaVersion\": 2,\n"
           << "  \"sender\": \"" << jsonEscape(options.senderName) << "\",\n"
           << "  \"durationMs\": " << options.durationMilliseconds << ",\n"
           << "  \"intervalMs\": " << options.intervalMilliseconds << ",\n"
           << "  \"samples\": [\n";
    for (std::size_t index = 0U; index < collection.samples.size(); ++index)
    {
        const Sample& sample = collection.samples[index];
        output << "    {\"elapsedMs\":" << sample.elapsedMilliseconds
               << ",\"connected\":" << (sample.connected ? "true" : "false");
        if (sample.connected)
        {
            output << ",\"width\":" << sample.width
                   << ",\"height\":" << sample.height
                   << ",\"format\":" << static_cast<unsigned int>(sample.format)
                   << ",\"handle\":\"" << handleString(sample.sharedHandle) << "\""
                   << ",\"maxRgb\":" << static_cast<unsigned int>(
                        sample.pixels.maximumRgb)
                   << ",\"maxAlpha\":" << static_cast<unsigned int>(
                        sample.pixels.maximumAlpha)
                   << ",\"nonzeroRgbPixels\":"
                   << sample.pixels.nonzeroRgbPixels
                   << ",\"nonzeroAlphaPixels\":"
                   << sample.pixels.nonzeroAlphaPixels
                   << ",\"extendedPremultipliedPixels\":"
                   << sample.pixels.extendedPremultipliedPixels
                   << ",\"zeroAlphaEmissionPixels\":"
                   << sample.pixels.zeroAlphaEmissionPixels;
        }
        output << "}"
               << (index + 1U == collection.samples.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"capturedFrame\": ";
    if (!collection.capturedFrame.has_value())
    {
        output << "null\n";
    }
    else
    {
        const CapturedFrame& frame = *collection.capturedFrame;
        output << "{\"path\":\""
               << jsonEscape(options.captureOutputPath.string())
               << "\",\"elapsedMs\":" << frame.elapsedMilliseconds
               << ",\"width\":" << frame.width
               << ",\"height\":" << frame.height
               << ",\"format\":" << static_cast<unsigned int>(frame.format)
               << "}\n";
    }
    output << "}\n";
    if (!output)
    {
        throw std::runtime_error("could not finish receiver probe output");
    }
}

[[nodiscard]] Collection collect(const Options& options)
{
    spoutDX receiver;
    receiver.SetAdapterAuto(true);
    receiver.SetKeyed(false);
    if (!receiver.OpenDirectX11())
    {
        throw std::runtime_error("Spout2 receiver could not open D3D11");
    }
    receiver.SetReceiverName(options.senderName.c_str());
    TextureScanner scanner(receiver.GetDX11Context());

    const auto startedAt = std::chrono::steady_clock::now();
    const auto deadline = startedAt
        + std::chrono::milliseconds(options.durationMilliseconds);
    auto nextSampleAt = startedAt;
    Collection collection{};
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_until(nextSampleAt);
        const auto sampledAt = std::chrono::steady_clock::now();
        Sample sample{};
        sample.elapsedMilliseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                sampledAt - startedAt).count());
        sample.connected = receiver.ReceiveTexture();
        if (sample.connected)
        {
            ID3D11Texture2D* const texture = receiver.GetSenderTexture();
            sample.width = receiver.GetSenderWidth();
            sample.height = receiver.GetSenderHeight();
            sample.format = receiver.GetSenderFormat();
            sample.sharedHandle = reinterpret_cast<std::uintptr_t>(
                receiver.GetSenderHandle());
            sample.pixels = scanner.scan(texture);
            if (!options.captureOutputPath.empty()
                && !collection.capturedFrame.has_value()
                && sample.pixels.extendedPremultipliedPixels > 0U
                && sample.pixels.zeroAlphaEmissionPixels > 0U)
            {
                scanner.writeCurrentFrame(options.captureOutputPath);
                collection.capturedFrame = CapturedFrame{
                    sample.elapsedMilliseconds,
                    sample.width,
                    sample.height,
                    sample.format};
            }
        }
        collection.samples.push_back(sample);
        nextSampleAt += std::chrono::milliseconds(options.intervalMilliseconds);
    }

    receiver.ReleaseReceiver();
    receiver.CloseDirectX11();
    return collection;
}

}

int main(const int argc, char** argv)
{
    try
    {
        const Options options = parseOptions(argc, argv);
        const Collection collection = collect(options);
        writeResult(options, collection);
        std::cout << "Spout2 receiver samples: "
                  << collection.samples.size() << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Spout2 receiver probe failed: " << error.what() << '\n';
        return 2;
    }
}
