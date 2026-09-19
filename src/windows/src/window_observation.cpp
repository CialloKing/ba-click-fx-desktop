#include "bafx/windows/window_observation.hpp"

#include <dwmapi.h>

namespace bafx::windows
{
namespace
{

[[nodiscard]] DWORD queryError() noexcept
{
    const DWORD error = GetLastError();
    return error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
}

}

WindowIdentity observeWindowIdentity(const HWND window) noexcept
{
    WindowIdentity result{};
    result.handle = window;
    if (window == nullptr)
    {
        result.processError = ERROR_INVALID_WINDOW_HANDLE;
        result.classError = ERROR_INVALID_WINDOW_HANDLE;
        return result;
    }
    SetLastError(ERROR_SUCCESS);
    if (GetWindowThreadProcessId(window, &result.processId) == 0U)
    {
        result.processError = queryError();
    }
    SetLastError(ERROR_SUCCESS);
    if (GetClassNameW(window, result.className.data(),
            static_cast<int>(result.className.size())) == 0)
    {
        result.classError = queryError();
    }
    return result;
}

WindowObservation observeWindow(const HWND window) noexcept
{
    WindowObservation result{};
    result.identity = observeWindowIdentity(window);
    result.valid = IsWindow(window) != FALSE;
    if (!result.valid)
    {
        result.boundsError = ERROR_INVALID_WINDOW_HANDLE;
        result.styleError = ERROR_INVALID_WINDOW_HANDLE;
        return result;
    }
    result.visible = IsWindowVisible(window) != FALSE;
    result.minimized = IsIconic(window) != FALSE;
    result.owner = GetWindow(window, GW_OWNER);
    SetLastError(ERROR_SUCCESS);
    result.extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (result.extendedStyle == 0)
    {
        result.styleError = GetLastError();
    }
    RECT bounds{};
    SetLastError(ERROR_SUCCESS);
    if (!GetWindowRect(window, &bounds))
    {
        result.boundsError = queryError();
    }
    else
    {
        result.bounds = {bounds.left, bounds.top, bounds.right, bounds.bottom};
    }
    result.cloakResult = DwmGetWindowAttribute(window, DWMWA_CLOAKED,
        &result.cloaked, sizeof(result.cloaked));
    HWND above = GetWindow(window, GW_HWNDPREV);
    while (above != nullptr && result.scannedAbove < 128U)
    {
        ++result.scannedAbove;
        RECT candidate{};
        RECT intersection{};
        if (result.boundsError == ERROR_SUCCESS && IsWindowVisible(above)
            && GetWindowRect(above, &candidate)
            && IntersectRect(&intersection, &bounds, &candidate))
        {
            result.aboveCandidate = observeWindowIdentity(above);
            break;
        }
        const HWND previous = GetWindow(above, GW_HWNDPREV);
        if (previous == above || previous == window)
        {
            result.scanTruncated = true;
            break;
        }
        above = previous;
    }
    result.scanTruncated = result.scanTruncated
        || (above != nullptr && result.scannedAbove == 128U
            && result.aboveCandidate.handle == nullptr);
    return result;
}

}
