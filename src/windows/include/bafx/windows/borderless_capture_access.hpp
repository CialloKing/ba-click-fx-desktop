#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace bafx::windows
{

struct PackageIdentityInfo;

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

// Accept a caller-owned identity snapshot so diagnostic collectors can retain
// exactly the evidence used to decide whether a broker request is legal.
[[nodiscard]] BorderlessCaptureAccessResult requestBorderlessCaptureAccess(
    const PackageIdentityInfo& identity) noexcept;

[[nodiscard]] bool borderlessCaptureAccessAllowed(
    const BorderlessCaptureAccessResult& result) noexcept;

[[nodiscard]] std::string borderlessCaptureAccessDiagnostic(
    const BorderlessCaptureAccessResult& result);

}
