#pragma once

#include "bafx/config/config.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bafx::control_center
{

struct DisplayRefreshState final
{
    std::uint32_t numerator{0U};
    std::uint32_t denominator{0U};
};

enum class DisplayDriverState : std::uint8_t
{
    Hardware,
    Warp,
    Unknown
};

enum class DisplayOutputState : std::uint8_t
{
    ConservativeSdr,
    LinearScRgb,
    Unknown
};

enum class DisplayColorState : std::uint8_t
{
    Sdr,
    WideColorGamut,
    Hdr,
    Unknown
};

struct DisplaySessionState final
{
    std::string monitor{};
    std::string device{};
    std::optional<std::string> displayKey{};
    bool coordinator{false};
    bool primary{false};
    bool effectsEnabled{true};
    bool hdrEnabled{false};
    bafx::config::FramePacing framePacing{
        bafx::config::FramePacing::MatchDisplay};
    std::int32_t left{0};
    std::int32_t top{0};
    std::int32_t right{0};
    std::int32_t bottom{0};
    std::uint32_t targetDpiX{0U};
    std::uint32_t targetDpiY{0U};
    std::uint32_t windowDpi{0U};
    std::optional<DisplayRefreshState> displayRefresh{};
    std::optional<DisplayRefreshState> captureRefresh{};
    std::string adapter{};
    DisplayDriverState driver{DisplayDriverState::Unknown};
    DisplayOutputState requestedOutput{DisplayOutputState::Unknown};
    DisplayOutputState resolvedOutput{DisplayOutputState::Unknown};
    DisplayOutputState actualOutput{DisplayOutputState::Unknown};
    bool outputPolicySatisfied{false};
    DisplayColorState colorMode{DisplayColorState::Unknown};
    std::optional<bool> hdrSupported{};
    std::optional<bool> hdrActive{};
    bool backgroundCaptureActive{false};
    bool backgroundCaptureRestartAllowed{false};
    std::string backgroundCaptureFailure{};
    bool renderFaulted{false};
    bool outputContractFaulted{false};
};

struct DisplayState final
{
    std::uint64_t generation{0U};
    std::vector<DisplaySessionState> sessions{};
};

struct DisplayStateParseResult final
{
    std::optional<DisplayState> state{};
    std::string error{};

    [[nodiscard]] bool succeeded() const noexcept
    {
        return state.has_value();
    }
};

// GetDisplayState is a bounded product protocol, not a general JSON surface.
// Rejecting shape drift keeps the Control Center from presenting stale fields
// as current runtime facts.
[[nodiscard]] DisplayStateParseResult parseDisplayState(
    std::string_view json) noexcept;

}
