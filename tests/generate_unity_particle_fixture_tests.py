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


def fixture(age: int = 50) -> dict:
    ring_count = 1 if age <= 120 else 0
    return {
        "schema": 2,
        "fixture": "UnityParticleStateV2",
        "unityVersion": "2021.3.45f1",
        "renderSize": {"width": 1950, "height": 1097},
        "captureTimeSeconds": age / 1000.0,
        "captureTimeMilliseconds": age,
        "seedBase": 20260716,
        "seedStride": 7919,
        "seedFormula": "seedBase + index * seedStride",
        "deterministic": {"runs": 2, "byteIdentical": True},
        "systems": [
            system(0, "FX_Touch_ParticleFixture", 0),
            system(1, "MeshTri", 2),
            system(2, "ring", ring_count),
            system(3, "Ring (3)", 4, 0.3078824),
            system(4, "Ring (4)", 0, 0.3078824),
        ],
    }


def fixture_inputs() -> list[tuple[dict, bytes]]:
    return [
        (fixture(age), f"fixture-{age}".encode("ascii"))
        for age in GENERATOR.VERIFY.EXPECTED_TIMES_MILLISECONDS
    ]


class FixtureGeneratorTests(unittest.TestCase):
    def test_generates_render_order_and_source_hash(self):
        inputs = fixture_inputs()
        generated = GENERATOR.generate_include(inputs).decode("ascii")

        for _, encoded in inputs:
            expected_hash = hashlib.sha256(encoded).hexdigest().upper()
            self.assertIn(expected_hash, generated)
        self.assertEqual(generated.count("UnityParticleFixtureDescriptor{"), 5)
        self.assertEqual(generated.count("UnityParticleObservation{"), 33)
        self.assertIn("{{0U, 7U, 14U, 21U, 27U, 33U}}", generated)
        self.assertLess(
            generated.index("UnityParticleSystemKind::CenterDisk"),
            generated.index("UnityParticleSystemKind::DissolveRing"),
        )

    def test_rejects_missing_fixture_age(self):
        with self.assertRaisesRegex(
            GENERATOR.VERIFY.ValidationError, "fixture ages"
        ):
            GENERATOR.generate_include(fixture_inputs()[:-1])

    def test_rejects_duplicate_fixture_age(self):
        inputs = fixture_inputs()
        inputs.append((fixture(50), b"duplicate"))
        with self.assertRaisesRegex(
            GENERATOR.VERIFY.ValidationError, "duplicate fixture age"
        ):
            GENERATOR.generate_include(inputs)

    def test_rejects_particles_on_fixture_root(self):
        inputs = fixture_inputs()
        value = inputs[0][0]
        value["systems"][0]["particleCount"] = 1
        value["systems"][0]["particles"] = [particle(0)]
        with self.assertRaisesRegex(
            GENERATOR.VERIFY.ValidationError, "root must not contain particles"
        ):
            GENERATOR.generate_include(inputs)

    def test_rejects_non_planar_rotation(self):
        inputs = fixture_inputs()
        value = inputs[0][0]
        value["systems"][3]["particles"][0]["rotation"]["x"] = 0.5
        with self.assertRaisesRegex(
            GENERATOR.VERIFY.ValidationError, "non-planar"
        ):
            GENERATOR.generate_include(inputs)

    def test_check_rejects_stale_generated_file(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture_paths = []
            for age in GENERATOR.VERIFY.EXPECTED_TIMES_MILLISECONDS:
                fixture_path = Path(directory) / f"fixture-{age}.json"
                fixture_path.write_text(
                    json.dumps(fixture(age), separators=(",", ":")) + "\n",
                    encoding="utf-8",
                )
                fixture_paths.append(fixture_path)
            output = Path(directory) / "fixture.inc"
            output.write_text("stale", encoding="ascii")
            with self.assertRaisesRegex(
                GENERATOR.VERIFY.ValidationError, "stale"
            ):
                GENERATOR.run(fixture_paths, output, True)


if __name__ == "__main__":
    unittest.main()
