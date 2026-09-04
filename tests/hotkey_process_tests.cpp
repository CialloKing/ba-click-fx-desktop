#include "host_control.hpp"
#include "host_state.hpp"
#include "bafx/windows/ipc_client.hpp"

#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>

namespace
{
using namespace bafx::desktop;
using namespace bafx::config;
constexpr UINT modifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT;

void check(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void pump()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

template<typename Predicate>
void until(Predicate predicate, const DWORD timeout = 3'000U)
{
    const auto deadline = GetTickCount64() + timeout;
    while (!predicate() && GetTickCount64() < deadline)
    {
        pump();
        MsgWaitForMultipleObjectsEx(0U, nullptr, 10U, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    check(predicate(), "timed out waiting for native hotkey state");
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* manager = reinterpret_cast<HostHotkeys*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (manager != nullptr && manager->handleMessage(message, wParam, lParam))
    {
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

struct Fixture
{
    HWND window{nullptr};
    std::filesystem::path directory;
    bool ownsDirectory{false};
    ~Fixture()
    {
        if (window != nullptr)
        {
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            DestroyWindow(window);
        }
        std::error_code ignored;
        if (ownsDirectory)
        {
            std::filesystem::remove_all(directory, ignored);
        }
    }
};

struct Child
{
    bafx::windows::UniqueHandle ready;
    bafx::windows::UniqueHandle stop;
    bafx::windows::UniqueHandle process;
    ~Child()
    {
        if (stop.get() != nullptr)
        {
            SetEvent(stop.get());
        }
        if (process.get() != nullptr)
        {
            WaitForSingleObject(process.get(), 5'000U);
        }
    }
};

void press(const WORD key, const bool repeat = false)
{
    std::vector<INPUT> inputs;
    const auto append = [&inputs](const WORD value, const DWORD flags)
    {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = value;
        input.ki.dwFlags = flags;
        inputs.push_back(input);
    };
    append(VK_CONTROL, 0U);
    append(VK_MENU, 0U);
    append(VK_SHIFT, 0U);
    append(key, 0U);
    if (repeat)
    {
        append(key, 0U);
        append(key, 0U);
    }
    append(key, KEYEVENTF_KEYUP);
    append(VK_SHIFT, KEYEVENTF_KEYUP);
    append(VK_MENU, KEYEVENTF_KEYUP);
    append(VK_CONTROL, KEYEVENTF_KEYUP);
    check(SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) == inputs.size(),
        "SendInput unavailable in this test desktop");
}

void run()
{
    const auto suffix = std::to_wstring(GetCurrentProcessId());
    const auto readyName = L"Local\\BAFX.Hotkey.Ready." + suffix;
    const auto stopName = L"Local\\BAFX.Hotkey.Stop." + suffix;
    Child child;
    child.ready.reset(CreateEventW(nullptr, TRUE, FALSE, readyName.c_str()));
    child.stop.reset(CreateEventW(nullptr, TRUE, FALSE, stopName.c_str()));
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, 32768U);
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --occupy \""
        + readyName + L"\" \"" + stopName + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE, "unable to start key owner");
    child.process.reset(process.hProcess);
    CloseHandle(process.hThread);
    check(WaitForSingleObject(child.ready.get(), 3'000U) == WAIT_OBJECT_0, "key owner did not register F24");

    Fixture fixture;
    fixture.directory = std::filesystem::temp_directory_path() / (L"bafx-hotkey-process-" + suffix);
    fixture.ownsDirectory = std::filesystem::create_directory(fixture.directory);
    check(fixture.ownsDirectory, "test directory already exists");
    WNDCLASSW windowClass{};
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.lpszClassName = L"BAFX.Hotkey.Test";
    check(RegisterClassW(&windowClass) != 0U, "unable to register test window");
    fixture.window = CreateWindowW(windowClass.lpszClassName, L"", 0U, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, windowClass.hInstance, nullptr);
    check(fixture.window != nullptr, "unable to create test window");
    WORD spareKey = 0U;
    DWORD spareError = ERROR_SUCCESS;
    for (WORD key = 'A'; key <= 'Z'; ++key)
    {
        if (RegisterHotKey(fixture.window, 0xBFFE, modifiers | MOD_NOREPEAT, key))
        {
            UnregisterHotKey(fixture.window, 0xBFFE);
            spareKey = key;
            break;
        }
        spareError = GetLastError();
    }
    if (spareKey == 0U)
    {
        throw std::runtime_error("no available spare hotkey, Win32=" + std::to_string(spareError));
    }
    Config initial;
    initial.hotkeys.bindings[0] = HotkeyBinding{modifiers, VK_F22};
    const auto path = fixture.directory / "config.json";
    check(saveConfigAtomic(path, initial).succeeded(), "initial save failed");
    bafx::windows::NamedPipeIpcServer::Options options;
    options.pipeName = L"\\\\.\\pipe\\BAFX.HotkeyTest." + suffix;
    HostControlPlane control(path, initial, options);
    HostHotkeys manager(fixture.window, initial.hotkeys, [&control](auto action, auto epoch)
    {
        control.enqueueHotkey(action, epoch);
    });
    SetWindowLongPtrW(fixture.window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&manager));
    control.attachHotkeys(manager, fixture.window);
    struct StopControl
    {
        HostControlPlane& control;
        ~StopControl() { control.stop(); }
    } stopControl{control};
    check(manager.initialState().registeredMask == 1U, "initial F22 registration failed");
    check(control.start(false).serviceStarted, "IPC startup failed");
    bafx::windows::IpcClientOptions clientOptions;
    clientOptions.pipeName = options.pipeName;
    bafx::windows::NamedPipeIpcClient client(clientOptions);
    const auto request = [&client](const std::string& text)
    {
        auto response = std::async(std::launch::async, [&client, text]() { return client.transact(text); });
        until([&response]() { return response.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; });
        return response.get();
    };
    const auto save = [&request, &control](const HotkeysConfig& hotkeys)
    {
        return request("SetHotkeys " + std::to_string(control.snapshot().generation) + " " + toJson(hotkeys));
    };

    auto target = initial.hotkeys;
    target.bindings[1] = HotkeyBinding{modifiers, VK_F24};
    check(!save(target).succeeded(), "occupied key unexpectedly saved");
    check(loadConfig(path).config.hotkeys == initial.hotkeys, "conflict overwrote saved bindings");
    check(manager.invoke(HostHotkeys::Operation::Query).state.registeredMask == 1U, "conflict lost old registration");
    press(VK_F22, true);
    until([&control]() { return control.runtimeSnapshot().paused; });
    check(control.snapshot().generation == 2U, "NOREPEAT executed more than once");

    const auto begun = request("BeginHotkeyCapture");
    const auto recording = bafx::control_center::parseHostState(begun.payload);
    check(recording.succeeded() && recording.state->hotkeyCaptureToken != 0U, "recording did not begin");
    const auto token = recording.state->hotkeyCaptureToken;
    press(VK_F22);
    until([&manager]() { return manager.invoke(HostHotkeys::Operation::Query).state.captured.has_value(); });
    check(control.runtimeSnapshot().paused, "recording executed the old action");
    check(request("EndHotkeyCapture " + std::to_string(token)).succeeded(), "recording did not end");

    target = {};
    target.bindings[2] = initial.hotkeys.bindings[0];
    target.bindings[1] = HotkeyBinding{modifiers, VK_F23};
    const auto renderGeneration = control.runtimeSnapshot().configGeneration;
    check(save(target).succeeded(), "swap/register transaction failed");
    check(control.runtimeSnapshot().configGeneration == renderGeneration, "hotkey edit invalidated rendering");
    press(VK_F22);
    until([&control]() { return control.snapshot().activeFxProfile == "轻量"; });
    press(VK_F23);
    until([&control]() { return !control.runtimeSnapshot().config.input.trailOnlyWhilePressed; });
    check(!loadConfig(path).config.input.trailOnlyWhilePressed, "trail action was not persisted");

    bafx::windows::UniqueHandle locked(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    check(locked.get() != nullptr && locked.get() != INVALID_HANDLE_VALUE, "unable to lock config for write failure");
    auto failed = target;
    failed.bindings[0] = HotkeyBinding{modifiers, spareKey};
    const auto writeFailed = save(failed);
    check(!writeFailed.succeeded() && writeFailed.errorCode == "config_write_failed",
        "locked config did not exercise file-write rollback");
    check(control.snapshot().config.hotkeys == target, "write failure changed active config");
    check(manager.invoke(HostHotkeys::Operation::Query).state.registeredMask == 6U, "write failure lost old keys");
    locked.reset();

    check(request("BeginHotkeyCapture").succeeded(), "second capture failed");
    until([&manager]() { return manager.invoke(HostHotkeys::Operation::Query).state.captureToken == 0U; }, 6'000U);
    press(VK_F22);
    until([&control]() { return control.snapshot().activeFxProfile == "纯点击"; });
    target.bindings[3] = HotkeyBinding{modifiers, spareKey};
    const auto exitSaved = save(target);
    if (!exitSaved.succeeded())
    {
        throw std::runtime_error("exit binding save failed: " + exitSaved.errorCode + ": " + exitSaved.errorMessage);
    }
    press(spareKey);
    until([&control]() { return control.runtimeSnapshot().shutdownRequested; });
    std::cout << "PASS: external collision, native WM_HOTKEY/NOREPEAT, capture suppression/expiry, "
        "atomic rollback, action reassignment, persistence, render generation and shutdown\n";
}
}

int wmain(const int argc, wchar_t** argv)
{
    if (argc == 4 && std::wstring_view(argv[1]) == L"--occupy")
    {
        bafx::windows::UniqueHandle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[2]));
        bafx::windows::UniqueHandle stop(OpenEventW(SYNCHRONIZE, FALSE, argv[3]));
        if (ready.get() == nullptr || stop.get() == nullptr
            || !RegisterHotKey(nullptr, 1, modifiers | MOD_NOREPEAT, VK_F24))
        {
            return 2;
        }
        SetEvent(ready.get());
        WaitForSingleObject(stop.get(), 30'000U);
        UnregisterHotKey(nullptr, 1);
        return 0;
    }
    try
    {
        run();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
