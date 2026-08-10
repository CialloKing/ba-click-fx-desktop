#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace bafx::windows
{

enum class EmbeddedUnityTextureId : std::uint8_t
{
    Circle01,
    GradRing3,
    Triangle02_1,
    Trail03,
    Trail03Coverage
};

struct EmbeddedUnityTexture
{
    std::string_view name;
    std::span<const std::uint8_t> pngBytes;
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::string_view pngSha256;
};

// The PNG containers are embedded as source text and decoded in memory; no
// game or Unity project path participates in the production runtime.
[[nodiscard]] const EmbeddedUnityTexture& embeddedUnityTexture(
    EmbeddedUnityTextureId id);

}
