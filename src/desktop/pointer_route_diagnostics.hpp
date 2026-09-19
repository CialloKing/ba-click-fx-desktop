#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace bafx::desktop
{

enum class PointerRouteOutcome : std::size_t
{
    NoMove,
    EdgeFrame,
    Cancelled,
    HeldForwarded,
    FreeForwarded,
    AmbientDisabled,
    NoPosition,
    NoTarget,
    MappingFailed,
    HeldWithoutStroke,
    Discarded,
    Count
};

struct PointerRouteHealth final
{
    std::array<std::uint64_t, static_cast<std::size_t>(PointerRouteOutcome::Count)> outcomes{};
    std::uint64_t events{0U};
    std::uint64_t cursorFailures{0U};
    std::uint64_t cursorFallbacks{0U};
    std::uint64_t mappingFailures{0U};
    std::uint64_t invalidViewports{0U};
    std::uint64_t ownerResets{0U};
    std::uint32_t lastCursorError{0U};
    std::uint32_t lastMappingError{0U};
    PointerRouteOutcome lastOutcome{PointerRouteOutcome::NoMove};
    bool rawHeld{false};
    bool pressedSessionActive{false};
};

}
