#include "test_support.hpp"

#include "bafx/windows/borderless_capture_access.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{

using bafx::windows::BorderlessCaptureAccessAsyncStatus;
using bafx::windows::BorderlessCaptureAccessOperation;
using bafx::windows::BorderlessCaptureAccessRequest;
using bafx::windows::BorderlessCaptureAccessResult;
using bafx::windows::BorderlessCaptureAccessStatus;

struct FakeOperationCounters
{
    std::size_t statusCalls{0U};
    std::size_t errorCalls{0U};
    std::size_t getResultsCalls{0U};
    std::size_t cancelCalls{0U};
};

class FakeBorderlessCaptureAccessOperation final
    : public BorderlessCaptureAccessOperation
{
public:
    explicit FakeBorderlessCaptureAccessOperation(
        std::vector<BorderlessCaptureAccessAsyncStatus> statuses,
        const BorderlessCaptureAccessResult result = {
            BorderlessCaptureAccessStatus::Allowed,
            S_OK},
        const HRESULT error = S_OK,
        std::shared_ptr<FakeOperationCounters> counters =
            std::make_shared<FakeOperationCounters>())
        : statuses_(std::move(statuses)),
          result_(result),
          error_(error),
          counters_(std::move(counters))
    {
    }

    [[nodiscard]] BorderlessCaptureAccessAsyncStatus status()
        const noexcept override
    {
        ++counters_->statusCalls;
        if (statuses_.empty())
        {
            return BorderlessCaptureAccessAsyncStatus::Error;
        }
        const std::size_t index = (std::min)(
            nextStatus_,
            statuses_.size() - 1U);
        if (nextStatus_ + 1U < statuses_.size())
        {
            ++nextStatus_;
        }
        return statuses_[index];
    }

    [[nodiscard]] BorderlessCaptureAccessResult getResults() noexcept override
    {
        ++counters_->getResultsCalls;
        return result_;
    }

    [[nodiscard]] HRESULT error() const noexcept override
    {
        ++counters_->errorCalls;
        return error_;
    }

    void cancel() noexcept override
    {
        ++counters_->cancelCalls;
    }

private:
    std::vector<BorderlessCaptureAccessAsyncStatus> statuses_{};
    BorderlessCaptureAccessResult result_{};
    HRESULT error_{S_OK};
    std::shared_ptr<FakeOperationCounters> counters_{};
    mutable std::size_t nextStatus_{0U};
};

constexpr auto requestTimeout = std::chrono::milliseconds(100);
const auto requestStartedAt = BorderlessCaptureAccessRequest::Clock::time_point{};

static_assert(!std::is_move_constructible_v<BorderlessCaptureAccessRequest>);
static_assert(!std::is_move_assignable_v<BorderlessCaptureAccessRequest>);

}

BAFX_TEST(borderless_access_pending_does_not_read_results_or_cancel)
{
    BorderlessCaptureAccessRequest request(requestTimeout);
    const auto counters = std::make_shared<FakeOperationCounters>();
    auto operation = std::make_unique<FakeBorderlessCaptureAccessOperation>(
        std::vector{
            BorderlessCaptureAccessAsyncStatus::Started},
        BorderlessCaptureAccessResult{},
        S_OK,
        counters);
    request.begin(std::move(operation), requestStartedAt);

    const auto poll = request.poll(
        requestStartedAt + std::chrono::milliseconds(99));

    BAFX_CHECK(poll.pending);
    BAFX_CHECK(!poll.result.has_value());
    BAFX_CHECK(counters->statusCalls == 1U);
    BAFX_CHECK(counters->getResultsCalls == 0U);
    BAFX_CHECK(counters->cancelCalls == 0U);
    BAFX_CHECK(request.pending());
}

BAFX_TEST(borderless_access_completion_at_deadline_wins_over_timeout)
{
    BorderlessCaptureAccessRequest request(requestTimeout);
    const auto counters = std::make_shared<FakeOperationCounters>();
    auto operation = std::make_unique<FakeBorderlessCaptureAccessOperation>(
        std::vector{
            BorderlessCaptureAccessAsyncStatus::Started,
            BorderlessCaptureAccessAsyncStatus::Completed},
        BorderlessCaptureAccessResult{
            BorderlessCaptureAccessStatus::Allowed,
            S_OK},
        S_OK,
        counters);
    request.begin(std::move(operation), requestStartedAt);

    const auto poll = request.poll(requestStartedAt + requestTimeout);

    BAFX_CHECK(!poll.pending);
    BAFX_CHECK(poll.result.has_value());
    BAFX_CHECK(poll.result->status == BorderlessCaptureAccessStatus::Allowed);
    BAFX_CHECK(
        poll.result->asyncStatus
        == BorderlessCaptureAccessAsyncStatus::Completed);
    BAFX_CHECK(!poll.result->cancelRequested);
    BAFX_CHECK(poll.result->elapsedMilliseconds == 100U);
    BAFX_CHECK(counters->statusCalls == 2U);
    BAFX_CHECK(counters->getResultsCalls == 1U);
    BAFX_CHECK(counters->cancelCalls == 0U);
}

BAFX_TEST(borderless_access_timeout_cancels_exactly_once)
{
    BorderlessCaptureAccessRequest request(requestTimeout);
    const auto counters = std::make_shared<FakeOperationCounters>();
    auto operation = std::make_unique<FakeBorderlessCaptureAccessOperation>(
        std::vector{
            BorderlessCaptureAccessAsyncStatus::Started},
        BorderlessCaptureAccessResult{},
        S_OK,
        counters);
    request.begin(std::move(operation), requestStartedAt);

    const auto timeout = request.poll(requestStartedAt + requestTimeout);
    const auto after = request.poll(
        requestStartedAt + requestTimeout + std::chrono::milliseconds(1));

    BAFX_CHECK(timeout.result.has_value());
    BAFX_CHECK(timeout.result->status == BorderlessCaptureAccessStatus::TimedOut);
    BAFX_CHECK(timeout.result->cancelRequested);
    BAFX_CHECK(counters->cancelCalls == 1U);
    BAFX_CHECK(!after.pending);
    BAFX_CHECK(!after.result.has_value());
    BAFX_CHECK(!request.active());
}

BAFX_TEST(borderless_access_completed_statuses_preserve_broker_result)
{
    constexpr BorderlessCaptureAccessStatus statuses[]{
        BorderlessCaptureAccessStatus::Allowed,
        BorderlessCaptureAccessStatus::DeniedBySystem,
        BorderlessCaptureAccessStatus::NotDeclaredByApp,
        BorderlessCaptureAccessStatus::DeniedByUser,
        BorderlessCaptureAccessStatus::UserPromptRequired};
    for (const BorderlessCaptureAccessStatus expected : statuses)
    {
        BorderlessCaptureAccessRequest request(requestTimeout);
        const auto counters = std::make_shared<FakeOperationCounters>();
        auto operation = std::make_unique<FakeBorderlessCaptureAccessOperation>(
            std::vector{
                BorderlessCaptureAccessAsyncStatus::Completed},
            BorderlessCaptureAccessResult{expected, S_OK},
            S_OK,
            counters);
        request.begin(std::move(operation), requestStartedAt);

        const auto poll = request.poll(requestStartedAt);

        BAFX_CHECK(poll.result.has_value());
        BAFX_CHECK(poll.result->status == expected);
        BAFX_CHECK(
            poll.result->asyncStatus
            == BorderlessCaptureAccessAsyncStatus::Completed);
        BAFX_CHECK(counters->getResultsCalls == 1U);
        BAFX_CHECK(counters->cancelCalls == 0U);
    }
}

BAFX_TEST(borderless_access_error_and_canceled_are_distinct)
{
    {
        BorderlessCaptureAccessRequest request(requestTimeout);
        auto operation = std::make_unique<FakeBorderlessCaptureAccessOperation>(
            std::vector{
                BorderlessCaptureAccessAsyncStatus::Error},
            BorderlessCaptureAccessResult{},
            REGDB_E_CLASSNOTREG);
        request.begin(std::move(operation), requestStartedAt);

        const auto poll = request.poll(requestStartedAt);

        BAFX_CHECK(poll.result.has_value());
        BAFX_CHECK(
            poll.result->status
            == BorderlessCaptureAccessStatus::Unsupported);
        BAFX_CHECK(poll.result->error == REGDB_E_CLASSNOTREG);
        BAFX_CHECK(
            poll.result->asyncStatus
            == BorderlessCaptureAccessAsyncStatus::Error);
    }
    {
        BorderlessCaptureAccessRequest request(requestTimeout);
        auto operation = std::make_unique<FakeBorderlessCaptureAccessOperation>(
            std::vector{
                BorderlessCaptureAccessAsyncStatus::Canceled});
        request.begin(std::move(operation), requestStartedAt);

        const auto poll = request.poll(requestStartedAt);

        BAFX_CHECK(poll.result.has_value());
        BAFX_CHECK(
            poll.result->status
            == BorderlessCaptureAccessStatus::Canceled);
        BAFX_CHECK(
            poll.result->asyncStatus
            == BorderlessCaptureAccessAsyncStatus::Canceled);
        BAFX_CHECK(!poll.result->cancelRequested);
    }
}

BAFX_TEST(borderless_access_explicit_cancel_is_terminal_and_one_shot)
{
    BorderlessCaptureAccessRequest request(requestTimeout);
    const auto counters = std::make_shared<FakeOperationCounters>();
    auto operation = std::make_unique<FakeBorderlessCaptureAccessOperation>(
        std::vector{
            BorderlessCaptureAccessAsyncStatus::Started},
        BorderlessCaptureAccessResult{},
        S_OK,
        counters);
    request.begin(std::move(operation), requestStartedAt);

    request.cancel(requestStartedAt + std::chrono::milliseconds(25));
    const auto canceled = request.poll(
        requestStartedAt + std::chrono::milliseconds(25));
    const auto after = request.poll(
        requestStartedAt + std::chrono::milliseconds(26));

    BAFX_CHECK(canceled.result.has_value());
    BAFX_CHECK(
        canceled.result->status
        == BorderlessCaptureAccessStatus::Canceled);
    BAFX_CHECK(canceled.result->cancelRequested);
    BAFX_CHECK(
        canceled.result->asyncStatus
        == BorderlessCaptureAccessAsyncStatus::Started);
    BAFX_CHECK(canceled.result->elapsedMilliseconds == 25U);
    BAFX_CHECK(counters->cancelCalls == 1U);
    BAFX_CHECK(!after.result.has_value());
}

BAFX_TEST(borderless_access_completion_wins_over_explicit_cancel)
{
    BorderlessCaptureAccessRequest request(requestTimeout);
    const auto counters = std::make_shared<FakeOperationCounters>();
    auto operation = std::make_unique<FakeBorderlessCaptureAccessOperation>(
        std::vector{
            BorderlessCaptureAccessAsyncStatus::Started,
            BorderlessCaptureAccessAsyncStatus::Completed},
        BorderlessCaptureAccessResult{
            BorderlessCaptureAccessStatus::Allowed,
            S_OK},
        S_OK,
        counters);
    request.begin(std::move(operation), requestStartedAt);

    request.cancel(requestStartedAt + std::chrono::milliseconds(25));
    const auto completed = request.poll(
        requestStartedAt + std::chrono::milliseconds(25));

    BAFX_CHECK(completed.result.has_value());
    BAFX_CHECK(completed.result->status == BorderlessCaptureAccessStatus::Allowed);
    BAFX_CHECK(
        completed.result->asyncStatus
        == BorderlessCaptureAccessAsyncStatus::Completed);
    BAFX_CHECK(completed.result->cancelRequested);
    BAFX_CHECK(counters->cancelCalls == 1U);
    BAFX_CHECK(counters->getResultsCalls == 1U);
}

BAFX_TEST(borderless_access_destructor_requests_cancel_once)
{
    const auto counters = std::make_shared<FakeOperationCounters>();
    {
        BorderlessCaptureAccessRequest request(requestTimeout);
        auto operation = std::make_unique<FakeBorderlessCaptureAccessOperation>(
            std::vector{
                BorderlessCaptureAccessAsyncStatus::Started},
            BorderlessCaptureAccessResult{},
            S_OK,
            counters);
        request.begin(std::move(operation), requestStartedAt);
    }

    BAFX_CHECK(counters->cancelCalls == 1U);
    BAFX_CHECK(counters->statusCalls == 2U);
}
