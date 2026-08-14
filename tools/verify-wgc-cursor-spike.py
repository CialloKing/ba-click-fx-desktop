#!/usr/bin/env python3
"""Validate SPK-002 WGC cursor pixel evidence from raw FP16 frames."""

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
EXPECTED_SPIKE = "SPK-002-CURSOR"
EXPECTED_SCOPE = "controlled-window-cursor-pixels-only"
EXPECTED_CAPTURE_TARGET = "HWND"
EXPECTED_SURFACE_FORMAT = "DXGI_FORMAT_R16G16B16A16_FLOAT"
EXPECTED_SIZE = (320, 240)
EXPECTED_CURSOR_EXTENT = 32
EXPECTED_CURSOR_HOTSPOT = 16
EXPECTED_CURSOR_OPAQUE_PIXELS = 176
EXPECTED_ROI_EXTENT = 96
EXPECTED_CONTROL_ROI = (16, 16, 32, 32)
EXPECTED_DIFFERENCE_THRESHOLD = 0.02
EXPECTED_MINIMUM_CURSOR_DELTA = 0.25
EXPECTED_MAXIMUM_BACKGROUND_RANGE = 0.01
EXPECTED_MAXIMUM_CONTROL_DELTA = 0.01
EXPECTED_MAXIMUM_STABILITY_PIXELS = 4
FLOAT_TOLERANCE = 1.0e-5

ROOT_FIELDS = {
    "schemaVersion",
    "spikeId",
    "applicationVersion",
    "revision",
    "capturedAtUtc",
    "timeoutMs",
    "contract",
    "os",
    "device",
    "fixture",
    "observations",
    "comparisons",
}
CONTRACT_FIELDS = {
    "scope",
    "captureTarget",
    "surfaceFormat",
    "systemBorderAllowed",
    "cursorShape",
    "cursorExtent",
    "cursorHotspot",
    "cursorOpaquePixels",
    "differenceThreshold",
    "minimumDifferentPixels",
    "minimumCursorDelta",
    "maximumBackgroundRange",
    "maximumControlDelta",
    "maximumStabilityDifferentPixels",
}
OS_FIELDS = {"available", "major", "minor", "build"}
DEVICE_FIELDS = {
    "driverType",
    "adapter",
    "adapterLuid",
    "vendorId",
    "deviceId",
    "featureLevel",
}
FIXTURE_FIELDS = {
    "size",
    "screenOrigin",
    "cursorScreenPoint",
    "cursorClientPoint",
}
MODE_FIELDS = {
    "requestedCursorExcluded",
    "capabilities",
    "previousGeneration",
    "generation",
    "markerNanoseconds",
    "capturedAtNanoseconds",
    "size",
    "artifact",
    "ledger",
}
CAPABILITY_FIELDS = {
    "borderHidden",
    "cursorExcluded",
    "cursorCaptureEnabled",
    "cursorControlConfirmed",
}
ARTIFACT_FIELDS = {"raw", "png", "width", "height", "rawBytes"}
PAIR_FIELDS = {
    "roi",
    "threshold",
    "referenceRgbRange",
    "maximumRgbDelta",
    "differentPixels",
    "edgeDifferentPixels",
    "differenceBounds",
}
REGION_FIELDS = {"left", "top", "width", "height"}
BOUNDS_FIELDS = {"left", "top", "right", "bottom"}
POINT_FIELDS = {"x", "y"}
SIZE_FIELDS = {"width", "height"}
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
    """Raised when cursor evidence is malformed or unacceptable."""


@dataclass(frozen=True)
class RawImage:
    width: int
    height: int
    pixels: tuple[tuple[float, float, float, float], ...]


@dataclass(frozen=True)
class PairMetrics:
    reference_rgb_range: float
    maximum_rgb_delta: float
    different_pixels: int
    edge_different_pixels: int
    difference_bounds: tuple[int, int, int, int] | None


@dataclass(frozen=True)
class ModeResult:
    captured_at_nanoseconds: int
    image: RawImage
    frames_acquired: int


@dataclass(frozen=True)
class VerificationResult:
    schema_version: int
    spike_id: str
    capture_revision: str
    status: str
    included_before_different_pixels: int
    included_after_different_pixels: int
    maximum_cursor_delta: float
    stability_different_pixels: int
    control_maximum_rgb_delta: float
    total_frames_acquired: int


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValidationError(f"duplicate JSON field: {key}")
        result[key] = value
    return result


def _object(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
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
    if not isinstance(value, bool):
        raise ValidationError(f"{label} must be a boolean")
    return value


def _integer(value: Any, label: str, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise ValidationError(f"{label} must be an integer >= {minimum}")
    return value


def _number(value: Any, label: str, minimum: float = 0.0) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValidationError(f"{label} must be a finite number")
    result = float(value)
    if not math.isfinite(result) or result < minimum:
        raise ValidationError(f"{label} must be a finite number >= {minimum}")
    return result


def _string(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValidationError(f"{label} must be a non-empty string")
    return value


def _close(actual: float, expected: float, label: str) -> None:
    if not math.isclose(actual, expected, rel_tol=FLOAT_TOLERANCE, abs_tol=FLOAT_TOLERANCE):
        raise ValidationError(f"{label} mismatch: recorded={actual}, raw={expected}")


def _size(value: Any, label: str) -> tuple[int, int]:
    source = _object(value, label)
    _require_fields(source, SIZE_FIELDS, label)
    return (
        _integer(source["width"], f"{label}.width", 1),
        _integer(source["height"], f"{label}.height", 1),
    )


def _point(value: Any, label: str) -> tuple[int, int]:
    source = _object(value, label)
    _require_fields(source, POINT_FIELDS, label)
    return (
        _integer(source["x"], f"{label}.x", -(2**31)),
        _integer(source["y"], f"{label}.y", -(2**31)),
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


def _bounds(value: Any, label: str) -> tuple[int, int, int, int] | None:
    if value is None:
        return None
    source = _object(value, label)
    _require_fields(source, BOUNDS_FIELDS, label)
    result = (
        _integer(source["left"], f"{label}.left"),
        _integer(source["top"], f"{label}.top"),
        _integer(source["right"], f"{label}.right"),
        _integer(source["bottom"], f"{label}.bottom"),
    )
    if result[2] < result[0] or result[3] < result[1]:
        raise ValidationError(f"{label} is inverted")
    return result


def _validate_ledger(value: Any, label: str) -> int:
    ledger = _object(value, label)
    _require_fields(ledger, LEDGER_FIELDS, label)
    counts = {
        field: _integer(ledger[field], f"{label}.{field}")
        for field in LEDGER_COUNT_FIELDS
    }
    if not _boolean(ledger["allReleased"], f"{label}.allReleased"):
        raise ValidationError(f"{label} did not release all resources")
    if counts["failures"] != 0:
        raise ValidationError(f"{label}.failures must be zero")
    for field in LIVE_LEDGER_FIELDS:
        if counts[field] != 0:
            raise ValidationError(f"{label}.{field} must be zero")
    for acquired, released in LEDGER_PAIRS:
        if counts[acquired] != counts[released]:
            raise ValidationError(f"{label} does not balance {acquired}/{released}")
    if counts["framesAcquired"] < 2:
        raise ValidationError(f"{label} must acquire at least two frames")
    if counts["framePoolsCreated"] < 1 or counts["sessionsCreated"] != 1:
        raise ValidationError(f"{label} has an invalid WGC session count")
    return counts["framesAcquired"]


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


def _validate_png(path: Path, label: str) -> None:
    try:
        with path.open("rb") as stream:
            signature = stream.read(8)
    except OSError as error:
        raise ValidationError(f"unable to read {label}: {error}") from error
    if signature != b"\x89PNG\r\n\x1a\n":
        raise ValidationError(f"{label} is not a PNG preview")


def _validate_mode(
    value: Any,
    label: str,
    expected_excluded: bool,
    expected_raw: str,
    expected_png: str,
    base_directory: Path,
) -> ModeResult:
    mode = _object(value, label)
    _require_fields(mode, MODE_FIELDS, label)
    requested_excluded = _boolean(
        mode["requestedCursorExcluded"], f"{label}.requestedCursorExcluded"
    )
    if requested_excluded != expected_excluded:
        raise ValidationError(f"{label} requested the wrong cursor mode")

    capabilities = _object(mode["capabilities"], f"{label}.capabilities")
    _require_fields(capabilities, CAPABILITY_FIELDS, f"{label}.capabilities")
    if not _boolean(
        capabilities["cursorControlConfirmed"],
        f"{label}.capabilities.cursorControlConfirmed",
    ):
        raise ValidationError(f"{label} cursor control was not confirmed")
    cursor_excluded = _boolean(
        capabilities["cursorExcluded"], f"{label}.capabilities.cursorExcluded"
    )
    cursor_enabled = _boolean(
        capabilities["cursorCaptureEnabled"],
        f"{label}.capabilities.cursorCaptureEnabled",
    )
    _boolean(capabilities["borderHidden"], f"{label}.capabilities.borderHidden")
    if cursor_excluded != expected_excluded or cursor_enabled == expected_excluded:
        raise ValidationError(f"{label} cursor capability readback is inconsistent")

    previous_generation = _integer(
        mode["previousGeneration"], f"{label}.previousGeneration", 1
    )
    generation = _integer(mode["generation"], f"{label}.generation", 1)
    marker = _integer(mode["markerNanoseconds"], f"{label}.markerNanoseconds", 1)
    captured = _integer(
        mode["capturedAtNanoseconds"], f"{label}.capturedAtNanoseconds", 1
    )
    if generation <= previous_generation:
        raise ValidationError(f"{label} generation did not advance")
    if captured <= marker:
        raise ValidationError(f"{label} frame is not newer than its QPC marker")
    if _size(mode["size"], f"{label}.size") != EXPECTED_SIZE:
        raise ValidationError(f"{label} has an unexpected content size")

    artifact = _object(mode["artifact"], f"{label}.artifact")
    _require_fields(artifact, ARTIFACT_FIELDS, f"{label}.artifact")
    raw_name = _string(artifact["raw"], f"{label}.artifact.raw")
    png_name = _string(artifact["png"], f"{label}.artifact.png")
    if raw_name != expected_raw or png_name != expected_png:
        raise ValidationError(f"{label} artifact names are not canonical")
    width = _integer(artifact["width"], f"{label}.artifact.width", 1)
    height = _integer(artifact["height"], f"{label}.artifact.height", 1)
    raw_bytes = _integer(artifact["rawBytes"], f"{label}.artifact.rawBytes", 1)
    if (width, height) != EXPECTED_SIZE:
        raise ValidationError(f"{label} artifact dimensions are unexpected")
    raw_path = _safe_artifact_path(base_directory, raw_name, f"{label}.artifact.raw")
    png_path = _safe_artifact_path(base_directory, png_name, f"{label}.artifact.png")
    image = _load_raw_image(raw_path, width, height, raw_bytes, raw_name)
    _validate_png(png_path, png_name)
    frames = _validate_ledger(mode["ledger"], f"{label}.ledger")
    return ModeResult(captured, image, frames)


def _pixel_delta(
    left: tuple[float, float, float, float],
    right: tuple[float, float, float, float],
) -> float:
    return max(abs(left[index] - right[index]) for index in range(3))


def _rgb_range(image: RawImage, region: tuple[int, int, int, int]) -> float:
    left, top, width, height = region
    minimum = [math.inf, math.inf, math.inf]
    maximum = [-math.inf, -math.inf, -math.inf]
    for y in range(top, top + height):
        for x in range(left, left + width):
            pixel = image.pixels[y * image.width + x]
            for channel in range(3):
                minimum[channel] = min(minimum[channel], pixel[channel])
                maximum[channel] = max(maximum[channel], pixel[channel])
    return max(maximum[index] - minimum[index] for index in range(3))


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
    left, top, width, height = region
    if left + width > left_image.width or top + height > left_image.height:
        raise ValidationError("raw comparison ROI is outside the image")
    maximum_delta = 0.0
    count = 0
    edge_count = 0
    bounds: tuple[int, int, int, int] | None = None
    for y_offset in range(height):
        for x_offset in range(width):
            x = left + x_offset
            y = top + y_offset
            index = y * left_image.width + x
            delta = _pixel_delta(left_image.pixels[index], right_image.pixels[index])
            maximum_delta = max(maximum_delta, delta)
            if delta <= threshold:
                continue
            count += 1
            if x_offset in {0, width - 1} or y_offset in {0, height - 1}:
                edge_count += 1
            if bounds is None:
                bounds = (x, y, x, y)
            else:
                bounds = (
                    min(bounds[0], x),
                    min(bounds[1], y),
                    max(bounds[2], x),
                    max(bounds[3], y),
                )
    return PairMetrics(
        _rgb_range(right_image, region),
        maximum_delta,
        count,
        edge_count,
        bounds,
    )


def _region_maximum_delta(
    images: tuple[RawImage, RawImage, RawImage],
    region: tuple[int, int, int, int],
) -> float:
    pairs = ((0, 1), (0, 2), (1, 2))
    maximum = 0.0
    left, top, width, height = region
    for first, second in pairs:
        for y in range(top, top + height):
            for x in range(left, left + width):
                index = y * images[first].width + x
                maximum = max(
                    maximum,
                    _pixel_delta(
                        images[first].pixels[index], images[second].pixels[index]
                    ),
                )
    return maximum


def _validate_reported_pair(
    value: Any,
    label: str,
    expected_region: tuple[int, int, int, int],
    expected_threshold: float,
    raw: PairMetrics,
) -> None:
    pair = _object(value, label)
    _require_fields(pair, PAIR_FIELDS, label)
    if _region(pair["roi"], f"{label}.roi") != expected_region:
        raise ValidationError(f"{label}.roi does not match the cursor point")
    threshold = _number(pair["threshold"], f"{label}.threshold")
    _close(threshold, expected_threshold, f"{label}.threshold")
    _close(
        _number(pair["referenceRgbRange"], f"{label}.referenceRgbRange"),
        raw.reference_rgb_range,
        f"{label}.referenceRgbRange",
    )
    _close(
        _number(pair["maximumRgbDelta"], f"{label}.maximumRgbDelta"),
        raw.maximum_rgb_delta,
        f"{label}.maximumRgbDelta",
    )
    if _integer(pair["differentPixels"], f"{label}.differentPixels") != raw.different_pixels:
        raise ValidationError(f"{label}.differentPixels does not match raw FP16")
    if (
        _integer(pair["edgeDifferentPixels"], f"{label}.edgeDifferentPixels")
        != raw.edge_different_pixels
    ):
        raise ValidationError(f"{label}.edgeDifferentPixels does not match raw FP16")
    if _bounds(pair["differenceBounds"], f"{label}.differenceBounds") != raw.difference_bounds:
        raise ValidationError(f"{label}.differenceBounds does not match raw FP16")


def _validate_cursor_bounds(
    bounds: tuple[int, int, int, int] | None,
    cursor_point: tuple[int, int],
    label: str,
) -> None:
    if bounds is None:
        raise ValidationError(f"{label} has no cursor difference bounds")
    margin = 8
    minimum_left = cursor_point[0] - EXPECTED_CURSOR_HOTSPOT - margin
    minimum_top = cursor_point[1] - EXPECTED_CURSOR_HOTSPOT - margin
    maximum_right = (
        cursor_point[0]
        + EXPECTED_CURSOR_EXTENT
        - EXPECTED_CURSOR_HOTSPOT
        - 1
        + margin
    )
    maximum_bottom = (
        cursor_point[1]
        + EXPECTED_CURSOR_EXTENT
        - EXPECTED_CURSOR_HOTSPOT
        - 1
        + margin
    )
    if (
        bounds[0] < minimum_left
        or bounds[1] < minimum_top
        or bounds[2] > maximum_right
        or bounds[3] > maximum_bottom
    ):
        raise ValidationError(f"{label} escapes the expected cursor raster")


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

    contract = _object(capture["contract"], "contract")
    _require_fields(contract, CONTRACT_FIELDS, "contract")
    expected_strings = {
        "scope": EXPECTED_SCOPE,
        "captureTarget": EXPECTED_CAPTURE_TARGET,
        "surfaceFormat": EXPECTED_SURFACE_FORMAT,
        "cursorShape": "custom-monochrome-cross",
    }
    for field, expected in expected_strings.items():
        if _string(contract[field], f"contract.{field}") != expected:
            raise ValidationError(f"contract.{field} is unexpected")
    if not _boolean(contract["systemBorderAllowed"], "contract.systemBorderAllowed"):
        raise ValidationError("contract must allow the WGC system border")
    if _integer(contract["cursorExtent"], "contract.cursorExtent") != EXPECTED_CURSOR_EXTENT:
        raise ValidationError("contract.cursorExtent is unexpected")
    if _integer(contract["cursorHotspot"], "contract.cursorHotspot") != EXPECTED_CURSOR_HOTSPOT:
        raise ValidationError("contract.cursorHotspot is unexpected")
    if (
        _integer(contract["cursorOpaquePixels"], "contract.cursorOpaquePixels")
        != EXPECTED_CURSOR_OPAQUE_PIXELS
    ):
        raise ValidationError("contract.cursorOpaquePixels is unexpected")
    threshold = _number(contract["differenceThreshold"], "contract.differenceThreshold")
    minimum_pixels = _integer(
        contract["minimumDifferentPixels"], "contract.minimumDifferentPixels", 1
    )
    minimum_delta = _number(contract["minimumCursorDelta"], "contract.minimumCursorDelta")
    maximum_background = _number(
        contract["maximumBackgroundRange"], "contract.maximumBackgroundRange"
    )
    maximum_control = _number(
        contract["maximumControlDelta"], "contract.maximumControlDelta"
    )
    maximum_stability_pixels = _integer(
        contract["maximumStabilityDifferentPixels"],
        "contract.maximumStabilityDifferentPixels",
    )
    if minimum_pixels != EXPECTED_CURSOR_OPAQUE_PIXELS // 4:
        raise ValidationError("contract.minimumDifferentPixels is unexpected")
    _close(
        threshold,
        EXPECTED_DIFFERENCE_THRESHOLD,
        "contract.differenceThreshold",
    )
    _close(
        minimum_delta,
        EXPECTED_MINIMUM_CURSOR_DELTA,
        "contract.minimumCursorDelta",
    )
    _close(
        maximum_background,
        EXPECTED_MAXIMUM_BACKGROUND_RANGE,
        "contract.maximumBackgroundRange",
    )
    _close(
        maximum_control,
        EXPECTED_MAXIMUM_CONTROL_DELTA,
        "contract.maximumControlDelta",
    )
    if maximum_stability_pixels != EXPECTED_MAXIMUM_STABILITY_PIXELS:
        raise ValidationError(
            "contract.maximumStabilityDifferentPixels is unexpected"
        )

    os_info = _object(capture["os"], "os")
    _require_fields(os_info, OS_FIELDS, "os")
    if not _boolean(os_info["available"], "os.available"):
        raise ValidationError("OS version evidence is unavailable")
    _integer(os_info["major"], "os.major", 1)
    _integer(os_info["minor"], "os.minor")
    _integer(os_info["build"], "os.build", 1)

    device = _object(capture["device"], "device")
    _require_fields(device, DEVICE_FIELDS, "device")
    if _string(device["driverType"], "device.driverType") != "hardware":
        raise ValidationError("cursor evidence must use hardware D3D11")
    _string(device["adapter"], "device.adapter")
    luid = _object(device["adapterLuid"], "device.adapterLuid")
    _require_fields(luid, {"low", "high"}, "device.adapterLuid")
    _integer(luid["low"], "device.adapterLuid.low")
    _integer(luid["high"], "device.adapterLuid.high", -(2**31))
    _integer(device["vendorId"], "device.vendorId")
    _integer(device["deviceId"], "device.deviceId")
    _integer(device["featureLevel"], "device.featureLevel", 1)

    fixture = _object(capture["fixture"], "fixture")
    _require_fields(fixture, FIXTURE_FIELDS, "fixture")
    if _size(fixture["size"], "fixture.size") != EXPECTED_SIZE:
        raise ValidationError("fixture.size is unexpected")
    origin = _point(fixture["screenOrigin"], "fixture.screenOrigin")
    screen_point = _point(fixture["cursorScreenPoint"], "fixture.cursorScreenPoint")
    client_point = _point(fixture["cursorClientPoint"], "fixture.cursorClientPoint")
    if client_point != (EXPECTED_SIZE[0] // 2, EXPECTED_SIZE[1] // 2):
        raise ValidationError("fixture cursor is not centered")
    if screen_point != (origin[0] + client_point[0], origin[1] + client_point[1]):
        raise ValidationError("fixture cursor screen/client mapping is inconsistent")

    observations = _object(capture["observations"], "observations")
    _require_fields(
        observations, {"includedBefore", "excluded", "includedAfter"}, "observations"
    )
    before = _validate_mode(
        observations["includedBefore"],
        "observations.includedBefore",
        False,
        "included-before.rgba16f",
        "included-before.png",
        base_directory,
    )
    excluded = _validate_mode(
        observations["excluded"],
        "observations.excluded",
        True,
        "excluded.rgba16f",
        "excluded.png",
        base_directory,
    )
    after = _validate_mode(
        observations["includedAfter"],
        "observations.includedAfter",
        False,
        "included-after.rgba16f",
        "included-after.png",
        base_directory,
    )
    if not (
        before.captured_at_nanoseconds
        < excluded.captured_at_nanoseconds
        < after.captured_at_nanoseconds
    ):
        raise ValidationError("cursor capture modes are not temporally ordered")

    roi = (
        client_point[0] - EXPECTED_ROI_EXTENT // 2,
        client_point[1] - EXPECTED_ROI_EXTENT // 2,
        EXPECTED_ROI_EXTENT,
        EXPECTED_ROI_EXTENT,
    )
    before_raw = _pair_metrics(before.image, excluded.image, roi, threshold)
    after_raw = _pair_metrics(after.image, excluded.image, roi, threshold)
    stability_raw = _pair_metrics(before.image, after.image, roi, threshold)

    comparisons = _object(capture["comparisons"], "comparisons")
    _require_fields(
        comparisons,
        {
            "includedBeforeVsExcluded",
            "includedAfterVsExcluded",
            "includedBeforeVsAfter",
            "controlRoi",
            "controlMaximumRgbDelta",
        },
        "comparisons",
    )
    _validate_reported_pair(
        comparisons["includedBeforeVsExcluded"],
        "comparisons.includedBeforeVsExcluded",
        roi,
        threshold,
        before_raw,
    )
    _validate_reported_pair(
        comparisons["includedAfterVsExcluded"],
        "comparisons.includedAfterVsExcluded",
        roi,
        threshold,
        after_raw,
    )
    _validate_reported_pair(
        comparisons["includedBeforeVsAfter"],
        "comparisons.includedBeforeVsAfter",
        roi,
        threshold,
        stability_raw,
    )
    if _region(comparisons["controlRoi"], "comparisons.controlRoi") != EXPECTED_CONTROL_ROI:
        raise ValidationError("comparisons.controlRoi is unexpected")
    control_raw = _region_maximum_delta(
        (before.image, excluded.image, after.image), EXPECTED_CONTROL_ROI
    )
    _close(
        _number(
            comparisons["controlMaximumRgbDelta"],
            "comparisons.controlMaximumRgbDelta",
        ),
        control_raw,
        "comparisons.controlMaximumRgbDelta",
    )

    for label, metrics in (
        ("includedBeforeVsExcluded", before_raw),
        ("includedAfterVsExcluded", after_raw),
    ):
        if metrics.reference_rgb_range > maximum_background:
            raise ValidationError(f"{label} background is not uniform")
        if metrics.different_pixels < minimum_pixels:
            raise ValidationError(f"{label} has too few cursor pixels")
        if metrics.maximum_rgb_delta < minimum_delta:
            raise ValidationError(f"{label} cursor delta is too small")
        if metrics.edge_different_pixels != 0:
            raise ValidationError(f"{label} difference reaches the ROI edge")
        _validate_cursor_bounds(metrics.difference_bounds, client_point, label)
    if control_raw > maximum_control:
        raise ValidationError("control ROI changed between cursor modes")
    if (
        stability_raw.different_pixels > maximum_stability_pixels
        or stability_raw.maximum_rgb_delta > threshold
    ):
        raise ValidationError("repeated cursor-included frames are not stable")
    before_bounds = before_raw.difference_bounds
    after_bounds = after_raw.difference_bounds
    if before_bounds is None or after_bounds is None:
        raise ValidationError("cursor difference bounds are missing")
    if any(
        abs(left - right) > 1
        for left, right in zip(
            before_bounds, after_bounds, strict=True
        )
    ):
        raise ValidationError("repeated cursor difference bounds moved")

    return VerificationResult(
        schema,
        spike,
        revision,
        "accepted",
        before_raw.different_pixels,
        after_raw.different_pixels,
        max(before_raw.maximum_rgb_delta, after_raw.maximum_rgb_delta),
        stability_raw.different_pixels,
        control_raw,
        before.frames_acquired + excluded.frames_acquired + after.frames_acquired,
    )


def validate_path(path: Path) -> VerificationResult:
    try:
        with path.open("r", encoding="utf-8") as stream:
            document = json.load(stream, object_pairs_hook=_reject_duplicate_keys)
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
    parser.add_argument("capture", type=Path, help="SPK-002 cursor.json")
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
        f"cursorPixels={result.included_before_different_pixels}/"
        f"{result.included_after_different_pixels}, "
        f"stabilityPixels={result.stability_different_pixels}, "
        f"controlDelta={result.control_maximum_rgb_delta:.6f}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
