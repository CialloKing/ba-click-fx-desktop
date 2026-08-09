#pragma once

#include "bafx/core/background_freshness.hpp"
#include "bafx/windows/overlay_window.hpp"

#include <d3d11.h>
#include <windows.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace bafx::windows
{

struct WgcBackgroundSensorOptions
{
    std::uint64_t epoch{1U};
    bool excludesOwnOverlay{false};
};

struct WgcBackgroundSample
{
    // The sensor owns this view. It remains valid until stop or a size change.
    ID3D11ShaderResourceView* texture{nullptr};
    bafx::core::BackgroundFrameStamp stamp{};
    WindowSize size{};
    std::uint64_t generation{0U};
};

enum class WgcBackgroundDrainStatus : std::uint8_t
{
    NoFrame,
    Updated,
    Reconfigured,
    Stopped
};

class WgcBackgroundSensor final
{
public:
    WgcBackgroundSensor(
        ID3D11Device* device,
        HMONITOR monitor,
        WgcBackgroundSensorOptions options = {});
    ~WgcBackgroundSensor();

    WgcBackgroundSensor(const WgcBackgroundSensor&) = delete;
    WgcBackgroundSensor& operator=(const WgcBackgroundSensor&) = delete;

    [[nodiscard]] static bool isSupported() noexcept;

    // Must be called by the owner of the D3D11 immediate context.
    [[nodiscard]] WgcBackgroundDrainStatus drainLatest(
        ID3D11DeviceContext* context);
    [[nodiscard]] std::optional<WgcBackgroundSample> latestSample() const noexcept;
    [[nodiscard]] HANDLE frameAvailableObject() const noexcept;
    [[nodiscard]] bool running() const noexcept;

    void stop() noexcept;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}

