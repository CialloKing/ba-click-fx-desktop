#include "host_hotkeys.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace bafx::desktop
{
namespace
{
constexpr UINT requestMessage = WM_APP + 0x241U;
constexpr UINT_PTR captureTimer = 0xBA10U;

bool contains(const bafx::config::HotkeysConfig& config,
    const bafx::config::HotkeyBinding& binding)
{
    return std::any_of(config.bindings.begin(), config.bindings.end(),
        [&binding](const auto& item) { return item == binding; });
}
}

HostHotkeys::HostHotkeys(const HWND window,
    const bafx::config::HotkeysConfig& initial, ActionSink sink)
    : window_(window), ownerThread_(GetCurrentThreadId()), sink_(std::move(sink)), active_(initial)
{
    registrations_.reserve(8U);
    for (std::size_t index = 0U; index < active_.bindings.size(); ++index)
    {
        if (active_.bindings[index].has_value())
        {
            state_.errors[index] = add(*active_.bindings[index], false);
        }
    }
    updateState();
}

HostHotkeys::~HostHotkeys()
{
    KillTimer(window_, captureTimer);
    for (const auto& registration : registrations_)
    {
        UnregisterHotKey(window_, registration.id);
    }
    std::lock_guard lock(requestsMutex_);
    for (const auto& request : requests_)
    {
        request->reply.set_value({false, {}, "hotkey manager stopped"});
    }
}

HotkeyOperationResult HostHotkeys::invoke(const Operation operation,
    const bafx::config::HotkeysConfig& bindings, const std::uint64_t token)
{
    auto request = std::make_shared<Request>();
    request->operation = operation;
    request->bindings = bindings;
    request->token = token;
    if (GetCurrentThreadId() == ownerThread_)
    {
        return execute(*request);
    }
    auto reply = request->reply.get_future();
    {
        std::lock_guard lock(requestsMutex_);
        if (requests_.size() >= 32U)
        {
            return {false, {}, "hotkey request queue is full"};
        }
        requests_.push_back(request);
        if (!PostMessageW(window_, requestMessage, 0U, 0))
        {
            requests_.pop_back();
            return {false, {}, "HostShell is unavailable"};
        }
    }
    if (reply.wait_for(std::chrono::milliseconds(750)) != std::future_status::ready)
    {
        // Cancel only requests not yet taken by the owner. Started operations
        // contain no I/O and must finish before their outcome is reported.
        std::lock_guard lock(requestsMutex_);
        const auto found = std::find(requests_.begin(), requests_.end(), request);
        if (found != requests_.end())
        {
            request->canceled = true;
            requests_.erase(found);
            return {false, {}, "HostShell did not process the hotkey request"};
        }
    }
    return reply.get();
}

HostHotkeys::Registration* HostHotkeys::find(const bafx::config::HotkeyBinding& binding)
{
    const auto found = std::find_if(registrations_.begin(), registrations_.end(),
        [&binding](const Registration& entry) { return entry.binding == binding; });
    return found == registrations_.end() ? nullptr : &*found;
}

DWORD HostHotkeys::add(const bafx::config::HotkeyBinding& binding, const bool staged)
{
    if (find(binding) != nullptr)
    {
        return ERROR_SUCCESS;
    }
    if (nextId_ > 0xBFFF)
    {
        invalidateEvents();
        nextId_ = 0x1000;
    }
    while (std::any_of(registrations_.begin(), registrations_.end(),
        [this](const Registration& item) { return item.id == nextId_; }))
    {
        ++nextId_;
    }
    const int id = nextId_++;
    if (!RegisterHotKey(window_, id, binding.modifiers | MOD_NOREPEAT, binding.key))
    {
        return GetLastError();
    }
    registrations_.push_back({binding, id, staged});
    return ERROR_SUCCESS;
}

void HostHotkeys::invalidateEvents()
{
    ++epoch_;
    MSG message{};
    while (PeekMessageW(&message, window_, WM_HOTKEY, WM_HOTKEY, PM_REMOVE))
    {
        // A queued notification must never be reinterpreted after reassignment.
    }
}

void HostHotkeys::updateState()
{
    state_.registeredMask = 0U;
    for (std::size_t index = 0U; index < active_.bindings.size(); ++index)
    {
        if (!active_.bindings[index].has_value())
        {
            state_.errors[index] = ERROR_SUCCESS;
        }
        else if (find(*active_.bindings[index]) != nullptr)
        {
            state_.registeredMask |= 1U << index;
            state_.errors[index] = ERROR_SUCCESS;
        }
    }
}

void HostHotkeys::discard(const bool stagedOnly)
{
    DWORD operationError = ERROR_SUCCESS;
    std::erase_if(registrations_, [this, stagedOnly, &operationError](const Registration& entry)
    {
        const bool remove = stagedOnly ? entry.staged : !contains(active_, entry.binding);
        if (!remove)
        {
            return false;
        }
        if (!UnregisterHotKey(window_, entry.id))
        {
            operationError = GetLastError();
            return false;
        }
        return true;
    });
    if (operationError != ERROR_SUCCESS)
    {
        state_.cleanupError = operationError;
        return;
    }
    // Keep a prior cleanup failure visible until no staged or inactive
    // registration remains; an unrelated successful unregister must not hide it.
    const bool cleanupPending = std::any_of(registrations_.begin(), registrations_.end(),
        [this](const Registration& entry)
        {
            return entry.staged || !contains(active_, entry.binding);
        });
    if (!cleanupPending)
    {
        state_.cleanupError = ERROR_SUCCESS;
    }
}

void HostHotkeys::endCapture()
{
    KillTimer(window_, captureTimer);
    state_.captureToken = 0U;
    state_.captured.reset();
    invalidateEvents();
}

void HostHotkeys::expireCapture()
{
    const ULONGLONG now = GetTickCount64();
    if (state_.captureToken != 0U
        && (now - captureStarted_ >= 30'000U || now - captureRenewed_ >= 5'000U))
    {
        endCapture();
    }
}

HotkeyOperationResult HostHotkeys::execute(const Request& request)
{
    expireCapture();
    switch (request.operation)
    {
    case Operation::Prepare:
    {
        if (preparing_ || state_.captureToken != 0U)
        {
            return {false, state_, "hotkeys are busy; finish recording before saving"};
        }
        std::string error;
        if (!bafx::config::validateHotkeys(request.bindings, &error))
        {
            return {false, state_, error};
        }
        preparing_ = true;
        pending_ = request.bindings;
        invalidateEvents();
        for (std::size_t index = 0U; index < pending_.bindings.size(); ++index)
        {
            if (!pending_.bindings[index].has_value())
            {
                continue;
            }
            const DWORD registered = add(*pending_.bindings[index], true);
            if (registered != ERROR_SUCCESS)
            {
                discard(true);
                preparing_ = false;
                updateState();
                return {false, state_, std::string(bafx::config::hotkeyActionNames[index])
                    + ": RegisterHotKey failed, Win32=" + std::to_string(registered)};
            }
        }
        break;
    }
    case Operation::Commit:
        if (!preparing_)
        {
            return {false, state_, "no prepared hotkey transaction"};
        }
        active_ = pending_;
        for (auto& entry : registrations_)
        {
            entry.staged = false;
        }
        discard(false);
        preparing_ = false;
        invalidateEvents();
        updateState();
        break;
    case Operation::Rollback:
        discard(true);
        preparing_ = false;
        invalidateEvents();
        updateState();
        break;
    case Operation::Retry:
        if (preparing_ || state_.captureToken != 0U)
        {
            return {false, state_, "hotkeys are busy"};
        }
        for (std::size_t index = 0U; index < active_.bindings.size(); ++index)
        {
            if (active_.bindings[index].has_value())
            {
                state_.errors[index] = add(*active_.bindings[index], false);
            }
        }
        updateState();
        break;
    case Operation::BeginCapture:
        if (preparing_ || state_.captureToken != 0U)
        {
            return {false, state_, "another hotkey operation is active"};
        }
        invalidateEvents();
        state_.captureToken = ++nextCaptureToken_;
        state_.captured.reset();
        captureStarted_ = captureRenewed_ = GetTickCount64();
        if (SetTimer(window_, captureTimer, 1'000U, nullptr) == 0U)
        {
            endCapture();
            return {false, state_, "unable to start recording timeout"};
        }
        break;
    case Operation::EndCapture:
        if (request.token != 0U && request.token == state_.captureToken)
        {
            endCapture();
        }
        break;
    case Operation::Query:
        if (request.token != 0U && request.token == state_.captureToken)
        {
            captureRenewed_ = GetTickCount64();
        }
        break;
    }
    return {true, state_, {}};
}

bool HostHotkeys::handleMessage(const UINT message, const WPARAM wParam, const LPARAM lParam)
{
    if (message == requestMessage)
    {
        for (;;)
        {
            std::shared_ptr<Request> request;
            {
                std::lock_guard lock(requestsMutex_);
                if (requests_.empty())
                {
                    break;
                }
                request = std::move(requests_.front());
                requests_.pop_front();
            }
            try
            {
                request->reply.set_value(execute(*request));
            }
            catch (...)
            {
                request->reply.set_value({false, state_, "hotkey operation failed"});
            }
        }
        return true;
    }
    if (message == WM_TIMER && wParam == captureTimer)
    {
        expireCapture();
        return true;
    }
    if (message != WM_HOTKEY)
    {
        return false;
    }
    expireCapture();
    const auto found = std::find_if(registrations_.begin(), registrations_.end(),
        [wParam](const Registration& item) { return item.id == static_cast<int>(wParam); });
    if (preparing_ || found == registrations_.end() || found->staged
        || found->binding.key != HIWORD(lParam)
        || found->binding.modifiers != (LOWORD(lParam) & 15U))
    {
        return true;
    }
    for (std::size_t index = 0U; index < active_.bindings.size(); ++index)
    {
        if (active_.bindings[index] == found->binding)
        {
            if (state_.captureToken != 0U)
            {
                state_.captured = found->binding;
            }
            else if (sink_)
            {
                sink_(static_cast<bafx::config::HotkeyAction>(index), epoch());
            }
            break;
        }
    }
    return true;
}

}
