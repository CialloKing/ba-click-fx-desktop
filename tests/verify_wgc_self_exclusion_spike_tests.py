#!/usr/bin/env python3
"""Contract tests for the SPK-002 WDA self-exclusion verifier."""

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
    / "verify-wgc-self-exclusion-spike.py"
)
SPEC = importlib.util.spec_from_file_location(
    "verify_wgc_self_exclusion_spike", SCRIPT_PATH
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
BACKGROUND = (0.1, 0.2, 0.3, 1.0)
PROBE = (1.0, 0.9, 0.8, 1.0)
MARKERS = {
    "included-before": ((0.8, 0.1, 0.15, 1.0), [224, 48, 64]),
    "excluded": ((0.1, 0.8, 0.15, 1.0), [48, 220, 80]),
    "included-after": ((0.8, 0.7, 0.1, 1.0), [240, 208, 48]),
}


def _png_header(width=WIDTH, height=HEIGHT):
    return b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR" + struct.pack(
        ">II", width, height
    )


def ledger(frame_count=12):
    return {
        "framesAcquired": frame_count,
        "framesClosed": frame_count,
        "framePoolsCreated": 1,
        "framePoolsClosed": 1,
        "framePoolsRecreated": 0,
        "sessionsCreated": 1,
        "sessionsClosed": 1,
        "frameArrivedRegistrations": 1,
        "frameArrivedUnregistrations": 1,
        "itemClosedRegistrations": 1,
        "itemClosedUnregistrations": 1,
        "liveFrames": 0,
        "liveFramePools": 0,
        "liveSessions": 0,
        "liveFrameArrivedRegistrations": 0,
        "liveItemClosedRegistrations": 0,
        "failures": 0,
        "allReleased": True,
    }


def _write_raw_frames(directory):
    background = struct.pack("<4e", *BACKGROUND)
    probe = struct.pack("<4e", *PROBE)
    excluded = background * (WIDTH * HEIGHT)
    included = bytearray(excluded)
    for y in range(32, 288):
        row_start = (y * WIDTH + 32) * 8
        row_end = (y * WIDTH + 288) * 8
        included[row_start:row_end] = probe * 256
    for stage, source in (
        ("included-before", included),
        ("excluded", excluded),
        ("included-after", included),
    ):
        payload = bytearray(source)
        marker = struct.pack("<4e", *MARKERS[stage][0])
        for y in range(MARKER_ROI[1], MARKER_ROI[1] + MARKER_ROI[3]):
            row_start = (y * WIDTH + MARKER_ROI[0]) * 8
            row_end = row_start + MARKER_ROI[2] * 8
            payload[row_start:row_end] = marker * MARKER_ROI[2]
        name = f"{stage}.rgba16f"
        (directory / name).write_bytes(payload)
        (directory / name.replace(".rgba16f", ".png")).write_bytes(_png_header())


def _region_document(region):
    return {
        "left": region[0],
        "top": region[1],
        "width": region[2],
        "height": region[3],
    }


def _comparison_document(metrics, roi=OVERLAY_ROI):
    return {
        "roi": _region_document(roi),
        "threshold": 0.02,
        "maximumRgbDelta": metrics.maximum_rgb_delta,
        "differentPixels": metrics.different_pixels,
    }


def frame(previous_generation, generation, marker):
    return {
        "previousGeneration": previous_generation,
        "generation": generation,
        "markerNs": marker,
        "capturedAtNs": marker + 10,
        "contentSize": {"width": MONITOR_SIZE[0], "height": MONITOR_SIZE[1]},
    }


def _spatial_comparison_document(metrics, left_roi, right_roi):
    return {
        "leftRoi": _region_document(left_roi),
        "rightRoi": _region_document(right_roi),
        "threshold": 0.02,
        "maximumRgbDelta": metrics.maximum_rgb_delta,
        "differentPixels": metrics.different_pixels,
    }


def _marker_document(image):
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


def observation(excluded, raw_name, previous_generation, first_generation, base_time):
    expected_affinity = 0x11 if excluded else 0
    second_generation = first_generation + 1
    stage = raw_name.removesuffix(".rgba16f")
    return {
        "requestedExcluded": excluded,
        "affinity": {
            "requested": expected_affinity,
            "observed": expected_affinity,
            "setSucceeded": True,
            "querySucceeded": True,
            "setError": 0,
            "queryError": 0,
            "confirmed": True,
        },
        "observedExtendedStyle": 0x00080020,
        "layeredStyleRestored": True,
        "transparentStyleRestored": True,
        "markerSrgb8": MARKERS[stage][1],
        "stablePair": {
            "first": frame(previous_generation, first_generation, base_time),
            "second": frame(first_generation, second_generation, base_time + 20),
            "maximumRgbDelta": 0.0,
        },
        "artifact": {
            "raw": raw_name,
            "png": raw_name.replace(".rgba16f", ".png"),
            "width": WIDTH,
            "height": HEIGHT,
            "rawBytes": RAW_BYTES,
        },
    }


def refresh_metrics(document, directory):
    observations = document["observations"]
    images = {}
    for key in ("includedBefore", "excluded", "includedAfter"):
        artifact = observations[key]["artifact"]
        images[key] = VERIFY._load_raw_image(
            directory / artifact["raw"], WIDTH, HEIGHT, RAW_BYTES, key
        )
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
        "excludedOverlayRgbRange": VERIFY._region_rgb_range(
            images["excluded"], (OVERLAY_ROI,)
        ),
        "controlMaximumRgbDelta": VERIFY._region_maximum_delta(
            (
                images["includedBefore"],
                images["excluded"],
                images["includedAfter"],
            ),
            CONTROL_ROI,
        ),
    }


def valid_capture(directory):
    _write_raw_frames(directory)
    document = {
        "schemaVersion": 1,
        "spikeId": "SPK-002-WDA-SELF-EXCLUSION",
        "applicationVersion": "0.1.0-test",
        "revision": "test",
        "capturedAtUtc": "2026-08-14T00:00:00.000Z",
        "timeoutMs": 12000,
        "contract": {
            "scope": "controlled-monitor-WDA-self-exclusion-pixels-only",
            "captureTarget": "MONITOR",
            "surfaceFormat": "DXGI_FORMAT_R16G16B16A16_FLOAT",
            "sessionTopology": "single-session-dynamic-affinity-matrix",
            "validatesProductStopWdaStartTransaction": False,
            "systemBorderAllowed": True,
            "cursorCaptureEnabled": False,
            "frameMarkerSemantic": "stage-unique-solid-srgb8",
            "maximumStableSampleAttempts": 4,
            "stableSampleTolerance": 0.01,
            "differenceThreshold": 0.02,
            "minimumChangedFraction": 0.95,
            "minimumOverlayDelta": 0.20,
            "maximumExcludedRange": 0.01,
            "maximumIncludedDelta": 0.01,
            "maximumControlDelta": 0.01,
            "maximumExcludedBackgroundDelta": 0.01,
            "maximumMarkerRange": 0.01,
            "minimumMarkerDelta": 0.10,
            "markerChannelMargin": 0.05,
        },
        "osVersion": {"major": 10, "minor": 0, "build": 19045},
        "rendererDevice": {
            "driverType": "hardware",
            "adapter": "Test GPU",
            "adapterLuid": {"low": 1, "high": 0},
            "vendorId": 1,
            "deviceId": 2,
            "driverVersion": 3,
            "featureLevel": 45312,
        },
        "observerFeatureLevel": 45312,
        "fixture": {
            "monitorBounds": {"left": 0, "top": 0, "right": 1920, "bottom": 1080},
            "captureScreenBounds": {
                "left": 100,
                "top": 200,
                "right": 740,
                "bottom": 520,
            },
            "captureRegion": _region_document((100, 200, WIDTH, HEIGHT)),
            "overlayRoi": _region_document(OVERLAY_ROI),
            "controlRoi": _region_document(CONTROL_ROI),
            "markerRoi": _region_document(MARKER_ROI),
            "markerReferenceRoi": _region_document(MARKER_REFERENCE_ROI),
            "overlayScreenBounds": {
                "left": 132,
                "top": 232,
                "right": 388,
                "bottom": 488,
            },
            "backgroundSrgb8": [30, 82, 146],
            "markerColorsSrgb8": {
                "includedBefore": MARKERS["included-before"][1],
                "excluded": MARKERS["excluded"][1],
                "includedAfter": MARKERS["included-after"][1],
            },
            "probeLinear": {"r": 0.82, "g": 0.16, "b": 0.52, "a": 1.0},
        },
        "wgcCapabilities": {
            "borderHidden": False,
            "cursorExcluded": True,
            "cursorCaptureEnabled": False,
            "cursorControlConfirmed": True,
        },
        "observations": {
            "includedBefore": observation(
                False, "included-before.rgba16f", 0, 1, 100
            ),
            "excluded": observation(True, "excluded.rgba16f", 2, 3, 200),
            "includedAfter": observation(
                False, "included-after.rgba16f", 4, 5, 300
            ),
        },
        "resourceLedger": ledger(),
    }
    refresh_metrics(document, directory)
    return document


class SelfExclusionVerifierTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temporary.name)
        cls.baseline_document = valid_capture(cls.directory)
        cls.baseline_artifacts = {
            name: (cls.directory / name).read_bytes()
            for name in (
                "included-before.rgba16f",
                "included-before.png",
                "excluded.rgba16f",
                "excluded.png",
                "included-after.rgba16f",
                "included-after.png",
            )
        }

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        # The exact-size FP16 fixture is expensive to rebuild.  Tests share the
        # immutable baseline and restore only artifacts they intentionally edit.
        self.document = copy.deepcopy(self.baseline_document)
        self.mutated_artifacts = set()

    def tearDown(self):
        for name in self.mutated_artifacts:
            (self.directory / name).write_bytes(self.baseline_artifacts[name])

    def write_artifact(self, name, payload):
        self.mutated_artifacts.add(name)
        path = self.directory / name
        path.write_bytes(payload)
        return path

    def validate(self):
        return VERIFY.validate_capture(self.document, self.directory)

    def write_document(self, name="self-exclusion.json"):
        path = self.directory / name
        path.write_text(json.dumps(self.document), encoding="utf-8")
        return path

    def test_valid_capture_is_accepted(self):
        result = self.validate()
        self.assertEqual(result.status, "accepted")
        self.assertEqual(result.included_before_different_pixels, 192 * 192)
        self.assertEqual(result.included_after_different_pixels, 192 * 192)
        self.assertEqual(result.included_stability_maximum_rgb_delta, 0.0)
        self.assertEqual(result.excluded_background_different_pixels, 0)
        self.assertEqual(result.minimum_marker_pair_different_pixels, 64 * 64)

    def test_unknown_nested_field_is_rejected(self):
        self.document["observations"]["excluded"]["affinity"]["extra"] = 1
        with self.assertRaisesRegex(VERIFY.ValidationError, "unknown fields"):
            self.validate()

    def test_missing_root_field_is_rejected(self):
        del self.document["wgcCapabilities"]
        with self.assertRaisesRegex(VERIFY.ValidationError, "missing fields"):
            self.validate()

    def test_monitor_target_is_required(self):
        self.document["contract"]["captureTarget"] = "HWND"
        with self.assertRaisesRegex(VERIFY.ValidationError, "captureTarget"):
            self.validate()

    def test_hardware_device_is_required(self):
        self.document["rendererDevice"]["driverType"] = "warp"
        with self.assertRaisesRegex(VERIFY.ValidationError, "hardware"):
            self.validate()

    def test_cursor_capture_must_be_disabled_and_confirmed(self):
        self.document["wgcCapabilities"]["cursorCaptureEnabled"] = True
        with self.assertRaisesRegex(VERIFY.ValidationError, "remained enabled"):
            self.validate()

    def test_dynamic_matrix_must_not_claim_product_restart_transaction(self):
        self.document["contract"][
            "validatesProductStopWdaStartTransaction"
        ] = True
        with self.assertRaisesRegex(VERIFY.ValidationError, "must not claim"):
            self.validate()

    def test_excluded_affinity_must_be_requested_and_observed(self):
        affinity = self.document["observations"]["excluded"]["affinity"]
        affinity["observed"] = 0
        with self.assertRaisesRegex(VERIFY.ValidationError, "requested WDA mode"):
            self.validate()

    def test_affinity_set_failure_is_rejected_even_with_matching_readback(self):
        affinity = self.document["observations"]["excluded"]["affinity"]
        affinity["setSucceeded"] = False
        affinity["setError"] = 5
        with self.assertRaisesRegex(VERIFY.ValidationError, "SetWindowDisplayAffinity"):
            self.validate()

    def test_affinity_query_error_must_be_zero(self):
        affinity = self.document["observations"]["includedAfter"]["affinity"]
        affinity["queryError"] = 87
        with self.assertRaisesRegex(VERIFY.ValidationError, "queryError"):
            self.validate()

    def test_layered_transparent_styles_must_be_restored(self):
        stage = self.document["observations"]["excluded"]
        stage["observedExtendedStyle"] &= ~0x00080000
        stage["layeredStyleRestored"] = False
        with self.assertRaisesRegex(VERIFY.ValidationError, "restore"):
            self.validate()

    def test_stale_stable_pair_frame_is_rejected(self):
        frame_document = self.document["observations"]["includedBefore"][
            "stablePair"
        ]["first"]
        frame_document["capturedAtNs"] = frame_document["markerNs"]
        with self.assertRaisesRegex(VERIFY.ValidationError, "not newer"):
            self.validate()

    def test_stable_pair_generation_must_advance(self):
        pair = self.document["observations"]["excluded"]["stablePair"]
        pair["second"]["generation"] = pair["first"]["generation"]
        with self.assertRaisesRegex(VERIFY.ValidationError, "did not advance"):
            self.validate()

    def test_global_generation_order_is_rejected(self):
        first = self.document["observations"]["includedAfter"]["stablePair"][
            "first"
        ]
        first["previousGeneration"] = 3
        first["generation"] = 4
        with self.assertRaisesRegex(VERIFY.ValidationError, "globally ordered"):
            self.validate()

    def test_fixed_capture_size_is_required(self):
        self.document["fixture"]["captureScreenBounds"]["right"] -= 1
        with self.assertRaisesRegex(VERIFY.ValidationError, "wrong size"):
            self.validate()

    def test_fixed_overlay_roi_is_required(self):
        self.document["fixture"]["overlayRoi"]["left"] = 63
        with self.assertRaisesRegex(VERIFY.ValidationError, "overlayRoi"):
            self.validate()

    def test_overlay_screen_mapping_is_required(self):
        self.document["fixture"]["overlayScreenBounds"]["left"] += 1
        with self.assertRaisesRegex(VERIFY.ValidationError, "unexpected|mapping"):
            self.validate()

    def test_truncated_raw_artifact_is_rejected(self):
        path = self.directory / "excluded.rgba16f"
        self.write_artifact(path.name, path.read_bytes()[:-8])
        with self.assertRaisesRegex(VERIFY.ValidationError, "byte count mismatch"):
            self.validate()

    def test_nonfinite_raw_artifact_is_rejected(self):
        path = self.directory / "included-before.rgba16f"
        payload = bytearray(path.read_bytes())
        payload[0:2] = struct.pack("<e", float("inf"))
        self.write_artifact(path.name, payload)
        with self.assertRaisesRegex(VERIFY.ValidationError, "non-finite"):
            self.validate()

    def test_artifact_path_escape_is_rejected(self):
        self.document["observations"]["excluded"]["artifact"]["raw"] = (
            "../excluded.rgba16f"
        )
        with self.assertRaisesRegex(VERIFY.ValidationError, "canonical|local file name"):
            self.validate()

    def test_png_dimensions_are_checked(self):
        self.write_artifact("excluded.png", _png_header(1, 1))
        with self.assertRaisesRegex(VERIFY.ValidationError, "dimensions"):
            self.validate()

    def test_forged_collector_metrics_are_rejected(self):
        comparison = self.document["metrics"]["includedBeforeVsExcluded"]
        comparison["differentPixels"] -= 1
        with self.assertRaisesRegex(VERIFY.ValidationError, "raw FP16"):
            self.validate()

    def test_forged_spatial_metrics_are_rejected(self):
        comparison = self.document["metrics"]["excludedOverlayVsControl"]
        comparison["differentPixels"] = 1
        with self.assertRaisesRegex(VERIFY.ValidationError, "raw FP16"):
            self.validate()

    def test_forged_marker_mean_is_rejected(self):
        marker = self.document["metrics"]["markers"]["includedBefore"]
        marker["meanLinear"]["r"] = 0.0
        with self.assertRaisesRegex(VERIFY.ValidationError, "meanLinear"):
            self.validate()

    def test_self_weakened_difference_threshold_is_rejected(self):
        self.document["contract"]["differenceThreshold"] = 2.0
        for comparison in self.document["metrics"].values():
            if isinstance(comparison, dict):
                comparison["threshold"] = 2.0
        with self.assertRaisesRegex(VERIFY.ValidationError, "differenceThreshold"):
            self.validate()

    def test_self_weakened_acceptance_limits_are_rejected(self):
        self.document["contract"]["minimumChangedFraction"] = 0.0
        self.document["contract"]["minimumOverlayDelta"] = 0.0
        self.document["contract"]["maximumControlDelta"] = 100.0
        with self.assertRaisesRegex(
            VERIFY.ValidationError, "minimumChangedFraction"
        ):
            self.validate()

    def test_self_weakened_stability_limit_is_rejected(self):
        self.document["contract"]["stableSampleTolerance"] = 1.0
        with self.assertRaisesRegex(VERIFY.ValidationError, "stableSampleTolerance"):
            self.validate()

    def test_missing_overlay_pixels_fail_after_metric_recalculation(self):
        excluded = (self.directory / "excluded.rgba16f").read_bytes()
        self.write_artifact("included-before.rgba16f", excluded)
        refresh_metrics(self.document, self.directory)
        with self.assertRaisesRegex(VERIFY.ValidationError, "materially cover"):
            self.validate()

    def test_excluded_overlay_must_return_to_controlled_background(self):
        path = self.directory / "excluded.rgba16f"
        payload = bytearray(path.read_bytes())
        alternate = struct.pack("<4e", 0.5, 0.5, 0.5, 1.0)
        for y in range(OVERLAY_ROI[1], OVERLAY_ROI[1] + OVERLAY_ROI[3]):
            start = (y * WIDTH + OVERLAY_ROI[0]) * 8
            end = start + OVERLAY_ROI[2] * 8
            payload[start:end] = alternate * OVERLAY_ROI[2]
        self.write_artifact(path.name, payload)
        refresh_metrics(self.document, self.directory)
        with self.assertRaisesRegex(VERIFY.ValidationError, "return to the background"):
            self.validate()

    def test_marker_channel_identity_is_recomputed_from_raw(self):
        path = self.directory / "included-before.rgba16f"
        payload = bytearray(path.read_bytes())
        gray = struct.pack("<4e", 0.7, 0.7, 0.7, 1.0)
        for y in range(MARKER_ROI[1], MARKER_ROI[1] + MARKER_ROI[3]):
            start = (y * WIDTH + MARKER_ROI[0]) * 8
            end = start + MARKER_ROI[2] * 8
            payload[start:end] = gray * MARKER_ROI[2]
        self.write_artifact(path.name, payload)
        refresh_metrics(self.document, self.directory)
        with self.assertRaisesRegex(VERIFY.ValidationError, "channel identity"):
            self.validate()

    def test_included_frames_must_be_stable_after_metric_recalculation(self):
        path = self.directory / "included-after.rgba16f"
        payload = bytearray(path.read_bytes())
        changed = struct.pack("<4e", 0.8, 0.8, 0.8, 1.0)
        offset = (OVERLAY_ROI[1] * WIDTH + OVERLAY_ROI[0]) * 8
        payload[offset : offset + 8] = changed
        self.write_artifact(path.name, payload)
        refresh_metrics(self.document, self.directory)
        with self.assertRaisesRegex(VERIFY.ValidationError, "not stable"):
            self.validate()

    def test_remote_control_must_be_stable_after_metric_recalculation(self):
        path = self.directory / "included-after.rgba16f"
        payload = bytearray(path.read_bytes())
        offset = (CONTROL_ROI[1] * WIDTH + CONTROL_ROI[0]) * 8
        payload[offset : offset + 8] = struct.pack("<4e", 0.8, 0.8, 0.8, 1.0)
        self.write_artifact(path.name, payload)
        refresh_metrics(self.document, self.directory)
        with self.assertRaisesRegex(VERIFY.ValidationError, "control ROI"):
            self.validate()

    def test_resource_ledger_must_balance_single_session(self):
        self.document["resourceLedger"]["liveSessions"] = 1
        with self.assertRaisesRegex(VERIFY.ValidationError, "liveSessions"):
            self.validate()

    def test_cli_rejects_duplicate_fields(self):
        path = self.directory / "duplicate.json"
        path.write_text('{"schemaVersion":1,"schemaVersion":1}', encoding="utf-8")
        result = subprocess.run(
            [sys.executable, "-B", str(SCRIPT_PATH), str(path)],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("duplicate JSON field", result.stderr)

    def test_cli_rejects_nonfinite_json_numbers(self):
        path = self.directory / "nan.json"
        path.write_text('{"schemaVersion":NaN}', encoding="utf-8")
        result = subprocess.run(
            [sys.executable, "-B", str(SCRIPT_PATH), str(path)],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("non-finite JSON number", result.stderr)

    def test_cli_writes_atomic_report(self):
        capture_path = self.write_document()
        report_path = self.directory / "verification.json"
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
            timeout=10,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("PASS: SPK-002-WDA-SELF-EXCLUSION", result.stdout)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(report["status"], "accepted")
        self.assertNotIn(b"\r\n", report_path.read_bytes())
        self.assertFalse((self.directory / "verification.json.tmp").exists())


if __name__ == "__main__":
    raise SystemExit(unittest.main())
