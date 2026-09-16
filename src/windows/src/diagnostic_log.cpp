#include "bafx/windows/diagnostic_log.hpp"
#include "bafx/windows/portable_paths.hpp"
#include "bafx/windows/unique_handle.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>

namespace bafx::windows
{
namespace
{

[[nodiscard]] std::string utcTimestamp()
{
    SYSTEMTIME time{};
    GetSystemTime(&time);
    std::ostringstream stream;
    stream << std::setfill('0')
           << std::setw(4) << time.wYear << '-'
           << std::setw(2) << time.wMonth << '-'
           << std::setw(2) << time.wDay << 'T'
           << std::setw(2) << time.wHour << ':'
           << std::setw(2) << time.wMinute << ':'
           << std::setw(2) << time.wSecond << '.'
           << std::setw(3) << time.wMilliseconds << 'Z';
    return stream.str();
}

[[nodiscard]] std::array<char, 96U> makeDiagnosticSessionId() noexcept
{
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);

    SYSTEMTIME time{};
    GetSystemTime(&time);
    std::array<char, 96U> result{};
    const int written = std::snprintf(
        result.data(),
        result.size(),
        "%04u%02u%02uT%02u%02u%02u.%03uZ-%lu-%llX",
        static_cast<unsigned int>(time.wYear),
        static_cast<unsigned int>(time.wMonth),
        static_cast<unsigned int>(time.wDay),
        static_cast<unsigned int>(time.wHour),
        static_cast<unsigned int>(time.wMinute),
        static_cast<unsigned int>(time.wSecond),
        static_cast<unsigned int>(time.wMilliseconds),
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long long>(counter.QuadPart));
    if (written <= 0 || static_cast<std::size_t>(written) >= result.size())
    {
        constexpr std::string_view fallback = "unknown";
        std::copy(fallback.begin(), fallback.end(), result.begin());
    }
    return result;
}

struct DiagnosticSessionContext
{
    std::array<char, 96U> id{makeDiagnosticSessionId()};
    std::atomic<std::uint64_t> nextSequence{1U};
    std::int64_t startedAtQpc{[]() noexcept
        {
            LARGE_INTEGER counter{};
            return QueryPerformanceCounter(&counter) ? counter.QuadPart : 0LL;
        }()};
    std::int64_t qpcFrequency{[]() noexcept
        {
            LARGE_INTEGER frequency{};
            return QueryPerformanceFrequency(&frequency)
                    && frequency.QuadPart > 0
                ? frequency.QuadPart
                : 0LL;
        }()};
};

[[nodiscard]] DiagnosticSessionContext& diagnosticSession() noexcept
{
    static DiagnosticSessionContext session;
    return session;
}

[[nodiscard]] std::timed_mutex& diagnosticLogMutex() noexcept
{
    static std::timed_mutex mutex;
    return mutex;
}

[[nodiscard]] std::uint64_t diagnosticMonotonicMicroseconds() noexcept
{
    const DiagnosticSessionContext& session = diagnosticSession();
    LARGE_INTEGER counter{};
    if (session.startedAtQpc <= 0
        || session.qpcFrequency <= 0
        || !QueryPerformanceCounter(&counter)
        || counter.QuadPart < session.startedAtQpc)
    {
        return 0U;
    }

    constexpr std::uint64_t microsecondsPerSecond = 1'000'000U;
    const std::uint64_t elapsed = static_cast<std::uint64_t>(
        counter.QuadPart - session.startedAtQpc);
    const std::uint64_t frequency = static_cast<std::uint64_t>(
        session.qpcFrequency);
    return (elapsed / frequency) * microsecondsPerSecond
        + (elapsed % frequency) * microsecondsPerSecond / frequency;
}

[[nodiscard]] std::filesystem::path diagnosticBackupPath(
    const std::filesystem::path& path,
    const std::uint32_t index)
{
    std::filesystem::path backup(path);
    backup += L"." + std::to_wstring(index);
    return backup;
}

enum class FailureOperation : std::uint8_t
{
    None,
    Directory,
    Open,
    Write,
    Rotate,
    Lock,
    Cleanup,
    Format
};

struct LogHealthState
{
    std::atomic<std::uint64_t> writtenRecords{0U};
    std::atomic<std::uint64_t> droppedRecords{0U};
    std::atomic<std::uint64_t> writeFailures{0U};
    std::atomic<std::uint64_t> rotationFailures{0U};
    std::atomic<std::uint64_t> truncatedRecords{0U};
    std::atomic<std::uint64_t> lockFailures{0U};
    std::atomic<std::uint64_t> cleanupFailures{0U};
    std::atomic<std::uint64_t> lastSuccessfulWriteUtcMilliseconds{0U};
    std::atomic<DWORD> lastError{ERROR_SUCCESS};
    std::atomic<FailureOperation> lastOperation{FailureOperation::None};
    std::atomic<std::uint64_t> pendingFailures{0U};
};

LogHealthState health;

std::string_view operationName(const FailureOperation operation) noexcept
{
    switch (operation)
    {
    case FailureOperation::None: return "none";
    case FailureOperation::Directory: return "create-directory";
    case FailureOperation::Open: return "open";
    case FailureOperation::Write: return "write";
    case FailureOperation::Rotate: return "rotate";
    case FailureOperation::Lock: return "lock";
    case FailureOperation::Cleanup: return "cleanup";
    case FailureOperation::Format: return "format";
    }
    return "unknown";
}

void recordFailure(const FailureOperation operation, const DWORD error) noexcept
{
    health.lastError.store(error, std::memory_order_relaxed);
    health.lastOperation.store(operation, std::memory_order_relaxed);
    if (operation == FailureOperation::Rotate)
    {
        health.rotationFailures.fetch_add(1U, std::memory_order_relaxed);
    }
    else if (operation == FailureOperation::Lock)
    {
        health.lockFailures.fetch_add(1U, std::memory_order_relaxed);
    }
    else if (operation == FailureOperation::Cleanup)
    {
        health.cleanupFailures.fetch_add(1U, std::memory_order_relaxed);
        // Maintenance failure does not mean an append failed or recovered.
        return;
    }
    else
    {
        health.writeFailures.fetch_add(1U, std::memory_order_relaxed);
    }
    health.pendingFailures.fetch_add(1U, std::memory_order_relaxed);
}

struct DiagnosticFileState
{
    std::filesystem::path path;
    UniqueHandle mutex;
    ULONGLONG nextCleanupAt{0U};
};

// Only the most recently used path is cached. Production uses one log per
// process; tests and temporary reports must not grow a permanent path registry.
[[nodiscard]] DiagnosticFileState& diagnosticFileState()
{
    static DiagnosticFileState state;
    return state;
}

class DiagnosticFileLock final
{
public:
    explicit DiagnosticFileLock(const std::filesystem::path& path)
        : local_(diagnosticLogMutex(), std::defer_lock)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(25);
        if (!local_.try_lock_until(deadline))
        {
            error_ = ERROR_TIMEOUT;
            return;
        }
        auto& state = diagnosticFileState();
        const auto absolute = std::filesystem::absolute(path).lexically_normal();
        if (state.path != absolute || state.mutex.get() == nullptr)
        {
            const auto canonical = std::filesystem::weakly_canonical(absolute).native();
            std::wstring normalized(canonical.size(), L'\0');
            if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE,
                    canonical.data(), static_cast<int>(canonical.size()),
                    normalized.data(), static_cast<int>(normalized.size()), nullptr, nullptr, 0) == 0)
            {
                error_ = GetLastError();
                return;
            }
            // A stable path hash coordinates processes and spelling variants.
            // std::hash is not required to be stable across process launches.
            std::uint64_t hash = 14695981039346656037ULL;
            for (const wchar_t character : normalized)
            {
                hash = (hash ^ static_cast<std::uint16_t>(character)) * 1099511628211ULL;
            }
            const auto name = L"Global\\BAFX.DiagnosticLog.v1." + std::to_wstring(hash);
            UniqueHandle mutex(CreateMutexW(nullptr, FALSE, name.c_str()));
            if (mutex.get() == nullptr)
            {
                error_ = GetLastError();
                return;
            }
            state.path = absolute;
            state.mutex = std::move(mutex);
            state.nextCleanupAt = 0U;
        }
        const auto remaining = std::max(std::chrono::milliseconds::zero(),
            std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
        const DWORD result = WaitForSingleObject(state.mutex.get(), static_cast<DWORD>(remaining.count()));
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED)
        {
            // No size or file handle is cached, including after owner death.
            acquired_ = true;
        }
        else
        {
            error_ = result == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        }
    }

    ~DiagnosticFileLock()
    {
        if (acquired_)
        {
            ReleaseMutex(diagnosticFileState().mutex.get());
        }
    }

    DiagnosticFileLock(const DiagnosticFileLock&) = delete;
    DiagnosticFileLock& operator=(const DiagnosticFileLock&) = delete;

    [[nodiscard]] DWORD error() const noexcept { return error_; }

private:
    std::unique_lock<std::timed_mutex> local_;
    DWORD error_{ERROR_SUCCESS};
    bool acquired_{false};
};

void cleanupLegacyBackupsUnlocked(const std::filesystem::path& path,
    const DiagnosticLogRetention retention, const bool force = false)
{
    if (retention.backupCount == 0U || retention.backupCount > 16U)
    {
        return;
    }
    auto& state = diagnosticFileState();
    const auto now = GetTickCount64();
    if (!force && now < state.nextCleanupAt)
    {
        return;
    }
    state.nextCleanupAt = now + 60'000U;
    for (std::uint32_t index = retention.backupCount + 1U; index <= 16U; ++index)
    {
        std::error_code error;
        std::filesystem::remove(diagnosticBackupPath(path, index), error);
        if (error)
        {
            recordFailure(FailureOperation::Cleanup, static_cast<DWORD>(error.value()));
        }
    }
}

[[nodiscard]] bool rotateDiagnosticLogUnlocked(
    const std::filesystem::path& path,
    const DiagnosticLogRetention retention,
    const std::uintmax_t incomingBytes = 0U)
{
    constexpr std::uint32_t maximumBackupCount = 16U;
    if (path.empty() || retention.maximumBytes == 0U || retention.backupCount == 0U
        || retention.backupCount > maximumBackupCount)
    {
        return true;
    }
    const auto failed = [](const std::error_code& error)
    {
        recordFailure(FailureOperation::Rotate, static_cast<DWORD>(error.value()));
        return false;
    };
    std::error_code error;
    const std::uintmax_t currentSize = std::filesystem::file_size(path, error);
    if (error == std::errc::no_such_file_or_directory)
    {
        return true;
    }
    if (error)
    {
        return failed(error);
    }
    if (currentSize < retention.maximumBytes && incomingBytes <= retention.maximumBytes - currentSize)
    {
        return true;
    }
    // Rotation is also a maintenance boundary if backups appeared since the
    // previous periodic cleanup (for example after restoring a support bundle).
    cleanupLegacyBackupsUnlocked(path, retention, true);
    for (std::uint32_t index = retention.backupCount; index > 0U; --index)
    {
        const auto source = index == 1U ? path : diagnosticBackupPath(path, index - 1U);
        const auto destination = diagnosticBackupPath(path, index);
        const bool exists = std::filesystem::exists(source, error);
        if (error)
        {
            return failed(error);
        }
        if (!exists)
        {
            continue;
        }
        std::filesystem::remove(destination, error);
        if (error)
        {
            return failed(error);
        }
        std::filesystem::rename(source, destination, error);
        if (error)
        {
            return failed(error);
        }
    }
    return true;
}

[[nodiscard]] std::string sanitizeLogValue(const std::string_view value)
{
    std::string result(value);
    for (char& character : result)
    {
        if (character == '\r' || character == '\n' || character == '\0')
        {
            character = ' ';
        }
    }
    return result;
}

[[nodiscard]] std::string sanitizeLogKey(const std::string_view value)
{
    if (value.empty())
    {
        return "Field";
    }

    std::string result(value);
    for (char& character : result)
    {
        const unsigned char code = static_cast<unsigned char>(character);
        if (std::isalnum(code) == 0
            && character != '.'
            && character != '_'
            && character != '-')
        {
            character = '_';
        }
    }
    return result;
}

[[nodiscard]] std::string_view diagnosticLevelName(
    const DiagnosticLevel level) noexcept
{
    switch (level)
    {
    case DiagnosticLevel::Debug:
        return "Debug";
    case DiagnosticLevel::Info:
        return "Info";
    case DiagnosticLevel::Warning:
        return "Warning";
    case DiagnosticLevel::Error:
        return "Error";
    }
    return "Unknown";
}

[[nodiscard]] std::string formatRecord(
    const std::string_view eventName,
    const std::span<const DiagnosticField> fields,
    const std::string_view body,
    const DiagnosticLevel level,
    const std::uint64_t sequence)
{
    std::ostringstream record;
    record << "Log.SchemaVersion=" << diagnosticLogSchemaVersion << '\n'
           << "Log.SessionId=" << diagnosticSession().id.data() << '\n'
           << "Event.Sequence=" << sequence << '\n'
           << "Event.Utc=" << utcTimestamp() << '\n'
           << "Event.MonotonicUs=" << diagnosticMonotonicMicroseconds() << '\n'
           << "Event.ProcessId=" << GetCurrentProcessId() << '\n'
           << "Event.ThreadId=" << GetCurrentThreadId() << '\n'
           << "Event.Level=" << diagnosticLevelName(level) << '\n'
           << "Event.Name=" << sanitizeLogValue(eventName) << '\n';
    for (const DiagnosticField& field : fields)
    {
        record << sanitizeLogKey(field.key) << '=' << sanitizeLogValue(field.value) << '\n';
    }
    if (!body.empty())
    {
        record << body;
        if (body.back() != '\n')
        {
            record << '\n';
        }
    }
    record << "---\n";
    return record.str();
}

[[nodiscard]] bool recordExceedsBudget(const std::string_view eventName,
    const std::span<const DiagnosticField> fields, const std::string_view body) noexcept
{
    // Reserve the envelope before allocating or copying caller-owned payloads.
    std::size_t remaining = diagnosticLogMaximumRecordBytes - 1'024U;
    const auto consume = [&remaining](const std::size_t bytes)
    {
        if (bytes > remaining)
        {
            return false;
        }
        remaining -= bytes;
        return true;
    };
    if (!consume(eventName.size()) || !consume(body.size()))
    {
        return true;
    }
    for (const auto& field : fields)
    {
        if (!consume(std::max(field.key.size(), std::size_t{5U}))
            || !consume(field.value.size()) || !consume(2U))
        {
            return true;
        }
    }
    return false;
}

// Allocation may throw here; only the public boundary is noexcept so its catch
// can keep diagnostic failures from terminating the application.
[[nodiscard]] bool appendDiagnosticRecordUnlocked(
    const std::filesystem::path& path,
    const std::string_view eventName,
    const std::span<const DiagnosticField> fields,
    const std::string_view body,
    const DiagnosticLevel level)
{
    const auto sequence = diagnosticSession().nextSequence.fetch_add(1U, std::memory_order_relaxed);
    std::string text;
    const DiagnosticLogRetention retention{};
    if (recordExceedsBudget(eventName, fields, body))
    {
        const auto budget = std::to_string(diagnosticLogMaximumRecordBytes);
        const std::array truncatedFields{
            DiagnosticField{"Log.RecordBudgetBytes", budget},
            DiagnosticField{"Log.OriginalLevel", diagnosticLevelName(level)},
            DiagnosticField{"Log.OriginalEvent", eventName.substr(0U, 128U)}};
        text = formatRecord("Log.RecordTruncated", truncatedFields, {}, DiagnosticLevel::Warning, sequence);
        health.truncatedRecords.fetch_add(1U, std::memory_order_relaxed);
    }
    else
    {
        text = formatRecord(eventName, fields, body, level, sequence);
    }
    // Failed rotation must not allow an unbounded active file to keep growing.
    if (!rotateDiagnosticLogUnlocked(path, retention, text.size()))
    {
        return false;
    }
    UniqueHandle output(CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (output.get() == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND)
    {
        // Prepare a missing directory only on demand, including after removal.
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
        {
            recordFailure(FailureOperation::Directory, static_cast<DWORD>(error.value()));
            return false;
        }
        output.reset(CreateFileW(path.c_str(), FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    }
    if (output.get() == nullptr || output.get() == INVALID_HANDLE_VALUE)
    {
        recordFailure(FailureOperation::Open, GetLastError());
        return false;
    }
    DWORD written = 0U;
    const BOOL succeeded = WriteFile(output.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    if (!succeeded || written != text.size())
    {
        recordFailure(FailureOperation::Write, succeeded ? ERROR_WRITE_FAULT : GetLastError());
        return false;
    }
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const std::uint64_t ticks = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32U) | now.dwLowDateTime;
    health.lastSuccessfulWriteUtcMilliseconds.store((ticks - 116444736000000000ULL) / 10'000U,
        std::memory_order_relaxed);
    health.writtenRecords.fetch_add(1U, std::memory_order_relaxed);
    return true;
}


}

std::filesystem::path defaultDiagnosticLogPath()
{
    return executableFilePath(
        L"ba-click-fx-desktop-support.log",
        L"ba-click-fx-desktop-support.log");
}

std::string_view diagnosticSessionId() noexcept
{
    return diagnosticSession().id.data();
}

void rotateDiagnosticLog(
    const std::filesystem::path& path,
    const DiagnosticLogRetention retention) noexcept
{
    try
    {
        const DiagnosticFileLock lock(path);
        if (lock.error() != ERROR_SUCCESS)
        {
            recordFailure(FailureOperation::Lock, lock.error());
            return;
        }
        cleanupLegacyBackupsUnlocked(path, retention, true);
        static_cast<void>(rotateDiagnosticLogUnlocked(path, retention));
    }
    catch (...)
    {
        recordFailure(FailureOperation::Rotate, ERROR_GEN_FAILURE);
    }
}

DiagnosticLogCleanupResult clearDiagnosticLogs(
    const std::filesystem::path& path) noexcept
{
    DiagnosticLogCleanupResult result{};
    try
    {
        const DiagnosticFileLock lock(path);
        if (lock.error() != ERROR_SUCCESS)
        {
            recordFailure(FailureOperation::Lock, lock.error());
            result.failedFiles = 1U;
            result.firstError = std::error_code(static_cast<int>(lock.error()), std::system_category());
            return result;
        }
        for (std::uint32_t index = 0U; index <= 16U; ++index)
        {
            const std::filesystem::path candidate = index == 0U
                ? path
                : diagnosticBackupPath(path, index);
            std::error_code sizeError;
            const std::uintmax_t bytes = std::filesystem::file_size(
                candidate,
                sizeError);
            std::error_code error;
            const bool removed = std::filesystem::remove(candidate, error);
            if (removed)
            {
                ++result.removedFiles;
                if (!sizeError)
                {
                    result.removedBytes += bytes;
                }
                continue;
            }
            if (error)
            {
                ++result.failedFiles;
                if (!result.firstError)
                {
                    result.firstError = error;
                }
            }
        }
    }
    catch (const std::filesystem::filesystem_error& error)
    {
        ++result.failedFiles;
        result.firstError = error.code();
    }
    catch (...)
    {
        ++result.failedFiles;
        result.firstError = std::make_error_code(
            std::errc::io_error);
    }
    return result;
}

DiagnosticLogHealth diagnosticLogHealth() noexcept
{
    return DiagnosticLogHealth{
        health.writtenRecords.load(std::memory_order_relaxed),
        health.droppedRecords.load(std::memory_order_relaxed),
        health.writeFailures.load(std::memory_order_relaxed),
        health.rotationFailures.load(std::memory_order_relaxed),
        health.truncatedRecords.load(std::memory_order_relaxed),
        health.lastSuccessfulWriteUtcMilliseconds.load(std::memory_order_relaxed),
        health.lastError.load(std::memory_order_relaxed),
        operationName(health.lastOperation.load(std::memory_order_relaxed)),
        health.lockFailures.load(std::memory_order_relaxed),
        health.cleanupFailures.load(std::memory_order_relaxed)};
}

std::string diagnosticLogHealthReport()
{
    const auto state = diagnosticLogHealth();
    std::ostringstream stream;
    stream << "Log.Health.Scope=process\n"
           << "Log.Health.WrittenRecords=" << state.writtenRecords << '\n'
           << "Log.Health.DroppedRecords=" << state.droppedRecords << '\n'
           << "Log.Health.WriteFailures=" << state.writeFailures << '\n'
           << "Log.Health.RotationFailures=" << state.rotationFailures << '\n'
           << "Log.Health.TruncatedRecords=" << state.truncatedRecords << '\n'
           << "Log.Health.LockFailures=" << state.lockFailures << '\n'
           << "Log.Health.CleanupFailures=" << state.cleanupFailures << '\n'
           << "Log.Health.LastSuccessfulWriteUnixMs=" << state.lastSuccessfulWriteUtcMilliseconds << '\n'
           << "Log.Health.LastError=" << state.lastError << '\n'
           << "Log.Health.LastFailureOperation=" << state.lastFailureOperation << '\n';
    return stream.str();
}

void appendDiagnosticRecord(
    const std::filesystem::path& path,
    const std::string_view eventName,
    const std::span<const DiagnosticField> fields,
    const std::string_view body,
    const DiagnosticLevel level) noexcept
{
    try
    {
        const DiagnosticFileLock lock(path);
        if (lock.error() != ERROR_SUCCESS)
        {
            recordFailure(FailureOperation::Lock, lock.error());
            health.droppedRecords.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        cleanupLegacyBackupsUnlocked(path, DiagnosticLogRetention{});
        if (!appendDiagnosticRecordUnlocked(path, eventName, fields, body, level))
        {
            health.droppedRecords.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        const auto pending = health.pendingFailures.load(std::memory_order_relaxed);
        if (pending > 0U)
        {
            // Use the low-level writer once, never recursively report failures.
            if (appendDiagnosticRecordUnlocked(path, "Log.WriteRecovered", {},
                    diagnosticLogHealthReport(), DiagnosticLevel::Warning))
            {
                health.pendingFailures.fetch_sub(pending, std::memory_order_relaxed);
            }
        }
    }
    catch (...)
    {
        recordFailure(FailureOperation::Format, ERROR_GEN_FAILURE);
        health.droppedRecords.fetch_add(1U, std::memory_order_relaxed);
    }
}

void appendDiagnosticEvent(
    const std::filesystem::path& path,
    const std::string_view eventName,
    const std::span<const DiagnosticField> fields,
    const DiagnosticLevel level) noexcept
{
    appendDiagnosticRecord(path, eventName, fields, {}, level);
}

void appendDiagnosticLog(
    const std::filesystem::path& path,
    const std::string_view event) noexcept
{
    const std::array fields{
        DiagnosticField{"Event.Message", event}};
    appendDiagnosticEvent(path, "Message", fields);
}


}
