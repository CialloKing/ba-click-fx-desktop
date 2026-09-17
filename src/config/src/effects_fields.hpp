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
inline constexpr EffectsField effectsFields[] =
{
    {"bloomClamp", &EffectsConfig::bloomClamp},
    {"bloomDiffusion", &EffectsConfig::bloomDiffusion},
    {"bloomIntensity", &EffectsConfig::bloomIntensity},
    {"bloomLayerEnabled", &EffectsConfig::bloomLayerEnabled},
    {"bloomSoftKnee", &EffectsConfig::bloomSoftKnee},
    {"bloomThreshold", &EffectsConfig::bloomThreshold},
    {"clickEnabled", &EffectsConfig::clickEnabled},
    {"clickShardsLayerEnabled", &EffectsConfig::clickShardsLayerEnabled},
    {"clickTimeScale", &EffectsConfig::clickTimeScale},
    {"diskLayerEnabled", &EffectsConfig::diskLayerEnabled},
    {"diskLifetimeMs", &EffectsConfig::diskLifetimeMs},
    {"diskRadius", &EffectsConfig::diskRadius},
    {"enabled", &EffectsConfig::enabled},
    {"themeColor", &EffectsConfig::themeColor},
    {"globalScale", &EffectsConfig::globalScale},
    {"opacity", &EffectsConfig::opacity},
    {"ringsAngularVelocityMultiplier", &EffectsConfig::ringsAngularVelocityMultiplier},
    {"ringsCount", &EffectsConfig::ringsCount},
    {"ringsHdrIntensity", &EffectsConfig::ringsHdrIntensity},
    {"ringsLayerEnabled", &EffectsConfig::ringsLayerEnabled},
    {"ringsLifetimeMs", &EffectsConfig::ringsLifetimeMs},
    {"ringsRadiusMax", &EffectsConfig::ringsRadiusMax},
    {"ringsRadiusMin", &EffectsConfig::ringsRadiusMin},
    {"ringsRotationDirection", &EffectsConfig::ringsRotationDirection},
    {"shardsClickCount", &EffectsConfig::shardsClickCount},
    {"shardsClickLifetimeMaxMs", &EffectsConfig::shardsClickLifetimeMaxMs},
    {"shardsClickLifetimeMinMs", &EffectsConfig::shardsClickLifetimeMinMs},
    {"shardsClickRadius", &EffectsConfig::shardsClickRadius},
    {"shardsClickSpeedMax", &EffectsConfig::shardsClickSpeedMax},
    {"shardsClickSpeedMin", &EffectsConfig::shardsClickSpeedMin},
    {"shardsHdrIntensity", &EffectsConfig::shardsHdrIntensity},
    {"shardsSizeMax", &EffectsConfig::shardsSizeMax},
    {"shardsSizeMin", &EffectsConfig::shardsSizeMin},
    {"trailEnabled", &EffectsConfig::trailEnabled},
    {"trailLayerEnabled", &EffectsConfig::trailLayerEnabled},
    {"trailLength", &EffectsConfig::trailLength},
    {"trailLifetimeMs", &EffectsConfig::trailLifetimeMs},
    {"trailOpacity", &EffectsConfig::trailOpacity},
    {"trailShardsLayerEnabled", &EffectsConfig::trailShardsLayerEnabled},
    {"trailTimeScale", &EffectsConfig::trailTimeScale},
    {"trailWidth", &EffectsConfig::trailWidth},
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
