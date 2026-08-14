#!/usr/bin/env python3
"""Contract tests for the SPK-002 WGC lifecycle verifier."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT_PATH = (
    Path(__file__).resolve().parents[1]
    / "tools"
    / "verify-wgc-lifecycle-spike.py"
)
SPEC = importlib.util.spec_from_file_location(
    "verify_wgc_lifecycle_spike", SCRIPT_PATH
)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"unable to load {SCRIPT_PATH}")
VERIFY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = VERIFY
SPEC.loader.exec_module(VERIFY)


def resource_ledger(frame_count, recreated_count):
    return {
        "framesAcquired": frame_count,
        "framesClosed": frame_count,
        "framePoolsCreated": 1,
        "framePoolsClosed": 1,
        "framePoolsRecreated": recreated_count,
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


def capabilities():
    return {"borderHidden": False, "cursorExcluded": True}


def valid_capture():
    initial_size = {"width": 320, "height": 240}
    resized_size = {"width": 480, "height": 300}
    restart_size = {"width": 360, "height": 220}
    return {
        "schemaVersion": 1,
        "spikeId": "SPK-002-LIFECYCLE",
        "applicationVersion": "0.1.0-test",
        "revision": "test",
        "capturedAtUtc": "2026-08-14T00:00:00.000Z",
        "timeoutMs": 12000,
        "contract": {
            "scope": "controlled-window-lifecycle-only",
            "captureTarget": "HWND",
            "ownerThread": "single",
            "callbacks": "notification-only",
            "surfaceFormat": "DXGI_FORMAT_R16G16B16A16_FLOAT",
            "systemBorderAllowed": True,
            "cursorExcluded": True,
        },
        "device": {
            "driverType": "hardware",
            "adapter": "Test GPU",
            "adapterLuid": {"low": 1, "high": 0},
            "vendorId": 1,
            "deviceId": 2,
            "featureLevel": 45312,
        },
        "scenarios": {
            "resizeClose": {
                "events": [
                    {"sequence": 0, "kind": "target-created", "size": initial_size},
                    {
                        "sequence": 1,
                        "kind": "sensor-started",
                        "size": initial_size,
                        "epoch": 1,
                    },
                    {
                        "sequence": 2,
                        "kind": "frame-updated",
                        "size": initial_size,
                        "generation": 1,
                        "epoch": 1,
                    },
                    {"sequence": 3, "kind": "resize-requested", "size": resized_size},
                    {
                        "sequence": 4,
                        "kind": "reconfigure-required",
                        "size": resized_size,
                        "epoch": 1,
                    },
                    {
                        "sequence": 5,
                        "kind": "frame-pool-recreated",
                        "size": resized_size,
                        "epoch": 2,
                    },
                    {
                        "sequence": 6,
                        "kind": "frame-updated",
                        "size": resized_size,
                        "generation": 3,
                        "epoch": 2,
                    },
                    {"sequence": 7, "kind": "target-closed"},
                    {"sequence": 8, "kind": "sensor-stopped", "epoch": 2},
                    {"sequence": 9, "kind": "sensor-destroyed"},
                ],
                "initialFrame": {
                    "generation": 1,
                    "epoch": 1,
                    "size": initial_size,
                },
                "requestedResize": resized_size,
                "reconfigurations": [
                    {
                        "size": resized_size,
                        "epochBefore": 1,
                        "epochAfter": 2,
                    }
                ],
                "resizedFrame": {
                    "generation": 3,
                    "epoch": 2,
                    "size": resized_size,
                },
                "capabilities": capabilities(),
                "ledger": resource_ledger(4, 1),
            },
            "restartStop": {
                "events": [
                    {"sequence": 0, "kind": "target-created", "size": restart_size},
                    {
                        "sequence": 1,
                        "kind": "sensor-started",
                        "size": restart_size,
                        "epoch": 1,
                    },
                    {
                        "sequence": 2,
                        "kind": "frame-updated",
                        "size": restart_size,
                        "generation": 1,
                        "epoch": 1,
                    },
                    {"sequence": 3, "kind": "sensor-stop-requested", "epoch": 1},
                    {"sequence": 4, "kind": "sensor-stopped", "epoch": 1},
                    {"sequence": 5, "kind": "sensor-stop-repeated", "epoch": 1},
                    {"sequence": 6, "kind": "sensor-destroyed"},
                    {"sequence": 7, "kind": "target-closed"},
                ],
                "initialFrame": {
                    "generation": 1,
                    "epoch": 1,
                    "size": restart_size,
                },
                "capabilities": capabilities(),
                "ledger": resource_ledger(1, 0),
            },
        },
    }


def resequence(events):
    for sequence, event in enumerate(events):
        event["sequence"] = sequence


class WgcLifecycleSpikeContractTests(unittest.TestCase):
    def test_valid_capture_is_accepted(self):
        result = VERIFY.validate_capture(valid_capture())
        self.assertEqual("accepted", result.status)
        self.assertEqual(10, result.resize_close_events)
        self.assertEqual(8, result.restart_stop_events)
        self.assertEqual(1, result.resize_close_reconfigurations)
        self.assertEqual(0, result.restart_stop_reconfigurations)

    def test_startup_reconfigure_pairs_are_allowed_before_first_frame(self):
        capture = valid_capture()
        resize = capture["scenarios"]["resizeClose"]
        initial_size = resize["initialFrame"]["size"]
        resize["events"][2:2] = [
            {
                "kind": "reconfigure-required",
                "size": initial_size,
                "epoch": 1,
            },
            {
                "kind": "frame-pool-recreated",
                "size": initial_size,
                "epoch": 2,
            },
        ]
        for event in resize["events"][4:]:
            if "epoch" in event:
                event["epoch"] += 1
        resize["initialFrame"]["epoch"] = 2
        resize["resizedFrame"]["epoch"] = 3
        resize["reconfigurations"].insert(
            0,
            {"size": initial_size, "epochBefore": 1, "epochAfter": 2},
        )
        resize["reconfigurations"][1].update(
            {"epochBefore": 2, "epochAfter": 3}
        )
        resize["ledger"]["framePoolsRecreated"] = 2
        resequence(resize["events"])

        restart = capture["scenarios"]["restartStop"]
        restart_size = restart["initialFrame"]["size"]
        restart["events"][2:2] = [
            {
                "kind": "reconfigure-required",
                "size": restart_size,
                "epoch": 1,
            },
            {
                "kind": "frame-pool-recreated",
                "size": restart_size,
                "epoch": 2,
            },
        ]
        for event in restart["events"][4:]:
            if "epoch" in event:
                event["epoch"] = 2
        restart["initialFrame"]["epoch"] = 2
        restart["ledger"]["framePoolsRecreated"] = 1
        resequence(restart["events"])

        result = VERIFY.validate_capture(capture)
        self.assertEqual(2, result.resize_close_reconfigurations)
        self.assertEqual(1, result.restart_stop_reconfigurations)

    def test_event_sequence_numbers_must_be_contiguous(self):
        capture = valid_capture()
        capture["scenarios"]["resizeClose"]["events"][4]["sequence"] = 9
        with self.assertRaisesRegex(VERIFY.ValidationError, "sequence must be 4"):
            VERIFY.validate_capture(capture)

    def test_events_cannot_be_out_of_order(self):
        capture = valid_capture()
        events = capture["scenarios"]["resizeClose"]["events"]
        events[7], events[8] = events[8], events[7]
        resequence(events)
        with self.assertRaisesRegex(VERIFY.ValidationError, "target-closed"):
            VERIFY.validate_capture(capture)

    def test_resize_requires_recreate_pair(self):
        capture = valid_capture()
        events = capture["scenarios"]["resizeClose"]["events"]
        del events[5]
        resequence(events)
        with self.assertRaisesRegex(VERIFY.ValidationError, "frame-pool-recreated"):
            VERIFY.validate_capture(capture)

    def test_resize_epoch_must_advance(self):
        capture = valid_capture()
        capture["scenarios"]["resizeClose"]["events"][5]["epoch"] = 1
        with self.assertRaisesRegex(VERIFY.ValidationError, "epoch did not advance"):
            VERIFY.validate_capture(capture)

    def test_resize_generation_must_advance(self):
        capture = valid_capture()
        resize = capture["scenarios"]["resizeClose"]
        resize["events"][6]["generation"] = 1
        resize["resizedFrame"]["generation"] = 1
        with self.assertRaisesRegex(VERIFY.ValidationError, "generation did not advance"):
            VERIFY.validate_capture(capture)

    def test_resize_final_size_must_equal_request(self):
        capture = valid_capture()
        capture["scenarios"]["resizeClose"]["events"][6]["size"] = {
            "width": 470,
            "height": 290,
        }
        with self.assertRaisesRegex(VERIFY.ValidationError, "final frame size"):
            VERIFY.validate_capture(capture)

    def test_live_resource_count_must_be_zero(self):
        capture = valid_capture()
        capture["scenarios"]["restartStop"]["ledger"]["liveSessions"] = 1
        with self.assertRaisesRegex(VERIFY.ValidationError, "liveSessions must be zero"):
            VERIFY.validate_capture(capture)

    def test_all_released_flag_must_be_true(self):
        capture = valid_capture()
        capture["scenarios"]["restartStop"]["ledger"]["allReleased"] = False
        with self.assertRaisesRegex(VERIFY.ValidationError, "allReleased must be true"):
            VERIFY.validate_capture(capture)

    def test_failures_must_be_zero(self):
        capture = valid_capture()
        capture["scenarios"]["resizeClose"]["ledger"]["failures"] = 1
        with self.assertRaisesRegex(VERIFY.ValidationError, "failures must be zero"):
            VERIFY.validate_capture(capture)

    def test_acquire_release_counts_must_balance(self):
        capture = valid_capture()
        capture["scenarios"]["resizeClose"]["ledger"]["framesClosed"] = 3
        with self.assertRaisesRegex(VERIFY.ValidationError, "count mismatch"):
            VERIFY.validate_capture(capture)

    def test_registration_counts_must_balance(self):
        capture = valid_capture()
        ledger = capture["scenarios"]["restartStop"]["ledger"]
        ledger["itemClosedUnregistrations"] = 0
        with self.assertRaisesRegex(VERIFY.ValidationError, "count mismatch"):
            VERIFY.validate_capture(capture)

    def test_each_scenario_must_have_one_session_resource_set(self):
        capture = valid_capture()
        capture["scenarios"]["resizeClose"]["ledger"]["framePoolsCreated"] = 2
        capture["scenarios"]["resizeClose"]["ledger"]["framePoolsClosed"] = 2
        with self.assertRaisesRegex(VERIFY.ValidationError, "framePoolsCreated"):
            VERIFY.validate_capture(capture)

    def test_device_must_be_hardware(self):
        capture = valid_capture()
        capture["device"]["driverType"] = "warp"
        with self.assertRaisesRegex(VERIFY.ValidationError, "must be hardware"):
            VERIFY.validate_capture(capture)

    def test_contract_values_are_exact(self):
        replacements = {
            "scope": "desktop",
            "captureTarget": "monitor",
            "ownerThread": "multiple",
            "callbacks": "rendering",
            "surfaceFormat": "DXGI_FORMAT_B8G8R8A8_UNORM",
            "systemBorderAllowed": False,
            "cursorExcluded": False,
        }
        for field, replacement in replacements.items():
            with self.subTest(field=field):
                capture = valid_capture()
                capture["contract"][field] = replacement
                with self.assertRaises(VERIFY.ValidationError):
                    VERIFY.validate_capture(capture)

    def test_both_scenarios_must_exclude_cursor(self):
        for scenario_name in ("resizeClose", "restartStop"):
            with self.subTest(scenario=scenario_name):
                capture = valid_capture()
                capture["scenarios"][scenario_name]["capabilities"][
                    "cursorExcluded"
                ] = False
                with self.assertRaisesRegex(
                    VERIFY.ValidationError, "cursorExcluded must be true"
                ):
                    VERIFY.validate_capture(capture)

    def test_duplicate_json_fields_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "lifecycle.json"
            path.write_text(
                '{"schemaVersion":1,"schemaVersion":1}', encoding="utf-8"
            )
            with self.assertRaisesRegex(VERIFY.ValidationError, "duplicate JSON"):
                VERIFY.validate_path(path)

    def test_invalid_cli_returns_nonzero(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "lifecycle.json"
            capture = valid_capture()
            capture["device"]["driverType"] = "warp"
            path.write_text(json.dumps(capture), encoding="utf-8")
            result = subprocess.run(
                [sys.executable, "-B", str(SCRIPT_PATH), str(path)],
                capture_output=True,
                text=True,
                timeout=5,
                check=False,
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("FAIL:", result.stderr)

    def test_cli_writes_atomic_structured_report(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture_path = root / "lifecycle.json"
            report_path = root / "verification.json"
            capture_path.write_text(json.dumps(valid_capture()), encoding="utf-8")
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
                timeout=5,
                check=False,
            )
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertIn("PASS: SPK-002-LIFECYCLE", result.stdout)
            self.assertFalse(report_path.with_name("verification.json.tmp").exists())
            self.assertNotIn(b"\r\n", report_path.read_bytes())
            report = json.loads(report_path.read_text(encoding="utf-8"))
            self.assertEqual("accepted", report["status"])
            self.assertEqual(1, report["resize_close_reconfigurations"])
            self.assertEqual(5, report["total_frames_acquired"])


if __name__ == "__main__":
    raise SystemExit(unittest.main())
