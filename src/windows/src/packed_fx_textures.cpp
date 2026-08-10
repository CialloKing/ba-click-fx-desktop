#include "packed_fx_textures.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>

namespace bafx::windows
{
namespace
{

#include "generated_packed_fx_texture_data.inc"

struct PackedTextureRecord
{
    std::string_view name;
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::size_t decodedByteCount{0U};
    std::string_view decodedSha256;
    std::span<const std::uint8_t> lz4Bytes;
};

[[nodiscard]] std::span<const std::uint8_t> bytesOf(
    const char* data,
    const std::size_t size) noexcept
{
    return {
        reinterpret_cast<const std::uint8_t*>(data),
        size};
}

[[nodiscard]] const std::array<PackedTextureRecord, 4>& records() noexcept
{
    static const std::array values{
        PackedTextureRecord{
            "center-disk",
            512U,
            512U,
            centerDiskDecodedByteCount,
            centerDiskDecodedSha256,
            bytesOf(centerDiskLz4Data, centerDiskLz4ByteCount)},
        PackedTextureRecord{
            "dissolve-ring",
            256U,
            128U,
            dissolveRingDecodedByteCount,
            dissolveRingDecodedSha256,
            bytesOf(dissolveRingLz4Data, dissolveRingLz4ByteCount)},
        PackedTextureRecord{
            "triangle-atlas",
            256U,
            128U,
            triangleAtlasDecodedByteCount,
            triangleAtlasDecodedSha256,
            bytesOf(triangleAtlasLz4Data, triangleAtlasLz4ByteCount)},
        PackedTextureRecord{
            "trail",
            512U,
            512U,
            trailDecodedByteCount,
            trailDecodedSha256,
            bytesOf(trailLz4Data, trailLz4ByteCount)}};
    return values;
}

[[nodiscard]] std::size_t readLength(
    const std::span<const std::uint8_t> source,
    std::size_t& sourceOffset,
    const std::size_t prefix)
{
    if (prefix != 15U)
    {
        return prefix;
    }

    std::size_t length = prefix;
    std::uint8_t extension = 0U;
    do
    {
        if (sourceOffset >= source.size())
        {
            throw std::runtime_error("Packed FX texture has a truncated LZ4 length");
        }
        extension = source[sourceOffset++];
        if (length > std::numeric_limits<std::size_t>::max() - extension)
        {
            throw std::overflow_error("Packed FX texture LZ4 length overflowed");
        }
        length += extension;
    } while (extension == 255U);
    return length;
}

void copyMatch(
    const std::span<std::uint8_t> output,
    const std::size_t outputOffset,
    const std::size_t distance,
    const std::size_t length)
{
    const std::size_t matchOffset = outputOffset - distance;
    if (distance >= length)
    {
        std::memcpy(
            output.data() + outputOffset,
            output.data() + matchOffset,
            length);
        return;
    }
    if (distance == 1U)
    {
        std::memset(output.data() + outputOffset, output[matchOffset], length);
        return;
    }

    // Grow the already decoded match exponentially. Every memcpy remains
    // non-overlapping, while long repeated runs avoid a byte-at-a-time loop.
    std::size_t copied = 0U;
    while (copied < length)
    {
        const std::size_t available = distance + copied;
        const std::size_t chunk = std::min(available, length - copied);
        std::memcpy(
            output.data() + outputOffset + copied,
            output.data() + matchOffset,
            chunk);
        copied += chunk;
    }
}

[[nodiscard]] std::unique_ptr<std::uint8_t[]> decodeLz4Block(
    const std::span<const std::uint8_t> source,
    const std::size_t decodedByteCount)
{
    if (source.empty() || decodedByteCount == 0U)
    {
        throw std::invalid_argument("Packed FX texture LZ4 ranges must not be empty");
    }

    // Every output byte is written by the decoder, so value-initializing this
    // buffer would add a redundant memory pass during startup.
    auto output = std::make_unique_for_overwrite<std::uint8_t[]>(
        decodedByteCount);
    const std::span<std::uint8_t> outputBytes{
        output.get(),
        decodedByteCount};
    std::size_t sourceOffset = 0U;
    std::size_t outputOffset = 0U;
    while (sourceOffset < source.size())
    {
        const std::uint8_t token = source[sourceOffset++];
        const std::size_t literalLength = readLength(
            source,
            sourceOffset,
            static_cast<std::size_t>(token >> 4U));
        if (literalLength > source.size() - sourceOffset
            || literalLength > outputBytes.size() - outputOffset)
        {
            throw std::runtime_error("Packed FX texture has an invalid LZ4 literal");
        }
        std::memcpy(
            outputBytes.data() + outputOffset,
            source.data() + sourceOffset,
            literalLength);
        sourceOffset += literalLength;
        outputOffset += literalLength;

        if (sourceOffset == source.size())
        {
            break;
        }
        if (source.size() - sourceOffset < 2U)
        {
            throw std::runtime_error("Packed FX texture has a truncated LZ4 offset");
        }

        const std::size_t distance = source[sourceOffset]
            | (static_cast<std::size_t>(source[sourceOffset + 1U]) << 8U);
        sourceOffset += 2U;
        if (distance == 0U || distance > outputOffset)
        {
            throw std::runtime_error("Packed FX texture has an invalid LZ4 offset");
        }

        const std::size_t matchPayload = readLength(
            source,
            sourceOffset,
            static_cast<std::size_t>(token & 0x0FU));
        if (matchPayload > std::numeric_limits<std::size_t>::max() - 4U)
        {
            throw std::overflow_error("Packed FX texture LZ4 match overflowed");
        }
        const std::size_t matchLength = matchPayload + 4U;
        if (matchLength > outputBytes.size() - outputOffset)
        {
            throw std::runtime_error("Packed FX texture has an invalid LZ4 match");
        }
        copyMatch(outputBytes, outputOffset, distance, matchLength);
        outputOffset += matchLength;
    }

    if (sourceOffset != source.size() || outputOffset != outputBytes.size())
    {
        throw std::runtime_error("Packed FX texture LZ4 output is incomplete");
    }
    return output;
}

}

DecodedPackedFxTexture decodePackedFxTexture(const PackedFxTextureId id)
{
    const std::size_t index = static_cast<std::size_t>(id);
    const auto& values = records();
    if (index >= values.size())
    {
        throw std::out_of_range("Unknown packed FX texture id");
    }

    const PackedTextureRecord& record = values[index];
    constexpr std::uint32_t rgba8BytesPerPixel = 4U;
    if (record.width
        > std::numeric_limits<std::uint32_t>::max() / rgba8BytesPerPixel)
    {
        throw std::overflow_error("Packed FX texture row pitch overflowed");
    }
    const std::uint32_t rowPitch = record.width * rgba8BytesPerPixel;
    if (record.height > std::numeric_limits<std::size_t>::max() / rowPitch
        || static_cast<std::size_t>(rowPitch) * record.height
            != record.decodedByteCount)
    {
        throw std::runtime_error("Packed FX texture dimensions do not match its payload");
    }

    return DecodedPackedFxTexture{
        record.name,
        record.width,
        record.height,
        rowPitch,
        record.decodedSha256,
        record.decodedByteCount,
        decodeLz4Block(record.lz4Bytes, record.decodedByteCount)};
}

}
