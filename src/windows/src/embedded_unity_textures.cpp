#include "embedded_unity_textures.hpp"

#include <array>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace bafx::windows
{
namespace
{

#include "generated_unity_texture_data.inc"

[[nodiscard]] int decodeBase64Character(const char value) noexcept
{
    if (value >= 'A' && value <= 'Z')
    {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z')
    {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9')
    {
        return value - '0' + 52;
    }
    if (value == '+')
    {
        return 62;
    }
    if (value == '/')
    {
        return 63;
    }
    return -1;
}

[[nodiscard]] std::vector<std::uint8_t> decodeBase64(
    const std::string_view encoded,
    const std::size_t expectedSize)
{
    std::vector<std::uint8_t> output;
    output.reserve(expectedSize);
    std::uint32_t accumulator = 0U;
    int bits = 0;
    for (const char value : encoded)
    {
        if (value == '=')
        {
            break;
        }
        const int decoded = decodeBase64Character(value);
        if (decoded < 0)
        {
            // Generated raw strings contain newlines solely for source readability.
            if (value == '\r' || value == '\n' || value == ' ' || value == '\t')
            {
                continue;
            }
            throw std::runtime_error("Embedded Unity texture has invalid base64 data");
        }
        accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(decoded);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            output.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFFU));
        }
    }
    if (output.size() != expectedSize)
    {
        throw std::runtime_error("Embedded Unity texture byte count does not match its manifest");
    }
    return output;
}

struct TextureStorage
{
    std::vector<std::uint8_t> bytes;
    EmbeddedUnityTexture texture;
};

[[nodiscard]] TextureStorage makeTexture(
    const std::string_view name,
    const std::string_view base64,
    const std::size_t byteCount,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::string_view sha256)
{
    TextureStorage storage{};
    storage.bytes = decodeBase64(base64, byteCount);
    storage.texture = EmbeddedUnityTexture{
        name,
        storage.bytes,
        width,
        height,
        sha256};
    return storage;
}

[[nodiscard]] const std::array<TextureStorage, 5>& storages()
{
    static const std::array<TextureStorage, 5> values{
        makeTexture(
            "FX_TEX_Circle_01.png",
            circle01PngBase64,
            circle01PngByteCount,
            512U,
            512U,
            "F8675E0A16959EDA829AE7D516FB609A4D45434520866466D04B78745F0BADD2"),
        makeTexture(
            "FX_TEX_Grad_Ring3.png",
            gradRing3PngBase64,
            gradRing3PngByteCount,
            256U,
            128U,
            "517236C7C818A3715F8BA03EC316853BEC92FFD6E032B8E5D21DAEDFFC809684"),
        makeTexture(
            "FX_TEX_Triangle_02_1.png",
            triangle02_1PngBase64,
            triangle02_1PngByteCount,
            256U,
            128U,
            "0EB35FDA5710344BEDB5713B0B197B1C190EC4D8851EF8DD916B4E17DE39A068"),
        makeTexture(
            "FX_TEX_Trail_03.png",
            trail03PngBase64,
            trail03PngByteCount,
            512U,
            512U,
            "16001511757E7007F43DB9613E24144B5E8D726239DE0262F55D9E14C0F00FEB"),
        makeTexture(
            "FX_TEX_Trail_03.desktop-coverage.png",
            trail03CoveragePngBase64,
            trail03CoveragePngByteCount,
            512U,
            512U,
            "D97FDC31023CAC592398B59DC4BC85C93D24719696DD913B75C1E9702FB1A394")};
    return values;
}

}

const EmbeddedUnityTexture& embeddedUnityTexture(const EmbeddedUnityTextureId id)
{
    const auto index = static_cast<std::size_t>(id);
    const auto& values = storages();
    if (index >= values.size())
    {
        throw std::out_of_range("Unknown embedded Unity texture id");
    }
    return values[index].texture;
}

}
