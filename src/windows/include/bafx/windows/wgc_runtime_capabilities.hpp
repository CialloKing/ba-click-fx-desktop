#pragma once

#include <windows.h>

#include <cstdint>
#include <string_view>

namespace bafx::windows
{

inline constexpr std::uint16_t borderlessCaptureUniversalApiContractVersion =
    12U;

enum class BorderlessCaptureCapabilityStatus : std::uint8_t
{
    Supported,
    UniversalApiContractMissing,
    GraphicsCaptureAccessTypeMissing,
    RequestAccessAsyncMethodMissing,
    BorderlessAccessKindMissing,
    IsBorderRequiredPropertyMissing,
    ProbeFailed
};

struct BorderlessCaptureCapabilityResult
{
    BorderlessCaptureCapabilityStatus status{
        BorderlessCaptureCapabilityStatus::ProbeFailed};
    HRESULT error{E_UNEXPECTED};
    bool universalApiContractV12{false};
    bool graphicsCaptureAccessType{false};
    bool requestAccessAsyncMethod{false};
    bool borderlessAccessKind{false};
    bool isBorderRequiredProperty{false};
};

[[nodiscard]] BorderlessCaptureCapabilityResult
queryBorderlessCaptureCapability() noexcept;

[[nodiscard]] bool borderlessCaptureCapabilitySupported(
    const BorderlessCaptureCapabilityResult& result) noexcept;

[[nodiscard]] std::string_view borderlessCaptureCapabilityStatusName(
    BorderlessCaptureCapabilityStatus status) noexcept;

}
