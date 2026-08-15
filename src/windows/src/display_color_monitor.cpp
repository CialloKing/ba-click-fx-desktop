#include "bafx/windows/display_color_monitor.hpp"

#include <windows.graphics.display.interop.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/base.h>

#include <atomic>
#include <iomanip>
#include <memory>
#include <new>
#include <sstream>

namespace bafx::windows
{
namespace
{

using winrt::Windows::Graphics::Display::DisplayInformation;
using winrt::Windows::Graphics::Display::IDisplayInformation;

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
    ~Implementation() noexcept
    {
        stop();
    }

    void stop() noexcept
    {
        if (notification != nullptr)
        {
            notification->beginStop();
        }
        if (advancedColorChangedRegistered)
        {
            displayInformation.AdvancedColorInfoChanged(
                advancedColorChangedToken);
            advancedColorChangedRegistered = false;
        }
        displayInformation = nullptr;
        notification.reset();
    }

    DisplayInformation displayInformation{nullptr};
    winrt::event_token advancedColorChangedToken{};
    std::shared_ptr<DisplayColorNotification> notification{};
    std::uint64_t observedGeneration{0U};
    bool advancedColorChangedRegistered{false};
};

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
        return DisplayColorMonitorResult{
            DisplayColorMonitorStatus::InvalidTarget,
            E_INVALIDARG,
            0U};
    }

    try
    {
        auto implementation = std::make_unique<Implementation>();
        implementation->notification =
            std::make_shared<DisplayColorNotification>(wakeWindow);

        const auto interop = winrt::get_activation_factory<
            DisplayInformation,
            IDisplayInformationStaticsInterop>();
        winrt::check_hresult(interop->GetForMonitor(
            monitor,
            winrt::guid_of<IDisplayInformation>(),
            winrt::put_abi(implementation->displayInformation)));

        const std::shared_ptr<DisplayColorNotification> notification =
            implementation->notification;
        implementation->advancedColorChangedToken =
            implementation->displayInformation.AdvancedColorInfoChanged(
                [notification](const auto&, const auto&) noexcept
                {
                    notification->notify();
                });
        implementation->advancedColorChangedRegistered = true;

        // Force the required DisplayInformation revision to be queried during
        // startup. An older runtime then takes the explicit unsupported path.
        static_cast<void>(
            implementation->displayInformation.GetAdvancedColorInfo());
        const std::uint64_t generation =
            implementation->notification->generation();
        implementation->observedGeneration = generation;
        implementation_ = std::move(implementation);
        return DisplayColorMonitorResult{
            DisplayColorMonitorStatus::Active,
            S_OK,
            generation};
    }
    catch (const winrt::hresult_error& error)
    {
        return DisplayColorMonitorResult{
            failureStatus(error.code()),
            error.code(),
            0U};
    }
    catch (const std::bad_alloc&)
    {
        return DisplayColorMonitorResult{
            DisplayColorMonitorStatus::Failed,
            E_OUTOFMEMORY,
            0U};
    }
    catch (...)
    {
        return DisplayColorMonitorResult{
            DisplayColorMonitorStatus::Failed,
            E_FAIL,
            0U};
    }
}

bool DisplayColorMonitor::notificationPending() const noexcept
{
    if (implementation_ == nullptr)
    {
        return false;
    }
    return implementation_->notification->generation()
        != implementation_->observedGeneration;
}

std::uint64_t DisplayColorMonitor::consumeNotification() noexcept
{
    if (implementation_ == nullptr)
    {
        return 0U;
    }
    const std::uint64_t generation =
        implementation_->notification->generation();
    implementation_->observedGeneration = generation;
    return generation;
}

bool DisplayColorMonitor::active() const noexcept
{
    return implementation_ != nullptr;
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
