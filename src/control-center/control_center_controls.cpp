#include "control_center_window.hpp"

#include <array>

namespace bafx::control_center
{

std::span<const ControlCenterWindow::SliderDescriptor>
ControlCenterWindow::sliderDescriptors() noexcept
{
    // One binding owns creation order, numeric mapping and page membership.
    // Keep special layout geometry in the existing native layout implementation.
    static const SliderDescriptor descriptors[] =
    {
        {&ControlCenterWindow::globalScale_, TextId::EffectScale, ControlId::GlobalScale,
            0.1, 4.0, 0.05, "effects.globalScale", Page::Basic, std::nullopt,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.globalScale;
            }},
        {&ControlCenterWindow::trailLength_, TextId::TrailLength, ControlId::TrailLength,
            0.0, 10000.0 / 300.0, 0.05, "effects.trailLength", Page::Basic, std::nullopt,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.trailLength;
            }},
        {&ControlCenterWindow::trailWidth_, TextId::TrailWidth, ControlId::TrailWidth,
            0.1, 4.0, 0.05, "effects.trailWidth", Page::Basic, std::nullopt,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.trailWidth;
            }},
        {&ControlCenterWindow::inputSamplingRate_, TextId::SamplingRate, ControlId::InputSamplingRate,
            0.0, 1000.0, 1.0, "input.samplingRateHz", Page::Basic, std::nullopt,
            [](const bafx::config::Config& config) -> double
            {
                return config.input.samplingRateHz;
            }},
        {&ControlCenterWindow::bloomIntensity_, TextId::BloomIntensity, ControlId::BloomIntensity,
            0.0, 10.0, 0.05, "effects.bloomIntensity", Page::Basic, std::nullopt,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.bloomIntensity;
            }},
        {&ControlCenterWindow::opacity_, TextId::Opacity, ControlId::Opacity,
            0.0, 1.0, 0.01, "effects.opacity", Page::Advanced, AdvancedSection::Timing,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.opacity;
            }},
        {&ControlCenterWindow::clickTimeScale_, TextId::ClickSpeed, ControlId::ClickTimeScale,
            0.01, 4.0, 0.01, "effects.clickTimeScale", Page::Advanced, AdvancedSection::Timing,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.clickTimeScale;
            }},
        {&ControlCenterWindow::trailTimeScale_, TextId::TrailSpeed, ControlId::TrailTimeScale,
            0.01, 4.0, 0.01, "effects.trailTimeScale", Page::Advanced, AdvancedSection::Timing,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.trailTimeScale;
            }},
        {&ControlCenterWindow::trailLifetimeMs_, TextId::TrailLifetime, ControlId::TrailLifetimeMs,
            0.0, 10000.0, 1.0, "effects.trailLifetimeMs", Page::Advanced, AdvancedSection::Timing,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.trailLifetimeMs;
            }},
        {&ControlCenterWindow::bloomDiffusion_, TextId::BloomDiffusion, ControlId::BloomDiffusion,
            0.0, 10.0, 0.01, "effects.bloomDiffusion", Page::Advanced, AdvancedSection::Bloom,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.bloomDiffusion;
            }},
        {&ControlCenterWindow::bloomThreshold_, TextId::BloomThreshold, ControlId::BloomThreshold,
            0.0, 64.0, 0.01, "effects.bloomThreshold", Page::Advanced, AdvancedSection::Bloom,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.bloomThreshold;
            }},
        {&ControlCenterWindow::bloomSoftKnee_, TextId::BloomSoftKnee, ControlId::BloomSoftKnee,
            0.0, 1.0, 0.01, "effects.bloomSoftKnee", Page::Advanced, AdvancedSection::Bloom,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.bloomSoftKnee;
            }},
        {&ControlCenterWindow::bloomClamp_, TextId::BloomClamp, ControlId::BloomClamp,
            0.0, 65504.0, 1.0, "effects.bloomClamp", Page::Advanced, AdvancedSection::Bloom,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.bloomClamp;
            }},
        {&ControlCenterWindow::diskRadius_, TextId::DiskRadius, ControlId::DiskRadius,
            20.0, 120.0, 0.01, "effects.diskRadius", Page::Advanced, AdvancedSection::Particles,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.diskRadius;
            }},
        {&ControlCenterWindow::diskLifetimeMs_, TextId::DiskLifetime, ControlId::DiskLifetimeMs,
            50.0, 500.0, 1.0, "effects.diskLifetimeMs", Page::Advanced, AdvancedSection::Particles,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.diskLifetimeMs;
            }},
        {&ControlCenterWindow::ringsHdrIntensity_, TextId::RingsHdrIntensity, ControlId::RingsHdrIntensity,
            0.0, 8.0, 0.01, "effects.ringsHdrIntensity", Page::Advanced, AdvancedSection::Particles,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.ringsHdrIntensity;
            }},
        {&ControlCenterWindow::shardsHdrIntensity_, TextId::ShardsHdrIntensity, ControlId::ShardsHdrIntensity,
            0.0, 8.0, 0.01, "effects.shardsHdrIntensity", Page::Advanced, AdvancedSection::Particles,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.shardsHdrIntensity;
            }},
        {&ControlCenterWindow::trailOpacity_, TextId::TrailOpacity, ControlId::TrailOpacity,
            0.0, 1.0, 0.01, "effects.trailOpacity", Page::Advanced, AdvancedSection::Particles,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.trailOpacity;
            }},
        {&ControlCenterWindow::ringsCount_, TextId::RingCount, ControlId::RingsCount,
            0.0, 6.0, 1.0, "effects.ringsCount", Page::Advanced, AdvancedSection::Rings,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.ringsCount;
            }},
        {&ControlCenterWindow::ringsLifetimeMs_, TextId::RingLifetime, ControlId::RingsLifetimeMs,
            50.0, 2000.0, 1.0, "effects.ringsLifetimeMs", Page::Advanced, AdvancedSection::Rings,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.ringsLifetimeMs;
            }},
        {&ControlCenterWindow::ringsRadiusMin_, TextId::RingRadiusMin, ControlId::RingsRadiusMin,
            20.0, 120.0, 0.01, "effects.ringsRadiusMin", Page::Advanced, AdvancedSection::Rings,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.ringsRadiusMin;
            }},
        {&ControlCenterWindow::ringsRadiusMax_, TextId::RingRadiusMax, ControlId::RingsRadiusMax,
            20.0, 120.0, 0.01, "effects.ringsRadiusMax", Page::Advanced, AdvancedSection::Rings,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.ringsRadiusMax;
            }},
        {&ControlCenterWindow::ringsAngularVelocityMultiplier_, TextId::RingAngularVelocity, ControlId::RingsAngularVelocityMultiplier,
            1.0, 30.0, 0.01, "effects.ringsAngularVelocityMultiplier", Page::Advanced, AdvancedSection::Rings,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.ringsAngularVelocityMultiplier;
            }},
        {&ControlCenterWindow::ringsRotationDirection_, TextId::RingDirection, ControlId::RingsRotationDirection,
            -1.0, 1.0, 2.0, "effects.ringsRotationDirection", Page::Advanced, AdvancedSection::Rings,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.ringsRotationDirection;
            }},
        {&ControlCenterWindow::shardsClickCount_, TextId::ClickShardCount, ControlId::ShardsClickCount,
            0.0, 12.0, 1.0, "effects.shardsClickCount", Page::Advanced, AdvancedSection::ClickShards,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.shardsClickCount;
            }},
        {&ControlCenterWindow::shardsClickLifetimeMinMs_, TextId::LifetimeMin, ControlId::ShardsClickLifetimeMinMs,
            100.0, 1000.0, 1.0, "effects.shardsClickLifetimeMinMs", Page::Advanced, AdvancedSection::ClickShards,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.shardsClickLifetimeMinMs;
            }},
        {&ControlCenterWindow::shardsClickLifetimeMaxMs_, TextId::LifetimeMax, ControlId::ShardsClickLifetimeMaxMs,
            100.0, 1000.0, 1.0, "effects.shardsClickLifetimeMaxMs", Page::Advanced, AdvancedSection::ClickShards,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.shardsClickLifetimeMaxMs;
            }},
        {&ControlCenterWindow::shardsClickRadius_, TextId::SpawnRadius, ControlId::ShardsClickRadius,
            0.0, 200.0, 0.01, "effects.shardsClickRadius", Page::Advanced, AdvancedSection::ClickShards,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.shardsClickRadius;
            }},
        {&ControlCenterWindow::shardsClickSpeedMin_, TextId::SpeedMin, ControlId::ShardsClickSpeedMin,
            0.0, 200.0, 0.01, "effects.shardsClickSpeedMin", Page::Advanced, AdvancedSection::ClickShards,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.shardsClickSpeedMin;
            }},
        {&ControlCenterWindow::shardsClickSpeedMax_, TextId::SpeedMax, ControlId::ShardsClickSpeedMax,
            0.0, 200.0, 0.01, "effects.shardsClickSpeedMax", Page::Advanced, AdvancedSection::ClickShards,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.shardsClickSpeedMax;
            }},
        {&ControlCenterWindow::shardsSizeMin_, TextId::ShardSizeMin, ControlId::ShardsSizeMin,
            0.0, 100.0, 0.01, "effects.shardsSizeMin", Page::Advanced, AdvancedSection::ClickShards,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.shardsSizeMin;
            }},
        {&ControlCenterWindow::shardsSizeMax_, TextId::ShardSizeMax, ControlId::ShardsSizeMax,
            0.0, 100.0, 0.01, "effects.shardsSizeMax", Page::Advanced, AdvancedSection::ClickShards,
            [](const bafx::config::Config& config) -> double
            {
                return config.effects.shardsSizeMax;
            }},
    };
    return descriptors;
}

std::span<const ControlCenterWindow::PageControlDescriptor>
ControlCenterWindow::pageControlDescriptors() noexcept
{
    static const PageControlDescriptor descriptors[] =
    {
        {&ControlCenterWindow::effectsHeading_, Page::Basic, std::nullopt, true},
        {&ControlCenterWindow::effectsEnabled_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::effectsModeLabel_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::effectsMode_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::clickEnabled_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::trailEnabled_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::trailAlwaysOn_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::leftClickEnabled_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::rightClickEnabled_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::middleClickEnabled_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::bloomQualityLabel_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::bloomQuality_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::backgroundHeading_, Page::Basic, std::nullopt, true},
        {&ControlCenterWindow::backgroundModeLabel_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::backgroundMode_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::cursorExcluded_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::allowSystemBorder_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::idleOptimization_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::fxProfileLabel_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::fxProfileSelector_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::fxProfileNameEdit_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::applyFxProfileButton_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::saveFxProfileButton_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::deleteFxProfileButton_, Page::Basic, std::nullopt, false},
        {&ControlCenterWindow::advancedTimingSectionButton_, Page::Advanced, std::nullopt, false},
        {&ControlCenterWindow::advancedParticlesSectionButton_, Page::Advanced, std::nullopt, false},
        {&ControlCenterWindow::advancedRingsSectionButton_, Page::Advanced, std::nullopt, false},
        {&ControlCenterWindow::advancedClickShardsSectionButton_, Page::Advanced, std::nullopt, false},
        {&ControlCenterWindow::advancedBloomSectionButton_, Page::Advanced, std::nullopt, false},
        {&ControlCenterWindow::advancedLayersSectionButton_, Page::Advanced, std::nullopt, false},
        {&ControlCenterWindow::advancedTimingHeading_, Page::Advanced, AdvancedSection::Timing, true},
        {&ControlCenterWindow::advancedParticlesHeading_, Page::Advanced, AdvancedSection::Particles, true},
        {&ControlCenterWindow::themeColorLabel_, Page::Advanced, AdvancedSection::Particles, false},
        {&ControlCenterWindow::themeColorEdit_, Page::Advanced, AdvancedSection::Particles, false},
        {&ControlCenterWindow::themeColorPreview_, Page::Advanced, AdvancedSection::Particles, false},
        {&ControlCenterWindow::themeColorChoose_, Page::Advanced, AdvancedSection::Particles, false},
        {&ControlCenterWindow::advancedRingsHeading_, Page::Advanced, AdvancedSection::Rings, true},
        {&ControlCenterWindow::advancedClickShardsHeading_, Page::Advanced, AdvancedSection::ClickShards, true},
        {&ControlCenterWindow::advancedBloomHeading_, Page::Advanced, AdvancedSection::Bloom, true},
        {&ControlCenterWindow::advancedLayersHeading_, Page::Advanced, AdvancedSection::Layers, true},
        {&ControlCenterWindow::diskLayerEnabled_, Page::Advanced, AdvancedSection::Layers, false},
        {&ControlCenterWindow::ringsLayerEnabled_, Page::Advanced, AdvancedSection::Layers, false},
        {&ControlCenterWindow::clickShardsLayerEnabled_, Page::Advanced, AdvancedSection::Layers, false},
        {&ControlCenterWindow::trailShardsLayerEnabled_, Page::Advanced, AdvancedSection::Layers, false},
        {&ControlCenterWindow::trailLayerEnabled_, Page::Advanced, AdvancedSection::Layers, false},
        {&ControlCenterWindow::bloomLayerEnabled_, Page::Advanced, AdvancedSection::Layers, false},
        {&ControlCenterWindow::displaySettingsHeading_, Page::DisplayPerformance, std::nullopt, true},
        {&ControlCenterWindow::displaySelectorLabel_, Page::DisplayPerformance, std::nullopt, false},
        {&ControlCenterWindow::displaySelector_, Page::DisplayPerformance, std::nullopt, false},
        {&ControlCenterWindow::displaySummaryText_, Page::DisplayPerformance, std::nullopt, false},
        {&ControlCenterWindow::hdrEnabled_, Page::DisplayPerformance, std::nullopt, false},
        {&ControlCenterWindow::activeFxRoiEnabled_, Page::DisplayPerformance, std::nullopt, false},
        {&ControlCenterWindow::framePacingLabel_, Page::DisplayPerformance, std::nullopt, false},
        {&ControlCenterWindow::framePacing_, Page::DisplayPerformance, std::nullopt, false},
        {&ControlCenterWindow::displayIndependent_, Page::DisplayPerformance, std::nullopt, false},
        {&ControlCenterWindow::displayEffectsEnabled_, Page::DisplayPerformance, std::nullopt, false},
        {&ControlCenterWindow::displayHdrEnabled_, Page::DisplayPerformance, std::nullopt, false},
        {&ControlCenterWindow::displayFramePacingLabel_, Page::DisplayPerformance, std::nullopt, false},
        {&ControlCenterWindow::displayFramePacing_, Page::DisplayPerformance, std::nullopt, false},
        {&ControlCenterWindow::displayDetailsHeading_, Page::DisplayPerformance, std::nullopt, true},
        {&ControlCenterWindow::displayDetailsText_, Page::DisplayPerformance, std::nullopt, false},
        {&ControlCenterWindow::activeFxRoiDetailsHeading_, Page::DisplayPerformance, std::nullopt, true},
        {&ControlCenterWindow::activeFxRoiDetailsText_, Page::DisplayPerformance, std::nullopt, false},
        {&ControlCenterWindow::systemSettingsHeading_, Page::System, std::nullopt, true},
        {&ControlCenterWindow::languageLabel_, Page::System, std::nullopt, false},
        {&ControlCenterWindow::languageSelector_, Page::System, std::nullopt, false},
        {&ControlCenterWindow::startWithWindows_, Page::System, std::nullopt, false},
        {&ControlCenterWindow::startMinimized_, Page::System, std::nullopt, false},
        {&ControlCenterWindow::closeToTray_, Page::System, std::nullopt, false},
        {&ControlCenterWindow::versionUpdateHeading_, Page::System, std::nullopt, true},
        {&ControlCenterWindow::controlCenterVersionText_, Page::System, std::nullopt, false},
        {&ControlCenterWindow::hostVersionText_, Page::System, std::nullopt, false},
        {&ControlCenterWindow::installStateText_, Page::System, std::nullopt, false},
        {&ControlCenterWindow::latestVersionText_, Page::System, std::nullopt, false},
        {&ControlCenterWindow::checkForUpdatesButton_, Page::System, std::nullopt, false},
        {&ControlCenterWindow::openReleaseButton_, Page::System, std::nullopt, false},
        {&ControlCenterWindow::repositoryStarHint_, Page::System, std::nullopt, false},
        {&ControlCenterWindow::openRepositoryButton_, Page::System, std::nullopt, false},
#if defined(BAFX_ENABLE_SPOUT2)
        {&ControlCenterWindow::spout2Enabled_, Page::System, std::nullopt, false},
        {&ControlCenterWindow::spout2SenderStatus_, Page::System, std::nullopt, false},
        {&ControlCenterWindow::obsSpoutPluginStatus_, Page::System, std::nullopt, false},
        {&ControlCenterWindow::spout2ObsHint_, Page::System, std::nullopt, false},
        {&ControlCenterWindow::refreshObsSpoutPluginButton_, Page::System, std::nullopt, false},
        {&ControlCenterWindow::openObsSpoutPluginPageButton_, Page::System, std::nullopt, false},
#endif
        {&ControlCenterWindow::openLogDirectoryButton_, Page::System, std::nullopt, false},
        {&ControlCenterWindow::clearLogsButton_, Page::System, std::nullopt, false},
    };
    return descriptors;
}

bool ControlCenterWindow::pageControlVisible(
    const Page page, const std::optional<AdvancedSection> section) const noexcept
{
    return activePage_ == page
        && (!section.has_value() || activeAdvancedSection_ == *section);
}

bool ControlCenterWindow::createSliders()
{
    bool created = true;
    for (const auto& descriptor : sliderDescriptors())
    {
        created = createSlider(this->*descriptor.control, descriptor.label,
            descriptor.minimum, descriptor.maximum, descriptor.step,
            std::string(descriptor.path), descriptor.id) && created;
    }
    return created;
}

}
