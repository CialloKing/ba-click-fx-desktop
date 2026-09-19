#pragma once

#include "bafx/windows/window_observation.hpp"
#include "bafx/fx/simulation_runtime.hpp"

#include <filesystem>
#include <map>

namespace bafx::desktop
{

struct SurfaceDiagnosticState final
{
    bool enabled{false};
    bool paused{false};
    bool powerUnavailable{false};
    bool faulted{false};
    bool pointerHeld{false};
    bool ambientEnabled{false};
    bool ambientActive{false};
    bool drawableLastFrame{false};
    bool renderScheduled{false};
    std::string_view scheduleReason{"not-evaluated"};
    std::uint64_t scheduleGeneration{0U};
    std::uint64_t instanceId{0U};

    bool operator==(const SurfaceDiagnosticState&) const = default;
};

class WindowVisibilityDiagnostics final
{
public:
    [[nodiscard]] bool begin(const std::filesystem::path& logPath,
        std::uint64_t nowTickMs, bool final = false) noexcept;
    void observe(const std::filesystem::path& logPath, HWND window,
        const SurfaceDiagnosticState& state, std::uint64_t configurationGeneration,
        std::uint64_t presentedFrames, std::uint64_t lastPresentAgeMs,
        const bafx::fx::SimulationInputDiagnostics& simulation = {}) noexcept;
    void end(const std::filesystem::path& logPath) noexcept;

private:
    struct Previous final
    {
        bafx::windows::WindowObservation window{};
        SurfaceDiagnosticState state{};
        bafx::fx::SimulationInputDiagnostics simulation{};
        std::uint64_t configurationGeneration{0U};
        std::uint64_t lastReportTickMs{0U};
        bool seen{false};
        bool reported{false};
    };

    std::map<HWND, Previous> windows_{};
    bafx::windows::WindowIdentity foreground_{};
    std::uint64_t tickMs_{0U};
    bool started_{false};
    bool foregroundChanged_{false};
    bool final_{false};
};

}
