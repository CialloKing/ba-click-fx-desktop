#include "test_support.hpp"

#include "embedded_unity_textures.hpp"

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
    bafx::windows::EmbeddedUnityTextureId id;
    std::string_view name;
    std::size_t byteCount;
    std::uint32_t width;
    std::uint32_t height;
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

[[nodiscard]] std::string sha256(const std::span<const std::uint8_t> bytes)
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

[[nodiscard]] std::uint32_t readBigEndian32(
    const std::span<const std::uint8_t> bytes,
    const std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U)
        | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U)
        | static_cast<std::uint32_t>(bytes[offset + 3U]);
}

}

BAFX_TEST(embedded_unity_pngs_match_locked_originals)
{
    constexpr std::array expectedTextures{
        ExpectedTexture{
            bafx::windows::EmbeddedUnityTextureId::Circle01,
            "FX_TEX_Circle_01.png",
            23540U,
            512U,
            512U},
        ExpectedTexture{
            bafx::windows::EmbeddedUnityTextureId::GradRing3,
            "FX_TEX_Grad_Ring3.png",
            19743U,
            256U,
            128U},
        ExpectedTexture{
            bafx::windows::EmbeddedUnityTextureId::Triangle02_1,
            "FX_TEX_Triangle_02_1.png",
            4582U,
            256U,
            128U},
        ExpectedTexture{
            bafx::windows::EmbeddedUnityTextureId::Trail03,
            "FX_TEX_Trail_03.png",
            54161U,
            512U,
            512U}};
    constexpr std::array<std::uint8_t, 8> pngSignature{
        0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU};

    for (const ExpectedTexture& expected : expectedTextures)
    {
        const bafx::windows::EmbeddedUnityTexture& texture =
            bafx::windows::embeddedUnityTexture(expected.id);
        BAFX_CHECK(texture.name == expected.name);
        BAFX_CHECK(texture.pngBytes.size() == expected.byteCount);
        BAFX_CHECK(texture.width == expected.width);
        BAFX_CHECK(texture.height == expected.height);
        BAFX_CHECK(texture.pngBytes.size() >= 24U);
        BAFX_CHECK(std::equal(
            pngSignature.begin(),
            pngSignature.end(),
            texture.pngBytes.begin()));
        BAFX_CHECK(readBigEndian32(texture.pngBytes, 16U) == expected.width);
        BAFX_CHECK(readBigEndian32(texture.pngBytes, 20U) == expected.height);
        BAFX_CHECK(sha256(texture.pngBytes) == texture.pngSha256);
    }
}
