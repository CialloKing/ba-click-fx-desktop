#include "test_support.hpp"

#include "bafx/windows/ipc.hpp"

#include <windows.h>

#include <array>
#include <chrono>
#include <string>

using namespace bafx::windows;

namespace
{

[[nodiscard]] std::wstring testPipeName()
{
    return L"\\\\.\\pipe\\BAFX.Test." + std::to_wstring(GetCurrentProcessId());
}

}

BAFX_TEST(ipc_parser_accepts_supported_commands)
{
    const IpcParseResult getState = parseIpcRequest("GetState");
    BAFX_CHECK(getState.succeeded());
    BAFX_CHECK(getState.request->command == IpcCommand::GetState);
    BAFX_CHECK(getState.request->payload.empty());

    const IpcParseResult setConfig = parseIpcRequest(
        "SetConfig {\"generation\":428,\"path\":\"trail.width\"}");
    BAFX_CHECK(setConfig.succeeded());
    BAFX_CHECK(setConfig.request->command == IpcCommand::SetConfig);
    BAFX_CHECK(
        setConfig.request->payload
        == "{\"generation\":428,\"path\":\"trail.width\"}");

    const IpcParseResult shutdown = parseIpcRequest("Shutdown");
    BAFX_CHECK(shutdown.succeeded());
    BAFX_CHECK(shutdown.request->command == IpcCommand::Shutdown);
}

BAFX_TEST(ipc_parser_rejects_invalid_payload_shapes)
{
    const IpcParseResult missing = parseIpcRequest("SetConfig");
    BAFX_CHECK(!missing.succeeded());
    BAFX_CHECK(missing.errorCode == "missing_payload");

    const IpcParseResult unexpected = parseIpcRequest("Pause now");
    BAFX_CHECK(!unexpected.succeeded());
    BAFX_CHECK(unexpected.errorCode == "unexpected_payload");

    const IpcParseResult unknown = parseIpcRequest("Reset");
    BAFX_CHECK(!unknown.succeeded());
    BAFX_CHECK(unknown.errorCode == "unknown_command");

    const IpcParseResult injected = parseIpcRequest("GetState\nSetConfig x");
    BAFX_CHECK(!injected.succeeded());
    BAFX_CHECK(injected.errorCode == "invalid_request");

    const IpcParseResult nul = parseIpcRequest(std::string("GetState\0x", 10U));
    BAFX_CHECK(!nul.succeeded());
    BAFX_CHECK(nul.errorCode == "invalid_request");
}

BAFX_TEST(ipc_response_is_single_line_and_has_stable_prefix)
{
    const std::string success = serializeIpcResponse(
        IpcResponse::success("{\"enabled\":true}"));
    BAFX_CHECK(success == "OK {\"enabled\":true}\n");

    IpcResponse failure = IpcResponse::failure(
        "bad_request",
        "line one\nline two");
    const std::string serialized = serializeIpcResponse(failure);
    BAFX_CHECK(serialized == "ERR bad_request line one line two\n");
    BAFX_CHECK(serialized.find('\n') == serialized.size() - 1U);
}

BAFX_TEST(ipc_server_starts_and_stops_without_a_client)
{
    NamedPipeIpcServer::Options options{};
    options.pipeName = testPipeName();
    options.ioTimeoutMilliseconds = 100U;
    options.retryDelayMilliseconds = 10U;
    NamedPipeIpcServer server(
        [](const IpcRequest&) {
            return IpcResponse::success();
        },
        options);

    BAFX_CHECK(server.start());
    BAFX_CHECK(server.running());
    const auto started = std::chrono::steady_clock::now();
    server.stop();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    BAFX_CHECK(!server.running());
    BAFX_CHECK(elapsed < std::chrono::seconds(2));
}

BAFX_TEST(ipc_server_round_trips_a_request)
{
    NamedPipeIpcServer::Options options{};
    options.pipeName = testPipeName() + L".roundtrip";
    options.ioTimeoutMilliseconds = 500U;
    options.retryDelayMilliseconds = 10U;
    NamedPipeIpcServer server(
        [](const IpcRequest& request) {
            if (request.command == IpcCommand::GetState)
            {
                return IpcResponse::success("{\"enabled\":true}");
            }
            return IpcResponse::failure("unexpected", "test command");
        },
        options);

    BAFX_CHECK(server.start());
    BAFX_CHECK(WaitNamedPipeW(options.pipeName.c_str(), 1'000U) != FALSE);
    const HANDLE client = CreateFileW(
        options.pipeName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0U,
        nullptr,
        OPEN_EXISTING,
        0U,
        nullptr);
    BAFX_CHECK(client != INVALID_HANDLE_VALUE);

    const std::string request = "GetState\nGetState\n";
    DWORD written = 0U;
    BAFX_CHECK(WriteFile(
        client,
        request.data(),
        static_cast<DWORD>(request.size()),
        &written,
        nullptr) != FALSE);
    BAFX_CHECK(written == request.size());

    std::array<char, 256U> responseBuffer{};
    std::string responses;
    DWORD available = 0U;
    bool received = false;
    DWORD receivedBytes = 0U;
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        if (PeekNamedPipe(
                client,
                nullptr,
                0U,
                nullptr,
                &available,
                nullptr) != FALSE
            && available > 0U)
        {
            BAFX_CHECK(ReadFile(
                client,
                responseBuffer.data(),
                static_cast<DWORD>(responseBuffer.size()),
                &receivedBytes,
                nullptr) != FALSE);
            responses.append(responseBuffer.data(), receivedBytes);
            received = responses.find("OK {\"enabled\":true}\nOK {\"enabled\":true}\n")
                != std::string::npos;
            if (received)
            {
                break;
            }
        }
        Sleep(5U);
    }
    CloseHandle(client);
    server.stop();

    BAFX_CHECK(received);
    BAFX_CHECK(
        responses == "OK {\"enabled\":true}\nOK {\"enabled\":true}\n");
}
