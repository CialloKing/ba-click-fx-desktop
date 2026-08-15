#include "display_target.hpp"

#include <windows.h>

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace bafx::desktop
{

std::string displayTargetDeviceUtf8(const DisplayTarget& target)
{
    if (target.deviceName.empty())
    {
        return "unknown";
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        target.deviceName.data(),
        static_cast<int>(target.deviceName.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0)
    {
        return "invalid-utf8";
    }

    std::string converted(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        target.deviceName.data(),
        static_cast<int>(target.deviceName.size()),
        converted.data(),
        required,
        nullptr,
        nullptr);
    return written == required ? converted : "invalid-utf8";
}

std::string formatDisplayTargetMonitor(const DisplayTarget& target)
{
    std::ostringstream stream;
    stream << "0x"
           << std::uppercase
           << std::hex
           << std::setw(static_cast<int>(sizeof(std::uintptr_t) * 2U))
           << std::setfill('0')
           << reinterpret_cast<std::uintptr_t>(target.monitor);
    return stream.str();
}

std::string formatDisplayTargetBounds(const DisplayTarget& target)
{
    std::ostringstream stream;
    stream << target.bounds.right - target.bounds.left
           << 'x'
           << target.bounds.bottom - target.bounds.top
           << '@'
           << target.bounds.left
           << ','
           << target.bounds.top;
    return stream.str();
}

}
