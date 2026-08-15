#include "bafx/windows/display_capabilities.hpp"

#include <dwmapi.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstddef>
#include <cmath>

namespace bafx::windows
{
namespace
{

using Microsoft::WRL::ComPtr;

struct OutputDescription1Abi final
{
    WCHAR deviceName[32]{};
    RECT desktopCoordinates{};
    BOOL attachedToDesktop{FALSE};
    DXGI_MODE_ROTATION rotation{DXGI_MODE_ROTATION_UNSPECIFIED};
    HMONITOR monitor{nullptr};
    UINT bitsPerColor{0U};
    DXGI_COLOR_SPACE_TYPE colorSpace{DXGI_COLOR_SPACE_CUSTOM};
    FLOAT redPrimary[2]{};
    FLOAT greenPrimary[2]{};
    FLOAT bluePrimary[2]{};
    FLOAT whitePoint[2]{};
    FLOAT minimumLuminance{0.0F};
    FLOAT maximumLuminance{0.0F};
    FLOAT maximumFullFrameLuminance{0.0F};
};

static_assert(offsetof(OutputDescription1Abi, monitor) == 88U);
static_assert(offsetof(OutputDescription1Abi, bitsPerColor)
    == 88U + sizeof(HMONITOR));
static_assert(sizeof(OutputDescription1Abi)
    == (sizeof(void*) == 8U ? 152U : 144U));

// IDXGIOutput6 first appeared in a newer SDK than the product's minimum build
// environment. The reserved methods preserve IDXGIOutput1-5's documented
// vtable slots, so runtime QueryInterface keeps HDR metadata in every build.
MIDL_INTERFACE("068346e8-aaec-4b84-add7-137f513f77a1")
Output6Abi : public IDXGIOutput
{
public:
    virtual HRESULT STDMETHODCALLTYPE reserved19() = 0;
    virtual HRESULT STDMETHODCALLTYPE reserved20() = 0;
    virtual HRESULT STDMETHODCALLTYPE reserved21() = 0;
    virtual HRESULT STDMETHODCALLTYPE reserved22() = 0;
    virtual HRESULT STDMETHODCALLTYPE reserved23() = 0;
    virtual HRESULT STDMETHODCALLTYPE reserved24() = 0;
    virtual HRESULT STDMETHODCALLTYPE reserved25() = 0;
    virtual HRESULT STDMETHODCALLTYPE reserved26() = 0;
    virtual HRESULT STDMETHODCALLTYPE getDescription1(
        OutputDescription1Abi* description) = 0;
};

// Keep Advanced Color device-info contracts local. Build SDK selection must
// not decide whether the resulting binary can probe a newer Windows runtime.
constexpr DISPLAYCONFIG_DEVICE_INFO_TYPE getAdvancedColorInfoType =
    static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(9);
constexpr DISPLAYCONFIG_DEVICE_INFO_TYPE getSdrWhiteLevelType =
    static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(11);
constexpr DISPLAYCONFIG_DEVICE_INFO_TYPE getAdvancedColorInfo2Type =
    static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15);
constexpr std::uint32_t advancedColorSupportedMask = 1U << 0U;
constexpr std::uint32_t advancedColorActiveMask = 1U << 1U;
constexpr std::uint32_t advancedColorLimitedByPolicyMask = 1U << 3U;
constexpr std::uint32_t highDynamicRangeSupportedMask = 1U << 4U;
constexpr std::uint32_t highDynamicRangeUserEnabledMask = 1U << 5U;
constexpr std::uint32_t wideColorSupportedMask = 1U << 6U;
constexpr std::uint32_t wideColorUserEnabledMask = 1U << 7U;
constexpr std::uint32_t advancedColorModeSdr = 0U;
constexpr std::uint32_t advancedColorModeWcg = 1U;
constexpr std::uint32_t advancedColorModeHdr = 2U;

struct AdvancedColorInfo2Abi final
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header{};
    std::uint32_t flags{0U};
    DISPLAYCONFIG_COLOR_ENCODING colorEncoding{
        DISPLAYCONFIG_COLOR_ENCODING_FORCE_UINT32};
    std::uint32_t bitsPerColorChannel{0U};
    std::uint32_t activeColorMode{advancedColorModeSdr};
};

struct AdvancedColorInfoAbi final
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header{};
    std::uint32_t flags{0U};
    DISPLAYCONFIG_COLOR_ENCODING colorEncoding{
        DISPLAYCONFIG_COLOR_ENCODING_FORCE_UINT32};
    std::uint32_t bitsPerColorChannel{0U};
};

struct SdrWhiteLevelAbi final
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header{};
    std::uint32_t sdrWhiteLevel{0U};
};

static_assert(sizeof(AdvancedColorInfo2Abi) == 36U);
static_assert(sizeof(AdvancedColorInfoAbi) == 32U);
static_assert(sizeof(SdrWhiteLevelAbi) == 24U);

[[nodiscard]] bool validLuminance(const float value) noexcept
{
    return std::isfinite(value) && value >= 0.0F;
}

[[nodiscard]] bool sameLuid(const LUID left, const LUID right) noexcept
{
    return left.HighPart == right.HighPart
        && left.LowPart == right.LowPart;
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

void queryAdvancedColorState(
    const DisplayPhysicalTarget& target,
    DisplayColorCapabilities& capabilities) noexcept
{
    AdvancedColorInfo2Abi advancedColor2{};
    advancedColor2.header.type = getAdvancedColorInfo2Type;
    advancedColor2.header.size = sizeof(advancedColor2);
    advancedColor2.header.adapterId = target.adapterLuid;
    advancedColor2.header.id = target.targetId;
    capabilities.advancedColorQueryResult =
        DisplayConfigGetDeviceInfo(&advancedColor2.header);
    if (capabilities.advancedColorQueryResult == ERROR_SUCCESS)
    {
        capabilities.advancedColorInfoV2 = true;
        capabilities.advancedColorSupported =
            (advancedColor2.flags & advancedColorSupportedMask) != 0U;
        capabilities.advancedColorActive =
            (advancedColor2.flags & advancedColorActiveMask) != 0U;
        capabilities.advancedColorLimitedByPolicy =
            (advancedColor2.flags & advancedColorLimitedByPolicyMask) != 0U;
        capabilities.highDynamicRangeSupported =
            (advancedColor2.flags & highDynamicRangeSupportedMask) != 0U;
        capabilities.highDynamicRangeUserEnabled =
            (advancedColor2.flags & highDynamicRangeUserEnabledMask) != 0U;
        capabilities.wideColorSupported =
            (advancedColor2.flags & wideColorSupportedMask) != 0U;
        capabilities.wideColorUserEnabled =
            (advancedColor2.flags & wideColorUserEnabledMask) != 0U;
        capabilities.colorEncoding = advancedColor2.colorEncoding;
        capabilities.displayConfigBitsPerColorChannel =
            advancedColor2.bitsPerColorChannel;
        switch (advancedColor2.activeColorMode)
        {
        case advancedColorModeSdr:
            capabilities.activeColorMode = DisplayColorMode::Sdr;
            break;
        case advancedColorModeWcg:
            capabilities.activeColorMode = DisplayColorMode::WideColorGamut;
            break;
        case advancedColorModeHdr:
            capabilities.activeColorMode = DisplayColorMode::Hdr;
            break;
        default:
            capabilities.activeColorMode = DisplayColorMode::Unknown;
            break;
        }
        return;
    }

    AdvancedColorInfoAbi advancedColor{};
    advancedColor.header.type = getAdvancedColorInfoType;
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
        (advancedColor.flags & advancedColorSupportedMask) != 0U;
    capabilities.advancedColorActive =
        (advancedColor.flags & advancedColorActiveMask) != 0U;
    capabilities.advancedColorLimitedByPolicy =
        (advancedColor.flags & advancedColorLimitedByPolicyMask) != 0U;
    capabilities.colorEncoding = advancedColor.colorEncoding;
    capabilities.displayConfigBitsPerColorChannel =
        advancedColor.bitsPerColorChannel;
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
    const DisplayPhysicalTarget& target,
    DisplayColorCapabilities& capabilities) noexcept
{
    SdrWhiteLevelAbi whiteLevel{};
    whiteLevel.header.type = getSdrWhiteLevelType;
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
    const float nits = static_cast<float>(whiteLevel.sdrWhiteLevel)
        * standardSdrWhiteNits / fixedPointScale;
    capabilities.sdrWhiteLevelValid = std::isfinite(nits)
        && nits > 0.0F
        && nits <= 10000.0F;
    if (capabilities.sdrWhiteLevelValid)
    {
        capabilities.sdrWhiteLevelNits = nits;
    }
}

[[nodiscard]] LONG firstQueryFailure(
    const LONG aggregate,
    const LONG sample) noexcept
{
    return aggregate == ERROR_SUCCESS ? sample : aggregate;
}

[[nodiscard]] std::uint32_t conservativeBitsPerChannel(
    const std::uint32_t aggregate,
    const std::uint32_t sample) noexcept
{
    if (aggregate == 0U || sample == 0U)
    {
        return 0U;
    }
    return (std::min)(aggregate, sample);
}

[[nodiscard]] bool sameAdvancedColorState(
    const DisplayColorCapabilities& left,
    const DisplayColorCapabilities& right) noexcept
{
    return left.advancedColorQueryResult == ERROR_SUCCESS
        && right.advancedColorQueryResult == ERROR_SUCCESS
        && left.activeColorMode == right.activeColorMode
        && left.colorEncoding == right.colorEncoding
        && left.displayConfigBitsPerColorChannel
            == right.displayConfigBitsPerColorChannel
        && left.advancedColorSupported == right.advancedColorSupported
        && left.advancedColorActive == right.advancedColorActive
        && left.advancedColorLimitedByPolicy
            == right.advancedColorLimitedByPolicy
        && left.highDynamicRangeSupported
            == right.highDynamicRangeSupported
        && left.highDynamicRangeUserEnabled
            == right.highDynamicRangeUserEnabled
        && left.wideColorSupported == right.wideColorSupported
        && left.wideColorUserEnabled == right.wideColorUserEnabled
        && left.advancedColorInfoV2 == right.advancedColorInfoV2;
}

[[nodiscard]] bool sameSdrWhiteLevel(
    const DisplayColorCapabilities& left,
    const DisplayColorCapabilities& right) noexcept
{
    return left.sdrWhiteLevelQueryResult == ERROR_SUCCESS
        && right.sdrWhiteLevelQueryResult == ERROR_SUCCESS
        && left.sdrWhiteLevelValid
        && right.sdrWhiteLevelValid
        && left.sdrWhiteLevelNits == right.sdrWhiteLevelNits;
}

void mergePhysicalTargetColorState(
    DisplayColorCapabilities& aggregate,
    const DisplayColorCapabilities& sample) noexcept
{
    aggregate.advancedColorQueryResult = firstQueryFailure(
        aggregate.advancedColorQueryResult,
        sample.advancedColorQueryResult);
    aggregate.sdrWhiteLevelQueryResult = firstQueryFailure(
        aggregate.sdrWhiteLevelQueryResult,
        sample.sdrWhiteLevelQueryResult);
    if (aggregate.activeColorMode != sample.activeColorMode)
    {
        aggregate.activeColorMode = DisplayColorMode::Unknown;
    }
    if (aggregate.colorEncoding != sample.colorEncoding)
    {
        aggregate.colorEncoding =
            DISPLAYCONFIG_COLOR_ENCODING_FORCE_UINT32;
    }
    aggregate.displayConfigBitsPerColorChannel =
        conservativeBitsPerChannel(
            aggregate.displayConfigBitsPerColorChannel,
            sample.displayConfigBitsPerColorChannel);
    aggregate.advancedColorSupported = aggregate.advancedColorSupported
        && sample.advancedColorSupported;
    aggregate.advancedColorActive = aggregate.advancedColorActive
        && sample.advancedColorActive;
    aggregate.advancedColorLimitedByPolicy =
        aggregate.advancedColorLimitedByPolicy
        || sample.advancedColorLimitedByPolicy;
    aggregate.highDynamicRangeSupported =
        aggregate.highDynamicRangeSupported
        && sample.highDynamicRangeSupported;
    aggregate.highDynamicRangeUserEnabled =
        aggregate.highDynamicRangeUserEnabled
        && sample.highDynamicRangeUserEnabled;
    aggregate.wideColorSupported = aggregate.wideColorSupported
        && sample.wideColorSupported;
    aggregate.wideColorUserEnabled = aggregate.wideColorUserEnabled
        && sample.wideColorUserEnabled;
    aggregate.advancedColorInfoV2 = aggregate.advancedColorInfoV2
        && sample.advancedColorInfoV2;

    if (aggregate.sdrWhiteLevelValid && sample.sdrWhiteLevelValid)
    {
        aggregate.sdrWhiteLevelNits = (std::min)(
            aggregate.sdrWhiteLevelNits,
            sample.sdrWhiteLevelNits);
    }
    else
    {
        aggregate.sdrWhiteLevelValid = false;
        aggregate.sdrWhiteLevelNits = 0.0F;
    }
}

void queryDisplayConfigColorState(
    const ActiveDisplayMonitor& display,
    DisplayColorCapabilities& capabilities) noexcept
{
    if (!display.sourceIdentityResolved || display.physicalTargets.empty())
    {
        return;
    }

    const DisplayColorCapabilities dxgiCapabilities = capabilities;
    DisplayColorCapabilities aggregate = dxgiCapabilities;
    DisplayColorCapabilities reference = dxgiCapabilities;
    bool first = true;
    bool advancedColorStateConsistent = true;
    bool sdrWhiteLevelConsistent = true;
    bool physicalTargetAdaptersConsistent = true;
    LUID physicalTargetAdapter{};
    for (const DisplayPhysicalTarget& target : display.physicalTargets)
    {
        DisplayColorCapabilities sample = dxgiCapabilities;
        queryAdvancedColorState(target, sample);
        querySdrWhiteLevel(target, sample);
        if (first)
        {
            physicalTargetAdapter = target.adapterLuid;
            aggregate = sample;
            reference = sample;
            advancedColorStateConsistent =
                sample.advancedColorQueryResult == ERROR_SUCCESS;
            sdrWhiteLevelConsistent = sample.sdrWhiteLevelQueryResult
                    == ERROR_SUCCESS
                && sample.sdrWhiteLevelValid;
            first = false;
            continue;
        }

        physicalTargetAdaptersConsistent =
            physicalTargetAdaptersConsistent
            && sameLuid(physicalTargetAdapter, target.adapterLuid);
        advancedColorStateConsistent = advancedColorStateConsistent
            && sameAdvancedColorState(reference, sample);
        sdrWhiteLevelConsistent = sdrWhiteLevelConsistent
            && sameSdrWhiteLevel(reference, sample);
        mergePhysicalTargetColorState(aggregate, sample);
    }

    capabilities = aggregate;
    capabilities.displayPathResolved = true;
    capabilities.adapterLuid = physicalTargetAdaptersConsistent
        ? physicalTargetAdapter
        : LUID{};
    capabilities.physicalTargetCount = static_cast<std::uint32_t>(
        display.physicalTargets.size());
    capabilities.targetId = display.physicalTargets.size() == 1U
        ? display.physicalTargets.front().targetId
        : 0U;
    capabilities.advancedColorStateConsistent =
        advancedColorStateConsistent;
    capabilities.sdrWhiteLevelConsistent = sdrWhiteLevelConsistent;
    capabilities.physicalTargetAdaptersConsistent =
        physicalTargetAdaptersConsistent;
    if (display.physicalTargets.size() > 1U)
    {
        // IDXGIOutput6 describes one enumerated output. A cloned source can
        // drive panels with different luminance envelopes, so do not promote
        // that one sample to source-wide metadata.
        capabilities.minimumLuminanceNits = 0.0F;
        capabilities.maximumLuminanceNits = 0.0F;
        capabilities.maximumFullFrameLuminanceNits = 0.0F;
        capabilities.luminanceMetadataValid = false;
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

                DisplayColorCapabilities capabilities{};
                bool dxgiDescriptionAvailable = false;
                ComPtr<Output6Abi> output6;
                if (SUCCEEDED(output.As(&output6)))
                {
                    OutputDescription1Abi description{};
                    if (SUCCEEDED(output6->getDescription1(&description)))
                    {
                        capabilities.colorSpace = description.colorSpace;
                        capabilities.bitsPerColor = description.bitsPerColor;
                        capabilities.minimumLuminanceNits =
                            description.minimumLuminance;
                        capabilities.maximumLuminanceNits =
                            description.maximumLuminance;
                        capabilities.maximumFullFrameLuminanceNits =
                            description.maximumFullFrameLuminance;
                        capabilities.luminanceMetadataValid =
                            validLuminance(description.minimumLuminance)
                            && validLuminance(description.maximumLuminance)
                            && validLuminance(
                                description.maximumFullFrameLuminance)
                            && description.maximumLuminance > 0.0F;
                        capabilities.activeColorMode = inferColorMode(
                            capabilities.colorSpace);
                        dxgiDescriptionAvailable = true;
                    }
                }

                DXGI_ADAPTER_DESC1 adapterDescription{};
                if (SUCCEEDED(adapter->GetDesc1(&adapterDescription)))
                {
                    capabilities.adapterLuid = adapterDescription.AdapterLuid;
                }
                const DisplayTopologySnapshot topology =
                    queryActiveDisplayTopology();
                const ActiveDisplayMonitor* const display =
                    findDisplayMonitor(topology, monitor);
                if (display != nullptr
                    && topology.status == DisplayTopologyStatus::Complete)
                {
                    // A cloned source has one DXGI output contract but several
                    // physical Advanced Color states. Aggregate only a complete
                    // topology so a missing hot-plug path cannot falsely enable
                    // HDR for the logical display.
                    queryDisplayConfigColorState(*display, capabilities);
                }
                if (!dxgiDescriptionAvailable
                    && !capabilities.displayPathResolved)
                {
                    // Neither source could describe this output. Preserve the
                    // existing null result instead of publishing empty facts.
                    return std::nullopt;
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
        timing.rateRefresh.uiDenominator,
        DisplayRefreshRateSource::DwmCompositionTiming};
}

}
