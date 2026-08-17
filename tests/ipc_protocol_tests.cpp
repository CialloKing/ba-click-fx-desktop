#include "test_support.hpp"

#include "bafx/windows/ipc.hpp"
#include "bafx/windows/ipc_client.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
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

    const IpcParseResult getDisplayState = parseIpcRequest("GetDisplayState");
    BAFX_CHECK(getDisplayState.succeeded());
    BAFX_CHECK(
        getDisplayState.request->command == IpcCommand::GetDisplayState);
    BAFX_CHECK(getDisplayState.request->payload.empty());

    const IpcParseResult getConfig = parseIpcRequest("GetConfig");
    BAFX_CHECK(getConfig.succeeded());
    BAFX_CHECK(getConfig.request->command == IpcCommand::GetConfig);
    BAFX_CHECK(getConfig.request->payload.empty());

    const IpcParseResult getFxConfig = parseIpcRequest("GetFxConfig");
    BAFX_CHECK(getFxConfig.succeeded());
    BAFX_CHECK(getFxConfig.request->command == IpcCommand::GetFxConfig);
    BAFX_CHECK(getFxConfig.request->payload.empty());

    const IpcParseResult setConfig = parseIpcRequest(
        "SetConfig {\"generation\":428,\"path\":\"trail.width\"}");
    BAFX_CHECK(setConfig.succeeded());
    BAFX_CHECK(setConfig.request->command == IpcCommand::SetConfig);
    BAFX_CHECK(
        setConfig.request->payload
        == "{\"generation\":428,\"path\":\"trail.width\"}");

    const IpcParseResult setDisplay = parseIpcRequest(
        "SetDisplayOverride {\"generation\":8,\"displayKey\":\"key\"}");
    BAFX_CHECK(setDisplay.succeeded());
    BAFX_CHECK(
        setDisplay.request->command == IpcCommand::SetDisplayOverride);
    BAFX_CHECK(!setDisplay.request->payload.empty());

    const IpcParseResult removeDisplay = parseIpcRequest(
        "RemoveDisplayOverride {\"generation\":9,\"displayKey\":\"key\"}");
    BAFX_CHECK(removeDisplay.succeeded());
    BAFX_CHECK(
        removeDisplay.request->command == IpcCommand::RemoveDisplayOverride);
    BAFX_CHECK(!removeDisplay.request->payload.empty());

    const IpcParseResult setFxParam = parseIpcRequest(
        "SetFxParam {\"generation\":1,\"path\":\"effects.opacity\",\"value\":0.5}");
    BAFX_CHECK(setFxParam.succeeded());
    BAFX_CHECK(setFxParam.request->command == IpcCommand::SetFxParam);
    BAFX_CHECK(
        setFxParam.request->payload
        == "{\"generation\":1,\"path\":\"effects.opacity\",\"value\":0.5}");

    const IpcParseResult setFxParams = parseIpcRequest(
        "SetFxParams {\"generation\":1,\"patch\":{\"effects.opacity\":0.5}}");
    BAFX_CHECK(setFxParams.succeeded());
    BAFX_CHECK(setFxParams.request->command == IpcCommand::SetFxParams);
    BAFX_CHECK(
        setFxParams.request->payload
        == "{\"generation\":1,\"patch\":{\"effects.opacity\":0.5}}");

    const IpcParseResult resetFxConfig = parseIpcRequest("ResetFxConfig");
    BAFX_CHECK(resetFxConfig.succeeded());
    BAFX_CHECK(resetFxConfig.request->command == IpcCommand::ResetFxConfig);
    BAFX_CHECK(resetFxConfig.request->payload.empty());

    const IpcParseResult shutdown = parseIpcRequest("Shutdown");
    BAFX_CHECK(shutdown.succeeded());
    BAFX_CHECK(shutdown.request->command == IpcCommand::Shutdown);
}

BAFX_TEST(ipc_parser_rejects_invalid_payload_shapes)
{
    const IpcParseResult missing = parseIpcRequest("SetConfig");
    BAFX_CHECK(!missing.succeeded());
    BAFX_CHECK(missing.errorCode == "missing_payload");

    const IpcParseResult missingDisplay = parseIpcRequest(
        "SetDisplayOverride");
    BAFX_CHECK(!missingDisplay.succeeded());
    BAFX_CHECK(missingDisplay.errorCode == "missing_payload");

    const IpcParseResult missingFxParam = parseIpcRequest("SetFxParam");
    BAFX_CHECK(!missingFxParam.succeeded());
    BAFX_CHECK(missingFxParam.errorCode == "missing_payload");

    const IpcParseResult missingFxParams = parseIpcRequest("SetFxParams");
    BAFX_CHECK(!missingFxParams.succeeded());
    BAFX_CHECK(missingFxParams.errorCode == "missing_payload");

    const IpcParseResult resetWithPayload = parseIpcRequest(
        "ResetFxConfig now");
    BAFX_CHECK(!resetWithPayload.succeeded());
    BAFX_CHECK(resetWithPayload.errorCode == "unexpected_payload");

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

BAFX_TEST(ipc_server_publishes_shutdown_after_the_client_consumes_the_response)
{
    NamedPipeIpcServer::Options options{};
    options.pipeName = testPipeName() + L".shutdown";
    options.ioTimeoutMilliseconds = 500U;
    options.retryDelayMilliseconds = 10U;
    std::atomic_bool handlerReceivedShutdown{false};
    std::atomic_bool handlerObservedStop{true};
    NamedPipeIpcServer* serverAddress = nullptr;
    NamedPipeIpcServer server(
        [&](const IpcRequest& request)
        {
            handlerReceivedShutdown.store(
                request.command == IpcCommand::Shutdown,
                std::memory_order_release);
            handlerObservedStop.store(
                serverAddress->stopRequested(),
                std::memory_order_release);
            return IpcResponse::success("{\"shutdownRequested\":true}");
        },
        options);
    serverAddress = &server;
    BAFX_CHECK(server.start());

    IpcClientOptions clientOptions{};
    clientOptions.pipeName = options.pipeName;
    clientOptions.timeoutMilliseconds = 1'000U;
    const NamedPipeIpcClient client(clientOptions);
    const IpcClientResponse response = client.transact("Shutdown");

    BAFX_CHECK(response.succeeded());
    BAFX_CHECK(response.payload == "{\"shutdownRequested\":true}");
    BAFX_CHECK(handlerReceivedShutdown.load(std::memory_order_acquire));
    BAFX_CHECK(!handlerObservedStop.load(std::memory_order_acquire));
    for (std::size_t attempt = 0U;
         attempt < 200U && !server.stopRequested();
         ++attempt)
    {
        Sleep(1U);
    }
    BAFX_CHECK(server.stopRequested());
    server.stop();
}
