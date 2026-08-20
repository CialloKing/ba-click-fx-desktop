#include "obs_spout_plugin_probe.hpp"

#include <windows.h>
#include <tlhelp32.h>
#include <winver.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace bafx::control_center
{
namespace
{

class ScopedHandle final
{
public:
    explicit ScopedHandle(HANDLE handle = nullptr) noexcept
        : handle_(handle)
    {
    }

    ~ScopedHandle()
    {
        if (valid())
        {
            CloseHandle(handle_);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr))
    {
    }

    ScopedHandle& operator=(ScopedHandle&& other) noexcept
    {
        if (this != &other)
        {
            if (valid())
            {
                CloseHandle(handle_);
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE get() const noexcept
    {
        return handle_;
    }

private:
    HANDLE handle_{nullptr};
};

class ScopedRegistryKey final
{
public:
    ScopedRegistryKey() noexcept = default;

    ~ScopedRegistryKey()
    {
        if (key_ != nullptr)
        {
            RegCloseKey(key_);
        }
    }

    ScopedRegistryKey(const ScopedRegistryKey&) = delete;
    ScopedRegistryKey& operator=(const ScopedRegistryKey&) = delete;

    [[nodiscard]] HKEY* address() noexcept
    {
        return &key_;
    }

    [[nodiscard]] HKEY get() const noexcept
    {
        return key_;
    }

private:
    HKEY key_{nullptr};
};

struct RunningObsProcess final
{
    DWORD processId{0U};
    std::filesystem::path executable{};
    ObsProcessModuleEvidence modules{};
    std::filesystem::path loadedPlugin{};
};

[[nodiscard]] bool equalsIgnoreCase(
    const std::wstring_view left,
    const std::wstring_view right) noexcept
{
    return left.size() == right.size()
        && _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

[[nodiscard]] bool containsIgnoreCase(
    const std::wstring_view value,
    const std::wstring_view needle)
{
    std::wstring loweredValue(value);
    std::wstring loweredNeedle(needle);
    std::ranges::transform(
        loweredValue,
        loweredValue.begin(),
        [](const wchar_t character)
        {
            return static_cast<wchar_t>(std::towlower(character));
        });
    std::ranges::transform(
        loweredNeedle,
        loweredNeedle.begin(),
        [](const wchar_t character)
        {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return loweredValue.find(loweredNeedle) != std::wstring::npos;
}

[[nodiscard]] std::optional<std::wstring> environmentValue(
    const wchar_t* const name)
{
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0U);
    if (required == 0U)
    {
        return std::nullopt;
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        name,
        value.data(),
        required);
    if (written == 0U || written >= required)
    {
        return std::nullopt;
    }
    value.resize(written);
    return value;
}

[[nodiscard]] std::optional<std::wstring> registryString(
    const HKEY key,
    const wchar_t* const name)
{
    DWORD type = 0U;
    DWORD bytes = 0U;
    if (RegQueryValueExW(
            key,
            name,
            nullptr,
            &type,
            nullptr,
            &bytes) != ERROR_SUCCESS
        || (type != REG_SZ && type != REG_EXPAND_SZ)
        || bytes < sizeof(wchar_t)
        || bytes > 64U * 1024U)
    {
        return std::nullopt;
    }

    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(
            key,
            name,
            nullptr,
            &type,
            reinterpret_cast<BYTE*>(value.data()),
            &bytes) != ERROR_SUCCESS)
    {
        return std::nullopt;
    }
    while (!value.empty() && value.back() == L'\0')
    {
        value.pop_back();
    }
    if (type != REG_EXPAND_SZ)
    {
        return value;
    }

    const DWORD expandedSize = ExpandEnvironmentStringsW(
        value.c_str(),
        nullptr,
        0U);
    if (expandedSize == 0U)
    {
        return value;
    }
    std::wstring expanded(expandedSize, L'\0');
    const DWORD expandedWritten = ExpandEnvironmentStringsW(
        value.c_str(),
        expanded.data(),
        expandedSize);
    if (expandedWritten == 0U || expandedWritten > expandedSize)
    {
        return value;
    }
    while (!expanded.empty() && expanded.back() == L'\0')
    {
        expanded.pop_back();
    }
    return expanded;
}

void appendUniquePath(
    std::vector<std::filesystem::path>& paths,
    std::filesystem::path candidate)
{
    if (candidate.empty())
    {
        return;
    }
    candidate = candidate.lexically_normal();
    const std::wstring value = candidate.native();
    const auto duplicate = std::ranges::find_if(
        paths,
        [&value](const std::filesystem::path& existing)
        {
            return equalsIgnoreCase(existing.native(), value);
        });
    if (duplicate == paths.end())
    {
        paths.push_back(std::move(candidate));
    }
}

[[nodiscard]] std::filesystem::path executableFromDisplayIcon(
    const std::wstring_view displayIcon)
{
    std::wstring value(displayIcon);
    if (!value.empty() && value.front() == L'"')
    {
        const std::size_t closingQuote = value.find(L'"', 1U);
        if (closingQuote != std::wstring::npos)
        {
            value = value.substr(1U, closingQuote - 1U);
        }
    }
    else
    {
        const std::size_t comma = value.rfind(L',');
        if (comma != std::wstring::npos)
        {
            value.resize(comma);
        }
    }
    return std::filesystem::path(value);
}

void appendObsRegistryRoots(
    std::vector<std::filesystem::path>& roots,
    const HKEY hive,
    const REGSAM view)
{
    constexpr wchar_t uninstallPath[] =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    ScopedRegistryKey uninstall;
    if (RegOpenKeyExW(
            hive,
            uninstallPath,
            0U,
            KEY_READ | view,
            uninstall.address()) != ERROR_SUCCESS)
    {
        return;
    }

    for (DWORD index = 0U;; ++index)
    {
        std::array<wchar_t, 512U> subkeyName{};
        DWORD nameLength = static_cast<DWORD>(subkeyName.size());
        const LSTATUS enumerated = RegEnumKeyExW(
            uninstall.get(),
            index,
            subkeyName.data(),
            &nameLength,
            nullptr,
            nullptr,
            nullptr,
            nullptr);
        if (enumerated == ERROR_NO_MORE_ITEMS)
        {
            break;
        }
        if (enumerated != ERROR_SUCCESS)
        {
            continue;
        }

        ScopedRegistryKey application;
        if (RegOpenKeyExW(
                uninstall.get(),
                subkeyName.data(),
                0U,
                KEY_READ | view,
                application.address()) != ERROR_SUCCESS)
        {
            continue;
        }
        const std::optional<std::wstring> displayName = registryString(
            application.get(),
            L"DisplayName");
        if (!displayName.has_value()
            || !containsIgnoreCase(*displayName, L"OBS Studio"))
        {
            continue;
        }

        const std::optional<std::wstring> installLocation = registryString(
            application.get(),
            L"InstallLocation");
        if (installLocation.has_value() && !installLocation->empty())
        {
            appendUniquePath(roots, *installLocation);
            continue;
        }
        const std::optional<std::wstring> displayIcon = registryString(
            application.get(),
            L"DisplayIcon");
        if (displayIcon.has_value())
        {
            const std::filesystem::path executable =
                executableFromDisplayIcon(*displayIcon);
            if (!executable.empty())
            {
                std::filesystem::path root = executable.parent_path();
                if (equalsIgnoreCase(root.filename().native(), L"64bit")
                    || equalsIgnoreCase(root.filename().native(), L"32bit"))
                {
                    root = root.parent_path().parent_path();
                }
                appendUniquePath(roots, std::move(root));
            }
        }
    }
}

[[nodiscard]] ScopedHandle moduleSnapshot(const DWORD processId) noexcept
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        ScopedHandle snapshot(CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
            processId));
        if (snapshot.valid() || GetLastError() != ERROR_BAD_LENGTH)
        {
            return snapshot;
        }
    }
    return ScopedHandle{};
}

void inspectProcessModules(RunningObsProcess& process) noexcept
{
    ScopedHandle snapshot = moduleSnapshot(process.processId);
    if (!snapshot.valid())
    {
        return;
    }

    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (Module32FirstW(snapshot.get(), &module) == FALSE)
    {
        return;
    }
    process.modules.inspectionSucceeded = true;
    do
    {
        if (equalsIgnoreCase(module.szModule, L"win-spout.dll"))
        {
            process.modules.pluginLoaded = true;
            process.loadedPlugin = module.szExePath;
            return;
        }
        module.dwSize = sizeof(module);
    } while (Module32NextW(snapshot.get(), &module) != FALSE);
}

[[nodiscard]] std::filesystem::path processExecutable(
    const DWORD processId) noexcept
{
    ScopedHandle process(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        processId));
    if (!process.valid())
    {
        return {};
    }

    std::wstring path(32'768U, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    if (QueryFullProcessImageNameW(
            process.get(),
            0U,
            path.data(),
            &length) == FALSE)
    {
        return {};
    }
    path.resize(length);
    return std::filesystem::path(path);
}

[[nodiscard]] std::vector<RunningObsProcess> runningObsProcesses() noexcept
{
    std::vector<RunningObsProcess> processes;
    ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0U));
    if (!snapshot.valid())
    {
        return processes;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.get(), &entry) == FALSE)
    {
        return processes;
    }
    do
    {
        if (!equalsIgnoreCase(entry.szExeFile, L"obs64.exe")
            && !equalsIgnoreCase(entry.szExeFile, L"obs32.exe"))
        {
            entry.dwSize = sizeof(entry);
            continue;
        }
        RunningObsProcess process{};
        process.processId = entry.th32ProcessID;
        process.executable = processExecutable(process.processId);
        inspectProcessModules(process);
        processes.push_back(std::move(process));
        entry.dwSize = sizeof(entry);
    } while (Process32NextW(snapshot.get(), &entry) != FALSE);
    return processes;
}

void appendRootsFromRunningObs(
    std::vector<std::filesystem::path>& roots,
    const std::span<const RunningObsProcess> processes)
{
    for (const RunningObsProcess& process : processes)
    {
        std::filesystem::path ancestor = process.executable.parent_path();
        for (int level = 0; level < 4 && !ancestor.empty(); ++level)
        {
            appendUniquePath(roots, ancestor);
            ancestor = ancestor.parent_path();
        }
    }
}

[[nodiscard]] std::vector<std::filesystem::path> pluginCandidates(
    const std::span<const RunningObsProcess> processes)
{
    std::vector<std::filesystem::path> roots;
    appendRootsFromRunningObs(roots, processes);
    appendObsRegistryRoots(roots, HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY);
    appendObsRegistryRoots(roots, HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY);
    appendObsRegistryRoots(roots, HKEY_CURRENT_USER, KEY_WOW64_64KEY);
    appendObsRegistryRoots(roots, HKEY_CURRENT_USER, KEY_WOW64_32KEY);

    if (const std::optional<std::wstring> value = environmentValue(
            L"ProgramW6432"))
    {
        appendUniquePath(roots, std::filesystem::path(*value) / L"obs-studio");
    }
    if (const std::optional<std::wstring> value = environmentValue(
            L"ProgramFiles"))
    {
        appendUniquePath(roots, std::filesystem::path(*value) / L"obs-studio");
    }

    std::vector<std::filesystem::path> candidates;
    for (const std::filesystem::path& root : roots)
    {
        appendUniquePath(
            candidates,
            root / L"obs-plugins" / L"64bit" / L"win-spout.dll");
        appendUniquePath(
            candidates,
            root / L"obs-plugins" / L"32bit" / L"win-spout.dll");
    }

    const auto appendGlobalCandidates = [&candidates](
        const std::optional<std::wstring>& base)
    {
        if (!base.has_value())
        {
            return;
        }
        const std::filesystem::path pluginRoot =
            std::filesystem::path(*base) / L"obs-studio" / L"plugins";
        appendUniquePath(
            candidates,
            pluginRoot / L"win-spout" / L"bin" / L"64bit"
                / L"win-spout.dll");
        appendUniquePath(
            candidates,
            pluginRoot / L"win-spout" / L"bin" / L"32bit"
                / L"win-spout.dll");
        appendUniquePath(candidates, pluginRoot / L"win-spout.dll");
    };
    appendGlobalCandidates(environmentValue(L"ProgramData"));
    appendGlobalCandidates(environmentValue(L"APPDATA"));
    return candidates;
}

[[nodiscard]] std::filesystem::path firstInstalledPlugin(
    const std::span<const std::filesystem::path> candidates) noexcept
{
    for (const std::filesystem::path& candidate : candidates)
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error)
        {
            return candidate;
        }
    }
    return {};
}

[[nodiscard]] std::string fileVersion(
    const std::filesystem::path& path) noexcept
{
    DWORD ignored = 0U;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0U)
    {
        return {};
    }
    std::vector<BYTE> data(size);
    if (GetFileVersionInfoW(path.c_str(), 0U, size, data.data()) == FALSE)
    {
        return {};
    }
    VS_FIXEDFILEINFO* information = nullptr;
    UINT informationSize = 0U;
    if (VerQueryValueW(
            data.data(),
            L"\\",
            reinterpret_cast<void**>(&information),
            &informationSize) == FALSE
        || information == nullptr
        || informationSize < sizeof(VS_FIXEDFILEINFO))
    {
        return {};
    }

    const std::array<unsigned long, 4U> components{
        HIWORD(information->dwFileVersionMS),
        LOWORD(information->dwFileVersionMS),
        HIWORD(information->dwFileVersionLS),
        LOWORD(information->dwFileVersionLS)};
    std::size_t componentCount = components.back() == 0U ? 3U : 4U;
    std::string version;
    for (std::size_t index = 0U; index < componentCount; ++index)
    {
        if (!version.empty())
        {
            version.push_back('.');
        }
        version += std::to_string(components[index]);
    }
    return version;
}

[[nodiscard]] std::string binaryArchitecture(
    const std::filesystem::path& path) noexcept
{
    ScopedHandle file(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!file.valid())
    {
        return {};
    }

    IMAGE_DOS_HEADER dos{};
    DWORD bytesRead = 0U;
    if (ReadFile(file.get(), &dos, sizeof(dos), &bytesRead, nullptr) == FALSE
        || bytesRead != sizeof(dos)
        || dos.e_magic != IMAGE_DOS_SIGNATURE
        || dos.e_lfanew <= 0)
    {
        return {};
    }
    LARGE_INTEGER offset{};
    offset.QuadPart = dos.e_lfanew;
    if (SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN) == FALSE)
    {
        return {};
    }
    DWORD signature = 0U;
    IMAGE_FILE_HEADER header{};
    if (ReadFile(
            file.get(),
            &signature,
            sizeof(signature),
            &bytesRead,
            nullptr) == FALSE
        || bytesRead != sizeof(signature)
        || signature != IMAGE_NT_SIGNATURE
        || ReadFile(
            file.get(),
            &header,
            sizeof(header),
            &bytesRead,
            nullptr) == FALSE
        || bytesRead != sizeof(header))
    {
        return {};
    }
    switch (header.Machine)
    {
    case IMAGE_FILE_MACHINE_AMD64:
        return "x64";
    case IMAGE_FILE_MACHINE_I386:
        return "x86";
    case IMAGE_FILE_MACHINE_ARM64:
        return "ARM64";
    default:
        return "unknown";
    }
}

}

ObsSpoutPluginState classifyObsSpoutPlugin(
    const bool installed,
    const std::span<const ObsProcessModuleEvidence> processEvidence) noexcept
{
    if (!installed)
    {
        const bool inspectionUnavailable = std::ranges::any_of(
            processEvidence,
            [](const ObsProcessModuleEvidence& process)
            {
                return !process.inspectionSucceeded;
            });
        if (inspectionUnavailable)
        {
            return ObsSpoutPluginState::InspectionUnavailable;
        }
        return ObsSpoutPluginState::Missing;
    }
    if (processEvidence.empty())
    {
        return ObsSpoutPluginState::InstalledObsNotRunning;
    }

    bool inspectionUnavailable = false;
    for (const ObsProcessModuleEvidence& process : processEvidence)
    {
        if (process.inspectionSucceeded && process.pluginLoaded)
        {
            return ObsSpoutPluginState::Loaded;
        }
        inspectionUnavailable = inspectionUnavailable
            || !process.inspectionSucceeded;
    }
    return inspectionUnavailable
        ? ObsSpoutPluginState::InspectionUnavailable
        : ObsSpoutPluginState::InstalledNotLoaded;
}

ObsSpoutPluginProbeResult probeObsSpoutPlugin() noexcept
{
    try
    {
        const std::vector<RunningObsProcess> processes = runningObsProcesses();
        std::filesystem::path pluginPath;
        for (const RunningObsProcess& process : processes)
        {
            if (process.modules.pluginLoaded && !process.loadedPlugin.empty())
            {
                pluginPath = process.loadedPlugin;
                break;
            }
        }
        if (pluginPath.empty())
        {
            pluginPath = firstInstalledPlugin(pluginCandidates(processes));
        }

        std::vector<ObsProcessModuleEvidence> evidence;
        evidence.reserve(processes.size());
        for (const RunningObsProcess& process : processes)
        {
            evidence.push_back(process.modules);
        }

        ObsSpoutPluginProbeResult result{};
        result.state = classifyObsSpoutPlugin(!pluginPath.empty(), evidence);
        result.obsRunning = !processes.empty();
        if (!processes.empty())
        {
            result.obsExecutable = processes.front().executable;
        }
        result.pluginPath = std::move(pluginPath);
        if (!result.pluginPath.empty())
        {
            result.pluginVersion = fileVersion(result.pluginPath);
            result.pluginArchitecture = binaryArchitecture(result.pluginPath);
        }
        return result;
    }
    catch (...)
    {
        ObsSpoutPluginProbeResult result{};
        result.state = ObsSpoutPluginState::InspectionUnavailable;
        return result;
    }
}

}
