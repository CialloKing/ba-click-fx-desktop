#include "bafx/windows/display_capabilities.hpp"

#include <dwmapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cmath>

namespace bafx::windows
{
namespace
{

using Microsoft::WRL::ComPtr;

[[nodiscard]] bool validLuminance(const float value) noexcept
{
    return std::isfinite(value) && value >= 0.0F;
}

}

std::optional<DisplayColorCapabilities> queryDisplayColorCapabilities(
    const HMONITOR monitor) noexcept
{
    if (monitor == nullptr)
    {
        return std::nullopt;
    }

    try
    {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        {
            return std::nullopt;
        }

        for (UINT adapterIndex = 0U;; ++adapterIndex)
        {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT adapterResult = factory->EnumAdapters1(
                adapterIndex,
                &adapter);
            if (adapterResult == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(adapterResult))
            {
                return std::nullopt;
            }

            for (UINT outputIndex = 0U;; ++outputIndex)
            {
                ComPtr<IDXGIOutput> output;
                const HRESULT outputResult = adapter->EnumOutputs(
                    outputIndex,
                    &output);
                if (outputResult == DXGI_ERROR_NOT_FOUND)
                {
                    break;
                }
                if (FAILED(outputResult))
                {
                    return std::nullopt;
                }

                DXGI_OUTPUT_DESC legacyDescription{};
                const HRESULT descriptionResult = output->GetDesc(
                    &legacyDescription);
                if (FAILED(descriptionResult))
                {
                    return std::nullopt;
                }
                if (legacyDescription.Monitor != monitor)
                {
                    continue;
                }

                ComPtr<IDXGIOutput6> output6;
                if (FAILED(output.As(&output6)))
                {
                    return std::nullopt;
                }

                DXGI_OUTPUT_DESC1 description{};
                if (FAILED(output6->GetDesc1(&description)))
                {
                    return std::nullopt;
                }
                const bool luminanceMetadataValid =
                    validLuminance(description.MinLuminance)
                    && validLuminance(description.MaxLuminance)
                    && validLuminance(description.MaxFullFrameLuminance)
                    && description.MaxLuminance > 0.0F;
                return DisplayColorCapabilities{
                    description.ColorSpace,
                    description.BitsPerColor,
                    description.MinLuminance,
                    description.MaxLuminance,
                    description.MaxFullFrameLuminance,
                    luminanceMetadataValid};
            }
        }
    }
    catch (...)
    {
        // Capability probing must not make the FX-only renderer unavailable.
    }
    return std::nullopt;
}

std::optional<DisplayRefreshRate> queryPrimaryCompositionRefreshRate() noexcept
{
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &timing))
        || timing.rateRefresh.uiNumerator == 0U
        || timing.rateRefresh.uiDenominator == 0U)
    {
        return std::nullopt;
    }

    const double hertz = static_cast<double>(timing.rateRefresh.uiNumerator)
        / static_cast<double>(timing.rateRefresh.uiDenominator);
    if (!std::isfinite(hertz) || hertz < 1.0 || hertz > 1000.0)
    {
        return std::nullopt;
    }
    return DisplayRefreshRate{
        timing.rateRefresh.uiNumerator,
        timing.rateRefresh.uiDenominator};
}

}
