#include "bafx/windows/borderless_capture_access.hpp"

#include "bafx/windows/package_identity.hpp"

#include <appmodel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <winrt/base.h>

#include <chrono>
#include <iomanip>
#include <sstream>

namespace bafx::windows
{
namespace
{

using winrt::Windows::Graphics::Capture::GraphicsCaptureAccess;
using winrt::Windows::Graphics::Capture::GraphicsCaptureAccessKind;
using winrt::Windows::Foundation::AsyncStatus;
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

[[nodiscard]] std::string hexHresult(const HRESULT error)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << static_cast<unsigned long>(error);
    return stream.str();
}

}

BorderlessCaptureAccessResult requestBorderlessCaptureAccess(
    const PackageIdentityInfo& identity) noexcept
{
    try
    {
        if (!identity.present)
        {
            if (identity.fullNameError
                != static_cast<DWORD>(APPMODEL_ERROR_NO_PACKAGE))
            {
                // Do not disguise an OOM or AppModel probe failure as the
                // expected portable-build result.
                const DWORD identityError = identity.fullNameError
                    != ERROR_SUCCESS
                    ? identity.fullNameError
                    : (identity.packagePathError != ERROR_SUCCESS
                        ? identity.packagePathError
                        : ERROR_INVALID_DATA);
                return BorderlessCaptureAccessResult{
                    BorderlessCaptureAccessStatus::Failed,
                    HRESULT_FROM_WIN32(identityError)};
            }
            return BorderlessCaptureAccessResult{
                BorderlessCaptureAccessStatus::NotPackaged,
                HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE)};
        }

        const auto operation = GraphicsCaptureAccess::RequestAccessAsync(
            GraphicsCaptureAccessKind::Borderless);
        const AsyncStatus asyncStatus = operation.wait_for(
            std::chrono::milliseconds(
                borderlessCaptureAccessTimeoutMilliseconds));
        if (asyncStatus == AsyncStatus::Started)
        {
            // Permission is optional. Cancel at a fixed boundary instead of
            // allowing .get() to stall the render/input owner indefinitely.
            try
            {
                operation.Cancel();
            }
            catch (...)
            {
                // Timeout remains the actionable result even if cancellation
                // races completion or the broker has already disconnected.
            }
            return BorderlessCaptureAccessResult{
                BorderlessCaptureAccessStatus::TimedOut,
                HRESULT_FROM_WIN32(ERROR_TIMEOUT)};
        }
        return BorderlessCaptureAccessResult{
            mapStatus(operation.GetResults()),
            S_OK};
    }
    catch (const winrt::hresult_error& error)
    {
        const HRESULT code = error.code();
        if (code == E_NOTIMPL
            || code == E_NOINTERFACE
            || code == REGDB_E_CLASSNOTREG)
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

BorderlessCaptureAccessResult requestBorderlessCaptureAccess() noexcept
{
    return requestBorderlessCaptureAccess(queryCurrentPackageIdentity());
}

bool borderlessCaptureAccessAllowed(
    const BorderlessCaptureAccessResult& result) noexcept
{
    return result.status == BorderlessCaptureAccessStatus::Allowed;
}

std::string_view borderlessCaptureAccessStatusName(
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
    case BorderlessCaptureAccessStatus::TimedOut:
        return "timed-out";
    case BorderlessCaptureAccessStatus::Unsupported:
        return "unsupported";
    case BorderlessCaptureAccessStatus::Failed:
        return "failed";
    }
    return "unknown";
}

std::string borderlessCaptureAccessDiagnostic(
    const BorderlessCaptureAccessResult& result)
{
    std::ostringstream stream;
    stream << "WGC.BorderlessAccess="
           << borderlessCaptureAccessStatusName(result.status)
           << ";HRESULT=" << hexHresult(result.error);
    return stream.str();
}

}
