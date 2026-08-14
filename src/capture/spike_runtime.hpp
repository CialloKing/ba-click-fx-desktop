#pragma once

#include "bafx/windows/unique_handle.hpp"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <thread>

namespace bafx::capture
{

class ComApartment final
{
public:
    ComApartment();
    ~ComApartment();

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool initialized_{false};
};

class ProcessWatchdog final
{
public:
    explicit ProcessWatchdog(std::uint32_t timeoutMilliseconds);
    ~ProcessWatchdog();

    ProcessWatchdog(const ProcessWatchdog&) = delete;
    ProcessWatchdog& operator=(const ProcessWatchdog&) = delete;

private:
    bafx::windows::UniqueHandle stopEvent_{};
    std::thread worker_{};
};

class Deadline final
{
public:
    explicit Deadline(std::chrono::milliseconds duration);

    [[nodiscard]] bool expired() const noexcept;
    [[nodiscard]] DWORD nextWaitMilliseconds() const noexcept;

private:
    std::chrono::steady_clock::time_point expiresAt_{};
};

}
