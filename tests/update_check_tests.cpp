#include "test_support.hpp"

#include "update_check.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using bafx::release_update::ReleaseTransport;
using bafx::release_update::ReleaseTransportResult;
using bafx::release_update::ReleaseTransportStatus;
using bafx::release_update::ReleaseUpdateChecker;
using bafx::release_update::ReleaseVersion;
using bafx::release_update::UpdateCheckSnapshot;
using bafx::release_update::UpdateCheckStatus;

[[nodiscard]] ReleaseTransportResult successfulResponse(
    std::string body,
    const std::uint32_t httpStatus = 200U)
{
    return ReleaseTransportResult{
        .status = ReleaseTransportStatus::Succeeded,
        .httpStatus = httpStatus,
        .body = std::move(body)};
}

class QueueTransport final : public ReleaseTransport
{
public:
    explicit QueueTransport(std::vector<ReleaseTransportResult> results)
        : results_(results.begin(), results.end())
    {
    }

    [[nodiscard]] ReleaseTransportResult fetchLatestRelease(
        const std::stop_token) override
    {
        std::scoped_lock lock(mutex_);
        ++fetchCount_;
        if (results_.empty())
        {
            return ReleaseTransportResult{
                .status = ReleaseTransportStatus::Failed,
                .failure = "No scripted response"};
        }

        ReleaseTransportResult result = std::move(results_.front());
        results_.pop_front();
        return result;
    }

    void cancel() noexcept override
    {
        ++cancelCount_;
    }

    [[nodiscard]] std::uint32_t fetchCount() const noexcept
    {
        return fetchCount_.load();
    }

    [[nodiscard]] std::uint32_t cancelCount() const noexcept
    {
        return cancelCount_.load();
    }

private:
    std::mutex mutex_{};
    std::deque<ReleaseTransportResult> results_{};
    std::atomic_uint32_t fetchCount_{0U};
    std::atomic_uint32_t cancelCount_{0U};
};

class BlockingTransport final : public ReleaseTransport
{
public:
    [[nodiscard]] ReleaseTransportResult fetchLatestRelease(
        const std::stop_token) override
    {
        std::unique_lock lock(mutex_);
        started_ = true;
        condition_.notify_all();
        condition_.wait(
            lock,
            [this]()
            {
                return cancelled_;
            });
        return ReleaseTransportResult{
            .status = ReleaseTransportStatus::Cancelled};
    }

    void cancel() noexcept override
    {
        std::scoped_lock lock(mutex_);
        cancelled_ = true;
        ++cancelCount_;
        condition_.notify_all();
    }

    [[nodiscard]] bool waitUntilStarted()
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(
            lock,
            std::chrono::seconds(2),
            [this]()
            {
                return started_;
            });
    }

    [[nodiscard]] std::uint32_t cancelCount() const noexcept
    {
        return cancelCount_.load();
    }

private:
    mutable std::mutex mutex_{};
    std::condition_variable condition_{};
    bool started_{false};
    bool cancelled_{false};
    std::atomic_uint32_t cancelCount_{0U};
};

[[nodiscard]] UpdateCheckSnapshot waitForTerminal(
    const ReleaseUpdateChecker& checker)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(2);
    do
    {
        UpdateCheckSnapshot snapshot = checker.snapshot();
        if (snapshot.status != UpdateCheckStatus::Checking)
        {
            return snapshot;
        }

        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);

    throw std::runtime_error("update check did not finish");
}

[[nodiscard]] UpdateCheckSnapshot runCheck(
    const ReleaseVersion currentVersion,
    ReleaseTransportResult result)
{
    const auto transport = std::make_shared<QueueTransport>(
        std::vector<ReleaseTransportResult>{std::move(result)});
    ReleaseUpdateChecker checker(currentVersion, transport);
    if (!checker.start())
    {
        throw std::runtime_error("update check did not start");
    }

    return waitForTerminal(checker);
}

}

BAFX_TEST(update_version_parser_accepts_only_canonical_numeric_tags)
{
    const std::optional<ReleaseVersion> zero =
        bafx::release_update::parseReleaseVersion("v0.0.0");
    BAFX_CHECK(zero.has_value());
    BAFX_CHECK(zero->major == 0U);
    BAFX_CHECK(zero->minor == 0U);
    BAFX_CHECK(zero->patch == 0U);

    const std::optional<ReleaseVersion> maximum =
        bafx::release_update::parseReleaseVersion(
            "v4294967295.4294967295.4294967295");
    BAFX_CHECK(maximum.has_value());
    BAFX_CHECK(maximum->major == UINT32_MAX);
    BAFX_CHECK(maximum->minor == UINT32_MAX);
    BAFX_CHECK(maximum->patch == UINT32_MAX);

    constexpr std::string_view invalidTags[] = {
        "0.2.5",
        "V0.2.5",
        "v0.2",
        "v0.2.5.0",
        "v00.2.5",
        "v0.02.5",
        "v0.2.05",
        "v0.2.5-alpha.1",
        "v0.2.5+build",
        " v0.2.5",
        "v0.2.5 ",
        "v+0.2.5",
        "v0.-2.5",
        "v0.2.-5",
        "v4294967296.0.0",
        "v0.4294967296.0",
        "v0.0.4294967296"};
    for (const std::string_view tag : invalidTags)
    {
        BAFX_CHECK(!bafx::release_update::parseReleaseVersion(tag).has_value());
    }
}

BAFX_TEST(update_version_comparison_is_numeric)
{
    const ReleaseVersion earlier{.major = 1U, .minor = 9U, .patch = 99U};
    const ReleaseVersion later{.major = 1U, .minor = 10U, .patch = 0U};
    const ReleaseVersion same{.major = 1U, .minor = 10U, .patch = 0U};
    BAFX_CHECK(earlier < later);
    BAFX_CHECK(later > earlier);
    BAFX_CHECK(later == same);
}

BAFX_TEST(update_response_parser_reads_only_top_level_tag_name)
{
    const auto result = bafx::release_update::parseReleaseResponse(
        R"({"nested":{"tag_name":"v9.9.9"},"tag\u005fname":"v1.2.3","html_url":"https://evil.example/"})");
    BAFX_CHECK(result.succeeded());
    BAFX_CHECK(result.tagName == "v1.2.3");
    BAFX_CHECK(result.version->major == 1U);
    BAFX_CHECK(result.version->minor == 2U);
    BAFX_CHECK(result.version->patch == 3U);
}

BAFX_TEST(update_response_parser_rejects_duplicate_or_invalid_tag_name)
{
    const auto duplicate = bafx::release_update::parseReleaseResponse(
        R"({"tag_name":"v1.2.3","tag\u005fname":"v1.2.4"})");
    BAFX_CHECK(
        duplicate.error
        == bafx::release_update::ReleaseResponseError::DuplicateTagName);

    const auto missing = bafx::release_update::parseReleaseResponse(
        R"({"name":"v1.2.3"})");
    BAFX_CHECK(
        missing.error
        == bafx::release_update::ReleaseResponseError::MissingTagName);

    const auto nonString = bafx::release_update::parseReleaseResponse(
        R"({"tag_name":123})");
    BAFX_CHECK(
        nonString.error
        == bafx::release_update::ReleaseResponseError::InvalidTagName);

    const auto overflow = bafx::release_update::parseReleaseResponse(
        R"({"tag_name":"v4294967296.0.0"})");
    BAFX_CHECK(
        overflow.error
        == bafx::release_update::ReleaseResponseError::InvalidTagName);

    const auto malformed = bafx::release_update::parseReleaseResponse(
        R"({"tag_name":"v1.2.3",})");
    BAFX_CHECK(
        malformed.error
        == bafx::release_update::ReleaseResponseError::MalformedJson);
}

BAFX_TEST(update_response_parser_enforces_the_response_limit)
{
    std::string exact = R"({"tag_name":"v1.2.3","padding":")";
    constexpr std::string_view suffix = R"("})";
    exact.append(
        bafx::release_update::maximumResponseBytes
            - exact.size()
            - suffix.size(),
        'x');
    exact.append(suffix);
    BAFX_CHECK(exact.size() == bafx::release_update::maximumResponseBytes);
    BAFX_CHECK(bafx::release_update::parseReleaseResponse(exact).succeeded());

    exact.push_back(' ');
    const auto oversized = bafx::release_update::parseReleaseResponse(exact);
    BAFX_CHECK(
        oversized.error
        == bafx::release_update::ReleaseResponseError::ResponseTooLarge);
}

BAFX_TEST(update_checker_classifies_current_available_and_ahead)
{
    constexpr ReleaseVersion current{.major = 0U, .minor = 2U, .patch = 5U};

    const UpdateCheckSnapshot same = runCheck(
        current,
        successfulResponse(R"({"tag_name":"v0.2.5"})"));
    BAFX_CHECK(same.status == UpdateCheckStatus::Current);
    BAFX_CHECK(same.latestTagName == "v0.2.5");

    const UpdateCheckSnapshot newer = runCheck(
        current,
        successfulResponse(R"({"tag_name":"v0.3.0"})"));
    BAFX_CHECK(newer.status == UpdateCheckStatus::UpdateAvailable);

    const UpdateCheckSnapshot older = runCheck(
        current,
        successfulResponse(R"({"tag_name":"v0.2.4"})"));
    BAFX_CHECK(older.status == UpdateCheckStatus::Ahead);
}

BAFX_TEST(update_checker_is_idle_and_does_not_request_automatically)
{
    const auto transport = std::make_shared<QueueTransport>(
        std::vector<ReleaseTransportResult>{});
    ReleaseUpdateChecker checker(
        ReleaseVersion{.major = 0U, .minor = 2U, .patch = 5U},
        transport);

    BAFX_CHECK(checker.snapshot().status == UpdateCheckStatus::Idle);
    BAFX_CHECK(transport->fetchCount() == 0U);
}

BAFX_TEST(update_checker_fails_closed_for_http_transport_and_body_errors)
{
    constexpr ReleaseVersion current{.major = 0U, .minor = 2U, .patch = 5U};

    const UpdateCheckSnapshot redirect = runCheck(
        current,
        successfulResponse({}, 302U));
    BAFX_CHECK(redirect.status == UpdateCheckStatus::Failed);

    const UpdateCheckSnapshot notFound = runCheck(
        current,
        successfulResponse({}, 404U));
    BAFX_CHECK(notFound.status == UpdateCheckStatus::Failed);

    const UpdateCheckSnapshot transportFailure = runCheck(
        current,
        ReleaseTransportResult{
            .status = ReleaseTransportStatus::Failed,
            .failure = "offline"});
    BAFX_CHECK(transportFailure.status == UpdateCheckStatus::Failed);
    BAFX_CHECK(transportFailure.failure == "offline");

    const UpdateCheckSnapshot timeout = runCheck(
        current,
        ReleaseTransportResult{
            .status = ReleaseTransportStatus::Failed,
            .failure = "WinHttpReceiveResponse timed out"});
    BAFX_CHECK(timeout.status == UpdateCheckStatus::Failed);
    BAFX_CHECK(timeout.failure == "WinHttpReceiveResponse timed out");

    const UpdateCheckSnapshot duplicate = runCheck(
        current,
        successfulResponse(
            R"({"tag_name":"v0.2.5","tag_name":"v0.2.6"})"));
    BAFX_CHECK(duplicate.status == UpdateCheckStatus::Failed);

    std::string oversized(
        bafx::release_update::maximumResponseBytes + 1U,
        'x');
    const UpdateCheckSnapshot large = runCheck(
        current,
        successfulResponse(std::move(oversized)));
    BAFX_CHECK(large.status == UpdateCheckStatus::Failed);
}

BAFX_TEST(update_checker_coalesces_repeated_clicks_and_cancels_safely)
{
    const auto transport = std::make_shared<BlockingTransport>();
    ReleaseUpdateChecker checker(
        ReleaseVersion{.major = 0U, .minor = 2U, .patch = 5U},
        transport);

    BAFX_CHECK(checker.start());
    BAFX_CHECK(transport->waitUntilStarted());
    BAFX_CHECK(checker.snapshot().status == UpdateCheckStatus::Checking);
    BAFX_CHECK(!checker.start());

    checker.cancel();
    BAFX_CHECK(checker.snapshot().status == UpdateCheckStatus::Idle);
    BAFX_CHECK(transport->cancelCount() == 1U);
}

BAFX_TEST(update_checker_destruction_cancels_and_joins_an_active_request)
{
    const auto transport = std::make_shared<BlockingTransport>();
    {
        ReleaseUpdateChecker checker(
            ReleaseVersion{.major = 0U, .minor = 2U, .patch = 5U},
            transport);
        BAFX_CHECK(checker.start());
        BAFX_CHECK(transport->waitUntilStarted());
        BAFX_CHECK(checker.snapshot().status == UpdateCheckStatus::Checking);
    }

    BAFX_CHECK(transport->cancelCount() == 1U);
}

BAFX_TEST(update_checker_can_start_again_after_completion)
{
    const auto transport = std::make_shared<QueueTransport>(
        std::vector<ReleaseTransportResult>{
            successfulResponse(R"({"tag_name":"v0.2.5"})"),
            successfulResponse(R"({"tag_name":"v0.2.6"})")});
    ReleaseUpdateChecker checker(
        ReleaseVersion{.major = 0U, .minor = 2U, .patch = 5U},
        transport);

    BAFX_CHECK(checker.start());
    BAFX_CHECK(waitForTerminal(checker).status == UpdateCheckStatus::Current);
    BAFX_CHECK(checker.start());
    BAFX_CHECK(
        waitForTerminal(checker).status == UpdateCheckStatus::UpdateAvailable);
    BAFX_CHECK(transport->fetchCount() == 2U);
    BAFX_CHECK(transport->cancelCount() == 0U);
}

BAFX_TEST(update_release_page_url_is_a_fixed_official_target)
{
    BAFX_CHECK(
        bafx::release_update::officialLatestReleasePageUrl()
        == L"https://github.com/CialloKing/ba-click-fx-desktop/releases/latest");

    const auto response = bafx::release_update::parseReleaseResponse(
        R"({"tag_name":"v0.2.6","html_url":"https://evil.example/release"})");
    BAFX_CHECK(response.succeeded());
    BAFX_CHECK(
        bafx::release_update::officialLatestReleasePageUrl()
        == L"https://github.com/CialloKing/ba-click-fx-desktop/releases/latest");
}
