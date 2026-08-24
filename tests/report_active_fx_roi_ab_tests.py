#!/usr/bin/env python3
"""Contract tests for the Active-FX ROI ABBA reporter."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import re
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
REPORTER_PATH = ROOT / "tools" / "report-active-fx-roi-ab.py"
SPEC = importlib.util.spec_from_file_location("report_active_fx_roi_ab", REPORTER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load {REPORTER_PATH}")
REPORTER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORTER)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _event(name: str, session: str, fields: dict[str, object]) -> str:
    values = {
        "Log.SessionId": session,
        "Event.Name": name,
        **fields,
    }
    return "\n".join(f"{key}={str(value).lower() if type(value) is bool else value}" for key, value in values.items())


class CaptureFixture:
    def __init__(
        self,
        root: Path,
        expectation: str = "applied",
        expected_reason: str | None = None,
        scenario_id: str = "center-click",
        measurement_path: str = "primary",
    ) -> None:
        self.root = root
        self.expectation = expectation
        self.expected_reason = expected_reason or (
            "applied" if expectation == "applied" else "background-differential-bloom"
        )
        self.scenario_id = scenario_id
        self.measurement_path = measurement_path
        self.executable_bytes = b"same-host-binary-v0.2.7"
        self.overrides: dict[int, dict[str, object]] = {}
        self.interval_overrides: dict[
            tuple[int, int], dict[str, object]
        ] = {}
        self.power_unavailable_runs: set[int] = set()
        self.environment_overrides: dict[int, dict[str, object]] = {}
        self.environment_identity = {
            "productVersion": "0.2.7",
            "driverType": "Hardware",
            "adapter": "NVIDIA GeForce RTX 4060 Laptop GPU",
            "adapterLuid": "00000000:0037D0F8",
            "driverVersion": "32.0.16.1088",
            "hardwareFallback": "none",
            "primaryDisplay": "3840x2160@0,0",
            "primaryDpi": 144,
            "refreshRateNumerator": 170,
            "refreshRateDenominator": 1,
            "outputWidth": 3840,
            "outputHeight": 2160,
            "hdrEnabled": False,
            "outputMapping": "conservative-sdr",
        }
        self.base_config = {
            "schemaVersion": 19,
            "background": {"mode": "recording-compatible"},
            "effects": {"enabled": True, "bloomLayerEnabled": True},
            "performance": {
                "activeFxRoiEnabled": False,
                "effectsMode": "full",
                "framePacing": "match-display",
                "idleOptimization": True,
            },
        }
        self.base_path = root / "base-config.json"
        self.base_path.write_text(
            json.dumps(self.base_config, indent=2) + "\n", encoding="utf-8"
        )
        self.manifest = {
            "schemaVersion": 3,
            "kind": "bafx-active-fx-roi-ab-capture",
            "captureStatus": "captured",
            "revision": "a" * 40,
            "workingTreeDirty": False,
            "capturedAtUtc": "2026-08-24T12:00:00.000Z",
            "executable": {
                "fileName": "ba-click-fx-desktop.exe",
                "sha256": hashlib.sha256(self.executable_bytes).hexdigest(),
            },
            "environment": {
                "contract": "rtx-4060-4k170-sdr-v1",
                "identity": dict(self.environment_identity),
            },
            "configuration": {
                "schemaVersion": 19,
                "baseConfig": "base-config.json",
                "baseSha256": _sha256(self.base_path),
                "differenceContract": "performance.activeFxRoiEnabled-only",
            },
            "scenario": {
                "id": scenario_id,
                "workload": f"fixed-age-{scenario_id}",
                "measurementPath": measurement_path,
                "expectation": expectation,
                "expectedDecisionReason": self.expected_reason,
            },
            "capabilities": {
                "center-click": {
                    "supported": True,
                    "driver": "host-demo-click-fixed-age-v1",
                    "failureCode": None,
                },
                "interior-trail": {
                    "supported": True,
                    "driver": "host-demo-interior-trail-fixed-age-v1",
                    "failureCode": None,
                },
                "boundary-top-left": {
                    "supported": True,
                    "driver": "host-demo-boundary-top-left-fixed-age-v1",
                    "failureCode": None,
                },
            },
            "schedule": {
                "pattern": "ABBA",
                "a": "roi-off",
                "b": "roi-on",
                "blocks": 5,
                "runs": 20,
                "warmupMs": 5000,
                "sampleMs": 30000,
                "hostDurationMs": 40500,
                "performanceIntervalMs": 10000,
                "discardCompleteIntervals": 1,
                "selectCompleteIntervals": 3,
            },
            "runs": [],
        }
        self.write()

    def interval(
        self, ordinal: int, roi_enabled: bool, complete_interval: int
    ) -> dict[str, object]:
        frame_count = 1200
        observed = frame_count
        if roi_enabled and self.expectation == "applied":
            requested = 1000
            applied = 960
            prefilter_full_pixels = 100_000
            prefilter_candidate_pixels = 40_000
            prefilter_drawn_pixels = 40_000
            pyramid_full_pixels = 100_000
            pyramid_candidate_pixels = 40_000
            pyramid_drawn_pixels = 40_000
            resolve_full_pixels = 100_000
            resolve_candidate_pixels = 40_000
            resolve_drawn_pixels = 100_000
            reason_frames = observed
            prefilter = 250
            pyramid = 210
            bloom_final = 820
        elif roi_enabled:
            requested = 1000
            applied = 0
            prefilter_full_pixels = 100_000
            prefilter_candidate_pixels = 100_000
            prefilter_drawn_pixels = 100_000
            pyramid_full_pixels = 100_000
            pyramid_candidate_pixels = 100_000
            pyramid_drawn_pixels = 100_000
            resolve_full_pixels = 100_000
            resolve_candidate_pixels = 100_000
            resolve_drawn_pixels = 100_000
            reason_frames = observed
            prefilter = 400
            pyramid = 300
            bloom_final = 1000
        else:
            requested = 0
            applied = 0
            prefilter_full_pixels = 100_000
            prefilter_candidate_pixels = 100_000
            prefilter_drawn_pixels = 100_000
            pyramid_full_pixels = 100_000
            pyramid_candidate_pixels = 100_000
            pyramid_drawn_pixels = 100_000
            resolve_full_pixels = 100_000
            resolve_candidate_pixels = 100_000
            resolve_drawn_pixels = 100_000
            reason_frames = 0
            prefilter = 400
            pyramid = 300
            bloom_final = 1000
        full_pixels = (
            prefilter_full_pixels + pyramid_full_pixels + resolve_full_pixels
        )
        candidate_pixels = (
            prefilter_candidate_pixels
            + pyramid_candidate_pixels
            + resolve_candidate_pixels
        )
        drawn_pixels = (
            prefilter_drawn_pixels + pyramid_drawn_pixels + resolve_drawn_pixels
        )
        cleared_pixels = prefilter_drawn_pixels + pyramid_drawn_pixels
        roi_prefix = (
            "ROI.Primary"
            if self.measurement_path == "primary"
            else "ROI.RecordingRebuild"
        )
        gpu_prefix = (
            "GPU.Primary"
            if self.measurement_path == "primary"
            else "GPU.RecordingRebuild"
        )
        reason_field = (
            "ROI.Active.Reason.boundary-fallback.Frames"
            if self.expected_reason in {"boundary-fallback", "touches-boundary"}
            else f"{roi_prefix}.Reason.{self.expected_reason}.Frames"
        )
        fields: dict[str, object] = {
            "Window.Final": False,
            "Window.DurationUs": 10_000_000,
            "Window.FrameCount": frame_count,
            "Configuration.SchemaVersion": 19,
            "Performance.ActiveFxRoiEnabled": roi_enabled,
            "ROI.ProductionPath": (
                "active-fx-pyramid-with-full-screen-fallback"
                if roi_enabled
                else "disabled-full-screen"
            ),
            f"{roi_prefix}.ObservedFrames": observed,
            f"{roi_prefix}.RequestedFrames": requested,
            f"{roi_prefix}.EligibleFrames": applied,
            f"{roi_prefix}.AppliedFrames": applied,
            f"{roi_prefix}.WarmupFrames": 0,
            f"{roi_prefix}.FullPixels.Total": full_pixels,
            f"{roi_prefix}.CandidatePixels.Total": candidate_pixels,
            f"{roi_prefix}.DrawnPixels.Total": drawn_pixels,
            f"{roi_prefix}.ClearedPixels.Total": cleared_pixels,
            f"{roi_prefix}.Stage.Prefilter.FullPixels.Total": prefilter_full_pixels,
            f"{roi_prefix}.Stage.Prefilter.CandidatePixels.Total": prefilter_candidate_pixels,
            f"{roi_prefix}.Stage.Prefilter.DrawnPixels.Total": prefilter_drawn_pixels,
            f"{roi_prefix}.Stage.Prefilter.ClearedPixels.Total": prefilter_drawn_pixels,
            f"{roi_prefix}.Stage.Downsample.FullPixels.Total": pyramid_full_pixels // 2,
            f"{roi_prefix}.Stage.Downsample.CandidatePixels.Total": pyramid_candidate_pixels // 2,
            f"{roi_prefix}.Stage.Downsample.DrawnPixels.Total": pyramid_drawn_pixels // 2,
            f"{roi_prefix}.Stage.Downsample.ClearedPixels.Total": pyramid_drawn_pixels // 2,
            f"{roi_prefix}.Stage.Upsample.FullPixels.Total": pyramid_full_pixels // 2,
            f"{roi_prefix}.Stage.Upsample.CandidatePixels.Total": pyramid_candidate_pixels // 2,
            f"{roi_prefix}.Stage.Upsample.DrawnPixels.Total": pyramid_drawn_pixels // 2,
            f"{roi_prefix}.Stage.Upsample.ClearedPixels.Total": pyramid_drawn_pixels // 2,
            f"{roi_prefix}.Stage.Resolve.FullPixels.Total": resolve_full_pixels,
            f"{roi_prefix}.Stage.Resolve.CandidatePixels.Total": resolve_candidate_pixels,
            f"{roi_prefix}.Stage.Resolve.DrawnPixels.Total": resolve_drawn_pixels,
            f"{roi_prefix}.Stage.Resolve.ClearedPixels.Total": 0,
            reason_field: reason_frames,
            f"{gpu_prefix}.Prefilter.P95": prefilter,
            f"{gpu_prefix}.Pyramid.P95": pyramid,
            f"{gpu_prefix}.FinalComposite.P95": 200,
            "GPU.BloomAndFinalComposite.P95": bloom_final,
            "Cpu.FrameTotal.P95": 900,
            "Cpu.FrameTotal.P99": 1000,
            "Cpu.PresentCall.P95": 180,
            "Cpu.PresentCall.P99": 200,
            "Cpu.PresentCall.Samples": frame_count,
            "GPU.RenderCommandSpan.P99": 1300,
            "GPU.PendingFrames.Max": 1,
            "GPU.DisjointSamples": 0,
            "GPU.QueryFailures": 0,
            "GPU.StateErrors": 0,
            "GPU.RingFullSkipped": 0,
            "GPU.AutoSkippedStageFrames": 0,
            "FramePacing.Timeouts": 0,
            "FramePacing.Failures": 0,
        }
        if self.expected_reason in {"boundary-fallback", "touches-boundary"}:
            fields["ROI.RequestedFrames"] = observed
        fields.update(self.overrides.get(ordinal, {}))
        fields.update(self.interval_overrides.get((ordinal, complete_interval), {}))
        return fields

    def write_log(
        self,
        ordinal: int,
        roi_enabled: bool,
        interval_count: int = 4,
        support_count: int = 1,
        configuration_count: int = 1,
    ) -> None:
        run = self.manifest["runs"][ordinal - 1]
        path = self.root / run["log"]
        session = f"session-{ordinal}"
        identity = {
            **self.environment_identity,
            **self.environment_overrides.get(ordinal, {}),
        }
        support = {
            "Product.Version": identity["productVersion"],
            "Graphics.DriverType": identity["driverType"],
            "Graphics.Adapter": identity["adapter"],
            "Graphics.AdapterLuid": identity["adapterLuid"],
            "Graphics.DriverVersion": identity["driverVersion"],
            "Graphics.HardwareFallback": identity["hardwareFallback"],
            "Display.Primary": identity["primaryDisplay"],
            "Display.PrimaryDpi": identity["primaryDpi"],
            "Display.RefreshRateNumerator": identity["refreshRateNumerator"],
            "Display.RefreshRateDenominator": identity["refreshRateDenominator"],
            "Graphics.OutputMapping": identity["outputMapping"],
        }
        configuration = {
            "Output.Width": identity["outputWidth"],
            "Output.Height": identity["outputHeight"],
            "Display.HdrEnabled": identity["hdrEnabled"],
        }
        blocks = [
            *(
                _event("SupportReport", session, support)
                for _ in range(support_count)
            ),
            *(
                _event("Configuration.Applied", session, configuration)
                for _ in range(configuration_count)
            ),
            *(
                [
                    _event(
                        "Display.Topology.Invalidated",
                        session,
                        {"PowerUnavailable": True},
                    )
                ]
                if ordinal in self.power_unavailable_runs
                else []
            ),
            *(
                _event(
                    "Performance.Interval",
                    session,
                    self.interval(ordinal, roi_enabled, complete_interval),
                )
                for complete_interval in range(1, interval_count + 1)
            ),
        ]
        blocks.append(_event("Process.Exited", session, {}))
        path.write_text("\n---\n".join(blocks) + "\n---\n", encoding="utf-8")

    def write(self) -> None:
        self.manifest["runs"] = []
        pattern = (("A", False), ("B", True), ("B", True), ("A", False))
        for ordinal in range(1, 21):
            block = (ordinal - 1) // 4 + 1
            position = (ordinal - 1) % 4 + 1
            arm, roi_enabled = pattern[position - 1]
            directory = f"run-{ordinal:02d}-{arm.lower()}-roi-{'on' if roi_enabled else 'off'}"
            run_root = self.root / directory
            run_root.mkdir(exist_ok=True)
            executable = run_root / "ba-click-fx-desktop.exe"
            executable.write_bytes(self.executable_bytes)
            config = json.loads(json.dumps(self.base_config))
            config["performance"]["activeFxRoiEnabled"] = roi_enabled
            config_path = run_root / "BAFX.config.json"
            config_path.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
            run = {
                "ordinal": ordinal,
                "block": block,
                "position": position,
                "arm": arm,
                "roiEnabled": roi_enabled,
                "directory": directory,
                "executable": f"{directory}/ba-click-fx-desktop.exe",
                "config": f"{directory}/BAFX.config.json",
                "log": f"{directory}/ba-click-fx-desktop-support.log",
                "arguments": [
                    f"--demo-scenario={self.scenario_id}",
                    "--demo-age-ms=130",
                    "--demo-delay-ms=5000",
                    "--disable-raw-input",
                    "--quit-after-ms=40500",
                ] + (["--spout2"] if self.measurement_path == "recording-rebuild" else []),
                "startedAtUtc": "2026-08-24T12:00:00.000Z",
                "elapsedMs": 40_600,
                "exitCode": 0,
                "executableSha256": _sha256(executable),
                "configSha256": _sha256(config_path),
            }
            self.manifest["runs"].append(run)
        for ordinal, run in enumerate(self.manifest["runs"], 1):
            self.write_log(ordinal, run["roiEnabled"])
        self.write_manifest()

    def rewrite_logs(self) -> None:
        for ordinal, run in enumerate(self.manifest["runs"], 1):
            self.write_log(ordinal, run["roiEnabled"])

    def write_manifest(self) -> None:
        (self.root / "capture.json").write_text(
            json.dumps(self.manifest, indent=2) + "\n", encoding="utf-8"
        )


class ActiveFxRoiAbReporterTests(unittest.TestCase):
    def test_event_parser_accepts_identical_duplicates_only(self) -> None:
        path = Path("support.log")
        event = REPORTER._parse_event(
            "Log.SchemaVersion=2\nEvent.Name=SupportReport\nLog.SchemaVersion=2\n",
            path,
        )
        self.assertEqual(event["Log.SchemaVersion"], "2")
        with self.assertRaisesRegex(
            REPORTER.ValidationError, "conflicting duplicate event field"
        ):
            REPORTER._parse_event(
                "Log.SchemaVersion=2\nLog.SchemaVersion=3\n",
                path,
            )

    def test_applied_capture_passes_all_release_gates(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary))
            report = REPORTER.build_report(fixture.root)

            self.assertTrue(report["passed"])
            self.assertEqual(report["paired"]["roiNotSlowerCount"], 10)
            self.assertAlmostEqual(report["roiOn"]["appliedRequestedRatio"], 0.96)
            self.assertAlmostEqual(
                report["roiOn"]["prefilterDrawnFullRatio"], 0.4
            )
            self.assertAlmostEqual(report["roiOn"]["pyramidDrawnFullRatio"], 0.4)
            markdown = REPORTER.render_markdown(report)
            self.assertIn("Bloom/final p95", markdown)
            self.assertIn("NVIDIA GeForce RTX 4060 Laptop GPU", markdown)
            self.assertEqual(report["captureSchemaVersion"], 3)
            self.assertEqual(
                report["environment"]["identity"], fixture.environment_identity
            )
            self.assertNotIn(
                "no deterministic interior-trail driver",
                "\n".join(report["limitations"]),
            )

    def test_accepts_each_scenario_with_its_exact_workload(self) -> None:
        for scenario_id in ("center-click", "interior-trail", "boundary-top-left"):
            with self.subTest(scenario=scenario_id), tempfile.TemporaryDirectory() as temporary:
                fixture = CaptureFixture(Path(temporary), scenario_id=scenario_id)
                report = REPORTER.build_report(fixture.root)
                self.assertTrue(report["passed"])
                self.assertEqual(report["scenario"]["id"], scenario_id)

    def test_manifest_environment_is_strict_and_schema_1_is_rejected(self) -> None:
        mutations = (
            ("old schema", lambda fixture: fixture.manifest.__setitem__("schemaVersion", 2), "schemaVersion must be 3"),
            (
                "wrong contract",
                lambda fixture: fixture.manifest["environment"].__setitem__(
                    "contract", "other"
                ),
                "environment.contract",
            ),
            (
                "missing field",
                lambda fixture: fixture.manifest["environment"]["identity"].pop(
                    "driverVersion"
                ),
                "fields differ",
            ),
            (
                "unknown field",
                lambda fixture: fixture.manifest["environment"]["identity"].__setitem__(
                    "unknown", "value"
                ),
                "fields differ",
            ),
            (
                "wrong type",
                lambda fixture: fixture.manifest["environment"]["identity"].__setitem__(
                    "outputWidth", "3840"
                ),
                "outputWidth must be an integer",
            ),
        )
        for label, mutate, message in mutations:
            with self.subTest(case=label), tempfile.TemporaryDirectory() as temporary:
                fixture = CaptureFixture(Path(temporary))
                mutate(fixture)
                fixture.write_manifest()
                with self.assertRaisesRegex(REPORTER.ValidationError, message):
                    REPORTER.build_report(fixture.root)

    def test_each_run_must_match_the_fixed_hardware_contract(self) -> None:
        cases = (
            ("driverType", "WARP", "hardware D3D11"),
            ("adapter", "NVIDIA GeForce RTX 4050", "adapter must contain"),
            ("hardwareFallback", "warp-after-failure", "hardware fallback"),
            ("outputWidth", 2560, "output must be 3840x2160"),
            ("outputHeight", 1440, "output must be 3840x2160"),
            ("refreshRateNumerator", 60, "refresh rate must be 170/1"),
            ("refreshRateDenominator", 1000, "refresh rate must be 170/1"),
            ("hdrEnabled", True, "HdrEnabled must be false"),
            ("outputMapping", "scrgb-linear", "OutputMapping must be conservative-sdr"),
        )
        for field, value, message in cases:
            with self.subTest(field=field), tempfile.TemporaryDirectory() as temporary:
                fixture = CaptureFixture(Path(temporary))
                fixture.environment_overrides[7] = {field: value}
                fixture.write_log(7, fixture.manifest["runs"][6]["roiEnabled"])
                with self.assertRaisesRegex(REPORTER.ValidationError, message):
                    REPORTER.build_report(fixture.root)

    def test_rejects_cross_run_environment_identity_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary))
            fixture.environment_overrides[20] = {
                "adapterLuid": "00000000:00ABCDEF"
            }
            fixture.write_log(20, fixture.manifest["runs"][19]["roiEnabled"])
            with self.assertRaisesRegex(REPORTER.ValidationError, "drift at adapterLuid"):
                REPORTER.build_report(fixture.root)

    def test_rejects_manifest_identity_not_proven_by_logs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary))
            fixture.manifest["environment"]["identity"]["driverVersion"] = "99.0"
            fixture.write_manifest()
            with self.assertRaisesRegex(REPORTER.ValidationError, "drift at driverVersion"):
                REPORTER.build_report(fixture.root)

    def test_requires_one_support_and_configuration_event_per_run(self) -> None:
        cases = (
            ("SupportReport", 0, 1),
            ("SupportReport", 2, 1),
            ("Configuration.Applied", 1, 0),
            ("Configuration.Applied", 1, 2),
        )
        for event_name, support_count, configuration_count in cases:
            with self.subTest(event=event_name, count=(support_count, configuration_count)), tempfile.TemporaryDirectory() as temporary:
                fixture = CaptureFixture(Path(temporary))
                fixture.write_log(
                    1,
                    False,
                    support_count=support_count,
                    configuration_count=configuration_count,
                )
                with self.assertRaisesRegex(
                    REPORTER.ValidationError, f"expected one {re.escape(event_name)} event"
                ):
                    REPORTER.build_report(fixture.root)

    def test_rejects_scenario_workload_or_command_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary), scenario_id="interior-trail")
            fixture.manifest["scenario"]["workload"] = "fixed-age-center-click"
            fixture.write_manifest()
            with self.assertRaisesRegex(REPORTER.ValidationError, "scenario.workload"):
                REPORTER.build_report(fixture.root)

            fixture.manifest["scenario"]["workload"] = "fixed-age-interior-trail"
            fixture.manifest["runs"][0]["arguments"][0] = "--demo-scenario=center-click"
            fixture.write_manifest()
            with self.assertRaisesRegex(REPORTER.ValidationError, "workload or measurement-path"):
                REPORTER.build_report(fixture.root)

    def test_recording_rebuild_requires_spout2_and_uses_its_counters(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary), measurement_path="recording-rebuild")
            report = REPORTER.build_report(fixture.root)
            self.assertTrue(report["passed"])

            fixture.manifest["runs"][0]["arguments"].remove("--spout2")
            fixture.write_manifest()
            with self.assertRaisesRegex(REPORTER.ValidationError, "measurement-path"):
                REPORTER.build_report(fixture.root)

        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary))
            fixture.manifest["runs"][0]["arguments"].append("--spout2")
            fixture.write_manifest()
            with self.assertRaisesRegex(REPORTER.ValidationError, "measurement-path"):
                REPORTER.build_report(fixture.root)

    def test_rejects_non_contract_schedule_and_run_count(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary))
            fixture.manifest["schedule"]["blocks"] = 4
            fixture.write_manifest()
            with self.assertRaisesRegex(REPORTER.ValidationError, "schedule"):
                REPORTER.build_report(fixture.root)

            fixture.manifest["schedule"]["blocks"] = 5
            fixture.manifest["runs"].pop()
            fixture.write_manifest()
            with self.assertRaisesRegex(REPORTER.ValidationError, "exactly 20"):
                REPORTER.build_report(fixture.root)

    def test_rejects_wrong_abba_arm(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary))
            fixture.manifest["runs"][1]["arm"] = "A"
            fixture.write_manifest()
            with self.assertRaisesRegex(REPORTER.ValidationError, "ABBA arm"):
                REPORTER.build_report(fixture.root)

    def test_rejects_executable_hash_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary))
            executable = fixture.root / fixture.manifest["runs"][7]["executable"]
            executable.write_bytes(b"different executable")
            with self.assertRaisesRegex(REPORTER.ValidationError, "executable identity"):
                REPORTER.build_report(fixture.root)

    def test_rejects_configuration_difference_outside_roi_boolean(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary))
            run = fixture.manifest["runs"][2]
            config_path = fixture.root / run["config"]
            config = json.loads(config_path.read_text(encoding="utf-8"))
            config["effects"]["enabled"] = False
            config_path.write_text(json.dumps(config) + "\n", encoding="utf-8")
            run["configSha256"] = _sha256(config_path)
            fixture.write_manifest()
            with self.assertRaisesRegex(REPORTER.ValidationError, "differ outside"):
                REPORTER.build_report(fixture.root)

    def test_rejects_non_schema_19_run_configuration(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary))
            run = fixture.manifest["runs"][0]
            config_path = fixture.root / run["config"]
            config = json.loads(config_path.read_text(encoding="utf-8"))
            config["schemaVersion"] = 18
            config_path.write_text(json.dumps(config) + "\n", encoding="utf-8")
            run["configSha256"] = _sha256(config_path)
            fixture.write_manifest()
            with self.assertRaisesRegex(REPORTER.ValidationError, "schemaVersion must be 19"):
                REPORTER.build_report(fixture.root)

    def test_rejects_capture_without_five_second_warmup_and_thirty_second_sample(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary))
            fixture.write_log(1, False, interval_count=3)
            with self.assertRaisesRegex(REPORTER.ValidationError, "4 complete intervals"):
                REPORTER.build_report(fixture.root)

    def test_rejects_each_selected_interval_without_presented_frames(self) -> None:
        for selected_interval in range(1, 4):
            with self.subTest(selected_interval=selected_interval):
                with tempfile.TemporaryDirectory() as temporary:
                    fixture = CaptureFixture(Path(temporary))
                    complete_interval = selected_interval + 1
                    fixture.interval_overrides[(1, complete_interval)] = {
                        "Window.FrameCount": 0,
                        "Cpu.PresentCall.Samples": 0,
                        "ROI.Primary.ObservedFrames": 0,
                    }
                    fixture.rewrite_logs()
                    with self.assertRaisesRegex(
                        REPORTER.ValidationError,
                        rf"selected interval {selected_interval}: has no presented frames",
                    ):
                        REPORTER.build_report(fixture.root)

    def test_rejects_selected_interval_present_sample_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary))
            fixture.interval_overrides[(1, 2)] = {
                "Cpu.PresentCall.Samples": 1199,
            }
            fixture.rewrite_logs()
            with self.assertRaisesRegex(
                REPORTER.ValidationError,
                "Cpu.PresentCall.Samples does not match Window.FrameCount",
            ):
                REPORTER.build_report(fixture.root)

    def test_rejects_selected_interval_roi_observed_frame_drift(self) -> None:
        paths = (
            ("primary", "ROI.Primary"),
            ("recording-rebuild", "ROI.RecordingRebuild"),
        )
        for measurement_path, roi_prefix in paths:
            with self.subTest(measurement_path=measurement_path):
                with tempfile.TemporaryDirectory() as temporary:
                    fixture = CaptureFixture(
                        Path(temporary), measurement_path=measurement_path
                    )
                    fixture.interval_overrides[(1, 2)] = {
                        f"{roi_prefix}.ObservedFrames": 1199,
                    }
                    fixture.rewrite_logs()
                    with self.assertRaisesRegex(
                        REPORTER.ValidationError,
                        rf"{re.escape(roi_prefix)}.ObservedFrames does not match "
                        "Window.FrameCount",
                    ):
                        REPORTER.build_report(fixture.root)

    def test_rejects_capture_with_power_unavailable_event(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary))
            fixture.power_unavailable_runs.add(1)
            fixture.rewrite_logs()
            with self.assertRaisesRegex(
                REPORTER.ValidationError,
                "display power was unavailable during capture",
            ):
                REPORTER.build_report(fixture.root)

    def test_failed_error_pending_and_pair_gates_are_reported(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary))
            for ordinal in (2, 6, 10):
                fixture.overrides[ordinal] = {
                    "GPU.BloomAndFinalComposite.P95": 1100,
                    "GPU.PendingFrames.Max": 2,
                    "GPU.QueryFailures": 1,
                }
            fixture.rewrite_logs()
            report = REPORTER.build_report(fixture.root)
            failed = {gate["id"] for gate in report["gates"] if not gate["passed"]}

            self.assertFalse(report["passed"])
            self.assertIn("gpu-pending-max", failed)
            self.assertIn("runtime-errors", failed)
            self.assertIn("paired-roi-not-slower", failed)
            self.assertEqual(report["paired"]["roiNotSlowerCount"], 7)

    def test_rejects_aggregate_and_stage_pixel_total_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary))
            fixture.overrides[2] = {
                "ROI.Primary.Stage.Resolve.DrawnPixels.Total": 99_999
            }
            fixture.rewrite_logs()
            with self.assertRaisesRegex(
                REPORTER.ValidationError, "does not equal the staged total"
            ):
                REPORTER.build_report(fixture.root)

    def test_fallback_scenario_checks_reason_and_regression(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(Path(temporary), expectation="fallback")
            report = REPORTER.build_report(fixture.root)
            self.assertTrue(report["passed"])

            fixture.overrides[2] = {
                "ROI.Primary.Reason.background-differential-bloom.Frames": 999
            }
            fixture.rewrite_logs()
            report = REPORTER.build_report(fixture.root)
            failed = {gate["id"] for gate in report["gates"] if not gate["passed"]}
            self.assertIn("fallback-reason-coverage", failed)

    def test_boundary_fallback_uses_top_level_active_status(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = CaptureFixture(
                Path(temporary),
                expectation="fallback",
                expected_reason="boundary-fallback",
            )
            report = REPORTER.build_report(fixture.root)

            self.assertTrue(report["passed"])
            self.assertEqual(report["roiOn"]["expectedReasonRatio"], 1.0)

    def test_rejects_duplicate_manifest_fields(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "capture.json").write_text(
                '{"schemaVersion":2,"schemaVersion":2}\n', encoding="utf-8"
            )
            with self.assertRaisesRegex(REPORTER.ValidationError, "duplicate field"):
                REPORTER.build_report(root)


if __name__ == "__main__":
    unittest.main()
