#include "test_support.hpp"

#include "packed_fx_textures.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{

struct ExpectedTexture
{
    bafx::windows::PackedFxTextureId id;
    std::string_view name;
    bafx::windows::PackedFxTextureLayout layout;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t rowPitch;
    std::string_view sha256;
};

class AlgorithmProvider final
{
public:
    AlgorithmProvider()
    {
        const NTSTATUS status = BCryptOpenAlgorithmProvider(
            &handle_,
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            0);
        if (status < 0)
        {
            throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
        }
    }

    ~AlgorithmProvider()
    {
        if (handle_ != nullptr)
        {
            BCryptCloseAlgorithmProvider(handle_, 0);
        }
    }

    AlgorithmProvider(const AlgorithmProvider&) = delete;
    AlgorithmProvider& operator=(const AlgorithmProvider&) = delete;

    [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept
    {
        return handle_;
    }

private:
    BCRYPT_ALG_HANDLE handle_{nullptr};
};

[[nodiscard]] std::string sha256(const std::vector<std::uint8_t>& bytes)
{
    AlgorithmProvider provider;
    std::array<std::uint8_t, 32> digest{};
    const NTSTATUS status = BCryptHash(
        provider.get(),
        nullptr,
        0,
        const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(bytes.data())),
        static_cast<ULONG>(bytes.size()),
        digest.data(),
        static_cast<ULONG>(digest.size()));
    if (status < 0)
    {
        throw std::runtime_error("BCryptHash failed");
    }

    std::ostringstream stream;
    stream << std::hex << std::uppercase << std::setfill('0');
    for (const std::uint8_t value : digest)
    {
        stream << std::setw(2) << static_cast<unsigned int>(value);
    }
    return stream.str();
}

}

BAFX_TEST(packed_fx_textures_decode_to_locked_pixels)
{
    using namespace bafx::windows;
    constexpr std::array expectedTextures{
        ExpectedTexture{
            PackedFxTextureId::CenterDisk,
            "center-disk",
            PackedFxTextureLayout::Rgba8Srgb,
            512U,
            512U,
            2048U,
            "9721F56BD0EF000C6A8D9ACC718B0D20FE845F4EF849275C097544188E23C3FE"},
        ExpectedTexture{
            PackedFxTextureId::DissolveRing,
            "dissolve-ring",
            PackedFxTextureLayout::R8Unorm,
            256U,
            128U,
            256U,
            "43C8D7B1AE2C4B629ABA382BCCD606593AC5D608B3EE437743FD3948BD23FC14"},
        ExpectedTexture{
            PackedFxTextureId::TriangleAtlas,
            "triangle-atlas",
            PackedFxTextureLayout::Rgba8Srgb,
            256U,
            128U,
            1024U,
            "B2C18E0C1377530C7870EFE02CE48C75C2142C7262C9FF668E71B85AE471269E"},
        ExpectedTexture{
            PackedFxTextureId::Trail,
            "trail",
            PackedFxTextureLayout::Rgba8Srgb,
            512U,
            512U,
            2048U,
            "DE8B8B1039080784BB164655424F8532EAC86A85AA9D90FB677EA02C2DB87E99"}};

    for (const ExpectedTexture& expected : expectedTextures)
    {
        const DecodedPackedFxTexture texture = decodePackedFxTexture(expected.id);
        BAFX_CHECK(texture.name == expected.name);
        BAFX_CHECK(texture.layout == expected.layout);
        BAFX_CHECK(texture.width == expected.width);
        BAFX_CHECK(texture.height == expected.height);
        BAFX_CHECK(texture.rowPitch == expected.rowPitch);
        BAFX_CHECK(texture.pixels.size()
            == static_cast<std::size_t>(expected.rowPitch) * expected.height);
        BAFX_CHECK(texture.decodedSha256 == expected.sha256);
        BAFX_CHECK(sha256(texture.pixels) == expected.sha256);
    }
}

BAFX_TEST(packed_fx_textures_are_raw_pixels_not_png_containers)
{
    constexpr std::array<std::uint8_t, 8> pngSignature{
        0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU};

    for (const auto id : {
             bafx::windows::PackedFxTextureId::CenterDisk,
             bafx::windows::PackedFxTextureId::DissolveRing,
             bafx::windows::PackedFxTextureId::TriangleAtlas,
             bafx::windows::PackedFxTextureId::Trail})
    {
        const auto texture = bafx::windows::decodePackedFxTexture(id);
        BAFX_CHECK(texture.pixels.size() >= pngSignature.size());
        BAFX_CHECK(!std::equal(
            pngSignature.begin(),
            pngSignature.end(),
            texture.pixels.begin()));
    }
}
