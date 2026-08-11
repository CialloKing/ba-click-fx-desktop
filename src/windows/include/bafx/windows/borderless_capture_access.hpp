#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace bafx::windows
{

enum class BorderlessCaptureAccessStatus : std::uint8_t
{
    NotPackaged,
    Allowed,
    DeniedBySystem,
    NotDeclaredByApp,
    DeniedByUser,
    UserPromptRequired,
    Unsupported,
    Failed
};

struct BorderlessCaptureAccessResult
{
    BorderlessCaptureAccessStatus status{
        BorderlessCaptureAccessStatus::NotPackaged};
    HRESULT error{S_OK};
};

[[nodiscard]] BorderlessCaptureAccessResult requestBorderlessCaptureAccess() noexcept;

[[nodiscard]] bool borderlessCaptureAccessAllowed(
    const BorderlessCaptureAccessResult& result) noexcept;

[[nodiscard]] std::string borderlessCaptureAccessDiagnostic(
    const BorderlessCaptureAccessResult& result);

}
