#pragma once

#include "bafx/config/config.hpp"

#include <windows.h>

#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

namespace bafx::desktop
{

struct HotkeyRuntimeState final
{
    std::array<DWORD, bafx::config::hotkeyActionCount> errors{};
    std::uint32_t registeredMask{0U};
    DWORD cleanupError{ERROR_SUCCESS};
    std::uint64_t captureToken{0U};
    std::optional<bafx::config::HotkeyBinding> captured{};
};

struct HotkeyOperationResult final
{
    bool succeeded{true};
    HotkeyRuntimeState state{};
    std::string error{};
};

// Owns registrations on the HostShell thread. Requests contain shared state,
// never caller-stack pointers, so a stopped message pump cannot cause a UAF.
class HostHotkeys final
{
public:
    enum class Operation
    {
        Prepare, Commit, Rollback, Query, Retry, BeginCapture, EndCapture
    };
    using ActionSink = std::function<void(bafx::config::HotkeyAction, std::uint64_t)>;

    HostHotkeys(HWND window, const bafx::config::HotkeysConfig& initial, ActionSink sink);
    ~HostHotkeys();
    HostHotkeys(const HostHotkeys&) = delete;
    HostHotkeys& operator=(const HostHotkeys&) = delete;

    [[nodiscard]] HotkeyOperationResult invoke(Operation operation,
        const bafx::config::HotkeysConfig& bindings = {}, std::uint64_t token = 0U);
    [[nodiscard]] bool handleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    [[nodiscard]] std::uint64_t epoch() const noexcept
    {
        return epoch_.load();
    }
    [[nodiscard]] HotkeyRuntimeState initialState() const noexcept
    {
        return state_;
    }

private:
    struct Registration
    {
        bafx::config::HotkeyBinding binding;
        int id{0};
        bool staged{false};
    };
    struct Request
    {
        Operation operation{};
        bafx::config::HotkeysConfig bindings{};
        std::uint64_t token{0U};
        std::atomic<bool> canceled{false};
        std::promise<HotkeyOperationResult> reply;
    };
    [[nodiscard]] HotkeyOperationResult execute(const Request& request);
    [[nodiscard]] DWORD add(const bafx::config::HotkeyBinding& binding, bool staged);
    void discard(bool stagedOnly);
    void updateState();
    void invalidateEvents();
    void expireCapture();
    void endCapture();
    [[nodiscard]] Registration* find(const bafx::config::HotkeyBinding& binding);

    HWND window_{nullptr};
    DWORD ownerThread_{0U};
    ActionSink sink_;
    bafx::config::HotkeysConfig active_{};
    bafx::config::HotkeysConfig pending_{};
    std::vector<Registration> registrations_{};
    HotkeyRuntimeState state_{};
    bool preparing_{false};
    int nextId_{0x1000};
    std::atomic<std::uint64_t> epoch_{1U};
    std::uint64_t nextCaptureToken_{0U};
    ULONGLONG captureStarted_{0U};
    ULONGLONG captureRenewed_{0U};
    std::mutex requestsMutex_{};
    std::deque<std::shared_ptr<Request>> requests_{};
};

}
