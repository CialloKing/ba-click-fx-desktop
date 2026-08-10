#include "bafx/windows/ipc_client.hpp"

#include <windows.h>
#include <shellapi.h>

#undef GetCurrentTime

#include <winrt/base.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace
{

namespace controls = winrt::Microsoft::UI::Xaml::Controls;
namespace xaml = winrt::Microsoft::UI::Xaml;
namespace json = winrt::Windows::Data::Json;

constexpr std::wstring_view controlCenterMutexName = L"Local\\BAFX.ControlCenter.v1";
constexpr std::wstring_view controlCenterWindowTitle = L"BAFX Control Center";

struct PendingPatch final
{
    std::string path{};
    std::string valueJson{};
};

[[nodiscard]] std::wstring utf8ToWide(const std::string_view value)
{
    if (value.empty())
    {
        return {};
    }

    const int count = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (count <= 0)
    {
        return L"无法解码来自 Host 的 UTF-8 文本";
    }

    std::wstring result(static_cast<std::size_t>(count), L'\0');
    static_cast<void>(MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        count));
    return result;
}

[[nodiscard]] std::string numberJson(const double value)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(3) << value;
    return stream.str();
}

[[nodiscard]] std::string patchRequest(
    const std::uint64_t generation,
    const std::string_view path,
    const std::string_view valueJson)
{
    return "SetConfig {\"generation\":" + std::to_string(generation)
        + ",\"path\":\"" + std::string(path)
        + "\",\"value\":" + std::string(valueJson) + "}";
}

[[nodiscard]] std::filesystem::path executableDirectory()
{
    std::array<wchar_t, 32'768U> buffer{};
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0U || length >= buffer.size())
    {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
}

void recordStartupFailure(const winrt::hstring& message) noexcept
{
    OutputDebugStringW(message.c_str());
    OutputDebugStringW(L"\n");

    try
    {
        // The Control Center can fail before its own UI is available. Keep the
        // failure next to the executable so a portable test bundle is diagnosable.
        std::wofstream stream(executableDirectory() / L"BAFX.ControlCenter.startup-error.log");
        stream << message.c_str() << L'\n';
    }
    catch (...)
    {
        // Startup diagnostics must never hide the original initialization error.
    }
}

[[nodiscard]] winrt::hstring describeStartupFailure(
    const winrt::hresult_error& error)
{
    std::wostringstream stream;
    stream << L"WinUI startup failed. HRESULT: 0x"
           << std::uppercase << std::hex
           << static_cast<std::uint32_t>(error.code())
           << L"\n" << error.message().c_str();
    return winrt::hstring(stream.str());
}

[[nodiscard]] bool activateExistingControlCenter()
{
    const HWND existing = FindWindowW(nullptr, controlCenterWindowTitle.data());
    if (existing == nullptr)
    {
        return false;
    }
    static_cast<void>(ShowWindow(existing, SW_RESTORE));
    static_cast<void>(SetForegroundWindow(existing));
    return true;
}

class ProcessMutex final
{
public:
    ProcessMutex() = default;

    ~ProcessMutex()
    {
        if (handle_ != nullptr)
        {
            CloseHandle(handle_);
        }
    }

    ProcessMutex(const ProcessMutex&) = delete;
    ProcessMutex& operator=(const ProcessMutex&) = delete;

    [[nodiscard]] bool acquire()
    {
        handle_ = CreateMutexW(nullptr, TRUE, controlCenterMutexName.data());
        if (handle_ == nullptr)
        {
            return false;
        }
        return GetLastError() != ERROR_ALREADY_EXISTS;
    }

private:
    HANDLE handle_{nullptr};
};

class ControlCenterApplication
    : public xaml::ApplicationT<
          ControlCenterApplication,
          xaml::Markup::IXamlMetadataProvider>
{
public:
    ControlCenterApplication()
    {
        // The code-only Control Center has no XAML compiler output. WinUI's
        // stock resource dictionary still resolves its control types through
        // this provider, so forward metadata to the SDK implementation.
        xamlMetadataProvider_.Initialize();
    }

    [[nodiscard]] xaml::Markup::IXamlType GetXamlType(
        const winrt::Windows::UI::Xaml::Interop::TypeName& type)
    {
        return xamlMetadataProvider_.GetXamlType(type);
    }

    [[nodiscard]] xaml::Markup::IXamlType GetXamlType(
        const winrt::hstring& fullName)
    {
        return xamlMetadataProvider_.GetXamlType(fullName);
    }

    [[nodiscard]] winrt::com_array<xaml::Markup::XmlnsDefinition>
        GetXmlnsDefinitions()
    {
        return xamlMetadataProvider_.GetXmlnsDefinitions();
    }

    void OnLaunched(const xaml::LaunchActivatedEventArgs&)
    {
        try
        {
            createWindow();
            if (!refreshFromHost())
            {
                scheduleHostRefreshRetry();
            }
            window_.Activate();
        }
        catch (const winrt::hresult_error& error)
        {
            recordStartupFailure(L"OnLaunched: " + describeStartupFailure(error));
            Exit();
        }
    }

private:
    void createWindow()
    {
        window_ = xaml::Window();
        window_.Title(controlCenterWindowTitle);

        // Creating the top-level window establishes the unpackaged WinUI
        // resource context used by the controls' default template dictionary.
        const controls::XamlControlsResources resources{};
        Resources().MergedDictionaries().Append(resources);
        window_.Closed(
            [this](const auto&, const auto&)
            {
                Exit();
            });

        const controls::ScrollViewer scrollViewer{};
        scrollViewer.VerticalScrollBarVisibility(controls::ScrollBarVisibility::Auto);
        const controls::StackPanel root{};
        root.Padding(xaml::Thickness{28.0});
        root.Spacing(16.0);

        const controls::TextBlock title{};
        title.Text(L"BAFX Desktop");
        title.FontSize(28.0);
        root.Children().Append(title);

        statusText_ = controls::TextBlock();
        statusText_.Text(L"正在连接 Host...");
        root.Children().Append(statusText_);

        infoBar_ = controls::InfoBar();
        infoBar_.IsOpen(false);
        infoBar_.IsClosable(true);
        root.Children().Append(infoBar_);

        appendStatusSection(root);
        appendEffectsSection(root);
        appendBackgroundSection(root);
        appendCommandSection(root);

        scrollViewer.Content(root);
        window_.Content(scrollViewer);

        patchCommitTimer_ = xaml::DispatcherTimer();
        patchCommitTimer_.Interval(std::chrono::milliseconds(120));
        patchCommitTimer_.Tick(
            [this](const auto&, const auto&)
            {
                commitPendingPatch();
            });

        hostRetryTimer_ = xaml::DispatcherTimer();
        hostRetryTimer_.Interval(std::chrono::milliseconds(250));
        hostRetryTimer_.Tick(
            [this](const auto&, const auto&)
            {
                if (refreshFromHost() || hostRetryAttempts_ == 0U)
                {
                    hostRetryTimer_.Stop();
                    return;
                }
                --hostRetryAttempts_;
            });
    }

    void appendStatusSection(const controls::StackPanel& root)
    {
        appendSectionTitle(root, L"状态");

        pauseButton_ = controls::Button();
        pauseButton_.Content(winrt::box_value(L"暂停特效"));
        pauseButton_.Click(
            [this](const auto&, const auto&)
            {
                sendCommand(paused_ ? "Resume" : "Pause");
            });
        root.Children().Append(pauseButton_);
    }

    void appendEffectsSection(const controls::StackPanel& root)
    {
        appendSectionTitle(root, L"特效");

        effectsEnabled_ = controls::ToggleSwitch();
        effectsEnabled_.Header(winrt::box_value(L"启用特效"));
        effectsEnabled_.Toggled(
            [this](const auto&, const auto&)
            {
                if (!updatingControls_)
                {
                    applyPatch(
                        "effects.enabled",
                        effectsEnabled_.IsOn() ? "true" : "false");
                }
            });
        root.Children().Append(effectsEnabled_);

        clickEnabled_ = controls::ToggleSwitch();
        clickEnabled_.Header(winrt::box_value(L"点击特效"));
        clickEnabled_.Toggled(
            [this](const auto&, const auto&)
            {
                if (!updatingControls_)
                {
                    applyPatch(
                        "effects.clickEnabled",
                        clickEnabled_.IsOn() ? "true" : "false");
                }
            });
        root.Children().Append(clickEnabled_);

        trailEnabled_ = controls::ToggleSwitch();
        trailEnabled_.Header(winrt::box_value(L"鼠标拖尾"));
        trailEnabled_.Toggled(
            [this](const auto&, const auto&)
            {
                if (!updatingControls_)
                {
                    applyPatch(
                        "effects.trailEnabled",
                        trailEnabled_.IsOn() ? "true" : "false");
                }
            });
        root.Children().Append(trailEnabled_);

        globalScale_ = appendNumberSlider(
            root,
            L"效果大小",
            0.1,
            4.0,
            0.05,
            "effects.globalScale");
        trailLength_ = appendNumberSlider(
            root,
            L"拖尾长度",
            0.0,
            3.0,
            0.05,
            "effects.trailLength");
        trailWidth_ = appendNumberSlider(
            root,
            L"拖尾宽度",
            0.1,
            4.0,
            0.05,
            "effects.trailWidth");
        bloomIntensity_ = appendNumberSlider(
            root,
            L"Bloom 强度",
            0.0,
            8.0,
            0.05,
            "effects.bloomIntensity");

        bloomQuality_ = controls::ComboBox();
        bloomQuality_.Header(winrt::box_value(L"Bloom 质量"));
        appendComboItem(bloomQuality_, L"低");
        appendComboItem(bloomQuality_, L"中");
        appendComboItem(bloomQuality_, L"高");
        appendComboItem(bloomQuality_, L"极高");
        bloomQuality_.SelectionChanged(
            [this](const auto&, const auto&)
            {
                if (updatingControls_)
                {
                    return;
                }

                switch (bloomQuality_.SelectedIndex())
                {
                case 0:
                    applyPatch("effects.bloomQuality", "\"low\"");
                    break;
                case 1:
                    applyPatch("effects.bloomQuality", "\"medium\"");
                    break;
                case 2:
                    applyPatch("effects.bloomQuality", "\"high\"");
                    break;
                case 3:
                    applyPatch("effects.bloomQuality", "\"ultra\"");
                    break;
                default:
                    setError(L"未知的 Bloom 质量选择");
                    break;
                }
            });
        root.Children().Append(bloomQuality_);
    }

    void appendBackgroundSection(const controls::StackPanel& root)
    {
        appendSectionTitle(root, L"背景感知");

        backgroundMode_ = controls::ComboBox();
        backgroundMode_.Header(winrt::box_value(L"渲染模式"));
        appendComboItem(backgroundMode_, L"仅特效");
        appendComboItem(backgroundMode_, L"背景感知");
        appendComboItem(backgroundMode_, L"录制兼容");
        backgroundMode_.SelectionChanged(
            [this](const auto&, const auto&)
            {
                if (updatingControls_)
                {
                    return;
                }

                switch (backgroundMode_.SelectedIndex())
                {
                case 0:
                    applyPatch("background.mode", "\"fx-only\"");
                    break;
                case 1:
                    applyPatch("background.mode", "\"background-aware\"");
                    break;
                case 2:
                    applyPatch("background.mode", "\"recording-compatible\"");
                    break;
                default:
                    setError(L"未知的背景模式选择");
                    break;
                }
            });
        root.Children().Append(backgroundMode_);

        cursorExcluded_ = controls::ToggleSwitch();
        cursorExcluded_.Header(winrt::box_value(L"排除鼠标指针"));
        cursorExcluded_.Toggled(
            [this](const auto&, const auto&)
            {
                if (!updatingControls_)
                {
                    applyPatch(
                        "background.cursorExcluded",
                        cursorExcluded_.IsOn() ? "true" : "false");
                }
            });
        root.Children().Append(cursorExcluded_);
    }

    void appendCommandSection(const controls::StackPanel& root)
    {
        appendSectionTitle(root, L"主程序");

        const controls::StackPanel commands{};
        commands.Orientation(controls::Orientation::Horizontal);
        commands.Spacing(8.0);

        const controls::Button refresh{};
        refresh.Content(winrt::box_value(L"刷新状态"));
        refresh.Click(
            [this](const auto&, const auto&)
            {
                static_cast<void>(refreshFromHost());
            });
        commands.Children().Append(refresh);

        const controls::Button startHost{};
        startHost.Content(winrt::box_value(L"启动 Host"));
        startHost.Click(
            [this](const auto&, const auto&)
            {
                startHostFromBundle();
            });
        commands.Children().Append(startHost);

        root.Children().Append(commands);
    }

    static void appendSectionTitle(
        const controls::StackPanel& root,
        const winrt::hstring& text)
    {
        const controls::TextBlock heading{};
        heading.Text(text);
        heading.FontSize(18.0);
        root.Children().Append(heading);
    }

    static void appendComboItem(
        const controls::ComboBox& comboBox,
        const winrt::hstring& text)
    {
        const controls::ComboBoxItem item{};
        item.Content(winrt::box_value(text));
        comboBox.Items().Append(item);
    }

    controls::Slider appendNumberSlider(
        const controls::StackPanel& root,
        const winrt::hstring& header,
        const double minimum,
        const double maximum,
        const double step,
        const std::string_view path)
    {
        controls::Slider slider{};
        slider.Header(winrt::box_value(header));
        slider.Minimum(minimum);
        slider.Maximum(maximum);
        slider.StepFrequency(step);
        const std::string pathCopy(path);
        slider.ValueChanged(
            [this, pathCopy](const auto&, const auto& arguments)
            {
                if (!updatingControls_)
                {
                    queueNumberPatch(pathCopy, arguments.NewValue());
                }
            });
        root.Children().Append(slider);
        return slider;
    }

    void queueNumberPatch(const std::string_view path, const double value)
    {
        // Commit a previous field before switching fields so a quick change from
        // one slider to another cannot silently discard the first adjustment.
        if (pendingPatch_.has_value() && pendingPatch_->path != path)
        {
            commitPendingPatch();
        }
        pendingPatch_ = PendingPatch{std::string(path), numberJson(value)};
        patchCommitTimer_.Stop();
        patchCommitTimer_.Start();
    }

    void commitPendingPatch()
    {
        patchCommitTimer_.Stop();
        if (!pendingPatch_.has_value())
        {
            return;
        }

        PendingPatch patch = std::move(*pendingPatch_);
        pendingPatch_.reset();
        applyPatch(patch.path, patch.valueJson);
    }

    [[nodiscard]] bool refreshFromHost()
    {
        const bafx::windows::IpcClientResponse stateResponse = client_.transact("GetState");
        if (!stateResponse.succeeded())
        {
            connected_ = false;
            statusText_.Text(L"Host 未运行或控制服务不可用");
            setInfo(L"无法连接 Host", describeResponse(stateResponse));
            return false;
        }

        const bafx::windows::IpcClientResponse configResponse = client_.transact("GetConfig");
        if (!configResponse.succeeded())
        {
            connected_ = false;
            statusText_.Text(L"Host 配置读取失败");
            setError(describeResponse(configResponse));
            return false;
        }

        try
        {
            const json::JsonObject state = json::JsonObject::Parse(
                winrt::hstring(utf8ToWide(stateResponse.payload)));
            const json::JsonObject config = json::JsonObject::Parse(
                winrt::hstring(utf8ToWide(configResponse.payload)));
            const json::JsonObject effects = config.GetNamedObject(L"effects");
            const json::JsonObject background = config.GetNamedObject(L"background");

            generation_ = static_cast<std::uint64_t>(state.GetNamedNumber(L"generation"));
            paused_ = state.GetNamedBoolean(L"paused");
            const winrt::hstring captureStatus = state.GetNamedString(L"backgroundCapture");

            updatingControls_ = true;
            effectsEnabled_.IsOn(effects.GetNamedBoolean(L"enabled"));
            clickEnabled_.IsOn(effects.GetNamedBoolean(L"clickEnabled"));
            trailEnabled_.IsOn(effects.GetNamedBoolean(L"trailEnabled"));
            globalScale_.Value(effects.GetNamedNumber(L"globalScale"));
            trailLength_.Value(effects.GetNamedNumber(L"trailLength"));
            trailWidth_.Value(effects.GetNamedNumber(L"trailWidth"));
            bloomIntensity_.Value(effects.GetNamedNumber(L"bloomIntensity"));
            bloomQuality_.SelectedIndex(
                bloomQualityIndex(effects.GetNamedString(L"bloomQuality")));
            backgroundMode_.SelectedIndex(
                captureModeIndex(background.GetNamedString(L"mode")));
            cursorExcluded_.IsOn(background.GetNamedBoolean(L"cursorExcluded"));
            updatingControls_ = false;

            pauseButton_.Content(winrt::box_value(paused_ ? L"恢复特效" : L"暂停特效"));
            statusText_.Text(
                L"Host 已连接 | "
                + winrt::hstring(paused_ ? L"已暂停" : L"运行中")
                + L" | 背景采样：" + captureStatus);
            connected_ = true;
            infoBar_.IsOpen(false);
            return true;
        }
        catch (const winrt::hresult_error& error)
        {
            updatingControls_ = false;
            connected_ = false;
            statusText_.Text(L"Host 返回了无法识别的状态数据");
            setError(error.message());
            return false;
        }
    }

    void applyPatch(
        const std::string_view path,
        const std::string_view valueJson)
    {
        if (!connected_)
        {
            setInfo(L"Host 未连接", L"请先启动 Host，然后刷新状态。");
            return;
        }

        const bafx::windows::IpcClientResponse response = client_.transact(
            patchRequest(generation_, path, valueJson));
        if (response.succeeded())
        {
            static_cast<void>(refreshFromHost());
            return;
        }

        if (response.errorCode == "generation_conflict")
        {
            setInfo(L"配置已变化", L"已刷新 Host 的最新设置，请再次调整。");
            static_cast<void>(refreshFromHost());
            return;
        }
        setError(describeResponse(response));
    }

    void sendCommand(const std::string_view command)
    {
        const bafx::windows::IpcClientResponse response = client_.transact(command);
        if (!response.succeeded())
        {
            setError(describeResponse(response));
            return;
        }
        static_cast<void>(refreshFromHost());
    }

    void startHostFromBundle()
    {
        const std::filesystem::path hostPath = executableDirectory() / L"ba-click-fx-desktop.exe";
        if (!std::filesystem::is_regular_file(hostPath))
        {
            setInfo(
                L"未找到 Host",
                L"请将 Control Center 与 ba-click-fx-desktop.exe 放在同一目录。");
            return;
        }

        const HINSTANCE result = ShellExecuteW(
            nullptr,
            L"open",
            hostPath.c_str(),
            nullptr,
            hostPath.parent_path().c_str(),
            SW_SHOWNOACTIVATE);
        if (reinterpret_cast<INT_PTR>(result) <= 32)
        {
            setError(L"启动 Host 失败，ShellExecute 错误码："
                + winrt::to_hstring(reinterpret_cast<INT_PTR>(result)));
            return;
        }

        setInfo(L"正在启动 Host", L"Control Center 会在 Host 初始化完成后自动刷新。");
        scheduleHostRefreshRetry();
    }

    void scheduleHostRefreshRetry()
    {
        // The Host can be between short-lived pipe clients while rebuilding its
        // sole server instance. Retry briefly so opening the UI is not racy.
        hostRetryAttempts_ = 8U;
        hostRetryTimer_.Stop();
        hostRetryTimer_.Start();
    }

    static int captureModeIndex(const winrt::hstring& mode) noexcept
    {
        if (mode == L"fx-only")
        {
            return 0;
        }
        if (mode == L"background-aware")
        {
            return 1;
        }
        if (mode == L"recording-compatible")
        {
            return 2;
        }
        return -1;
    }

    static int bloomQualityIndex(const winrt::hstring& quality) noexcept
    {
        if (quality == L"low")
        {
            return 0;
        }
        if (quality == L"medium")
        {
            return 1;
        }
        if (quality == L"high")
        {
            return 2;
        }
        if (quality == L"ultra")
        {
            return 3;
        }
        return -1;
    }

    static winrt::hstring describeResponse(
        const bafx::windows::IpcClientResponse& response)
    {
        if (!response.errorMessage.empty())
        {
            return winrt::hstring(utf8ToWide(response.errorMessage));
        }
        if (!response.errorCode.empty())
        {
            return winrt::hstring(utf8ToWide(response.errorCode));
        }
        return L"控制服务未返回可用响应";
    }

    void setInfo(
        const winrt::hstring& title,
        const winrt::hstring& message)
    {
        infoBar_.Severity(controls::InfoBarSeverity::Informational);
        infoBar_.Title(title);
        infoBar_.Message(message);
        infoBar_.IsOpen(true);
    }

    void setError(const winrt::hstring& message)
    {
        infoBar_.Severity(controls::InfoBarSeverity::Error);
        infoBar_.Title(L"操作未完成");
        infoBar_.Message(message);
        infoBar_.IsOpen(true);
    }

    xaml::Window window_{nullptr};
    xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider xamlMetadataProvider_{};
    bafx::windows::NamedPipeIpcClient client_{};
    controls::TextBlock statusText_{nullptr};
    controls::InfoBar infoBar_{nullptr};
    controls::Button pauseButton_{nullptr};
    controls::ToggleSwitch effectsEnabled_{nullptr};
    controls::ToggleSwitch clickEnabled_{nullptr};
    controls::ToggleSwitch trailEnabled_{nullptr};
    controls::Slider globalScale_{nullptr};
    controls::Slider trailLength_{nullptr};
    controls::Slider trailWidth_{nullptr};
    controls::Slider bloomIntensity_{nullptr};
    controls::ComboBox bloomQuality_{nullptr};
    controls::ComboBox backgroundMode_{nullptr};
    controls::ToggleSwitch cursorExcluded_{nullptr};
    xaml::DispatcherTimer patchCommitTimer_{nullptr};
    xaml::DispatcherTimer hostRetryTimer_{nullptr};
    std::optional<PendingPatch> pendingPatch_{};
    std::uint64_t generation_{0U};
    std::uint32_t hostRetryAttempts_{0U};
    bool connected_{false};
    bool paused_{false};
    bool updatingControls_{false};
};

}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    ProcessMutex instanceGuard;
    if (!instanceGuard.acquire())
    {
        static_cast<void>(activateExistingControlCenter());
        return 0;
    }

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    xaml::Application::Start(
        [](const auto&)
        {
            try
            {
                winrt::make<ControlCenterApplication>();
            }
            catch (const winrt::hresult_error& error)
            {
                recordStartupFailure(L"Application factory: " + describeStartupFailure(error));
            }
        });
    return 0;
}
