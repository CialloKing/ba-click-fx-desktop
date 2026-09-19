#include "test_support.hpp"
#include "control_center_window.hpp"
#include "control_center_layout.hpp"
#include "display_state_poller.hpp"
#include "host_snapshot_poller.hpp"
#include "product/version.hpp"

#include <commctrl.h>

#include <cstdlib>
#include <fstream>
#include <algorithm>

// Match the application's common-controls activation context. TaskDialog is
// exported by version 6 and cannot be imported by a manifest-less test runner.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace bafx::control_center
{
namespace
{
std::wstring caption(const HWND control)
{
    std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(control)) + 1U, L'\0');
    value.resize(static_cast<std::size_t>(GetWindowTextW(control, value.data(), static_cast<int>(value.size()))));
    return value;
}

// Optional artifacts render the real native controls into a bitmap in this
// test process. No desktop capture, installed Host, or user settings are used.
void writeUiBitmap(const HWND window, const std::wstring& name)
{
    std::wstring directory(32'768U, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"BAFX_UI_ARTIFACTS", directory.data(),
        static_cast<DWORD>(directory.size()));
    if (length == 0U || length >= directory.size())
    {
        return;
    }
    directory.resize(length);
    std::filesystem::create_directories(directory);
    RECT client{};
    GetClientRect(window, &client);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = client.right;
    info.bmiHeader.biHeight = -client.bottom;
    info.bmiHeader.biPlanes = 1U;
    info.bmiHeader.biBitCount = 32U;
    info.bmiHeader.biCompression = BI_RGB;
    const HDC dc = CreateCompatibleDC(nullptr);
    void* pixels = nullptr;
    const HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0U);
    BAFX_CHECK(bitmap != nullptr);
    const HGDIOBJ original = SelectObject(dc, bitmap);
    // WM_PRINT on a hidden overlapped parent offsets children by its frame.
    // Render each real child at its client coordinates to avoid that artifact.
    FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));
    wchar_t windowClass[32]{};
    GetClassNameW(window, windowClass, 32);
    const bool taskDialog = std::wstring_view(windowClass) == L"#32770";
    if (taskDialog)
    {
        PrintWindow(window, dc, PW_CLIENTONLY);
    }
    for (HWND child = GetWindow(window, GW_CHILD); child != nullptr; child = GetWindow(child, GW_HWNDNEXT))
    {
        if (taskDialog)
        {
            break;
        }
        if ((GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) == 0)
        {
            continue;
        }
        RECT bounds{};
        GetWindowRect(child, &bounds);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&bounds), 2U);
        const int saved = SaveDC(dc);
        SetViewportOrgEx(dc, bounds.left, bounds.top, nullptr);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
        RestoreDC(dc, saved);
    }
    GdiFlush();
    BITMAPFILEHEADER header{};
    header.bfType = 0x4D42U;
    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
    const DWORD bytes = static_cast<DWORD>(client.right * client.bottom * 4);
    header.bfSize = header.bfOffBits + bytes;
    std::ofstream file(std::filesystem::path(directory) / (name + L".bmp"), std::ios::binary);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    file.write(static_cast<const char*>(pixels), bytes);
    SelectObject(dc, original);
    DeleteObject(bitmap);
    DeleteDC(dc);
    BAFX_CHECK(file.good());
}

struct RefreshWrites
{
    unsigned text{0U};
    unsigned sliders{0U};
    unsigned lists{0U};
    unsigned enabled{0U};
    unsigned checks{0U};
    unsigned selections{0U};
    unsigned readPosition{0U};

    bool empty() const noexcept
    {
        return text + sliders + lists + enabled + checks + selections + readPosition == 0U;
    }
};

LRESULT CALLBACK countRefreshWrites(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR, DWORD_PTR data)
{
    auto& writes = *reinterpret_cast<RefreshWrites*>(data);
    if (message == WM_SETTEXT)
    {
        ++writes.text;
    }
    if (message == TBM_SETPOS)
    {
        ++writes.sliders;
    }
    if (message == CB_RESETCONTENT)
    {
        ++writes.lists;
    }
    if (message == WM_ENABLE)
    {
        ++writes.enabled;
    }
    if (message == BM_SETCHECK)
    {
        ++writes.checks;
    }
    if (message == CB_SETCURSEL)
    {
        ++writes.selections;
    }
    if (message == EM_SETSEL || message == EM_SCROLLCARET || message == EM_LINESCROLL)
    {
        ++writes.readPosition;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

thread_local std::vector<std::wstring> dialogTexts;
void CALLBACK captureDialog(const HWND dialog, UINT, const UINT_PTR timer, DWORD)
{
    KillTimer(dialog, timer);
    try
    {
        RedrawWindow(dialog, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        writeUiBitmap(dialog, std::wstring(L"confirmation-")
            + (currentUiLanguage() == UiLanguage::English ? L"en" : L"zh"));
    }
    catch (...)
    {
        // Never propagate a C++ exception through a native window callback.
    }
    PostMessageW(dialog, TDM_CLICK_BUTTON, IDNO, 0);
}

LRESULT CALLBACK inspectDialog(const int code, const WPARAM window, const LPARAM detail)
{
    if (code == HCBT_ACTIVATE)
    {
        const HWND dialog = reinterpret_cast<HWND>(window);
        wchar_t className[32]{};
        GetClassNameW(dialog, className, 32);
        if (std::wstring_view(className) != L"#32770")
        {
            return CallNextHookEx(nullptr, code, window, detail);
        }
        dialogTexts.push_back(caption(dialog));
        EnumChildWindows(dialog, [](const HWND child, LPARAM) -> BOOL
        {
            dialogTexts.push_back(caption(child));
            return TRUE;
        }, 0);
        // Close only our newly activated confirmation, without accepting it.
        if (SetTimer(dialog, 0xBAF0U, 100U, captureDialog) == 0U)
        {
            PostMessageW(dialog, TDM_CLICK_BUTTON, IDNO, 0);
        }
    }
    return CallNextHookEx(nullptr, code, window, detail);
}
}

struct ControlCenterUiTest
{
    static void run()
    {
        const INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
        BAFX_CHECK(InitCommonControlsEx(&controls));
        ControlCenterWindow ui(GetModuleHandleW(nullptr));
        BAFX_CHECK(ui.registerWindowClass());
        // Construct only the view, bypassing create() and all Host startup/IPC.
        ui.window_ = CreateWindowExW(0U, controlCenterWindowClassName.data(), L"BAFX UI test",
            WS_OVERLAPPEDWINDOW, 0, 0, 960, 640, nullptr, nullptr, GetModuleHandleW(nullptr), &ui);
        BAFX_CHECK(ui.window_ != nullptr);
        setUiLanguage(UiLanguage::SimplifiedChinese);
        ui.createFonts();
        BAFX_CHECK(ui.createControls());
        ui.languagePath_ = std::filesystem::temp_directory_path()
            / (L"bafx-ui-language-" + std::to_wstring(GetCurrentProcessId()));
        struct Cleanup
        {
            std::filesystem::path path;
            ~Cleanup()
            {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        } cleanup{ui.languagePath_};

        ui.config_.system.closeToTray = false;
        ui.setConnected(false);
        ui.selectPage(ControlCenterWindow::Page::System);
        BAFX_CHECK(IsWindowEnabled(ui.languageSelector_));
        BAFX_CHECK(IsWindowEnabled(ui.clearLogsButton_));
        ui.setInfo(TextId::HostDisconnected, TextId::StartHostAndRefresh);
        SetWindowTextW(ui.themeColorEdit_, L"#12ab");
        SetWindowTextW(ui.fxProfileNameEdit_, L"我的 {0} draft");
        ui.fxProfiles_ = {{"Unity 原版", true}, {"我的 {0} draft", false}};
        ui.hotkeyDraftDirty_ = true;
        ui.hotkeyDraft_.bindings[0] = bafx::config::HotkeyBinding{2U, 65U};
        const auto draft = ui.hotkeyDraft_;
        const auto generation = ui.generation_;
        SendMessageW(ui.languageSelector_, CB_SETCURSEL, 2U, 0);
        ui.onCommand(static_cast<int>(ControlCenterWindow::ControlId::Language), CBN_SELCHANGE);
        BAFX_CHECK(currentUiLanguage() == UiLanguage::English);
        BAFX_CHECK(loadLanguagePreference(ui.languagePath_) == UiLanguage::English);
        BAFX_CHECK(caption(ui.systemPageButton_) == L"System");
        BAFX_CHECK(caption(ui.messageText_).find(L"Start Host") != std::wstring::npos);
        BAFX_CHECK(caption(ui.themeColorEdit_) == L"#12ab");
        BAFX_CHECK(caption(ui.fxProfileNameEdit_) == L"我的 {0} draft");
        BAFX_CHECK(ui.hotkeyDraft_ == draft && ui.hotkeyDraftDirty_);
        BAFX_CHECK(ui.generation_ == generation && !ui.connected_);
        BAFX_CHECK(ui.activePage_ == ControlCenterWindow::Page::System);
        BAFX_CHECK(!IsWindowVisible(ui.window_));
        ui.hostVersionBlocked_ = true;
        ui.setConnected(false);
        BAFX_CHECK(IsWindowEnabled(ui.languageSelector_));
        BAFX_CHECK(IsWindowEnabled(ui.clearLogsButton_));

        const HANDLE locked = CreateFileW(ui.languagePath_.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        BAFX_CHECK(locked != INVALID_HANDLE_VALUE);
        SendMessageW(ui.languageSelector_, CB_SETCURSEL, 1U, 0);
        ui.changeLanguage();
        CloseHandle(locked);
        BAFX_CHECK(ui.languagePreference_ == UiLanguage::English);
        BAFX_CHECK(SendMessageW(ui.languageSelector_, CB_GETCURSEL, 0U, 0) == 2);
        BAFX_CHECK(caption(ui.messageText_).find(L"Could not save") != std::wstring::npos);
        // A Windows settings notification re-resolves only the auto preference.
        ui.languagePreference_ = UiLanguage::SimplifiedChinese;
        setUiLanguage(ui.languagePreference_);
        SendMessageW(ui.window_, WM_SETTINGCHANGE, 0U, 0);
        BAFX_CHECK(currentUiLanguage() == UiLanguage::SimplifiedChinese);
        ui.languagePreference_ = UiLanguage::System;
        SendMessageW(ui.window_, WM_SETTINGCHANGE, 0U, 0);
        BAFX_CHECK(currentUiLanguage() == resolveUiLanguage(UiLanguage::System, GetUserDefaultUILanguage()));
        ui.languagePreference_ = UiLanguage::English;
        setUiLanguage(ui.languagePreference_);

        // Seed a cached Host snapshot without making a transaction. This also
        // exercises translated built-ins while retaining their wire identities.
        HostState state{};
        state.productVersion = bafx::product::version;
        state.productVersionStatus = HostProductVersionStatus::Match;
        state.generation = 7U;
        state.backgroundCapture = "active";
        state.fxProfiles = {{"Unity 原版", true}, {"轻量", true}, {"纯点击", true}, {"纯拖尾", true}};
        state.activeFxProfile = "轻量";
        state.spout2Status = "sent";
        state.spout2Sender = "BAFX Effects";
        state.spout2Enabled = true;
        ui.hostVersionBlocked_ = false;
        ui.displayStateError_.clear();
        ui.displayState_.topologyStatus = DisplayTopologyState::Complete;
        DisplaySessionState display{};
        display.monitor = "DISPLAY1";
        display.device = "Example monitor";
        display.displayKey = "display-1";
        display.right = 1920;
        display.bottom = 1080;
        display.adapter = "Example GPU";
        display.driver = DisplayDriverState::Hardware;
        display.targetDpiX = display.targetDpiY = display.windowDpi = 96U;
        display.displayRefresh = DisplayRefreshState{144U, 1U};
        ui.displayState_.sessions = {display};
        ui.updateControls(state, ui.config_);
        ui.updateHostVersionText(state);
        {
            // Background completion preserves newer user feedback and drafts.
            HostSnapshotResult snapshot;
            snapshot.status = HostSnapshotStatus::Succeeded;
            snapshot.state = state;
            snapshot.config = ui.config_;
            ui.refreshInfoRevision_ = ui.infoRevision_;
            ui.setError(TextId::InvalidHotkeyConfig);
            const auto feedback = caption(ui.messageText_);
            ui.invalidateHostRefresh();
            ui.updateFxProfileActionState();
            BAFX_CHECK(!IsWindowEnabled(ui.applyFxProfileButton_));
            BAFX_CHECK(IsWindowEnabled(ui.languageSelector_));
            ui.acceptHostSnapshot(snapshot);
            BAFX_CHECK(ui.hostSnapshotCurrent_ && ui.connected_);
            BAFX_CHECK(caption(ui.messageText_) == feedback);
            BAFX_CHECK(ui.hotkeyDraft_ == draft && ui.hotkeyDraftDirty_);
            BAFX_CHECK(caption(ui.themeColorEdit_) == L"#12ab");

            DisplayStatePollResult displaySnapshot;
            displaySnapshot.generation = state.generation;
            displaySnapshot.response.status = bafx::windows::IpcClientStatus::Ok;
            displaySnapshot.response.commandSucceeded = true;
            displaySnapshot.parsed.state = ui.displayState_;
            // Pausing advances the mutation generation independently of the
            // display configuration generation; both snapshots remain valid.
            displaySnapshot.parsed.state->configGeneration = 1U;
            ui.displayState_ = {};
            ui.updateDisplayControls(ui.config_);
            BAFX_CHECK(!IsWindowEnabled(ui.displaySelector_));
            BAFX_CHECK(ui.acceptDisplayStateResponse(displaySnapshot));
            BAFX_CHECK(IsWindowEnabled(ui.displaySelector_));
            BAFX_CHECK(IsWindowEnabled(ui.displayIndependent_));
            DisplayStatePollResult failedDisplay;
            failedDisplay.generation = state.generation - 1U;
            BAFX_CHECK(!ui.acceptDisplayStateResponse(failedDisplay));
            BAFX_CHECK(IsWindowEnabled(ui.displayIndependent_));
            failedDisplay.generation = state.generation;
            BAFX_CHECK(!ui.acceptDisplayStateResponse(failedDisplay));
            BAFX_CHECK(!IsWindowEnabled(ui.displayIndependent_));
            BAFX_CHECK(ui.displayState_.sessions.size() == 1U);
            BAFX_CHECK(ui.acceptDisplayStateResponse(displaySnapshot));
            ui.clearInfo();
        }
        {
            RefreshWrites writes;
            for (HWND child = GetWindow(ui.window_, GW_CHILD); child != nullptr; child = GetWindow(child, GW_HWNDNEXT))
            {
                BAFX_CHECK(SetWindowSubclass(child, countRefreshWrites, 1U, reinterpret_cast<DWORD_PTR>(&writes)));
            }
            ui.updateControls(state, ui.config_);
            BAFX_CHECK(writes.empty());
            auto changed = ui.config_;
            changed.effects.opacity = 0.5F;
            ui.updateControls(state, changed);
            BAFX_CHECK(writes.sliders == 1U && writes.lists == 0U);

            SetWindowTextW(ui.themeColorEdit_, L"#12ab");
            auto& slider = ui.*ui.sliderDescriptors().front().control;
            SendMessageW(slider.trackbar, TBM_SETPOS, TRUE, 3);
            ui.pendingPatch_ = ControlCenterWindow::PendingPatch{state.generation, slider.path, "3"};
            ui.updateControls(state, changed);
            BAFX_CHECK(caption(ui.themeColorEdit_) == L"#12ab");
            BAFX_CHECK(SendMessageW(slider.trackbar, TBM_GETPOS, 0U, 0) == 3);
            ui.pendingPatch_.reset();
            // Even with the same Host snapshot, repair a rejected optimistic
            // control value once it is no longer an uncommitted local edit.
            writes = {};
            ui.updateControls(state, changed);
            BAFX_CHECK(writes.sliders == 1U && writes.lists == 0U);
            for (HWND child = GetWindow(ui.window_, GW_CHILD); child != nullptr; child = GetWindow(child, GW_HWNDNEXT))
            {
                RemoveWindowSubclass(child, countRefreshWrites, 1U);
            }
        }
        SetWindowTextW(ui.themeColorEdit_, L"#12ab");
        SetWindowTextW(ui.fxProfileNameEdit_, L"我的 {0} draft");
        setUiLanguage(UiLanguage::SimplifiedChinese);
        ui.retranslateUi();
        BAFX_CHECK(ui.selectedFxProfile()->name == "轻量");
        BAFX_CHECK(ui.selectedDisplaySession()->displayKey == "display-1");
        BAFX_CHECK(caption(ui.statusText_).find(L"Host 已连接") != std::wstring::npos);
        BAFX_CHECK(caption(ui.themeColorEdit_) == L"#12ab");
        BAFX_CHECK(caption(ui.fxProfileNameEdit_) == L"我的 {0} draft");
        BAFX_CHECK(ui.hotkeyDraft_ == draft && ui.hotkeyDraftDirty_);
        BAFX_CHECK(ui.generation_ == 7U);
        setUiLanguage(UiLanguage::English);
        ui.retranslateUi();
        BAFX_CHECK(caption(ui.fxProfileSelector_) == L"Lightweight");
        BAFX_CHECK(caption(ui.statusText_).find(L"Host connected") != std::wstring::npos);

        // Native combo selection painting requires a visible ancestor. Keep
        // the test view off-screen and out of the taskbar while rendering it.
        SetWindowLongPtrW(ui.window_, GWL_EXSTYLE, WS_EX_TOOLWINDOW);
        SetWindowPos(ui.window_, nullptr, -30'000, -30'000, 0, 0,
            SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);

        ui.activePage_ = ControlCenterWindow::Page::DisplayPerformance;
        ui.updatePageVisibility();
        ui.layoutControls(ui.scale(860), ui.scale(600));
        for (const HWND details : {ui.displayDetailsText_, ui.activeFxRoiDetailsText_})
        {
            SendMessageW(details, EM_SETSEL, 6U, 12);
            SendMessageW(details, EM_LINESCROLL, 0U, 8);
            const auto firstLine = SendMessageW(details, EM_GETFIRSTVISIBLELINE, 0U, 0);
            BAFX_CHECK(firstLine > 0);
            const auto verifyReadPosition = [&]
            {
                DWORD first = 0U;
                DWORD last = 0U;
                SendMessageW(details, EM_GETSEL, reinterpret_cast<WPARAM>(&first),
                    reinterpret_cast<LPARAM>(&last));
                BAFX_CHECK(first == 6U && last == 12U);
                BAFX_CHECK(SendMessageW(details, EM_GETFIRSTVISIBLELINE, 0U, 0) == firstLine);
                BAFX_CHECK(IsWindowVisible(details));
            };
            ui.updateDisplayDetails();
            verifyReadPosition();
            // Updated diagnostics must preserve the view too, not just a
            // byte-identical snapshot that skips WM_SETTEXT altogether.
            ui.displayState_.sessions.front().backgroundCaptureFailure += " changed";
            ++ui.displayState_.sessions.front().activeFxRoi.sampleAgeMs;
            ui.updateDisplayDetails();
            verifyReadPosition();
        }

        for (const UINT dpi : {96U, 144U, 192U})
        {
            ui.layoutDpi_ = dpi;
            ui.createFonts();
            RECT size{0, 0, ui.scale(minimumControlCenterClientWidth), ui.scale(minimumControlCenterClientHeight)};
            AdjustWindowRectEx(&size, WS_OVERLAPPEDWINDOW, FALSE, 0U);
            SetWindowPos(ui.window_, nullptr, 0, 0, size.right - size.left, size.bottom - size.top,
                SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
            for (const auto language : {UiLanguage::SimplifiedChinese, UiLanguage::English})
            {
                setUiLanguage(language);
                ui.languagePreference_ = language;
                SendMessageW(ui.languageSelector_, CB_SETCURSEL, static_cast<WPARAM>(language), 0);
                ui.retranslateUi();
                const HMENU tray = ui.createTrayMenu();
                BAFX_CHECK(tray != nullptr);
                wchar_t menuText[128]{};
                GetMenuStringW(tray, 0U, menuText, 128, MF_BYPOSITION);
                BAFX_CHECK(std::wstring_view(menuText) == tr(TextId::OpenControlCenter));
                DestroyMenu(tray);
                for (int page = 0; page < 5; ++page)
                {
                    // Navigation is presentation-only here; selectPage() also
                    // starts live polling, which belongs to the actual app.
                    ui.activePage_ = static_cast<ControlCenterWindow::Page>(page);
                    ui.updatePageVisibility();
                    ui.layoutControls(ui.scale(860), ui.scale(600));
                    const int sections = page == 1 ? 6 : 1;
                    for (int section = 0; section < sections; ++section)
                    {
                        if (page == 1)
                        {
                            ui.selectAdvancedSection(static_cast<ControlCenterWindow::AdvancedSection>(section));
                            ui.layoutControls(ui.scale(860), ui.scale(600));
                        }
                        RECT client{};
                        GetClientRect(ui.window_, &client);
                        for (HWND child = GetWindow(ui.window_, GW_CHILD); child != nullptr; child = GetWindow(child, GW_HWNDNEXT))
                        {
                            if ((GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) == 0)
                            {
                                continue;
                            }
                            RECT bounds{};
                            GetWindowRect(child, &bounds);
                            MapWindowPoints(nullptr, ui.window_, reinterpret_cast<POINT*>(&bounds), 2U);
                            if (bounds.left < 0 || bounds.top < 0 || bounds.right > client.right || bounds.bottom > client.bottom)
                            {
                                throw std::runtime_error("Control outside client area: id=" + std::to_string(GetDlgCtrlID(child))
                                    + ", page=" + std::to_string(page));
                            }
                        }
                        writeUiBitmap(ui.window_, std::wstring(language == UiLanguage::English ? L"en" : L"zh")
                            + L"-" + std::to_wstring(dpi) + L"-" + std::to_wstring(page) + L"-" + std::to_wstring(section));
                    }
                }
            }
        }
        ui.connected_ = false;
        for (const auto language : {UiLanguage::English, UiLanguage::SimplifiedChinese})
        {
            setUiLanguage(language);
            dialogTexts.clear();
            const HHOOK hook = SetWindowsHookExW(WH_CBT, inspectDialog, nullptr, GetCurrentThreadId());
            BAFX_CHECK(hook != nullptr);
            const int result = localizedMessageBox(ui.window_, TextId::ResetQuestion, TextId::ResetSettings,
                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            UnhookWindowsHookEx(hook);
            BAFX_CHECK(result == IDNO);
            // TaskDialog's body/buttons are DirectUI elements, not child HWND
            // captions. Their rendered text is included in the optional bitmap.
            BAFX_CHECK(std::find(dialogTexts.begin(), dialogTexts.end(), tr(TextId::ResetSettings)) != dialogTexts.end());
        }
        // Prevent the fixture destructor from contacting any real capture owner.
        ui.hotkeyDraftDirty_ = false;
        ui.setConnected(false);
        {
            RefreshWrites writes;
            for (HWND child = GetWindow(ui.window_, GW_CHILD); child != nullptr; child = GetWindow(child, GW_HWNDNEXT))
            {
                BAFX_CHECK(SetWindowSubclass(child, countRefreshWrites, 1U, reinterpret_cast<DWORD_PTR>(&writes)));
            }
            ui.setConnected(false);
            BAFX_CHECK(writes.empty());
            for (HWND child = GetWindow(ui.window_, GW_CHILD); child != nullptr; child = GetWindow(child, GW_HWNDNEXT))
            {
                RemoveWindowSubclass(child, countRefreshWrites, 1U);
            }
        }
    }
};
}

BAFX_TEST(control_center_language_switch_preserves_view_state)
{
    bafx::control_center::ControlCenterUiTest::run();
}

BAFX_TEST(control_center_ipc_diagnostics_suppress_repeated_poll_failures)
{
    const auto readLog = []()
    {
        std::ifstream input(bafx::control_center::controlCenterLogPath(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };
    bafx::windows::IpcClientOptions options;
    options.pipeName = L"\\\\.\\pipe\\BAFX.AbsentDiagnosticTest." + std::to_wstring(GetCurrentProcessId());
    options.timeoutMilliseconds = 5U;
    bafx::control_center::DiagnosticIpcClient client(options);
    BAFX_CHECK(!client.transact("GetState").succeeded());
    const auto firstFailure = readLog();
    BAFX_CHECK(firstFailure.find("IPC.Command=GetState\n") != std::string::npos);
    BAFX_CHECK(!client.transact("GetState").succeeded());
    BAFX_CHECK(readLog() == firstFailure);
    BAFX_CHECK(!client.transact("Pause").succeeded());
    const auto appended = readLog();
    BAFX_CHECK(appended.find("Event.Name=IPC.Requested") != std::string::npos);
    BAFX_CHECK(appended.find("IPC.Command=Pause\n") != std::string::npos);
    BAFX_CHECK(appended.find("IPC.TimeoutMs=5\n") != std::string::npos);
}
