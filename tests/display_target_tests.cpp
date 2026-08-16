#include "test_support.hpp"

#include "display_output_retarget.hpp"
#include "display_target.hpp"

#include <cstdint>

using namespace bafx::desktop;

namespace
{

[[nodiscard]] HMONITOR monitor(const std::uintptr_t value) noexcept
{
    return reinterpret_cast<HMONITOR>(value);
}

[[nodiscard]] bafx::windows::DisplayColorCapabilities hdrCapabilities(
    const float referenceWhiteNits,
    const bool referenceWhiteValid = true) noexcept
{
    bafx::windows::DisplayColorCapabilities capabilities{};
    capabilities.displayPathResolved = true;
    capabilities.advancedColorQueryResult = ERROR_SUCCESS;
    capabilities.advancedColorStateConsistent = true;
    capabilities.advancedColorActive = true;
    capabilities.activeColorMode = bafx::windows::DisplayColorMode::Hdr;
    capabilities.sdrWhiteLevelQueryResult = referenceWhiteValid
        ? ERROR_SUCCESS
        : ERROR_GEN_FAILURE;
    capabilities.sdrWhiteLevelNits = referenceWhiteNits;
    capabilities.sdrWhiteLevelValid = referenceWhiteValid;
    capabilities.sdrWhiteLevelConsistent = referenceWhiteValid;
    return capabilities;
}

}

BAFX_TEST(display_target_identity_includes_monitor_device_and_bounds)
{
    const DisplayTarget first{
        monitor(1U),
        L"\\\\.\\DISPLAY1",
        RECT{0, 0, 1920, 1080}};
    const DisplayTarget same = first;
    const DisplayTarget otherMonitor{
        monitor(2U),
        L"\\\\.\\DISPLAY2",
        RECT{0, 0, 1920, 1080}};

    BAFX_CHECK(sameDisplayTarget(first, same));
    BAFX_CHECK(!sameDisplayTarget(first, otherMonitor));
}

BAFX_TEST(display_target_identity_preserves_negative_virtual_coordinates)
{
    const DisplayTarget first{
        monitor(1U),
        L"\\\\.\\DISPLAY1",
        RECT{-1920, 0, 0, 1080}};
    const DisplayTarget moved{
        monitor(1U),
        L"\\\\.\\DISPLAY1",
        RECT{0, 0, 1920, 1080}};

    BAFX_CHECK(!sameDisplayTarget(first, moved));
    const bafx::windows::WindowSize size = displayTargetSize(first);
    BAFX_CHECK(size.width == 1920U);
    BAFX_CHECK(size.height == 1080U);
}

BAFX_TEST(display_target_intent_pins_geometry_application)
{
    const DisplayTarget target{
        monitor(1U),
        L"\\\\.\\DISPLAY1",
        RECT{0, 0, 2560, 1440}};
    const DisplayTargetIntent stable{target, false};
    const DisplayTargetIntent topologyChange{target, true};

    BAFX_CHECK(sameDisplayTargetIntent(stable, stable));
    BAFX_CHECK(!sameDisplayTargetIntent(stable, topologyChange));
}

BAFX_TEST(display_target_diagnostic_format_preserves_identity_and_origin)
{
    const DisplayTarget target{
        monitor(0x2AU),
        L"\\\\.\\DISPLAY2",
        RECT{-2560, 0, 0, 1440}};

    BAFX_CHECK(displayTargetDeviceUtf8(target) == "\\\\.\\DISPLAY2");
    BAFX_CHECK(formatDisplayTargetBounds(target) == "2560x1440@-2560,0");
    const std::string monitorText = formatDisplayTargetMonitor(target);
    BAFX_CHECK(monitorText.starts_with("0x"));
    BAFX_CHECK(monitorText.ends_with("2A"));
}

BAFX_TEST(conservative_sdr_keeps_verified_background_reference_white)
{
    const bafx::windows::DisplayColorCapabilities capabilities =
        hdrCapabilities(203.0F);
    const bafx::windows::CompositionOutputPolicy policy =
        resolveDisplayOutputPolicy(
            bafx::windows::CompositionOutputPreference::ConservativeSdr,
            capabilities);

    BAFX_CHECK(
        policy.preference
        == bafx::windows::CompositionOutputPreference::ConservativeSdr);
    BAFX_CHECK(
        policy.mapping.mode
        == bafx::windows::CompositionOutputMappingMode::ConservativeSdr);
    BAFX_CHECK(!policy.mapping.referenceWhiteValid);
    BAFX_CHECK(policy.mapping.backgroundReferenceWhiteRequired);
    BAFX_CHECK(policy.mapping.backgroundReferenceWhiteValid);
    BAFX_CHECK_NEAR(
        policy.mapping.backgroundReferenceWhiteNits,
        203.0F,
        0.0F);
}

BAFX_TEST(unverified_reference_white_cannot_promote_hdr_output)
{
    const bafx::windows::DisplayColorCapabilities capabilities =
        hdrCapabilities(0.0F, false);
    const bafx::windows::CompositionOutputPolicy policy =
        resolveDisplayOutputPolicy(
            bafx::windows::CompositionOutputPreference::PreferLinearScRgb,
            capabilities);

    BAFX_CHECK(
        policy.preference
        == bafx::windows::CompositionOutputPreference::ConservativeSdr);
    BAFX_CHECK(!policy.mapping.referenceWhiteValid);
    BAFX_CHECK(policy.mapping.backgroundReferenceWhiteRequired);
    BAFX_CHECK(!policy.mapping.backgroundReferenceWhiteValid);
}

BAFX_TEST(non_positive_reference_white_cannot_promote_hdr_output)
{
    const bafx::windows::DisplayColorCapabilities capabilities =
        hdrCapabilities(0.0F, true);
    const bafx::windows::CompositionOutputPolicy policy =
        resolveDisplayOutputPolicy(
            bafx::windows::CompositionOutputPreference::PreferLinearScRgb,
            capabilities);

    BAFX_CHECK(
        policy.preference
        == bafx::windows::CompositionOutputPreference::ConservativeSdr);
    BAFX_CHECK(policy.mapping.backgroundReferenceWhiteRequired);
    BAFX_CHECK(!policy.mapping.backgroundReferenceWhiteValid);
}

BAFX_TEST(legacy_dxgi_only_hdr_may_keep_unit_background_white)
{
    bafx::windows::DisplayColorCapabilities capabilities =
        hdrCapabilities(0.0F, false);
    capabilities.displayPathResolved = false;
    capabilities.displayConfigTopologyStatus =
        bafx::windows::DisplayTopologyStatus::Incomplete;
    capabilities.displayConfigTopologyError = ERROR_NOT_SUPPORTED;

    const bafx::windows::CompositionOutputPolicy policy =
        resolveDisplayOutputPolicy(
            bafx::windows::CompositionOutputPreference::ConservativeSdr,
            capabilities);

    BAFX_CHECK(!policy.mapping.backgroundReferenceWhiteRequired);
    BAFX_CHECK(!policy.mapping.backgroundReferenceWhiteValid);
    BAFX_CHECK_NEAR(
        policy.mapping.backgroundReferenceWhiteNits,
        0.0F,
        0.0F);
}

BAFX_TEST(sdr_with_unknown_white_does_not_require_background_white)
{
    bafx::windows::DisplayColorCapabilities capabilities =
        hdrCapabilities(0.0F, false);
    capabilities.activeColorMode = bafx::windows::DisplayColorMode::Sdr;

    const bafx::windows::CompositionOutputPolicy policy =
        resolveDisplayOutputPolicy(
            bafx::windows::CompositionOutputPreference::ConservativeSdr,
            capabilities);

    BAFX_CHECK(!policy.mapping.backgroundReferenceWhiteRequired);
    BAFX_CHECK(!policy.mapping.backgroundReferenceWhiteValid);
}

BAFX_TEST(wide_color_with_unknown_white_requires_background_white)
{
    bafx::windows::DisplayColorCapabilities capabilities =
        hdrCapabilities(0.0F, false);
    capabilities.activeColorMode =
        bafx::windows::DisplayColorMode::WideColorGamut;

    const bafx::windows::CompositionOutputPolicy policy =
        resolveDisplayOutputPolicy(
            bafx::windows::CompositionOutputPreference::ConservativeSdr,
            capabilities);

    BAFX_CHECK(policy.mapping.backgroundReferenceWhiteRequired);
    BAFX_CHECK(!policy.mapping.backgroundReferenceWhiteValid);
}

BAFX_TEST(background_white_recovery_changes_the_output_contract)
{
    const bafx::windows::DisplayColorCapabilities unavailable =
        hdrCapabilities(0.0F, false);
    const bafx::windows::DisplayColorCapabilities recovered =
        hdrCapabilities(203.0F);

    BAFX_CHECK(displayOutputContractChanged(
        bafx::windows::CompositionOutputPreference::ConservativeSdr,
        bafx::windows::CompositionOutputPreference::ConservativeSdr,
        unavailable,
        recovered));
}

BAFX_TEST(sdr_background_white_change_is_an_output_contract_change)
{
    const bafx::windows::DisplayColorCapabilities previous =
        hdrCapabilities(203.0F);
    const bafx::windows::DisplayColorCapabilities current =
        hdrCapabilities(250.0F);

    BAFX_CHECK(displayOutputContractChanged(
        bafx::windows::CompositionOutputPreference::ConservativeSdr,
        bafx::windows::CompositionOutputPreference::ConservativeSdr,
        previous,
        current));
}
