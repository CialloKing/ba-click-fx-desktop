#include "test_support.hpp"

#include "bafx/windows/composition_renderer.hpp"

#include <optional>

namespace
{

constexpr bafx::windows::WindowSize outputSize{100U, 80U};
constexpr bafx::core::RectI dirtyRect{10, 20, 30, 40};

[[nodiscard]] bafx::core::UnityBloomPassRoiPlan validPlan() noexcept
{
    bafx::core::UnityBloomPassRoiPlan plan{};
    plan.resolveRect = dirtyRect;
    return plan;
}

[[nodiscard]] bafx::windows::FxActiveRoiPassDiagnostics
validDiagnostics() noexcept
{
    bafx::windows::FxActiveRoiPassDiagnostics diagnostics{};
    diagnostics.requested = true;
    diagnostics.eligible = true;
    diagnostics.executed = true;
    diagnostics.actualPath =
        bafx::windows::FxActiveRoiActualPath::RoiPyramid;
    diagnostics.decisionReason =
        bafx::windows::FxActiveRoiDecisionReason::Applied;
    diagnostics.stages.resolve.fullPixels = 8'000U;
    diagnostics.stages.resolve.candidatePixels = 400U;
    diagnostics.stages.resolve.drawnPixels = 400U;
    diagnostics.stages.resolve.clearedPixels = 400U;
    diagnostics.partialFinalOutput = true;
    return diagnostics;
}

}

BAFX_TEST(active_fx_present_accepts_only_a_verified_partial_output)
{
    const auto selected = bafx::windows::selectActiveFxPresentDirtyRect(
        validPlan(),
        validDiagnostics(),
        outputSize);
    BAFX_CHECK(selected.has_value());
    BAFX_CHECK(selected->left == dirtyRect.left);
    BAFX_CHECK(selected->top == dirtyRect.top);
    BAFX_CHECK(selected->right == dirtyRect.right);
    BAFX_CHECK(selected->bottom == dirtyRect.bottom);
}

BAFX_TEST(active_fx_present_rejects_warmup_and_full_output_writes)
{
    auto warmup = validDiagnostics();
    warmup.warmup = true;
    warmup.actualPath = bafx::windows::FxActiveRoiActualPath::RoiWarmup;
    BAFX_CHECK(!bafx::windows::selectActiveFxPresentDirtyRect(
        validPlan(),
        warmup,
        outputSize).has_value());

    auto fullOutput = validDiagnostics();
    fullOutput.partialFinalOutput = false;
    BAFX_CHECK(!bafx::windows::selectActiveFxPresentDirtyRect(
        validPlan(),
        fullOutput,
        outputSize).has_value());
}

BAFX_TEST(active_fx_present_rejects_unverified_or_inconsistent_regions)
{
    BAFX_CHECK(!bafx::windows::selectActiveFxPresentDirtyRect(
        std::nullopt,
        validDiagnostics(),
        outputSize).has_value());

    auto inconsistent = validDiagnostics();
    --inconsistent.stages.resolve.clearedPixels;
    BAFX_CHECK(!bafx::windows::selectActiveFxPresentDirtyRect(
        validPlan(),
        inconsistent,
        outputSize).has_value());

    auto outside = validPlan();
    outside.resolveRect.right = 101;
    BAFX_CHECK(!bafx::windows::selectActiveFxPresentDirtyRect(
        outside,
        validDiagnostics(),
        outputSize).has_value());
}
