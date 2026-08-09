#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <filesystem>

namespace bafx::windows
{

[[nodiscard]] Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> loadSrgbTexture(
    ID3D11Device* device,
    const std::filesystem::path& path);

}
