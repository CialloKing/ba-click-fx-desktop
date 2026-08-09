#pragma once

#include "bafx/windows/ipc.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace bafx::windows
{

inline constexpr DWORD kDefaultIpcClientTimeoutMilliseconds = 1'000U;

enum class IpcClientStatus : std::uint8_t
{
    Ok,
    InvalidOptions,
    InvalidRequest,
    ConnectFailed,
    Timeout,
    WriteFailed,
    ReadFailed,
    ResponseTooLarge,
    InvalidResponse,
    InternalError
};

struct IpcClientOptions final
{
    std::wstring pipeName{kDefaultIpcPipeName};
    std::size_t maxRequestBytes{kDefaultIpcMaxRequestBytes};
    std::size_t maxResponseBytes{kDefaultIpcMaxResponseBytes};
    DWORD timeoutMilliseconds{kDefaultIpcClientTimeoutMilliseconds};
};

// A transport-successful response can still contain an ERR record from Host.
// Call succeeded() when a command-level success is required.
struct IpcClientResponse final
{
    IpcClientStatus status{IpcClientStatus::InternalError};
    DWORD win32Error{ERROR_SUCCESS};
    bool commandSucceeded{false};
    std::string payload{};
    std::string errorCode{};
    std::string errorMessage{};

    [[nodiscard]] bool transportSucceeded() const noexcept
    {
        return status == IpcClientStatus::Ok;
    }

    [[nodiscard]] bool succeeded() const noexcept
    {
        return transportSucceeded() && commandSucceeded;
    }
};

// Each transaction opens one local pipe connection, sends exactly one UTF-8
// command record, and reads exactly one response record. The request must not
// include a line terminator because transact() supplies the framing newline.
class NamedPipeIpcClient final
{
public:
    explicit NamedPipeIpcClient(IpcClientOptions options = {});

    [[nodiscard]] IpcClientResponse transact(std::string_view request) const noexcept;

private:
    IpcClientOptions options_{};
};

}
