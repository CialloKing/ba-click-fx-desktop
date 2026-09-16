#pragma once

#include "bafx/config/config.hpp"

#include <windows.h>

#include <optional>
#include <string>

namespace bafx::control_center
{

// Global and per-display controls share one interpretation of the selection.
[[nodiscard]] std::optional<bafx::config::FramePacing> selectedFramePacing(
    HWND comboBox) noexcept;
[[nodiscard]] std::wstring hresultText(HRESULT result);

}
