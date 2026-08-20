#!/usr/bin/env python3
"""Contract tests for the OBS Spout2 extended composite verifier."""

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
BACKGROUNDS = {
    "black": [0, 0, 0],
    "gray": [96, 96, 96],
    "white": [255, 255, 255],
    "color": [32, 80, 144],
}


def _fixture(root: Path) -> tuple[Path, dict[str, np.ndarray]]:
    width = 8
    height = 6
    bgra = np.zeros((height, width, 4), dtype=np.uint8)
    bgra[1, 1] = [24, 48, 96, 0]
    bgra[2, 2] = [24, 48, 96, 32]
    bgra[3, 3] = [80, 40, 20, 128]
    raw_path = root / "active-frame.bgra"
    raw_path.write_bytes(bgra.tobytes())
    rgb = bgra[:, :, [2, 1, 0]].astype(np.uint16)
    alpha = bgra[:, :, 3].astype(np.uint16)

    images: dict[str, np.ndarray] = {}
    cases: list[dict[str, object]] = []
    for name, background in BACKGROUNDS.items():
        background_array = np.asarray(background, dtype=np.uint16)
        contribution = (
            background_array[None, None, :] * (255 - alpha[:, :, None]) + 127
        ) // 255
        image = np.minimum(rgb + contribution, 255).astype(np.uint8)
        image_path = root / f"{name}.png"
        Image.fromarray(image, mode="RGB").save(image_path)
        images[name] = image
        cases.append(
            {
                "name": name,
                "backgroundRgb": background,
                "image": image_path.name,
            }
        )
    manifest = {
        "schemaVersion": 1,
        "contract": "bgra8-srgb-extended-premultiplied-fx-only-v2",
        "obsBlendMethod": "default",
        "obsBlendMode": "normal",
        "rawFrame": {
            "path": raw_path.name,
            "width": width,
            "height": height,
            "format": 87,
        },
        "cases": cases,
    }
    manifest_path = root / "manifest.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    return manifest_path, images


def _run(manifest: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(VERIFIER), str(manifest), "--tolerance=0"],
        check=False,
        capture_output=True,
        text=True,
        timeout=10,
        encoding="utf-8",
        errors="replace",
    )


class VerifyObsSpout2CompositeTests(unittest.TestCase):
    def test_accepts_exact_extended_premultiplied_composites(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, _ = _fixture(root)

            result = _run(manifest)

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads((root / "composite-verification.json").read_text())
            self.assertEqual(report["status"], "passed")
            self.assertGreater(report["rawFrame"]["rgbAboveAlphaPixels"], 0)
            self.assertGreater(report["rawFrame"]["zeroAlphaEmissionPixels"], 0)
            self.assertEqual(len(report["cases"]), 4)

    def test_rejects_rgb_canonicalized_to_alpha(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, images = _fixture(root)
            black = images["black"].copy()
            black[1, 1] = 0
            Image.fromarray(black, mode="RGB").save(root / "black.png")

            result = _run(manifest)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("differs from ONE/INV_SRC_ALPHA", result.stderr)
            self.assertFalse((root / "composite-verification.json").exists())

    def test_rejects_missing_required_background(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, _ = _fixture(root)
            document = json.loads(manifest.read_text())
            document["cases"] = document["cases"][:-1]
            manifest.write_text(json.dumps(document), encoding="utf-8")

            result = _run(manifest)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("black, gray, white, and color", result.stderr)

    def test_rejects_linear_srgb_composite(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, _ = _fixture(root)
            document = json.loads(manifest.read_text())
            raw = np.fromfile(root / "active-frame.bgra", dtype=np.uint8).reshape(
                (6, 8, 4)
            )
            source = raw[:, :, [2, 1, 0]]
            alpha = raw[:, :, 3]
            background = np.asarray(BACKGROUNDS["gray"], dtype=np.uint8)
            source_normalized = source.astype(np.float64) / 255.0
            background_normalized = background.astype(np.float64) / 255.0
            source_linear = np.where(
                source_normalized <= 0.04045,
                source_normalized / 12.92,
                np.power((source_normalized + 0.055) / 1.055, 2.4),
            )
            background_linear = np.where(
                background_normalized <= 0.04045,
                background_normalized / 12.92,
                np.power((background_normalized + 0.055) / 1.055, 2.4),
            )
            composite_linear = np.clip(
                source_linear
                + background_linear[None, None, :]
                * (1.0 - alpha.astype(np.float64)[:, :, None] / 255.0),
                0.0,
                1.0,
            )
            encoded = np.where(
                composite_linear <= 0.0031308,
                composite_linear * 12.92,
                1.055 * np.power(composite_linear, 1.0 / 2.4) - 0.055,
            )
            Image.fromarray(
                np.rint(encoded * 255.0).astype(np.uint8),
                mode="RGB",
            ).save(root / "gray.png")

            result = _run(manifest)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("differs from ONE/INV_SRC_ALPHA", result.stderr)


if __name__ == "__main__":
    unittest.main()
