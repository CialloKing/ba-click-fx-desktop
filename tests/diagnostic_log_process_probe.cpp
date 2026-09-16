#include "bafx/windows/diagnostic_log.hpp"
#include "bafx/windows/unique_handle.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
constexpr std::size_t recordsPerChild = 24U;

int writeChild(const std::filesystem::path& path, const std::string_view identity)
{
    const std::string payload(48U * 1024U, identity.front());
    for (std::size_t index = 0U; index < recordsPerChild; ++index)
    {
        const auto record = std::string(identity) + ":" + std::to_string(index);
        const std::array fields{
            bafx::windows::DiagnosticField{"Probe.Record", record},
            bafx::windows::DiagnosticField{"Probe.Payload", payload}};
        bafx::windows::appendDiagnosticEvent(path, "Probe.Concurrent", fields);
    }
    return bafx::windows::diagnosticLogHealth().droppedRecords == 0U ? 0 : 1;
}

int runParent()
{
    const auto directory = std::filesystem::temp_directory_path()
        / (L"bafx-log-process-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{directory};
    const auto path = directory / L"concurrent.log";
    {
        // Start at the boundary so both children must coordinate rotation.
        std::ofstream seed(path, std::ios::binary);
        seed << "Event.Name=Seed\nPadding="
             << std::string(bafx::windows::DiagnosticLogRetention{}.maximumBytes - 100U, 'x')
             << "\n---\n";
    }
    std::wstring executable(32'768U, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0U || length >= executable.size())
    {
        return 2;
    }
    executable.resize(length);
    std::vector<bafx::windows::UniqueHandle> children;
    bool failed = false;
    for (const auto* identity : {L"A", L"B"})
    {
        const auto childPath = *identity == L'A' ? path : directory / L"." / L"CONCURRENT.LOG";
        std::wstring command = L"\"" + executable + L"\" --child \"" + childPath.native() + L"\" " + identity;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        {
            failed = true;
            break;
        }
        CloseHandle(process.hThread);
        children.emplace_back(process.hProcess);
    }
    for (const auto& child : children)
    {
        if (WaitForSingleObject(child.get(), 10'000U) != WAIT_OBJECT_0)
        {
            TerminateProcess(child.get(), 125U);
            WaitForSingleObject(child.get(), 1'000U);
            failed = true;
        }
        DWORD code = 1U;
        if (!GetExitCodeProcess(child.get(), &code) || code != 0U)
        {
            failed = true;
        }
    }
    if (failed || children.size() != 2U)
    {
        return 3;
    }
    std::string records;
    for (std::uint32_t index = 0U; index <= 3U; ++index)
    {
        auto candidate = path;
        if (index != 0U)
        {
            candidate += L"." + std::to_wstring(index);
        }
        if (!std::filesystem::exists(candidate))
        {
            continue;
        }
        if (std::filesystem::file_size(candidate) > bafx::windows::DiagnosticLogRetention{}.maximumBytes)
        {
            return 4;
        }
        std::ifstream input(candidate, std::ios::binary);
        records.append(std::istreambuf_iterator<char>(input), {});
    }
    for (const char identity : {'A', 'B'})
    {
        for (std::size_t index = 0U; index < recordsPerChild; ++index)
        {
            const std::string expected = "Probe.Record=" + std::string(1U, identity) + ":"
                + std::to_string(index) + "\nProbe.Payload="
                + std::string(48U * 1024U, identity) + "\n---\n";
            const auto found = records.find(expected);
            if (found == std::string::npos || records.find(expected, found + 1U) != std::string::npos)
            {
                return 5;
            }
        }
    }
    return 0;
}
}

int wmain(const int count, wchar_t* arguments[])
{
    try
    {
        if (count == 4 && std::wstring_view(arguments[1]) == L"--child")
        {
            return writeChild(arguments[2], std::wstring_view(arguments[3]) == L"A" ? "A" : "B");
        }
        return runParent();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 6;
    }
}
