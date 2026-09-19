#pragma once

#include <windows.h>

#include <array>
#include <cstdint>

namespace bafx::windows
{

struct WindowIdentity final
{
    HWND handle{nullptr};
    DWORD processId{0U};
    DWORD processError{ERROR_SUCCESS};
    std::array<wchar_t, 128U> className{};
    DWORD classError{ERROR_SUCCESS};

    bool operator==(const WindowIdentity&) const = default;
};

struct WindowObservation final
{
    WindowIdentity identity{};
    WindowIdentity aboveCandidate{};
    HWND owner{nullptr};
    std::array<LONG, 4U> bounds{};
    LONG_PTR extendedStyle{0};
    DWORD boundsError{ERROR_SUCCESS};
    DWORD styleError{ERROR_SUCCESS};
    DWORD cloaked{0U};
    HRESULT cloakResult{E_HANDLE};
    std::uint32_t scannedAbove{0U};
    bool scanTruncated{false};
    bool valid{false};
    bool visible{false};
    bool minimized{false};

    bool operator==(const WindowObservation&) const = default;
};

[[nodiscard]] WindowIdentity observeWindowIdentity(HWND window) noexcept;
// Read-only, bounded Z-order observation. An overlapping window is a candidate
// for occlusion, not proof of the final pixels composed by DWM.
[[nodiscard]] WindowObservation observeWindow(HWND window) noexcept;

}
