#pragma once

#include "bafx/windows/overlay_window.hpp"

#include <cstdint>
#include <filesystem>

namespace bafx::desktop
{

class InputHealthDiagnostics final
{
public:
    void service(const std::filesystem::path& logPath,
        const bafx::windows::PointerHealthSnapshot& snapshot,
        std::uint64_t nowTickMs, bool final = false) noexcept;

private:
    bafx::windows::PointerHealthSnapshot previous_{};
    std::uint64_t lastReportTickMs_{0U};
    bool reported_{false};
};

}
