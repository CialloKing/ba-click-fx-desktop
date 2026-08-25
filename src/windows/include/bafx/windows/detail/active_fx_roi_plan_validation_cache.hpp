#pragma once

#include "bafx/core/roi.hpp"
#include "bafx/core/unity_bloom.hpp"

#include <optional>

namespace bafx::windows::detail
{

struct ActiveFxRoiPlanValidationResult
{
    bool valid{false};
    bool cacheHit{false};
};

// Cache only a planner-produced plan. A candidate still has to match every
// semantic field before it can reuse that prior validation.
class ActiveFxRoiPlanValidationCache final
{
public:
    [[nodiscard]] ActiveFxRoiPlanValidationResult validate(
        const bafx::core::UnityBloomPassRoiPlan& candidate,
        bafx::core::RectI monitorBounds,
        const bafx::core::UnityBloomPlan& bloomPlan) noexcept;

    void reset() noexcept;

private:
    struct Entry final
    {
        bafx::core::RectI monitorBounds{};
        bafx::core::UnityBloomPlan bloomPlan{};
        bafx::core::UnityBloomPassRoiPlan expected{};
    };

    std::optional<Entry> entry_{};
};

}
