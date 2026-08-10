#pragma once

#include "packed_fx_textures.hpp"

#include <d3d11.h>
#include <wrl/client.h>

namespace bafx::windows
{

// Decode, upload and release CPU pixels within one call. The returned immutable
// SRV is the only long-lived copy needed by the renderer.
[[nodiscard]] Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>
loadPackedFxTexture(
    ID3D11Device* device,
    PackedFxTextureId id);

}
