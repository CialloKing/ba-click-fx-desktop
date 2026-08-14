#!/usr/bin/env python3
"""Contract tests for the Unity particle-state fixture validator."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest


SCRIPT_PATH = (
    Path(__file__).resolve().parents[1]
    / "tools"
    / "verify-unity-particle-fixture.py"
)
SPEC = importlib.util.spec_from_file_location("verify_unity_particle_fixture", SCRIPT_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"unable to load {SCRIPT_PATH}")
VERIFY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = VERIFY
SPEC.loader.exec_module(VERIFY)


def valid_fixture(capture_time_milliseconds: int = 50) -> dict:
    return {
        "schema": 2,
        "fixture": "UnityParticleStateV2",
        "unityVersion": "2021.3.45f1",
        "renderSize": {"width": 1950, "height": 1097},
        "captureTimeSeconds": capture_time_milliseconds / 1000.0,
        "captureTimeMilliseconds": capture_time_milliseconds,
        "seedBase": 20260716,
        "seedStride": 7919,
        "seedFormula": "seedBase + index * seedStride",
        "deterministic": {"runs": 2, "byteIdentical": True},
        "systems": [
            {
                "index": 0,
                "path": "FX_Touch/Ring",
                "name": "Ring",
                "seed": 20260716,
                "simulationSpace": "Local",
                "localToWorldMatrix": [
                    1.0,
                    0.0,
                    0.0,
                    0.0,
                    0.0,
                    1.0,
                    0.0,
                    0.0,
                    0.0,
                    0.0,
                    1.0,
                    0.0,
                    0.0,
                    0.0,
                    0.0,
                    1.0,
                ],
                "particleCount": 1,
                "particles": [
                    {
                        "index": 0,
                        "randomSeed": 7,
                        "atlasFrame": 0,
                        "position": {"x": 0.0, "y": 0.0, "z": 0.0},
                        "worldPosition": {"x": 0.0, "y": 0.0, "z": 0.0},
                        "projectedPixel": {"x": 975.0, "y": 548.5},
                        "velocity": {"x": 1.0, "y": 2.0, "z": 0.0},
                        "startLifetime": 1.0,
                        "remainingLifetime": 0.95,
                        "size": 0.5,
                        "rotation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
                        "color": {"r": 1.0, "g": 0.5, "b": 0.25, "a": 1.0},
                        "custom1": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 0.0},
                    }
                ],
            }
        ],
    }


class FixtureContractTests(unittest.TestCase):
    def test_valid_fixture(self):
        fixture = valid_fixture()
        self.assertIs(VERIFY.validate_fixture(fixture), fixture)

    def test_accepts_each_locked_capture_time(self):
        for age in VERIFY.EXPECTED_TIMES_MILLISECONDS:
            with self.subTest(age=age):
                fixture = valid_fixture(age)
                self.assertIs(VERIFY.validate_fixture(fixture), fixture)

    def test_rejects_unlisted_capture_time(self):
        fixture = valid_fixture(110)
        with self.assertRaisesRegex(VERIFY.ValidationError, "must be one of"):
            VERIFY.validate_fixture(fixture)

    def test_rejects_mismatched_seconds_and_milliseconds(self):
        fixture = valid_fixture(120)
        fixture["captureTimeSeconds"] = 0.05
        with self.assertRaisesRegex(VERIFY.ValidationError, "captureTimeSeconds"):
            VERIFY.validate_fixture(fixture)

    def test_rejects_boolean_as_integer(self):
        fixture = valid_fixture()
        fixture["captureTimeMilliseconds"] = True
        with self.assertRaisesRegex(VERIFY.ValidationError, "captureTimeMilliseconds"):
            VERIFY.validate_fixture(fixture)

    def test_rejects_wrong_seed_formula(self):
        fixture = valid_fixture()
        fixture["seedFormula"] = "seedBase + index * 7919"
        with self.assertRaisesRegex(VERIFY.ValidationError, "seedFormula"):
            VERIFY.validate_fixture(fixture)

    def test_rejects_non_contiguous_particle_order(self):
        fixture = valid_fixture()
        fixture["systems"][0]["particles"][0]["index"] = 1
        with self.assertRaisesRegex(VERIFY.ValidationError, "serialized order"):
            VERIFY.validate_fixture(fixture)

    def test_rejects_unknown_particle_field(self):
        fixture = valid_fixture()
        fixture["systems"][0]["particles"][0]["unexpected"] = 0
        with self.assertRaisesRegex(VERIFY.ValidationError, "fields differ"):
            VERIFY.validate_fixture(fixture)

    def test_rejects_non_triangle_atlas_frame(self):
        fixture = valid_fixture()
        fixture["systems"][0]["particles"][0]["atlasFrame"] = 1
        with self.assertRaisesRegex(VERIFY.ValidationError, "atlasFrame must be 0"):
            VERIFY.validate_fixture(fixture)

    def test_rejects_triangle_atlas_frame_outside_two_tiles(self):
        fixture = valid_fixture()
        fixture["systems"][0]["name"] = "Ring (3)"
        fixture["systems"][0]["particles"][0]["atlasFrame"] = 2
        with self.assertRaisesRegex(VERIFY.ValidationError, "atlasFrame must be 0 or 1"):
            VERIFY.validate_fixture(fixture)

    def test_rejects_system_seed_that_does_not_follow_stride(self):
        fixture = valid_fixture()
        fixture["systems"].append(
            {
                **fixture["systems"][0],
                "index": 1,
                "name": "Ring (1)",
                "path": "FX_Touch/Ring (1)",
                "seed": 20260716 + 1,
                "particleCount": 0,
                "particles": [],
            }
        )
        with self.assertRaisesRegex(VERIFY.ValidationError, "seed must equal"):
            VERIFY.validate_fixture(fixture)

    def test_repeat_file_must_match_bytes_exactly(self):
        fixture = valid_fixture()
        first = json.dumps(fixture, separators=(",", ":"), ensure_ascii=False).encode(
            "utf-8"
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "particle-state.json"
            path.write_bytes(first)
            path.with_name(path.name + ".repeat").write_bytes(
                json.dumps(fixture, indent=2, ensure_ascii=False).encode("utf-8")
            )
            with self.assertRaisesRegex(VERIFY.ValidationError, "repeat bytes"):
                VERIFY.validate_path(path)

    def test_repeat_file_with_identical_bytes_is_accepted(self):
        fixture = valid_fixture()
        data = json.dumps(fixture, separators=(",", ":"), ensure_ascii=False).encode(
            "utf-8"
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "particle-state.json"
            path.write_bytes(data)
            path.with_name(path.name + ".repeat").write_bytes(data)
            VERIFY.validate_path(path)

    def test_rejects_duplicate_json_fields(self):
        duplicate = (
            b'{"schema":2,"schema":2,"fixture":"UnityParticleStateV2"}'
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "duplicate.json"
            path.write_bytes(duplicate)
            with self.assertRaisesRegex(VERIFY.ValidationError, "duplicate JSON"):
                VERIFY.validate_path(path)

    def test_cli_exit_codes_distinguish_success_contract_and_arguments(self):
        data = json.dumps(
            valid_fixture(), separators=(",", ":"), ensure_ascii=False
        ).encode("utf-8")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "particle-state.json"
            path.write_bytes(data)
            success = subprocess.run(
                [sys.executable, "-B", str(SCRIPT_PATH), str(path)],
                capture_output=True,
                text=True,
                timeout=5,
                check=False,
            )
            self.assertEqual(0, success.returncode)

            path.write_bytes(data.replace(b"20260716", b"20260717", 1))
            failure = subprocess.run(
                [sys.executable, "-B", str(SCRIPT_PATH), str(path)],
                capture_output=True,
                text=True,
                timeout=5,
                check=False,
            )
            self.assertEqual(1, failure.returncode)
            self.assertIn("FAIL:", failure.stderr)

        argument_error = subprocess.run(
            [sys.executable, "-B", str(SCRIPT_PATH)],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        self.assertEqual(2, argument_error.returncode)


if __name__ == "__main__":
    raise SystemExit(unittest.main())
