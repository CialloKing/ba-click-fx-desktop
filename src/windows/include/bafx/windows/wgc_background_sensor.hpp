#pragma once

#include "bafx/core/background_freshness.hpp"
#include "bafx/windows/overlay_window.hpp"

#include <d3d11.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

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

// Keep the production lifecycle evidence in the same stable key/value form
// as the support log.  The formatter is also used by offline tests so a
// future resource can not silently disappear from diagnostics.
[[nodiscard]] std::string wgcBackgroundResourceLedgerDiagnostic(
    const WgcBackgroundResourceLedgerSnapshot& snapshot);

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
    // Hardware probes can require an explicit enable/disable write and
    // readback. Product callers leave this unset to preserve old-OS fallback.
    std::optional<bool> cursorCaptureEnabledOverride{};
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
    bool cursorCaptureEnabled{false};
    bool cursorControlConfirmed{false};
};

enum class WgcBackgroundDrainStatus : std::uint8_t
{
    NoFrame,
    Updated,
    ReconfigureRequired,
    Stopped
};

struct WgcBackgroundDrainDiagnostics
{
    WgcBackgroundDrainStatus status{WgcBackgroundDrainStatus::NoFrame};
    std::uint32_t framesAcquired{0U};
    std::uint32_t framesSuperseded{0U};
    std::uint32_t timestampRejectedFrames{0U};
    bool ownedCopySubmitted{false};
    bool accepted{false};
    std::uint64_t epoch{0U};
    std::uint64_t frameArrivedCallbacksTotal{0U};
    std::uint64_t acceptedGeneration{0U};
    // This is only the CPU duration of issuing CopySubresourceRegion.
    std::chrono::nanoseconds ownedCopySubmitCpu{};
};

struct WgcBackgroundTransportSnapshot
{
    std::uint64_t epoch{0U};
    std::uint64_t frameArrivedCallbacksTotal{0U};
    std::uint64_t acceptedGeneration{0U};
    bool running{false};
    bool itemClosed{false};
};

struct WgcBackgroundStopDiagnostics
{
    std::chrono::nanoseconds frameArrivedUnregister{};
    std::chrono::nanoseconds itemClosedUnregister{};
    std::chrono::nanoseconds sessionClose{};
    std::chrono::nanoseconds framePoolClose{};
    std::chrono::nanoseconds total{};
    bool sensorPresent{false};
    bool completed{false};
};

[[nodiscard]] std::string wgcBackgroundStopDiagnostic(
    const WgcBackgroundStopDiagnostics& diagnostics);

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
    [[nodiscard]] WgcBackgroundDrainDiagnostics drainLatestDetailed(
        ID3D11DeviceContext* context);
    [[nodiscard]] std::optional<WindowSize> pendingFramePoolSize() const noexcept;
    // Must run on the same owner that drains and uses the immediate context.
    void recreateFramePool(WindowSize size);
    [[nodiscard]] std::optional<WgcBackgroundSample> latestSample() const noexcept;
    // Idle render frames must observe producer progress without touching the
    // capture queue or issuing a full-screen copy on the immediate context.
    [[nodiscard]] WgcBackgroundTransportSnapshot transportSnapshot() const noexcept;
    [[nodiscard]] std::uint64_t expectedEpoch() const noexcept;
    [[nodiscard]] WgcBackgroundSessionCapabilities capabilities() const noexcept;
    [[nodiscard]] WgcBackgroundResourceLedgerSnapshot resourceLedger() const noexcept;
    [[nodiscard]] WgcBackgroundStopDiagnostics stopDiagnostics() const noexcept;
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
