#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bafx::control_center
{

struct FxProfileState final
{
    std::string name{};
    bool builtIn{false};
};

enum class HostProductVersionStatus
{
    Match,
    Missing,
    Invalid,
    Mismatch
};

struct HostState final
{
    std::optional<std::string> productVersion{};
    HostProductVersionStatus productVersionStatus{
        HostProductVersionStatus::Missing};
    std::uint64_t generation{0U};
    bool paused{false};
    std::string backgroundCapture{};
    bool spout2Enabled{false};
    std::string spout2Sender{};
    std::string spout2Status{};
    std::string spout2Error{};
    std::string spout2OutputContract{};
    std::vector<FxProfileState> fxProfiles{};
    std::string activeFxProfile{"自定义"};
    std::string fxProfileWarning{};
    std::optional<std::string> hotkeysJson{};
    std::uint64_t hotkeyRegisteredMask{0U};
    std::uint64_t hotkeyCleanupError{0U};
    std::uint64_t hotkeyCaptureToken{0U};
    std::uint64_t hotkeyCaptureKey{0U};
    std::uint64_t hotkeyCaptureModifiers{0U};
    std::array<std::uint64_t, 4U> hotkeyErrors{};
    std::string hotkeyActionError{};

    [[nodiscard]] bool settingsCompatible() const noexcept
    {
        return productVersionStatus == HostProductVersionStatus::Match;
    }
};

struct HostStateParseResult final
{
    std::optional<HostState> state{};
    std::string error{};

    [[nodiscard]] bool succeeded() const noexcept
    {
        return state.has_value();
    }
};

// GetState is deliberately a small flat JSON object. Keeping its parser beside
// the Win32 client avoids taking a Windows Runtime dependency just for JSON.
[[nodiscard]] HostStateParseResult parseHostState(std::string_view json) noexcept;

}
