#include "test_support.hpp"

#include "display_output_retarget.hpp"
#include "display_target.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

[[nodiscard]] DisplayPhysicalTargetIdentity physicalTarget(
    std::wstring devicePath)
{
    DisplayPhysicalTargetIdentity identity{};
    identity.devicePath = std::move(devicePath);
    return identity;
}

[[nodiscard]] bafx::windows::DisplayPhysicalTarget cadenceTarget(
    const std::uint32_t virtualHertz,
    const std::optional<std::uint32_t> physicalHertz,
    const bool boosted = false)
{
    bafx::windows::DisplayPhysicalTarget target{};
    target.refreshRate = bafx::windows::DisplayRefreshRate{
        virtualHertz,
        1U,
        bafx::windows::DisplayRefreshRateSource::
            DisplayConfigVirtualRefresh};
    if (physicalHertz.has_value())
    {
        target.physicalRefreshRate = bafx::windows::DisplayRefreshRate{
            *physicalHertz,
            1U,
            bafx::windows::DisplayRefreshRateSource::
                DisplayConfigPhysicalRefresh};
    }
    target.available = true;
    target.dynamicRefreshRateBoosted = boosted;
    return target;
}

}

BAFX_TEST(display_capture_cadence_uses_consistent_clone_refresh_rate)
{
    const std::vector targets{
        cadenceTarget(120U, 120U),
        cadenceTarget(120U, 120U)};
    const bafx::windows::DisplayCaptureCadenceResolution cadence =
        bafx::windows::resolveDisplayCaptureCadence(targets);

    BAFX_CHECK(cadence.refreshRate.has_value());
    BAFX_CHECK(cadence.refreshRate->numerator == 120U);
    BAFX_CHECK(
        cadence.fallbackReason
        == bafx::windows::DisplayCaptureCadenceFallbackReason::None);
}

BAFX_TEST(display_capture_cadence_rejects_mixed_clone_refresh_rates)
{
    const std::vector targets{
        cadenceTarget(120U, 120U),
        cadenceTarget(60U, 60U)};
    const bafx::windows::DisplayCaptureCadenceResolution cadence =
        bafx::windows::resolveDisplayCaptureCadence(targets);

    BAFX_CHECK(!cadence.refreshRate.has_value());
    BAFX_CHECK(
        cadence.fallbackReason
        == bafx::windows::DisplayCaptureCadenceFallbackReason::
            MixedCloneRefreshRates);
}

BAFX_TEST(display_capture_cadence_requires_physical_rate_for_drr_boost)
{
    const std::vector resolvedTargets{
        cadenceTarget(60U, 144U, true)};
    const bafx::windows::DisplayCaptureCadenceResolution resolved =
        bafx::windows::resolveDisplayCaptureCadence(resolvedTargets);
    BAFX_CHECK(resolved.refreshRate.has_value());
    BAFX_CHECK(resolved.refreshRate->numerator == 144U);

    const std::vector unresolvedTargets{
        cadenceTarget(60U, std::nullopt, true)};
    const bafx::windows::DisplayCaptureCadenceResolution unresolved =
        bafx::windows::resolveDisplayCaptureCadence(unresolvedTargets);
    BAFX_CHECK(!unresolved.refreshRate.has_value());
    BAFX_CHECK(
        unresolved.fallbackReason
        == bafx::windows::DisplayCaptureCadenceFallbackReason::
            DrrPhysicalRefreshRateUnavailable);
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

BAFX_TEST(persistent_display_key_uses_only_case_normalized_target_path)
{
    DisplayTarget first{};
    first.monitor = monitor(1U);
    first.deviceName = L"\\\\.\\DISPLAY1";
    first.physicalTargetCount = 1U;
    first.physicalTargetIdentities = {
        physicalTarget(L"\\\\?\\DISPLAY#ACME123#A1#{GUID}")};

    DisplayTarget samePanel{};
    samePanel.monitor = monitor(99U);
    samePanel.deviceName = L"\\\\.\\DISPLAY9";
    samePanel.physicalTargetCount = 1U;
    samePanel.physicalTargetIdentities = {
        physicalTarget(L"\\\\?\\display#acme123#a1#{guid}")};

    const std::optional<std::string> firstKey =
        displayTargetPersistentKey(first);
    const std::optional<std::string> secondKey =
        displayTargetPersistentKey(samePanel);
    BAFX_CHECK(firstKey.has_value());
    BAFX_CHECK(secondKey.has_value());
    BAFX_CHECK(*firstKey == *secondKey);
    BAFX_CHECK(firstKey->starts_with("displayconfig-v1-sha256:"));
    BAFX_CHECK(firstKey->size() == 88U);
    BAFX_CHECK(firstKey->find("DISPLAY") == std::string::npos);
    BAFX_CHECK(
        *firstKey
        == "displayconfig-v1-sha256:"
           "f1aed1f41a34c5ab7f75b3dee7d107672bae6a21e04cf7e3e793e65c4b99aa0e");
}

BAFX_TEST(persistent_display_key_canonicalizes_clone_order)
{
    DisplayTarget first{};
    first.physicalTargetCount = 2U;
    first.physicalTargetIdentities = {
        physicalTarget(L"\\\\?\\DISPLAY#PANEL-B#2#{GUID}"),
        physicalTarget(L"\\\\?\\DISPLAY#PANEL-A#1#{GUID}")};

    DisplayTarget reversed{};
    reversed.physicalTargetCount = 2U;
    reversed.physicalTargetIdentities = {
        physicalTarget(L"\\\\?\\display#panel-a#1#{guid}"),
        physicalTarget(L"\\\\?\\display#panel-b#2#{guid}")};

    const std::optional<std::string> firstKey =
        displayTargetPersistentKey(first);
    const std::optional<std::string> reversedKey =
        displayTargetPersistentKey(reversed);
    BAFX_CHECK(firstKey.has_value());
    BAFX_CHECK(reversedKey.has_value());
    BAFX_CHECK(*firstKey == *reversedKey);

    reversed.physicalTargetIdentities[1U].devicePath =
        L"\\\\?\\display#panel-c#3#{guid}";
    const std::optional<std::string> changedKey =
        displayTargetPersistentKey(reversed);
    BAFX_CHECK(changedKey.has_value());
    BAFX_CHECK(*changedKey != *firstKey);
}

BAFX_TEST(persistent_display_key_rejects_incomplete_physical_identity)
{
    DisplayTarget noPhysicalTarget{};
    noPhysicalTarget.monitor = monitor(1U);
    noPhysicalTarget.deviceName = L"\\\\.\\DISPLAY1";
    BAFX_CHECK(!displayTargetPersistentKey(noPhysicalTarget).has_value());

    DisplayTarget incompleteClone{};
    incompleteClone.monitor = monitor(2U);
    incompleteClone.deviceName = L"\\\\.\\DISPLAY2";
    incompleteClone.physicalTargetCount = 2U;
    incompleteClone.physicalTargetIdentities = {
        physicalTarget(L"\\\\?\\DISPLAY#PANEL-A#1#{GUID}"),
        physicalTarget(L"")};
    BAFX_CHECK(!displayTargetPersistentKey(incompleteClone).has_value());

    incompleteClone.physicalTargetIdentities.pop_back();
    BAFX_CHECK(!displayTargetPersistentKey(incompleteClone).has_value());
}

BAFX_TEST(incomplete_topology_retains_path_for_the_same_physical_endpoint)
{
    DisplayTarget previous{};
    previous.deviceName = L"\\\\.\\DISPLAY1";
    previous.dpiX = 96U;
    previous.dpiY = 96U;
    previous.sourceAdapterLuid = LUID{10U, 1};
    previous.sourceId = 2U;
    previous.sourceAdapterResolved = true;
    previous.sourceIdentityResolved = true;
    previous.physicalTargetCount = 1U;
    previous.captureRefreshRate = bafx::windows::DisplayRefreshRate{
        60U,
        1U,
        bafx::windows::DisplayRefreshRateSource::DisplayConfigPath};
    previous.physicalTargetIdentities = {
        physicalTarget(L"\\\\?\\DISPLAY#PANEL-A#1#{GUID}")};
    previous.physicalTargetIdentities.front().adapterLuid = LUID{20U, 3};
    previous.physicalTargetIdentities.front().targetId = 4U;

    DisplayTarget observed = previous;
    observed.dpiX = 144U;
    observed.dpiY = 144U;
    observed.captureRefreshRate = bafx::windows::DisplayRefreshRate{
        120U,
        1U,
        bafx::windows::DisplayRefreshRateSource::DisplayConfigPath};
    observed.physicalTargetIdentities.front().devicePath.clear();
    observed.physicalTargetIdentities.front().captureRefreshRate =
        observed.captureRefreshRate;

    const DisplayTarget stabilized = stabilizeDisplayTargetObservation(
        previous,
        observed,
        bafx::windows::DisplayTopologyStatus::Incomplete);
    BAFX_CHECK(stabilized.dpiX == 144U);
    BAFX_CHECK(stabilized.captureRefreshRate.has_value());
    BAFX_CHECK(stabilized.captureRefreshRate->numerator == 120U);
    BAFX_CHECK(
        stabilized.physicalTargetIdentities.front().devicePath
        == previous.physicalTargetIdentities.front().devicePath);
    BAFX_CHECK(
        displayTargetPersistentKey(stabilized)
        == displayTargetPersistentKey(previous));
}

BAFX_TEST(incomplete_topology_does_not_inherit_path_across_endpoint_change)
{
    DisplayTarget previous{};
    previous.deviceName = L"\\\\.\\DISPLAY1";
    previous.sourceAdapterResolved = true;
    previous.physicalTargetCount = 1U;
    previous.physicalTargetIdentities = {
        physicalTarget(L"\\\\?\\DISPLAY#PANEL-A#1#{GUID}")};
    previous.physicalTargetIdentities.front().adapterLuid = LUID{20U, 3};
    previous.physicalTargetIdentities.front().targetId = 4U;

    DisplayTarget observed = previous;
    observed.physicalTargetIdentities.front().targetId = 5U;
    observed.physicalTargetIdentities.front().devicePath.clear();

    const DisplayTarget stabilized = stabilizeDisplayTargetObservation(
        previous,
        observed,
        bafx::windows::DisplayTopologyStatus::Incomplete);
    BAFX_CHECK(
        stabilized.physicalTargetIdentities.front().devicePath.empty());
    BAFX_CHECK(!displayTargetPersistentKey(stabilized).has_value());
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

BAFX_TEST(missing_color_capabilities_require_background_fail_closed)
{
    const bafx::windows::CompositionOutputPolicy policy =
        resolveDisplayOutputPolicy(
            bafx::windows::CompositionOutputPreference::ConservativeSdr,
            std::nullopt);

    BAFX_CHECK(
        policy.preference
        == bafx::windows::CompositionOutputPreference::ConservativeSdr);
    BAFX_CHECK(policy.mapping.backgroundReferenceWhiteRequired);
    BAFX_CHECK(!policy.mapping.backgroundReferenceWhiteValid);
    BAFX_CHECK_NEAR(
        policy.mapping.backgroundReferenceWhiteNits,
        0.0F,
        0.0F);
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
