#include "bafx/windows/wgc_runtime_capabilities.hpp"

#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/base.h>

#include <new>

namespace bafx::windows
{
namespace
{

using winrt::Windows::Foundation::Metadata::ApiInformation;

constexpr wchar_t universalApiContractName[] =
    L"Windows.Foundation.UniversalApiContract";
constexpr wchar_t graphicsCaptureAccessTypeName[] =
    L"Windows.Graphics.Capture.GraphicsCaptureAccess";
constexpr wchar_t graphicsCaptureAccessKindTypeName[] =
    L"Windows.Graphics.Capture.GraphicsCaptureAccessKind";
constexpr wchar_t graphicsCaptureSessionTypeName[] =
    L"Windows.Graphics.Capture.GraphicsCaptureSession";

[[nodiscard]] BorderlessCaptureCapabilityStatus missingCapability(
    const BorderlessCaptureCapabilityResult& result) noexcept
{
    if (!result.universalApiContractV12)
    {
        return BorderlessCaptureCapabilityStatus::UniversalApiContractMissing;
    }
    if (!result.graphicsCaptureAccessType)
    {
        return BorderlessCaptureCapabilityStatus::
            GraphicsCaptureAccessTypeMissing;
    }
    if (!result.requestAccessAsyncMethod)
    {
        return BorderlessCaptureCapabilityStatus::
            RequestAccessAsyncMethodMissing;
    }
    if (!result.borderlessAccessKind)
    {
        return BorderlessCaptureCapabilityStatus::BorderlessAccessKindMissing;
    }
    if (!result.isBorderRequiredProperty)
    {
        return BorderlessCaptureCapabilityStatus::
            IsBorderRequiredPropertyMissing;
    }
    return BorderlessCaptureCapabilityStatus::Supported;
}

}

BorderlessCaptureCapabilityResult queryBorderlessCaptureCapability() noexcept
{
    BorderlessCaptureCapabilityResult result{};
    try
    {
        // String-based metadata queries keep the complete Windows 11 code in
        // every build while making availability a property of the target OS.
        result.universalApiContractV12 = ApiInformation::IsApiContractPresent(
            universalApiContractName,
            borderlessCaptureUniversalApiContractVersion);
        result.graphicsCaptureAccessType = ApiInformation::IsTypePresent(
            graphicsCaptureAccessTypeName);
        result.requestAccessAsyncMethod = ApiInformation::IsMethodPresent(
            graphicsCaptureAccessTypeName,
            L"RequestAccessAsync");
        result.borderlessAccessKind = ApiInformation::IsEnumNamedValuePresent(
            graphicsCaptureAccessKindTypeName,
            L"Borderless");
        result.isBorderRequiredProperty =
            ApiInformation::IsWriteablePropertyPresent(
                graphicsCaptureSessionTypeName,
                L"IsBorderRequired");
        result.status = missingCapability(result);
        result.error =
            result.status == BorderlessCaptureCapabilityStatus::Supported
                ? S_OK
                : E_NOTIMPL;
    }
    catch (const winrt::hresult_error& error)
    {
        result.status = BorderlessCaptureCapabilityStatus::ProbeFailed;
        result.error = error.code();
    }
    catch (const std::bad_alloc&)
    {
        result.status = BorderlessCaptureCapabilityStatus::ProbeFailed;
        result.error = E_OUTOFMEMORY;
    }
    catch (...)
    {
        result.status = BorderlessCaptureCapabilityStatus::ProbeFailed;
        result.error = E_FAIL;
    }
    return result;
}

bool borderlessCaptureCapabilitySupported(
    const BorderlessCaptureCapabilityResult& result) noexcept
{
    return result.status == BorderlessCaptureCapabilityStatus::Supported;
}

std::string_view borderlessCaptureCapabilityStatusName(
    const BorderlessCaptureCapabilityStatus status) noexcept
{
    switch (status)
    {
    case BorderlessCaptureCapabilityStatus::Supported:
        return "supported";
    case BorderlessCaptureCapabilityStatus::UniversalApiContractMissing:
        return "universal-api-contract-v12-missing";
    case BorderlessCaptureCapabilityStatus::GraphicsCaptureAccessTypeMissing:
        return "graphics-capture-access-type-missing";
    case BorderlessCaptureCapabilityStatus::RequestAccessAsyncMethodMissing:
        return "request-access-async-method-missing";
    case BorderlessCaptureCapabilityStatus::BorderlessAccessKindMissing:
        return "borderless-access-kind-missing";
    case BorderlessCaptureCapabilityStatus::IsBorderRequiredPropertyMissing:
        return "is-border-required-property-missing";
    case BorderlessCaptureCapabilityStatus::ProbeFailed:
        return "probe-failed";
    }
    return "unknown";
}

}
