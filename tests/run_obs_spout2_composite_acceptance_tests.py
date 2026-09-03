#!/usr/bin/env python3
"""Source contract for the bounded local OBS Spout2 acceptance runner."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import unittest


if len(sys.argv) < 3:
    raise RuntimeError("expected PowerShell executable and acceptance script paths")

POWERSHELL = Path(sys.argv.pop(1))
SCRIPT = Path(sys.argv.pop(1))


class RunObsSpout2CompositeAcceptanceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = SCRIPT.read_text(encoding="utf-8-sig")

    def test_script_has_valid_powershell_syntax(self) -> None:
        parser = (
            "$tokens=$null; $errors=$null; "
            "[System.Management.Automation.Language.Parser]::ParseFile("
            "$env:BAFX_SCRIPT_UNDER_TEST,[ref]$tokens,[ref]$errors) | Out-Null; "
            "if(@($errors).Count -ne 0) { "
            "$errors | ForEach-Object { Write-Error $_.Message }; exit 1 }"
        )

        result = subprocess.run(
            [
                str(POWERSHELL),
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-Command",
                parser,
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
            encoding="utf-8",
            errors="replace",
            env={**os.environ, "BAFX_SCRIPT_UNDER_TEST": str(SCRIPT.resolve())},
        )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_uses_fixed_age_raw_bgra_and_four_background_oracle(self) -> None:
        for parameter in ("HostPath", "ProbePath", "ObsPath", "OutputDirectory"):
            self.assertIn(f"[string]${parameter}", self.source)
        for token in (
            "--demo-age-ms=130",
            "--demo-delay-ms=2000",
            "--capture-output=$activeFrame",
            "--capture-output=$activeFrameAfter",
            "DXGI_FORMAT_B8G8R8A8_UNORM",
            "SaveSourceScreenshot",
            "verify-obs-spout2-composite.py",
            "--data-base64",
            "--tolerance=3",
            "Fixed-age Spout2 frame changed while OBS captured the four backgrounds",
        ):
            self.assertIn(token, self.source)
        for case in (
            "name = 'black'; rgb = @(0, 0, 0)",
            "name = 'gray'; rgb = @(96, 96, 96)",
            "name = 'white'; rgb = @(255, 255, 255)",
            "name = 'color'; rgb = @(32, 80, 144)",
        ):
            self.assertIn(case, self.source)

    def test_builds_an_isolated_scene_without_recursive_capture(self) -> None:
        collection = self.source.index("CreateSceneCollection")
        scene = self.source.index("-Request 'CreateScene'")
        color = self.source.index("inputKind = 'color_source_v3'")
        spout = self.source.index("inputKind = 'spout_capture'")
        capture_phase = self.source.index("$captureCases = @(")
        screenshot = self.source.index("SaveSourceScreenshot", capture_phase)
        self.assertLess(collection, scene)
        self.assertLess(scene, color)
        self.assertLess(color, spout)
        self.assertLess(spout, screenshot)
        self.assertIn("$items.Count -eq 2", self.source)
        self.assertIn("GetSourceFilterList", self.source)
        self.assertIn("displayOrWindowCapturePresent = $false", self.source)
        for forbidden in (
            "monitor_capture",
            "window_capture",
            "game_capture",
            "repair-obs-spout2-scene.ps1",
        ):
            self.assertNotIn(forbidden, self.source)

    def test_requires_default_byte_domain_scene_item_method(self) -> None:
        self.assertIn("compositemode = 4", self.source)
        self.assertIn(
            "bgra8-srgb-extended-premultiplied-fx-only-v6",
            self.source,
        )
        self.assertIn("obsBlendMethod = 'default'", self.source)
        self.assertIn("obsBlendMode = 'normal'", self.source)
        self.assertIn("sceneItemBlendMode = 'OBS_BLEND_NORMAL'", self.source)
        self.assertIn("$spoutItem.blend_method -eq 'default'", self.source)
        self.assertIn("sourceSrgbAware = $false", self.source)
        self.assertIn("expectedBlendDomain = 'srgb-byte'", self.source)
        self.assertIn("temporary-scene-collection.json", self.source)
        self.assertNotIn("SetSceneItemBlendMethod", self.source)
        self.assertNotIn("OBS_BLEND_SRGB_OFF", self.source)

    def test_records_the_dynamic_idle_active_transparent_lifecycle(self) -> None:
        for token in (
            "[ValidateSet('FixedComposite', 'DynamicLifecycle')]",
            "$Mode -eq 'DynamicLifecycle'",
            "--demo-delay-ms=$lifecycleDemoDelayMilliseconds",
            "frame-baseline.png",
            "frame-idle-sender-connected.png",
            "frame-active-{0:D5}ms.png",
            "frame-final-transparent.png",
            "receiver-lifecycle-verification.json",
            "verify-obs-spout2-evidence.py",
            "obs-lifecycle-video-only.mp4",
            "threeStageLifecycleVerified",
        ):
            self.assertIn(token, self.source)
        self.assertIn("$lifecycleCanvasWidth = 1280", self.source)
        self.assertIn("$lifecycleCanvasHeight = 720", self.source)
        self.assertIn("$spoutScale = [double]$canvasWidth / [double]$frameWidth", self.source)
        dynamic_phase = self.source.index("$sceneReadyElapsed")
        start_record = self.source.index("-Request 'StartRecord'", dynamic_phase)
        active_capture = self.source.index("frame-active-{0:D5}ms.png")
        stop_record = self.source.index("-Request 'StopRecord'", start_record)
        self.assertLess(start_record, active_capture)
        self.assertLess(active_capture, stop_record)

    def test_records_and_decodes_the_isolated_scene(self) -> None:
        for token in (
            "SetRecordDirectory",
            "StartRecord",
            "GetRecordStatus",
            "StopRecord",
            "verify-obs-spout2-recording.py",
            "referenceImage = $recordingReferenceName",
            "recording-frame.png",
            "recording-ffprobe.json",
            "decoded-recording-frame-vs-obs-scene-png",
            "RecQuality'; value = 'Stream'",
            "VBitrate'; value = '20000'",
            "GetRecordDirectory",
            "Wait-ForStableFile -Path $recordingRawPath",
        ):
            self.assertIn(token, self.source)
        self.assertIn("$recordingStarted", self.source)
        self.assertIn("durationMilliseconds", self.source)
        self.assertLess(
            self.source.index("$recordingStarted = $true"),
            self.source.index("-Request 'StartRecord'"),
        )

    def test_removes_audio_from_retained_recording_evidence(self) -> None:
        for token in (
            "GetSpecialInputs",
            "SetInputMute",
            "GetInputMute",
            "'-c:v' 'copy'",
            "'-an'",
            "audioStreams.Count -eq 0",
            "videoPacketsCopiedWithoutReencoding = $true",
            "Remove-GeneratedRecordingDirectory",
            "temporaryRawContainerRemoved",
        ):
            self.assertIn(token, self.source)

    def test_validates_obs_plugin_log_and_utf8_scene_json(self) -> None:
        for token in (
            "Get-ObsSessionLogContract",
            "win-spout loaded!",
            "has changed\\s*/\\s*gone away",
            "sender not found",
            "Failed to create source",
            "senderAcquisitionCount",
            "[IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8)",
        ):
            self.assertIn(token, self.source)
        self.assertNotIn("Get-Content -LiteralPath $file.FullName -Raw", self.source)

    def test_restores_managed_external_configuration_in_finally(self) -> None:
        finally_block = self.source.index("finally\n{")
        recording_status = self.source.index("-Request 'GetRecordStatus'", finally_block)
        collection_restore = self.source.index("-Request 'SetCurrentSceneCollection'", finally_block)
        restoration = self.source.index("Restore-ManagedState", finally_block)
        summary = self.source.index("externalConfigurationRestoreExact", restoration)
        self.assertLess(recording_status, collection_restore)
        self.assertLess(finally_block, restoration)
        self.assertLess(restoration, summary)
        for token in (
            "external-state-before.json",
            "external-state-after.json",
            "Backup-ManagedState",
            "Assert-StatesEqual",
            "Get-FileHash",
            "Refusing to remove an unmanaged path",
            "Output directory already exists; refusing to overwrite evidence",
            "OutputDirectory must be outside the OBS configuration root",
            "$sentinelRoot = Join-Path $configRoot '.sentinel'",
            "OBS 32 ignores --disable-shutdown-check",
            "'^run_[0-9a-f-]+$'",
            "SHA256SUMS.txt",
        ):
            self.assertIn(token, self.source)


if __name__ == "__main__":
    unittest.main()
