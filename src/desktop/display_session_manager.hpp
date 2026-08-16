#pragma once

#include "display_session.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bafx::desktop
{

struct DisplaySessionManagerOptions final
{
    HINSTANCE instance{nullptr};
    HWND wakeWindow{nullptr};
    bafx::windows::BorderlessCaptureAccessAuthority*
        borderlessAccessAuthority{nullptr};
    std::wstring_view surfaceTitle{};
    bafx::windows::FxBloomSettings bloomSettings{};
    bafx::windows::WgcBackgroundStopObserver backgroundStopObserver{};
    bafx::windows::CompositionOutputPreference outputPreference{
        bafx::windows::CompositionOutputPreference::ConservativeSdr};
    std::uint64_t simulationSeed{0U};
    float trailLengthMultiplier{1.0F};
    std::uint32_t inputSamplingRateHz{0U};
    bool alwaysOnTrailEnabled{false};
};

struct DisplaySessionFailure final
{
    DisplayTarget target{};
    std::string operation{};
    std::string message{};
};

struct DisplaySessionReconcileResult final
{
    bafx::windows::DisplayTopologyStatus topologyStatus{
        bafx::windows::DisplayTopologyStatus::QueryFailed};
    LONG topologyError{ERROR_GEN_FAILURE};
    std::size_t added{0U};
    std::size_t updated{0U};
    std::size_t recreated{0U};
    std::size_t removed{0U};
    bool removalsDeferred{false};
    std::vector<DisplaySessionFailure> failures{};
};

// Reconciles independent per-display resource domains. The coordinator keeps
// the Host-owned WGC transaction while every secondary owns an equivalent
// session-local capture state machine.
class DisplaySessionManager final
{
public:
    explicit DisplaySessionManager(DisplaySessionManagerOptions options);

    DisplaySessionManager(const DisplaySessionManager&) = delete;
    DisplaySessionManager& operator=(const DisplaySessionManager&) = delete;

    [[nodiscard]] DisplaySession& createCoordinator(DisplayTarget target);
    [[nodiscard]] DisplaySession& coordinator();
    [[nodiscard]] const DisplaySession& coordinator() const;
    [[nodiscard]] DisplaySessionReconcileResult reconcileSecondaries(
        const DisplayTargetSnapshot& snapshot);
    // DRR and some dock transitions do not reliably emit a window message.
    // Compare one shared snapshot before entering the heavier reconciliation
    // transaction so the periodic fallback stays silent while state is stable.
    [[nodiscard]] bool topologyDiffers(
        const DisplayTargetSnapshot& snapshot) const noexcept;
    [[nodiscard]] std::size_t pruneCoordinatorDuplicates() noexcept;
    void updateCreationSettings(
        bafx::windows::FxBloomSettings bloomSettings,
        bafx::windows::CompositionOutputPreference outputPreference,
        float trailLengthMultiplier,
        std::uint32_t inputSamplingRateHz,
        bool alwaysOnTrailEnabled) noexcept;

    [[nodiscard]] DisplaySession* findBySource(
        const DisplayTarget& target) noexcept;
    [[nodiscard]] const DisplaySession* findBySource(
        const DisplayTarget& target) const noexcept;
    [[nodiscard]] DisplaySession* findAtPoint(POINT point) noexcept;
    [[nodiscard]] const std::vector<std::unique_ptr<DisplaySession>>&
        sessions() const noexcept;

private:
    [[nodiscard]] DisplaySession* findForReconciliation(
        const DisplayTarget& target) noexcept;
    [[nodiscard]] const DisplaySession* findForReconciliation(
        const DisplayTarget& target) const noexcept;
    [[nodiscard]] std::unique_ptr<DisplaySession> createSession(
        DisplayTarget target);
    [[nodiscard]] static bool targetPresent(
        const DisplayTargetSnapshot& snapshot,
        const DisplayTarget& target) noexcept;
    [[nodiscard]] std::uint64_t nextSimulationSeed() noexcept;

    HINSTANCE instance_{nullptr};
    HWND wakeWindow_{nullptr};
    bafx::windows::BorderlessCaptureAccessAuthority*
        borderlessAccessAuthority_{nullptr};
    std::wstring surfaceTitle_{};
    bafx::windows::FxBloomSettings bloomSettings_{};
    bafx::windows::WgcBackgroundStopObserver backgroundStopObserver_{};
    bafx::windows::CompositionOutputPreference outputPreference_{
        bafx::windows::CompositionOutputPreference::ConservativeSdr};
    std::uint64_t simulationSeed_{0U};
    std::uint64_t sessionSequence_{0U};
    float trailLengthMultiplier_{1.0F};
    std::uint32_t inputSamplingRateHz_{0U};
    bool alwaysOnTrailEnabled_{false};
    DisplaySession* coordinator_{nullptr};
    std::vector<std::unique_ptr<DisplaySession>> sessions_{};
};

}
