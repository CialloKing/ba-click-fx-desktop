#include "test_support.hpp"

#include "background_capture_runtime.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{

class TemporaryBackgroundCaptureLog final
{
public:
    TemporaryBackgroundCaptureLog()
    {
        directory_ = std::filesystem::temp_directory_path()
            / ("bafx-background-capture-log-"
                + std::to_string(GetCurrentProcessId())
                + "-"
                + std::to_string(GetTickCount64()));
        std::filesystem::create_directories(directory_);
        path_ = directory_ / "support.log";
    }

    ~TemporaryBackgroundCaptureLog()
    {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    TemporaryBackgroundCaptureLog(const TemporaryBackgroundCaptureLog&) = delete;
    TemporaryBackgroundCaptureLog& operator=(
        const TemporaryBackgroundCaptureLog&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

    [[nodiscard]] std::string read() const
    {
        std::ifstream input(path_, std::ios::binary);
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    }

private:
    std::filesystem::path directory_{};
    std::filesystem::path path_{};
};

}

BAFX_TEST(background_snapshot_invalidation_log_preserves_causal_identity)
{
    const TemporaryBackgroundCaptureLog log;
    bafx::desktop::appendBackgroundSnapshotInvalidation(
        log.path(),
        11U,
        bafx::windows::BackgroundSnapshotInvalidation{
            bafx::windows::BackgroundSnapshotInvalidationReason::
                WgcSessionStopped,
            13U,
            17U,
            19U,
            23U,
            29U});

    const std::string contents = log.read();
    BAFX_CHECK(contents.find("Event.Level=Warning") != std::string::npos);
    BAFX_CHECK(
        contents.find("Event.Name=BackgroundSnapshot.Invalidated")
        != std::string::npos);
    BAFX_CHECK(contents.find("Control.Generation=11") != std::string::npos);
    BAFX_CHECK(contents.find("Frame.Id=13") != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Epoch=17") != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Generation=19") != std::string::npos);
    BAFX_CHECK(
        contents.find("BackgroundSnapshot.Epoch=23") != std::string::npos);
    BAFX_CHECK(
        contents.find("BackgroundSnapshot.Generation=29")
        != std::string::npos);
    BAFX_CHECK(
        contents.find(
            "BackgroundSnapshot.InvalidationReason=wgc-session-stopped")
        != std::string::npos);
}

BAFX_TEST(background_stop_observer_persists_the_uncancellable_stage_boundary)
{
    const TemporaryBackgroundCaptureLog log;
    const bafx::windows::WgcBackgroundStopObserver observer =
        bafx::desktop::backgroundCaptureStopObserver(log.path());
    observer.notify(bafx::windows::WgcBackgroundStopProgress{
        bafx::windows::WgcBackgroundStopStage::SessionClose,
        bafx::windows::WgcBackgroundStopStageState::Begin,
        17U,
        19U});

    const std::string contents = log.read();
    BAFX_CHECK(contents.find("Event.Level=Warning") != std::string::npos);
    BAFX_CHECK(
        contents.find("Event.Name=BackgroundCapture.StopProgress")
        != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Stop.Stage=session-close")
        != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Stop.StageState=begin")
        != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Stop.OwnerThreadId=17")
        != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Stop.CallerThreadId=19")
        != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Stop.OwnerThreadMatched=false")
        != std::string::npos);
}

BAFX_TEST(background_composite_participation_log_keeps_legacy_marker)
{
    const TemporaryBackgroundCaptureLog log;
    bafx::windows::CompositionFrameDiagnostics diagnostics{};
    diagnostics.frameId = 31U;
    diagnostics.wgc.epoch = 37U;
    diagnostics.wgc.acceptedGeneration = 41U;
    diagnostics.backgroundSnapshotEpoch = 43U;
    diagnostics.backgroundSnapshotGeneration = 47U;
    diagnostics.backgroundParticipated = true;

    bafx::desktop::appendBackgroundCompositeParticipation(
        log.path(),
        53U,
        diagnostics);

    const std::string contents = log.read();
    BAFX_CHECK(contents.find("Event.Level=Info") != std::string::npos);
    BAFX_CHECK(
        contents.find("Event.Name=BackgroundComposite.Participated")
        != std::string::npos);
    BAFX_CHECK(contents.find("Control.Generation=53") != std::string::npos);
    BAFX_CHECK(contents.find("Frame.Id=31") != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Epoch=37") != std::string::npos);
    BAFX_CHECK(contents.find("WGC.Generation=41") != std::string::npos);
    BAFX_CHECK(
        contents.find("BackgroundSnapshot.Epoch=43") != std::string::npos);
    BAFX_CHECK(
        contents.find("BackgroundSnapshot.Generation=47")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("Event.Name=Message\nEvent.Message="
                      "WGC background sample entered the final desktop composite")
        != std::string::npos);
}

BAFX_TEST(background_composite_participation_log_rejects_unpresented_frame)
{
    const TemporaryBackgroundCaptureLog log;
    bafx::windows::CompositionFrameDiagnostics diagnostics{};
    diagnostics.frameId = 59U;

    bafx::desktop::appendBackgroundCompositeParticipation(
        log.path(),
        61U,
        diagnostics);

    BAFX_CHECK(!std::filesystem::exists(log.path()));
}

BAFX_TEST(borderless_access_log_preserves_permission_decision)
{
    const TemporaryBackgroundCaptureLog log;
    const bafx::windows::BorderlessCaptureAccessResult result{
        bafx::windows::BorderlessCaptureAccessStatus::NotPackaged,
        HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE),
        bafx::windows::BorderlessCaptureAccessAsyncStatus::Canceled,
        4321U,
        true};

    bafx::desktop::appendBorderlessCaptureAccessCheck(
        log.path(),
        67U,
        3U,
        result);

    const std::string contents = log.read();
    BAFX_CHECK(contents.find("Event.Level=Warning") != std::string::npos);
    BAFX_CHECK(
        contents.find("Event.Name=WGC.BorderlessAccess.Checked")
        != std::string::npos);
    BAFX_CHECK(contents.find("Control.Generation=67") != std::string::npos);
    BAFX_CHECK(
        contents.find("Transaction.ActionIndex=3") != std::string::npos);
    BAFX_CHECK(
        contents.find("Background.AllowSystemBorder=false")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.BorderlessAccess.Status=not-packaged")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.BorderlessAccess.HRESULT=0x80073D54")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.BorderlessAccess.AsyncStatus=canceled")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.BorderlessAccess.ElapsedMs=4321")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.BorderlessAccess.CancelRequested=true")
        != std::string::npos);
    BAFX_CHECK(
        contents.find("WGC.BorderlessAccess.Allowed=false")
        != std::string::npos);
}
