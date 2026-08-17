#!/usr/bin/env python3
"""Contract tests for the SPK-002 WGC session exclusion verifier."""

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


SCRIPT_PATH = (
    Path(__file__).resolve().parents[1]
    / "tools"
    / "verify-wgc-session-exclusion-spike.py"
)
RUNBOOK_PATH = (
    Path(__file__).resolve().parents[1]
    / "tools"
    / "run-wgc-session-exclusion-spike.ps1"
)
SPEC = importlib.util.spec_from_file_location(
    "verify_wgc_session_exclusion_spike", SCRIPT_PATH
)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"unable to load {SCRIPT_PATH}")
VERIFY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = VERIFY
SPEC.loader.exec_module(VERIFY)

WIDTH = 640
HEIGHT = 320
RAW_BYTES = WIDTH * HEIGHT * 8
MONITOR_SIZE = (1920, 1080)
OVERLAY_ROI = (64, 64, 192, 192)
CONTROL_ROI = (416, 64, 192, 192)
MARKER_ROI = (328, 128, 64, 64)
MARKER_REFERENCE_ROI = (416, 64, 64, 64)
BACKGROUND = tuple(
    VERIFY._srgb_to_linear(channel) for channel in (30, 82, 146)
) + (1.0,)
PROBE = (0.82, 0.16, 0.52, 1.0)
MARKERS = {
    "includedBefore": (
        tuple(VERIFY._srgb_to_linear(channel) for channel in (224, 48, 64))
        + (1.0,),
        [224, 48, 64],
    ),
    "excluded": (
        tuple(VERIFY._srgb_to_linear(channel) for channel in (48, 220, 80))
        + (1.0,),
        [48, 220, 80],
    ),
    "includedAfter": (
        tuple(VERIFY._srgb_to_linear(channel) for channel in (240, 208, 48))
        + (1.0,),
        [240, 208, 48],
    ),
}
ARTIFACT_NAMES = (
    "included-before.rgba16f",
    "included-before.png",
    "excluded.rgba16f",
    "excluded.png",
    "included-after.rgba16f",
    "included-after.png",
)


def _png_header(width: int = WIDTH, height: int = HEIGHT) -> bytes:
    return b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR" + struct.pack(
        ">II", width, height
    )


def _region_document(region: tuple[int, int, int, int]) -> dict[str, int]:
    return {
        "left": region[0],
        "top": region[1],
        "width": region[2],
        "height": region[3],
    }


def _paint_region(
    payload: bytearray,
    region: tuple[int, int, int, int],
    color: tuple[float, float, float, float],
) -> None:
    pixel = struct.pack("<4e", *color)
    for y in range(region[1], region[1] + region[3]):
        start = (y * WIDTH + region[0]) * 8
        payload[start : start + region[2] * 8] = pixel * region[2]


def _write_raw_frames(directory: Path) -> None:
    background = struct.pack("<4e", *BACKGROUND)
    blank = background * (WIDTH * HEIGHT)
    for stage, stem, included in (
        ("includedBefore", "included-before", True),
        ("excluded", "excluded", False),
        ("includedAfter", "included-after", True),
    ):
        payload = bytearray(blank)
        if included:
            _paint_region(payload, OVERLAY_ROI, PROBE)
        _paint_region(payload, MARKER_ROI, MARKERS[stage][0])
        (directory / f"{stem}.rgba16f").write_bytes(payload)
        (directory / f"{stem}.png").write_bytes(_png_header())


def _frame(
    previous_generation: int,
    generation: int,
    marker: int,
    iteration: int,
) -> dict[str, object]:
    return {
        "previousGeneration": previous_generation,
        "generation": generation,
        "markerNs": marker,
        "capturedAtNs": marker + 10,
        "configurationIteration": iteration,
        "contentSize": {
            "width": MONITOR_SIZE[0],
            "height": MONITOR_SIZE[1],
        },
    }


def _window_exclusion(
    excluded: bool,
    iteration: int,
    *,
    hresult: int = 0,
    confirmed: bool = True,
) -> dict[str, object]:
    window_ids = [0x123400001234] if excluded else []
    return {
        "setHresult": hresult,
        "getHresult": 0 if hresult == 0 else -2147467259,
        "windowIdHresult": 0,
        "sessionIterationHresult": 0,
        "setIteration": iteration,
        "sessionIteration": iteration,
        "requestedWindowIds": window_ids,
        "observedWindowIds": window_ids if confirmed else [],
        "confirmed": confirmed,
        "iterationObserved": confirmed,
    }


def _observation(
    stage: str,
    stem: str,
    excluded: bool,
    first_generation: int,
    marker: int,
    iteration: int,
) -> dict[str, object]:
    return {
        "requestedExcluded": excluded,
        "captured": True,
        "displayAffinity": 0,
        "windowExclusion": _window_exclusion(excluded, iteration),
        "observedExtendedStyle": 0x00080020,
        "layeredStyleRestored": True,
        "transparentStyleRestored": True,
        "markerSrgb8": MARKERS[stage][1],
        "stablePair": {
            "first": _frame(
                first_generation - 1,
                first_generation,
                marker,
                iteration,
            ),
            "second": _frame(
                first_generation,
                first_generation + 1,
                marker + 20,
                iteration,
            ),
            "maximumRgbDelta": 0.0,
        },
        "artifact": {
            "raw": f"{stem}.rgba16f",
            "png": f"{stem}.png",
            "width": WIDTH,
            "height": HEIGHT,
            "rawBytes": RAW_BYTES,
        },
    }


def _comparison_document(
    metrics: VERIFY.PairMetrics,
    roi: tuple[int, int, int, int] = OVERLAY_ROI,
) -> dict[str, object]:
    return {
        "roi": _region_document(roi),
        "threshold": 0.02,
        "maximumRgbDelta": metrics.maximum_rgb_delta,
        "differentPixels": metrics.different_pixels,
    }


def _spatial_comparison_document(
    metrics: VERIFY.PairMetrics,
    left_roi: tuple[int, int, int, int],
    right_roi: tuple[int, int, int, int],
) -> dict[str, object]:
    return {
        "leftRoi": _region_document(left_roi),
        "rightRoi": _region_document(right_roi),
        "threshold": 0.02,
        "maximumRgbDelta": metrics.maximum_rgb_delta,
        "differentPixels": metrics.different_pixels,
    }


def _marker_document(image: VERIFY.RawImage) -> dict[str, object]:
    reference = VERIFY._spatial_pair_metrics(
        image, MARKER_ROI, MARKER_REFERENCE_ROI, 0.02
    )
    mean = VERIFY._region_mean_rgb(image, MARKER_ROI)
    return {
        "meanLinear": {"r": mean[0], "g": mean[1], "b": mean[2]},
        "rgbRange": VERIFY._region_rgb_range(image, (MARKER_ROI,)),
        "vsReference": _spatial_comparison_document(
            reference, MARKER_ROI, MARKER_REFERENCE_ROI
        ),
    }


def _ledger(frame_count: int = 8, used: bool = True) -> dict[str, object]:
    count = 1 if used else 0
    return {
        "framesAcquired": frame_count,
        "framesClosed": frame_count,
        "framePoolsCreated": count,
        "framePoolsClosed": count,
        "sessionsCreated": count,
        "sessionsClosed": count,
        "frameArrivedRegistrations": count,
        "frameArrivedUnregistrations": count,
        "itemClosedRegistrations": count,
        "itemClosedUnregistrations": count,
        "liveFrames": 0,
        "liveFramePools": 0,
        "liveSessions": 0,
        "liveFrameArrivedRegistrations": 0,
        "liveItemClosedRegistrations": 0,
        "failures": 0,
        "allReleased": True,
    }


def _load_images(
    document: dict[str, object], directory: Path
) -> dict[str, VERIFY.RawImage]:
    observations = document["observations"]
    images = {}
    for stage in ("includedBefore", "excluded", "includedAfter"):
        artifact = observations[stage]["artifact"]
        images[stage] = VERIFY._load_raw_image(
            directory / artifact["raw"], WIDTH, HEIGHT, RAW_BYTES, stage
        )
    return images


def _refresh_metrics(document: dict[str, object], directory: Path) -> None:
    images = _load_images(document, directory)
    before = VERIFY._pair_metrics(
        images["includedBefore"], images["excluded"], OVERLAY_ROI, 0.02
    )
    after = VERIFY._pair_metrics(
        images["includedAfter"], images["excluded"], OVERLAY_ROI, 0.02
    )
    stability = VERIFY._pair_metrics(
        images["includedBefore"], images["includedAfter"], OVERLAY_ROI, 0.02
    )
    marker_before_excluded = VERIFY._pair_metrics(
        images["includedBefore"], images["excluded"], MARKER_ROI, 0.02
    )
    marker_excluded_after = VERIFY._pair_metrics(
        images["excluded"], images["includedAfter"], MARKER_ROI, 0.02
    )
    marker_before_after = VERIFY._pair_metrics(
        images["includedBefore"], images["includedAfter"], MARKER_ROI, 0.02
    )
    excluded_spatial = VERIFY._spatial_pair_metrics(
        images["excluded"], OVERLAY_ROI, CONTROL_ROI, 0.02
    )
    document["metrics"] = {
        "includedBeforeVsExcluded": _comparison_document(before),
        "includedAfterVsExcluded": _comparison_document(after),
        "includedBeforeVsAfter": _comparison_document(stability),
        "markerIncludedBeforeVsExcluded": _comparison_document(
            marker_before_excluded, MARKER_ROI
        ),
        "markerExcludedVsIncludedAfter": _comparison_document(
            marker_excluded_after, MARKER_ROI
        ),
        "markerIncludedBeforeVsAfter": _comparison_document(
            marker_before_after, MARKER_ROI
        ),
        "excludedOverlayVsControl": _spatial_comparison_document(
            excluded_spatial, OVERLAY_ROI, CONTROL_ROI
        ),
        "markers": {
            "includedBefore": _marker_document(images["includedBefore"]),
            "excluded": _marker_document(images["excluded"]),
            "includedAfter": _marker_document(images["includedAfter"]),
        },
        "controlMaximumRgbDelta": VERIFY._region_maximum_delta(
            (
                images["includedBefore"],
                images["excluded"],
                images["includedAfter"],
            ),
            CONTROL_ROI,
        ),
        "excludedOverlayRgbRange": VERIFY._region_rgb_range(
            images["excluded"], (OVERLAY_ROI,)
        ),
    }


def _valid_capture(directory: Path) -> dict[str, object]:
    _write_raw_frames(directory)
    document = {
        "schema": "spk-002-session-exclusion/v1",
        "spikeId": "SPK-002-SESSION-EXCLUSION",
        "applicationVersion": "0.1.0-test",
        "revision": "test-revision",
        "capturedAtUtc": "2026-08-18T00:00:00.000Z",
        "timeoutMs": 12000,
        "capability": {
            "status": "Available",
            "interfaces": {
                "displaySessionQi": 0,
                "session7Qi": 0,
                "frame3Qi": 0,
            },
        },
        "evidence": {
            "result": "Passed",
            "lastPhase": "artifact-write.end",
            "failureReason": "",
            "watchdog": {
                "hardTimeoutMs": 15000,
                "deadlineExpired": False,
            },
        },
        "contract": {
            "scope": "controlled-monitor-WGC-session-window-exclusion-pixels-only",
            "captureTarget": "MONITOR",
            "surfaceFormat": "DXGI_FORMAT_R16G16B16A16_FLOAT",
            "sessionTopology": "single-monitor-session-empty-window-single-window-empty",
            "overlayDisplayAffinity": "WDA_NONE",
            "validatesProductStopWdaStartTransaction": False,
            "systemBorderAllowed": True,
            "cursorCaptureEnabled": False,
            "frameMarkerSemantic": "stage-unique-solid-srgb8",
            "maximumStableSampleAttempts": 4,
            "stableSampleTolerance": 0.01,
            "differenceThreshold": 0.02,
            "maximumControlDelta": 0.01,
            "maximumIncludedDelta": 0.01,
            "maximumExcludedRange": 0.01,
            "maximumExcludedBackgroundDelta": 0.01,
            "minimumOverlayDelta": 0.20,
            "maximumMarkerRange": 0.01,
            "minimumMarkerDelta": 0.10,
            "markerChannelMargin": 0.05,
            "minimumChangedFraction": 0.95,
        },
        "sdk": {
            "name": "Windows SDK",
            "version": "10.0.26100.0",
            "targetWin32Winnt": 0x0A00,
        },
        "osVersion": {"major": 10, "minor": 0, "build": 26100},
        "rendererDevice": {
            "driverType": "hardware",
            "adapter": "Test GPU",
            "adapterLuid": {"low": 1, "high": 0},
            "vendorId": 0x10DE,
            "deviceId": 0x1234,
            "driverVersion": 1,
            "featureLevel": 45312,
        },
        "observerFeatureLevel": 45312,
        "display": {
            "width": MONITOR_SIZE[0],
            "height": MONITOR_SIZE[1],
            "colorMode": "Sdr",
            "bitDepth": 8,
            "dxgiColorSpace": 0,
        },
        "fixture": {
            "monitorBounds": {
                "left": 0,
                "top": 0,
                "right": MONITOR_SIZE[0],
                "bottom": MONITOR_SIZE[1],
            },
            "captureScreenBounds": {
                "left": 100,
                "top": 200,
                "right": 100 + WIDTH,
                "bottom": 200 + HEIGHT,
            },
            "overlayScreenBounds": {
                "left": 132,
                "top": 232,
                "right": 388,
                "bottom": 488,
            },
            "captureRegion": _region_document((100, 200, WIDTH, HEIGHT)),
            "overlayRoi": _region_document(OVERLAY_ROI),
            "controlRoi": _region_document(CONTROL_ROI),
            "markerRoi": _region_document(MARKER_ROI),
            "markerReferenceRoi": _region_document(MARKER_REFERENCE_ROI),
            "backgroundSrgb8": [30, 82, 146],
            "markerColorsSrgb8": {
                stage: marker[1] for stage, marker in MARKERS.items()
            },
            "probeLinear": {"r": 0.82, "g": 0.16, "b": 0.52, "a": 1.0},
        },
        "observations": {
            "includedBefore": _observation(
                "includedBefore", "included-before", False, 1, 100, 10
            ),
            "excluded": _observation(
                "excluded", "excluded", True, 3, 200, 20
            ),
            "includedAfter": _observation(
                "includedAfter", "included-after", False, 5, 300, 30
            ),
        },
        "metrics": {},
        "cleanup": {"result": "Passed", "ledgerBalanced": True},
        "resourceLedger": _ledger(),
    }
    _refresh_metrics(document, directory)
    return document


def _mark_not_captured(document: dict[str, object]) -> None:
    for observation in document["observations"].values():
        observation["captured"] = False


class SessionExclusionVerifierTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temporary.name)
        cls.baseline_document = _valid_capture(cls.directory)
        cls.baseline_artifacts = {
            name: (cls.directory / name).read_bytes() for name in ARTIFACT_NAMES
        }

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def setUp(self) -> None:
        # Rebuilding three full FP16 frames for every test is needlessly slow.
        self.document = copy.deepcopy(self.baseline_document)
        self.mutated_artifacts: set[str] = set()

    def tearDown(self) -> None:
        for name in self.mutated_artifacts:
            (self.directory / name).write_bytes(self.baseline_artifacts[name])

    def write_artifact(self, name: str, payload: bytes | bytearray) -> Path:
        self.mutated_artifacts.add(name)
        path = self.directory / name
        path.write_bytes(payload)
        return path

    def validate(self) -> VERIFY.VerificationResult:
        return VERIFY.validate_capture(self.document, self.directory)

    def write_document(self, name: str = "session-exclusion.json") -> Path:
        path = self.directory / name
        path.write_text(json.dumps(self.document), encoding="utf-8")
        return path

    def test_available_passed_capture_is_accepted(self) -> None:
        result = self.validate()
        self.assertEqual(result.status, "accepted")
        self.assertEqual(result.capability_status, "Available")
        self.assertEqual(result.evidence_result, "Passed")
        self.assertEqual(result.included_before_different_pixels, 192 * 192)
        self.assertEqual(result.included_after_different_pixels, 192 * 192)
        self.assertEqual(result.excluded_background_different_pixels, 0)
        self.assertEqual(result.minimum_marker_pair_different_pixels, 64 * 64)

    def test_unavailable_capability_report_is_accepted(self) -> None:
        self.document["capability"] = {
            "status": "Unavailable",
            "interfaces": {
                "displaySessionQi": -2147467262,
                "session7Qi": -2147467262,
                "frame3Qi": -2147467262,
            },
        }
        self.document["evidence"]["result"] = "Not Run"
        _mark_not_captured(self.document)
        self.document["cleanup"] = {
            "result": "NotRun",
            "ledgerBalanced": True,
        }
        self.document["resourceLedger"] = _ledger(0, used=False)

        result = self.validate()

        self.assertEqual(result.capability_status, "Unavailable")
        self.assertEqual(result.evidence_result, "Not Run")
        self.assertEqual(result.frames_acquired, 0)

    def test_qi_success_with_set_get_failure_is_rejected_capability(self) -> None:
        self.document["capability"]["status"] = "Rejected"
        self.document["evidence"]["result"] = "Not Run"
        _mark_not_captured(self.document)
        exclusion = self.document["observations"]["includedBefore"][
            "windowExclusion"
        ]
        exclusion.update(
            {
                "setHresult": -2147024891,
                "getHresult": -2147467259,
                "confirmed": False,
                "iterationObserved": False,
            }
        )

        result = self.validate()

        self.assertEqual(result.capability_status, "Rejected")
        self.assertEqual(result.evidence_result, "Not Run")

    def test_unknown_capability_status_is_rejected(self) -> None:
        self.document["capability"]["status"] = "Supported"
        with self.assertRaisesRegex(VERIFY.ValidationError, "status is unknown"):
            self.validate()

    def test_qi_failure_must_not_be_classified_rejected(self) -> None:
        self.document["capability"]["status"] = "Rejected"
        self.document["capability"]["interfaces"]["session7Qi"] = -2147467262
        self.document["evidence"]["result"] = "Not Run"
        _mark_not_captured(self.document)
        with self.assertRaisesRegex(VERIFY.ValidationError, "requires successful interface QI"):
            self.validate()

    def test_non_unavailable_qi_failure_is_not_a_rejected_operation(self) -> None:
        self.document["capability"]["status"] = "Rejected"
        self.document["capability"]["interfaces"]["session7Qi"] = -2147024891
        self.document["evidence"]["result"] = "Not Run"
        _mark_not_captured(self.document)
        with self.assertRaisesRegex(VERIFY.ValidationError, "requires successful interface QI"):
            self.validate()

    def test_rejected_requires_a_failed_set_or_get_operation(self) -> None:
        self.document["capability"]["status"] = "Rejected"
        self.document["evidence"]["result"] = "Not Run"
        _mark_not_captured(self.document)
        with self.assertRaisesRegex(VERIFY.ValidationError, "failed Set/Get"):
            self.validate()

    def test_unknown_hresult_is_rejected(self) -> None:
        self.document["observations"]["excluded"]["windowExclusion"][
            "setHresult"
        ] = -12345
        with self.assertRaisesRegex(VERIFY.ValidationError, "unknown HRESULT"):
            self.validate()

    def test_window_id_readback_must_match_requested_window(self) -> None:
        self.document["observations"]["excluded"]["windowExclusion"][
            "observedWindowIds"
        ] = [0x9999]
        with self.assertRaisesRegex(
            VERIFY.ValidationError, "WindowId readback differs"
        ):
            self.validate()

    def test_stable_frames_must_use_the_set_iteration(self) -> None:
        self.document["observations"]["excluded"]["stablePair"]["first"][
            "configurationIteration"
        ] = 10
        with self.assertRaisesRegex(VERIFY.ValidationError, "old configuration"):
            self.validate()

    def test_set_and_session_iterations_must_match(self) -> None:
        self.document["observations"]["includedAfter"]["windowExclusion"][
            "sessionIteration"
        ] = 29
        with self.assertRaisesRegex(
            VERIFY.ValidationError, "configuration iterations differ"
        ):
            self.validate()

    def test_configuration_iterations_must_advance_between_stages(self) -> None:
        observation = self.document["observations"]["includedAfter"]
        observation["windowExclusion"]["setIteration"] = 20
        observation["windowExclusion"]["sessionIteration"] = 20
        for frame in observation["stablePair"].values():
            if isinstance(frame, dict):
                frame["configurationIteration"] = 20
        with self.assertRaisesRegex(VERIFY.ValidationError, "iterations are not ordered"):
            self.validate()

    def test_stale_frame_qpc_is_rejected(self) -> None:
        frame = self.document["observations"]["includedBefore"]["stablePair"][
            "first"
        ]
        frame["capturedAtNs"] = frame["markerNs"]
        with self.assertRaisesRegex(VERIFY.ValidationError, "not newer"):
            self.validate()

    def test_generation_order_must_advance_between_stages(self) -> None:
        pair = self.document["observations"]["includedAfter"]["stablePair"]
        pair["first"]["previousGeneration"] = 3
        pair["first"]["generation"] = 4
        pair["second"]["previousGeneration"] = 4
        pair["second"]["generation"] = 5
        with self.assertRaisesRegex(VERIFY.ValidationError, "globally ordered"):
            self.validate()

    def test_nonfinite_fp16_is_rejected(self) -> None:
        path = self.directory / "included-before.rgba16f"
        payload = bytearray(path.read_bytes())
        payload[0:2] = struct.pack("<e", float("inf"))
        self.write_artifact(path.name, payload)
        with self.assertRaisesRegex(VERIFY.ValidationError, "non-finite"):
            self.validate()

    def test_artifact_path_escape_is_rejected(self) -> None:
        self.document["observations"]["excluded"]["artifact"]["raw"] = (
            "../excluded.rgba16f"
        )
        with self.assertRaisesRegex(
            VERIFY.ValidationError, "canonical|local file name"
        ):
            self.validate()

    def test_forged_collector_metrics_are_rejected(self) -> None:
        self.document["metrics"]["includedBeforeVsExcluded"][
            "differentPixels"
        ] -= 1
        with self.assertRaisesRegex(VERIFY.ValidationError, "raw FP16"):
            self.validate()

    def test_acceptance_thresholds_cannot_be_weakened(self) -> None:
        self.document["contract"]["differenceThreshold"] = 1.0
        with self.assertRaisesRegex(VERIFY.ValidationError, "differenceThreshold"):
            self.validate()

    def test_black_excluded_roi_cannot_impersonate_the_background(self) -> None:
        path = self.directory / "excluded.rgba16f"
        payload = bytearray(path.read_bytes())
        _paint_region(payload, OVERLAY_ROI, (0.0, 0.0, 0.0, 1.0))
        self.write_artifact(path.name, payload)
        _refresh_metrics(self.document, self.directory)
        with self.assertRaisesRegex(
            VERIFY.ValidationError, "controlled background|control ROI"
        ):
            self.validate()

    def test_resource_ledger_must_be_balanced(self) -> None:
        self.document["resourceLedger"]["liveSessions"] = 1
        self.document["resourceLedger"]["allReleased"] = False
        self.document["cleanup"]["ledgerBalanced"] = False
        with self.assertRaisesRegex(VERIFY.ValidationError, "all resources"):
            self.validate()

    def test_watchdog_timeout_is_rejected(self) -> None:
        self.document["evidence"]["watchdog"]["deadlineExpired"] = True
        with self.assertRaisesRegex(VERIFY.ValidationError, "deadline expired"):
            self.validate()

    def test_watchdog_deadline_must_match_capture_timeout(self) -> None:
        self.document["evidence"]["watchdog"]["hardTimeoutMs"] += 1
        with self.assertRaisesRegex(VERIFY.ValidationError, "inconsistent"):
            self.validate()

    def test_target_machine_runbook_keeps_collector_bounded(self) -> None:
        runbook = RUNBOOK_PATH.read_text(encoding="utf-8")
        for required in (
            "Start-Process",
            "WaitForExit($TimeoutMilliseconds)",
            "$process.Kill()",
            "--timeout-ms=$CaptureTimeoutMilliseconds",
            "verify-wgc-session-exclusion-spike.py",
            "verification.json",
        ):
            self.assertIn(required, runbook)
        self.assertNotIn("Remove-Item", runbook)

    def test_target_machine_runbook_preserves_existing_evidence(self) -> None:
        runbook = RUNBOOK_PATH.read_text(encoding="utf-8")
        self.assertIn(
            "Get-ChildItem -LiteralPath $OutputDirectory -Force",
            runbook,
        )
        self.assertIn("output directory is not empty", runbook)
        self.assertIn("choose a new directory to preserve prior evidence", runbook)

    def test_target_machine_runbook_leaves_watchdog_grace(self) -> None:
        runbook = RUNBOOK_PATH.read_text(encoding="utf-8")
        self.assertIn(
            "$minimumProcessTimeoutMilliseconds = $CaptureTimeoutMilliseconds + 5000",
            runbook,
        )
        self.assertIn(
            "Process timeout must be at least capture timeout plus 5000 ms",
            runbook,
        )

    def test_cli_rejects_duplicate_fields(self) -> None:
        path = self.directory / "duplicate-session-exclusion.json"
        path.write_text('{"schema":"a","schema":"b"}', encoding="utf-8")
        result = subprocess.run(
            [sys.executable, "-B", str(SCRIPT_PATH), str(path)],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("duplicate JSON field", result.stderr)

    def test_cli_rejects_nonfinite_json_numbers(self) -> None:
        path = self.directory / "nan-session-exclusion.json"
        path.write_text('{"schema":NaN}', encoding="utf-8")
        result = subprocess.run(
            [sys.executable, "-B", str(SCRIPT_PATH), str(path)],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("non-finite JSON number", result.stderr)

    def test_cli_replaces_verification_report_atomically(self) -> None:
        capture_path = self.write_document()
        report_path = self.directory / "verification.json"
        report_path.write_text('{"stale":true}\n', encoding="utf-8")
        result = subprocess.run(
            [
                sys.executable,
                "-B",
                str(SCRIPT_PATH),
                str(capture_path),
                f"--report={report_path}",
            ],
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(
            report["schema"], "spk-002-session-exclusion-verification/v1"
        )
        self.assertEqual(report["capture_schema"], "spk-002-session-exclusion/v1")
        self.assertEqual(report["status"], "accepted")
        self.assertEqual(report["capability_status"], "Available")
        self.assertEqual(report["evidence_result"], "Passed")
        self.assertFalse(report_path.with_name("verification.json.tmp").exists())


if __name__ == "__main__":
    unittest.main()
