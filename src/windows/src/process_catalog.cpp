#include "bafx/windows/process_catalog.hpp"
#include "bafx/windows/unique_handle.hpp"

#include <tlhelp32.h>

#include <algorithm>
#include <stdexcept>

namespace bafx::windows
{

std::wstring processTextWide(const std::string_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0)
    {
        throw std::runtime_error("invalid UTF-8 process text");
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::string processTextUtf8(const std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0)
    {
        throw std::runtime_error("invalid Unicode process text");
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}

std::filesystem::path queryProcessExecutable(const DWORD processId, DWORD* const error) noexcept
{
    try
    {
        UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
        DWORD failure = process.get() == nullptr ? GetLastError() : ERROR_SUCCESS;
        std::wstring path(32'768U, L'\0');
        DWORD length = static_cast<DWORD>(path.size());
        if (failure == ERROR_SUCCESS
            && QueryFullProcessImageNameW(process.get(), 0U, path.data(), &length) == FALSE)
        {
            failure = GetLastError();
        }
        if (error != nullptr)
        {
            *error = failure;
        }
        if (failure != ERROR_SUCCESS)
        {
            return {};
        }
        path.resize(length);
        return std::filesystem::path(path);
    }
    catch (...)
    {
        if (error != nullptr)
        {
            *error = ERROR_NOT_ENOUGH_MEMORY;
        }
        return {};
    }
}

ProcessSnapshot queryProcessSnapshot(
    const std::span<const std::wstring> executableNames, const bool currentSessionOnly) noexcept
{
    ProcessSnapshot result;
    try
    {
        DWORD currentSession = 0U;
        if (currentSessionOnly && ProcessIdToSessionId(GetCurrentProcessId(), &currentSession) == FALSE)
        {
            result.error = GetLastError();
            return result;
        }
        UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0U));
        if (snapshot.get() == INVALID_HANDLE_VALUE)
        {
            result.error = GetLastError();
            return result;
        }
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        BOOL available = Process32FirstW(snapshot.get(), &entry);
        while (available != FALSE)
        {
            const bool candidate = executableNames.empty()
                || std::any_of(executableNames.begin(), executableNames.end(), [&entry](const auto& name)
                {
                    return CompareStringOrdinal(entry.szExeFile, -1, name.c_str(), -1, TRUE) == CSTR_EQUAL;
                });
            if (candidate && entry.th32ProcessID != 0U)
            {
                DWORD session = 0U;
                const bool sessionKnown = !currentSessionOnly
                    || ProcessIdToSessionId(entry.th32ProcessID, &session) != FALSE;
                if (!currentSessionOnly || !sessionKnown || session == currentSession)
                {
                    ProcessInfo process;
                    process.id = entry.th32ProcessID;
                    process.executableName = entry.szExeFile;
                    process.sessionKnown = sessionKnown;
                    process.executablePath = queryProcessExecutable(process.id, &process.pathError);
                    result.processes.push_back(std::move(process));
                }
            }
            entry.dwSize = sizeof(entry);
            available = Process32NextW(snapshot.get(), &entry);
        }
        result.error = GetLastError();
        result.complete = result.error == ERROR_NO_MORE_FILES;
        if (result.complete)
        {
            result.error = ERROR_SUCCESS;
        }
    }
    catch (...)
    {
        result.complete = false;
        result.error = ERROR_NOT_ENOUGH_MEMORY;
    }
    return result;
}

}
