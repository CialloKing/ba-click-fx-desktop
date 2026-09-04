#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace bafx::release_update
{

inline constexpr std::size_t maximumResponseBytes = 256U * 1024U;

struct ReleaseVersion final
{
    std::uint32_t major{0U};
    std::uint32_t minor{0U};
    std::uint32_t patch{0U};

    [[nodiscard]] auto operator<=>(const ReleaseVersion&) const noexcept = default;
};

[[nodiscard]] std::optional<ReleaseVersion> parseReleaseVersion(
    std::string_view text) noexcept;

enum class ReleaseResponseError : std::uint8_t
{
    None,
    ResponseTooLarge,
    MalformedJson,
    MissingTagName,
    DuplicateTagName,
    InvalidTagName
};

struct ReleaseResponseParseResult final
{
    std::optional<ReleaseVersion> version{};
    std::string tagName{};
    ReleaseResponseError error{ReleaseResponseError::None};

    [[nodiscard]] bool succeeded() const noexcept
    {
        return version.has_value() && error == ReleaseResponseError::None;
    }
};

// Only the top-level tag_name field is part of the update-check trust boundary.
// Asset URLs and release text from the response are deliberately ignored.
[[nodiscard]] ReleaseResponseParseResult parseReleaseResponse(
    std::string_view response) noexcept;

enum class ReleaseTransportStatus : std::uint8_t
{
    Succeeded,
    Cancelled,
    Failed
};

struct ReleaseTransportResult final
{
    ReleaseTransportStatus status{ReleaseTransportStatus::Failed};
    std::uint32_t httpStatus{0U};
    std::string body{};
    std::string failure{};
};

class ReleaseTransport
{
public:
    virtual ~ReleaseTransport() = default;

    [[nodiscard]] virtual ReleaseTransportResult fetchLatestRelease(
        std::stop_token stopToken) = 0;
    virtual void cancel() noexcept = 0;
};

enum class UpdateCheckStatus : std::uint8_t
{
    Idle,
    Checking,
    Current,
    UpdateAvailable,
    Ahead,
    Failed
};

struct UpdateCheckSnapshot final
{
    UpdateCheckStatus status{UpdateCheckStatus::Idle};
    std::optional<ReleaseVersion> latestVersion{};
    std::string latestTagName{};
    std::string failure{};
    std::uint64_t sequence{0U};
};

class ReleaseUpdateChecker final
{
public:
    ReleaseUpdateChecker(
        ReleaseVersion currentVersion,
        std::shared_ptr<ReleaseTransport> transport);
    ~ReleaseUpdateChecker();

    ReleaseUpdateChecker(const ReleaseUpdateChecker&) = delete;
    ReleaseUpdateChecker& operator=(const ReleaseUpdateChecker&) = delete;
    ReleaseUpdateChecker(ReleaseUpdateChecker&&) = delete;
    ReleaseUpdateChecker& operator=(ReleaseUpdateChecker&&) = delete;

    // A second request while Checking is intentionally coalesced.
    [[nodiscard]] bool start();
    void cancel();
    [[nodiscard]] UpdateCheckSnapshot snapshot() const;

private:
    void run(std::stop_token stopToken) noexcept;
    void publish(UpdateCheckSnapshot snapshot) noexcept;
    void stopWorker(bool resetToIdle);

    ReleaseVersion currentVersion_{};
    std::shared_ptr<ReleaseTransport> transport_{};
    mutable std::mutex stateMutex_{};
    std::mutex lifecycleMutex_{};
    UpdateCheckSnapshot state_{};
    std::jthread worker_{};
};

// Browser targets are not derived from the network response.
[[nodiscard]] std::wstring_view officialLatestReleasePageUrl() noexcept;
[[nodiscard]] std::wstring_view officialProjectRepositoryUrl() noexcept;

// This factory has no endpoint arguments so callers cannot redirect the check
// to an untrusted host or relax the transport policy.
[[nodiscard]] std::shared_ptr<ReleaseTransport> makeWinHttpReleaseTransport();

}
