#include "bafx/windows/borderless_capture_access.hpp"

#include "bafx/windows/package_identity.hpp"

#include <appmodel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <winrt/base.h>

#include <iomanip>
#include <sstream>

namespace bafx::windows
{
namespace
{

using winrt::Windows::Graphics::Capture::GraphicsCaptureAccess;
using winrt::Windows::Graphics::Capture::GraphicsCaptureAccessKind;
using winrt::Windows::Security::Authorization::AppCapabilityAccess::
    AppCapabilityAccessStatus;

[[nodiscard]] BorderlessCaptureAccessStatus mapStatus(
    const AppCapabilityAccessStatus status) noexcept
{
    switch (status)
    {
    case AppCapabilityAccessStatus::Allowed:
        return BorderlessCaptureAccessStatus::Allowed;
    case AppCapabilityAccessStatus::DeniedBySystem:
        return BorderlessCaptureAccessStatus::DeniedBySystem;
    case AppCapabilityAccessStatus::NotDeclaredByApp:
        return BorderlessCaptureAccessStatus::NotDeclaredByApp;
    case AppCapabilityAccessStatus::DeniedByUser:
        return BorderlessCaptureAccessStatus::DeniedByUser;
    case AppCapabilityAccessStatus::UserPromptRequired:
        return BorderlessCaptureAccessStatus::UserPromptRequired;
    }
    return BorderlessCaptureAccessStatus::Failed;
}

[[nodiscard]] std::string statusName(
    const BorderlessCaptureAccessStatus status) noexcept
{
    switch (status)
    {
    case BorderlessCaptureAccessStatus::NotPackaged:
        return "not-packaged";
    case BorderlessCaptureAccessStatus::Allowed:
        return "allowed";
    case BorderlessCaptureAccessStatus::DeniedBySystem:
        return "denied-by-system";
    case BorderlessCaptureAccessStatus::NotDeclaredByApp:
        return "not-declared";
    case BorderlessCaptureAccessStatus::DeniedByUser:
        return "denied-by-user";
    case BorderlessCaptureAccessStatus::UserPromptRequired:
        return "user-prompt-required";
    case BorderlessCaptureAccessStatus::Unsupported:
        return "unsupported";
    case BorderlessCaptureAccessStatus::Failed:
        return "failed";
    }
    return "unknown";
}

[[nodiscard]] std::string hexHresult(const HRESULT error)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << static_cast<unsigned long>(error);
    return stream.str();
}

}

BorderlessCaptureAccessResult requestBorderlessCaptureAccess() noexcept
{
    try
    {
        if (!queryCurrentPackageIdentity().present)
        {
            return BorderlessCaptureAccessResult{
                BorderlessCaptureAccessStatus::NotPackaged,
                HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE)};
        }

        const auto status = GraphicsCaptureAccess::RequestAccessAsync(
            GraphicsCaptureAccessKind::Borderless).get();
        return BorderlessCaptureAccessResult{mapStatus(status), S_OK};
    }
    catch (const winrt::hresult_error& error)
    {
        const HRESULT code = error.code();
        if (code == E_NOTIMPL || code == E_NOINTERFACE)
        {
            return BorderlessCaptureAccessResult{
                BorderlessCaptureAccessStatus::Unsupported,
                code};
        }
        return BorderlessCaptureAccessResult{
            BorderlessCaptureAccessStatus::Failed,
            code};
    }
    catch (...)
    {
        return BorderlessCaptureAccessResult{
            BorderlessCaptureAccessStatus::Failed,
            E_FAIL};
    }
}

bool borderlessCaptureAccessAllowed(
    const BorderlessCaptureAccessResult& result) noexcept
{
    return result.status == BorderlessCaptureAccessStatus::Allowed;
}

std::string borderlessCaptureAccessDiagnostic(
    const BorderlessCaptureAccessResult& result)
{
    std::ostringstream stream;
    stream << "WGC.BorderlessAccess=" << statusName(result.status)
           << ";HRESULT=" << hexHresult(result.error);
    return stream.str();
}

}
