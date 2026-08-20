#!/usr/bin/env python3
"""Contract tests for the OBS Spout2 recording brightness verifier."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image


if len(sys.argv) < 2:
    raise RuntimeError("expected verifier path")

VERIFIER = Path(sys.argv.pop(1))
WIDTH = 128
HEIGHT = 96
BACKGROUND = np.asarray([96, 96, 96], dtype=np.uint16)


def _fixture(root: Path) -> tuple[Path, np.ndarray, np.ndarray]:
    bgra = np.zeros((HEIGHT, WIDTH, 4), dtype=np.uint8)
    bgra[28:68, 42:86, :3] = [120, 105, 80]
    bgra[28:48, 42:86, 3] = 1
    bgra[48:68, 42:86, 3] = 48
    raw_path = root / "active-frame.bgra"
    raw_path.write_bytes(bgra.tobytes())

    source_rgb = bgra[:, :, [2, 1, 0]].astype(np.uint16)
    alpha = bgra[:, :, 3].astype(np.uint16)
    contribution = (
        BACKGROUND[None, None, :] * (255 - alpha[:, :, None]) + 127
    ) // 255
    reference = np.minimum(source_rgb + contribution, 255).astype(np.uint8)
    decoded = reference.copy()
    decoded[::7, ::7] = np.maximum(decoded[::7, ::7].astype(np.int16) - 1, 0)
    Image.fromarray(reference, mode="RGB").save(root / "reference.png")
    Image.fromarray(decoded, mode="RGB").save(root / "decoded.png")
    (root / "recording.mp4").write_bytes(b"recording-fixture")

    manifest = {
        "schemaVersion": 1,
        "contract": "bgra8-srgb-extended-premultiplied-fx-only-v3",
        "backgroundRgb": [96, 96, 96],
        "rawFrame": {
            "path": raw_path.name,
            "width": WIDTH,
            "height": HEIGHT,
            "format": 87,
        },
        "referenceImage": "reference.png",
        "decodedFrame": "decoded.png",
        "video": "recording.mp4",
    }
    manifest_path = root / "manifest.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    return manifest_path, reference, decoded


def _run(manifest: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(VERIFIER), str(manifest)],
        check=False,
        capture_output=True,
        text=True,
        timeout=10,
        encoding="utf-8",
        errors="replace",
    )


class VerifyObsSpout2RecordingTests(unittest.TestCase):
    def test_accepts_small_recording_quantization(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, _, _ = _fixture(root)

            result = _run(manifest)

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads((root / "recording-verification.json").read_text())
            self.assertEqual(report["status"], "passed")
            self.assertGreater(report["metrics"]["effectBrightnessEnergyRatio"], 0.9)

    def test_rejects_darker_effect_with_unchanged_background(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, reference, _ = _fixture(root)
            delta = reference.astype(np.int16) - 96
            decoded = np.clip(96 + np.rint(delta * 0.65), 0, 255).astype(np.uint8)
            Image.fromarray(decoded, mode="RGB").save(root / "decoded.png")

            result = _run(manifest)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("brightness energy differs", result.stderr)

    def test_rejects_dark_recording_background(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, _, decoded = _fixture(root)
            Image.fromarray(
                np.maximum(decoded.astype(np.int16) - 5, 0).astype(np.uint8),
                mode="RGB",
            ).save(root / "decoded.png")

            result = _run(manifest)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("recording background differs", result.stderr)

    def test_rejects_core_darkening_hidden_by_equal_total_energy(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, reference, _ = _fixture(root)
            decoded = reference.astype(np.float64)
            left = decoded[28:68, 42:64]
            right = decoded[28:68, 64:86]
            left[:] = 96.0 + (left - 96.0) * 0.70
            right[:] = 96.0 + (right - 96.0) * 1.30
            Image.fromarray(
                np.clip(np.rint(decoded), 0, 255).astype(np.uint8),
                mode="RGB",
            ).save(root / "decoded.png")

            result = _run(manifest)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("bright-pixel luma error", result.stderr)

    def test_rejects_wrong_decoded_dimensions(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, _, decoded = _fixture(root)
            Image.fromarray(decoded[:-1], mode="RGB").save(root / "decoded.png")

            result = _run(manifest)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("must match the raw Spout2 frame dimensions", result.stderr)


if __name__ == "__main__":
    unittest.main()
