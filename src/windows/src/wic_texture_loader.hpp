#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <span>

namespace bafx::windows
{

[[nodiscard]] Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> loadSrgbTexture(
    ID3D11Device* device,
    std::span<const std::uint8_t> pngBytes);

}
