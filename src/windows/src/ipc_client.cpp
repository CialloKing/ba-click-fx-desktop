#include "bafx/windows/ipc_client.hpp"

#include "bafx/windows/unique_handle.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>

namespace bafx::windows
{
namespace
{

class Deadline final
{
public:
    explicit Deadline(const DWORD timeoutMilliseconds) noexcept
        : startedAt_(GetTickCount64())
        , timeoutMilliseconds_(timeoutMilliseconds)
    {
    }

    [[nodiscard]] DWORD remainingMilliseconds() const noexcept
    {
        const ULONGLONG elapsed = GetTickCount64() - startedAt_;
        if (elapsed >= timeoutMilliseconds_)
        {
            return 0U;
        }
        return static_cast<DWORD>(timeoutMilliseconds_ - elapsed);
    }

private:
    ULONGLONG startedAt_{0U};
    DWORD timeoutMilliseconds_{0U};
};

[[nodiscard]] IpcClientResponse clientFailure(
    const IpcClientStatus status,
    const DWORD win32Error,
    std::string errorCode,
    std::string errorMessage)
{
    IpcClientResponse response{};
    response.status = status;
    response.win32Error = win32Error;
    response.errorCode = std::move(errorCode);
    response.errorMessage = std::move(errorMessage);
    return response;
}

[[nodiscard]] bool validPipeName(const std::wstring_view pipeName) noexcept
{
    return pipeName.starts_with(L"\\\\.\\pipe\\");
}

[[nodiscard]] bool containsRecordTerminator(const std::string_view value) noexcept
{
    return value.find('\r') != std::string_view::npos
        || value.find('\n') != std::string_view::npos
        || value.find('\0') != std::string_view::npos;
}

[[nodiscard]] bool shouldRetryPipeOpen(const DWORD error) noexcept
{
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PIPE_BUSY;
}

[[nodiscard]] UniqueHandle openPipe(
    const std::wstring& pipeName,
    const Deadline& deadline,
    DWORD& error) noexcept
{
    while (true)
    {
        const DWORD remaining = deadline.remainingMilliseconds();
        if (remaining == 0U)
        {
            error = ERROR_SEM_TIMEOUT;
            return {};
        }
        if (WaitNamedPipeW(pipeName.c_str(), remaining) == FALSE)
        {
            error = GetLastError();
            if (shouldRetryPipeOpen(error))
            {
                // The single-instance server briefly removes and recreates its
                // pipe after a short-lived client disconnects. Keep retrying
                // inside this request's deadline instead of reporting Host down.
                Sleep(1U);
                continue;
            }
            return {};
        }

        UniqueHandle pipe(CreateFileW(
            pipeName.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0U,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr));
        if (pipe.get() != INVALID_HANDLE_VALUE)
        {
            return pipe;
        }

        error = GetLastError();
        if (!shouldRetryPipeOpen(error))
        {
            return {};
        }
        // A competing client can claim the only instance between the wait and
        // CreateFileW, or the server can be recreating it after a disconnect.
        Sleep(1U);
    }
}

[[nodiscard]] bool waitForOperation(
    const HANDLE pipe,
    OVERLAPPED& overlapped,
    const HANDLE event,
    const Deadline& deadline,
    DWORD& transferred,
    DWORD& error) noexcept
{
    const DWORD waitResult = WaitForSingleObject(event, deadline.remainingMilliseconds());
    if (waitResult == WAIT_OBJECT_0)
    {
        if (GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE)
        {
            return true;
        }
        error = GetLastError();
        return false;
    }

    if (waitResult == WAIT_TIMEOUT)
    {
        static_cast<void>(CancelIoEx(pipe, &overlapped));
        // Waiting for cancellation keeps the OVERLAPPED event alive until the
        // kernel no longer references it.
        static_cast<void>(WaitForSingleObject(event, INFINITE));
        error = ERROR_SEM_TIMEOUT;
        return false;
    }

    error = waitResult == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
    static_cast<void>(CancelIoEx(pipe, &overlapped));
    static_cast<void>(WaitForSingleObject(event, INFINITE));
    return false;
}

[[nodiscard]] bool writeAll(
    const HANDLE pipe,
    const std::string_view data,
    const Deadline& deadline,
    DWORD& error) noexcept
{
    std::size_t offset = 0U;
    while (offset < data.size())
    {
        UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (event.get() == nullptr)
        {
            error = GetLastError();
            return false;
        }

        const std::size_t remaining = data.size() - offset;
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            remaining,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        DWORD transferred = 0U;
        const BOOL written = WriteFile(
            pipe,
            data.data() + offset,
            requested,
            &transferred,
            &overlapped);
        if (written == FALSE)
        {
            const DWORD writeError = GetLastError();
            if (writeError != ERROR_IO_PENDING)
            {
                error = writeError;
                return false;
            }
            if (!waitForOperation(
                    pipe,
                    overlapped,
                    event.get(),
                    deadline,
                    transferred,
                    error))
            {
                return false;
            }
        }
        if (transferred == 0U)
        {
            error = ERROR_NO_DATA;
            return false;
        }
        offset += transferred;
    }
    return true;
}

[[nodiscard]] bool readResponseLine(
    const HANDLE pipe,
    const std::size_t maxResponseBytes,
    const Deadline& deadline,
    std::string& response,
    DWORD& error,
    bool& responseTooLarge) noexcept
{
    std::array<char, 4U * 1024U> buffer{};
    response.clear();
    responseTooLarge = false;
    while (true)
    {
        UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (event.get() == nullptr)
        {
            error = GetLastError();
            return false;
        }

        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        DWORD transferred = 0U;
        const BOOL read = ReadFile(
            pipe,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &transferred,
            &overlapped);
        if (read == FALSE)
        {
            const DWORD readError = GetLastError();
            if (readError != ERROR_IO_PENDING)
            {
                error = readError;
                return false;
            }
            if (!waitForOperation(
                    pipe,
                    overlapped,
                    event.get(),
                    deadline,
                    transferred,
                    error))
            {
                return false;
            }
        }
        if (transferred == 0U)
        {
            error = ERROR_NO_DATA;
            return false;
        }

        response.append(buffer.data(), transferred);
        if (response.size() > maxResponseBytes)
        {
            responseTooLarge = true;
            return false;
        }

        const std::size_t newline = response.find('\n');
        if (newline == std::string::npos)
        {
            continue;
        }
        if (newline + 1U != response.size())
        {
            error = ERROR_INVALID_DATA;
            return false;
        }
        response.resize(newline);
        if (!response.empty() && response.back() == '\r')
        {
            response.pop_back();
        }
        return true;
    }
}

[[nodiscard]] IpcClientResponse parseResponse(const std::string_view line)
{
    if (line == "OK")
    {
        IpcClientResponse response{};
        response.status = IpcClientStatus::Ok;
        response.commandSucceeded = true;
        return response;
    }
    if (line.starts_with("OK "))
    {
        IpcClientResponse response{};
        response.status = IpcClientStatus::Ok;
        response.commandSucceeded = true;
        response.payload = std::string(line.substr(3U));
        return response;
    }
    if (!line.starts_with("ERR "))
    {
        return clientFailure(
            IpcClientStatus::InvalidResponse,
            ERROR_INVALID_DATA,
            "invalid_response",
            "response does not begin with OK or ERR");
    }

    const std::size_t codeStart = 4U;
    const std::size_t codeEnd = line.find(' ', codeStart);
    if (codeEnd == std::string_view::npos
        || codeEnd == codeStart
        || codeEnd + 1U >= line.size())
    {
        return clientFailure(
            IpcClientStatus::InvalidResponse,
            ERROR_INVALID_DATA,
            "invalid_response",
            "ERR response must include an error code and message");
    }

    IpcClientResponse response{};
    response.status = IpcClientStatus::Ok;
    response.errorCode = std::string(line.substr(codeStart, codeEnd - codeStart));
    response.errorMessage = std::string(line.substr(codeEnd + 1U));
    return response;
}

}

NamedPipeIpcClient::NamedPipeIpcClient(IpcClientOptions options)
    : options_(std::move(options))
{
}

IpcClientResponse NamedPipeIpcClient::transact(const std::string_view request) const noexcept
{
    try
    {
        if (!validPipeName(options_.pipeName)
            || options_.maxRequestBytes == 0U
            || options_.maxResponseBytes == 0U
            || options_.timeoutMilliseconds == 0U
            || options_.timeoutMilliseconds == INFINITE)
        {
            return clientFailure(
                IpcClientStatus::InvalidOptions,
                ERROR_INVALID_PARAMETER,
                "invalid_options",
                "pipe name, byte limits, and a finite timeout are required");
        }
        if (request.empty()
            || request.size() > options_.maxRequestBytes
            || containsRecordTerminator(request))
        {
            return clientFailure(
                IpcClientStatus::InvalidRequest,
                ERROR_INVALID_DATA,
                "invalid_request",
                "request must be one non-empty record within the configured byte limit");
        }

        const Deadline deadline(options_.timeoutMilliseconds);
        DWORD error = ERROR_SUCCESS;
        UniqueHandle pipe = openPipe(options_.pipeName, deadline, error);
        if (pipe.get() == nullptr)
        {
            return clientFailure(
                error == ERROR_SEM_TIMEOUT ? IpcClientStatus::Timeout : IpcClientStatus::ConnectFailed,
                error,
                error == ERROR_SEM_TIMEOUT ? "timeout" : "connect_failed",
                error == ERROR_SEM_TIMEOUT
                    ? "opening the named pipe timed out"
                    : "opening the named pipe failed");
        }

        std::string record(request);
        record.push_back('\n');
        if (!writeAll(pipe.get(), record, deadline, error))
        {
            return clientFailure(
                error == ERROR_SEM_TIMEOUT ? IpcClientStatus::Timeout : IpcClientStatus::WriteFailed,
                error,
                error == ERROR_SEM_TIMEOUT ? "timeout" : "write_failed",
                error == ERROR_SEM_TIMEOUT
                    ? "writing the named pipe request timed out"
                    : "writing the named pipe request failed");
        }

        std::string responseLine;
        bool responseTooLarge = false;
        if (!readResponseLine(
                pipe.get(),
                options_.maxResponseBytes,
                deadline,
                responseLine,
                error,
                responseTooLarge))
        {
            if (responseTooLarge)
            {
                return clientFailure(
                    IpcClientStatus::ResponseTooLarge,
                    ERROR_BUFFER_OVERFLOW,
                    "response_too_large",
                    "response exceeds the configured byte limit");
            }
            return clientFailure(
                error == ERROR_SEM_TIMEOUT ? IpcClientStatus::Timeout : IpcClientStatus::ReadFailed,
                error,
                error == ERROR_SEM_TIMEOUT ? "timeout" : "read_failed",
                error == ERROR_SEM_TIMEOUT
                    ? "reading the named pipe response timed out"
                    : "reading the named pipe response failed");
        }

        return parseResponse(responseLine);
    }
    catch (...)
    {
        return clientFailure(
            IpcClientStatus::InternalError,
            ERROR_NOT_ENOUGH_MEMORY,
            "internal_error",
            "named pipe client could not complete the transaction");
    }
}

}
