#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bafx::desktop
{

struct MetricSummary
{
    std::uint64_t sampleCount{0U};
    std::uint64_t recordedSampleCount{0U};
    std::uint64_t droppedSampleCount{0U};
    std::uint64_t minimum{0U};
    std::uint64_t p50{0U};
    std::uint64_t p95{0U};
    std::uint64_t p99{0U};
    std::uint64_t maximum{0U};
    double average{0.0};
};

// Interactive diagnostics keep exact samples for one bounded reporting window.
// The all-sample average and extrema remain valid if an extreme cadence fills
// the buffer; the report exposes droppedSampleCount instead of hiding bias.
class BoundedMetric final
{
public:
    explicit BoundedMetric(std::size_t capacity = 4096U);

    void add(std::uint64_t value) noexcept;
    void reset() noexcept;

    [[nodiscard]] MetricSummary summarize() const;
    [[nodiscard]] bool empty() const noexcept;

private:
    std::vector<std::uint64_t> samples_{};
    std::size_t capacity_{0U};
    std::uint64_t sampleCount_{0U};
    std::uint64_t minimum_{0U};
    std::uint64_t maximum_{0U};
    long double total_{0.0L};
};

}
