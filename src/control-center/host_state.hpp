#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bafx::control_center
{

struct HostState final
{
    std::uint64_t generation{0U};
    bool paused{false};
    std::string backgroundCapture{};
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
