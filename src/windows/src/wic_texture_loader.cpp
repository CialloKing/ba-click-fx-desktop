#include "wic_texture_loader.hpp"

#include "bafx/windows/error.hpp"

#include <wincodec.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace bafx::windows
{

using Microsoft::WRL::ComPtr;

ComPtr<ID3D11ShaderResourceView> loadSrgbTexture(
    ID3D11Device* device,
    const std::filesystem::path& path)
{
    ComPtr<IWICImagingFactory> factory;
    throwIfFailed(
        CoCreateInstance(
            CLSID_WICImagingFactory2,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory)),
        "CoCreateInstance(WICImagingFactory2)");

    ComPtr<IWICBitmapDecoder> decoder;
    throwIfFailed(
        factory->CreateDecoderFromFilename(
            path.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnLoad,
            &decoder),
        "IWICImagingFactory::CreateDecoderFromFilename");

    ComPtr<IWICBitmapFrameDecode> frame;
    throwIfFailed(decoder->GetFrame(0, &frame), "IWICBitmapDecoder::GetFrame");

    UINT width = 0U;
    UINT height = 0U;
    throwIfFailed(frame->GetSize(&width, &height), "IWICBitmapFrameDecode::GetSize");
    if (width == 0U || height == 0U
        || width > std::numeric_limits<UINT>::max() / 4U)
    {
        throw std::runtime_error("WIC texture has an invalid extent");
    }
    const UINT rowPitch = width * 4U;
    if (height > std::numeric_limits<UINT>::max() / rowPitch)
    {
        throw std::overflow_error("WIC texture byte size exceeds D3D11 limits");
    }
    const UINT byteCount = rowPitch * height;

    ComPtr<IWICFormatConverter> converter;
    throwIfFailed(
        factory->CreateFormatConverter(&converter),
        "IWICImagingFactory::CreateFormatConverter");
    throwIfFailed(
        converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom),
        "IWICFormatConverter::Initialize(RGBA)");

    std::vector<std::uint8_t> pixels(byteCount);
    throwIfFailed(
        converter->CopyPixels(nullptr, rowPitch, byteCount, pixels.data()),
        "IWICBitmapSource::CopyPixels");

    D3D11_TEXTURE2D_DESC textureDescription{};
    textureDescription.Width = width;
    textureDescription.Height = height;
    textureDescription.MipLevels = 1;
    textureDescription.ArraySize = 1;
    textureDescription.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    textureDescription.SampleDesc = DXGI_SAMPLE_DESC{1, 0};
    textureDescription.Usage = D3D11_USAGE_IMMUTABLE;
    textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA textureData{};
    textureData.pSysMem = pixels.data();
    textureData.SysMemPitch = rowPitch;

    ComPtr<ID3D11Texture2D> texture;
    throwIfFailed(
        device->CreateTexture2D(&textureDescription, &textureData, &texture),
        "ID3D11Device::CreateTexture2D(Unity texture)");

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
    viewDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    viewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDescription.Texture2D.MostDetailedMip = 0;
    viewDescription.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> resource;
    throwIfFailed(
        device->CreateShaderResourceView(
            texture.Get(),
            &viewDescription,
            &resource),
        "ID3D11Device::CreateShaderResourceView(Unity texture)");
    return resource;
}

}
