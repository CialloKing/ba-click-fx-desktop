#!/usr/bin/env python3
"""Contract tests for the non-release Active-FX ROI diagnostic reporter."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from datetime import datetime, timedelta, timezone
from pathlib import Path
import re
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
REPORTER_PATH = ROOT / "tools" / "report-active-fx-roi-diagnostic.py"
REPORTER_SPEC = importlib.util.spec_from_file_location(
    "report_active_fx_roi_diagnostic", REPORTER_PATH
)
if REPORTER_SPEC is None or REPORTER_SPEC.loader is None:
    raise RuntimeError(f"cannot load {REPORTER_PATH}")
REPORTER = importlib.util.module_from_spec(REPORTER_SPEC)
REPORTER_SPEC.loader.exec_module(REPORTER)

# Share the release reporter's established raw-log fixture. The diagnostic
# fixture below changes only the envelope, ordering and NVIDIA evidence.
FIXTURE_PATH = ROOT / "tests" / "report_active_fx_roi_ab_tests.py"
FIXTURE_SPEC = importlib.util.spec_from_file_location(
    "report_active_fx_roi_ab_fixture", FIXTURE_PATH
)
if FIXTURE_SPEC is None or FIXTURE_SPEC.loader is None:
    raise RuntimeError(f"cannot load {FIXTURE_PATH}")
FIXTURE_MODULE = importlib.util.module_from_spec(FIXTURE_SPEC)
FIXTURE_SPEC.loader.exec_module(FIXTURE_MODULE)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _utc_text(value: datetime) -> str:
    return value.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def _local_nvidia_text(value: datetime) -> str:
    local = value.astimezone()
    return local.strftime("%Y/%m/%d %H:%M:%S.%f")[:-3]


def _telemetry_csv(
    started_at: datetime, stopped_at: datetime, gpu: dict[str, object]
) -> tuple[str, int]:
    header = ",".join(REPORTER.TELEMETRY_FIELDS)
    base = [
        str(gpu["index"]),
        str(gpu["uuid"]),
        str(gpu["name"]),
    ]
    elapsed_ms = round((stopped_at - started_at).total_seconds() * 1000)
    sample_count = elapsed_ms // REPORTER.TELEMETRY_INTERVAL_MS + 1
    rows = [header]
    for sample_index in range(sample_count):
        timestamp = started_at + timedelta(
            milliseconds=sample_index * REPORTER.TELEMETRY_INTERVAL_MS
        )
        high = sample_index != 0
        row = [
            _local_nvidia_text(timestamp),
            *base,
            "P0" if high else "P2",
            "1201" if high else "1001",
            "2201" if high else "2001",
            "46" if high else "41",
            "53" if high else "51",
            "31" if high else "11",
            "41" if high else "21",
        ]
        rows.append(",".join(row))
    return "\n".join(rows) + "\n", sample_count


def _diagnostic_fixture(root: Path, telemetry_enabled: bool = True) -> object:
    fixture = FIXTURE_MODULE.CaptureFixture(root)
    fixture.manifest["schemaVersion"] = 1
    fixture.manifest["kind"] = "bafx-active-fx-roi-diagnostic-capture"
    fixture.manifest["captureStatus"] = "diagnostic-captured"
    fixture.manifest["releaseEligible"] = False
    fixture.manifest["diagnosticNotice"] = REPORTER.DIAGNOSTIC_NOTICE
    fixture.manifest["schedule"] = {
        "pattern": "ABBA+BAAB",
        "blockPatterns": ["ABBA", "BAAB"],
        "a": "roi-off",
        "b": "roi-on",
        "blocks": 2,
        "runs": 8,
        "warmupMs": 5000,
        "sampleMs": 30000,
        "hostDurationMs": 40500,
        "performanceIntervalMs": 10000,
        "discardCompleteIntervals": 1,
        "selectCompleteIntervals": 3,
    }
    gpu = {
        "index": 0,
        "uuid": "GPU-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
        "name": "NVIDIA GeForce RTX 4060 Laptop GPU",
    }
    fixture.manifest["nvidiaTelemetry"] = (
        {
            "enabled": True,
            "provider": "nvidia-smi",
            "intervalMs": 200,
            "fields": list(REPORTER.TELEMETRY_FIELDS),
            "executable": {
                "fileName": "nvidia-smi.exe",
                "sha256": "b" * 64,
                "companyName": "NVIDIA Corporation",
                "fileDescription": "NVIDIA-SMI Command Line Utility",
                "productVersion": "32.0.16.1088",
            },
            "gpu": gpu,
        }
        if telemetry_enabled
        else {"enabled": False}
    )

    patterns = (
        ("ABBA", (("A", False), ("B", True), ("B", True), ("A", False))),
        ("BAAB", (("B", True), ("A", False), ("A", False), ("B", True))),
    )
    runs = fixture.manifest["runs"][:8]
    fixture.manifest["runs"] = runs
    for ordinal, run in enumerate(runs, 1):
        block = (ordinal - 1) // 4
        position = (ordinal - 1) % 4
        block_pattern, pattern = patterns[block]
        arm, roi_enabled = pattern[position]
        old_directory = root / run["directory"]
        directory_name = (
            f"run-{ordinal:02d}-{arm.lower()}-roi-"
            f"{'on' if roi_enabled else 'off'}"
        )
        new_directory = root / directory_name
        if old_directory != new_directory:
            old_directory.rename(new_directory)
        run["block"] = block + 1
        run["position"] = position + 1
        run["blockPattern"] = block_pattern
        run["arm"] = arm
        run["roiEnabled"] = roi_enabled
        run["directory"] = directory_name
        run["executable"] = f"{directory_name}/ba-click-fx-desktop.exe"
        run["config"] = f"{directory_name}/BAFX.config.json"
        run["log"] = f"{directory_name}/ba-click-fx-desktop-support.log"
        run_started_at = datetime(
            2026, 8, 25, 12, ordinal, 0, tzinfo=timezone.utc
        )
        run_elapsed_ms = 40_600
        run["startedAtUtc"] = _utc_text(run_started_at)
        run["elapsedMs"] = run_elapsed_ms

        config_path = root / run["config"]
        config = json.loads(config_path.read_text(encoding="utf-8"))
        config["performance"]["activeFxRoiEnabled"] = roi_enabled
        config_path.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
        run["configSha256"] = _sha256(config_path)
        fixture.overrides[ordinal] = {
            "GPU.Primary.FinalComposite.P95": 2000 + ordinal,
            "GPU.Primary.FinalComposite.P99": 3000 + ordinal,
            "GPU.RenderCommandSpan.P99": 4000 + ordinal,
            "Cpu.FrameTotal.P95": 5000 + ordinal,
            "Cpu.FrameTotal.P99": 6000 + ordinal,
            "Cpu.PresentCall.P95": 7000 + ordinal,
            "Cpu.PresentCall.P99": 8000 + ordinal,
        }
        fixture.write_log(ordinal, roi_enabled)

        if telemetry_enabled:
            csv_path = new_directory / "nvidia-smi.csv"
            stderr_path = new_directory / "nvidia-smi.stderr.txt"
            telemetry_started_at = run_started_at + timedelta(milliseconds=100)
            telemetry_stopped_at = run_started_at + timedelta(milliseconds=40_500)
            telemetry_csv, sample_count = _telemetry_csv(
                telemetry_started_at, telemetry_stopped_at, gpu
            )
            csv_path.write_text(telemetry_csv, encoding="utf-8")
            stderr_path.write_text("", encoding="utf-8")
            run["nvidiaTelemetry"] = {
                "file": f"{directory_name}/nvidia-smi.csv",
                "stderr": f"{directory_name}/nvidia-smi.stderr.txt",
                "sha256": _sha256(csv_path),
                "samples": sample_count,
                "intervalMs": 200,
                "arguments": [
                    f"--id={gpu['uuid']}",
                    f"--query-gpu={','.join(REPORTER.TELEMETRY_FIELDS)}",
                    "--format=csv,noheader,nounits",
                    "--loop-ms=200",
                ],
                "startedAtUtc": _utc_text(telemetry_started_at),
                "stoppedAtUtc": _utc_text(telemetry_stopped_at),
                "collectorStoppedProcess": True,
            }
    fixture.write_manifest()
    return fixture


class ActiveFxRoiDiagnosticReporterTests(unittest.TestCase):
    def test_reports_ordered_runs_arm_aggregates_and_telemetry_ranges(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = _diagnostic_fixture(Path(temporary))
            report = REPORTER.build_report(fixture.root)

            self.assertFalse(report["releaseEligible"])
            self.assertEqual(report["schedule"]["pattern"], "ABBA+BAAB")
            self.assertEqual(
                [run["arm"] for run in report["runs"]],
                ["A", "B", "B", "A", "B", "A", "A", "B"],
            )
            self.assertEqual(report["runs"][4]["blockPattern"], "BAAB")
            self.assertEqual(
                report["runs"][4]["metrics"]["finalCompositeP99Us"], 3005
            )
            telemetry = report["runs"][0]["nvidiaTelemetry"]
            self.assertEqual(telemetry["pstate"]["values"], ["P0", "P2"])
            self.assertEqual(
                telemetry["smClockMHz"], {"min": 1001.0, "max": 1201.0}
            )
            self.assertNotIn("graphicsClockMHz", telemetry)
            self.assertEqual(
                telemetry["instantPowerWatts"], {"min": 41.0, "max": 46.0}
            )
            self.assertNotIn("powerWatts", telemetry)
            self.assertEqual(telemetry["timestamp"]["minimumSamples"], 101)
            self.assertEqual(telemetry["timestamp"]["spanMs"], 40_400)
            self.assertEqual(telemetry["timestamp"]["maximumGapMs"], 200)
            self.assertIn("finalCompositeP99Us", report["roiOff"])
            self.assertIn("bloomFinalP95Us", report["roiOn"])

            markdown = REPORTER.render_markdown(report)
            self.assertIn("NON-RELEASE", markdown)
            self.assertIn("Primary FinalComposite p99", markdown)
            self.assertIn("ABBA/A", markdown)
            self.assertIn("BAAB/B", markdown)
            self.assertIn("P0-P2", markdown)
            self.assertIn("SM clock", markdown)
            self.assertIn("Instant power", markdown)

    def test_accepts_an_explicitly_disabled_telemetry_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = _diagnostic_fixture(Path(temporary), telemetry_enabled=False)
            report = REPORTER.build_report(fixture.root)

            self.assertFalse(report["nvidiaTelemetry"]["enabled"])
            self.assertTrue(
                all(run["nvidiaTelemetry"] is None for run in report["runs"])
            )
            self.assertIn("NVIDIA telemetry was not captured", REPORTER.render_markdown(report))

    def test_rejects_legacy_clock_and_power_query_fields(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = _diagnostic_fixture(Path(temporary))
            fields = fixture.manifest["nvidiaTelemetry"]["fields"]
            fields[5] = "clocks.current.graphics"
            fields[7] = "power.draw"
            fixture.write_manifest()

            with self.assertRaisesRegex(REPORTER.ValidationError, "fields differ"):
                REPORTER.build_report(fixture.root)

    def test_release_reporter_rejects_the_diagnostic_envelope(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = _diagnostic_fixture(Path(temporary))
            with self.assertRaises(REPORTER.ValidationError):
                REPORTER.RELEASE.build_report(fixture.root)

    def test_requires_the_baab_order_and_tail_metric(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = _diagnostic_fixture(Path(temporary))
            fixture.manifest["runs"][4]["blockPattern"] = "ABBA"
            fixture.write_manifest()
            with self.assertRaisesRegex(REPORTER.ValidationError, "block pattern"):
                REPORTER.build_report(fixture.root)

        with tempfile.TemporaryDirectory() as temporary:
            fixture = _diagnostic_fixture(Path(temporary))
            log_path = fixture.root / fixture.manifest["runs"][0]["log"]
            text = log_path.read_text(encoding="utf-8")
            text = re.sub(
                r"^GPU\.Primary\.FinalComposite\.P99=.*\n", "", text, flags=re.MULTILINE
            )
            log_path.write_text(text, encoding="utf-8")
            with self.assertRaisesRegex(
                REPORTER.ValidationError, "FinalComposite.P99"
            ):
                REPORTER.build_report(fixture.root)

    def test_rejects_unproven_or_malformed_telemetry(self) -> None:
        cases = (
            (
                "hash mismatch",
                lambda fixture: fixture.manifest["runs"][0]["nvidiaTelemetry"].__setitem__(
                    "sha256", "0" * 64
                ),
                "SHA-256 mismatch",
            ),
            (
                "sample mismatch",
                lambda fixture: fixture.manifest["runs"][0]["nvidiaTelemetry"].__setitem__(
                    "samples",
                    fixture.manifest["runs"][0]["nvidiaTelemetry"]["samples"] + 1,
                ),
                "manifest records",
            ),
            (
                "identity drift",
                lambda fixture: _replace_telemetry_value(
                    fixture, "NVIDIA GeForce RTX 4060 Laptop GPU", "NVIDIA RTX 4090"
                ),
                "GPU identity changed",
            ),
        )
        for label, mutate, message in cases:
            with self.subTest(case=label), tempfile.TemporaryDirectory() as temporary:
                fixture = _diagnostic_fixture(Path(temporary))
                mutate(fixture)
                fixture.write_manifest()
                with self.assertRaisesRegex(REPORTER.ValidationError, message):
                    REPORTER.build_report(fixture.root)

    def test_rejects_short_missing_or_stalled_telemetry_timelines(self) -> None:
        cases = (
            ("two samples", _keep_telemetry_endpoints, "below the minimum"),
            ("missing middle", _remove_telemetry_middle, "CSV gap"),
            ("stalled clock", _stall_telemetry_clock, "last CSV sample"),
        )
        for label, mutate, message in cases:
            with self.subTest(case=label), tempfile.TemporaryDirectory() as temporary:
                fixture = _diagnostic_fixture(Path(temporary))
                mutate(fixture)
                with self.assertRaisesRegex(REPORTER.ValidationError, message):
                    REPORTER.build_report(fixture.root)

    def test_rejects_sampler_session_order_or_run_boundary_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = _diagnostic_fixture(Path(temporary))
            telemetry = fixture.manifest["runs"][0]["nvidiaTelemetry"]
            telemetry["startedAtUtc"], telemetry["stoppedAtUtc"] = (
                telemetry["stoppedAtUtc"],
                telemetry["startedAtUtc"],
            )
            fixture.write_manifest()
            with self.assertRaisesRegex(REPORTER.ValidationError, "precedes"):
                REPORTER.build_report(fixture.root)

        with tempfile.TemporaryDirectory() as temporary:
            fixture = _diagnostic_fixture(Path(temporary))
            fixture.manifest["runs"][0]["nvidiaTelemetry"][
                "startedAtUtc"
            ] = "2026-08-25T12:01:05.000Z"
            fixture.write_manifest()
            with self.assertRaisesRegex(REPORTER.ValidationError, "run start"):
                REPORTER.build_report(fixture.root)


def _replace_telemetry_value(fixture: object, old: str, new: str) -> None:
    run = fixture.manifest["runs"][0]
    path = fixture.root / run["nvidiaTelemetry"]["file"]
    path.write_text(path.read_text(encoding="utf-8").replace(old, new), encoding="utf-8")
    run["nvidiaTelemetry"]["sha256"] = _sha256(path)


def _rewrite_telemetry_rows(fixture: object, rows: list[str]) -> None:
    run = fixture.manifest["runs"][0]
    path = fixture.root / run["nvidiaTelemetry"]["file"]
    path.write_text("\n".join(rows) + "\n", encoding="utf-8")
    run["nvidiaTelemetry"]["samples"] = len(rows) - 1
    run["nvidiaTelemetry"]["sha256"] = _sha256(path)
    fixture.write_manifest()


def _telemetry_rows(fixture: object) -> list[str]:
    run = fixture.manifest["runs"][0]
    path = fixture.root / run["nvidiaTelemetry"]["file"]
    return path.read_text(encoding="utf-8").splitlines()


def _keep_telemetry_endpoints(fixture: object) -> None:
    rows = _telemetry_rows(fixture)
    _rewrite_telemetry_rows(fixture, [rows[0], rows[1], rows[-1]])


def _remove_telemetry_middle(fixture: object) -> None:
    rows = _telemetry_rows(fixture)
    _rewrite_telemetry_rows(fixture, rows[:80] + rows[96:])


def _stall_telemetry_clock(fixture: object) -> None:
    rows = _telemetry_rows(fixture)
    stalled_timestamp = rows[50].split(",", 1)[0]
    for index in range(51, len(rows)):
        _, remainder = rows[index].split(",", 1)
        rows[index] = f"{stalled_timestamp},{remainder}"
    _rewrite_telemetry_rows(fixture, rows)


if __name__ == "__main__":
    unittest.main()
