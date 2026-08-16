#pragma once

#include "bafx/windows/unique_handle.hpp"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace bafx::windows
{

inline constexpr wchar_t kDefaultIpcPipeName[] =
    L"\\\\.\\pipe\\BAFX.Host.v1";
inline constexpr wchar_t kHostSingleInstanceMutexName[] =
    L"Local\\BAFX.Host.v1";

inline constexpr std::size_t kDefaultIpcMaxRequestBytes = 64U * 1024U;
inline constexpr std::size_t kDefaultIpcMaxResponseBytes = 256U * 1024U;

enum class IpcCommand : std::uint8_t
{
    GetState,
    GetConfig,
    GetFxConfig,
    SetConfig,
    SetFxParam,
    SetFxParams,
    ResetFxConfig,
    Pause,
    Resume,
    Shutdown
};

struct IpcRequest final
{
    IpcCommand command{};
    std::string payload{};
};

struct IpcParseResult final
{
    std::optional<IpcRequest> request{};
    std::string errorCode{};
    std::string errorMessage{};

    [[nodiscard]] bool succeeded() const noexcept
    {
        return request.has_value();
    }
};

// Requests are UTF-8 bytes on a single line. The command token is followed by
// one ASCII space and an opaque payload when the command needs one.
[[nodiscard]] IpcParseResult parseIpcRequest(std::string_view line);

struct IpcResponse final
{
    bool succeeded{true};
    std::string payload{};
    std::string errorCode{};
    std::string errorMessage{};
    bool closeConnection{false};
    bool stopServer{false};

    [[nodiscard]] static IpcResponse success(std::string payload = {});

    [[nodiscard]] static IpcResponse failure(
        std::string errorCode,
        std::string errorMessage);
};

// Responses are always one line. Newline characters supplied by a handler are
// replaced before they reach the pipe so a handler cannot inject extra frames.
[[nodiscard]] std::string serializeIpcResponse(const IpcResponse& response);

using IpcRequestHandler = std::function<IpcResponse(const IpcRequest&)>;

class NamedPipeIpcServer final
{
public:
    struct Options final
    {
        std::wstring pipeName{kDefaultIpcPipeName};
        std::size_t maxRequestBytes{kDefaultIpcMaxRequestBytes};
        std::size_t maxResponseBytes{kDefaultIpcMaxResponseBytes};
        DWORD ioTimeoutMilliseconds{1'000U};
        DWORD retryDelayMilliseconds{250U};
        std::uint32_t maxCommandsPerConnection{128U};
    };

    explicit NamedPipeIpcServer(IpcRequestHandler handler);

    NamedPipeIpcServer(
        IpcRequestHandler handler,
        Options options);

    ~NamedPipeIpcServer();

    NamedPipeIpcServer(const NamedPipeIpcServer&) = delete;
    NamedPipeIpcServer& operator=(const NamedPipeIpcServer&) = delete;

    // start() performs only bounded, non-blocking setup. All pipe I/O runs on
    // the service thread, so the render/message loop remains responsive.
    [[nodiscard]] bool start() noexcept;

    // stop() wakes every pending overlapped operation before joining the
    // service thread. Handlers must return promptly and must not call stop().
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    // Shutdown becomes observable only after its response has entered the
    // pipe and the client has had a bounded chance to consume it.
    [[nodiscard]] bool stopRequested() const noexcept;
    [[nodiscard]] DWORD lastError() const noexcept;

private:
    enum class ConnectResult : std::uint8_t
    {
        Connected,
        Retry,
        Stopped,
        Failed
    };

    [[nodiscard]] UniqueHandle createPipe() noexcept;
    [[nodiscard]] ConnectResult waitForConnection(HANDLE pipe) noexcept;
    [[nodiscard]] bool serveClient(HANDLE pipe) noexcept;
    [[nodiscard]] bool processLine(
        HANDLE pipe,
        std::string line,
        std::uint32_t& commandCount) noexcept;
    [[nodiscard]] bool readChunk(
        HANDLE pipe,
        std::string& chunk) noexcept;
    [[nodiscard]] bool writeAll(
        HANDLE pipe,
        std::string_view data) noexcept;
    [[nodiscard]] bool waitForOverlapped(
        HANDLE pipe,
        OVERLAPPED& overlapped,
        HANDLE operationEvent,
        DWORD& transferred) noexcept;
    void cancelAndDrain(
        HANDLE pipe,
        OVERLAPPED& overlapped,
        HANDLE operationEvent) noexcept;
    void waitForShutdownClientCompletion(HANDLE pipe) noexcept;
    void workerMain(UniqueHandle pipe) noexcept;
    [[nodiscard]] bool waitForStop(DWORD timeout) const noexcept;
    void setLastError(DWORD error) noexcept;

    mutable std::mutex lifecycleMutex_{};
    UniqueHandle stopEvent_{};
    std::thread worker_{};
    IpcRequestHandler handler_{};
    Options options_{};
    std::atomic_bool running_{false};
    std::atomic_bool stopRequestedFlag_{false};
    std::atomic<DWORD> lastError_{ERROR_SUCCESS};
};

}
