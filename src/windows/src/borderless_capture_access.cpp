#include "bafx/windows/borderless_capture_access.hpp"

#include "bafx/windows/package_identity.hpp"

#include <appmodel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <winrt/base.h>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <limits>
#include <memory>
#include <new>
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

[[nodiscard]] BorderlessCaptureAccessStatus failureStatus(
    const HRESULT error) noexcept
{
    if (error == E_NOTIMPL
        || error == E_NOINTERFACE
        || error == REGDB_E_CLASSNOTREG)
    {
        return BorderlessCaptureAccessStatus::Unsupported;
    }
    return BorderlessCaptureAccessStatus::Failed;
}

[[nodiscard]] BorderlessCaptureAccessResult failureResult(
    const HRESULT error) noexcept
{
    return BorderlessCaptureAccessResult{failureStatus(error), error};
}

[[nodiscard]] BorderlessCaptureAccessResult identityResult(
    const PackageIdentityInfo& identity) noexcept
{
    if (identity.present)
    {
        return BorderlessCaptureAccessResult{
            BorderlessCaptureAccessStatus::Failed,
            E_PENDING};
    }
    if (identity.fullNameError
        == static_cast<DWORD>(APPMODEL_ERROR_NO_PACKAGE))
    {
        return BorderlessCaptureAccessResult{
            BorderlessCaptureAccessStatus::NotPackaged,
            HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE)};
    }

    // Do not disguise an OOM or AppModel probe failure as the expected
    // portable-build result.
    const DWORD identityError = identity.fullNameError != ERROR_SUCCESS
        ? identity.fullNameError
        : (identity.packagePathError != ERROR_SUCCESS
            ? identity.packagePathError
            : ERROR_INVALID_DATA);
    return failureResult(HRESULT_FROM_WIN32(identityError));
}

[[nodiscard]] BorderlessCaptureAccessAsyncStatus mapAsyncStatus(
    const AsyncStatus status) noexcept
{
    switch (status)
    {
    case AsyncStatus::Started:
        return BorderlessCaptureAccessAsyncStatus::Started;
    case AsyncStatus::Completed:
        return BorderlessCaptureAccessAsyncStatus::Completed;
    case AsyncStatus::Canceled:
        return BorderlessCaptureAccessAsyncStatus::Canceled;
    case AsyncStatus::Error:
        return BorderlessCaptureAccessAsyncStatus::Error;
    }
    return BorderlessCaptureAccessAsyncStatus::Error;
}

class WinrtBorderlessCaptureAccessOperation final
    : public BorderlessCaptureAccessOperation
{
public:
    explicit WinrtBorderlessCaptureAccessOperation(
        winrt::Windows::Foundation::IAsyncOperation<AppCapabilityAccessStatus>
            operation) noexcept
        : operation_(std::move(operation))
    {
    }

    [[nodiscard]] BorderlessCaptureAccessAsyncStatus status()
        const noexcept override
    {
        try
        {
            return mapAsyncStatus(operation_.Status());
        }
        catch (const winrt::hresult_error& error)
        {
            observationError_ = error.code();
        }
        catch (...)
        {
            observationError_ = E_FAIL;
        }
        return BorderlessCaptureAccessAsyncStatus::Error;
    }

    [[nodiscard]] BorderlessCaptureAccessResult getResults() noexcept override
    {
        try
        {
            return BorderlessCaptureAccessResult{
                mapStatus(operation_.GetResults()),
                S_OK};
        }
        catch (const winrt::hresult_error& error)
        {
            return failureResult(error.code());
        }
        catch (...)
        {
            return failureResult(E_FAIL);
        }
    }

    [[nodiscard]] HRESULT error() const noexcept override
    {
        if (FAILED(observationError_))
        {
            return observationError_;
        }
        try
        {
            return operation_.ErrorCode();
        }
        catch (const winrt::hresult_error& error)
        {
            return error.code();
        }
        catch (...)
        {
            return E_FAIL;
        }
    }

    void cancel() noexcept override
    {
        try
        {
            operation_.Cancel();
        }
        catch (const winrt::hresult_error& error)
        {
            observationError_ = error.code();
        }
        catch (...)
        {
            observationError_ = E_FAIL;
        }
    }

private:
    winrt::Windows::Foundation::IAsyncOperation<AppCapabilityAccessStatus>
        operation_;
    mutable HRESULT observationError_{S_OK};
};

[[nodiscard]] std::uint32_t elapsedMilliseconds(
    const BorderlessCaptureAccessRequest::Clock::time_point startedAt,
    const BorderlessCaptureAccessRequest::Clock::time_point now) noexcept
{
    if (now <= startedAt)
    {
        return 0U;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - startedAt).count();
    return static_cast<std::uint32_t>((std::min)(
        static_cast<std::uint64_t>(elapsed),
        static_cast<std::uint64_t>(
            (std::numeric_limits<std::uint32_t>::max)())));
}

[[nodiscard]] BorderlessCaptureAccessResult terminalResult(
    BorderlessCaptureAccessOperation& operation,
    const BorderlessCaptureAccessAsyncStatus asyncStatus,
    const std::uint32_t elapsed,
    const bool cancelRequested) noexcept
{
    BorderlessCaptureAccessResult result{};
    switch (asyncStatus)
    {
    case BorderlessCaptureAccessAsyncStatus::Completed:
        result = operation.getResults();
        break;
    case BorderlessCaptureAccessAsyncStatus::Canceled:
    {
        const HRESULT error = operation.error();
        result = BorderlessCaptureAccessResult{
            BorderlessCaptureAccessStatus::Canceled,
            FAILED(error) ? error : HRESULT_FROM_WIN32(ERROR_CANCELLED)};
        break;
    }
    case BorderlessCaptureAccessAsyncStatus::Error:
        result = failureResult(operation.error());
        break;
    case BorderlessCaptureAccessAsyncStatus::NotStarted:
        result = failureResult(E_UNEXPECTED);
        break;
    case BorderlessCaptureAccessAsyncStatus::Started:
        result = BorderlessCaptureAccessResult{
            BorderlessCaptureAccessStatus::TimedOut,
            HRESULT_FROM_WIN32(ERROR_TIMEOUT)};
        break;
    }
    result.asyncStatus = asyncStatus;
    result.elapsedMilliseconds = elapsed;
    result.cancelRequested = cancelRequested;
    return result;
}

[[nodiscard]] std::string hexHresult(const HRESULT error)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << static_cast<unsigned long>(error);
    return stream.str();
}

}

BorderlessCaptureAccessRequest::BorderlessCaptureAccessRequest(
    const std::chrono::milliseconds timeout) noexcept
    : timeout_(timeout > std::chrono::milliseconds::zero()
        ? timeout
        : std::chrono::milliseconds(1))
{
}

BorderlessCaptureAccessRequest::~BorderlessCaptureAccessRequest() noexcept
{
    // Releasing the last WinRT handle does not express owner intent. Request
    // cancellation explicitly so shutdown cannot leave a broker prompt alive.
    cancel();
}

void BorderlessCaptureAccessRequest::begin(
    const PackageIdentityInfo& identity,
    const Clock::time_point now) noexcept
{
    cancel(now);
    readyResult_.reset();
    startedAt_ = now;
    cancelRequested_ = false;
    if (!identity.present)
    {
        readyResult_ = identityResult(identity);
        return;
    }

    try
    {
        begin(
            std::make_unique<WinrtBorderlessCaptureAccessOperation>(
                GraphicsCaptureAccess::RequestAccessAsync(
                    GraphicsCaptureAccessKind::Borderless)),
            now);
    }
    catch (const winrt::hresult_error& error)
    {
        readyResult_ = failureResult(error.code());
    }
    catch (const std::bad_alloc&)
    {
        readyResult_ = failureResult(E_OUTOFMEMORY);
    }
    catch (...)
    {
        readyResult_ = failureResult(E_FAIL);
    }
}

void BorderlessCaptureAccessRequest::begin(
    std::unique_ptr<BorderlessCaptureAccessOperation> operation,
    const Clock::time_point now) noexcept
{
    cancel(now);
    readyResult_.reset();
    startedAt_ = now;
    cancelRequested_ = false;
    if (operation == nullptr)
    {
        readyResult_ = failureResult(E_INVALIDARG);
        return;
    }
    operation_ = std::move(operation);
}

BorderlessCaptureAccessPollResult BorderlessCaptureAccessRequest::poll(
    const Clock::time_point now) noexcept
{
    if (readyResult_.has_value())
    {
        BorderlessCaptureAccessPollResult poll{};
        poll.result = readyResult_;
        readyResult_.reset();
        return poll;
    }
    if (operation_ == nullptr)
    {
        return {};
    }

    const std::uint32_t elapsed = elapsedMilliseconds(startedAt_, now);
    BorderlessCaptureAccessAsyncStatus asyncStatus = operation_->status();
    if (asyncStatus == BorderlessCaptureAccessAsyncStatus::Started
        && now - startedAt_ < timeout_)
    {
        return BorderlessCaptureAccessPollResult{true, std::nullopt};
    }
    if (asyncStatus == BorderlessCaptureAccessAsyncStatus::Started)
    {
        // Completion wins at the deadline. Re-read before cancellation so an
        // Allowed result cannot be mislabeled as a timeout at the boundary.
        asyncStatus = operation_->status();
        if (asyncStatus == BorderlessCaptureAccessAsyncStatus::Started)
        {
            operation_->cancel();
            cancelRequested_ = true;
        }
    }

    BorderlessCaptureAccessPollResult poll{};
    poll.result = terminalResult(
        *operation_,
        asyncStatus,
        elapsed,
        cancelRequested_);
    operation_.reset();
    return poll;
}

void BorderlessCaptureAccessRequest::cancel(
    const Clock::time_point now) noexcept
{
    if (operation_ == nullptr)
    {
        return;
    }
    const std::uint32_t elapsed = elapsedMilliseconds(startedAt_, now);
    BorderlessCaptureAccessAsyncStatus asyncStatus = operation_->status();
    if (asyncStatus == BorderlessCaptureAccessAsyncStatus::Started)
    {
        operation_->cancel();
        cancelRequested_ = true;
        // Cancel is advisory. Observe a completion or broker error that won
        // the race instead of claiming that the broker confirmed Canceled.
        asyncStatus = operation_->status();
    }
    if (asyncStatus == BorderlessCaptureAccessAsyncStatus::Started)
    {
        // ErrorCode has no terminal meaning while IAsyncInfo is still Started.
        // Record the owner's cancellation boundary without inventing a broker
        // decision that has not arrived.
        readyResult_ = BorderlessCaptureAccessResult{
            BorderlessCaptureAccessStatus::Canceled,
            HRESULT_FROM_WIN32(ERROR_CANCELLED),
            asyncStatus,
            elapsed,
            cancelRequested_};
        operation_.reset();
        return;
    }
    readyResult_ = terminalResult(
        *operation_,
        asyncStatus,
        elapsed,
        cancelRequested_);
    operation_.reset();
}

bool BorderlessCaptureAccessRequest::active() const noexcept
{
    return operation_ != nullptr || readyResult_.has_value();
}

bool BorderlessCaptureAccessRequest::pending() const noexcept
{
    return operation_ != nullptr;
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
    case BorderlessCaptureAccessStatus::Canceled:
        return "canceled";
    case BorderlessCaptureAccessStatus::Unsupported:
        return "unsupported";
    case BorderlessCaptureAccessStatus::Failed:
        return "failed";
    }
    return "unknown";
}

std::string_view borderlessCaptureAccessAsyncStatusName(
    const BorderlessCaptureAccessAsyncStatus status) noexcept
{
    switch (status)
    {
    case BorderlessCaptureAccessAsyncStatus::NotStarted:
        return "not-started";
    case BorderlessCaptureAccessAsyncStatus::Started:
        return "started";
    case BorderlessCaptureAccessAsyncStatus::Completed:
        return "completed";
    case BorderlessCaptureAccessAsyncStatus::Canceled:
        return "canceled";
    case BorderlessCaptureAccessAsyncStatus::Error:
        return "error";
    }
    return "unknown";
}

std::string borderlessCaptureAccessDiagnostic(
    const BorderlessCaptureAccessResult& result)
{
    std::ostringstream stream;
    stream << "WGC.BorderlessAccess="
           << borderlessCaptureAccessStatusName(result.status)
           << ";HRESULT=" << hexHresult(result.error)
           << ";AsyncStatus="
           << borderlessCaptureAccessAsyncStatusName(result.asyncStatus)
           << ";ElapsedMs=" << result.elapsedMilliseconds
           << ";CancelRequested="
           << (result.cancelRequested ? "true" : "false");
    return stream.str();
}

}
