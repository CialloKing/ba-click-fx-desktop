#include "performance_window.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace bafx::desktop
{
namespace
{

[[nodiscard]] std::uint64_t percentile(
    const std::vector<std::uint64_t>& sorted,
    const double fraction) noexcept
{
    if (sorted.empty())
    {
        return 0U;
    }

    const double rank = std::ceil(
        fraction * static_cast<double>(sorted.size()));
    const std::size_t index = static_cast<std::size_t>(
        std::max(1.0, rank) - 1.0);
    return sorted[std::min(index, sorted.size() - 1U)];
}

}

BoundedMetric::BoundedMetric(const std::size_t capacity)
    : capacity_(capacity)
{
    if (capacity_ == 0U)
    {
        throw std::invalid_argument("Metric sample capacity must be positive");
    }
    samples_.reserve(capacity_);
}

void BoundedMetric::add(const std::uint64_t value) noexcept
{
    if (sampleCount_ == 0U)
    {
        minimum_ = value;
        maximum_ = value;
    }
    else
    {
        minimum_ = std::min(minimum_, value);
        maximum_ = std::max(maximum_, value);
    }

    ++sampleCount_;
    total_ += static_cast<long double>(value);
    if (samples_.size() < capacity_)
    {
        samples_.push_back(value);
    }
}

void BoundedMetric::reset() noexcept
{
    samples_.clear();
    sampleCount_ = 0U;
    minimum_ = 0U;
    maximum_ = 0U;
    total_ = 0.0L;
}

MetricSummary BoundedMetric::summarize() const
{
    if (sampleCount_ == 0U)
    {
        return {};
    }

    std::vector<std::uint64_t> sorted(samples_);
    std::sort(sorted.begin(), sorted.end());
    return MetricSummary{
        sampleCount_,
        static_cast<std::uint64_t>(sorted.size()),
        sampleCount_ - static_cast<std::uint64_t>(sorted.size()),
        minimum_,
        percentile(sorted, 0.50),
        percentile(sorted, 0.95),
        percentile(sorted, 0.99),
        maximum_,
        static_cast<double>(total_ / static_cast<long double>(sampleCount_))};
}

bool BoundedMetric::empty() const noexcept
{
    return sampleCount_ == 0U;
}

}
