#!/usr/bin/env python3
"""Validate SPK-002 monitor-WGC WDA self-exclusion pixel evidence."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
import math
from pathlib import Path
import struct
import sys
from typing import Any


EXPECTED_SCHEMA = 1
EXPECTED_SPIKE = "SPK-002-WDA-SELF-EXCLUSION"
EXPECTED_SCOPE = "controlled-monitor-WDA-self-exclusion-pixels-only"
EXPECTED_CAPTURE_TARGET = "MONITOR"
EXPECTED_SURFACE_FORMAT = "DXGI_FORMAT_R16G16B16A16_FLOAT"
EXPECTED_CAPTURE_SIZE = (640, 320)
EXPECTED_OVERLAY_WINDOW = (32, 32, 256, 256)
EXPECTED_OVERLAY_ROI = (64, 64, 192, 192)
EXPECTED_CONTROL_ROI = (416, 64, 192, 192)
EXPECTED_MARKER_ROI = (328, 128, 64, 64)
EXPECTED_MARKER_REFERENCE_ROI = (416, 64, 64, 64)
EXPECTED_BACKGROUND_SRGB8 = (30, 82, 146)
EXPECTED_PROBE_LINEAR = (0.82, 0.16, 0.52, 1.0)
EXPECTED_MARKER_SRGB8 = {
    "includedBefore": (224, 48, 64),
    "excluded": (48, 220, 80),
    "includedAfter": (240, 208, 48),
}
EXPECTED_MAXIMUM_STABLE_SAMPLE_ATTEMPTS = 4
EXPECTED_DIFFERENCE_THRESHOLD = 0.02
EXPECTED_MINIMUM_OVERLAY_DIFFERENT_FRACTION = 0.95
EXPECTED_MINIMUM_OVERLAY_RGB_DELTA = 0.20
EXPECTED_MAXIMUM_STABLE_RGB_DELTA = 0.01
EXPECTED_MAXIMUM_EXCLUDED_OVERLAY_RGB_RANGE = 0.01
EXPECTED_MAXIMUM_INCLUDED_STABILITY_RGB_DELTA = 0.01
EXPECTED_MAXIMUM_CONTROL_RGB_DELTA = 0.01
EXPECTED_MAXIMUM_EXCLUDED_BACKGROUND_DELTA = 0.01
EXPECTED_MAXIMUM_MARKER_RGB_RANGE = 0.01
EXPECTED_MINIMUM_MARKER_RGB_DELTA = 0.10
EXPECTED_MARKER_CHANNEL_MARGIN = 0.05
EXPECTED_WDA_NONE = 0
EXPECTED_WDA_EXCLUDEFROMCAPTURE = 0x11
EXPECTED_OVERLAY_STYLE_MASK = 0x00080020
FLOAT_TOLERANCE = 1.0e-5

ROOT_FIELDS = {
    "schemaVersion",
    "spikeId",
    "applicationVersion",
    "revision",
    "capturedAtUtc",
    "timeoutMs",
    "contract",
    "osVersion",
    "rendererDevice",
    "observerFeatureLevel",
    "fixture",
    "wgcCapabilities",
    "observations",
    "metrics",
    "resourceLedger",
}
CONTRACT_FIELDS = {
    "scope",
    "captureTarget",
    "surfaceFormat",
    "sessionTopology",
    "validatesProductStopWdaStartTransaction",
    "systemBorderAllowed",
    "cursorCaptureEnabled",
    "frameMarkerSemantic",
    "maximumStableSampleAttempts",
    "stableSampleTolerance",
    "differenceThreshold",
    "maximumControlDelta",
    "maximumIncludedDelta",
    "maximumExcludedRange",
    "maximumExcludedBackgroundDelta",
    "minimumOverlayDelta",
    "maximumMarkerRange",
    "minimumMarkerDelta",
    "markerChannelMargin",
    "minimumChangedFraction",
}
OS_FIELDS = {"major", "minor", "build"}
DEVICE_FIELDS = {
    "driverType",
    "adapter",
    "adapterLuid",
    "vendorId",
    "deviceId",
    "driverVersion",
    "featureLevel",
}
FIXTURE_FIELDS = {
    "monitorBounds",
    "captureScreenBounds",
    "overlayScreenBounds",
    "captureRegion",
    "overlayRoi",
    "controlRoi",
    "markerRoi",
    "markerReferenceRoi",
    "backgroundSrgb8",
    "markerColorsSrgb8",
    "probeLinear",
}
WGC_CAPABILITY_FIELDS = {
    "borderHidden",
    "cursorExcluded",
    "cursorCaptureEnabled",
    "cursorControlConfirmed",
}
OBSERVATION_FIELDS = {
    "requestedExcluded",
    "affinity",
    "observedExtendedStyle",
    "layeredStyleRestored",
    "transparentStyleRestored",
    "markerSrgb8",
    "stablePair",
    "artifact",
}
AFFINITY_FIELDS = {
    "requested",
    "observed",
    "setSucceeded",
    "querySucceeded",
    "setError",
    "queryError",
    "confirmed",
}
STABLE_PAIR_FIELDS = {
    "first",
    "second",
    "maximumRgbDelta",
}
FRAME_FIELDS = {
    "previousGeneration",
    "generation",
    "markerNs",
    "capturedAtNs",
    "contentSize",
}
ARTIFACT_FIELDS = {"raw", "png", "width", "height", "rawBytes"}
COMPARISON_FIELDS = {"roi", "threshold", "maximumRgbDelta", "differentPixels"}
SPATIAL_COMPARISON_FIELDS = {
    "leftRoi",
    "rightRoi",
    "threshold",
    "maximumRgbDelta",
    "differentPixels",
}
MARKER_METRIC_FIELDS = {"meanLinear", "rgbRange", "vsReference"}
METRICS_FIELDS = {
    "includedBeforeVsExcluded",
    "includedAfterVsExcluded",
    "includedBeforeVsAfter",
    "markerIncludedBeforeVsExcluded",
    "markerExcludedVsIncludedAfter",
    "markerIncludedBeforeVsAfter",
    "excludedOverlayVsControl",
    "markers",
    "excludedOverlayRgbRange",
    "controlMaximumRgbDelta",
}
REGION_FIELDS = {"left", "top", "width", "height"}
BOUNDS_FIELDS = {"left", "top", "right", "bottom"}
SIZE_FIELDS = {"width", "height"}
RGB_FIELDS = {"r", "g", "b"}
RGBA_FIELDS = {"r", "g", "b", "a"}
LEDGER_COUNT_FIELDS = {
    "framesAcquired",
    "framesClosed",
    "framePoolsCreated",
    "framePoolsClosed",
    "framePoolsRecreated",
    "sessionsCreated",
    "sessionsClosed",
    "frameArrivedRegistrations",
    "frameArrivedUnregistrations",
    "itemClosedRegistrations",
    "itemClosedUnregistrations",
    "liveFrames",
    "liveFramePools",
    "liveSessions",
    "liveFrameArrivedRegistrations",
    "liveItemClosedRegistrations",
    "failures",
}
LEDGER_FIELDS = LEDGER_COUNT_FIELDS | {"allReleased"}
LIVE_LEDGER_FIELDS = {
    "liveFrames",
    "liveFramePools",
    "liveSessions",
    "liveFrameArrivedRegistrations",
    "liveItemClosedRegistrations",
}
LEDGER_PAIRS = (
    ("framesAcquired", "framesClosed"),
    ("framePoolsCreated", "framePoolsClosed"),
    ("sessionsCreated", "sessionsClosed"),
    ("frameArrivedRegistrations", "frameArrivedUnregistrations"),
    ("itemClosedRegistrations", "itemClosedUnregistrations"),
)


class ValidationError(ValueError):
    """Raised when WDA evidence is malformed or fails acceptance."""


@dataclass(frozen=True)
class RawImage:
    width: int
    height: int
    pixels: tuple[tuple[float, float, float, float], ...]


@dataclass(frozen=True)
class PairMetrics:
    maximum_rgb_delta: float
    different_pixels: int


@dataclass(frozen=True)
class ObservationResult:
    first_generation: int
    generation: int
    first_marker_nanoseconds: int
    captured_at_nanoseconds: int
    image: RawImage


@dataclass(frozen=True)
class VerificationResult:
    schema_version: int
    spike_id: str
    capture_revision: str
    status: str
    included_before_different_pixels: int
    included_after_different_pixels: int
    maximum_overlay_rgb_delta: float
    included_stability_maximum_rgb_delta: float
    excluded_overlay_rgb_range: float
    excluded_background_maximum_rgb_delta: float
    excluded_background_different_pixels: int
    control_maximum_rgb_delta: float
    minimum_marker_pair_rgb_delta: float
    minimum_marker_pair_different_pixels: int
    frames_acquired: int


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValidationError(f"duplicate JSON field: {key}")
        result[key] = value
    return result


def _reject_nonfinite_constant(value: str) -> None:
    raise ValidationError(f"non-finite JSON number: {value}")


def _object(value: Any, label: str) -> dict[str, Any]:
    if type(value) is not dict:
        raise ValidationError(f"{label} must be an object")
    return value


def _require_fields(value: dict[str, Any], expected: set[str], label: str) -> None:
    missing = expected - value.keys()
    unknown = value.keys() - expected
    if missing:
        raise ValidationError(f"{label} missing fields: {sorted(missing)}")
    if unknown:
        raise ValidationError(f"{label} has unknown fields: {sorted(unknown)}")


def _boolean(value: Any, label: str) -> bool:
    if type(value) is not bool:
        raise ValidationError(f"{label} must be a boolean")
    return value


def _integer(value: Any, label: str, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        raise ValidationError(f"{label} must be an integer >= {minimum}")
    return value


def _number(value: Any, label: str, minimum: float = 0.0) -> float:
    if type(value) not in (int, float):
        raise ValidationError(f"{label} must be a finite number")
    result = float(value)
    if not math.isfinite(result) or result < minimum:
        raise ValidationError(f"{label} must be a finite number >= {minimum}")
    return result


def _string(value: Any, label: str) -> str:
    if type(value) is not str or not value:
        raise ValidationError(f"{label} must be a non-empty string")
    return value


def _close(actual: float, expected: float, label: str) -> None:
    if not math.isclose(
        actual,
        expected,
        rel_tol=FLOAT_TOLERANCE,
        abs_tol=FLOAT_TOLERANCE,
    ):
        raise ValidationError(f"{label} mismatch: recorded={actual}, raw={expected}")


def _size(value: Any, label: str) -> tuple[int, int]:
    source = _object(value, label)
    _require_fields(source, SIZE_FIELDS, label)
    return (
        _integer(source["width"], f"{label}.width", 1),
        _integer(source["height"], f"{label}.height", 1),
    )


def _region(value: Any, label: str) -> tuple[int, int, int, int]:
    source = _object(value, label)
    _require_fields(source, REGION_FIELDS, label)
    return (
        _integer(source["left"], f"{label}.left"),
        _integer(source["top"], f"{label}.top"),
        _integer(source["width"], f"{label}.width", 1),
        _integer(source["height"], f"{label}.height", 1),
    )


def _bounds(value: Any, label: str) -> tuple[int, int, int, int]:
    source = _object(value, label)
    _require_fields(source, BOUNDS_FIELDS, label)
    result = (
        _integer(source["left"], f"{label}.left", -(2**31)),
        _integer(source["top"], f"{label}.top", -(2**31)),
        _integer(source["right"], f"{label}.right", -(2**31)),
        _integer(source["bottom"], f"{label}.bottom", -(2**31)),
    )
    if result[2] <= result[0] or result[3] <= result[1]:
        raise ValidationError(f"{label} must have positive dimensions")
    return result


def _srgb8(value: Any, label: str) -> tuple[int, int, int]:
    if type(value) is not list or len(value) != 3:
        raise ValidationError(f"{label} must contain three channels")
    channels = tuple(
        _integer(channel, f"{label}[{index}]")
        for index, channel in enumerate(value)
    )
    if any(channel > 255 for channel in channels):
        raise ValidationError(f"{label} channels must be <= 255")
    return channels


def _linear_rgb(value: Any, label: str) -> tuple[float, float, float]:
    source = _object(value, label)
    _require_fields(source, RGB_FIELDS, label)
    return tuple(_number(source[name], f"{label}.{name}") for name in "rgb")


def _rgba(value: Any, label: str) -> tuple[float, float, float, float]:
    source = _object(value, label)
    _require_fields(source, RGBA_FIELDS, label)
    return tuple(_number(source[name], f"{label}.{name}") for name in "rgba")


def _region_inside(
    region: tuple[int, int, int, int],
    size: tuple[int, int],
    label: str,
) -> None:
    left, top, width, height = region
    if left + width > size[0] or top + height > size[1]:
        raise ValidationError(f"{label} is outside the capture")


def _regions_overlap(
    left: tuple[int, int, int, int],
    right: tuple[int, int, int, int],
) -> bool:
    return not (
        left[0] + left[2] <= right[0]
        or right[0] + right[2] <= left[0]
        or left[1] + left[3] <= right[1]
        or right[1] + right[3] <= left[1]
    )


def _safe_artifact_path(base_directory: Path, name: str, label: str) -> Path:
    relative = Path(name)
    if relative.name != name or relative.is_absolute() or name in {".", ".."}:
        raise ValidationError(f"{label} must be a local file name")
    path = base_directory / relative
    if path.is_symlink():
        raise ValidationError(f"{label} must not be a symbolic link")
    return path


def _load_raw_image(
    path: Path,
    width: int,
    height: int,
    expected_bytes: int,
    label: str,
) -> RawImage:
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise ValidationError(f"unable to read {label}: {error}") from error
    actual_bytes = width * height * 8
    if expected_bytes != actual_bytes or len(payload) != actual_bytes:
        raise ValidationError(
            f"{label} byte count mismatch: metadata={expected_bytes}, "
            f"file={len(payload)}, expected={actual_bytes}"
        )
    pixels = tuple(struct.iter_unpack("<4e", payload))
    if len(pixels) != width * height:
        raise ValidationError(f"{label} pixel count mismatch")
    if any(not math.isfinite(channel) for pixel in pixels for channel in pixel):
        raise ValidationError(f"{label} contains a non-finite FP16 value")
    return RawImage(width, height, pixels)


def _validate_png(path: Path, width: int, height: int, label: str) -> None:
    try:
        header = path.read_bytes()[:24]
    except OSError as error:
        raise ValidationError(f"unable to read {label}: {error}") from error
    if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValidationError(f"{label} is not a PNG preview")
    if header[8:16] != b"\x00\x00\x00\rIHDR":
        raise ValidationError(f"{label} has no canonical IHDR")
    png_width, png_height = struct.unpack(">II", header[16:24])
    if (png_width, png_height) != (width, height):
        raise ValidationError(f"{label} dimensions do not match metadata")


def _pixel_delta(
    left: tuple[float, float, float, float],
    right: tuple[float, float, float, float],
) -> float:
    return max(abs(left[index] - right[index]) for index in range(3))


def _pair_metrics(
    left_image: RawImage,
    right_image: RawImage,
    region: tuple[int, int, int, int],
    threshold: float,
) -> PairMetrics:
    if (left_image.width, left_image.height) != (
        right_image.width,
        right_image.height,
    ):
        raise ValidationError("raw comparison images have different dimensions")
    _region_inside(region, (left_image.width, left_image.height), "comparison ROI")
    maximum_delta = 0.0
    different_pixels = 0
    left, top, width, height = region
    for y in range(top, top + height):
        for x in range(left, left + width):
            index = y * left_image.width + x
            delta = _pixel_delta(left_image.pixels[index], right_image.pixels[index])
            maximum_delta = max(maximum_delta, delta)
            if delta > threshold:
                different_pixels += 1
    return PairMetrics(maximum_delta, different_pixels)


def _spatial_pair_metrics(
    image: RawImage,
    left_region: tuple[int, int, int, int],
    right_region: tuple[int, int, int, int],
    threshold: float,
) -> PairMetrics:
    _region_inside(left_region, (image.width, image.height), "left spatial ROI")
    _region_inside(right_region, (image.width, image.height), "right spatial ROI")
    if left_region[2:] != right_region[2:]:
        raise ValidationError("spatial comparison ROIs have different dimensions")
    maximum_delta = 0.0
    different_pixels = 0
    for y_offset in range(left_region[3]):
        for x_offset in range(left_region[2]):
            left_index = (
                (left_region[1] + y_offset) * image.width
                + left_region[0]
                + x_offset
            )
            right_index = (
                (right_region[1] + y_offset) * image.width
                + right_region[0]
                + x_offset
            )
            delta = _pixel_delta(
                image.pixels[left_index], image.pixels[right_index]
            )
            maximum_delta = max(maximum_delta, delta)
            if delta > threshold:
                different_pixels += 1
    return PairMetrics(maximum_delta, different_pixels)


def _region_mean_rgb(
    image: RawImage,
    region: tuple[int, int, int, int],
) -> tuple[float, float, float]:
    _region_inside(region, (image.width, image.height), "mean RGB ROI")
    sums = [0.0, 0.0, 0.0]
    left, top, width, height = region
    for y in range(top, top + height):
        for x in range(left, left + width):
            pixel = image.pixels[y * image.width + x]
            for channel in range(3):
                sums[channel] += pixel[channel]
    count = width * height
    return tuple(value / count for value in sums)


def _region_rgb_range(
    image: RawImage,
    regions: tuple[tuple[int, int, int, int], ...],
) -> float:
    minimum = [math.inf, math.inf, math.inf]
    maximum = [-math.inf, -math.inf, -math.inf]
    for region in regions:
        _region_inside(region, (image.width, image.height), "RGB range ROI")
        left, top, width, height = region
        for y in range(top, top + height):
            for x in range(left, left + width):
                pixel = image.pixels[y * image.width + x]
                for channel in range(3):
                    minimum[channel] = min(minimum[channel], pixel[channel])
                    maximum[channel] = max(maximum[channel], pixel[channel])
    return max(maximum[index] - minimum[index] for index in range(3))


def _region_maximum_delta(
    images: tuple[RawImage, RawImage, RawImage],
    region: tuple[int, int, int, int],
) -> float:
    maximum = 0.0
    for first, second in ((0, 1), (0, 2), (1, 2)):
        maximum = max(
            maximum,
            _pair_metrics(images[first], images[second], region, math.inf).maximum_rgb_delta,
        )
    return maximum


def _validate_contract(value: Any) -> dict[str, float]:
    contract = _object(value, "contract")
    _require_fields(contract, CONTRACT_FIELDS, "contract")
    for field, expected in {
        "scope": EXPECTED_SCOPE,
        "captureTarget": EXPECTED_CAPTURE_TARGET,
        "surfaceFormat": EXPECTED_SURFACE_FORMAT,
        "sessionTopology": "single-session-dynamic-affinity-matrix",
        "frameMarkerSemantic": "stage-unique-solid-srgb8",
    }.items():
        if _string(contract[field], f"contract.{field}") != expected:
            raise ValidationError(f"contract.{field} is unexpected")
    if not _boolean(contract["systemBorderAllowed"], "contract.systemBorderAllowed"):
        raise ValidationError("contract must allow the WGC system border")
    if _boolean(contract["cursorCaptureEnabled"], "contract.cursorCaptureEnabled"):
        raise ValidationError("contract must disable WGC cursor capture")
    if _boolean(
        contract["validatesProductStopWdaStartTransaction"],
        "contract.validatesProductStopWdaStartTransaction",
    ):
        raise ValidationError(
            "dynamic-affinity evidence must not claim the product restart transaction"
        )

    attempts = _integer(
        contract["maximumStableSampleAttempts"],
        "contract.maximumStableSampleAttempts",
        1,
    )
    if attempts != EXPECTED_MAXIMUM_STABLE_SAMPLE_ATTEMPTS:
        raise ValidationError(
            "contract.maximumStableSampleAttempts is unexpected"
        )
    expected_numbers = {
        "stableSampleTolerance": EXPECTED_MAXIMUM_STABLE_RGB_DELTA,
        "differenceThreshold": EXPECTED_DIFFERENCE_THRESHOLD,
        "minimumChangedFraction": EXPECTED_MINIMUM_OVERLAY_DIFFERENT_FRACTION,
        "minimumOverlayDelta": EXPECTED_MINIMUM_OVERLAY_RGB_DELTA,
        "maximumExcludedRange": EXPECTED_MAXIMUM_EXCLUDED_OVERLAY_RGB_RANGE,
        "maximumExcludedBackgroundDelta": EXPECTED_MAXIMUM_EXCLUDED_BACKGROUND_DELTA,
        "maximumIncludedDelta": EXPECTED_MAXIMUM_INCLUDED_STABILITY_RGB_DELTA,
        "maximumControlDelta": EXPECTED_MAXIMUM_CONTROL_RGB_DELTA,
        "maximumMarkerRange": EXPECTED_MAXIMUM_MARKER_RGB_RANGE,
        "minimumMarkerDelta": EXPECTED_MINIMUM_MARKER_RGB_DELTA,
        "markerChannelMargin": EXPECTED_MARKER_CHANNEL_MARGIN,
    }
    result: dict[str, float] = {}
    for field, expected in expected_numbers.items():
        result[field] = _number(contract[field], f"contract.{field}")
        _close(result[field], expected, f"contract.{field}")
    return result


def _validate_environment(capture: dict[str, Any]) -> tuple[int, int]:
    os_info = capture["osVersion"]
    if os_info is not None:
        parsed_os = _object(os_info, "osVersion")
        _require_fields(parsed_os, OS_FIELDS, "osVersion")
        _integer(parsed_os["major"], "osVersion.major", 1)
        _integer(parsed_os["minor"], "osVersion.minor")
        _integer(parsed_os["build"], "osVersion.build", 1)

    device = _object(capture["rendererDevice"], "rendererDevice")
    _require_fields(device, DEVICE_FIELDS, "rendererDevice")
    if _string(device["driverType"], "rendererDevice.driverType") != "hardware":
        raise ValidationError("self-exclusion evidence must use hardware D3D11")
    _string(device["adapter"], "rendererDevice.adapter")
    luid = _object(device["adapterLuid"], "rendererDevice.adapterLuid")
    _require_fields(luid, {"low", "high"}, "rendererDevice.adapterLuid")
    _integer(luid["low"], "rendererDevice.adapterLuid.low")
    _integer(luid["high"], "rendererDevice.adapterLuid.high", -(2**31))
    _integer(device["vendorId"], "rendererDevice.vendorId")
    _integer(device["deviceId"], "rendererDevice.deviceId")
    if device["driverVersion"] is not None:
        _integer(device["driverVersion"], "rendererDevice.driverVersion")
    _integer(device["featureLevel"], "rendererDevice.featureLevel", 1)
    _integer(capture["observerFeatureLevel"], "observerFeatureLevel", 1)

    capabilities = _object(capture["wgcCapabilities"], "wgcCapabilities")
    _require_fields(capabilities, WGC_CAPABILITY_FIELDS, "wgcCapabilities")
    _boolean(capabilities["borderHidden"], "wgcCapabilities.borderHidden")
    if not _boolean(capabilities["cursorExcluded"], "wgcCapabilities.cursorExcluded"):
        raise ValidationError("WGC cursor exclusion was not confirmed")
    if _boolean(
        capabilities["cursorCaptureEnabled"],
        "wgcCapabilities.cursorCaptureEnabled",
    ):
        raise ValidationError("WGC cursor capture remained enabled")
    if not _boolean(
        capabilities["cursorControlConfirmed"],
        "wgcCapabilities.cursorControlConfirmed",
    ):
        raise ValidationError("WGC cursor control was not confirmed")

    fixture = _object(capture["fixture"], "fixture")
    _require_fields(fixture, FIXTURE_FIELDS, "fixture")
    monitor_bounds = _bounds(fixture["monitorBounds"], "fixture.monitorBounds")
    capture_bounds = _bounds(
        fixture["captureScreenBounds"], "fixture.captureScreenBounds"
    )
    monitor_size = (
        monitor_bounds[2] - monitor_bounds[0],
        monitor_bounds[3] - monitor_bounds[1],
    )
    capture_size = (
        capture_bounds[2] - capture_bounds[0],
        capture_bounds[3] - capture_bounds[1],
    )
    if (
        capture_bounds[2] - capture_bounds[0],
        capture_bounds[3] - capture_bounds[1],
    ) != EXPECTED_CAPTURE_SIZE:
        raise ValidationError("fixture.captureScreenBounds has the wrong size")
    if (
        capture_bounds[0] < monitor_bounds[0]
        or capture_bounds[1] < monitor_bounds[1]
        or capture_bounds[2] > monitor_bounds[2]
        or capture_bounds[3] > monitor_bounds[3]
    ):
        raise ValidationError("fixture capture is outside the monitor")

    capture_region = _region(fixture["captureRegion"], "fixture.captureRegion")
    overlay_roi = _region(fixture["overlayRoi"], "fixture.overlayRoi")
    control_roi = _region(fixture["controlRoi"], "fixture.controlRoi")
    marker_roi = _region(fixture["markerRoi"], "fixture.markerRoi")
    marker_reference_roi = _region(
        fixture["markerReferenceRoi"], "fixture.markerReferenceRoi"
    )
    expected_capture_region = (
        capture_bounds[0] - monitor_bounds[0],
        capture_bounds[1] - monitor_bounds[1],
        EXPECTED_CAPTURE_SIZE[0],
        EXPECTED_CAPTURE_SIZE[1],
    )
    if capture_region != expected_capture_region:
        raise ValidationError("fixture.captureRegion screen mapping is inconsistent")
    if overlay_roi != EXPECTED_OVERLAY_ROI:
        raise ValidationError("fixture.overlayRoi is unexpected")
    if control_roi != EXPECTED_CONTROL_ROI:
        raise ValidationError("fixture.controlRoi is unexpected")
    if marker_roi != EXPECTED_MARKER_ROI:
        raise ValidationError("fixture.markerRoi is unexpected")
    if marker_reference_roi != EXPECTED_MARKER_REFERENCE_ROI:
        raise ValidationError("fixture.markerReferenceRoi is unexpected")
    _region_inside(capture_region, monitor_size, "fixture.captureRegion")
    _region_inside(overlay_roi, capture_size, "fixture.overlayRoi")
    _region_inside(control_roi, capture_size, "fixture.controlRoi")
    _region_inside(marker_roi, capture_size, "fixture.markerRoi")
    _region_inside(
        marker_reference_roi, capture_size, "fixture.markerReferenceRoi"
    )
    overlay_screen_bounds = _bounds(
        fixture["overlayScreenBounds"], "fixture.overlayScreenBounds"
    )
    overlay_window = (
        overlay_screen_bounds[0] - capture_bounds[0],
        overlay_screen_bounds[1] - capture_bounds[1],
        overlay_screen_bounds[2] - overlay_screen_bounds[0],
        overlay_screen_bounds[3] - overlay_screen_bounds[1],
    )
    if overlay_window != EXPECTED_OVERLAY_WINDOW:
        raise ValidationError("fixture overlay screen bounds are unexpected")
    expected_overlay_bounds = (
        capture_bounds[0] + overlay_window[0],
        capture_bounds[1] + overlay_window[1],
        capture_bounds[0] + overlay_window[0] + overlay_window[2],
        capture_bounds[1] + overlay_window[1] + overlay_window[3],
    )
    if overlay_screen_bounds != expected_overlay_bounds:
        raise ValidationError("fixture overlay screen/artifact mapping is inconsistent")
    if not (
        overlay_roi[0] >= overlay_window[0]
        and overlay_roi[1] >= overlay_window[1]
        and overlay_roi[0] + overlay_roi[2]
        <= overlay_window[0] + overlay_window[2]
        and overlay_roi[1] + overlay_roi[3]
        <= overlay_window[1] + overlay_window[3]
    ):
        raise ValidationError("fixture.overlayRoi is outside the overlay window")
    if _regions_overlap(overlay_window, control_roi):
        raise ValidationError("fixture.controlRoi overlaps the overlay window")
    if (
        _regions_overlap(marker_roi, overlay_window)
        or _regions_overlap(marker_roi, control_roi)
    ):
        raise ValidationError("fixture.markerRoi overlaps a measurement ROI")
    if _srgb8(fixture["backgroundSrgb8"], "fixture.backgroundSrgb8") != (
        EXPECTED_BACKGROUND_SRGB8
    ):
        raise ValidationError("fixture.backgroundSrgb8 is unexpected")
    marker_colors = _object(
        fixture["markerColorsSrgb8"], "fixture.markerColorsSrgb8"
    )
    _require_fields(
        marker_colors,
        set(EXPECTED_MARKER_SRGB8),
        "fixture.markerColorsSrgb8",
    )
    for stage, expected in EXPECTED_MARKER_SRGB8.items():
        if _srgb8(
            marker_colors[stage], f"fixture.markerColorsSrgb8.{stage}"
        ) != expected:
            raise ValidationError(
                f"fixture.markerColorsSrgb8.{stage} is unexpected"
            )
    probe = _rgba(fixture["probeLinear"], "fixture.probeLinear")
    for index, (actual, expected) in enumerate(
        zip(probe, EXPECTED_PROBE_LINEAR, strict=True)
    ):
        _close(actual, expected, f"fixture.probeLinear[{index}]")
    return monitor_size


def _validate_affinity(value: Any, label: str, expected_excluded: bool) -> None:
    affinity = _object(value, label)
    _require_fields(affinity, AFFINITY_FIELDS, label)
    expected = EXPECTED_WDA_EXCLUDEFROMCAPTURE if expected_excluded else EXPECTED_WDA_NONE
    requested = _integer(affinity["requested"], f"{label}.requested")
    observed = _integer(affinity["observed"], f"{label}.observed")
    if requested != expected or observed != expected:
        raise ValidationError(f"{label} does not confirm the requested WDA mode")
    if not _boolean(affinity["setSucceeded"], f"{label}.setSucceeded"):
        raise ValidationError(f"{label} SetWindowDisplayAffinity failed")
    if not _boolean(affinity["querySucceeded"], f"{label}.querySucceeded"):
        raise ValidationError(f"{label} GetWindowDisplayAffinity failed")
    if _integer(affinity["setError"], f"{label}.setError") != 0:
        raise ValidationError(f"{label}.setError must be zero")
    if _integer(affinity["queryError"], f"{label}.queryError") != 0:
        raise ValidationError(f"{label}.queryError must be zero")
    if not _boolean(affinity["confirmed"], f"{label}.confirmed"):
        raise ValidationError(f"{label} affinity readback was not confirmed")


def _validate_frame(
    value: Any,
    label: str,
    monitor_size: tuple[int, int],
) -> tuple[int, int, int, int]:
    frame = _object(value, label)
    _require_fields(frame, FRAME_FIELDS, label)
    previous_generation = _integer(
        frame["previousGeneration"], f"{label}.previousGeneration"
    )
    generation = _integer(frame["generation"], f"{label}.generation", 1)
    marker = _integer(frame["markerNs"], f"{label}.markerNs", 1)
    captured = _integer(frame["capturedAtNs"], f"{label}.capturedAtNs", 1)
    if generation <= previous_generation:
        raise ValidationError(f"{label} generation did not advance")
    if captured <= marker:
        raise ValidationError(f"{label} frame is not newer than its QPC marker")
    if _size(frame["contentSize"], f"{label}.contentSize") != monitor_size:
        raise ValidationError(f"{label}.contentSize does not match the target monitor")
    return previous_generation, generation, marker, captured


def _validate_observation(
    value: Any,
    label: str,
    stage_name: str,
    expected_excluded: bool,
    expected_raw: str,
    expected_png: str,
    monitor_size: tuple[int, int],
    base_directory: Path,
    maximum_stable_delta: float,
) -> ObservationResult:
    observation = _object(value, label)
    _require_fields(observation, OBSERVATION_FIELDS, label)
    if (
        _boolean(observation["requestedExcluded"], f"{label}.requestedExcluded")
        != expected_excluded
    ):
        raise ValidationError(f"{label} requested the wrong WDA mode")
    _validate_affinity(observation["affinity"], f"{label}.affinity", expected_excluded)
    extended_style = _integer(
        observation["observedExtendedStyle"],
        f"{label}.observedExtendedStyle",
    )
    layered = _boolean(
        observation["layeredStyleRestored"], f"{label}.layeredStyleRestored"
    )
    transparent = _boolean(
        observation["transparentStyleRestored"],
        f"{label}.transparentStyleRestored",
    )
    if (
        extended_style & EXPECTED_OVERLAY_STYLE_MASK
        != EXPECTED_OVERLAY_STYLE_MASK
        or not layered
        or not transparent
    ):
        raise ValidationError(f"{label} did not restore layered transparent styles")
    if layered != bool(extended_style & 0x00080000) or transparent != bool(
        extended_style & 0x00000020
    ):
        raise ValidationError(f"{label} style booleans disagree with the style mask")
    marker_srgb8 = _srgb8(observation["markerSrgb8"], f"{label}.markerSrgb8")
    if marker_srgb8 != EXPECTED_MARKER_SRGB8[stage_name]:
        raise ValidationError(f"{label}.markerSrgb8 is unexpected")

    stable = _object(observation["stablePair"], f"{label}.stablePair")
    _require_fields(stable, STABLE_PAIR_FIELDS, f"{label}.stablePair")
    _, first_generation, first_marker, first_captured = _validate_frame(
        stable["first"],
        f"{label}.stablePair.first",
        monitor_size,
    )
    second_previous, second_generation, second_marker, second_captured = _validate_frame(
        stable["second"],
        f"{label}.stablePair.second",
        monitor_size,
    )
    if second_previous < first_generation or second_generation <= first_generation:
        raise ValidationError(f"{label}.stablePair generations did not advance")
    if not first_marker < first_captured < second_marker < second_captured:
        raise ValidationError(f"{label}.stablePair QPC samples are stale or unordered")
    stable_delta = _number(
        stable["maximumRgbDelta"], f"{label}.stablePair.maximumRgbDelta"
    )
    if stable_delta > maximum_stable_delta:
        raise ValidationError(f"{label}.stablePair is not stable")

    artifact = _object(observation["artifact"], f"{label}.artifact")
    _require_fields(artifact, ARTIFACT_FIELDS, f"{label}.artifact")
    raw_name = _string(artifact["raw"], f"{label}.artifact.raw")
    png_name = _string(artifact["png"], f"{label}.artifact.png")
    if raw_name != expected_raw or png_name != expected_png:
        raise ValidationError(f"{label} artifact names are not canonical")
    width = _integer(artifact["width"], f"{label}.artifact.width", 1)
    height = _integer(artifact["height"], f"{label}.artifact.height", 1)
    raw_bytes = _integer(artifact["rawBytes"], f"{label}.artifact.rawBytes", 1)
    if (width, height) != EXPECTED_CAPTURE_SIZE:
        raise ValidationError(f"{label} artifact dimensions are unexpected")
    raw_path = _safe_artifact_path(base_directory, raw_name, f"{label}.artifact.raw")
    png_path = _safe_artifact_path(base_directory, png_name, f"{label}.artifact.png")
    image = _load_raw_image(raw_path, width, height, raw_bytes, raw_name)
    _validate_png(png_path, width, height, png_name)
    return ObservationResult(
        first_generation,
        second_generation,
        first_marker,
        second_captured,
        image,
    )


def _validate_reported_comparison(
    value: Any,
    label: str,
    expected_roi: tuple[int, int, int, int],
    expected_threshold: float,
    raw: PairMetrics,
) -> None:
    comparison = _object(value, label)
    _require_fields(comparison, COMPARISON_FIELDS, label)
    if _region(comparison["roi"], f"{label}.roi") != expected_roi:
        raise ValidationError(f"{label}.roi is unexpected")
    threshold = _number(comparison["threshold"], f"{label}.threshold")
    _close(threshold, expected_threshold, f"{label}.threshold")
    _close(
        _number(comparison["maximumRgbDelta"], f"{label}.maximumRgbDelta"),
        raw.maximum_rgb_delta,
        f"{label}.maximumRgbDelta",
    )
    if (
        _integer(comparison["differentPixels"], f"{label}.differentPixels")
        != raw.different_pixels
    ):
        raise ValidationError(f"{label}.differentPixels does not match raw FP16")


def _validate_reported_spatial_comparison(
    value: Any,
    label: str,
    expected_left_roi: tuple[int, int, int, int],
    expected_right_roi: tuple[int, int, int, int],
    expected_threshold: float,
    raw: PairMetrics,
) -> None:
    comparison = _object(value, label)
    _require_fields(comparison, SPATIAL_COMPARISON_FIELDS, label)
    if _region(comparison["leftRoi"], f"{label}.leftRoi") != expected_left_roi:
        raise ValidationError(f"{label}.leftRoi is unexpected")
    if _region(comparison["rightRoi"], f"{label}.rightRoi") != expected_right_roi:
        raise ValidationError(f"{label}.rightRoi is unexpected")
    threshold = _number(comparison["threshold"], f"{label}.threshold")
    _close(threshold, expected_threshold, f"{label}.threshold")
    _close(
        _number(comparison["maximumRgbDelta"], f"{label}.maximumRgbDelta"),
        raw.maximum_rgb_delta,
        f"{label}.maximumRgbDelta",
    )
    if (
        _integer(comparison["differentPixels"], f"{label}.differentPixels")
        != raw.different_pixels
    ):
        raise ValidationError(f"{label}.differentPixels does not match raw FP16")


def _validate_marker_metrics(
    value: Any,
    label: str,
    image: RawImage,
    threshold: float,
) -> tuple[tuple[float, float, float], float, PairMetrics]:
    metrics = _object(value, label)
    _require_fields(metrics, MARKER_METRIC_FIELDS, label)
    raw_mean = _region_mean_rgb(image, EXPECTED_MARKER_ROI)
    reported_mean = _linear_rgb(metrics["meanLinear"], f"{label}.meanLinear")
    for index, (reported, raw) in enumerate(
        zip(reported_mean, raw_mean, strict=True)
    ):
        _close(reported, raw, f"{label}.meanLinear[{index}]")
    raw_range = _region_rgb_range(image, (EXPECTED_MARKER_ROI,))
    _close(
        _number(metrics["rgbRange"], f"{label}.rgbRange"),
        raw_range,
        f"{label}.rgbRange",
    )
    raw_reference = _spatial_pair_metrics(
        image,
        EXPECTED_MARKER_ROI,
        EXPECTED_MARKER_REFERENCE_ROI,
        threshold,
    )
    _validate_reported_spatial_comparison(
        metrics["vsReference"],
        f"{label}.vsReference",
        EXPECTED_MARKER_ROI,
        EXPECTED_MARKER_REFERENCE_ROI,
        threshold,
        raw_reference,
    )
    return raw_mean, raw_range, raw_reference


def _validate_ledger(value: Any) -> int:
    ledger = _object(value, "resourceLedger")
    _require_fields(ledger, LEDGER_FIELDS, "resourceLedger")
    counts = {
        field: _integer(ledger[field], f"resourceLedger.{field}")
        for field in LEDGER_COUNT_FIELDS
    }
    if not _boolean(ledger["allReleased"], "resourceLedger.allReleased"):
        raise ValidationError("resourceLedger did not release all resources")
    if counts["failures"] != 0:
        raise ValidationError("resourceLedger.failures must be zero")
    for field in LIVE_LEDGER_FIELDS:
        if counts[field] != 0:
            raise ValidationError(f"resourceLedger.{field} must be zero")
    for acquired, released in LEDGER_PAIRS:
        if counts[acquired] != counts[released]:
            raise ValidationError(
                f"resourceLedger does not balance {acquired}/{released}"
            )
    if counts["framesAcquired"] < 6:
        raise ValidationError("resourceLedger must acquire at least six frames")
    if counts["framePoolsCreated"] != 1 or counts["framePoolsRecreated"] != 0:
        raise ValidationError("resourceLedger must use one fixed-size frame pool")
    for field in (
        "sessionsCreated",
        "frameArrivedRegistrations",
        "itemClosedRegistrations",
    ):
        if counts[field] != 1:
            raise ValidationError(f"resourceLedger.{field} must equal one")
    return counts["framesAcquired"]


def validate_capture(document: Any, base_directory: Path) -> VerificationResult:
    capture = _object(document, "root")
    _require_fields(capture, ROOT_FIELDS, "root")
    schema = _integer(capture["schemaVersion"], "schemaVersion", 1)
    if schema != EXPECTED_SCHEMA:
        raise ValidationError(f"unsupported schemaVersion: {schema}")
    spike = _string(capture["spikeId"], "spikeId")
    if spike != EXPECTED_SPIKE:
        raise ValidationError(f"unexpected spikeId: {spike}")
    _string(capture["applicationVersion"], "applicationVersion")
    revision = _string(capture["revision"], "revision")
    _string(capture["capturedAtUtc"], "capturedAtUtc")
    _integer(capture["timeoutMs"], "timeoutMs", 1)

    contract = _validate_contract(capture["contract"])
    monitor_size = _validate_environment(capture)
    observations = _object(capture["observations"], "observations")
    _require_fields(
        observations,
        {"includedBefore", "excluded", "includedAfter"},
        "observations",
    )
    before = _validate_observation(
        observations["includedBefore"],
        "observations.includedBefore",
        "includedBefore",
        False,
        "included-before.rgba16f",
        "included-before.png",
        monitor_size,
        base_directory,
        contract["stableSampleTolerance"],
    )
    excluded = _validate_observation(
        observations["excluded"],
        "observations.excluded",
        "excluded",
        True,
        "excluded.rgba16f",
        "excluded.png",
        monitor_size,
        base_directory,
        contract["stableSampleTolerance"],
    )
    after = _validate_observation(
        observations["includedAfter"],
        "observations.includedAfter",
        "includedAfter",
        False,
        "included-after.rgba16f",
        "included-after.png",
        monitor_size,
        base_directory,
        contract["stableSampleTolerance"],
    )
    if not (
        excluded.first_generation > before.generation
        and after.first_generation > excluded.generation
    ):
        raise ValidationError("WDA observation generations are not globally ordered")
    if not (
        before.captured_at_nanoseconds < excluded.first_marker_nanoseconds
        and excluded.captured_at_nanoseconds < after.first_marker_nanoseconds
    ):
        raise ValidationError("WDA observations are not temporally ordered")

    threshold = contract["differenceThreshold"]
    before_raw = _pair_metrics(
        before.image, excluded.image, EXPECTED_OVERLAY_ROI, threshold
    )
    after_raw = _pair_metrics(
        after.image, excluded.image, EXPECTED_OVERLAY_ROI, threshold
    )
    stability_raw = _pair_metrics(
        before.image, after.image, EXPECTED_OVERLAY_ROI, threshold
    )
    marker_before_excluded_raw = _pair_metrics(
        before.image, excluded.image, EXPECTED_MARKER_ROI, threshold
    )
    marker_excluded_after_raw = _pair_metrics(
        excluded.image, after.image, EXPECTED_MARKER_ROI, threshold
    )
    marker_before_after_raw = _pair_metrics(
        before.image, after.image, EXPECTED_MARKER_ROI, threshold
    )
    excluded_spatial_raw = _spatial_pair_metrics(
        excluded.image,
        EXPECTED_OVERLAY_ROI,
        EXPECTED_CONTROL_ROI,
        threshold,
    )
    excluded_range = _region_rgb_range(
        excluded.image, (EXPECTED_OVERLAY_ROI,)
    )
    # The remote control ROI represents the same controlled background.  A
    # combined range prevents a uniform but unrelated excluded rectangle from
    # being accepted as successful WDA self-exclusion.
    excluded_background_range = _region_rgb_range(
        excluded.image, (EXPECTED_OVERLAY_ROI, EXPECTED_CONTROL_ROI)
    )
    control_raw = _region_maximum_delta(
        (before.image, excluded.image, after.image), EXPECTED_CONTROL_ROI
    )

    metrics = _object(capture["metrics"], "metrics")
    _require_fields(metrics, METRICS_FIELDS, "metrics")
    _validate_reported_comparison(
        metrics["includedBeforeVsExcluded"],
        "metrics.includedBeforeVsExcluded",
        EXPECTED_OVERLAY_ROI,
        threshold,
        before_raw,
    )
    _validate_reported_comparison(
        metrics["includedAfterVsExcluded"],
        "metrics.includedAfterVsExcluded",
        EXPECTED_OVERLAY_ROI,
        threshold,
        after_raw,
    )
    _validate_reported_comparison(
        metrics["includedBeforeVsAfter"],
        "metrics.includedBeforeVsAfter",
        EXPECTED_OVERLAY_ROI,
        threshold,
        stability_raw,
    )
    for key, raw in (
        ("markerIncludedBeforeVsExcluded", marker_before_excluded_raw),
        ("markerExcludedVsIncludedAfter", marker_excluded_after_raw),
        ("markerIncludedBeforeVsAfter", marker_before_after_raw),
    ):
        _validate_reported_comparison(
            metrics[key],
            f"metrics.{key}",
            EXPECTED_MARKER_ROI,
            threshold,
            raw,
        )
    _validate_reported_spatial_comparison(
        metrics["excludedOverlayVsControl"],
        "metrics.excludedOverlayVsControl",
        EXPECTED_OVERLAY_ROI,
        EXPECTED_CONTROL_ROI,
        threshold,
        excluded_spatial_raw,
    )
    marker_documents = _object(metrics["markers"], "metrics.markers")
    _require_fields(
        marker_documents, set(EXPECTED_MARKER_SRGB8), "metrics.markers"
    )
    marker_raw = {
        "includedBefore": _validate_marker_metrics(
            marker_documents["includedBefore"],
            "metrics.markers.includedBefore",
            before.image,
            threshold,
        ),
        "excluded": _validate_marker_metrics(
            marker_documents["excluded"],
            "metrics.markers.excluded",
            excluded.image,
            threshold,
        ),
        "includedAfter": _validate_marker_metrics(
            marker_documents["includedAfter"],
            "metrics.markers.includedAfter",
            after.image,
            threshold,
        ),
    }
    _close(
        _number(
            metrics["excludedOverlayRgbRange"],
            "metrics.excludedOverlayRgbRange",
        ),
        excluded_range,
        "metrics.excludedOverlayRgbRange",
    )
    _close(
        _number(
            metrics["controlMaximumRgbDelta"],
            "metrics.controlMaximumRgbDelta",
        ),
        control_raw,
        "metrics.controlMaximumRgbDelta",
    )

    minimum_pixels = math.ceil(
        EXPECTED_OVERLAY_ROI[2]
        * EXPECTED_OVERLAY_ROI[3]
        * contract["minimumChangedFraction"]
    )
    for label, metrics in (
        ("includedBeforeVsExcluded", before_raw),
        ("includedAfterVsExcluded", after_raw),
    ):
        if metrics.different_pixels < minimum_pixels:
            raise ValidationError(f"{label} does not materially cover the overlay ROI")
        if metrics.maximum_rgb_delta < contract["minimumOverlayDelta"]:
            raise ValidationError(f"{label} overlay RGB delta is too small")
    if excluded_range > contract["maximumExcludedRange"]:
        raise ValidationError("excluded overlay ROI is not uniform background")
    if excluded_background_range > contract["maximumExcludedBackgroundDelta"]:
        raise ValidationError("excluded overlay ROI did not return to the background")
    if (
        excluded_spatial_raw.maximum_rgb_delta
        > contract["maximumExcludedBackgroundDelta"]
        or excluded_spatial_raw.different_pixels != 0
    ):
        raise ValidationError("excluded overlay ROI did not reveal remote background")
    if stability_raw.maximum_rgb_delta > contract["maximumIncludedDelta"]:
        raise ValidationError("included-before/after overlay pixels are not stable")
    if control_raw > contract["maximumControlDelta"]:
        raise ValidationError("remote control ROI changed between WDA modes")

    minimum_marker_pixels = math.ceil(
        EXPECTED_MARKER_ROI[2]
        * EXPECTED_MARKER_ROI[3]
        * contract["minimumChangedFraction"]
    )
    for stage, (mean, rgb_range, reference) in marker_raw.items():
        if rgb_range > contract["maximumMarkerRange"]:
            raise ValidationError(f"{stage} marker is not uniform")
        if (
            reference.maximum_rgb_delta < contract["minimumMarkerDelta"]
            or reference.different_pixels < minimum_marker_pixels
        ):
            raise ValidationError(f"{stage} marker is absent from the raw frame")
        red, green, blue = mean
        margin = contract["markerChannelMargin"]
        if stage == "includedBefore":
            identity_matches = red > green + margin and red > blue + margin
        elif stage == "excluded":
            identity_matches = green > red + margin and green > blue + margin
        else:
            identity_matches = red > blue + margin and green > blue + margin
        if not identity_matches:
            raise ValidationError(f"{stage} marker channel identity is wrong")
    for label, marker_metrics in (
        ("markerIncludedBeforeVsExcluded", marker_before_excluded_raw),
        ("markerExcludedVsIncludedAfter", marker_excluded_after_raw),
        ("markerIncludedBeforeVsAfter", marker_before_after_raw),
    ):
        if (
            marker_metrics.maximum_rgb_delta < contract["minimumMarkerDelta"]
            or marker_metrics.different_pixels < minimum_marker_pixels
        ):
            raise ValidationError(f"{label} stage markers are not distinct")

    frames_acquired = _validate_ledger(capture["resourceLedger"])
    marker_pairs = (
        marker_before_excluded_raw,
        marker_excluded_after_raw,
        marker_before_after_raw,
    )
    return VerificationResult(
        schema,
        spike,
        revision,
        "accepted",
        before_raw.different_pixels,
        after_raw.different_pixels,
        max(before_raw.maximum_rgb_delta, after_raw.maximum_rgb_delta),
        stability_raw.maximum_rgb_delta,
        excluded_range,
        excluded_spatial_raw.maximum_rgb_delta,
        excluded_spatial_raw.different_pixels,
        control_raw,
        min(pair.maximum_rgb_delta for pair in marker_pairs),
        min(pair.different_pixels for pair in marker_pairs),
        frames_acquired,
    )


def validate_path(path: Path) -> VerificationResult:
    try:
        with path.open("r", encoding="utf-8") as stream:
            document = json.load(
                stream,
                object_pairs_hook=_reject_duplicate_keys,
                parse_constant=_reject_nonfinite_constant,
            )
    except OSError as error:
        raise ValidationError(f"unable to read {path}: {error}") from error
    except json.JSONDecodeError as error:
        raise ValidationError(f"invalid JSON in {path}: {error}") from error
    return validate_capture(document, path.parent)


def _write_report(path: Path, result: VerificationResult) -> None:
    temporary = path.with_name(path.name + ".tmp")
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with temporary.open("w", encoding="utf-8", newline="\n") as stream:
            stream.write(json.dumps(asdict(result), indent=2) + "\n")
        temporary.replace(path)
    except OSError as error:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise ValidationError(f"unable to write report {path}: {error}") from error


def _parse_args(arguments: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path, help="SPK-002 self-exclusion.json")
    parser.add_argument("--report", type=Path, help="optional verification JSON")
    return parser.parse_args(arguments)


def main(arguments: list[str] | None = None) -> int:
    options = _parse_args(sys.argv[1:] if arguments is None else arguments)
    try:
        result = validate_path(options.capture)
        if options.report is not None:
            _write_report(options.report, result)
    except ValidationError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(
        f"PASS: {result.spike_id} {result.status}; "
        f"overlayPixels={result.included_before_different_pixels}/"
        f"{result.included_after_different_pixels}, "
        f"backgroundDelta={result.excluded_background_maximum_rgb_delta:.6f}, "
        f"markerPixels={result.minimum_marker_pair_different_pixels}, "
        f"stabilityDelta={result.included_stability_maximum_rgb_delta:.6f}, "
        f"controlDelta={result.control_maximum_rgb_delta:.6f}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
