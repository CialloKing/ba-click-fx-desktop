#!/usr/bin/env python3
"""Verify OBS preserves the Spout2 extended-premultiplied BGRA8 payload."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

import numpy as np
from PIL import Image


CONTRACT = "bgra8-srgb-extended-premultiplied-fx-only-v2"
EXPECTED_BACKGROUNDS = {
    "black": [0, 0, 0],
    "gray": [96, 96, 96],
    "white": [255, 255, 255],
    "color": [32, 80, 144],
}


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--tolerance", type=int, default=3)
    return parser.parse_args()


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _load_manifest(path: Path) -> dict[str, object]:
    document = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(document, dict):
        raise RuntimeError("manifest root must be an object")
    if document.get("schemaVersion") != 1:
        raise RuntimeError("manifest schemaVersion must be 1")
    if document.get("contract") != CONTRACT:
        raise RuntimeError("manifest contract does not match the verifier")
    if document.get("obsBlendMethod") != "default":
        raise RuntimeError("obsBlendMethod must be default")
    if document.get("obsBlendMode") != "normal":
        raise RuntimeError("obsBlendMode must be normal")
    return document


def _resolve_file(root: Path, value: object, label: str) -> Path:
    if not isinstance(value, str) or not value:
        raise RuntimeError(f"{label} must be a non-empty path")
    path = (root / value).resolve()
    if not path.is_file():
        raise RuntimeError(f"{label} was not found: {path}")
    return path


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
    rgb = bgra[:, :, [2, 1, 0]].copy()
    alpha = bgra[:, :, 3].copy()
    return rgb, alpha, path


def _prediction(
    source_rgb: np.ndarray,
    alpha: np.ndarray,
    background: np.ndarray,
) -> np.ndarray:
    source = source_rgb.astype(np.uint16)
    inverse_alpha = 255 - alpha.astype(np.uint16)
    contribution = (
        background.astype(np.uint16)[None, None, :] * inverse_alpha[:, :, None]
        + 127
    ) // 255
    return np.minimum(source + contribution, 255).astype(np.uint8)


def _percentile(values: np.ndarray, percentile: float) -> float:
    if values.size == 0:
        return 0.0
    return float(np.percentile(values, percentile))


def _verify_cases(
    root: Path,
    cases: object,
    source_rgb: np.ndarray,
    alpha: np.ndarray,
    tolerance: int,
) -> list[dict[str, object]]:
    if not isinstance(cases, list):
        raise RuntimeError("cases must be an array")
    by_name: dict[str, dict[str, object]] = {}
    for case in cases:
        if not isinstance(case, dict) or not isinstance(case.get("name"), str):
            raise RuntimeError("each case must have a string name")
        name = str(case["name"])
        if name in by_name:
            raise RuntimeError(f"duplicate composite case: {name}")
        by_name[name] = case
    if set(by_name) != set(EXPECTED_BACKGROUNDS):
        raise RuntimeError("cases must contain black, gray, white, and color exactly once")

    effect_mask = np.any(source_rgb != 0, axis=2) | (alpha != 0)
    zero_alpha_emission = (alpha == 0) & np.any(source_rgb != 0, axis=2)
    reports: list[dict[str, object]] = []
    for name, expected_background in EXPECTED_BACKGROUNDS.items():
        case = by_name[name]
        background = case.get("backgroundRgb")
        if background != expected_background:
            raise RuntimeError(f"{name}.backgroundRgb is not the contracted value")
        image_path = _resolve_file(root, case.get("image"), f"{name}.image")
        with Image.open(image_path) as source:
            observed = np.asarray(source.convert("RGB"), dtype=np.uint8)
        if observed.shape != source_rgb.shape:
            raise RuntimeError(f"{name} image dimensions do not match the raw frame")

        background_array = np.asarray(expected_background, dtype=np.uint8)
        predicted = _prediction(source_rgb, alpha, background_array)
        delta = np.abs(observed.astype(np.int16) - predicted.astype(np.int16))
        effect_delta = delta[effect_mask]
        maximum = int(delta.max())
        effect_p99 = _percentile(effect_delta, 99.0)
        if maximum > tolerance or effect_p99 > tolerance:
            raise RuntimeError(
                f"{name} composite differs from ONE/INV_SRC_ALPHA: "
                f"max={maximum}, effectP99={effect_p99:.3f}, tolerance={tolerance}"
            )
        if np.any(
            observed[zero_alpha_emission].astype(np.int16)
            < background_array.astype(np.int16) - tolerance
        ):
            raise RuntimeError(f"{name} darkens zero-Alpha additive emission pixels")
        reports.append(
            {
                "name": name,
                "backgroundRgb": expected_background,
                "image": image_path.name,
                "sha256": _sha256(image_path),
                "maximumChannelError": maximum,
                "effectMeanAbsoluteError": (
                    float(effect_delta.mean()) if effect_delta.size else 0.0
                ),
                "effectP99ChannelError": effect_p99,
            }
        )
    return reports


def main() -> int:
    options = _arguments()
    if options.tolerance < 0 or options.tolerance > 32:
        raise RuntimeError("tolerance must be between 0 and 32")
    manifest_path = options.manifest.resolve()
    root = manifest_path.parent
    document = _load_manifest(manifest_path)
    source_rgb, alpha, raw_path = _load_raw_frame(root, document.get("rawFrame"))
    rgb_above_alpha = np.any(source_rgb > alpha[:, :, None], axis=2)
    zero_alpha_emission = (alpha == 0) & np.any(source_rgb != 0, axis=2)
    coverage = alpha != 0
    if not np.any(rgb_above_alpha):
        raise RuntimeError("raw frame contains no RGB above Alpha")
    if not np.any(zero_alpha_emission):
        raise RuntimeError("raw frame contains no zero-Alpha additive emission")
    if not np.any(coverage):
        raise RuntimeError("raw frame contains no Cross2 coverage")

    case_reports = _verify_cases(
        root,
        document.get("cases"),
        source_rgb,
        alpha,
        options.tolerance,
    )
    report_path = (
        options.report.resolve()
        if options.report is not None
        else root / "composite-verification.json"
    )
    if report_path.exists():
        raise RuntimeError(f"verification report already exists: {report_path}")
    report = {
        "schemaVersion": 1,
        "status": "passed",
        "contract": CONTRACT,
        "obsBlendMethod": "default",
        "obsBlendMode": "normal",
        "dimensions": [int(source_rgb.shape[1]), int(source_rgb.shape[0])],
        "rawFrame": {
            "path": raw_path.name,
            "sha256": _sha256(raw_path),
            "rgbAboveAlphaPixels": int(np.count_nonzero(rgb_above_alpha)),
            "zeroAlphaEmissionPixels": int(np.count_nonzero(zero_alpha_emission)),
            "coveragePixels": int(np.count_nonzero(coverage)),
        },
        "tolerance": options.tolerance,
        "cases": case_reports,
    }
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"OBS Spout2 extended composite verified: {report_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"OBS Spout2 composite verification failed: {error}", file=sys.stderr)
        raise SystemExit(1)
