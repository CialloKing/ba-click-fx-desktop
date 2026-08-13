#!/usr/bin/env python3
"""Compare the native ten-slice capture with the Unity visual reference.

The comparison deliberately ignores individual particle coordinates.  Unity and
the native simulator use different random streams, so aggregate display energy,
coverage, centroid distance from the click, and a coarse radial histogram are
the stable cross-implementation contract.  Optional FP16 checks validate the
native layer graph without treating a PNG hash as numerical evidence.

The script uses only Python's standard library.  It reads files and writes
nothing unless the caller redirects stdout.
"""

from __future__ import annotations

import argparse
import binascii
import json
import math
import os
from dataclasses import asdict, dataclass
from pathlib import Path
import struct
import sys
import zlib

try:
    from PIL import Image as PillowImage
except ImportError:
    PillowImage = None


AGES = (50, 100, 110, 120, 130, 140, 150, 180, 250, 450)
# Bins are fractions of the viewport height.  They are intentionally coarse:
# changing a particle's random angle must not look like a placement failure.
RADIAL_EDGES = (
    0.0,
    0.02,
    0.04,
    0.06,
    0.09,
    0.13,
    0.20,
    0.32,
    0.50,
    math.inf,
)
NON_BLACK_THRESHOLD = 2
EXPOSURE_GAIN = 0.125058532
SAMPLE_SCALE = 1.42925835
FP16_LAYER_TOLERANCE = 0.002
COMPOSITE_GAIN_TOLERANCE = 0.03
DOWNSAMPLE_MEAN_RATIO_MIN = 0.85
DOWNSAMPLE_MEAN_RATIO_MAX = 1.15


class ValidationError(RuntimeError):
    """A malformed input or an unavailable required artifact."""


@dataclass(frozen=True)
class Image8:
    width: int
    height: int
    rgb: bytes


@dataclass(frozen=True)
class DisplayMetrics:
    width: int
    height: int
    display_energy: int
    channel_energy: tuple[int, int, int]
    chromaticity: tuple[float, float, float]
    linear_luma_energy: float
    coverage_pixels: int
    coverage_fraction: float
    centroid_x: float
    centroid_y: float
    centroid_distance_px: float
    radial_rms_fraction: float
    radial_histogram: tuple[float, ...]


@dataclass(frozen=True)
class Tolerance:
    energy_relative: float
    coverage_relative: float
    centroid_distance_px: float
    radial_histogram_l1: float
    chromaticity_l1: float


@dataclass(frozen=True)
class LayerMetrics:
    finite: bool
    non_negative_rgb: bool
    direct_seed_min_delta: float
    final_direct_min_delta: float
    final_direct_alpha_min_delta: float
    direct_seed_alpha_min_delta: float
    direct_rgb_sum: float
    seed_rgb_sum: float
    final_rgb_sum: float
    bloom_delta_rgb_sum: float
    up00_rgb_sum: float
    composite_gain_ratio: float | None
    down_mean_ratios: tuple[float, ...]
    up_mean_monotonic: bool


@dataclass(frozen=True)
class SliceResult:
    age_ms: int
    golden: DisplayMetrics
    native: DisplayMetrics
    tolerance: Tolerance
    energy_relative_error: float
    coverage_relative_error: float
    centroid_distance_error_px: float
    radial_histogram_l1: float
    chromaticity_l1: float
    passed_display: bool
    layers: LayerMetrics | None
    passed_layers: bool | None


def _srgb_to_linear_byte() -> tuple[float, ...]:
    values = []
    for value in range(256):
        encoded = value / 255.0
        values.append(
            encoded / 12.92
            if encoded <= 0.04045
            else ((encoded + 0.055) / 1.055) ** 2.4)
    return tuple(values)


SRGB_LINEAR = _srgb_to_linear_byte()


def _read_png(path: Path) -> Image8:
    """Decode the 8-bit, non-interlaced RGBA/RGB PNGs emitted by both tools."""
    if PillowImage is not None:
        try:
            with PillowImage.open(path) as image:
                rgb_image = image.convert("RGB")
                return Image8(rgb_image.width, rgb_image.height, rgb_image.tobytes())
        except (OSError, ValueError) as error:
            raise ValidationError(f"unable to decode PNG: {path}: {error}") from error

    try:
        payload = path.read_bytes()
    except OSError as error:
        raise ValidationError(f"unable to read PNG: {path}: {error}") from error

    if payload[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValidationError(f"not a PNG: {path}")
    offset = 8
    width = height = bit_depth = color_type = None
    interlace = None
    idat = bytearray()
    while offset < len(payload):
        if offset + 12 > len(payload):
            raise ValidationError(f"truncated PNG chunk: {path}")
        length = struct.unpack_from(">I", payload, offset)[0]
        chunk_start = offset + 4
        chunk_end = chunk_start + 4 + length + 4
        if chunk_end > len(payload):
            raise ValidationError(f"PNG chunk exceeds file: {path}")
        kind = payload[chunk_start:chunk_start + 4]
        data_start = chunk_start + 4
        data_end = data_start + length
        data = payload[data_start:data_end]
        expected_crc = struct.unpack_from(">I", payload, data_end)[0]
        actual_crc = binascii.crc32(kind)
        actual_crc = binascii.crc32(data, actual_crc) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise ValidationError(f"PNG CRC mismatch in {path}")
        if kind == b"IHDR":
            if length != 13:
                raise ValidationError(f"invalid PNG IHDR: {path}")
            width, height, bit_depth, color_type, compression, filtering, interlace = (
                struct.unpack(">IIBBBBB", data)
            )
            if compression != 0 or filtering != 0 or interlace != 0:
                raise ValidationError(
                    f"unsupported PNG encoding (must be non-interlaced): {path}"
                )
            if bit_depth != 8 or color_type not in (2, 6):
                raise ValidationError(
                    f"unsupported PNG format (need 8-bit RGB/RGBA): {path}"
                )
        elif kind == b"IDAT":
            idat.extend(data)
        elif kind == b"IEND":
            break
        offset = chunk_end

    if width is None or height is None or not idat:
        raise ValidationError(f"PNG lacks IHDR/IDAT: {path}")
    channels = 4 if color_type == 6 else 3
    row_bytes = width * channels
    try:
        filtered = zlib.decompress(bytes(idat))
    except zlib.error as error:
        raise ValidationError(f"PNG zlib decode failed: {path}: {error}") from error
    expected_size = height * (row_bytes + 1)
    if len(filtered) != expected_size:
        raise ValidationError(f"PNG scanline size mismatch: {path}")

    rows = bytearray(height * row_bytes)
    source_offset = 0
    for y in range(height):
        filter_type = filtered[source_offset]
        source_offset += 1
        source = filtered[source_offset:source_offset + row_bytes]
        source_offset += row_bytes
        destination_offset = y * row_bytes
        previous_offset = (y - 1) * row_bytes
        for index, value in enumerate(source):
            left = rows[destination_offset + index - channels] if index >= channels else 0
            above = rows[previous_offset + index] if y > 0 else 0
            upper_left = (
                rows[previous_offset + index - channels]
                if y > 0 and index >= channels
                else 0
            )
            if filter_type == 0:
                decoded = value
            elif filter_type == 1:
                decoded = value + left
            elif filter_type == 2:
                decoded = value + above
            elif filter_type == 3:
                decoded = value + ((left + above) // 2)
            elif filter_type == 4:
                estimate = left + above - upper_left
                distance_left = abs(estimate - left)
                distance_above = abs(estimate - above)
                distance_upper_left = abs(estimate - upper_left)
                if distance_left <= distance_above and distance_left <= distance_upper_left:
                    predictor = left
                elif distance_above <= distance_upper_left:
                    predictor = above
                else:
                    predictor = upper_left
                decoded = value + predictor
            else:
                raise ValidationError(f"unsupported PNG filter {filter_type}: {path}")
            rows[destination_offset + index] = decoded & 0xFF

    if channels == 3:
        return Image8(width, height, bytes(rows))
    rgb = bytearray(width * height * 3)
    for source_index in range(width * height):
        source_offset = source_index * 4
        destination_offset = source_index * 3
        rgb[destination_offset:destination_offset + 3] = rows[
            source_offset:source_offset + 3
        ]
    return Image8(width, height, bytes(rgb))


def _display_metrics(image: Image8) -> DisplayMetrics:
    width = image.width
    height = image.height
    pixel_count = width * height
    if len(image.rgb) != pixel_count * 3:
        raise ValidationError("decoded PNG storage does not match dimensions")
    center_x = (width - 1) * 0.5
    center_y = (height - 1) * 0.5
    edge_squared = tuple((edge * height) ** 2 for edge in RADIAL_EDGES[1:])
    histogram = [0.0] * (len(RADIAL_EDGES) - 1)
    display_energy = 0
    channel_energy = [0, 0, 0]
    linear_energy = 0.0
    weighted_x = 0.0
    weighted_y = 0.0
    weighted_radius_squared = 0.0
    coverage = 0
    x_squared = tuple((index - center_x) ** 2 for index in range(width))
    for y in range(height):
        row_offset = y * width * 3
        y_squared = (y - center_y) ** 2
        for x in range(width):
            offset = row_offset + x * 3
            red = image.rgb[offset]
            green = image.rgb[offset + 1]
            blue = image.rgb[offset + 2]
            weight = red + green + blue
            if weight == 0:
                continue
            display_energy += weight
            channel_energy[0] += red
            channel_energy[1] += green
            channel_energy[2] += blue
            linear_energy += (
                SRGB_LINEAR[red] * 0.2126
                + SRGB_LINEAR[green] * 0.7152
                + SRGB_LINEAR[blue] * 0.0722
            )
            weighted_x += (x + 0.5) * weight
            weighted_y += (y + 0.5) * weight
            radius_squared = x_squared[x] + y_squared
            weighted_radius_squared += radius_squared * weight
            if max(red, green, blue) > NON_BLACK_THRESHOLD:
                coverage += 1
            bin_index = len(histogram) - 1
            for edge_index, edge_squared_value in enumerate(edge_squared):
                if radius_squared < edge_squared_value:
                    bin_index = edge_index
                    break
            histogram[bin_index] += weight

    if display_energy <= 0:
        raise ValidationError("PNG has zero display energy")
    histogram = tuple(value / display_energy for value in histogram)
    chromaticity = tuple(value / display_energy for value in channel_energy)
    centroid_x = weighted_x / display_energy
    centroid_y = weighted_y / display_energy
    distance = math.hypot(centroid_x - (center_x + 0.5), centroid_y - (center_y + 0.5))
    radial_rms = math.sqrt(weighted_radius_squared / display_energy) / height
    return DisplayMetrics(
        width=width,
        height=height,
        display_energy=display_energy,
        channel_energy=tuple(channel_energy),
        chromaticity=chromaticity,
        linear_luma_energy=linear_energy,
        coverage_pixels=coverage,
        coverage_fraction=coverage / pixel_count,
        centroid_x=centroid_x,
        centroid_y=centroid_y,
        centroid_distance_px=distance,
        radial_rms_fraction=radial_rms,
        radial_histogram=histogram,
    )


def _relative_error(actual: float, expected: float) -> float:
    return abs(actual - expected) / max(abs(expected), 1.0e-12)


def _iter_half_pixels(path: Path, width: int, height: int):
    expected_bytes = width * height * 8
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise ValidationError(f"unable to read FP16 layer: {path}: {error}") from error
    if len(payload) != expected_bytes:
        raise ValidationError(
            f"FP16 byte count mismatch for {path}: expected {expected_bytes}, got {len(payload)}"
        )
    return struct.iter_unpack("<4e", payload)


def _sum_layer(path: Path, width: int, height: int) -> tuple[float, bool, bool, float]:
    total = 0.0
    finite = True
    non_negative = True
    maximum_alpha = 0.0
    for red, green, blue, alpha in _iter_half_pixels(path, width, height):
        values = (red, green, blue, alpha)
        if not all(math.isfinite(value) for value in values):
            finite = False
        if red < -0.002 or green < -0.002 or blue < -0.002:
            non_negative = False
        total += red + green + blue
        maximum_alpha = max(maximum_alpha, abs(alpha))
    return total, finite, non_negative, maximum_alpha


def _layer_metrics(age_directory: Path, layer_info: dict[str, dict]) -> LayerMetrics:
    required = (
        "DirectSurface",
        "BloomSeed",
        "FinalOverlay",
        "Prefilter_Down00",
        "Up00",
    )
    missing = [name for name in required if name not in layer_info]
    if missing:
        raise ValidationError(
            f"{age_directory.name}: all-layer capture is missing {', '.join(missing)}"
        )
    dimensions = {
        name: (int(info["width"]), int(info["height"]))
        for name, info in layer_info.items()
    }
    direct_path = age_directory / "DirectSurface.rgba16f"
    seed_path = age_directory / "BloomSeed.rgba16f"
    final_path = age_directory / "FinalOverlay.rgba16f"
    width, height = dimensions["DirectSurface"]
    if dimensions["BloomSeed"] != (width, height) or dimensions["FinalOverlay"] != (width, height):
        raise ValidationError(f"{age_directory.name}: full-resolution layer dimensions differ")

    direct_sum = 0.0
    seed_sum = 0.0
    final_sum = 0.0
    bloom_delta_sum = 0.0
    direct_seed_min = math.inf
    final_direct_min = math.inf
    final_direct_alpha_min = math.inf
    direct_seed_alpha_min = math.inf
    finite = True
    non_negative = True
    direct_iter = _iter_half_pixels(direct_path, width, height)
    seed_iter = _iter_half_pixels(seed_path, width, height)
    final_iter = _iter_half_pixels(final_path, width, height)
    for direct, seed, final in zip(direct_iter, seed_iter, final_iter):
        if not all(math.isfinite(value) for value in (*direct, *seed, *final)):
            finite = False
        if min(direct[:3] + seed[:3] + final[:3]) < -0.002:
            non_negative = False
        direct_sum += sum(direct[:3])
        seed_sum += sum(seed[:3])
        final_sum += sum(final[:3])
        for channel in range(3):
            direct_seed_min = min(direct_seed_min, direct[channel] - seed[channel])
            final_direct_min = min(final_direct_min, final[channel] - direct[channel])
        final_direct_alpha_min = min(
            final_direct_alpha_min,
            final[3] - direct[3],
        )
        direct_seed_alpha_min = min(
            direct_seed_alpha_min,
            direct[3] - seed[3],
        )
        bloom_delta_sum += sum(final[channel] - direct[channel] for channel in range(3))

    up_width, up_height = dimensions["Up00"]
    up00_sum, up_finite, up_non_negative, _ = _sum_layer(
        age_directory / "Up00.rgba16f", up_width, up_height
    )
    finite = finite and up_finite
    non_negative = non_negative and up_non_negative
    composite_gain_ratio = None
    if up00_sum > 0.0:
        area_ratio = (width * height) / (up_width * up_height)
        expected_delta = EXPOSURE_GAIN * area_ratio * up00_sum
        composite_gain_ratio = bloom_delta_sum / expected_delta

    down_names = sorted(
        (
            name
            for name in layer_info
            if name == "Prefilter_Down00" or name.startswith("Down")
        ),
        key=lambda name: 0 if name == "Prefilter_Down00" else int(name[4:]),
    )
    prefilter_mean = None
    down_ratios = []
    for name in down_names:
        dw, dh = dimensions[name]
        total, layer_finite, layer_non_negative, _ = _sum_layer(
            age_directory / f"{name}.rgba16f", dw, dh
        )
        finite = finite and layer_finite
        non_negative = non_negative and layer_non_negative
        mean = total / (dw * dh)
        if prefilter_mean is None:
            prefilter_mean = mean
        down_ratios.append(mean / prefilter_mean if prefilter_mean > 0.0 else 1.0)

    up_names = sorted(
        (name for name in layer_info if name.startswith("Up")),
        key=lambda name: int(name[2:]),
    )
    up_means = []
    for name in up_names:
        uw, uh = dimensions[name]
        total, layer_finite, layer_non_negative, _ = _sum_layer(
            age_directory / f"{name}.rgba16f", uw, uh
        )
        finite = finite and layer_finite
        non_negative = non_negative and layer_non_negative
        up_means.append(total / (uw * uh))
    up_monotonic = all(
        up_means[index] + 1.0e-5 >= up_means[index + 1]
        for index in range(len(up_means) - 1)
    )

    return LayerMetrics(
        finite=finite,
        non_negative_rgb=non_negative,
        direct_seed_min_delta=direct_seed_min,
        final_direct_min_delta=final_direct_min,
        final_direct_alpha_min_delta=final_direct_alpha_min,
        direct_seed_alpha_min_delta=direct_seed_alpha_min,
        direct_rgb_sum=direct_sum,
        seed_rgb_sum=seed_sum,
        final_rgb_sum=final_sum,
        bloom_delta_rgb_sum=bloom_delta_sum,
        up00_rgb_sum=up00_sum,
        composite_gain_ratio=composite_gain_ratio,
        down_mean_ratios=tuple(down_ratios),
        up_mean_monotonic=up_monotonic,
    )


def _layer_contract_failures(layers: LayerMetrics) -> tuple[str, ...]:
    failures = []
    if not layers.finite:
        failures.append("layer graph contains non-finite values")
    if not layers.non_negative_rgb:
        failures.append("layer graph contains negative RGB")
    if layers.direct_seed_min_delta < -FP16_LAYER_TOLERANCE:
        failures.append(
            "BloomSeed RGB exceeds DirectSurface RGB: "
            f"minDelta={layers.direct_seed_min_delta:.6g}"
        )
    if layers.final_direct_min_delta < -FP16_LAYER_TOLERANCE:
        failures.append(
            "FinalOverlay RGB falls below DirectSurface RGB: "
            f"minDelta={layers.final_direct_min_delta:.6g}"
        )
    # Bloom-disabled materials still contribute authored coverage to the direct
    # surface, so seed Alpha is a subset rather than an identical copy.
    if layers.direct_seed_alpha_min_delta < -FP16_LAYER_TOLERANCE:
        failures.append(
            "BloomSeed Alpha exceeds DirectSurface Alpha: "
            f"minDelta={layers.direct_seed_alpha_min_delta:.6g}"
        )
    # Bloom propagation may expand transport coverage, but it must never erase
    # the authored direct coverage already present at the same pixel.
    if layers.final_direct_alpha_min_delta < -FP16_LAYER_TOLERANCE:
        failures.append(
            "FinalOverlay Alpha falls below DirectSurface Alpha: "
            f"minDelta={layers.final_direct_alpha_min_delta:.6g}"
        )
    if (
        layers.composite_gain_ratio is None
        or abs(layers.composite_gain_ratio - 1.0) > COMPOSITE_GAIN_TOLERANCE
    ):
        failures.append(
            "Bloom composite gain differs from the planned exposure: "
            f"ratio={layers.composite_gain_ratio}"
        )
    invalid_down_ratios = tuple(
        ratio
        for ratio in layers.down_mean_ratios
        if not DOWNSAMPLE_MEAN_RATIO_MIN <= ratio <= DOWNSAMPLE_MEAN_RATIO_MAX
    )
    if invalid_down_ratios:
        failures.append(
            "Bloom downsample mean energy left the conservation range: "
            + ",".join(f"{ratio:.6g}" for ratio in invalid_down_ratios)
        )
    if not layers.up_mean_monotonic:
        failures.append("Bloom upsample mean energy is not monotonic")
    return tuple(failures)


def _tolerance(age_ms: int) -> Tolerance:
    if age_ms <= 50:
        return Tolerance(0.35, 0.30, 32.0, 0.15, 0.08)
    if age_ms <= 180:
        return Tolerance(0.30, 0.25, 32.0, 0.15, 0.10)
    if age_ms <= 250:
        return Tolerance(0.35, 0.30, 64.0, 0.20, 0.14)
    return Tolerance(0.40, 0.35, 96.0, 0.25, 0.16)


def _find_age_directory(root: Path, age_ms: int) -> Path:
    directory = root / f"{age_ms:04d}ms"
    if not directory.is_dir():
        raise ValidationError(f"missing native age directory: {directory}")
    return directory


def _load_manifest(root: Path) -> dict:
    path = root / "manifest.json"
    if not path.is_file():
        raise ValidationError(f"native manifest not found: {path}")
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValidationError(f"invalid native manifest {path}: {error}") from error
    if manifest.get("viewport", {}).get("width") != 1950 or manifest.get("viewport", {}).get("height") != 1097:
        raise ValidationError("native manifest viewport must be 1950x1097")
    if manifest.get("schemaVersion") != 1:
        raise ValidationError("native manifest schemaVersion must be 1")
    if manifest.get("driver") != "WARP" or int(manifest.get("seed", -1)) != 20260716:
        raise ValidationError("native manifest must use WARP and seed 20260716")
    if manifest.get("rowOrigin") != "top-left":
        raise ValidationError("native manifest rowOrigin must be top-left")
    bloom = manifest.get("bloom", {})
    if int(bloom.get("mipCount", -1)) != 6:
        raise ValidationError("native manifest Bloom mipCount must be 6")
    if abs(float(bloom.get("sampleScale", math.nan)) - SAMPLE_SCALE) > 1.0e-6:
        raise ValidationError("native manifest Bloom sampleScale differs from Unity")
    if abs(float(bloom.get("exposureGain", math.nan)) - EXPOSURE_GAIN) > 1.0e-7:
        raise ValidationError("native manifest Bloom exposureGain differs from Unity")
    manifest_ages = [int(entry.get("ageMs", -1)) for entry in manifest.get("ages", [])]
    if sorted(manifest_ages) != list(AGES):
        raise ValidationError(
            "native manifest must contain each locked age exactly once: "
            + ", ".join(str(age) for age in AGES)
        )
    return manifest


def _compare_slice(age_ms: int, unity_root: Path, native_root: Path, check_layers: bool, manifest: dict) -> SliceResult:
    golden_path = unity_root / f"FX_Touch_{age_ms:04d}ms.png"
    native_directory = _find_age_directory(native_root, age_ms)
    native_path = native_directory / "FinalOverlay.png"
    golden = _display_metrics(_read_png(golden_path))
    native = _display_metrics(_read_png(native_path))
    if (golden.width, golden.height) != (native.width, native.height):
        raise ValidationError(f"{age_ms}ms: Golden/native dimensions differ")
    tolerance = _tolerance(age_ms)
    energy_error = _relative_error(native.display_energy, golden.display_energy)
    coverage_error = _relative_error(native.coverage_pixels, golden.coverage_pixels)
    centroid_error = abs(native.centroid_distance_px - golden.centroid_distance_px)
    radial_error = sum(
        abs(a - b) for a, b in zip(native.radial_histogram, golden.radial_histogram)
    )
    chromaticity_error = sum(
        abs(a - b) for a, b in zip(native.chromaticity, golden.chromaticity)
    )
    passed_display = (
        energy_error <= tolerance.energy_relative
        and coverage_error <= tolerance.coverage_relative
        and centroid_error <= tolerance.centroid_distance_px
        and radial_error <= tolerance.radial_histogram_l1
        and chromaticity_error <= tolerance.chromaticity_l1
    )

    layers = None
    passed_layers = None
    if check_layers:
        age_record = next(
            (entry for entry in manifest.get("ages", []) if int(entry.get("ageMs", -1)) == age_ms),
            None,
        )
        if age_record is None:
            raise ValidationError(f"{age_ms}ms: manifest has no age record")
        layer_info = {entry["name"]: entry for entry in age_record.get("layers", [])}
        layers = _layer_metrics(native_directory, layer_info)
        passed_layers = not _layer_contract_failures(layers)
    return SliceResult(
        age_ms=age_ms,
        golden=golden,
        native=native,
        tolerance=tolerance,
        energy_relative_error=energy_error,
        coverage_relative_error=coverage_error,
        centroid_distance_error_px=centroid_error,
        radial_histogram_l1=radial_error,
        chromaticity_l1=chromaticity_error,
        passed_display=passed_display,
        layers=layers,
        passed_layers=passed_layers,
    )


def _default_unity_root() -> Path:
    value = os.environ.get("BAFX_UNITY_RUNTIME_ROOT")
    if value:
        return Path(value) / "Reference" / "Captures"
    return Path(
        r"D:\WebProjects\BA鼠标输入与点击特效系统\UnityMouseFxLab\UnityMouseFxLab\Reference\Captures"
    )


def _format_result(result: SliceResult) -> str:
    status = "PASS" if result.passed_display and (result.passed_layers is not False) else "FAIL"
    return (
        f"{status} {result.age_ms:04d}ms "
        f"energy={result.native.display_energy}/{result.golden.display_energy} "
        f"({result.energy_relative_error:.1%}) "
        f"coverage={result.native.coverage_pixels}/{result.golden.coverage_pixels} "
        f"({result.coverage_relative_error:.1%}) "
        f"centroidDelta={result.centroid_distance_error_px:.2f}px "
        f"radialL1={result.radial_histogram_l1:.3f} "
        f"chromaL1={result.chromaticity_l1:.3f}"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--unity-captures",
        type=Path,
        default=None,
        help="directory containing FX_Touch_XXXXms.png (default: Unity reference)",
    )
    parser.add_argument(
        "--native-root",
        type=Path,
        default=Path("artifacts/local/gpu-captures/native"),
        help="native capture directory containing manifest.json and age folders",
    )
    parser.add_argument(
        "--no-layers",
        action="store_true",
        help="skip FP16 intermediate-layer checks even when manifest has allLayers=true",
    )
    parser.add_argument(
        "--require-layers",
        action="store_true",
        help="fail if the native manifest does not contain all intermediate layers",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="emit a JSON report after the human-readable lines",
    )
    args = parser.parse_args(argv)
    unity_root = (args.unity_captures or _default_unity_root()).resolve()
    native_root = args.native_root.resolve()
    try:
        manifest = _load_manifest(native_root)
        manifest_has_layers = bool(manifest.get("allLayers", False))
        if args.require_layers and not manifest_has_layers:
            raise ValidationError("--require-layers requires a manifest captured with --all-layers")
        check_layers = manifest_has_layers and not args.no_layers
        results = [
            _compare_slice(age, unity_root, native_root, check_layers, manifest)
            for age in AGES
        ]
    except ValidationError as error:
        print(f"golden-metrics: ERROR: {error}", file=sys.stderr)
        return 2

    for result in results:
        print(_format_result(result))
    passed = all(
        result.passed_display and (result.passed_layers is not False)
        for result in results
    )
    if args.json:
        print(json.dumps([asdict(result) for result in results], indent=2))
    if not passed:
        for result in results:
            if not result.passed_display:
                print(
                    f"golden-metrics: display threshold failure at {result.age_ms}ms",
                    file=sys.stderr,
                )
            if result.passed_layers is False:
                details = "; ".join(_layer_contract_failures(result.layers))
                print(
                    f"golden-metrics: layer threshold failure at {result.age_ms}ms: "
                    + details,
                    file=sys.stderr,
                )
        return 1
    print(
        f"Golden metrics passed: {len(results)} slices"
        + (" + FP16 layers" if check_layers else "")
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
