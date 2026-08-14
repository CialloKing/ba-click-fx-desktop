#pragma once

#include "bafx/core/background_freshness.hpp"
#include "bafx/windows/overlay_window.hpp"

#include <d3d11.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

namespace bafx::windows
{

struct WgcBackgroundResourceLedgerSnapshot
{
    std::uint64_t framesAcquired{0U};
    std::uint64_t framesClosed{0U};
    std::uint64_t framePoolsCreated{0U};
    std::uint64_t framePoolsClosed{0U};
    std::uint64_t framePoolsRecreated{0U};
    std::uint64_t sessionsCreated{0U};
    std::uint64_t sessionsClosed{0U};
    std::uint64_t frameArrivedRegistrations{0U};
    std::uint64_t frameArrivedUnregistrations{0U};
    std::uint64_t itemClosedRegistrations{0U};
    std::uint64_t itemClosedUnregistrations{0U};
    std::uint64_t liveFrames{0U};
    std::uint64_t liveFramePools{0U};
    std::uint64_t liveSessions{0U};
    std::uint64_t liveFrameArrivedRegistrations{0U};
    std::uint64_t liveItemClosedRegistrations{0U};
    std::uint64_t failures{0U};

    [[nodiscard]] bool allReleased() const noexcept;
};

class WgcBackgroundResourceLedger final
{
public:
    [[nodiscard]] WgcBackgroundResourceLedgerSnapshot snapshot() const noexcept;

private:
    friend class WgcBackgroundSensor;

    std::atomic<std::uint64_t> framesAcquired_{0U};
    std::atomic<std::uint64_t> framesClosed_{0U};
    std::atomic<std::uint64_t> framePoolsCreated_{0U};
    std::atomic<std::uint64_t> framePoolsClosed_{0U};
    std::atomic<std::uint64_t> framePoolsRecreated_{0U};
    std::atomic<std::uint64_t> sessionsCreated_{0U};
    std::atomic<std::uint64_t> sessionsClosed_{0U};
    std::atomic<std::uint64_t> frameArrivedRegistrations_{0U};
    std::atomic<std::uint64_t> frameArrivedUnregistrations_{0U};
    std::atomic<std::uint64_t> itemClosedRegistrations_{0U};
    std::atomic<std::uint64_t> itemClosedUnregistrations_{0U};
    std::atomic<std::uint64_t> liveFrames_{0U};
    std::atomic<std::uint64_t> liveFramePools_{0U};
    std::atomic<std::uint64_t> liveSessions_{0U};
    std::atomic<std::uint64_t> liveFrameArrivedRegistrations_{0U};
    std::atomic<std::uint64_t> liveItemClosedRegistrations_{0U};
    std::atomic<std::uint64_t> failures_{0U};
};

struct WgcBackgroundSensorOptions
{
    std::uint64_t epoch{1U};
    bool excludesOwnOverlay{false};
    // Cursor capture is optional because older Windows builds may not expose
    // IGraphicsCaptureSession2. The product default keeps this enabled.
    bool cursorExcluded{true};
    // Windows may require a visible privacy border for WGC. Keep it opt-in so
    // the default desktop experience falls back instead of showing a yellow frame.
    bool allowSystemBorder{false};
    // A caller-owned ledger keeps constructor-failure evidence available to
    // hardware collectors after a sensor instance fails to materialize.
    std::shared_ptr<WgcBackgroundResourceLedger> resourceLedger{};
};

struct WgcBackgroundSample
{
    // The sensor owns this view. It remains valid until stop or a size change.
    ID3D11ShaderResourceView* texture{nullptr};
    bafx::core::BackgroundFrameStamp stamp{};
    WindowSize size{};
    std::uint64_t generation{0U};
};

struct WgcBackgroundSessionCapabilities
{
    // A false value is accepted only when allowSystemBorder was requested;
    // otherwise sensor construction fails before StartCapture.
    bool borderHidden{false};
    bool cursorExcluded{false};
};

enum class WgcBackgroundDrainStatus : std::uint8_t
{
    NoFrame,
    Updated,
    ReconfigureRequired,
    Stopped
};

class WgcBackgroundSensor final
{
public:
    WgcBackgroundSensor(
        ID3D11Device* device,
        HMONITOR monitor,
        WgcBackgroundSensorOptions options = {});
    WgcBackgroundSensor(
        ID3D11Device* device,
        HWND window,
        WgcBackgroundSensorOptions options = {});
    ~WgcBackgroundSensor();

    WgcBackgroundSensor(const WgcBackgroundSensor&) = delete;
    WgcBackgroundSensor& operator=(const WgcBackgroundSensor&) = delete;

    [[nodiscard]] static bool isSupported() noexcept;

    // Must be called by the owner of the D3D11 immediate context.
    [[nodiscard]] WgcBackgroundDrainStatus drainLatest(
        ID3D11DeviceContext* context);
    [[nodiscard]] std::optional<WindowSize> pendingFramePoolSize() const noexcept;
    // Must run on the same owner that drains and uses the immediate context.
    void recreateFramePool(WindowSize size);
    [[nodiscard]] std::optional<WgcBackgroundSample> latestSample() const noexcept;
    [[nodiscard]] std::uint64_t expectedEpoch() const noexcept;
    [[nodiscard]] WgcBackgroundSessionCapabilities capabilities() const noexcept;
    [[nodiscard]] WgcBackgroundResourceLedgerSnapshot resourceLedger() const noexcept;
    [[nodiscard]] HANDLE frameAvailableObject() const noexcept;
    [[nodiscard]] bool running() const noexcept;

    void stop() noexcept;

private:
    enum class ResourceLedgerEvent : std::uint8_t;
    static void recordResourceLedgerEvent(
        const std::shared_ptr<WgcBackgroundResourceLedger>& ledger,
        ResourceLedgerEvent event) noexcept;

    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}
