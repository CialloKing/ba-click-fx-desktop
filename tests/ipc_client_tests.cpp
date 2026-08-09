#include "test_support.hpp"

#include "bafx/windows/ipc.hpp"
#include "bafx/windows/ipc_client.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <string>
#include <utility>

using namespace bafx::windows;

namespace
{

[[nodiscard]] std::wstring clientTestPipeName(const std::wstring_view suffix)
{
    return L"\\\\.\\pipe\\BAFX.ClientTest."
        + std::to_wstring(GetCurrentProcessId())
        + L"."
        + std::wstring(suffix);
}

[[nodiscard]] NamedPipeIpcServer::Options clientTestServerOptions(
    const std::wstring_view suffix)
{
    NamedPipeIpcServer::Options options{};
    options.pipeName = clientTestPipeName(suffix);
    options.ioTimeoutMilliseconds = 500U;
    options.retryDelayMilliseconds = 10U;
    return options;
}

[[nodiscard]] IpcClientOptions clientTestOptions(const std::wstring& pipeName)
{
    IpcClientOptions options{};
    options.pipeName = pipeName;
    options.timeoutMilliseconds = 500U;
    return options;
}

}

BAFX_TEST(ipc_client_sends_one_utf8_record_and_parses_success)
{
    const NamedPipeIpcServer::Options serverOptions = clientTestServerOptions(L"utf8");
    std::string receivedPayload;
    NamedPipeIpcServer server(
        [&receivedPayload](const IpcRequest& request)
        {
            receivedPayload = request.payload;
            return IpcResponse::success("{\"state\":\"ready\"}");
        },
        serverOptions);

    BAFX_CHECK(server.start());
    NamedPipeIpcClient client(clientTestOptions(serverOptions.pipeName));
    const IpcClientResponse response = client.transact(
        "SetConfig {\"path\":\"effects.globalScale\",\"value\":\"\xE6\xB5\x8B\xE8\xAF\x95\"}");
    server.stop();

    BAFX_CHECK(response.transportSucceeded());
    BAFX_CHECK(response.succeeded());
    BAFX_CHECK(response.payload == "{\"state\":\"ready\"}");
    BAFX_CHECK(
        receivedPayload
        == "{\"path\":\"effects.globalScale\",\"value\":\"\xE6\xB5\x8B\xE8\xAF\x95\"}");
}

BAFX_TEST(ipc_client_preserves_host_command_error)
{
    const NamedPipeIpcServer::Options serverOptions = clientTestServerOptions(L"error");
    NamedPipeIpcServer server(
        [](const IpcRequest&)
        {
            return IpcResponse::failure(
                "generation_conflict",
                "configuration generation changed; refresh before retrying");
        },
        serverOptions);

    BAFX_CHECK(server.start());
    NamedPipeIpcClient client(clientTestOptions(serverOptions.pipeName));
    const IpcClientResponse response = client.transact("GetState");
    server.stop();

    BAFX_CHECK(response.transportSucceeded());
    BAFX_CHECK(!response.succeeded());
    BAFX_CHECK(!response.commandSucceeded);
    BAFX_CHECK(response.errorCode == "generation_conflict");
    BAFX_CHECK(
        response.errorMessage
        == "configuration generation changed; refresh before retrying");
}

BAFX_TEST(ipc_client_parses_empty_success_response)
{
    const NamedPipeIpcServer::Options serverOptions = clientTestServerOptions(L"empty-ok");
    NamedPipeIpcServer server(
        [](const IpcRequest&)
        {
            return IpcResponse::success();
        },
        serverOptions);

    BAFX_CHECK(server.start());
    NamedPipeIpcClient client(clientTestOptions(serverOptions.pipeName));
    const IpcClientResponse response = client.transact("Pause");
    server.stop();

    BAFX_CHECK(response.succeeded());
    BAFX_CHECK(response.payload.empty());
    BAFX_CHECK(response.errorCode.empty());
}

BAFX_TEST(ipc_client_rejects_multiple_records_before_connecting)
{
    IpcClientOptions options{};
    options.pipeName = clientTestPipeName(L"invalid-request");
    NamedPipeIpcClient client(std::move(options));
    const IpcClientResponse response = client.transact("GetState\nPause");

    BAFX_CHECK(response.status == IpcClientStatus::InvalidRequest);
    BAFX_CHECK(response.win32Error == ERROR_INVALID_DATA);
    BAFX_CHECK(response.errorCode == "invalid_request");
}

BAFX_TEST(ipc_client_bounds_response_bytes)
{
    const NamedPipeIpcServer::Options serverOptions = clientTestServerOptions(L"response-limit");
    NamedPipeIpcServer server(
        [](const IpcRequest&)
        {
            return IpcResponse::success(std::string(64U, 'x'));
        },
        serverOptions);

    BAFX_CHECK(server.start());
    IpcClientOptions options = clientTestOptions(serverOptions.pipeName);
    options.maxResponseBytes = 32U;
    NamedPipeIpcClient client(std::move(options));
    const IpcClientResponse response = client.transact("GetConfig");
    server.stop();

    BAFX_CHECK(response.status == IpcClientStatus::ResponseTooLarge);
    BAFX_CHECK(response.win32Error == ERROR_BUFFER_OVERFLOW);
    BAFX_CHECK(response.errorCode == "response_too_large");
}

BAFX_TEST(ipc_client_reports_response_timeout)
{
    const NamedPipeIpcServer::Options serverOptions = clientTestServerOptions(L"timeout");
    std::atomic_bool receivedRequest{false};
    NamedPipeIpcServer server(
        [&receivedRequest](const IpcRequest&)
        {
            receivedRequest.store(true, std::memory_order_release);
            Sleep(200U);
            return IpcResponse::success("{\"late\":true}");
        },
        serverOptions);

    BAFX_CHECK(server.start());
    IpcClientOptions options = clientTestOptions(serverOptions.pipeName);
    options.timeoutMilliseconds = 40U;
    NamedPipeIpcClient client(std::move(options));
    const auto started = std::chrono::steady_clock::now();
    const IpcClientResponse response = client.transact("GetState");
    const auto elapsed = std::chrono::steady_clock::now() - started;
    server.stop();

    BAFX_CHECK(receivedRequest.load(std::memory_order_acquire));
    BAFX_CHECK(response.status == IpcClientStatus::Timeout);
    BAFX_CHECK(response.win32Error == ERROR_SEM_TIMEOUT);
    BAFX_CHECK(response.errorCode == "timeout");
    BAFX_CHECK(elapsed < std::chrono::milliseconds(150));
}
