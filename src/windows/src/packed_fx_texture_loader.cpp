#include "packed_fx_texture_loader.hpp"

#include "bafx/windows/error.hpp"

#include <stdexcept>

namespace bafx::windows
{

using Microsoft::WRL::ComPtr;

ComPtr<ID3D11ShaderResourceView> loadPackedFxTexture(
    ID3D11Device* device,
    const PackedFxTextureId id)
{
    if (device == nullptr)
    {
        throw std::invalid_argument("Packed FX texture upload requires a D3D11 device");
    }

    const DecodedPackedFxTexture decoded = decodePackedFxTexture(id);

    D3D11_TEXTURE2D_DESC textureDescription{};
    textureDescription.Width = decoded.width;
    textureDescription.Height = decoded.height;
    textureDescription.MipLevels = 1U;
    textureDescription.ArraySize = 1U;
    textureDescription.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    textureDescription.SampleDesc = DXGI_SAMPLE_DESC{1U, 0U};
    textureDescription.Usage = D3D11_USAGE_IMMUTABLE;
    textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA textureData{};
    textureData.pSysMem = decoded.pixels.get();
    textureData.SysMemPitch = decoded.rowPitch;

    ComPtr<ID3D11Texture2D> texture;
    throwIfFailed(
        device->CreateTexture2D(&textureDescription, &textureData, &texture),
        "ID3D11Device::CreateTexture2D(packed FX texture)");

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
    viewDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    viewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDescription.Texture2D.MostDetailedMip = 0U;
    viewDescription.Texture2D.MipLevels = 1U;

    ComPtr<ID3D11ShaderResourceView> resource;
    throwIfFailed(
        device->CreateShaderResourceView(
            texture.Get(),
            &viewDescription,
            &resource),
        "ID3D11Device::CreateShaderResourceView(packed FX texture)");
    return resource;
}

}
