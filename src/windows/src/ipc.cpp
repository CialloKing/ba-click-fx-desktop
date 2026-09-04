#include "bafx/windows/ipc.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace bafx::windows
{
namespace
{

constexpr std::size_t maximumCommandTokenBytes = 32U;
constexpr std::size_t maximumParserLineBytes = 1U * 1024U * 1024U;

[[nodiscard]] std::string sanitizeLine(const std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const char character : value)
    {
        if (character == '\r' || character == '\n' || character == '\0')
        {
            result.push_back(' ');
        }
        else
        {
            result.push_back(character);
        }
    }
    return result;
}

[[nodiscard]] std::string sanitizeToken(const std::string_view value)
{
    std::string result = sanitizeLine(value);
    for (char& character : result)
    {
        if (character == ' ' || character == '\t')
        {
            character = '_';
        }
    }
    return result;
}

[[nodiscard]] std::optional<IpcCommand> commandFromName(
    const std::string_view name) noexcept
{
    if (name == "GetState")
    {
        return IpcCommand::GetState;
    }
    if (name == "GetDisplayState")
    {
        return IpcCommand::GetDisplayState;
    }
    if (name == "GetConfig")
    {
        return IpcCommand::GetConfig;
    }
    if (name == "GetFxConfig")
    {
        return IpcCommand::GetFxConfig;
    }
    if (name == "SetConfig")
    {
        return IpcCommand::SetConfig;
    }
    if (name == "SetHotkeys")
    {
        return IpcCommand::SetHotkeys;
    }
    if (name == "GetHotkeyState")
    {
        return IpcCommand::GetHotkeyState;
    }
    if (name == "RetryHotkeys")
    {
        return IpcCommand::RetryHotkeys;
    }
    if (name == "BeginHotkeyCapture")
    {
        return IpcCommand::BeginHotkeyCapture;
    }
    if (name == "EndHotkeyCapture")
    {
        return IpcCommand::EndHotkeyCapture;
    }
    if (name == "SetDisplayOverride")
    {
        return IpcCommand::SetDisplayOverride;
    }
    if (name == "RemoveDisplayOverride")
    {
        return IpcCommand::RemoveDisplayOverride;
    }
    if (name == "SetFxParam")
    {
        return IpcCommand::SetFxParam;
    }
    if (name == "SetFxParams")
    {
        return IpcCommand::SetFxParams;
    }
    if (name == "ResetFxConfig")
    {
        return IpcCommand::ResetFxConfig;
    }
    if (name == "SaveFxProfile")
    {
        return IpcCommand::SaveFxProfile;
    }
    if (name == "ApplyFxProfile")
    {
        return IpcCommand::ApplyFxProfile;
    }
    if (name == "DeleteFxProfile")
    {
        return IpcCommand::DeleteFxProfile;
    }
    if (name == "Pause")
    {
        return IpcCommand::Pause;
    }
    if (name == "Resume")
    {
        return IpcCommand::Resume;
    }
    if (name == "ClearLogs")
    {
        return IpcCommand::ClearLogs;
    }
    if (name == "Shutdown")
    {
        return IpcCommand::Shutdown;
    }
    return std::nullopt;
}

[[nodiscard]] bool commandAcceptsPayload(const IpcCommand command) noexcept
{
    return command == IpcCommand::SetConfig
        || command == IpcCommand::SetHotkeys
        || command == IpcCommand::GetHotkeyState
        || command == IpcCommand::EndHotkeyCapture
        || command == IpcCommand::SetDisplayOverride
        || command == IpcCommand::RemoveDisplayOverride
        || command == IpcCommand::SetFxParam
        || command == IpcCommand::SetFxParams
        || command == IpcCommand::SaveFxProfile
        || command == IpcCommand::ApplyFxProfile
        || command == IpcCommand::DeleteFxProfile;
}

[[nodiscard]] bool isTransientPipeError(const DWORD error) noexcept
{
    return error == ERROR_NOT_ENOUGH_MEMORY;
}

[[nodiscard]] DWORD boundedDword(const std::size_t value) noexcept
{
    return static_cast<DWORD>(std::min<std::size_t>(
        value,
        static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
}

}

IpcParseResult parseIpcRequest(const std::string_view line)
{
    IpcParseResult result{};
    if (line.empty())
    {
        result.errorCode = "empty_request";
        result.errorMessage = "request line is empty";
        return result;
    }
    if (line.size() > maximumParserLineBytes)
    {
        result.errorCode = "request_too_large";
        result.errorMessage = "request line exceeds the parser limit";
        return result;
    }
    if (line.find('\r') != std::string_view::npos
        || line.find('\n') != std::string_view::npos
        || line.find('\0') != std::string_view::npos)
    {
        result.errorCode = "invalid_request";
        result.errorMessage = "request contains a line terminator or NUL";
        return result;
    }

    const std::size_t separator = line.find(' ');
    const std::string_view commandName = line.substr(
        0U,
        separator == std::string_view::npos ? line.size() : separator);
    if (commandName.empty() || commandName.size() > maximumCommandTokenBytes)
    {
        result.errorCode = "invalid_command";
        result.errorMessage = "command token is empty or too long";
        return result;
    }

    const std::optional<IpcCommand> command = commandFromName(commandName);
    if (!command.has_value())
    {
        result.errorCode = "unknown_command";
        result.errorMessage = "command is not supported";
        return result;
    }

    const std::string_view payload = separator == std::string_view::npos
        ? std::string_view{}
        : line.substr(separator + 1U);
    if (commandAcceptsPayload(*command) && payload.empty()
        && *command != IpcCommand::GetHotkeyState)
    {
        result.errorCode = "missing_payload";
        result.errorMessage = "configuration command requires a payload";
        return result;
    }
    if (!commandAcceptsPayload(*command) && !payload.empty())
    {
        result.errorCode = "unexpected_payload";
        result.errorMessage = "command does not accept a payload";
        return result;
    }

    result.request = IpcRequest{*command, std::string(payload)};
    return result;
}

IpcResponse IpcResponse::success(std::string payload)
{
    IpcResponse response{};
    response.succeeded = true;
    response.payload = std::move(payload);
    return response;
}

IpcResponse IpcResponse::failure(
    std::string errorCode,
    std::string errorMessage)
{
    IpcResponse response{};
    response.succeeded = false;
    response.errorCode = std::move(errorCode);
    response.errorMessage = std::move(errorMessage);
    return response;
}

std::string serializeIpcResponse(const IpcResponse& response)
{
    if (response.succeeded)
    {
        const std::string payload = sanitizeLine(response.payload);
        if (payload.empty())
        {
            return "OK\n";
        }
        return "OK " + payload + "\n";
    }

    std::string code = sanitizeToken(response.errorCode);
    std::string message = sanitizeLine(response.errorMessage);
    if (code.empty())
    {
        code = "internal_error";
    }
    if (message.empty())
    {
        message = "request failed";
    }
    return "ERR " + code + " " + message + "\n";
}

NamedPipeIpcServer::NamedPipeIpcServer(IpcRequestHandler handler)
    : NamedPipeIpcServer(std::move(handler), Options{})
{
}

NamedPipeIpcServer::NamedPipeIpcServer(
    IpcRequestHandler handler,
    Options options)
    : handler_(std::move(handler))
    , options_(std::move(options))
{
}

NamedPipeIpcServer::~NamedPipeIpcServer()
{
    stop();
}

bool NamedPipeIpcServer::start() noexcept
{
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (worker_.joinable() || running_.load(std::memory_order_acquire))
    {
        setLastError(ERROR_ALREADY_EXISTS);
        return false;
    }
    if (options_.pipeName.empty()
        || !std::wstring_view(options_.pipeName).starts_with(L"\\\\.\\pipe\\"))
    {
        setLastError(ERROR_INVALID_NAME);
        return false;
    }
    if (options_.maxRequestBytes == 0U
        || options_.maxRequestBytes > maximumParserLineBytes
        || options_.maxResponseBytes == 0U
        || options_.maxResponseBytes > maximumParserLineBytes
        || options_.ioTimeoutMilliseconds == 0U
        || options_.maxCommandsPerConnection == 0U)
    {
        setLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    setLastError(ERROR_SUCCESS);
    stopRequestedFlag_.store(false, std::memory_order_release);
    stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (stopEvent_.get() == nullptr)
    {
        setLastError(GetLastError());
        return false;
    }

    UniqueHandle initialPipe = createPipe();
    if (initialPipe.get() == nullptr)
    {
        stopEvent_.reset();
        return false;
    }

    running_.store(true, std::memory_order_release);
    try
    {
        worker_ = std::thread(
            [this, pipe = std::move(initialPipe)]() mutable
            {
                workerMain(std::move(pipe));
            });
    }
    catch (...)
    {
        running_.store(false, std::memory_order_release);
        stopEvent_.reset();
        setLastError(ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    return true;
}

void NamedPipeIpcServer::stop() noexcept
{
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    stopRequestedFlag_.store(true, std::memory_order_release);
    if (!worker_.joinable())
    {
        running_.store(false, std::memory_order_release);
        stopEvent_.reset();
        return;
    }

    if (stopEvent_.get() != nullptr)
    {
        static_cast<void>(SetEvent(stopEvent_.get()));
    }
    worker_.join();
    running_.store(false, std::memory_order_release);
    stopEvent_.reset();
}

bool NamedPipeIpcServer::running() const noexcept
{
    return running_.load(std::memory_order_acquire);
}

DWORD NamedPipeIpcServer::lastError() const noexcept
{
    return lastError_.load(std::memory_order_acquire);
}

UniqueHandle NamedPipeIpcServer::createPipe() noexcept
{
    const DWORD bufferSize = boundedDword(std::max(
        options_.maxRequestBytes,
        options_.maxResponseBytes));
    const DWORD openMode = PIPE_ACCESS_DUPLEX
        | FILE_FLAG_OVERLAPPED
        | FILE_FLAG_FIRST_PIPE_INSTANCE;
    const DWORD pipeMode = PIPE_TYPE_BYTE
        | PIPE_READMODE_BYTE
        | PIPE_WAIT
        | PIPE_REJECT_REMOTE_CLIENTS;
    const HANDLE pipe = CreateNamedPipeW(
        options_.pipeName.c_str(),
        openMode,
        pipeMode,
        1U,
        bufferSize,
        bufferSize,
        options_.ioTimeoutMilliseconds,
        nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
    {
        setLastError(GetLastError());
        return {};
    }
    return UniqueHandle(pipe);
}

NamedPipeIpcServer::ConnectResult NamedPipeIpcServer::waitForConnection(
    const HANDLE pipe) noexcept
{
    UniqueHandle operationEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (operationEvent.get() == nullptr)
    {
        setLastError(GetLastError());
        return ConnectResult::Failed;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = operationEvent.get();
    const BOOL connected = ConnectNamedPipe(pipe, &overlapped);
    if (connected != FALSE)
    {
        return ConnectResult::Connected;
    }

    const DWORD connectError = GetLastError();
    if (connectError == ERROR_PIPE_CONNECTED)
    {
        return ConnectResult::Connected;
    }
    if (connectError != ERROR_IO_PENDING)
    {
        setLastError(connectError);
        return isTransientPipeError(connectError)
            ? ConnectResult::Retry
            : ConnectResult::Failed;
    }

    const HANDLE handles[] = {stopEvent_.get(), operationEvent.get()};
    const DWORD waitResult = WaitForMultipleObjects(
        2U,
        handles,
        FALSE,
        INFINITE);
    if (waitResult == WAIT_OBJECT_0)
    {
        cancelAndDrain(pipe, overlapped, operationEvent.get());
        return ConnectResult::Stopped;
    }
    if (waitResult != WAIT_OBJECT_0 + 1U)
    {
        setLastError(waitResult == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE);
        cancelAndDrain(pipe, overlapped, operationEvent.get());
        return ConnectResult::Failed;
    }

    DWORD transferred = 0U;
    if (GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE
        || GetLastError() == ERROR_PIPE_CONNECTED)
    {
        return ConnectResult::Connected;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_OPERATION_ABORTED && stopRequested())
    {
        return ConnectResult::Stopped;
    }
    setLastError(error);
    return ConnectResult::Retry;
}

bool NamedPipeIpcServer::serveClient(const HANDLE pipe) noexcept
{
    try
    {
        std::string pending;
        pending.reserve(std::min<std::size_t>(options_.maxRequestBytes, 4U * 1024U));
        std::uint32_t commandCount = 0U;
        while (!stopRequested())
        {
            std::string chunk;
            if (!readChunk(pipe, chunk))
            {
                return false;
            }
            for (const char character : chunk)
            {
                if (character != '\n')
                {
                    if (pending.size() >= options_.maxRequestBytes)
                    {
                        static_cast<void>(writeAll(
                            pipe,
                            serializeIpcResponse(IpcResponse::failure(
                                "request_too_large",
                                "request exceeds the configured limit"))));
                        return false;
                    }
                    pending.push_back(character);
                    continue;
                }

                if (!pending.empty() && pending.back() == '\r')
                {
                    pending.pop_back();
                }
                if (!processLine(pipe, std::move(pending), commandCount))
                {
                    return false;
                }
                pending.clear();
            }
        }
    }
    catch (...)
    {
        setLastError(ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    return false;
}

bool NamedPipeIpcServer::processLine(
    const HANDLE pipe,
    std::string line,
    std::uint32_t& commandCount) noexcept
{
    try
    {
        ++commandCount;
        if (commandCount > options_.maxCommandsPerConnection)
        {
            static_cast<void>(writeAll(
                pipe,
                serializeIpcResponse(IpcResponse::failure(
                    "command_limit",
                    "connection command limit exceeded"))));
            return false;
        }

        const IpcParseResult parsed = parseIpcRequest(line);
        IpcResponse response{};
        if (!parsed.succeeded())
        {
            response = IpcResponse::failure(
                parsed.errorCode,
                parsed.errorMessage);
        }
        else if (!handler_)
        {
            response = IpcResponse::failure(
                "handler_unavailable",
                "no request handler is installed");
        }
        else
        {
            try
            {
                response = handler_(*parsed.request);
            }
            catch (...)
            {
                response = IpcResponse::failure(
                    "handler_error",
                    "request handler raised an exception");
            }
        }

        if (parsed.succeeded()
            && parsed.request->command == IpcCommand::Shutdown
            && response.succeeded)
        {
            // Shutdown is acknowledged before the service wakes its waiters,
            // allowing the control center to exit cleanly.
            response.stopServer = true;
            response.closeConnection = true;
        }

        std::string serialized = serializeIpcResponse(response);
        if (serialized.size() > options_.maxResponseBytes)
        {
            serialized = serializeIpcResponse(IpcResponse::failure(
                "response_too_large",
                "response exceeds the configured limit"));
        }
        if (!writeAll(pipe, serialized))
        {
            return false;
        }
        if (response.stopServer)
        {
            // A completed pipe write can still be discarded by an immediate
            // server disconnect. Wait for the one-request client to close,
            // bounded by the normal I/O deadline, before publishing shutdown.
            waitForShutdownClientCompletion(pipe);
            stopRequestedFlag_.store(true, std::memory_order_release);
            static_cast<void>(SetEvent(stopEvent_.get()));
        }
        return !response.closeConnection && !response.stopServer;
    }
    catch (...)
    {
        setLastError(ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
}

bool NamedPipeIpcServer::readChunk(
    const HANDLE pipe,
    std::string& chunk) noexcept
{
    std::array<char, 4U * 1024U> buffer{};
    UniqueHandle operationEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (operationEvent.get() == nullptr)
    {
        setLastError(GetLastError());
        return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = operationEvent.get();
    DWORD bytesRead = 0U;
    const BOOL read = ReadFile(
        pipe,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &bytesRead,
        &overlapped);
    if (read == FALSE)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA)
        {
            return false;
        }
        if (error != ERROR_IO_PENDING)
        {
            setLastError(error);
            return false;
        }
        if (!waitForOverlapped(
                pipe,
                overlapped,
                operationEvent.get(),
                bytesRead))
        {
            return false;
        }
    }
    if (bytesRead == 0U)
    {
        return false;
    }
    chunk.assign(buffer.data(), bytesRead);
    return true;
}

bool NamedPipeIpcServer::writeAll(
    const HANDLE pipe,
    const std::string_view data) noexcept
{
    std::size_t offset = 0U;
    while (offset < data.size() && !stopRequested())
    {
        UniqueHandle operationEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (operationEvent.get() == nullptr)
        {
            setLastError(GetLastError());
            return false;
        }

        const std::size_t remaining = data.size() - offset;
        const DWORD requested = boundedDword(remaining);
        OVERLAPPED overlapped{};
        overlapped.hEvent = operationEvent.get();
        DWORD bytesWritten = 0U;
        const BOOL written = WriteFile(
            pipe,
            data.data() + offset,
            requested,
            &bytesWritten,
            &overlapped);
        if (written == FALSE)
        {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING)
            {
                setLastError(error);
                return false;
            }
            if (!waitForOverlapped(
                    pipe,
                    overlapped,
                    operationEvent.get(),
                    bytesWritten))
            {
                return false;
            }
        }
        if (bytesWritten == 0U)
        {
            setLastError(ERROR_NO_DATA);
            return false;
        }
        offset += bytesWritten;
    }
    return offset == data.size();
}

bool NamedPipeIpcServer::waitForOverlapped(
    const HANDLE pipe,
    OVERLAPPED& overlapped,
    const HANDLE operationEvent,
    DWORD& transferred) noexcept
{
    const HANDLE handles[] = {stopEvent_.get(), operationEvent};
    const DWORD waitResult = WaitForMultipleObjects(
        2U,
        handles,
        FALSE,
        options_.ioTimeoutMilliseconds);
    if (waitResult == WAIT_OBJECT_0)
    {
        cancelAndDrain(pipe, overlapped, operationEvent);
        setLastError(ERROR_OPERATION_ABORTED);
        return false;
    }
    if (waitResult == WAIT_TIMEOUT)
    {
        cancelAndDrain(pipe, overlapped, operationEvent);
        setLastError(ERROR_SEM_TIMEOUT);
        return false;
    }
    if (waitResult != WAIT_OBJECT_0 + 1U)
    {
        cancelAndDrain(pipe, overlapped, operationEvent);
        setLastError(waitResult == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE);
        return false;
    }

    if (GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) == FALSE)
    {
        const DWORD error = GetLastError();
        if (!(error == ERROR_OPERATION_ABORTED && stopRequested()))
        {
            setLastError(error);
        }
        return false;
    }
    return true;
}

void NamedPipeIpcServer::cancelAndDrain(
    const HANDLE pipe,
    OVERLAPPED& overlapped,
    const HANDLE operationEvent) noexcept
{
    static_cast<void>(CancelIoEx(pipe, &overlapped));
    // Cancellation completes the overlapped request and signals its event.
    // Waiting here keeps the event and pipe lifetime ordered before returning.
    static_cast<void>(WaitForSingleObject(operationEvent, INFINITE));
}

void NamedPipeIpcServer::waitForShutdownClientCompletion(
    const HANDLE pipe) noexcept
{
    std::array<char, 1U> ignored{};
    UniqueHandle operationEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (operationEvent.get() == nullptr)
    {
        return;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = operationEvent.get();
    DWORD transferred = 0U;
    const BOOL read = ReadFile(
        pipe,
        ignored.data(),
        static_cast<DWORD>(ignored.size()),
        &transferred,
        &overlapped);
    if (read != FALSE)
    {
        return;
    }
    const DWORD readError = GetLastError();
    if (readError == ERROR_BROKEN_PIPE || readError == ERROR_NO_DATA)
    {
        return;
    }
    if (readError != ERROR_IO_PENDING)
    {
        return;
    }

    const HANDLE handles[] = {stopEvent_.get(), operationEvent.get()};
    const DWORD waitResult = WaitForMultipleObjects(
        2U,
        handles,
        FALSE,
        options_.ioTimeoutMilliseconds);
    if (waitResult == WAIT_OBJECT_0 + 1U)
    {
        static_cast<void>(GetOverlappedResult(
            pipe,
            &overlapped,
            &transferred,
            FALSE));
        return;
    }

    // A client that never reads or closes must delay shutdown only until the
    // existing IPC deadline. Overlapped pipe reads are cancellable here.
    cancelAndDrain(pipe, overlapped, operationEvent.get());
}

void NamedPipeIpcServer::workerMain(UniqueHandle pipe) noexcept
{
    try
    {
        while (!stopRequested())
        {
            const ConnectResult connection = waitForConnection(pipe.get());
            if (connection == ConnectResult::Stopped
                || connection == ConnectResult::Failed)
            {
                break;
            }
            if (connection == ConnectResult::Retry)
            {
                pipe.reset();
                if (stopRequested())
                {
                    break;
                }
                pipe = createPipe();
                if (pipe.get() == nullptr)
                {
                    if (isTransientPipeError(lastError()))
                    {
                        static_cast<void>(waitForStop(options_.retryDelayMilliseconds));
                        continue;
                    }
                    break;
                }
                continue;
            }

            static_cast<void>(serveClient(pipe.get()));
            if (stopRequested())
            {
                break;
            }
            static_cast<void>(DisconnectNamedPipe(pipe.get()));
            pipe.reset();
            pipe = createPipe();
            if (pipe.get() == nullptr)
            {
                if (isTransientPipeError(lastError()))
                {
                    static_cast<void>(waitForStop(options_.retryDelayMilliseconds));
                    continue;
                }
                break;
            }
        }

        if (pipe.get() != nullptr)
        {
            static_cast<void>(CancelIoEx(pipe.get(), nullptr));
            static_cast<void>(DisconnectNamedPipe(pipe.get()));
        }
    }
    catch (...)
    {
        setLastError(ERROR_UNHANDLED_EXCEPTION);
    }
    running_.store(false, std::memory_order_release);
}

bool NamedPipeIpcServer::stopRequested() const noexcept
{
    return stopRequestedFlag_.load(std::memory_order_acquire);
}

bool NamedPipeIpcServer::waitForStop(const DWORD timeout) const noexcept
{
    return stopEvent_.get() != nullptr
        && WaitForSingleObject(stopEvent_.get(), timeout) == WAIT_OBJECT_0;
}

void NamedPipeIpcServer::setLastError(const DWORD error) noexcept
{
    lastError_.store(error, std::memory_order_release);
}

}
