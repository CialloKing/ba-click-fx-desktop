#include "bafx/windows/display_capabilities.hpp"

#include <dwmapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cmath>
#include <cwchar>
#include <vector>

namespace bafx::windows
{
namespace
{

using Microsoft::WRL::ComPtr;

struct DisplayPathTarget
{
    LUID adapterLuid{};
    std::uint32_t targetId{0U};
};

[[nodiscard]] bool validLuminance(const float value) noexcept
{
    return std::isfinite(value) && value >= 0.0F;
}

[[nodiscard]] bool hdrColorSpace(
    const DXGI_COLOR_SPACE_TYPE colorSpace) noexcept
{
    return colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
        || colorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
}

[[nodiscard]] DisplayColorMode inferColorMode(
    const DXGI_COLOR_SPACE_TYPE colorSpace) noexcept
{
    if (hdrColorSpace(colorSpace))
    {
        return DisplayColorMode::Hdr;
    }
    if (colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
        || colorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709)
    {
        return DisplayColorMode::Sdr;
    }
    return DisplayColorMode::Unknown;
}

[[nodiscard]] std::optional<DisplayPathTarget> findDisplayPathTarget(
    const HMONITOR monitor) noexcept
{
    MONITORINFOEXW monitorInformation{};
    monitorInformation.cbSize = sizeof(monitorInformation);
    if (!GetMonitorInfoW(monitor, &monitorInformation))
    {
        return std::nullopt;
    }

    constexpr UINT queryFlags = QDC_ONLY_ACTIVE_PATHS
        | QDC_VIRTUAL_MODE_AWARE;
    for (std::uint32_t attempt = 0U; attempt < 3U; ++attempt)
    {
        UINT32 pathCount = 0U;
        UINT32 modeCount = 0U;
        if (GetDisplayConfigBufferSizes(
                queryFlags,
                &pathCount,
                &modeCount) != ERROR_SUCCESS)
        {
            return std::nullopt;
        }

        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        const LONG queryResult = QueryDisplayConfig(
            queryFlags,
            &pathCount,
            paths.data(),
            &modeCount,
            modes.data(),
            nullptr);
        if (queryResult == ERROR_INSUFFICIENT_BUFFER)
        {
            // A hot-plug can change the required counts between the two API
            // calls. Retry from a new snapshot rather than mixing topologies.
            continue;
        }
        if (queryResult != ERROR_SUCCESS)
        {
            return std::nullopt;
        }
        paths.resize(pathCount);

        std::optional<DisplayPathTarget> matchingTarget{};
        for (const DISPLAYCONFIG_PATH_INFO& path : paths)
        {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
            sourceName.header.type =
                DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sourceName.header.size = sizeof(sourceName);
            sourceName.header.adapterId = path.sourceInfo.adapterId;
            sourceName.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sourceName.header)
                    != ERROR_SUCCESS
                || _wcsicmp(
                       sourceName.viewGdiDeviceName,
                       monitorInformation.szDevice) != 0)
            {
                continue;
            }
            const DisplayPathTarget target{
                path.targetInfo.adapterId,
                path.targetInfo.id};
            if (matchingTarget.has_value()
                && (matchingTarget->adapterLuid.HighPart
                        != target.adapterLuid.HighPart
                    || matchingTarget->adapterLuid.LowPart
                        != target.adapterLuid.LowPart
                    || matchingTarget->targetId != target.targetId))
            {
                // A cloned GDI source may drive more than one physical target.
                // HMONITOR cannot identify which target owns the color state,
                // so reporting either one would be fabricated capability data.
                return std::nullopt;
            }
            matchingTarget = target;
        }
        return matchingTarget;
    }
    return std::nullopt;
}

void queryAdvancedColorState(
    const DisplayPathTarget& target,
    DisplayColorCapabilities& capabilities) noexcept
{
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 advancedColor2{};
    advancedColor2.header.type =
        DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
    advancedColor2.header.size = sizeof(advancedColor2);
    advancedColor2.header.adapterId = target.adapterLuid;
    advancedColor2.header.id = target.targetId;
    capabilities.advancedColorQueryResult =
        DisplayConfigGetDeviceInfo(&advancedColor2.header);
    if (capabilities.advancedColorQueryResult == ERROR_SUCCESS)
    {
        capabilities.advancedColorInfoV2 = true;
        capabilities.advancedColorSupported =
            advancedColor2.advancedColorSupported != 0U;
        capabilities.advancedColorActive =
            advancedColor2.advancedColorActive != 0U;
        capabilities.advancedColorLimitedByPolicy =
            advancedColor2.advancedColorLimitedByPolicy != 0U;
        capabilities.colorEncoding = advancedColor2.colorEncoding;
        switch (advancedColor2.activeColorMode)
        {
        case DISPLAYCONFIG_ADVANCED_COLOR_MODE_SDR:
            capabilities.activeColorMode = DisplayColorMode::Sdr;
            break;
        case DISPLAYCONFIG_ADVANCED_COLOR_MODE_WCG:
            capabilities.activeColorMode = DisplayColorMode::WideColorGamut;
            break;
        case DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR:
            capabilities.activeColorMode = DisplayColorMode::Hdr;
            break;
        default:
            capabilities.activeColorMode = DisplayColorMode::Unknown;
            break;
        }
        return;
    }

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO advancedColor{};
    advancedColor.header.type =
        DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    advancedColor.header.size = sizeof(advancedColor);
    advancedColor.header.adapterId = target.adapterLuid;
    advancedColor.header.id = target.targetId;
    capabilities.advancedColorQueryResult =
        DisplayConfigGetDeviceInfo(&advancedColor.header);
    if (capabilities.advancedColorQueryResult != ERROR_SUCCESS)
    {
        return;
    }

    capabilities.advancedColorSupported =
        advancedColor.advancedColorSupported != 0U;
    capabilities.advancedColorActive =
        advancedColor.advancedColorEnabled != 0U;
    capabilities.advancedColorLimitedByPolicy =
        advancedColor.advancedColorForceDisabled != 0U;
    capabilities.colorEncoding = advancedColor.colorEncoding;
    if (!capabilities.advancedColorActive)
    {
        capabilities.activeColorMode = DisplayColorMode::Sdr;
    }
    else if (hdrColorSpace(capabilities.colorSpace))
    {
        capabilities.activeColorMode = DisplayColorMode::Hdr;
    }
    else
    {
        // The legacy contract cannot distinguish HDR from WCG directly. A PQ
        // output is HDR; every other enabled Advanced Color path is kept WCG.
        capabilities.activeColorMode = DisplayColorMode::WideColorGamut;
    }
}

void querySdrWhiteLevel(
    const DisplayPathTarget& target,
    DisplayColorCapabilities& capabilities) noexcept
{
    DISPLAYCONFIG_SDR_WHITE_LEVEL whiteLevel{};
    whiteLevel.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    whiteLevel.header.size = sizeof(whiteLevel);
    whiteLevel.header.adapterId = target.adapterLuid;
    whiteLevel.header.id = target.targetId;
    capabilities.sdrWhiteLevelQueryResult =
        DisplayConfigGetDeviceInfo(&whiteLevel.header);
    if (capabilities.sdrWhiteLevelQueryResult != ERROR_SUCCESS)
    {
        return;
    }

    constexpr float standardSdrWhiteNits = 80.0F;
    constexpr float fixedPointScale = 1000.0F;
    const float nits = static_cast<float>(whiteLevel.SDRWhiteLevel)
        * standardSdrWhiteNits / fixedPointScale;
    capabilities.sdrWhiteLevelValid = std::isfinite(nits)
        && nits > 0.0F
        && nits <= 10000.0F;
    if (capabilities.sdrWhiteLevelValid)
    {
        capabilities.sdrWhiteLevelNits = nits;
    }
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
                DisplayColorCapabilities capabilities{
                    description.ColorSpace,
                    description.BitsPerColor,
                    description.MinLuminance,
                    description.MaxLuminance,
                    description.MaxFullFrameLuminance,
                    luminanceMetadataValid};
                capabilities.activeColorMode = inferColorMode(
                    capabilities.colorSpace);

                DXGI_ADAPTER_DESC1 adapterDescription{};
                if (SUCCEEDED(adapter->GetDesc1(&adapterDescription)))
                {
                    capabilities.adapterLuid = adapterDescription.AdapterLuid;
                }
                const std::optional<DisplayPathTarget> target =
                    findDisplayPathTarget(monitor);
                if (target.has_value())
                {
                    capabilities.displayPathResolved = true;
                    capabilities.adapterLuid = target->adapterLuid;
                    capabilities.targetId = target->targetId;
                    queryAdvancedColorState(*target, capabilities);
                    querySdrWhiteLevel(*target, capabilities);
                }
                return capabilities;
            }
        }
    }
    catch (...)
    {
        // Capability probing must not make the FX-only renderer unavailable.
    }
    return std::nullopt;
}

std::string_view displayColorModeName(const DisplayColorMode mode) noexcept
{
    switch (mode)
    {
    case DisplayColorMode::Unknown:
        return "unknown";
    case DisplayColorMode::Sdr:
        return "sdr";
    case DisplayColorMode::WideColorGamut:
        return "wide-color-gamut";
    case DisplayColorMode::Hdr:
        return "hdr";
    }
    return "unknown";
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
