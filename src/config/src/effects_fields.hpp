#pragma once

#include "bafx/config/config.hpp"

#include <array>
#include <string_view>
#include <variant>

namespace bafx::config::detail
{

using EffectsMember = std::variant<bool EffectsConfig::*, float EffectsConfig::*,
    std::uint32_t EffectsConfig::*, std::string EffectsConfig::*>;

struct EffectsField final
{
    std::string_view name;
    EffectsMember member;
};

// This is the canonical effects-only wire surface. Legacy aliases and coupled
// values are handled explicitly by the parser, not silently registered here.
// Preserve the established read order so documents with several invalid fields
// continue to report the same first error.
inline constexpr EffectsField effectsFields[] =
{
    {"enabled", &EffectsConfig::enabled},
    {"diskLayerEnabled", &EffectsConfig::diskLayerEnabled},
    {"ringsLayerEnabled", &EffectsConfig::ringsLayerEnabled},
    {"clickShardsLayerEnabled", &EffectsConfig::clickShardsLayerEnabled},
    {"trailShardsLayerEnabled", &EffectsConfig::trailShardsLayerEnabled},
    {"trailLayerEnabled", &EffectsConfig::trailLayerEnabled},
    {"bloomLayerEnabled", &EffectsConfig::bloomLayerEnabled},
    {"themeColor", &EffectsConfig::themeColor},
    {"globalScale", &EffectsConfig::globalScale},
    {"opacity", &EffectsConfig::opacity},
    {"clickEnabled", &EffectsConfig::clickEnabled},
    {"trailEnabled", &EffectsConfig::trailEnabled},
    {"trailLength", &EffectsConfig::trailLength},
    {"trailWidth", &EffectsConfig::trailWidth},
    {"clickTimeScale", &EffectsConfig::clickTimeScale},
    {"trailTimeScale", &EffectsConfig::trailTimeScale},
    {"trailLifetimeMs", &EffectsConfig::trailLifetimeMs},
    {"diskLifetimeMs", &EffectsConfig::diskLifetimeMs},
    {"diskRadius", &EffectsConfig::diskRadius},
    {"ringsCount", &EffectsConfig::ringsCount},
    {"ringsLifetimeMs", &EffectsConfig::ringsLifetimeMs},
    {"ringsRadiusMin", &EffectsConfig::ringsRadiusMin},
    {"ringsRadiusMax", &EffectsConfig::ringsRadiusMax},
    {"ringsAngularVelocityMultiplier", &EffectsConfig::ringsAngularVelocityMultiplier},
    {"ringsRotationDirection", &EffectsConfig::ringsRotationDirection},
    {"ringsHdrIntensity", &EffectsConfig::ringsHdrIntensity},
    {"shardsHdrIntensity", &EffectsConfig::shardsHdrIntensity},
    {"shardsClickCount", &EffectsConfig::shardsClickCount},
    {"shardsClickLifetimeMinMs", &EffectsConfig::shardsClickLifetimeMinMs},
    {"shardsClickLifetimeMaxMs", &EffectsConfig::shardsClickLifetimeMaxMs},
    {"shardsClickRadius", &EffectsConfig::shardsClickRadius},
    {"shardsClickSpeedMin", &EffectsConfig::shardsClickSpeedMin},
    {"shardsClickSpeedMax", &EffectsConfig::shardsClickSpeedMax},
    {"shardsSizeMin", &EffectsConfig::shardsSizeMin},
    {"shardsSizeMax", &EffectsConfig::shardsSizeMax},
    {"trailOpacity", &EffectsConfig::trailOpacity},
    {"bloomIntensity", &EffectsConfig::bloomIntensity},
    {"bloomDiffusion", &EffectsConfig::bloomDiffusion},
    {"bloomThreshold", &EffectsConfig::bloomThreshold},
    {"bloomSoftKnee", &EffectsConfig::bloomSoftKnee},
    {"bloomClamp", &EffectsConfig::bloomClamp},
};

inline constexpr auto effectsFieldNames = []
{
    std::array<std::string_view, std::size(effectsFields)> names{};
    for (std::size_t index = 0U; index < names.size(); ++index)
    {
        names[index] = effectsFields[index].name;
    }
    return names;
}();

static_assert([]
{
    for (std::size_t index = 0U; index < effectsFieldNames.size(); ++index)
    {
        for (std::size_t other = index + 1U; other < effectsFieldNames.size(); ++other)
        {
            if (effectsFieldNames[index] == effectsFieldNames[other])
            {
                return false;
            }
        }
    }
    return true;
}(), "Effects wire fields must be unique");

[[nodiscard]] inline const EffectsField* findEffectsField(std::string_view path) noexcept
{
    constexpr std::string_view prefix = "effects.";
    if (!path.starts_with(prefix))
    {
        return nullptr;
    }
    path.remove_prefix(prefix.size());
    for (const auto& field : effectsFields)
    {
        if (field.name == path)
        {
            return &field;
        }
    }
    return nullptr;
}

}
