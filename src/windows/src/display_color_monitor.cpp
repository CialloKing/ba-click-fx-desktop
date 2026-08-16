#include "bafx/windows/display_color_monitor.hpp"

#include <roapi.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <memory>
#include <new>
#include <sstream>

namespace bafx::windows
{
namespace
{

using Microsoft::WRL::ComPtr;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::Foundation::TypedEventHandler;
using winrt::Windows::Graphics::Display::DisplayInformation;
using winrt::Windows::Graphics::Display::IDisplayInformation;

constexpr std::chrono::milliseconds initialRetryDelay{1000};
constexpr std::chrono::milliseconds maximumRetryDelay{30000};
constexpr wchar_t displayInformationClassName[] =
    L"Windows.Graphics.Display.DisplayInformation";

// Per-monitor DisplayInformation interop was published with Windows 11 22H2.
// Keep its stable ABI local so older SDKs still emit the full runtime path.
MIDL_INTERFACE("7449121C-382B-4705-8DA7-A795BA482013")
DisplayInformationStaticsInteropAbi : public ::IInspectable
{
public:
    virtual HRESULT STDMETHODCALLTYPE GetForWindow(
        HWND window,
        REFIID interfaceIdentifier,
        void** displayInformation) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetForMonitor(
        HMONITOR monitor,
        REFIID interfaceIdentifier,
        void** displayInformation) = 0;
};

// Advanced Color notifications live on IDisplayInformation5. Calling the ABI
// directly keeps availability in the target OS instead of the build SDK.
MIDL_INTERFACE("3A5442DC-2CDE-4A8D-80D1-21DC5ADCC1AA")
DisplayInformation5Abi : public ::IInspectable
{
public:
    virtual HRESULT STDMETHODCALLTYPE GetAdvancedColorInfo(
        ::IInspectable** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_AdvancedColorInfoChanged(
        void* handler,
        winrt::event_token* token) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_AdvancedColorInfoChanged(
        winrt::event_token token) = 0;
};

[[nodiscard]] ComPtr<DisplayInformationStaticsInteropAbi>
getDisplayInformationInteropFactory()
{
    const winrt::hstring className{displayInformationClassName};
    ComPtr<DisplayInformationStaticsInteropAbi> factory;
    winrt::check_hresult(RoGetActivationFactory(
        reinterpret_cast<HSTRING>(winrt::get_abi(className)),
        IID_PPV_ARGS(&factory)));
    return factory;
}

class DisplayColorNotification final
{
public:
    explicit DisplayColorNotification(const HWND wakeWindow) noexcept
        : wakeWindow_(wakeWindow)
    {
    }

    void notify() noexcept
    {
        if (stopping_.load(std::memory_order_acquire))
        {
            return;
        }
        generation_.fetch_add(1U, std::memory_order_release);
        // The Host may be blocked in a message-aware frame wait. Posting a
        // no-op wakes the owner without running color queries on a WinRT thread.
        PostMessageW(wakeWindow_, WM_NULL, 0, 0);
    }

    void beginStop() noexcept
    {
        stopping_.store(true, std::memory_order_release);
    }

    [[nodiscard]] std::uint64_t generation() const noexcept
    {
        return generation_.load(std::memory_order_acquire);
    }

private:
    HWND wakeWindow_{nullptr};
    std::atomic<std::uint64_t> generation_{0U};
    std::atomic_bool stopping_{false};
};

[[nodiscard]] DisplayColorMonitorStatus failureStatus(
    const HRESULT error) noexcept
{
    if (error == E_NOTIMPL
        || error == E_NOINTERFACE
        || error == REGDB_E_CLASSNOTREG
        || error == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)
        || error == HRESULT_FROM_WIN32(ERROR_OLD_WIN_VERSION))
    {
        return DisplayColorMonitorStatus::Unsupported;
    }
    return DisplayColorMonitorStatus::Failed;
}

[[nodiscard]] std::string hexHresult(const HRESULT error)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << static_cast<unsigned long>(error);
    return stream.str();
}

}

struct DisplayColorMonitor::Implementation
{
    using Clock = std::chrono::steady_clock;

    Implementation(
        const HMONITOR targetMonitor,
        const HWND targetWakeWindow) noexcept
        : monitor(targetMonitor),
          wakeWindow(targetWakeWindow)
    {
    }

    ~Implementation() noexcept
    {
        stop();
    }

    [[nodiscard]] DisplayColorMonitorResult subscribe(
        const bool signalOwner) noexcept
    {
        stopSubscription();
        try
        {
            notification = std::make_shared<DisplayColorNotification>(
                wakeWindow);

            const auto interop = getDisplayInformationInteropFactory();
            winrt::check_hresult(interop->GetForMonitor(
                monitor,
                winrt::guid_of<IDisplayInformation>(),
                winrt::put_abi(displayInformation)));

            auto* const displayInformationUnknown =
                reinterpret_cast<IUnknown*>(
                    winrt::get_abi(displayInformation));
            winrt::check_hresult(displayInformationUnknown->QueryInterface(
                IID_PPV_ARGS(&advancedColorInformation)));

            const std::shared_ptr<DisplayColorNotification> callback =
                notification;
            const TypedEventHandler<DisplayInformation, IInspectable> handler{
                [callback](const auto&, const auto&) noexcept
                {
                    callback->notify();
                }};
            winrt::check_hresult(
                advancedColorInformation->add_AdvancedColorInfoChanged(
                    winrt::get_abi(handler),
                    &advancedColorChangedToken));
            advancedColorChangedRegistered = true;

            // Force the required DisplayInformation revision to be queried.
            // An older runtime then takes the explicit unsupported path.
            ComPtr<::IInspectable> advancedColorInfo;
            winrt::check_hresult(
                advancedColorInformation->GetAdvancedColorInfo(
                    &advancedColorInfo));
            observedGeneration = notification->generation();
            if (signalOwner)
            {
                // A recovered subscription may have missed the state change
                // that made the target valid, so force one owner-side query.
                notification->notify();
            }
            return DisplayColorMonitorResult{
                DisplayColorMonitorStatus::Active,
                S_OK,
                notification->generation()};
        }
        catch (const winrt::hresult_error& error)
        {
            stopSubscription();
            return DisplayColorMonitorResult{
                failureStatus(error.code()),
                error.code(),
                0U};
        }
        catch (const std::bad_alloc&)
        {
            stopSubscription();
            return DisplayColorMonitorResult{
                DisplayColorMonitorStatus::Failed,
                E_OUTOFMEMORY,
                0U};
        }
        catch (...)
        {
            stopSubscription();
            return DisplayColorMonitorResult{
                DisplayColorMonitorStatus::Failed,
                E_FAIL,
                0U};
        }
    }

    void updateRetry(
        const DisplayColorMonitorResult& result,
        const Clock::time_point now) noexcept
    {
        if (result.status != DisplayColorMonitorStatus::Failed)
        {
            retryScheduled = false;
            retryDelay = initialRetryDelay;
            return;
        }
        retryAt = now + retryDelay;
        retryScheduled = true;
        retryDelay = (std::min)(retryDelay * 2, maximumRetryDelay);
    }

    [[nodiscard]] bool retryDue(const Clock::time_point now) const noexcept
    {
        return retryScheduled && now >= retryAt;
    }

    [[nodiscard]] bool active() const noexcept
    {
        return advancedColorChangedRegistered
            && displayInformation != nullptr
            && notification != nullptr;
    }

    void stopSubscription() noexcept
    {
        if (notification != nullptr)
        {
            notification->beginStop();
        }
        if (advancedColorChangedRegistered)
        {
            if (advancedColorInformation != nullptr)
            {
                // A hot-unplug can invalidate DisplayInformation before its
                // owner removes the callback. Teardown is best-effort and the
                // shared notification already rejects every late callback.
                static_cast<void>(
                    advancedColorInformation->remove_AdvancedColorInfoChanged(
                        advancedColorChangedToken));
            }
            advancedColorChangedRegistered = false;
            advancedColorChangedToken = {};
        }
        advancedColorInformation.Reset();
        displayInformation = nullptr;
        notification.reset();
    }

    void stop() noexcept
    {
        retryScheduled = false;
        stopSubscription();
    }

    HMONITOR monitor{nullptr};
    HWND wakeWindow{nullptr};
    DisplayInformation displayInformation{nullptr};
    ComPtr<DisplayInformation5Abi> advancedColorInformation{};
    winrt::event_token advancedColorChangedToken{};
    std::shared_ptr<DisplayColorNotification> notification{};
    Clock::time_point retryAt{};
    std::chrono::milliseconds retryDelay{initialRetryDelay};
    std::uint64_t observedGeneration{0U};
    bool advancedColorChangedRegistered{false};
    bool retryScheduled{false};
};

DisplayColorMonitor::DisplayColorMonitor() noexcept = default;

DisplayColorMonitor::~DisplayColorMonitor() noexcept
{
    stop();
}

DisplayColorMonitorResult DisplayColorMonitor::start(
    const HMONITOR monitor,
    const HWND wakeWindow) noexcept
{
    stop();
    if (monitor == nullptr || wakeWindow == nullptr)
    {
        result_ = DisplayColorMonitorResult{
            DisplayColorMonitorStatus::InvalidTarget,
            E_INVALIDARG,
            0U};
        return result_;
    }

    try
    {
        auto implementation = std::make_unique<Implementation>(
            monitor,
            wakeWindow);
        result_ = implementation->subscribe(false);
        implementation->updateRetry(
            result_,
            Implementation::Clock::now());
        implementation_ = std::move(implementation);
        return result_;
    }
    catch (const std::bad_alloc&)
    {
        result_ = DisplayColorMonitorResult{
            DisplayColorMonitorStatus::Failed,
            E_OUTOFMEMORY,
            0U};
        return result_;
    }
    catch (...)
    {
        result_ = DisplayColorMonitorResult{
            DisplayColorMonitorStatus::Failed,
            E_FAIL,
            0U};
        return result_;
    }
}

bool DisplayColorMonitor::notificationPending() noexcept
{
    if (implementation_ == nullptr)
    {
        return false;
    }
    const Implementation::Clock::time_point now =
        Implementation::Clock::now();
    if (implementation_->retryDue(now))
    {
        result_ = implementation_->subscribe(true);
        implementation_->updateRetry(result_, now);
    }
    if (!implementation_->active())
    {
        return false;
    }
    return implementation_->notification->generation()
        != implementation_->observedGeneration;
}

std::uint64_t DisplayColorMonitor::consumeNotification() noexcept
{
    if (implementation_ == nullptr || !implementation_->active())
    {
        return 0U;
    }
    const std::uint64_t generation =
        implementation_->notification->generation();
    implementation_->observedGeneration = generation;
    // Support reports expose the last owner-consumed generation, not merely
    // the generation captured when the WinRT subscription was created.
    result_.generation = generation;
    return generation;
}

bool DisplayColorMonitor::active() const noexcept
{
    return implementation_ != nullptr && implementation_->active();
}

const DisplayColorMonitorResult& DisplayColorMonitor::result() const noexcept
{
    return result_;
}

void DisplayColorMonitor::stop() noexcept
{
    if (implementation_ == nullptr)
    {
        return;
    }
    implementation_->stop();
    implementation_.reset();
}

std::string_view displayColorMonitorStatusName(
    const DisplayColorMonitorStatus status) noexcept
{
    switch (status)
    {
    case DisplayColorMonitorStatus::Active:
        return "active";
    case DisplayColorMonitorStatus::InvalidTarget:
        return "invalid-target";
    case DisplayColorMonitorStatus::Unsupported:
        return "unsupported";
    case DisplayColorMonitorStatus::Failed:
        return "failed";
    }
    return "unknown";
}

std::string displayColorMonitorDiagnostic(
    const DisplayColorMonitorResult& result)
{
    std::ostringstream stream;
    stream << "Display.ColorMonitor="
           << displayColorMonitorStatusName(result.status)
           << ";HRESULT=" << hexHresult(result.error)
           << ";Generation=" << result.generation;
    return stream.str();
}

}
