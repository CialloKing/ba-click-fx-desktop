#pragma once

#include "bafx/fx/simulation.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bafx::reference
{

struct UnityParticleFixtureDescriptor
{
    std::uint32_t schema{0U};
    std::string_view fixture{};
    std::string_view sourceFixture{};
    std::string_view sourceSha256{};
    bafx::fx::Viewport viewport{};
    std::uint32_t ageMilliseconds{0U};
    std::size_t particleCount{0U};
};

// This is a capture-only observation adapter. It must not seed or replace the
// production Simulation random streams.
[[nodiscard]] std::span<const UnityParticleFixtureDescriptor>
    unityParticleFixtureV2Descriptors() noexcept;
[[nodiscard]] bafx::fx::FrameSnapshot makeUnityParticleFixtureV2Snapshot(
    std::uint32_t ageMilliseconds);

}
