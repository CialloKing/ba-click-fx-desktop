#pragma once

#include <windows.h>

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace bafx::windows
{

inline constexpr std::uint32_t diagnosticLogSchemaVersion = 2U;
inline constexpr std::size_t diagnosticLogMaximumRecordBytes = 64U * 1024U;

struct DiagnosticField
{
    std::string_view key{};
    std::string_view value{};
};

enum class DiagnosticLevel : std::uint8_t
{
    Debug,
    Info,
    Warning,
    Error
};

struct DiagnosticLogRetention
{
    std::uintmax_t maximumBytes{8U * 1024U * 1024U};
    std::uint32_t backupCount{3U};
};

struct DiagnosticLogCleanupResult
{
    std::uint32_t removedFiles{0U};
    std::uintmax_t removedBytes{0U};
    std::uint32_t failedFiles{0U};
    std::error_code firstError{};
};

struct DiagnosticLogTiming
{
    std::chrono::nanoseconds lockAndPrepare{};
    std::chrono::nanoseconds format{};
    std::chrono::nanoseconds fileOperations{};
};

// Process-wide counters remain available even when the filesystem is unusable.
// A concurrent snapshot is observational, not a transaction boundary.
struct DiagnosticLogHealth
{
    std::uint64_t writtenRecords{0U};
    std::uint64_t droppedRecords{0U};
    std::uint64_t writeFailures{0U};
    std::uint64_t rotationFailures{0U};
    std::uint64_t truncatedRecords{0U};
    std::uint64_t lastSuccessfulWriteUtcMilliseconds{0U};
    DWORD lastError{ERROR_SUCCESS};
    std::string_view lastFailureOperation{"none"};
    std::uint64_t lockFailures{0U};
    std::uint64_t cleanupFailures{0U};
};

[[nodiscard]] DiagnosticLogHealth diagnosticLogHealth() noexcept;
[[nodiscard]] std::string diagnosticLogHealthReport();

[[nodiscard]] std::filesystem::path defaultDiagnosticLogPath();

[[nodiscard]] std::string_view diagnosticSessionId() noexcept;

// Performs one best-effort rotation using the supplied retention. Normal
// appends independently enforce the default retention for long-running hosts.
void rotateDiagnosticLog(
    const std::filesystem::path& path,
    DiagnosticLogRetention retention = {}) noexcept;

[[nodiscard]] DiagnosticLogCleanupResult clearDiagnosticLogs(
    const std::filesystem::path& path) noexcept;

void appendDiagnosticEvent(
    const std::filesystem::path& path,
    std::string_view eventName,
    std::span<const DiagnosticField> fields = {},
    DiagnosticLevel level = DiagnosticLevel::Info,
    DiagnosticLogTiming* timing = nullptr) noexcept;

// The body is reserved for existing key/value support reports.
void appendDiagnosticRecord(
    const std::filesystem::path& path,
    std::string_view eventName,
    std::span<const DiagnosticField> fields,
    std::string_view body,
    DiagnosticLevel level = DiagnosticLevel::Info,
    DiagnosticLogTiming* timing = nullptr) noexcept;

void appendDiagnosticLog(
    const std::filesystem::path& path,
    std::string_view event) noexcept;

}
