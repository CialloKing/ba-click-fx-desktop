#include "test_support.hpp"
#include "host_recovery_state.hpp"

#include <limits>

BAFX_TEST(output_recovery_stops_after_three_attempts_and_retains_copied_budget)
{
    bafx::desktop::OutputRenegotiationBudget budget;
    BAFX_CHECK(budget.remaining() == 3U);
    BAFX_CHECK(budget.retryAfterFailure());
    auto queued = budget;
    BAFX_CHECK(queued.remaining() == 2U);
    BAFX_CHECK(queued.retryAfterFailure());
    BAFX_CHECK(!queued.retryAfterFailure());
    BAFX_CHECK(!queued.retryAfterFailure());
    BAFX_CHECK(queued.remaining() == 1U);
}

BAFX_TEST(background_recovery_completion_preserves_monotonic_request_identity)
{
    bafx::desktop::BackgroundRetryState state(7U);
    state.request();
    BAFX_CHECK(state.pending());
    BAFX_CHECK(state.token() == 8U);
    state.finishRequest();
    BAFX_CHECK(!state.pending());
    BAFX_CHECK(state.token() == 8U);
    // Immediate resource reconciliation advances identity without queuing
    // another deferred transaction.
    state.advanceToken();
    BAFX_CHECK(!state.pending());
    BAFX_CHECK(state.token() == 9U);
}

BAFX_TEST(background_recovery_overflow_cannot_publish_a_partial_request)
{
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    bafx::desktop::BackgroundRetryState state(maximum - 1U);
    state.request();
    BAFX_CHECK(state.exhausted());
    for (const bool pending : {true, false})
    {
        if (!pending)
        {
            state.finishRequest();
        }
        bool rejected = false;
        try
        {
            state.request("original failure context");
        }
        catch (const std::runtime_error& error)
        {
            rejected = std::string_view(error.what()) == "original failure context";
        }
        BAFX_CHECK(rejected);
        BAFX_CHECK(state.token() == maximum);
        BAFX_CHECK(state.pending() == pending);
    }
}
