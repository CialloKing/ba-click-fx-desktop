#include "bafx/windows/borderless_capture_access.hpp"

#include "bafx/windows/error.hpp"
#include "bafx/windows/package_identity.hpp"
#include "bafx/windows/unique_handle.hpp"

#include <appmodel.h>
#include <roapi.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <limits>
#include <memory>
#include <new>
#include <sstream>
#include <utility>

namespace bafx::windows
{
namespace
{

using Microsoft::WRL::ComPtr;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Security::Authorization::AppCapabilityAccess::
    AppCapability;
using winrt::Windows::Security::Authorization::AppCapabilityAccess::
    AppCapabilityAccessStatus;
using BorderlessAccessAsyncOperation =
    winrt::Windows::Foundation::IAsyncOperation<AppCapabilityAccessStatus>;

constexpr wchar_t borderlessCapabilityName[] =
    L"graphicsCaptureWithoutBorder";
constexpr wchar_t graphicsCaptureAccessClassName[] =
    L"Windows.Graphics.Capture.GraphicsCaptureAccess";
constexpr std::int32_t borderlessGraphicsCaptureAccessKind = 0;

// GraphicsCaptureAccess is a Windows 11 contract. Keep the one-method ABI
// local so an older SDK still compiles the complete permission path.
MIDL_INTERFACE("743ED370-06EC-5040-A58A-901F0F757095")
GraphicsCaptureAccessStaticsAbi : public IInspectable
{
public:
    virtual HRESULT STDMETHODCALLTYPE RequestAccessAsync(
        std::int32_t request,
        IInspectable** operation) = 0;
};

[[nodiscard]] BorderlessAccessAsyncOperation
requestBorderlessCaptureAccessAsync()
{
    const winrt::hstring className{graphicsCaptureAccessClassName};
    ComPtr<GraphicsCaptureAccessStaticsAbi> factory;
    winrt::check_hresult(RoGetActivationFactory(
        winrt::get_abi(className),
        IID_PPV_ARGS(&factory)));

    ComPtr<IInspectable> operation;
    winrt::check_hresult(factory->RequestAccessAsync(
        borderlessGraphicsCaptureAccessKind,
        &operation));
    if (operation == nullptr)
    {
        winrt::throw_hresult(E_UNEXPECTED);
    }
    return BorderlessAccessAsyncOperation{
        operation.Detach(),
        winrt::take_ownership_from_abi};
}

class BorderlessAccessNotification final
{
public:
    BorderlessAccessNotification()
        : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {
        if (event_.get() == nullptr)
        {
            throwLastError("CreateEventW(borderless access changed)");
        }
    }

    void notify() noexcept
    {
        if (stopping_.load(std::memory_order_acquire))
        {
            return;
        }
        generation_.fetch_add(1U, std::memory_order_release);
        SetEvent(event_.get());
    }

    void beginStop() noexcept
    {
        stopping_.store(true, std::memory_order_release);
        SetEvent(event_.get());
    }

    [[nodiscard]] DWORD resetAfterObserve(
        const std::uint64_t observedGeneration) noexcept
    {
        if (!ResetEvent(event_.get()))
        {
            const DWORD error = GetLastError();
            return error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
        }
        if (stopping_.load(std::memory_order_acquire)
            || generation() != observedGeneration)
        {
            if (!SetEvent(event_.get()))
            {
                const DWORD error = GetLastError();
                return error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
            }
        }
        return ERROR_SUCCESS;
    }

    [[nodiscard]] HANDLE eventObject() const noexcept
    {
        return event_.get();
    }

    [[nodiscard]] std::uint64_t generation() const noexcept
    {
        return generation_.load(std::memory_order_acquire);
    }

private:
    UniqueHandle event_{};
    std::atomic<std::uint64_t> generation_{0U};
    std::atomic_bool stopping_{false};
};

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

[[nodiscard]] BorderlessCaptureAccessResult capabilityFailureResult(
    const BorderlessCaptureCapabilityResult& capability) noexcept
{
    const BorderlessCaptureAccessStatus status =
        capability.status == BorderlessCaptureCapabilityStatus::ProbeFailed
            ? failureStatus(capability.error)
            : BorderlessCaptureAccessStatus::Unsupported;
    BorderlessCaptureAccessResult result{
        status,
        capability.status == BorderlessCaptureCapabilityStatus::ProbeFailed
            ? capability.error
            : E_NOTIMPL};
    result.capability = capability;
    return result;
}

[[nodiscard]] BorderlessCaptureAccessResult trustFailureResult(
    const ExternalHostTrustResult& trust) noexcept
{
    BorderlessCaptureAccessResult result{
        BorderlessCaptureAccessStatus::IdentityUntrusted,
        FAILED(trust.error) ? trust.error : E_ACCESSDENIED};
    result.externalHostTrust = trust;
    return result;
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
        BorderlessAccessAsyncOperation operation) noexcept
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
    BorderlessAccessAsyncOperation operation_;
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

struct BorderlessCaptureAccessMonitor::Implementation
{
    ~Implementation() noexcept
    {
        stop();
    }

    void stop() noexcept
    {
        if (notification != nullptr)
        {
            // Block late callbacks before removing the handler so an event
            // cannot revive a monitor whose owner is already unwinding.
            notification->beginStop();
        }
        if (accessChangedRegistered)
        {
            try
            {
                capability.AccessChanged(accessChangedToken);
            }
            catch (...)
            {
                // Teardown is fail-closed: no caller can safely recover from
                // a WinRT event removal failure during stack unwinding.
            }
            accessChangedRegistered = false;
        }
        capability = nullptr;
        notification.reset();
    }

    AppCapability capability{nullptr};
    winrt::event_token accessChangedToken{};
    std::shared_ptr<BorderlessAccessNotification> notification{};
    std::uint64_t observedGeneration{0U};
    bool accessChangedRegistered{false};
};

BorderlessCaptureAccessMonitor::BorderlessCaptureAccessMonitor() noexcept =
    default;

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

BorderlessCaptureAccessMonitor::~BorderlessCaptureAccessMonitor() noexcept
{
    stop();
}

BorderlessCaptureAccessHealthResult
BorderlessCaptureAccessMonitor::start() noexcept
{
    stop();
    try
    {
        auto implementation = std::make_unique<Implementation>();
        implementation->notification =
            std::make_shared<BorderlessAccessNotification>();
        implementation->capability = AppCapability::Create(
            borderlessCapabilityName);
        const std::shared_ptr<BorderlessAccessNotification> notification =
            implementation->notification;
        implementation->accessChangedToken =
            implementation->capability.AccessChanged(
                [notification](const auto&, const auto&) noexcept
                {
                    notification->notify();
                });
        implementation->accessChangedRegistered = true;

        const BorderlessCaptureAccessStatus status = mapStatus(
            implementation->capability.CheckAccess());
        const std::uint64_t generation =
            implementation->notification->generation();
        implementation->observedGeneration = generation;
        implementation_ = std::move(implementation);
        return BorderlessCaptureAccessHealthResult{
            status,
            status == BorderlessCaptureAccessStatus::Allowed
                ? S_OK
                : E_ACCESSDENIED,
            generation};
    }
    catch (const winrt::hresult_error& error)
    {
        return BorderlessCaptureAccessHealthResult{
            failureStatus(error.code()),
            error.code(),
            0U};
    }
    catch (const HResultError& error)
    {
        return BorderlessCaptureAccessHealthResult{
            failureStatus(error.result()),
            error.result(),
            0U};
    }
    catch (const std::bad_alloc&)
    {
        return BorderlessCaptureAccessHealthResult{
            BorderlessCaptureAccessStatus::Failed,
            E_OUTOFMEMORY,
            0U};
    }
    catch (...)
    {
        return BorderlessCaptureAccessHealthResult{
            BorderlessCaptureAccessStatus::Failed,
            E_FAIL,
            0U};
    }
}

BorderlessCaptureAccessHealthResult
BorderlessCaptureAccessMonitor::observe() noexcept
{
    if (implementation_ == nullptr)
    {
        return BorderlessCaptureAccessHealthResult{
            BorderlessCaptureAccessStatus::Failed,
            E_HANDLE,
            0U};
    }
    const std::uint64_t generation =
        implementation_->notification->generation();
    const auto completeObservation =
        [&](const BorderlessCaptureAccessStatus status,
            const HRESULT result) noexcept
    {
        implementation_->observedGeneration = generation;
        const DWORD notificationError =
            implementation_->notification->resetAfterObserve(generation);
        if (notificationError != ERROR_SUCCESS)
        {
            // A manual-reset event that cannot be reset would wake every wait
            // iteration forever. Retire the monitor and its handle immediately.
            stop();
            return BorderlessCaptureAccessHealthResult{
                BorderlessCaptureAccessStatus::Failed,
                HRESULT_FROM_WIN32(notificationError),
                generation};
        }
        return BorderlessCaptureAccessHealthResult{
            status,
            result,
            generation};
    };
    try
    {
        const BorderlessCaptureAccessStatus status = mapStatus(
            implementation_->capability.CheckAccess());
        return completeObservation(
            status,
            status == BorderlessCaptureAccessStatus::Allowed
                ? S_OK
                : E_ACCESSDENIED);
    }
    catch (const winrt::hresult_error& error)
    {
        return completeObservation(
            failureStatus(error.code()),
            error.code());
    }
    catch (const HResultError& error)
    {
        return completeObservation(
            failureStatus(error.result()),
            error.result());
    }
    catch (...)
    {
        return completeObservation(
            BorderlessCaptureAccessStatus::Failed,
            E_FAIL);
    }
}

bool BorderlessCaptureAccessMonitor::notificationPending() const noexcept
{
    if (implementation_ == nullptr)
    {
        return false;
    }
    return implementation_->notification->generation()
        != implementation_->observedGeneration;
}

HANDLE BorderlessCaptureAccessMonitor::changeEvent() const noexcept
{
    return implementation_ != nullptr
        ? implementation_->notification->eventObject()
        : nullptr;
}

bool BorderlessCaptureAccessMonitor::active() const noexcept
{
    return implementation_ != nullptr;
}

void BorderlessCaptureAccessMonitor::stop() noexcept
{
    if (implementation_ == nullptr)
    {
        return;
    }
    implementation_->stop();
    implementation_.reset();
}

void BorderlessCaptureAccessRequest::begin(
    const PackageIdentityInfo& identity,
    const Clock::time_point now) noexcept
{
    cancel(now);
    readyResult_.reset();
    capability_.reset();
    externalHostTrust_.reset();
    startedAt_ = now;
    cancelRequested_ = false;
    if (!identity.present)
    {
        readyResult_ = identityResult(identity);
        return;
    }

    capability_ = queryBorderlessCaptureCapability();
    if (!borderlessCaptureCapabilitySupported(*capability_))
    {
        readyResult_ = capabilityFailureResult(*capability_);
        return;
    }

    externalHostTrust_ = queryExternalHostTrust(identity);
    if (!externalHostTrusted(*externalHostTrust_))
    {
        readyResult_ = trustFailureResult(*externalHostTrust_);
        readyResult_->capability = capability_;
        return;
    }

    try
    {
        operation_ = std::make_unique<WinrtBorderlessCaptureAccessOperation>(
            requestBorderlessCaptureAccessAsync());
    }
    catch (const winrt::hresult_error& error)
    {
        readyResult_ = failureResult(error.code());
        readyResult_->capability = capability_;
        readyResult_->externalHostTrust = externalHostTrust_;
    }
    catch (const std::bad_alloc&)
    {
        readyResult_ = failureResult(E_OUTOFMEMORY);
        readyResult_->capability = capability_;
        readyResult_->externalHostTrust = externalHostTrust_;
    }
    catch (...)
    {
        readyResult_ = failureResult(E_FAIL);
        readyResult_->capability = capability_;
        readyResult_->externalHostTrust = externalHostTrust_;
    }
}

void BorderlessCaptureAccessRequest::begin(
    std::unique_ptr<BorderlessCaptureAccessOperation> operation,
    const Clock::time_point now) noexcept
{
    cancel(now);
    readyResult_.reset();
    capability_.reset();
    externalHostTrust_.reset();
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
        capability_.reset();
        externalHostTrust_.reset();
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
    poll.result->capability = capability_;
    poll.result->externalHostTrust = externalHostTrust_;
    operation_.reset();
    capability_.reset();
    externalHostTrust_.reset();
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
        readyResult_->capability = capability_;
        readyResult_->externalHostTrust = externalHostTrust_;
        operation_.reset();
        return;
    }
    readyResult_ = terminalResult(
        *operation_,
        asyncStatus,
        elapsed,
        cancelRequested_);
    readyResult_->capability = capability_;
    readyResult_->externalHostTrust = externalHostTrust_;
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

BorderlessCaptureAccessAuthority::BorderlessCaptureAccessAuthority(
    PackageIdentityInfo identity,
    const std::chrono::milliseconds timeout)
    : identity_(std::move(identity))
    , request_(timeout)
{
}

BorderlessCaptureAccessPollResult BorderlessCaptureAccessAuthority::poll(
    const Clock::time_point now) noexcept
{
    if (terminalResult_.has_value())
    {
        return BorderlessCaptureAccessPollResult{false, terminalResult_};
    }
    if (!requestStarted_)
    {
        request_.begin(identity_, now);
        requestStarted_ = true;
    }

    BorderlessCaptureAccessPollResult result = request_.poll(now);
    if (result.result.has_value())
    {
        terminalResult_ = result.result;
        requestStarted_ = false;
    }
    return result;
}

void BorderlessCaptureAccessAuthority::invalidate(
    const Clock::time_point now) noexcept
{
    request_.cancel(now);
    static_cast<void>(request_.poll(now));
    terminalResult_.reset();
    requestStarted_ = false;
    if (generation_ != (std::numeric_limits<std::uint64_t>::max)())
    {
        // The epoch belongs to the process permission authority. Per-display
        // WGC retry tokens must never decide when the system prompt is reset.
        ++generation_;
    }
}

bool BorderlessCaptureAccessAuthority::pending() const noexcept
{
    return requestStarted_ && request_.pending();
}

bool BorderlessCaptureAccessAuthority::terminal() const noexcept
{
    return terminalResult_.has_value();
}

std::uint64_t BorderlessCaptureAccessAuthority::generation() const noexcept
{
    return generation_;
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
    case BorderlessCaptureAccessStatus::IdentityUntrusted:
        return "identity-untrusted";
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
    if (result.capability.has_value())
    {
        stream << ";Capability="
               << borderlessCaptureCapabilityStatusName(
                      result.capability->status)
               << ";CapabilityHRESULT="
               << hexHresult(result.capability->error);
    }
    if (result.externalHostTrust.has_value())
    {
        stream << ";ExternalHostTrust="
               << externalHostTrustStatusName(
                      result.externalHostTrust->status)
               << ";ExternalHostTrustHRESULT="
               << hexHresult(result.externalHostTrust->error);
    }
    return stream.str();
}

std::string borderlessCaptureAccessHealthDiagnostic(
    const BorderlessCaptureAccessHealthResult& result)
{
    std::ostringstream stream;
    stream << "WGC.BorderlessAccess.Health="
           << borderlessCaptureAccessStatusName(result.status)
           << ";HRESULT=" << hexHresult(result.error)
           << ";Generation=" << result.generation;
    return stream.str();
}

}
