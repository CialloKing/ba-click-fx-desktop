#!/usr/bin/env python3
"""Contract tests for the SPK-002 WGC cursor verifier."""

from __future__ import annotations

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
    / "verify-wgc-cursor-spike.py"
)
SPEC = importlib.util.spec_from_file_location("verify_wgc_cursor_spike", SCRIPT_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"unable to load {SCRIPT_PATH}")
VERIFY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = VERIFY
SPEC.loader.exec_module(VERIFY)

WIDTH = 320
HEIGHT = 240
RAW_BYTES = WIDTH * HEIGHT * 8
ROI = (112, 72, 96, 96)
CONTROL_ROI = (16, 16, 32, 32)
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def ledger(frame_count=2):
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
    background = struct.pack("<4e", 0.1, 0.2, 0.3, 1.0)
    cursor = struct.pack("<4e", 2.0, 2.0, 2.0, 1.0)
    excluded = background * (WIDTH * HEIGHT)
    included = bytearray(excluded)
    for coordinate in range(4, 28):
        for thickness in range(14, 18):
            for x, y in ((thickness, coordinate), (coordinate, thickness)):
                image_x = WIDTH // 2 - 16 + x
                image_y = HEIGHT // 2 - 16 + y
                offset = (image_y * WIDTH + image_x) * 8
                included[offset : offset + 8] = cursor
    for name, payload in (
        ("included-before.rgba16f", included),
        ("excluded.rgba16f", excluded),
        ("included-after.rgba16f", included),
    ):
        (directory / name).write_bytes(payload)
        (directory / name.replace(".rgba16f", ".png")).write_bytes(PNG_SIGNATURE)


def mode(excluded, raw_name, marker, captured):
    return {
        "requestedCursorExcluded": excluded,
        "capabilities": {
            "borderHidden": False,
            "cursorExcluded": excluded,
            "cursorCaptureEnabled": not excluded,
            "cursorControlConfirmed": True,
        },
        "previousGeneration": 1,
        "generation": 2,
        "markerNanoseconds": marker,
        "capturedAtNanoseconds": captured,
        "size": {"width": WIDTH, "height": HEIGHT},
        "artifact": {
            "raw": raw_name,
            "png": raw_name.replace(".rgba16f", ".png"),
            "width": WIDTH,
            "height": HEIGHT,
            "rawBytes": RAW_BYTES,
        },
        "ledger": ledger(),
    }


def _pair_document(metrics):
    bounds = metrics.difference_bounds
    return {
        "roi": {"left": ROI[0], "top": ROI[1], "width": ROI[2], "height": ROI[3]},
        "threshold": 0.02,
        "referenceRgbRange": metrics.reference_rgb_range,
        "maximumRgbDelta": metrics.maximum_rgb_delta,
        "differentPixels": metrics.different_pixels,
        "edgeDifferentPixels": metrics.edge_different_pixels,
        "differenceBounds": None
        if bounds is None
        else {
            "left": bounds[0],
            "top": bounds[1],
            "right": bounds[2],
            "bottom": bounds[3],
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
    before = VERIFY._pair_metrics(images["includedBefore"], images["excluded"], ROI, 0.02)
    after = VERIFY._pair_metrics(images["includedAfter"], images["excluded"], ROI, 0.02)
    stability = VERIFY._pair_metrics(
        images["includedBefore"], images["includedAfter"], ROI, 0.02
    )
    document["comparisons"] = {
        "includedBeforeVsExcluded": _pair_document(before),
        "includedAfterVsExcluded": _pair_document(after),
        "includedBeforeVsAfter": _pair_document(stability),
        "controlRoi": {
            "left": CONTROL_ROI[0],
            "top": CONTROL_ROI[1],
            "width": CONTROL_ROI[2],
            "height": CONTROL_ROI[3],
        },
        "controlMaximumRgbDelta": VERIFY._region_maximum_delta(
            (images["includedBefore"], images["excluded"], images["includedAfter"]),
            CONTROL_ROI,
        ),
    }


def valid_capture(directory):
    _write_raw_frames(directory)
    document = {
        "schemaVersion": 1,
        "spikeId": "SPK-002-CURSOR",
        "applicationVersion": "0.1.0-test",
        "revision": "test",
        "capturedAtUtc": "2026-08-14T00:00:00.000Z",
        "timeoutMs": 12000,
        "contract": {
            "scope": "controlled-window-cursor-pixels-only",
            "captureTarget": "HWND",
            "surfaceFormat": "DXGI_FORMAT_R16G16B16A16_FLOAT",
            "systemBorderAllowed": True,
            "cursorShape": "custom-monochrome-cross",
            "cursorExtent": 32,
            "cursorHotspot": 16,
            "cursorOpaquePixels": 176,
            "differenceThreshold": 0.02,
            "minimumDifferentPixels": 44,
            "minimumCursorDelta": 0.25,
            "maximumBackgroundRange": 0.01,
            "maximumControlDelta": 0.01,
            "maximumStabilityDifferentPixels": 4,
        },
        "os": {"available": True, "major": 10, "minor": 0, "build": 19045},
        "device": {
            "driverType": "hardware",
            "adapter": "Test GPU",
            "adapterLuid": {"low": 1, "high": 0},
            "vendorId": 1,
            "deviceId": 2,
            "featureLevel": 45312,
        },
        "fixture": {
            "size": {"width": WIDTH, "height": HEIGHT},
            "screenOrigin": {"x": 100, "y": 200},
            "cursorScreenPoint": {"x": 260, "y": 320},
            "cursorClientPoint": {"x": 160, "y": 120},
        },
        "observations": {
            "includedBefore": mode(
                False, "included-before.rgba16f", 100, 200
            ),
            "excluded": mode(True, "excluded.rgba16f", 300, 400),
            "includedAfter": mode(False, "included-after.rgba16f", 500, 600),
        },
    }
    refresh_metrics(document, directory)
    return document


class CursorVerifierTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.document = valid_capture(self.directory)

    def tearDown(self):
        self.temporary.cleanup()

    def validate(self):
        return VERIFY.validate_capture(self.document, self.directory)

    def write_document(self):
        path = self.directory / "cursor.json"
        path.write_text(json.dumps(self.document), encoding="utf-8")
        return path

    def test_valid_capture_is_accepted(self):
        result = self.validate()
        self.assertEqual(result.status, "accepted")
        self.assertEqual(result.included_before_different_pixels, 176)
        self.assertEqual(result.included_after_different_pixels, 176)
        self.assertEqual(result.stability_different_pixels, 0)

    def test_unknown_root_field_is_rejected(self):
        self.document["unexpected"] = True
        with self.assertRaisesRegex(VERIFY.ValidationError, "unknown fields"):
            self.validate()

    def test_unconfirmed_cursor_control_is_rejected(self):
        self.document["observations"]["includedBefore"]["capabilities"][
            "cursorControlConfirmed"
        ] = False
        with self.assertRaisesRegex(VERIFY.ValidationError, "not confirmed"):
            self.validate()

    def test_inverted_cursor_readback_is_rejected(self):
        self.document["observations"]["excluded"]["capabilities"][
            "cursorCaptureEnabled"
        ] = True
        with self.assertRaisesRegex(VERIFY.ValidationError, "inconsistent"):
            self.validate()

    def test_stale_qpc_frame_is_rejected(self):
        mode_document = self.document["observations"]["includedBefore"]
        mode_document["capturedAtNanoseconds"] = mode_document["markerNanoseconds"]
        with self.assertRaisesRegex(VERIFY.ValidationError, "not newer"):
            self.validate()

    def test_nonadvancing_generation_is_rejected(self):
        self.document["observations"]["excluded"]["generation"] = 1
        with self.assertRaisesRegex(VERIFY.ValidationError, "did not advance"):
            self.validate()

    def test_unbalanced_resource_ledger_is_rejected(self):
        self.document["observations"]["includedAfter"]["ledger"][
            "liveSessions"
        ] = 1
        with self.assertRaisesRegex(VERIFY.ValidationError, "liveSessions"):
            self.validate()

    def test_software_device_is_rejected(self):
        self.document["device"]["driverType"] = "warp"
        with self.assertRaisesRegex(VERIFY.ValidationError, "hardware"):
            self.validate()

    def test_truncated_raw_artifact_is_rejected(self):
        path = self.directory / "excluded.rgba16f"
        path.write_bytes(path.read_bytes()[:-8])
        with self.assertRaisesRegex(VERIFY.ValidationError, "byte count mismatch"):
            self.validate()

    def test_nonfinite_raw_artifact_is_rejected(self):
        path = self.directory / "excluded.rgba16f"
        payload = bytearray(path.read_bytes())
        payload[0:2] = struct.pack("<e", float("inf"))
        path.write_bytes(payload)
        with self.assertRaisesRegex(VERIFY.ValidationError, "non-finite"):
            self.validate()

    def test_artifact_path_escape_is_rejected(self):
        self.document["observations"]["excluded"]["artifact"]["raw"] = (
            "../excluded.rgba16f"
        )
        with self.assertRaisesRegex(VERIFY.ValidationError, "canonical|local file name"):
            self.validate()

    def test_invalid_png_preview_is_rejected(self):
        (self.directory / "excluded.png").write_bytes(b"not-png")
        with self.assertRaisesRegex(VERIFY.ValidationError, "not a PNG"):
            self.validate()

    def test_forged_pixel_count_is_rejected(self):
        self.document["comparisons"]["includedBeforeVsExcluded"][
            "differentPixels"
        ] = 999
        with self.assertRaisesRegex(VERIFY.ValidationError, "raw FP16"):
            self.validate()

    def test_self_weakened_threshold_is_rejected(self):
        self.document["contract"]["differenceThreshold"] = 2.0
        for comparison in (
            "includedBeforeVsExcluded",
            "includedAfterVsExcluded",
            "includedBeforeVsAfter",
        ):
            self.document["comparisons"][comparison]["threshold"] = 2.0
        with self.assertRaisesRegex(VERIFY.ValidationError, "differenceThreshold"):
            self.validate()

    def test_missing_cursor_pixels_are_rejected_after_recalculation(self):
        excluded = (self.directory / "excluded.rgba16f").read_bytes()
        (self.directory / "included-before.rgba16f").write_bytes(excluded)
        refresh_metrics(self.document, self.directory)
        with self.assertRaisesRegex(VERIFY.ValidationError, "too few cursor pixels"):
            self.validate()

    def test_control_region_change_is_rejected_after_recalculation(self):
        path = self.directory / "included-after.rgba16f"
        payload = bytearray(path.read_bytes())
        offset = (16 * WIDTH + 16) * 8
        payload[offset : offset + 8] = struct.pack("<4e", 1.0, 1.0, 1.0, 1.0)
        path.write_bytes(payload)
        refresh_metrics(self.document, self.directory)
        with self.assertRaisesRegex(VERIFY.ValidationError, "control ROI"):
            self.validate()

    def test_unstable_repeated_cursor_is_rejected_after_recalculation(self):
        path = self.directory / "included-after.rgba16f"
        payload = bytearray(path.read_bytes())
        background = struct.pack("<4e", 0.1, 0.2, 0.3, 1.0)
        changed = 0
        for coordinate in range(4, 28):
            if changed >= 5:
                break
            x = WIDTH // 2 - 16 + 14
            y = HEIGHT // 2 - 16 + coordinate
            offset = (y * WIDTH + x) * 8
            payload[offset : offset + 8] = background
            changed += 1
        path.write_bytes(payload)
        refresh_metrics(self.document, self.directory)
        with self.assertRaisesRegex(VERIFY.ValidationError, "not stable"):
            self.validate()

    def test_temporal_mode_order_is_rejected(self):
        self.document["observations"]["excluded"]["capturedAtNanoseconds"] = 150
        self.document["observations"]["excluded"]["markerNanoseconds"] = 125
        with self.assertRaisesRegex(VERIFY.ValidationError, "temporally ordered"):
            self.validate()

    def test_cli_rejects_duplicate_fields(self):
        path = self.directory / "duplicate.json"
        path.write_text('{"schemaVersion": 1, "schemaVersion": 1}', encoding="utf-8")
        result = subprocess.run(
            [sys.executable, "-B", str(SCRIPT_PATH), str(path)],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("duplicate JSON field", result.stderr)

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
        self.assertIn("PASS: SPK-002-CURSOR", result.stdout)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(report["status"], "accepted")
        self.assertFalse((self.directory / "verification.json.tmp").exists())


if __name__ == "__main__":
    unittest.main()
