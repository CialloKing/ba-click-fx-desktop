#!/usr/bin/env python3
"""Verify an OBS recording preserves a fixed Spout2 composite's brightness."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

import numpy as np
from PIL import Image


CONTRACT = "bgra8-srgb-extended-premultiplied-fx-only-v2"
EXPECTED_BACKGROUND = [96, 96, 96]
MINIMUM_ENERGY_RATIO = 0.90
MAXIMUM_ENERGY_RATIO = 1.10
MINIMUM_BRIGHT_ENERGY_RATIO = 0.90
MAXIMUM_BRIGHT_ENERGY_RATIO = 1.10
MAXIMUM_BACKGROUND_CHANNEL_ERROR = 4.0
MAXIMUM_DARKENED_BRIGHT_PIXEL_FRACTION = 0.10
MAXIMUM_BRIGHT_LUMA_P95_ERROR = 12.0


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--report", type=Path)
    return parser.parse_args()


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _resolve_file(root: Path, value: object, label: str) -> Path:
    if not isinstance(value, str) or not value:
        raise RuntimeError(f"{label} must be a non-empty local file name")
    relative = Path(value)
    if relative.is_absolute() or relative.name != value or value in {".", ".."}:
        raise RuntimeError(f"{label} must be a local file name")
    path = (root / relative).resolve()
    if not path.is_file():
        raise RuntimeError(f"{label} was not found: {path}")
    return path


def _load_manifest(path: Path) -> dict[str, object]:
    document = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(document, dict):
        raise RuntimeError("manifest root must be an object")
    if document.get("schemaVersion") != 1:
        raise RuntimeError("manifest schemaVersion must be 1")
    if document.get("contract") != CONTRACT:
        raise RuntimeError("manifest contract does not match the verifier")
    if document.get("backgroundRgb") != EXPECTED_BACKGROUND:
        raise RuntimeError("backgroundRgb must be the contracted 96 gray")
    return document


def _load_raw_frame(
    root: Path,
    value: object,
) -> tuple[np.ndarray, np.ndarray, Path]:
    if not isinstance(value, dict):
        raise RuntimeError("rawFrame must be an object")
    width = value.get("width")
    height = value.get("height")
    if not isinstance(width, int) or width <= 0:
        raise RuntimeError("rawFrame.width must be positive")
    if not isinstance(height, int) or height <= 0:
        raise RuntimeError("rawFrame.height must be positive")
    if value.get("format") != 87:
        raise RuntimeError("rawFrame.format must be DXGI_FORMAT_B8G8R8A8_UNORM")
    path = _resolve_file(root, value.get("path"), "rawFrame.path")
    payload = path.read_bytes()
    if len(payload) != width * height * 4:
        raise RuntimeError("rawFrame byte count does not match its dimensions")
    bgra = np.frombuffer(payload, dtype=np.uint8).reshape((height, width, 4))
    return bgra[:, :, [2, 1, 0]].copy(), bgra[:, :, 3].copy(), path


def _load_rgb(root: Path, value: object, label: str) -> tuple[np.ndarray, Path]:
    path = _resolve_file(root, value, label)
    with Image.open(path) as source:
        image = np.asarray(source.convert("RGB"), dtype=np.uint8)
    return image, path


def _luma(image: np.ndarray) -> np.ndarray:
    values = image.astype(np.float64)
    return (
        values[:, :, 0] * 0.2126
        + values[:, :, 1] * 0.7152
        + values[:, :, 2] * 0.0722
    )


def _percentile(values: np.ndarray, percentile: float) -> float:
    if values.size == 0:
        return 0.0
    return float(np.percentile(values, percentile))


def _verify(
    source_rgb: np.ndarray,
    alpha: np.ndarray,
    reference: np.ndarray,
    decoded: np.ndarray,
) -> dict[str, object]:
    if reference.shape != source_rgb.shape or decoded.shape != source_rgb.shape:
        raise RuntimeError("recording images must match the raw Spout2 frame dimensions")

    active = np.any(source_rgb != 0, axis=2) | (alpha != 0)
    rgb_above_alpha = np.any(source_rgb > alpha[:, :, None], axis=2)
    zero_alpha_emission = (alpha == 0) & np.any(source_rgb != 0, axis=2)
    if np.count_nonzero(active) < 100:
        raise RuntimeError("raw frame does not contain enough active effect pixels")
    if not np.any(rgb_above_alpha) or not np.any(zero_alpha_emission):
        raise RuntimeError("raw frame is not extended premultiplied")

    rows, columns = np.where(active)
    margin = 32
    top = max(0, int(rows.min()) - margin)
    bottom = min(active.shape[0], int(rows.max()) + margin + 1)
    left = max(0, int(columns.min()) - margin)
    right = min(active.shape[1], int(columns.max()) + margin + 1)
    roi = np.zeros_like(active)
    roi[top:bottom, left:right] = True
    background_mask = ~roi
    if np.count_nonzero(background_mask) < 100:
        raise RuntimeError("frame does not contain enough isolated background pixels")

    expected_background = np.asarray(EXPECTED_BACKGROUND, dtype=np.float64)
    reference_background = np.median(reference[background_mask], axis=0)
    decoded_background = np.median(decoded[background_mask], axis=0)
    reference_background_error = float(
        np.max(np.abs(reference_background - expected_background))
    )
    decoded_background_error = float(
        np.max(np.abs(decoded_background - reference_background))
    )
    if reference_background_error > 3.0:
        raise RuntimeError("reference screenshot background differs from 96 gray")
    if decoded_background_error > MAXIMUM_BACKGROUND_CHANNEL_ERROR:
        raise RuntimeError(
            "recording background differs from the OBS reference: "
            f"maxChannelError={decoded_background_error:.3f}"
        )

    reference_luma = _luma(reference)
    decoded_luma = _luma(decoded)
    coefficients = np.asarray([0.2126, 0.7152, 0.0722])
    reference_background_luma = float(np.dot(reference_background, coefficients))
    decoded_background_luma = float(np.dot(decoded_background, coefficients))
    reference_delta = reference_luma - reference_background_luma
    decoded_delta = decoded_luma - decoded_background_luma

    reference_energy = float(np.maximum(reference_delta[roi], 0.0).sum())
    decoded_energy = float(np.maximum(decoded_delta[roi], 0.0).sum())
    if reference_energy <= 0.0:
        raise RuntimeError("reference screenshot contains no positive effect energy")
    energy_ratio = decoded_energy / reference_energy
    if not MINIMUM_ENERGY_RATIO <= energy_ratio <= MAXIMUM_ENERGY_RATIO:
        raise RuntimeError(
            "recorded effect brightness energy differs from the OBS reference: "
            f"ratio={energy_ratio:.6f}"
        )

    bright = active & (reference_delta >= 24.0)
    if np.count_nonzero(bright) < 100:
        raise RuntimeError("reference screenshot contains too few bright effect pixels")
    reference_bright_energy = float(reference_delta[bright].sum())
    decoded_bright_energy = float(np.maximum(decoded_delta[bright], 0.0).sum())
    bright_energy_ratio = decoded_bright_energy / reference_bright_energy
    if not MINIMUM_BRIGHT_ENERGY_RATIO <= bright_energy_ratio <= MAXIMUM_BRIGHT_ENERGY_RATIO:
        raise RuntimeError(
            "recorded bright pixels differ from the OBS reference: "
            f"ratio={bright_energy_ratio:.6f}"
        )

    bright_error = np.abs(decoded_delta[bright] - reference_delta[bright])
    bright_p95_error = _percentile(bright_error, 95.0)
    if bright_p95_error > MAXIMUM_BRIGHT_LUMA_P95_ERROR:
        raise RuntimeError(
            "recorded bright-pixel luma error is too large: "
            f"p95={bright_p95_error:.6f}"
        )

    darkened = decoded_delta[bright] < reference_delta[bright] * 0.85 - 2.0
    darkened_fraction = float(np.count_nonzero(darkened) / np.count_nonzero(bright))
    if darkened_fraction > MAXIMUM_DARKENED_BRIGHT_PIXEL_FRACTION:
        raise RuntimeError(
            "too many recorded bright pixels are visibly darker than the OBS reference: "
            f"fraction={darkened_fraction:.6f}"
        )

    roi_delta_error = np.abs(decoded_delta[roi] - reference_delta[roi])
    return {
        "effectBounds": [left, top, right, bottom],
        "activePixels": int(np.count_nonzero(active)),
        "rgbAboveAlphaPixels": int(np.count_nonzero(rgb_above_alpha)),
        "zeroAlphaEmissionPixels": int(np.count_nonzero(zero_alpha_emission)),
        "referenceBackgroundRgb": [float(value) for value in reference_background],
        "decodedBackgroundRgb": [float(value) for value in decoded_background],
        "backgroundMaximumChannelError": decoded_background_error,
        "effectBrightnessEnergyRatio": energy_ratio,
        "brightPixelEnergyRatio": bright_energy_ratio,
        "brightPixelLumaP95AbsoluteError": bright_p95_error,
        "darkenedBrightPixelFraction": darkened_fraction,
        "roiLumaMeanAbsoluteError": float(roi_delta_error.mean()),
        "roiLumaP99AbsoluteError": _percentile(roi_delta_error, 99.0),
    }


def main() -> int:
    options = _arguments()
    manifest_path = options.manifest.resolve()
    root = manifest_path.parent
    document = _load_manifest(manifest_path)
    source_rgb, alpha, raw_path = _load_raw_frame(root, document.get("rawFrame"))
    reference, reference_path = _load_rgb(
        root, document.get("referenceImage"), "referenceImage"
    )
    decoded, decoded_path = _load_rgb(
        root, document.get("decodedFrame"), "decodedFrame"
    )
    video_path = _resolve_file(root, document.get("video"), "video")
    metrics = _verify(source_rgb, alpha, reference, decoded)

    report_path = (
        options.report.resolve()
        if options.report is not None
        else root / "recording-verification.json"
    )
    if report_path.exists():
        raise RuntimeError(f"verification report already exists: {report_path}")
    report = {
        "schemaVersion": 1,
        "status": "passed",
        "contract": CONTRACT,
        "comparison": "decoded-recording-frame-vs-obs-scene-png",
        "backgroundRgb": EXPECTED_BACKGROUND,
        "dimensions": [int(source_rgb.shape[1]), int(source_rgb.shape[0])],
        "rawFrame": {"path": raw_path.name, "sha256": _sha256(raw_path)},
        "referenceImage": {
            "path": reference_path.name,
            "sha256": _sha256(reference_path),
        },
        "decodedFrame": {
            "path": decoded_path.name,
            "sha256": _sha256(decoded_path),
        },
        "video": {"path": video_path.name, "sha256": _sha256(video_path)},
        "thresholds": {
            "effectBrightnessEnergyRatio": [MINIMUM_ENERGY_RATIO, MAXIMUM_ENERGY_RATIO],
            "brightPixelEnergyRatio": [
                MINIMUM_BRIGHT_ENERGY_RATIO,
                MAXIMUM_BRIGHT_ENERGY_RATIO,
            ],
            "backgroundMaximumChannelError": MAXIMUM_BACKGROUND_CHANNEL_ERROR,
            "darkenedBrightPixelFraction": MAXIMUM_DARKENED_BRIGHT_PIXEL_FRACTION,
            "brightPixelLumaP95AbsoluteError": MAXIMUM_BRIGHT_LUMA_P95_ERROR,
        },
        "metrics": metrics,
    }
    report_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"OBS Spout2 recording brightness verified: {report_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"OBS Spout2 recording verification failed: {error}", file=sys.stderr)
        raise SystemExit(1)
