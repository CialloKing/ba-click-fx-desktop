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
TRAIL_SIGNAL_THRESHOLD = 24
EXPOSURE_GAIN = 0.125058532
SAMPLE_SCALE = 1.42925835
FP16_LAYER_TOLERANCE = 0.002
COMPOSITE_RGB_RELATIVE_TOLERANCE = 0.001
DOWNSAMPLE_MEAN_RATIO_MIN = 0.85
DOWNSAMPLE_MEAN_RATIO_MAX = 1.15
CAPTURE_WIDTH = 1950
CAPTURE_HEIGHT = 1097
CAPTURE_RGBA16F_BYTES = CAPTURE_WIDTH * CAPTURE_HEIGHT * 8
DRAG_WITH_TRAIL_REFERENCE = (
    "Reference/Diagnostics/Interaction/"
    "FX_Touch_0140ms_Move_0432px_WithTrail_Age0140ms.png"
)
DRAG_NO_TRAIL_REFERENCE = (
    "Reference/Diagnostics/Interaction/"
    "FX_Touch_0140ms_Move_0432px_NoTrail_Age0140ms.png"
)
TRAIL_ONLY_REFERENCE = (
    "Reference/Diagnostics/Trail/FX_Touch_0140ms_TrailOnly_20px.png"
)
FINAL_ONLY_LAYER_NAMES = frozenset(("FinalOverlay",))
LAYER_ALPHA_SEMANTICS = {
    "DirectSurface": "authored-coverage-union",
    "BloomSeed": "bloom-source-coverage",
    "Prefilter_Down00": "bloom-transport-energy",
    "Down01": "bloom-transport-energy",
    "Down02": "bloom-transport-energy",
    "Down03": "bloom-transport-energy",
    "Down04": "bloom-transport-energy",
    "Down05": "bloom-transport-energy",
    "Up00": "bloom-transport-energy",
    "Up01": "bloom-transport-energy",
    "Up02": "bloom-transport-energy",
    "Up03": "bloom-transport-energy",
    "Up04": "bloom-transport-energy",
    "BloomResult": "bloom-transport-coverage",
    "FinalOverlay": "coverage-union",
}
ALL_LAYER_NAMES = frozenset(
    (
        "DirectSurface",
        "BloomSeed",
        "Prefilter_Down00",
        "Down01",
        "Down02",
        "Down03",
        "Down04",
        "Down05",
        "Up00",
        "Up01",
        "Up02",
        "Up03",
        "Up04",
        "BloomResult",
        "FinalOverlay",
    )
)
EXPECTED_LAYER_EXTENTS = {
    "DirectSurface": (1950, 1097),
    "BloomSeed": (1950, 1097),
    "Prefilter_Down00": (975, 548),
    "Down01": (487, 274),
    "Down02": (243, 137),
    "Down03": (121, 68),
    "Down04": (60, 34),
    "Down05": (30, 17),
    "Up00": (975, 548),
    "Up01": (487, 274),
    "Up02": (243, 137),
    "Up03": (121, 68),
    "Up04": (60, 34),
    "BloomResult": (1950, 1097),
    "FinalOverlay": (1950, 1097),
}
TRAIL_DELTA_REFERENCE_ENERGY = 4_386_871
TRAIL_DELTA_REFERENCE_COVERAGE = 40_598
TRAIL_DELTA_REFERENCE_CENTROID = (1108.4099, 549.2720)
TRAIL_DELTA_REFERENCE_CHROMATICITY = (0.0, 0.384149, 0.615851)
TRAIL_DELTA_REFERENCE_BOUNDS = (894, 460, 1244, 637)
TRAIL_DELTA_ENERGY_RELATIVE_TOLERANCE = 0.05
TRAIL_DELTA_COVERAGE_RELATIVE_TOLERANCE = 0.03
TRAIL_DELTA_CENTROID_TOLERANCE = (2.0, 3.0)
TRAIL_DELTA_CHROMATICITY_L1_TOLERANCE = 0.01
TRAIL_DELTA_BOUNDS_TOLERANCE_PX = 4
TRAIL_ONLY_REFERENCE_ENERGY = 588_979
TRAIL_ONLY_REFERENCE_COVERAGE = 25_711
TRAIL_ONLY_REFERENCE_CENTROID = (977.2882, 547.8723)
TRAIL_ONLY_REFERENCE_CHROMATICITY = (0.0, 0.323896, 0.676104)
TRAIL_ONLY_REFERENCE_BOUNDS = (895, 457, 1054, 639)
TRAIL_ONLY_ENERGY_RELATIVE_TOLERANCE = 0.15
TRAIL_ONLY_COVERAGE_RELATIVE_TOLERANCE = 0.12
TRAIL_ONLY_CENTROID_TOLERANCE_PX = 1.0
TRAIL_ONLY_CHROMATICITY_L1_TOLERANCE = 0.03
TRAIL_ONLY_BOUNDS_TOLERANCE_PX = 12


class ValidationError(RuntimeError):
    """A malformed input or an unavailable required artifact."""


class ArgumentValidationError(RuntimeError):
    """A command-line error that the caller may request as JSON."""


class GoldenArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise ArgumentValidationError(message)


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
    bloom_result_rgb_sum: float
    up00_rgb_sum: float
    composite_rgb_max_absolute_error: float
    composite_rgb_mean_absolute_error: float
    composite_rgb_max_tolerance_ratio: float
    composite_rgb_first_failure: tuple[int, int, int] | None
    composite_alpha_max_absolute_error: float
    composite_alpha_first_failure: tuple[int, int] | None
    down_mean_ratios: tuple[float, ...]
    up_mean_monotonic: bool


@dataclass(frozen=True)
class TrailDeltaMetrics:
    energy: int
    coverage_pixels: int
    centroid_x: float
    centroid_y: float
    chromaticity: tuple[float, float, float]
    bounds: tuple[int, int, int, int]
    negative_energy: int


@dataclass(frozen=True)
class TrailDeltaResult:
    metrics: TrailDeltaMetrics
    failures: tuple[str, ...]
    weak_tail_metrics: TrailDeltaMetrics | None = None


@dataclass(frozen=True)
class DragCaseResult:
    interaction: TrailDeltaResult
    trail_only: TrailDeltaResult
    layers: LayerMetrics | None
    passed_layers: bool | None


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


def _linear_to_srgb_preview_byte(linear: float) -> int:
    if not math.isfinite(linear):
        raise ValidationError("FP16 preview source contains non-finite RGB")
    clamped = min(max(linear, 0.0), 1.0)
    encoded = (
        clamped * 12.92
        if clamped <= 0.0031308
        else 1.055 * clamped ** (1.0 / 2.4) - 0.055
    )
    # C++ lround rounds positive half values away from zero.
    return math.floor(encoded * 255.0 + 0.5)


def _read_bound_preview(directory: Path, name: str) -> Image8:
    image = _read_png(directory / f"{name}.png")
    if (image.width, image.height) != (CAPTURE_WIDTH, CAPTURE_HEIGHT):
        raise ValidationError(f"{name} PNG must be {CAPTURE_WIDTH}x{CAPTURE_HEIGHT}")
    raw_path = directory / f"{name}.rgba16f"
    for pixel_index, pixel in enumerate(
        _iter_half_pixels(raw_path, image.width, image.height)
    ):
        rgb_offset = pixel_index * 3
        for channel in range(3):
            expected = _linear_to_srgb_preview_byte(pixel[channel])
            actual = image.rgb[rgb_offset + channel]
            if actual != expected:
                raise ValidationError(
                    f"{name} PNG does not match its FP16 source at pixel "
                    f"{pixel_index}, channel {channel}"
                )
    return image


def _trail_delta_metrics(
    with_trail: Image8,
    no_trail: Image8,
    signal_threshold: int = NON_BLACK_THRESHOLD,
    signal_only: bool = False,
) -> TrailDeltaMetrics:
    if (with_trail.width, with_trail.height) != (no_trail.width, no_trail.height):
        raise ValidationError("WithTrail/NoTrail dimensions differ")
    expected_rgb_bytes = with_trail.width * with_trail.height * 3
    if (
        len(with_trail.rgb) != expected_rgb_bytes
        or len(no_trail.rgb) != expected_rgb_bytes
    ):
        raise ValidationError("paired PNG storage does not match dimensions")
    if type(signal_threshold) is not int or signal_threshold < 0:
        raise ValidationError("Trail signal threshold must be a non-negative integer")
    if type(signal_only) is not bool:
        raise ValidationError("Trail signal-only flag must be a boolean")

    channel_energy = [0, 0, 0]
    negative_energy = 0
    coverage_pixels = 0
    weighted_x = 0
    weighted_y = 0
    weight = 0
    minimum_x = with_trail.width
    minimum_y = with_trail.height
    maximum_x = -1
    maximum_y = -1
    for byte_index in range(0, len(with_trail.rgb), 3):
        pixel_index = byte_index // 3
        x = pixel_index % with_trail.width
        y = pixel_index // with_trail.width
        positive = []
        for channel in range(3):
            delta = with_trail.rgb[byte_index + channel] - no_trail.rgb[byte_index + channel]
            positive.append(max(delta, 0))
            negative_energy += max(-delta, 0)
        is_signal = max(positive) > signal_threshold
        if not signal_only or is_signal:
            for channel in range(3):
                channel_energy[channel] += positive[channel]
            pixel_weight = sum(positive)
            if pixel_weight > 0:
                weight += pixel_weight
                weighted_x += x * pixel_weight
                weighted_y += y * pixel_weight
        if is_signal:
            coverage_pixels += 1
            minimum_x = min(minimum_x, x)
            minimum_y = min(minimum_y, y)
            maximum_x = max(maximum_x, x)
            maximum_y = max(maximum_y, y)

    energy = sum(channel_energy)
    if energy <= 0 or coverage_pixels <= 0 or maximum_x < minimum_x:
        raise ValidationError("WithTrail/NoTrail pair has no positive Trail delta")
    return TrailDeltaMetrics(
        energy=energy,
        coverage_pixels=coverage_pixels,
        centroid_x=weighted_x / weight,
        centroid_y=weighted_y / weight,
        chromaticity=tuple(value / energy for value in channel_energy),
        bounds=(minimum_x, minimum_y, maximum_x, maximum_y),
        negative_energy=negative_energy,
    )


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
        "BloomResult",
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
    bloom_result_path = age_directory / "BloomResult.rgba16f"
    final_path = age_directory / "FinalOverlay.rgba16f"
    width, height = dimensions["DirectSurface"]
    if (
        dimensions["BloomSeed"] != (width, height)
        or dimensions["BloomResult"] != (width, height)
        or dimensions["FinalOverlay"] != (width, height)
    ):
        raise ValidationError(f"{age_directory.name}: full-resolution layer dimensions differ")

    direct_sum = 0.0
    seed_sum = 0.0
    final_sum = 0.0
    bloom_delta_sum = 0.0
    bloom_result_sum = 0.0
    direct_seed_min = math.inf
    final_direct_min = math.inf
    final_direct_alpha_min = math.inf
    direct_seed_alpha_min = math.inf
    composite_rgb_max_absolute_error = 0.0
    composite_rgb_absolute_error_sum = 0.0
    composite_rgb_max_tolerance_ratio = 0.0
    composite_rgb_first_failure = None
    composite_alpha_max_absolute_error = 0.0
    composite_alpha_first_failure = None
    finite = True
    non_negative = True
    direct_iter = _iter_half_pixels(direct_path, width, height)
    seed_iter = _iter_half_pixels(seed_path, width, height)
    bloom_result_iter = _iter_half_pixels(bloom_result_path, width, height)
    final_iter = _iter_half_pixels(final_path, width, height)
    for pixel_index, (direct, seed, bloom_result, final) in enumerate(
        zip(direct_iter, seed_iter, bloom_result_iter, final_iter)
    ):
        if not all(
            math.isfinite(value)
            for value in (*direct, *seed, *bloom_result, *final)
        ):
            finite = False
        if min(direct[:3] + seed[:3] + bloom_result[:3] + final[:3]) < -0.002:
            non_negative = False
        direct_sum += sum(direct[:3])
        seed_sum += sum(seed[:3])
        bloom_result_sum += sum(bloom_result[:3])
        final_sum += sum(final[:3])
        for channel in range(3):
            direct_seed_min = min(direct_seed_min, direct[channel] - seed[channel])
            final_direct_min = min(final_direct_min, final[channel] - direct[channel])
            expected = direct[channel] + bloom_result[channel]
            absolute_error = abs(final[channel] - expected)
            magnitude = max(abs(final[channel]), abs(expected))
            tolerance = FP16_LAYER_TOLERANCE \
                + COMPOSITE_RGB_RELATIVE_TOLERANCE * magnitude
            tolerance_ratio = absolute_error / tolerance
            composite_rgb_max_absolute_error = max(
                composite_rgb_max_absolute_error,
                absolute_error,
            )
            composite_rgb_absolute_error_sum += absolute_error
            composite_rgb_max_tolerance_ratio = max(
                composite_rgb_max_tolerance_ratio,
                tolerance_ratio,
            )
            if composite_rgb_first_failure is None and tolerance_ratio > 1.0:
                composite_rgb_first_failure = (
                    pixel_index % width,
                    pixel_index // width,
                    channel,
                )
        final_direct_alpha_min = min(
            final_direct_alpha_min,
            final[3] - direct[3],
        )
        direct_seed_alpha_min = min(
            direct_seed_alpha_min,
            direct[3] - seed[3],
        )
        composite_alpha_error = abs(
            final[3] - max(direct[3], bloom_result[3])
        )
        composite_alpha_max_absolute_error = max(
            composite_alpha_max_absolute_error,
            composite_alpha_error,
        )
        if (
            composite_alpha_first_failure is None
            and composite_alpha_error > FP16_LAYER_TOLERANCE
        ):
            composite_alpha_first_failure = (
                pixel_index % width,
                pixel_index // width,
            )
        bloom_delta_sum += sum(final[channel] - direct[channel] for channel in range(3))

    up_width, up_height = dimensions["Up00"]
    up00_sum, up_finite, up_non_negative, _ = _sum_layer(
        age_directory / "Up00.rgba16f", up_width, up_height
    )
    finite = finite and up_finite
    non_negative = non_negative and up_non_negative
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
        bloom_result_rgb_sum=bloom_result_sum,
        up00_rgb_sum=up00_sum,
        composite_rgb_max_absolute_error=composite_rgb_max_absolute_error,
        composite_rgb_mean_absolute_error=(
            composite_rgb_absolute_error_sum / (width * height * 3)
        ),
        composite_rgb_max_tolerance_ratio=composite_rgb_max_tolerance_ratio,
        composite_rgb_first_failure=composite_rgb_first_failure,
        composite_alpha_max_absolute_error=composite_alpha_max_absolute_error,
        composite_alpha_first_failure=composite_alpha_first_failure,
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
    if layers.composite_rgb_first_failure is not None:
        failures.append(
            "FinalOverlay RGB differs from DirectSurface + BloomResult: "
            f"maxAbs={layers.composite_rgb_max_absolute_error:.6g}, "
            f"meanAbs={layers.composite_rgb_mean_absolute_error:.6g}, "
            f"maxToleranceRatio={layers.composite_rgb_max_tolerance_ratio:.6g}, "
            f"first={layers.composite_rgb_first_failure}"
        )
    if layers.composite_alpha_first_failure is not None:
        failures.append(
            "FinalOverlay Alpha differs from max(DirectSurface, BloomResult): "
            f"maxAbs={layers.composite_alpha_max_absolute_error:.6g}, "
            f"first={layers.composite_alpha_first_failure}"
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


def _trail_delta_failures(metrics: TrailDeltaMetrics) -> tuple[str, ...]:
    """Evaluate the interaction pair using its high-signal Trail body."""
    failures = []
    if metrics.negative_energy != 0:
        failures.append(
            "WithTrail is darker than NoTrail: "
            f"negativeEnergy={metrics.negative_energy}"
        )
    energy_error = _relative_error(
        metrics.energy,
        TRAIL_DELTA_REFERENCE_ENERGY,
    )
    if energy_error > TRAIL_DELTA_ENERGY_RELATIVE_TOLERANCE:
        failures.append(f"Trail delta energy differs from Unity: error={energy_error:.1%}")
    coverage_error = _relative_error(
        metrics.coverage_pixels,
        TRAIL_DELTA_REFERENCE_COVERAGE,
    )
    if coverage_error > TRAIL_DELTA_COVERAGE_RELATIVE_TOLERANCE:
        failures.append(
            f"Trail delta coverage differs from Unity: error={coverage_error:.1%}"
        )
    centroid_delta = (
        abs(metrics.centroid_x - TRAIL_DELTA_REFERENCE_CENTROID[0]),
        abs(metrics.centroid_y - TRAIL_DELTA_REFERENCE_CENTROID[1]),
    )
    if any(
        delta > tolerance
        for delta, tolerance in zip(
            centroid_delta,
            TRAIL_DELTA_CENTROID_TOLERANCE,
        )
    ):
        failures.append(
            "Trail delta centroid differs from Unity: "
            f"dx={centroid_delta[0]:.2f}px dy={centroid_delta[1]:.2f}px"
        )
    chromaticity_error = sum(
        abs(actual - expected)
        for actual, expected in zip(
            metrics.chromaticity,
            TRAIL_DELTA_REFERENCE_CHROMATICITY,
        )
    )
    if chromaticity_error > TRAIL_DELTA_CHROMATICITY_L1_TOLERANCE:
        failures.append(
            "Trail delta chromaticity differs from Unity: "
            f"l1={chromaticity_error:.4f}"
        )
    for actual, expected in zip(metrics.bounds, TRAIL_DELTA_REFERENCE_BOUNDS):
        if abs(actual - expected) > TRAIL_DELTA_BOUNDS_TOLERANCE_PX:
            failures.append(
                "Trail delta bounds differ from Unity: "
                f"actual={metrics.bounds} expected={TRAIL_DELTA_REFERENCE_BOUNDS}"
            )
            break
    return tuple(failures)


def _trail_only_failures(metrics: TrailDeltaMetrics) -> tuple[str, ...]:
    """Validate the isolated Trail material without cross-RNG particle overlap."""
    failures = []
    if metrics.negative_energy != 0:
        failures.append(
            "Trail-only frame contains negative energy: "
            f"negativeEnergy={metrics.negative_energy}"
        )
    energy_error = _relative_error(metrics.energy, TRAIL_ONLY_REFERENCE_ENERGY)
    if energy_error > TRAIL_ONLY_ENERGY_RELATIVE_TOLERANCE:
        failures.append(
            f"Trail-only energy differs from Unity: error={energy_error:.1%}"
        )
    coverage_error = _relative_error(
        metrics.coverage_pixels,
        TRAIL_ONLY_REFERENCE_COVERAGE,
    )
    if coverage_error > TRAIL_ONLY_COVERAGE_RELATIVE_TOLERANCE:
        failures.append(
            f"Trail-only coverage differs from Unity: error={coverage_error:.1%}"
        )
    centroid_delta = (
        abs(metrics.centroid_x - TRAIL_ONLY_REFERENCE_CENTROID[0]),
        abs(metrics.centroid_y - TRAIL_ONLY_REFERENCE_CENTROID[1]),
    )
    if any(delta > TRAIL_ONLY_CENTROID_TOLERANCE_PX for delta in centroid_delta):
        failures.append(
            "Trail-only centroid differs from Unity: "
            f"dx={centroid_delta[0]:.2f}px dy={centroid_delta[1]:.2f}px"
        )
    chromaticity_error = sum(
        abs(actual - expected)
        for actual, expected in zip(
            metrics.chromaticity,
            TRAIL_ONLY_REFERENCE_CHROMATICITY,
        )
    )
    if chromaticity_error > TRAIL_ONLY_CHROMATICITY_L1_TOLERANCE:
        failures.append(
            "Trail-only chromaticity differs from Unity: "
            f"l1={chromaticity_error:.4f}"
        )
    for actual, expected in zip(metrics.bounds, TRAIL_ONLY_REFERENCE_BOUNDS):
        if abs(actual - expected) > TRAIL_ONLY_BOUNDS_TOLERANCE_PX:
            failures.append(
                "Trail-only bounds differ from Unity: "
                f"actual={metrics.bounds} expected={TRAIL_ONLY_REFERENCE_BOUNDS}"
            )
            break
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


def _require_object(value: object, label: str) -> dict:
    if not isinstance(value, dict):
        raise ValidationError(f"{label} must be an object")
    return value


def _require_list(value: object, label: str) -> list:
    if not isinstance(value, list):
        raise ValidationError(f"{label} must be an array")
    return value


def _require_exact_int(container: dict, key: str, expected: int, label: str) -> int:
    value = container.get(key)
    # bool subclasses int in Python, but JSON true is never a valid dimension.
    if type(value) is not int or value != expected:
        raise ValidationError(f"{label}.{key} must be {expected}")
    return value


def _require_string(container: dict, key: str, expected: str, label: str) -> str:
    value = container.get(key)
    if type(value) is not str or value != expected:
        raise ValidationError(f"{label}.{key} must be {expected!r}")
    return value


def _require_finite_number(container: dict, key: str, label: str) -> float:
    value = container.get(key)
    if type(value) not in (int, float) or not math.isfinite(value):
        raise ValidationError(f"{label}.{key} must be a finite number")
    return float(value)


def _validate_layer_records(records: object, label: str) -> frozenset[str]:
    layers = _require_list(records, label)
    names = set()
    for index, value in enumerate(layers):
        layer_label = f"{label}[{index}]"
        layer = _require_object(value, layer_label)
        name = layer.get("name")
        if type(name) is not str or not name:
            raise ValidationError(f"{layer_label}.name must be a non-empty string")
        if name in names:
            raise ValidationError(f"{label} contains duplicate layer {name!r}")
        names.add(name)
        alpha_semantic = layer.get("alphaSemantic")
        expected_alpha_semantic = LAYER_ALPHA_SEMANTICS.get(name)
        if (
            type(alpha_semantic) is not str
            or alpha_semantic != expected_alpha_semantic
        ):
            raise ValidationError(
                f"{layer_label}.alphaSemantic differs from the layer contract"
            )
        width = layer.get("width")
        height = layer.get("height")
        raw_bytes = layer.get("rawBytes")
        expected_extent = EXPECTED_LAYER_EXTENTS.get(name)
        if (
            type(width) is not int
            or type(height) is not int
            or type(raw_bytes) is not int
            or expected_extent is None
            or (width, height) != expected_extent
            or raw_bytes != width * height * 8
        ):
            raise ValidationError(f"{layer_label} has invalid FP16 extent or byte count")
    return frozenset(names)


def _require_file_size(path: Path, expected: int) -> None:
    try:
        actual = path.stat().st_size
    except OSError as error:
        raise ValidationError(f"unable to inspect required artifact {path}: {error}") from error
    if actual != expected:
        raise ValidationError(
            f"artifact byte count mismatch for {path}: expected {expected}, got {actual}"
        )


def _validate_drag_manifest(root: Path, manifest: dict, age_record: dict) -> None:
    case = _require_object(manifest["case"], "native manifest case")
    _require_exact_int(case, "contractVersion", 1, "native manifest case")
    _require_exact_int(case, "movementPixels", 432, "native manifest case")
    _require_exact_int(case, "movementSteps", 12, "native manifest case")
    _require_exact_int(case, "trailOnlyPixels", 20, "native manifest case")
    _require_string(
        case,
        "trailFixture",
        "two-endpoint-unity-editor-diagnostic",
        "native manifest case",
    )
    references = _require_object(
        case.get("unityReference"),
        "native manifest case.unityReference",
    )
    _require_string(
        references,
        "withTrail",
        DRAG_WITH_TRAIL_REFERENCE,
        "native manifest case.unityReference",
    )
    _require_string(
        references,
        "noTrail",
        DRAG_NO_TRAIL_REFERENCE,
        "native manifest case.unityReference",
    )
    _require_string(
        references,
        "trailOnly",
        TRAIL_ONLY_REFERENCE,
        "native manifest case.unityReference",
    )

    frames = _require_list(
        age_record.get("comparisonFrames"),
        "native drag-trail comparisonFrames",
    )
    expected_names = {
        "FinalOverlay_NoTrail",
        "FinalOverlay_TrailOnly20px",
    }
    if len(frames) != len(expected_names):
        raise ValidationError(
            "native drag-trail comparisonFrames must contain exactly two frames"
        )
    frame_names = []
    for index, value in enumerate(frames):
        label = f"native drag-trail comparisonFrames[{index}]"
        frame = _require_object(value, label)
        name = frame.get("name")
        if type(name) is not str:
            raise ValidationError(f"{label}.name must be a string")
        frame_names.append(name)
        _require_string(frame, "alphaSemantic", "coverage-union", label)
        _require_exact_int(frame, "width", CAPTURE_WIDTH, label)
        _require_exact_int(frame, "height", CAPTURE_HEIGHT, label)
        _require_exact_int(frame, "rawBytes", CAPTURE_RGBA16F_BYTES, label)
    if set(frame_names) != expected_names or len(set(frame_names)) != len(frame_names):
        raise ValidationError(
            "native drag-trail comparisonFrames names differ from the locked fixture"
        )
    age_directory = root / "0140ms"
    for name in expected_names:
        _require_file_size(
            age_directory / f"{name}.rgba16f",
            CAPTURE_RGBA16F_BYTES,
        )


def _load_manifest(root: Path) -> dict:
    path = root / "manifest.json"
    if not path.is_file():
        raise ValidationError(f"native manifest not found: {path}")
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValidationError(f"invalid native manifest {path}: {error}") from error
    manifest = _require_object(manifest, "native manifest")
    _require_exact_int(manifest, "schemaVersion", 3, "native manifest")
    _require_string(manifest, "driver", "WARP", "native manifest")
    _require_string(manifest, "captureProfile", "fx-only", "native manifest")
    _require_string(
        manifest,
        "compositeFormula",
        "direct-plus-bloom-result-max-alpha-v1",
        "native manifest",
    )
    _require_exact_int(manifest, "seed", 20260716, "native manifest")
    _require_string(manifest, "rowOrigin", "top-left", "native manifest")
    if type(manifest.get("allLayers")) is not bool:
        raise ValidationError("native manifest allLayers must be a boolean")

    viewport = _require_object(manifest.get("viewport"), "native manifest viewport")
    _require_exact_int(viewport, "width", CAPTURE_WIDTH, "native manifest viewport")
    _require_exact_int(viewport, "height", CAPTURE_HEIGHT, "native manifest viewport")

    bloom = _require_object(manifest.get("bloom"), "native manifest bloom")
    _require_exact_int(bloom, "mipCount", 6, "native manifest bloom")
    sample_scale = _require_finite_number(
        bloom,
        "sampleScale",
        "native manifest bloom",
    )
    if abs(sample_scale - SAMPLE_SCALE) > 1.0e-6:
        raise ValidationError("native manifest Bloom sampleScale differs from Unity")
    exposure_gain = _require_finite_number(
        bloom,
        "exposureGain",
        "native manifest bloom",
    )
    if abs(exposure_gain - EXPOSURE_GAIN) > 1.0e-7:
        raise ValidationError("native manifest Bloom exposureGain differs from Unity")

    case_name = "click"
    if "case" in manifest:
        case = _require_object(manifest["case"], "native manifest case")
        case_name_value = case.get("name")
        if type(case_name_value) is not str:
            raise ValidationError("native manifest case.name must be a string")
        case_name = case_name_value
    if case_name not in ("click", "drag-trail"):
        raise ValidationError(f"native manifest case is unsupported: {case_name}")

    ages = _require_list(manifest.get("ages"), "native manifest ages")
    age_records = []
    manifest_ages = []
    for index, value in enumerate(ages):
        label = f"native manifest ages[{index}]"
        age_record = _require_object(value, label)
        age = age_record.get("ageMs")
        if type(age) is not int:
            raise ValidationError(f"{label}.ageMs must be an integer")
        layer_names = _validate_layer_records(
            age_record.get("layers"),
            f"{label}.layers",
        )
        expected_layer_names = (
            ALL_LAYER_NAMES if manifest["allLayers"] else FINAL_ONLY_LAYER_NAMES
        )
        if layer_names != expected_layer_names:
            raise ValidationError(
                f"{label}.layers differ from the declared allLayers contract"
            )
        age_records.append(age_record)
        manifest_ages.append(age)
    required_ages = AGES if case_name == "click" else (140,)
    if sorted(manifest_ages) != list(required_ages):
        raise ValidationError(
            f"native {case_name} manifest must contain each locked age exactly once: "
            + ", ".join(str(age) for age in required_ages)
        )
    if case_name == "drag-trail":
        _validate_drag_manifest(root, manifest, age_records[0])
    return manifest


def _black_image_like(image: Image8) -> Image8:
    return Image8(image.width, image.height, bytes(len(image.rgb)))


def _compare_drag_case(
    unity_root: Path,
    native_root: Path,
    check_layers: bool,
    manifest: dict,
) -> DragCaseResult:
    unity_interaction_root = unity_root.parent / "Diagnostics" / "Interaction"
    unity_with = _read_png(unity_interaction_root / Path(DRAG_WITH_TRAIL_REFERENCE).name)
    unity_without = _read_png(unity_interaction_root / Path(DRAG_NO_TRAIL_REFERENCE).name)
    unity_metrics = _trail_delta_metrics(
        unity_with,
        unity_without,
        TRAIL_SIGNAL_THRESHOLD,
        signal_only=True,
    )
    # The constants above are deliberately duplicated from the locked external
    # evidence so a changed reference cannot silently redefine its own gate.
    if _trail_delta_failures(unity_metrics):
        raise ValidationError("locked Unity drag-trail pair differs from recorded metrics")

    directory = _find_age_directory(native_root, 140)
    native_with = _read_bound_preview(directory, "FinalOverlay")
    native_without = _read_bound_preview(directory, "FinalOverlay_NoTrail")
    native_metrics = _trail_delta_metrics(
        native_with,
        native_without,
        TRAIL_SIGNAL_THRESHOLD,
        signal_only=True,
    )
    interaction = TrailDeltaResult(
        metrics=native_metrics,
        failures=_trail_delta_failures(native_metrics),
        weak_tail_metrics=_trail_delta_metrics(native_with, native_without),
    )

    unity_trail_only = _read_png(
        unity_root.parent / "Diagnostics" / "Trail" / Path(TRAIL_ONLY_REFERENCE).name
    )
    unity_trail_only_metrics = _trail_delta_metrics(
        unity_trail_only,
        _black_image_like(unity_trail_only),
    )
    if _trail_only_failures(unity_trail_only_metrics):
        raise ValidationError("locked Unity Trail-only frame differs from recorded metrics")
    native_trail_only = _read_bound_preview(
        directory,
        "FinalOverlay_TrailOnly20px",
    )
    native_trail_only_metrics = _trail_delta_metrics(
        native_trail_only,
        _black_image_like(native_trail_only),
    )
    trail_only = TrailDeltaResult(
        metrics=native_trail_only_metrics,
        failures=_trail_only_failures(native_trail_only_metrics),
    )

    layers = None
    passed_layers = None
    if check_layers:
        age_record = manifest["ages"][0]
        layer_info = {entry["name"]: entry for entry in age_record["layers"]}
        layers = _layer_metrics(directory, layer_info)
        passed_layers = not _layer_contract_failures(layers)
    return DragCaseResult(
        interaction=interaction,
        trail_only=trail_only,
        layers=layers,
        passed_layers=passed_layers,
    )


def _compare_slice(age_ms: int, unity_root: Path, native_root: Path, check_layers: bool, manifest: dict) -> SliceResult:
    golden_path = unity_root / f"FX_Touch_{age_ms:04d}ms.png"
    native_directory = _find_age_directory(native_root, age_ms)
    golden = _display_metrics(_read_png(golden_path))
    native = _display_metrics(
        _read_bound_preview(native_directory, "FinalOverlay")
    )
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


def _format_trail_result(label: str, result: TrailDeltaResult) -> str:
    metrics = result.metrics
    status = "PASS" if not result.failures else "FAIL"
    return (
        f"{status} {label} energy={metrics.energy} "
        f"coverage={metrics.coverage_pixels} "
        f"centroid=({metrics.centroid_x:.2f},{metrics.centroid_y:.2f}) "
        f"chroma=({metrics.chromaticity[0]:.4f},"
        f"{metrics.chromaticity[1]:.4f},{metrics.chromaticity[2]:.4f}) "
        f"bounds={metrics.bounds} negativeEnergy={metrics.negative_energy}"
    )


def _threshold_failures(
    results: list[SliceResult],
    drag_result: DragCaseResult | None,
) -> list[str]:
    failures = []
    for result in results:
        if not result.passed_display:
            failures.append(f"display threshold failure at {result.age_ms}ms")
        if result.passed_layers is False:
            details = "; ".join(_layer_contract_failures(result.layers))
            failures.append(
                f"layer threshold failure at {result.age_ms}ms: {details}"
            )
    if drag_result is not None:
        failures.extend(
            f"trail threshold failure: {failure}"
            for failure in drag_result.interaction.failures
        )
        failures.extend(
            f"Trail-only threshold failure: {failure}"
            for failure in drag_result.trail_only.failures
        )
        if drag_result.passed_layers is False:
            details = "; ".join(_layer_contract_failures(drag_result.layers))
            failures.append(f"drag-trail layer threshold failure: {details}")
    return failures


def _json_report(
    case_name: str | None,
    passed: bool,
    results: list[SliceResult],
    drag_result: DragCaseResult | None,
    errors: list[str],
    error_kind: str | None,
) -> dict:
    return {
        "schemaVersion": 1,
        "case": case_name,
        "passed": passed,
        "errorKind": error_kind,
        "results": {
            "slices": [asdict(result) for result in results],
            "drag": asdict(drag_result) if drag_result is not None else None,
        },
        "errors": errors,
    }


def _json_safe(value: object) -> object:
    if isinstance(value, float) and not math.isfinite(value):
        return None
    if isinstance(value, dict):
        return {key: _json_safe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(item) for item in value]
    return value


def _write_json(report: dict) -> None:
    print(json.dumps(_json_safe(report), indent=2, allow_nan=False))


def main(argv: list[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    json_requested = "--json" in arguments
    parser = GoldenArgumentParser(description=__doc__)
    parser.add_argument(
        "--unity-captures",
        type=Path,
        default=None,
        help=(
            "directory containing FX_Touch_XXXXms.png; drag diagnostics are "
            "resolved from the sibling Diagnostics/Interaction directory"
        ),
    )
    parser.add_argument(
        "--native-root",
        type=Path,
        default=Path("artifacts/local/gpu-captures/native"),
        help="native capture directory containing manifest.json and age folders",
    )
    layer_options = parser.add_mutually_exclusive_group()
    layer_options.add_argument(
        "--no-layers",
        action="store_true",
        help="skip FP16 intermediate-layer checks even when manifest has allLayers=true",
    )
    layer_options.add_argument(
        "--require-layers",
        action="store_true",
        help="fail if the native manifest does not contain all intermediate layers",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="emit a JSON report after the human-readable lines",
    )
    try:
        args = parser.parse_args(arguments)
    except ArgumentValidationError as error:
        if json_requested:
            _write_json(_json_report(
                None,
                False,
                [],
                None,
                [str(error)],
                "arguments",
            ))
        else:
            parser.print_usage(sys.stderr)
            print(f"{parser.prog}: error: {error}", file=sys.stderr)
        return 2
    unity_root = (args.unity_captures or _default_unity_root()).resolve()
    native_root = args.native_root.resolve()
    case_name = None
    try:
        manifest = _load_manifest(native_root)
        manifest_has_layers = bool(manifest.get("allLayers", False))
        if args.require_layers and not manifest_has_layers:
            raise ValidationError("--require-layers requires a manifest captured with --all-layers")
        check_layers = manifest_has_layers and not args.no_layers
        case_name = manifest.get("case", {}).get("name", "click")
        if case_name == "click":
            results = [
                _compare_slice(age, unity_root, native_root, check_layers, manifest)
                for age in AGES
            ]
            drag_result = None
        else:
            results = []
            drag_result = _compare_drag_case(
                unity_root,
                native_root,
                check_layers,
                manifest,
            )
    except ValidationError as error:
        if args.json:
            _write_json(_json_report(
                case_name,
                False,
                [],
                None,
                [str(error)],
                "input",
            ))
        else:
            print(f"golden-metrics: ERROR: {error}", file=sys.stderr)
        return 2

    failures = _threshold_failures(results, drag_result)
    passed = not failures
    if args.json:
        _write_json(_json_report(
            case_name,
            passed,
            results,
            drag_result,
            failures,
            None if passed else "threshold",
        ))
        return 0 if passed else 1

    for result in results:
        print(_format_result(result))
    if drag_result is not None:
        print(_format_trail_result("drag-trail-delta", drag_result.interaction))
        weak = drag_result.interaction.weak_tail_metrics
        if weak is not None:
            print(
                "INFO drag-trail-weak-tail "
                f"threshold>{NON_BLACK_THRESHOLD} "
                f"coverage={weak.coverage_pixels} bounds={weak.bounds}"
            )
        print(_format_trail_result("trail-only-20px", drag_result.trail_only))
    if not passed:
        for failure in failures:
            print(f"golden-metrics: {failure}", file=sys.stderr)
        return 1
    print(
        (
            f"Golden metrics passed: {len(results)} slices"
            if drag_result is None
            else "Golden metrics passed: drag interaction + isolated Trail"
        )
        + (" + FP16 layers" if check_layers else "")
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
