#!/usr/bin/env python3
"""Tests for the Unity particle fixture C++ generator."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
GENERATOR_PATH = ROOT / "tools" / "generate-unity-particle-fixture.py"
SPEC = importlib.util.spec_from_file_location("generate_unity_particle_fixture", GENERATOR_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"unable to load generator: {GENERATOR_PATH}")
GENERATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GENERATOR)


def particle(index: int, x: float = 975.0, y: float = 548.5) -> dict:
    return {
        "index": index,
        "randomSeed": index + 7,
        "atlasFrame": 0,
        "position": {"x": 0.0, "y": 0.0, "z": 1.0},
        "worldPosition": {"x": 0.0, "y": 0.0, "z": 1.0},
        "projectedPixel": {"x": x, "y": y},
        "velocity": {"x": 0.0, "y": 0.0, "z": 0.0},
        "startLifetime": 0.6,
        "remainingLifetime": 0.575,
        "size": 0.1,
        "rotation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
        "color": {"r": 1.0, "g": 1.0, "b": 1.0, "a": 1.0},
        "custom1": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 0.0},
    }


def system(index: int, name: str, count: int, scale: float = 1.0) -> dict:
    return {
        "index": index,
        "path": "FX_Touch_ParticleFixture" + ("/" + name if index else ""),
        "name": name,
        "seed": 20260716 + index * 7919,
        "simulationSpace": "Local" if index == 0 else "World",
        "localToWorldMatrix": [
            scale, 0.0, 0.0, 0.0,
            0.0, scale, 0.0, 0.0,
            0.0, 0.0, scale, 1.0,
            0.0, 0.0, 0.0, 1.0,
        ],
        "particleCount": count,
        "particles": [particle(item, 975.0 + item, 548.5 + item) for item in range(count)],
    }


def fixture() -> dict:
    return {
        "schema": 2,
        "fixture": "UnityParticleStateV2",
        "unityVersion": "2021.3.45f1",
        "renderSize": {"width": 1950, "height": 1097},
        "captureTimeSeconds": 0.05,
        "captureTimeMilliseconds": 50,
        "seedBase": 20260716,
        "seedStride": 7919,
        "seedFormula": "seedBase + index * seedStride",
        "deterministic": {"runs": 2, "byteIdentical": True},
        "systems": [
            system(0, "FX_Touch_ParticleFixture", 0),
            system(1, "MeshTri", 2),
            system(2, "ring", 1),
            system(3, "Ring (3)", 4, 0.3078824),
            system(4, "Ring (4)", 0, 0.3078824),
        ],
    }


class FixtureGeneratorTests(unittest.TestCase):
    def test_generates_render_order_and_source_hash(self):
        value = fixture()
        encoded = b"fixture-bytes"
        generated = GENERATOR.generate_include(value, encoded).decode("ascii")

        expected_hash = hashlib.sha256(encoded).hexdigest().upper()
        self.assertIn(expected_hash, generated)
        self.assertEqual(generated.count("UnityParticleObservation{"), 7)
        self.assertLess(
            generated.index("UnityParticleSystemKind::CenterDisk"),
            generated.index("UnityParticleSystemKind::DissolveRing"),
        )

    def test_rejects_system_count_drift(self):
        value = fixture()
        value["systems"][1]["particleCount"] = 1
        value["systems"][1]["particles"] = value["systems"][1]["particles"][:1]
        with self.assertRaisesRegex(
            GENERATOR.VERIFY.ValidationError, "MeshTri must contain 2"
        ):
            GENERATOR.generate_include(value, b"fixture")

    def test_rejects_non_planar_rotation(self):
        value = fixture()
        value["systems"][3]["particles"][0]["rotation"]["x"] = 0.5
        with self.assertRaisesRegex(
            GENERATOR.VERIFY.ValidationError, "non-planar"
        ):
            GENERATOR.generate_include(value, b"fixture")

    def test_check_rejects_stale_generated_file(self):
        value = fixture()
        with tempfile.TemporaryDirectory() as directory:
            fixture_path = Path(directory) / "fixture.json"
            output = Path(directory) / "fixture.inc"
            fixture_path.write_text(
                json.dumps(value, separators=(",", ":")) + "\n",
                encoding="utf-8",
            )
            output.write_text("stale", encoding="ascii")
            with self.assertRaisesRegex(
                GENERATOR.VERIFY.ValidationError, "stale"
            ):
                GENERATOR.run(fixture_path, output, True)


if __name__ == "__main__":
    unittest.main()
