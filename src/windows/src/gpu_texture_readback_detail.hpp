#pragma once

#include "bafx/windows/gpu_texture_readback.hpp"

#include <cstddef>
#include <cstdint>

namespace bafx::windows::detail
{

[[nodiscard]] Rgba16FloatImage copyMappedRgba16FloatRows(
    const void* data,
    std::size_t rowPitch,
    std::uint32_t width,
    std::uint32_t height);

}
