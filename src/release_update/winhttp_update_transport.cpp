#include "update_check.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <string>
#include <utility>

namespace bafx::release_update
{
namespace
{

constexpr wchar_t githubApiHost[] = L"api.github.com";
constexpr wchar_t githubLatestReleasePath[] =
    L"/repos/CialloKing/ba-click-fx-desktop/releases/latest";
constexpr wchar_t updateUserAgent[] = L"ba-click-fx-desktop-update-check";
constexpr wchar_t githubHeaders[] =
    L"Accept: application/vnd.github+json\r\n"
    L"X-GitHub-Api-Version: 2022-11-28\r\n";

constexpr int resolveTimeoutMilliseconds = 5'000;
constexpr int connectTimeoutMilliseconds = 5'000;
constexpr int sendTimeoutMilliseconds = 5'000;
constexpr int receiveTimeoutMilliseconds = 10'000;
constexpr auto completeRequestTimeout = std::chrono::seconds(30);
constexpr std::size_t readBufferBytes = 8U * 1024U;

[[nodiscard]] ReleaseTransportResult cancelledResult()
{
    return ReleaseTransportResult{
        .status = ReleaseTransportStatus::Cancelled};
}

[[nodiscard]] ReleaseTransportResult failedResult(std::string failure)
{
    return ReleaseTransportResult{
        .status = ReleaseTransportStatus::Failed,
        .failure = std::move(failure)};
}

[[nodiscard]] ReleaseTransportResult nativeFailure(
    const std::string_view operation,
    const DWORD error)
{
    return failedResult(
        std::string(operation) + " failed with Win32 error "
        + std::to_string(error));
}

enum class CompletionKind : std::uint8_t
{
    None,
    SendRequest,
    HeadersAvailable,
    ReadComplete,
    RequestError
};

struct AsyncCompletion final
{
    CompletionKind kind{CompletionKind::None};
    DWORD value{0U};
    DWORD error{ERROR_SUCCESS};
};

enum class CompletionWaitStatus : std::uint8_t
{
    Completed,
    Cancelled,
    TimedOut
};

struct CompletionWaitResult final
{
    CompletionWaitStatus status{CompletionWaitStatus::TimedOut};
    AsyncCompletion completion{};
};

class AsyncRequestContext final
{
public:
    AsyncRequestContext() = default;

    AsyncRequestContext(const AsyncRequestContext&) = delete;
    AsyncRequestContext& operator=(const AsyncRequestContext&) = delete;

    void addReference() noexcept
    {
        referenceCount_.fetch_add(1U, std::memory_order_relaxed);
    }

    void releaseReference() noexcept
    {
        if (referenceCount_.fetch_sub(1U, std::memory_order_acq_rel) == 1U)
        {
            delete this;
        }
    }

    [[nodiscard]] bool cancelled() const noexcept
    {
        return cancelled_.load(std::memory_order_acquire);
    }

    void cancel() noexcept
    {
        cancelled_.store(true, std::memory_order_release);
        completionCondition_.notify_all();
        closeRequest();
    }

    [[nodiscard]] bool retainSession(const HINTERNET session) noexcept
    {
        if (cancelled())
        {
            WinHttpCloseHandle(session);
            return false;
        }

        session_ = session;
        return true;
    }

    [[nodiscard]] bool retainConnection(const HINTERNET connection) noexcept
    {
        if (cancelled())
        {
            WinHttpCloseHandle(connection);
            return false;
        }

        connection_ = connection;
        return true;
    }

    [[nodiscard]] HINTERNET session() const noexcept
    {
        return session_;
    }

    [[nodiscard]] HINTERNET connection() const noexcept
    {
        return connection_;
    }

    [[nodiscard]] bool configureRequest(
        const HINTERNET request,
        DWORD& error) noexcept
    {
        std::scoped_lock lock(requestMutex_);
        if (cancelled())
        {
            WinHttpCloseHandle(request);
            error = ERROR_OPERATION_ABORTED;
            return false;
        }

        DWORD disabledFeatures = WINHTTP_DISABLE_REDIRECTS;
        if (!WinHttpSetOption(
                request,
                WINHTTP_OPTION_DISABLE_FEATURE,
                &disabledFeatures,
                sizeof(disabledFeatures)))
        {
            error = GetLastError();
            WinHttpCloseHandle(request);
            return false;
        }

        // Default WinHTTP certificate validation remains intact. This module
        // intentionally never writes WINHTTP_OPTION_SECURITY_FLAGS.
        if (!WinHttpAddRequestHeaders(
                request,
                githubHeaders,
                static_cast<DWORD>(-1L),
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
        {
            error = GetLastError();
            WinHttpCloseHandle(request);
            return false;
        }

        constexpr DWORD callbackFlags =
            WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE
            | WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE
            | WINHTTP_CALLBACK_FLAG_READ_COMPLETE
            | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR
            | WINHTTP_CALLBACK_FLAG_HANDLES;
        const WINHTTP_STATUS_CALLBACK previousCallback = WinHttpSetStatusCallback(
            request,
            &AsyncRequestContext::statusCallback,
            callbackFlags,
            0U);
        if (previousCallback == WINHTTP_INVALID_STATUS_CALLBACK)
        {
            error = GetLastError();
            WinHttpCloseHandle(request);
            return false;
        }

        DWORD_PTR contextValue = reinterpret_cast<DWORD_PTR>(this);
        if (!WinHttpSetOption(
                request,
                WINHTTP_OPTION_CONTEXT_VALUE,
                &contextValue,
                sizeof(contextValue)))
        {
            error = GetLastError();
            WinHttpSetStatusCallback(request, nullptr, 0U, 0U);
            WinHttpCloseHandle(request);
            return false;
        }

        // Once a non-null context is attached, the final HANDLE_CLOSING
        // callback owns a reference so late callbacks cannot touch freed state.
        addReference();
        requestReferenceActive_.store(true, std::memory_order_release);
        request_ = request;
        if (cancelled())
        {
            error = ERROR_OPERATION_ABORTED;
            return false;
        }

        error = ERROR_SUCCESS;
        return true;
    }

    [[nodiscard]] bool beginSend(DWORD& error) noexcept
    {
        return beginOperation(
            [this](const HINTERNET request)
            {
                return WinHttpSendRequest(
                    request,
                    WINHTTP_NO_ADDITIONAL_HEADERS,
                    0U,
                    WINHTTP_NO_REQUEST_DATA,
                    0U,
                    0U,
                    reinterpret_cast<DWORD_PTR>(this));
            },
            error);
    }

    [[nodiscard]] bool beginReceive(DWORD& error) noexcept
    {
        return beginOperation(
            [](const HINTERNET request)
            {
                return WinHttpReceiveResponse(request, nullptr);
            },
            error);
    }

    [[nodiscard]] bool beginRead(
        const DWORD requested,
        DWORD& error) noexcept
    {
        return beginOperation(
            [this, requested](const HINTERNET request)
            {
                return WinHttpReadData(
                    request,
                    readBuffer_.data(),
                    requested,
                    nullptr);
            },
            error);
    }

    [[nodiscard]] bool queryStatusCode(
        DWORD& statusCode,
        DWORD& error) noexcept
    {
        std::scoped_lock lock(requestMutex_);
        if (cancelled() || request_ == nullptr)
        {
            error = ERROR_OPERATION_ABORTED;
            return false;
        }

        DWORD statusCodeBytes = sizeof(statusCode);
        if (!WinHttpQueryHeaders(
                request_,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &statusCode,
                &statusCodeBytes,
                WINHTTP_NO_HEADER_INDEX))
        {
            error = GetLastError();
            return false;
        }

        error = ERROR_SUCCESS;
        return true;
    }

    [[nodiscard]] const char* readBuffer() const noexcept
    {
        return readBuffer_.data();
    }

    [[nodiscard]] CompletionWaitResult waitForCompletion(
        const std::chrono::steady_clock::time_point deadline) noexcept
    {
        std::unique_lock lock(completionMutex_);
        const bool signalled = completionCondition_.wait_until(
            lock,
            deadline,
            [this]()
            {
                return cancelled()
                    || completion_.kind != CompletionKind::None;
            });
        if (cancelled())
        {
            return CompletionWaitResult{
                .status = CompletionWaitStatus::Cancelled};
        }

        if (!signalled)
        {
            return CompletionWaitResult{
                .status = CompletionWaitStatus::TimedOut};
        }

        const AsyncCompletion completion = completion_;
        completion_ = AsyncCompletion{};
        return CompletionWaitResult{
            .status = CompletionWaitStatus::Completed,
            .completion = completion};
    }

    void closeRequest() noexcept
    {
        HINTERNET request = nullptr;
        {
            std::scoped_lock lock(requestMutex_);
            request = std::exchange(request_, nullptr);
        }

        if (request != nullptr)
        {
            // The request is asynchronous. Closing it is the documented
            // cancellation operation and is serialized with API initiation.
            WinHttpCloseHandle(request);
        }
    }

private:
    ~AsyncRequestContext()
    {
        closeRequest();
        if (connection_ != nullptr)
        {
            WinHttpCloseHandle(connection_);
        }

        if (session_ != nullptr)
        {
            WinHttpCloseHandle(session_);
        }
    }

    template <typename Operation>
    [[nodiscard]] bool beginOperation(
        Operation&& operation,
        DWORD& error) noexcept
    {
        {
            std::scoped_lock completionLock(completionMutex_);
            completion_ = AsyncCompletion{};
        }

        std::scoped_lock requestLock(requestMutex_);
        if (cancelled() || request_ == nullptr)
        {
            error = ERROR_OPERATION_ABORTED;
            return false;
        }

        if (!operation(request_))
        {
            error = GetLastError();
            return false;
        }

        error = ERROR_SUCCESS;
        return true;
    }

    static void CALLBACK statusCallback(
        const HINTERNET,
        const DWORD_PTR contextValue,
        const DWORD internetStatus,
        void* statusInformation,
        const DWORD statusInformationLength) noexcept
    {
        if (contextValue == 0U)
        {
            return;
        }

        auto* context = reinterpret_cast<AsyncRequestContext*>(contextValue);
        context->onStatus(
            internetStatus,
            statusInformation,
            statusInformationLength);
    }

    void onStatus(
        const DWORD internetStatus,
        const void* statusInformation,
        const DWORD statusInformationLength) noexcept
    {
        if (internetStatus == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
        {
            if (requestReferenceActive_.exchange(
                    false,
                    std::memory_order_acq_rel))
            {
                releaseReference();
            }

            return;
        }

        AsyncCompletion completion{};
        switch (internetStatus)
        {
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
            completion.kind = CompletionKind::SendRequest;
            break;
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
            completion.kind = CompletionKind::HeadersAvailable;
            break;
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            completion.kind = CompletionKind::ReadComplete;
            completion.value = statusInformationLength;
            break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            completion.kind = CompletionKind::RequestError;
            if (statusInformation == nullptr
                || statusInformationLength < sizeof(WINHTTP_ASYNC_RESULT))
            {
                completion.error = ERROR_INVALID_DATA;
            }
            else
            {
                const auto* result =
                    static_cast<const WINHTTP_ASYNC_RESULT*>(statusInformation);
                completion.error = result->dwError;
            }
            break;
        default:
            return;
        }

        {
            std::scoped_lock lock(completionMutex_);
            completion_ = completion;
        }
        completionCondition_.notify_all();
    }

    std::atomic_uint32_t referenceCount_{1U};
    std::atomic_bool cancelled_{false};
    std::atomic_bool requestReferenceActive_{false};
    std::mutex requestMutex_{};
    std::mutex completionMutex_{};
    std::condition_variable completionCondition_{};
    AsyncCompletion completion_{};
    HINTERNET session_{nullptr};
    HINTERNET connection_{nullptr};
    HINTERNET request_{nullptr};
    std::array<char, readBufferBytes> readBuffer_{};
};

[[nodiscard]] ReleaseTransportResult immediateFailure(
    AsyncRequestContext& context,
    const std::stop_token stopToken,
    const std::string_view operation,
    const DWORD error)
{
    if (context.cancelled()
        || stopToken.stop_requested()
        || error == ERROR_WINHTTP_OPERATION_CANCELLED
        || error == ERROR_OPERATION_ABORTED)
    {
        return cancelledResult();
    }

    return nativeFailure(operation, error);
}

[[nodiscard]] bool awaitCompletion(
    AsyncRequestContext& context,
    const std::stop_token stopToken,
    const CompletionKind expected,
    const std::chrono::steady_clock::time_point deadline,
    AsyncCompletion& completion,
    ReleaseTransportResult& failure)
{
    const CompletionWaitResult waitResult = context.waitForCompletion(deadline);
    if (waitResult.status == CompletionWaitStatus::Cancelled
        || stopToken.stop_requested())
    {
        failure = cancelledResult();
        return false;
    }

    if (waitResult.status == CompletionWaitStatus::TimedOut)
    {
        context.cancel();
        failure = failedResult("GitHub request exceeded the 30 second deadline");
        return false;
    }

    completion = waitResult.completion;
    if (completion.kind == CompletionKind::RequestError)
    {
        failure = immediateFailure(
            context,
            stopToken,
            "Asynchronous WinHTTP request",
            completion.error);
        return false;
    }

    if (completion.kind != expected)
    {
        context.cancel();
        failure = failedResult("WinHTTP returned an unexpected completion");
        return false;
    }

    return true;
}

class WinHttpReleaseTransport final : public ReleaseTransport
{
public:
    ~WinHttpReleaseTransport() override
    {
        cancel();
    }

    [[nodiscard]] ReleaseTransportResult fetchLatestRelease(
        const std::stop_token stopToken) override
    {
        // A transport instance owns one WinHTTP handle tree. Serializing calls
        // keeps cancellation precise instead of affecting another request.
        std::scoped_lock operationLock(operationMutex_);
        auto* context = new AsyncRequestContext();
        {
            std::scoped_lock activeLock(activeMutex_);
            activeContext_ = context;
        }

        try
        {
            ReleaseTransportResult result{};
            {
                const std::stop_callback stopCallback(
                    stopToken,
                    [context]() noexcept
                    {
                        context->cancel();
                    });
                result = fetch(*context, stopToken);
                context->closeRequest();
                clearActiveContext(context);
            }
            context->releaseReference();
            return result;
        }
        catch (...)
        {
            context->closeRequest();
            clearActiveContext(context);
            context->releaseReference();
            throw;
        }
    }

    void cancel() noexcept override
    {
        AsyncRequestContext* context = nullptr;
        {
            std::scoped_lock activeLock(activeMutex_);
            context = activeContext_;
            if (context != nullptr)
            {
                context->addReference();
            }
        }

        if (context != nullptr)
        {
            context->cancel();
            context->releaseReference();
        }
    }

private:
    [[nodiscard]] ReleaseTransportResult fetch(
        AsyncRequestContext& context,
        const std::stop_token stopToken)
    {
        if (stopToken.stop_requested())
        {
            return cancelledResult();
        }

        const HINTERNET session = WinHttpOpen(
            updateUserAgent,
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            // Secure defaults require TLS 1.2+ and reject attempts to re-enable
            // older protocols; the request itself remains fully asynchronous.
            WINHTTP_FLAG_ASYNC | WINHTTP_FLAG_SECURE_DEFAULTS);
        if (session == nullptr)
        {
            return immediateFailure(
                context,
                stopToken,
                "WinHttpOpen",
                GetLastError());
        }

        if (!context.retainSession(session))
        {
            return cancelledResult();
        }

        if (!WinHttpSetTimeouts(
                context.session(),
                resolveTimeoutMilliseconds,
                connectTimeoutMilliseconds,
                sendTimeoutMilliseconds,
                receiveTimeoutMilliseconds))
        {
            return immediateFailure(
                context,
                stopToken,
                "WinHttpSetTimeouts",
                GetLastError());
        }

        const HINTERNET connection = WinHttpConnect(
            context.session(),
            githubApiHost,
            INTERNET_DEFAULT_HTTPS_PORT,
            0U);
        if (connection == nullptr)
        {
            return immediateFailure(
                context,
                stopToken,
                "WinHttpConnect",
                GetLastError());
        }

        if (!context.retainConnection(connection))
        {
            return cancelledResult();
        }

        const HINTERNET request = WinHttpOpenRequest(
            context.connection(),
            L"GET",
            githubLatestReleasePath,
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);
        if (request == nullptr)
        {
            return immediateFailure(
                context,
                stopToken,
                "WinHttpOpenRequest",
                GetLastError());
        }

        DWORD error = ERROR_SUCCESS;
        if (!context.configureRequest(request, error))
        {
            return immediateFailure(
                context,
                stopToken,
                "Configure WinHTTP request",
                error);
        }

        const auto deadline = std::chrono::steady_clock::now()
            + completeRequestTimeout;
        if (!context.beginSend(error))
        {
            return immediateFailure(
                context,
                stopToken,
                "WinHttpSendRequest",
                error);
        }

        AsyncCompletion completion{};
        ReleaseTransportResult failure{};
        if (!awaitCompletion(
                context,
                stopToken,
                CompletionKind::SendRequest,
                deadline,
                completion,
                failure))
        {
            return failure;
        }

        if (!context.beginReceive(error))
        {
            return immediateFailure(
                context,
                stopToken,
                "WinHttpReceiveResponse",
                error);
        }

        if (!awaitCompletion(
                context,
                stopToken,
                CompletionKind::HeadersAvailable,
                deadline,
                completion,
                failure))
        {
            return failure;
        }

        DWORD statusCode = 0U;
        if (!context.queryStatusCode(statusCode, error))
        {
            return immediateFailure(
                context,
                stopToken,
                "WinHttpQueryHeaders(status)",
                error);
        }

        // Redirect responses are surfaced to the checker as HTTP failures;
        // neither their Location nor their response body is consumed.
        if (statusCode != 200U)
        {
            return ReleaseTransportResult{
                .status = ReleaseTransportStatus::Succeeded,
                .httpStatus = statusCode};
        }

        std::string body;
        while (true)
        {
            // Read one byte beyond the accepted limit when necessary so an
            // exactly-256-KiB body can still be distinguished from overflow.
            const std::size_t bytesUntilOverflow =
                maximumResponseBytes - body.size() + 1U;
            const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
                readBufferBytes,
                bytesUntilOverflow));
            if (!context.beginRead(requested, error))
            {
                return immediateFailure(
                    context,
                    stopToken,
                    "WinHttpReadData",
                    error);
            }

            if (!awaitCompletion(
                    context,
                    stopToken,
                    CompletionKind::ReadComplete,
                    deadline,
                    completion,
                    failure))
            {
                return failure;
            }

            const DWORD received = completion.value;
            if (received == 0U)
            {
                break;
            }

            if (received > requested
                || static_cast<std::size_t>(received)
                    > maximumResponseBytes - body.size())
            {
                return failedResult("GitHub response exceeds 256 KiB");
            }

            body.append(context.readBuffer(), received);
        }

        return ReleaseTransportResult{
            .status = ReleaseTransportStatus::Succeeded,
            .httpStatus = statusCode,
            .body = std::move(body)};
    }

    void clearActiveContext(AsyncRequestContext* const context) noexcept
    {
        std::scoped_lock activeLock(activeMutex_);
        if (activeContext_ == context)
        {
            activeContext_ = nullptr;
        }
    }

    std::mutex operationMutex_{};
    std::mutex activeMutex_{};
    AsyncRequestContext* activeContext_{nullptr};
};

}

std::shared_ptr<ReleaseTransport> makeWinHttpReleaseTransport()
{
    return std::make_shared<WinHttpReleaseTransport>();
}

}
