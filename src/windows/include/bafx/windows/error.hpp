#pragma once

#include <windows.h>

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace bafx::windows
{

class HResultError final : public std::runtime_error
{
public:
    HResultError(const HRESULT result, const std::string_view operation)
        : std::runtime_error(formatMessage(result, operation))
        , result_(result)
    {
    }

    [[nodiscard]] HRESULT result() const noexcept
    {
        return result_;
    }

private:
    [[nodiscard]] static std::string formatMessage(
        const HRESULT result,
        const std::string_view operation)
    {
        std::ostringstream stream;
        stream << operation << " failed with HRESULT 0x"
               << std::hex << std::uppercase << std::setw(8)
               << std::setfill('0') << static_cast<unsigned long>(result);
        return stream.str();
    }

    HRESULT result_;
};

inline void throwIfFailed(const HRESULT result, const std::string_view operation)
{
    if (FAILED(result))
    {
        throw HResultError(result, operation);
    }
}

inline void throwLastError(const std::string_view operation)
{
    const DWORD error = GetLastError();
    throw HResultError(HRESULT_FROM_WIN32(error), operation);
}

}

