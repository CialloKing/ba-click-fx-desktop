#!/usr/bin/env python3
"""Contract tests for the paired performance baseline reporter."""

from __future__ import annotations

import importlib.util
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest


SCRIPT_PATH = (
    Path(__file__).resolve().parents[1]
    / "tools"
    / "report-performance-baseline.py"
)
SPEC = importlib.util.spec_from_file_location(
    "report_performance_baseline", SCRIPT_PATH
)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"unable to load {SCRIPT_PATH}")
REPORTER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = REPORTER
SPEC.loader.exec_module(REPORTER)


def _event(name: str, session: str, fields: dict[str, object]) -> str:
    lines = [
        "Log.SchemaVersion=2",
        f"Log.SessionId={session}",
        f"Event.Name={name}",
    ]
    lines.extend(f"{key}={value}" for key, value in fields.items())
    return "\n".join(lines)


def _interval(mode: str, background_aware: bool) -> dict[str, object]:
    available = "true" if background_aware else "false"
    wgc_frames = 900 if background_aware else 0
    return {
        "Background.Mode": mode,
        "Output.Width": 2560,
        "Output.Height": 1440,
        "Window.Final": "false",
        "Window.DurationUs": 10_000_000,
        "Window.FrameCount": 1200,
        "Window.PresentedFps": 120.0,
        "Background.CompositeStatus": (
            "participating" if background_aware else "inactive"
        ),
        "WGC.ActiveFrames": wgc_frames,
        "WGC.DrainAttemptedFrames": wgc_frames,
        "WGC.ProducerCallbackFps": 90.0 if background_aware else 0.0,
        "WGC.AcceptedFps": 88.0 if background_aware else 0.0,
        "WGC.FramesAcquired": wgc_frames,
        "WGC.FramesSuperseded": 0,
        "WGC.TimestampRejectedFrames": 0,
        "WGC.OwnedCopiesSubmitted": wgc_frames,
        "WGC.SamplesAccepted": wgc_frames,
        "Background.SnapshotAttempts": wgc_frames,
        "Background.SnapshotsRefreshed": wgc_frames,
        "Background.ParticipatingFrames": wgc_frames,
        "Input.RawMessages": 0,
        "Input.OverflowMoveDrops": 0,
        "MessagePump.OtherDispatched": 710,
        "MessagePump.InputBudgetExhaustions": 0,
        "MessagePump.OtherBudgetExhaustions": 0,
        "FramePacing.FrameReadyWakes": 1200,
        "FramePacing.MessageWakes": 3,
        "FramePacing.Timeouts": 0,
        "FramePacing.Failures": 0,
        "GPU.TimestampProfiler.Available": "true",
        "GPU.TimestampProfiler.Observed": "true",
        "GPU.FramesSubmitted": 1200,
        "GPU.SamplesCompleted": 1190,
        "GPU.RingFullSkipped": 0,
        "GPU.DisjointSamples": 0,
        "GPU.QueryFailures": 0,
        "GPU.StateErrors": 0,
        "GPU.PendingFrames.Max": 1,
        "GPU.WgcDrainAndCopy.Available": available,
        "GPU.WgcDrainAndCopy.P95": 300 if background_aware else 0,
        "GPU.BloomAndFinalComposite.P50": 700,
        "GPU.BloomAndFinalComposite.P95": 800,
        "GPU.BloomAndFinalComposite.P99": 900,
        "GPU.BloomAndFinalComposite.Max": 1000,
        "Cpu.PresentCall.P50": 200,
        "Cpu.PresentCall.P95": 300,
        "Cpu.PresentCall.P99": 400,
        "Cpu.PresentCall.Max": 500,
        "Cpu.FrameTotal.Max": 2000,
        "Cpu.FrameTotal.DroppedSamples": 0,
        "Cpu.PresentCall.DroppedSamples": 0,
        "GPU.BloomAndFinalComposite.DroppedSamples": 0,
    }


class CaptureFixture:
    def __init__(self, root: Path):
        self.root = root
        self.intervals: dict[str, dict[str, object]] = {}
        manifest = {
            "schemaVersion": 1,
            "scenarioId": "p0-static-click-message-pressure-v2",
            "captureStatus": "captured",
            "revision": "0123456789abcdef0123456789abcdef01234567",
            "workingTreeDirty": False,
            "executableSha256": hashlib.sha256(b"test-host").hexdigest(),
            "capturedAtUtc": "2026-08-15T00:00:00.000Z",
            "durationMs": 10500,
            "demoAgeMs": 130,
            "demoDelayMs": 50,
            "messageCount": 705,
            "messageIntervalMs": 5,
            "rawInputRegistration": "disabled",
            "modes": {},
        }
        for name, background_mode in (
            ("fx-only", "recording-compatible"),
            ("background-aware", "background-aware"),
        ):
            directory = root / name
            directory.mkdir()
            (directory / REPORTER.HOST_NAME).write_bytes(b"test-host")
            configuration = {
                "schemaVersion": 7,
                "background": {"mode": background_mode},
                "effects": {"bloomQuality": "high"},
            }
            (directory / REPORTER.CONFIG_NAME).write_text(
                json.dumps(configuration), encoding="utf-8"
            )
            manifest["modes"][name] = {
                "backgroundMode": background_mode,
                "postedMessages": 705,
                "exitCode": 0,
                "commandLine": [
                    REPORTER.HOST_NAME,
                    "--demo-age-ms=130",
                    "--demo-delay-ms=50",
                    "--disable-raw-input",
                ],
            }
            interval = _interval(
                background_mode, name == "background-aware"
            )
            self.intervals[name] = interval
            self._write_log(name)
        (root / "capture.json").write_text(
            json.dumps(manifest), encoding="utf-8"
        )

    def _write_log(self, name: str) -> None:
        session = f"session-{name}"
        support = {
            "Product.Version": "0.1.0-test",
            "Support.WGC": "active" if name == "background-aware" else "fallback-fx-only",
            "Graphics.DriverType": "Hardware",
            "Graphics.Adapter": "Test GPU",
            "Graphics.AdapterLuid": "00000000:00000001",
            "Graphics.DriverVersion": "1.2.3.4",
            "Graphics.HardwareFallback": "none",
            "Display.Primary": "2560x1440@0,0",
            "Display.PrimaryDpi": 120,
            "Display.RefreshRateNumerator": 120,
            "Display.RefreshRateDenominator": 1,
            "Display.RefreshRateHz": 120.0,
        }
        mode = "background-aware" if name == "background-aware" else "recording-compatible"
        blocks = [
            _event("Process.Startup", session, {"Product.Version": "0.1.0-test"}),
            _event("SupportReport", session, support),
            _event("Configuration.Applied", session, {"Background.Mode": mode}),
            _event("Performance.Interval", session, self.intervals[name]),
            _event(
                "Performance.Interval",
                session,
                {"Window.Final": "true", "Window.DurationUs": 500_000},
            ),
            _event("Process.Exited", session, {}),
        ]
        path = self.root / name / REPORTER.LOG_NAME
        path.write_text("\n---\n".join(blocks) + "\n---\n", encoding="utf-8")

    def rewrite(self, name: str) -> None:
        self._write_log(name)


class PerformanceBaselineReporterTests(unittest.TestCase):
    def test_valid_pair_produces_machine_and_human_reports(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary))
            report = REPORTER.build_report(fixture.root)

            self.assertEqual("passed", report["status"])
            self.assertEqual(1, report["modes"]["fx-only"]["metrics"]["GPU.PendingFrames.Max"])
            markdown = REPORTER.render_markdown(report)
            self.assertIn("GPU Bloom/final p95", markdown)
            self.assertIn("harmless thread messages", markdown)

    def test_real_raw_input_rejects_nonidentical_workload(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary))
            fixture.intervals["background-aware"]["Input.RawMessages"] = 1
            fixture.rewrite("background-aware")

            with self.assertRaisesRegex(
                REPORTER.ValidationError, "real Raw Input changed"
            ):
                REPORTER.build_report(fixture.root)

    def test_gpu_queue_regression_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary))
            fixture.intervals["fx-only"]["GPU.PendingFrames.Max"] = 2
            fixture.rewrite("fx-only")

            with self.assertRaisesRegex(
                REPORTER.ValidationError, "queued beyond one frame"
            ):
                REPORTER.build_report(fixture.root)

    def test_background_aware_must_reach_the_final_composite(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary))
            fixture.intervals["background-aware"]["Background.CompositeStatus"] = "awaiting-fresh-sample"
            fixture.rewrite("background-aware")

            with self.assertRaisesRegex(
                REPORTER.ValidationError, "did not participate"
            ):
                REPORTER.build_report(fixture.root)


if __name__ == "__main__":
    unittest.main()
