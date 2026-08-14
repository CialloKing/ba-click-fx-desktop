#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace bafx::windows
{

inline constexpr std::uint32_t borderlessCaptureAccessTimeoutMilliseconds = 100U;

enum class BorderlessCaptureAccessStatus : std::uint8_t
{
    NotPackaged,
    Allowed,
    DeniedBySystem,
    NotDeclaredByApp,
    DeniedByUser,
    UserPromptRequired,
    TimedOut,
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
