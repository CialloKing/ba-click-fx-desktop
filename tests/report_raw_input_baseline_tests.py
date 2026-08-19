#!/usr/bin/env python3
"""Contract tests for the controlled Raw Input baseline reporter."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest


SCRIPT_PATH = (
    Path(__file__).resolve().parents[1]
    / "tools"
    / "report-raw-input-baseline.py"
)
SPEC = importlib.util.spec_from_file_location(
    "report_raw_input_baseline", SCRIPT_PATH
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


def _metric(prefix: str, unit: str, samples: int = 20) -> dict[str, object]:
    return {
        f"{prefix}.Available": "true",
        f"{prefix}.Unit": unit,
        f"{prefix}.Samples": samples,
        f"{prefix}.RecordedSamples": samples,
        f"{prefix}.DroppedSamples": 0,
        f"{prefix}.P50": 1,
        f"{prefix}.P95": 2,
        f"{prefix}.P99": 3,
        f"{prefix}.Max": 4,
    }


def _interval(background_mode: str, raw_messages: int) -> dict[str, object]:
    background_aware = background_mode == "background-aware"
    interval: dict[str, object] = {
        "Background.Mode": background_mode,
        "Output.Width": 2560,
        "Output.Height": 1440,
        "Window.Final": "false",
        "Window.DurationUs": 10_500_000,
        "Window.PresentedFps": 120.0,
        "Background.CompositeStatus": (
            "participating" if background_aware else "inactive"
        ),
        "WGC.SamplesAccepted": 20 if background_aware else 0,
        "Background.ParticipatingFrames": 20 if background_aware else 0,
        "Input.RawMessages": raw_messages,
        "Input.ButtonEdges": raw_messages,
        "Input.MoveEvents": 0,
        "Input.CancelEvents": 0,
        "Input.CompactedMoveEvents": 0,
        "Input.OverflowMoveDrops": 0,
        "Input.MessageTimeUnavailable": 0,
        "MessagePump.InputDispatched": raw_messages,
        "MessagePump.InputBudgetExhaustions": 0,
    }
    interval.update(_metric("Input.Win32QueueAge", "ms"))
    interval.update(_metric("Input.PendingEvents", "events"))
    interval.update(_metric("Cpu.PresentCall", "us"))
    if raw_messages > 0:
        interval.update(
            _metric(
                "Input.DispatchToPresentReturn",
                "us",
                samples=2,
            )
        )
        interval.update(
            _metric(
                "Input.MessageToPresentReturn",
                "ms",
                samples=2,
            )
        )
    return interval


def _receiver(click_count: int) -> dict[str, object]:
    return {
        "captureStatus": "captured",
        "exitCode": 0,
        "hostExitedNormally": True,
        "commandLine": [
            REPORTER.HOST_NAME,
            "--quit-after-ms=10500",
        ],
        "targetRectangle": {
            "left": 100,
            "top": 100,
            "right": 300,
            "bottom": 300,
        },
        "targetPoint": {"x": 200, "y": 200},
        "primaryWorkArea": {
            "left": 0,
            "top": 0,
            "right": 2560,
            "bottom": 1440,
        },
        "cursorClipRectangle": {
            "left": 0,
            "top": 0,
            "right": 2560,
            "bottom": 1440,
        },
        "originalCursor": {"x": 400, "y": 400},
        "restoredCursor": {"x": 400, "y": 400},
        "plannedSendInputCount": click_count * 2,
        "attemptedSendInputCount": click_count * 2,
        "acceptedSendInputCount": click_count * 2,
        "taggedDownCount": click_count,
        "taggedUpCount": click_count,
        "unexpectedButtonMessages": 0,
        "unexpectedMoveMessages": 0,
        "captureLossCount": 0,
        "emergencyUpCount": 0,
        "captureReleased": True,
        "receiverStopped": True,
        "cursorRestored": True,
        "cleanupSuccess": True,
    }


class RawInputBaselineFixture:
    def __init__(self, root: Path, raw_messages: int = 4):
        self.root = root
        self.raw_messages = raw_messages
        self.intervals = {
            name: _interval(background_mode, raw_messages)
            for name, background_mode in (
                ("fx-only", "recording-compatible"),
                ("background-aware", "background-aware"),
            )
        }
        self.host_bytes = b"raw-input-test-host"
        self._write_files()

    def _write_files(self) -> None:
        click_count = 2
        manifest = {
            "schemaVersion": 1,
            "scenarioId": "p0-raw-input-down-v1",
            "captureStatus": "captured",
            "revision": "0123456789abcdef0123456789abcdef01234567",
            "workingTreeDirty": False,
            "executableSha256": hashlib.sha256(self.host_bytes).hexdigest(),
            "capturedAtUtc": "2026-08-19T00:00:00.000Z",
            "durationMs": 10500,
            "clickCount": click_count,
            "clickHoldMs": 30,
            "clickIntervalMs": 300,
            "inputConfirmationTimeoutMs": 250,
            "receiverReadyTimeoutMs": 2000,
            "receiverStopTimeoutMs": 2000,
            "cursorRestoreTimeoutMs": 500,
            "rawInputRegistration": "enabled-inputsink-devnotify",
            "modes": {},
        }
        for name, background_mode in (
            ("fx-only", "recording-compatible"),
            ("background-aware", "background-aware"),
        ):
            directory = self.root / name
            directory.mkdir()
            (directory / REPORTER.HOST_NAME).write_bytes(self.host_bytes)
            (directory / REPORTER.CONFIG_NAME).write_text(
                json.dumps(
                    {
                        "schemaVersion": 16,
                        "background": {"mode": background_mode},
                        "effects": {"themeColor": "#4ca7ff"},
                    }
                ),
                encoding="utf-8",
            )
            manifest["modes"][name] = _receiver(click_count)
            self._write_log(name, background_mode)
        (self.root / "capture.json").write_text(
            json.dumps(manifest), encoding="utf-8"
        )

    def _write_log(self, name: str, background_mode: str) -> None:
        session = f"session-{name}"
        support = {
            "Product.Version": "0.1.0-test",
            "Support.WGC": (
                "active" if name == "background-aware" else "fallback-fx-only"
            ),
            "Graphics.DriverType": "Hardware",
            "Graphics.Adapter": "Test GPU",
            "Graphics.AdapterLuid": "00000000:00000001",
            "Graphics.DriverVersion": "1.2.3.4",
            "Graphics.HardwareFallback": "none",
            "Display.Primary": "2560x1440@0,0",
            "Display.PrimaryDpi": 120,
            "Display.RefreshRateHz": 120.0,
        }
        blocks = [
            _event("Process.Startup", session, {}),
            _event("SupportReport", session, support),
            _event(
                "Configuration.Applied",
                session,
                {"Background.Mode": background_mode},
            ),
            _event("Performance.Interval", session, self.intervals[name]),
            _event("Process.Exited", session, {}),
        ]
        (self.root / name / REPORTER.LOG_NAME).write_text(
            "\n---\n".join(blocks) + "\n---\n", encoding="utf-8"
        )


class RawInputBaselineReporterTests(unittest.TestCase):
    def test_supported_pair_produces_latency_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = RawInputBaselineFixture(Path(temporary))

            report = REPORTER.build_report(fixture.root)

            self.assertEqual("passed", report["status"])
            self.assertEqual(4, report["modes"]["fx-only"]["rawMessages"])
            self.assertEqual(
                1,
                report["modes"]["background-aware"]["latencyMetrics"][
                    "Input.DispatchToPresentReturn.P50"
                ],
            )
            self.assertIn("# P0 Raw Input baseline", REPORTER.render_markdown(report))

    def test_zero_raw_input_pair_is_explicitly_unsupported(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = RawInputBaselineFixture(Path(temporary), raw_messages=0)

            report = REPORTER.build_report(fixture.root)

            self.assertEqual("unsupported", report["status"])
            self.assertIn("did not observe injected edges", report["reason"])
            self.assertEqual(
                "unsupported", report["modes"]["fx-only"]["status"]
            )

    def test_mode_capability_mismatch_is_failed(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = RawInputBaselineFixture(Path(temporary))
            fixture.intervals["background-aware"]["Input.RawMessages"] = 0
            fixture.intervals["background-aware"]["Input.ButtonEdges"] = 0
            fixture.intervals["background-aware"]["MessagePump.InputDispatched"] = 0
            fixture._write_log("background-aware", "background-aware")

            report = REPORTER.build_report(fixture.root)

            self.assertEqual("failed", report["status"])
            self.assertIn("disagreed", report["reason"])

    def test_executable_hash_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = RawInputBaselineFixture(Path(temporary))
            executable = Path(temporary) / "fx-only" / REPORTER.HOST_NAME
            executable.write_bytes(b"different-host")

            with self.assertRaisesRegex(
                REPORTER.ValidationError, "executable hash does not match"
            ):
                REPORTER.build_report(fixture.root)

    def test_cursor_restore_failure_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = RawInputBaselineFixture(Path(temporary))
            manifest_path = Path(temporary) / "capture.json"
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["modes"]["fx-only"]["restoredCursor"] = {"x": 401, "y": 400}
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            with self.assertRaisesRegex(
                REPORTER.ValidationError, "cursor was not restored exactly"
            ):
                REPORTER.build_report(fixture.root)


if __name__ == "__main__":
    unittest.main()
