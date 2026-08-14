#!/usr/bin/env python3
"""Validate SPK-001 DirectComposition/WGC source-over observations.

The collector writes only raw observations.  This script owns schema, timing,
stability and numerical acceptance so evidence can be re-evaluated without
rerunning an interactive desktop capture.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
import math
from pathlib import Path
import sys
from typing import Any


EXPECTED_SCHEMA = 1
EXPECTED_SPIKE = "SPK-001"
EXPECTED_SURFACE_FORMAT = "DXGI_FORMAT_R16G16B16A16_FLOAT"
EXPECTED_ALPHA_MODE = "premultiplied"
EXPECTED_COLOR_SPACE = "rgb-full-g10-p709"
EXPECTED_FORMULA = "C=S.rgb+(1-S.a)*B"
EXPECTED_SOURCE_INJECTION = "ClearRenderTargetView-production-swap-chain"
EXPECTED_GDI_SEMANTIC = "diagnostic-only-unsynchronized"
EXPECTED_SDR_DISPLAY = "rgb-full-g22-p709"
PRE_PRESENT_TOLERANCE = 0.005
FORMULA_ABSOLUTE_TOLERANCE = 0.015
FORMULA_RELATIVE_TOLERANCE = 0.002
MAXIMUM_STABILITY_TOLERANCE = 0.02
OPAQUE_ALPHA_TOLERANCE = 0.01
CRITICAL_SIGNAL_MINIMUM = 0.20
CHANNELS = ("r", "g", "b", "a")
RGB_CHANNELS = ("r", "g", "b")
BACKGROUND_DEFINITIONS = (
    ("black", (0, 0, 0)),
    ("gray-18-percent", (119, 119, 119)),
    ("color", (52, 120, 212)),
    ("white", (255, 255, 255)),
)
SOURCE_DEFINITIONS = (
    ("transparent", (0.0, 0.0, 0.0, 0.0)),
    ("additive-0.25", (0.25, 0.25, 0.25, 0.0)),
    ("extended-1-alpha-0.25", (1.0, 1.0, 1.0, 0.25)),
    ("extended-4-alpha-0.5", (4.0, 4.0, 4.0, 0.5)),
)


class ValidationError(ValueError):
    """Raised when a capture is malformed or fails SPK-001 acceptance."""


@dataclass(frozen=True)
class VerificationResult:
    schema_version: int
    spike_id: str
    capture_revision: str
    matrix_cell: str
    display_mode: str
    status: str
    backgrounds: int
    presentations: int
    formula_channel_checks: int
    maximum_formula_absolute_error: float
    degradations: tuple[str, ...]


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValidationError(f"duplicate JSON field: {key}")
        result[key] = value
    return result


def _object(value: Any, label: str) -> dict[str, Any]:
    if type(value) is not dict:
        raise ValidationError(f"{label} must be an object")
    return value


def _array(value: Any, label: str) -> list[Any]:
    if type(value) is not list:
        raise ValidationError(f"{label} must be an array")
    return value


def _string(value: Any, label: str) -> str:
    if type(value) is not str or not value:
        raise ValidationError(f"{label} must be a non-empty string")
    return value


def _boolean(value: Any, label: str) -> bool:
    if type(value) is not bool:
        raise ValidationError(f"{label} must be a boolean")
    return value


def _integer(value: Any, label: str, *, minimum: int | None = None) -> int:
    if type(value) is not int:
        raise ValidationError(f"{label} must be an integer")
    if minimum is not None and value < minimum:
        raise ValidationError(f"{label} must be >= {minimum}")
    return value


def _finite(value: Any, label: str) -> float:
    if type(value) not in (int, float) or isinstance(value, bool):
        raise ValidationError(f"{label} must be a finite number")
    result = float(value)
    if not math.isfinite(result):
        raise ValidationError(f"{label} must be a finite number")
    return result


def _require_fields(
    value: dict[str, Any], expected: set[str], label: str
) -> None:
    if set(value) != expected:
        missing = sorted(expected - set(value))
        extra = sorted(set(value) - expected)
        raise ValidationError(
            f"{label} fields differ; missing={missing}, extra={extra}"
        )


def _exact(value: Any, expected: Any, label: str) -> None:
    if value != expected:
        raise ValidationError(f"{label} must equal {expected!r}")


def _pixel(value: Any, label: str) -> dict[str, float]:
    pixel = _object(value, label)
    _require_fields(pixel, set(CHANNELS), label)
    return {channel: _finite(pixel[channel], f"{label}.{channel}") for channel in CHANNELS}


def _pixel_difference(
    left: dict[str, float], right: dict[str, float]
) -> float:
    return max(abs(left[channel] - right[channel]) for channel in CHANNELS)


def _mean_selected_pixel(
    attempts: list[dict[str, Any]], pair: tuple[int, int]
) -> dict[str, float]:
    return {
        channel: (
            attempts[pair[0]]["sample_pixel"][channel]
            + attempts[pair[1]]["sample_pixel"][channel]
        )
        * 0.5
        for channel in CHANNELS
    }


def _formula_tolerance(expected: float) -> float:
    return max(
        FORMULA_ABSOLUTE_TOLERANCE,
        abs(expected) * FORMULA_RELATIVE_TOLERANCE,
    )


def _is_allowed_sdr_degradation(
    background_name: str,
    source_name: str,
    baseline: float,
    expected: float,
    actual: float,
    tolerance: float,
) -> bool:
    overrange_case = (
        background_name == "white" and source_name != "transparent"
    ) or source_name == "extended-4-alpha-0.5"
    return (
        overrange_case
        and expected > 1.0
        and actual < expected - tolerance
        and actual >= baseline - tolerance
    )


def _validate_contract(document: dict[str, Any]) -> float:
    contract = _object(document["contract"], "contract")
    required = {
        "surfaceFormat",
        "swapChainAlphaMode",
        "swapChainColorSpace",
        "observerFormat",
        "observerExcludesOwnOverlay",
        "cursorExcluded",
        "systemBorderAllowed",
        "sourceInjection",
        "desktopGdiDiagnosticSemantic",
        "nonFiniteJsonEncoding",
        "formula",
        "stableSampleTolerance",
    }
    _require_fields(contract, required, "contract")
    _exact(contract["surfaceFormat"], EXPECTED_SURFACE_FORMAT, "contract.surfaceFormat")
    _exact(contract["observerFormat"], EXPECTED_SURFACE_FORMAT, "contract.observerFormat")
    _exact(contract["swapChainAlphaMode"], EXPECTED_ALPHA_MODE, "contract.swapChainAlphaMode")
    _exact(contract["swapChainColorSpace"], EXPECTED_COLOR_SPACE, "contract.swapChainColorSpace")
    _exact(contract["formula"], EXPECTED_FORMULA, "contract.formula")
    _exact(contract["sourceInjection"], EXPECTED_SOURCE_INJECTION, "contract.sourceInjection")
    _exact(
        contract["desktopGdiDiagnosticSemantic"],
        EXPECTED_GDI_SEMANTIC,
        "contract.desktopGdiDiagnosticSemantic",
    )
    _exact(contract["nonFiniteJsonEncoding"], "null", "contract.nonFiniteJsonEncoding")
    if _boolean(
        contract["observerExcludesOwnOverlay"],
        "contract.observerExcludesOwnOverlay",
    ):
        raise ValidationError("observer must include the probe overlay")
    if not _boolean(contract["cursorExcluded"], "contract.cursorExcluded"):
        raise ValidationError("observer must exclude the cursor")
    _boolean(contract["systemBorderAllowed"], "contract.systemBorderAllowed")
    tolerance = _finite(
        contract["stableSampleTolerance"], "contract.stableSampleTolerance"
    )
    if tolerance <= 0.0 or tolerance > MAXIMUM_STABILITY_TOLERANCE:
        raise ValidationError(
            "contract.stableSampleTolerance is outside the accepted range"
        )
    return tolerance


def _validate_environment(document: dict[str, Any]) -> tuple[int, int, str]:
    os_version = _object(document["osVersion"], "osVersion")
    _require_fields(os_version, {"major", "minor", "build"}, "osVersion")
    for name in ("major", "minor", "build"):
        _integer(os_version[name], f"osVersion.{name}", minimum=0)

    renderer = _object(document["rendererDevice"], "rendererDevice")
    renderer_fields = {
        "driverType",
        "adapter",
        "adapterLuid",
        "vendorId",
        "deviceId",
        "driverVersion",
        "featureLevel",
    }
    _require_fields(renderer, renderer_fields, "rendererDevice")
    if renderer["driverType"] != "hardware":
        raise ValidationError("rendererDevice.driverType must be hardware")
    _string(renderer["adapter"], "rendererDevice.adapter")
    luid = _object(renderer["adapterLuid"], "rendererDevice.adapterLuid")
    _require_fields(luid, {"low", "high"}, "rendererDevice.adapterLuid")
    _integer(luid["low"], "rendererDevice.adapterLuid.low", minimum=0)
    _integer(luid["high"], "rendererDevice.adapterLuid.high")
    for name in ("vendorId", "deviceId", "driverVersion", "featureLevel"):
        _integer(renderer[name], f"rendererDevice.{name}", minimum=0)
    _integer(document["observerFeatureLevel"], "observerFeatureLevel", minimum=0)

    affinity = _object(document["captureAffinity"], "captureAffinity")
    if (
        _integer(affinity.get("requested"), "captureAffinity.requested", minimum=0)
        != 0
        or _integer(affinity.get("observed"), "captureAffinity.observed", minimum=0)
        != 0
        or not _boolean(affinity.get("confirmed"), "captureAffinity.confirmed")
    ):
        raise ValidationError("capture affinity must be confirmed as WDA_NONE")

    capabilities = _object(document["wgcCapabilities"], "wgcCapabilities")
    _boolean(capabilities.get("borderHidden"), "wgcCapabilities.borderHidden")
    if not _boolean(
        capabilities.get("cursorExcluded"), "wgcCapabilities.cursorExcluded"
    ):
        raise ValidationError("WGC cursor exclusion was not confirmed")

    display = _object(document["display"], "display")
    display_fields = {
        "colorSpace",
        "colorSpaceName",
        "bitsPerColor",
        "minimumLuminanceNits",
        "maximumLuminanceNits",
        "maximumFullFrameLuminanceNits",
        "luminanceMetadataValid",
    }
    _require_fields(display, display_fields, "display")
    display_mode = _string(display.get("colorSpaceName"), "display.colorSpaceName")
    if display_mode != EXPECTED_SDR_DISPLAY:
        raise ValidationError(
            f"SDR verification requires {EXPECTED_SDR_DISPLAY}; observed {display_mode}"
        )
    _integer(display["colorSpace"], "display.colorSpace", minimum=0)
    _integer(display["bitsPerColor"], "display.bitsPerColor", minimum=1)
    for name in (
        "minimumLuminanceNits",
        "maximumLuminanceNits",
        "maximumFullFrameLuminanceNits",
    ):
        if _finite(display[name], f"display.{name}") < 0.0:
            raise ValidationError(f"display.{name} must be non-negative")
    _boolean(display["luminanceMetadataValid"], "display.luminanceMetadataValid")

    monitor = _object(document["monitorBounds"], "monitorBounds")
    width = _integer(monitor.get("right"), "monitorBounds.right") - _integer(
        monitor.get("left"), "monitorBounds.left"
    )
    height = _integer(monitor.get("bottom"), "monitorBounds.bottom") - _integer(
        monitor.get("top"), "monitorBounds.top"
    )
    if width <= 0 or height <= 0:
        raise ValidationError("monitorBounds must have positive dimensions")

    probe = _object(document["probeBounds"], "probeBounds")
    probe_left = _integer(probe.get("left"), "probeBounds.left")
    probe_top = _integer(probe.get("top"), "probeBounds.top")
    probe_right = _integer(probe.get("right"), "probeBounds.right")
    probe_bottom = _integer(probe.get("bottom"), "probeBounds.bottom")
    if (
        probe_left < monitor["left"]
        or probe_top < monitor["top"]
        or probe_right > monitor["right"]
        or probe_bottom > monitor["bottom"]
        or probe_right <= probe_left
        or probe_bottom <= probe_top
    ):
        raise ValidationError("probeBounds must be a positive region inside the monitor")
    return width, height, display_mode


def _validate_presentation(
    value: Any,
    label: str,
    expected_name: str,
    expected_source: tuple[float, float, float, float],
    stable_tolerance: float,
    content_size: tuple[int, int],
    expected_gdi_srgb: tuple[int, int, int] | None,
    generations: list[int],
) -> tuple[dict[str, float], int]:
    presentation = _object(value, label)
    _require_fields(
        presentation, {"name", "requestedSource", "attempts", "stablePair"}, label
    )
    _exact(presentation["name"], expected_name, f"{label}.name")
    requested = _pixel(presentation["requestedSource"], f"{label}.requestedSource")
    for channel, expected in zip(CHANNELS, expected_source, strict=True):
        if abs(requested[channel] - expected) > PRE_PRESENT_TOLERANCE:
            raise ValidationError(f"{label}.requestedSource.{channel} drifted")

    raw_attempts = _array(presentation["attempts"], f"{label}.attempts")
    if len(raw_attempts) < 2 or len(raw_attempts) > 3:
        raise ValidationError(f"{label}.attempts must contain 2 or 3 samples")
    attempts: list[dict[str, Any]] = []
    for index, raw_attempt in enumerate(raw_attempts):
        attempt_label = f"{label}.attempts[{index}]"
        attempt = _object(raw_attempt, attempt_label)
        _require_fields(
            attempt,
            {
                "presentMarkerNs",
                "prePresentPixel",
                "desktopGdiDiagnosticSrgb8",
                "sample",
            },
            attempt_label,
        )
        marker = _integer(
            attempt["presentMarkerNs"], f"{attempt_label}.presentMarkerNs", minimum=0
        )
        pre_present = _pixel(
            attempt["prePresentPixel"], f"{attempt_label}.prePresentPixel"
        )
        for channel, expected in zip(CHANNELS, expected_source, strict=True):
            if abs(pre_present[channel] - expected) > PRE_PRESENT_TOLERANCE:
                raise ValidationError(
                    f"{attempt_label}.prePresentPixel.{channel} drifted"
                )

        diagnostic = _array(
            attempt["desktopGdiDiagnosticSrgb8"],
            f"{attempt_label}.desktopGdiDiagnosticSrgb8",
        )
        if len(diagnostic) != 3:
            raise ValidationError(f"{attempt_label} GDI diagnostic must have 3 channels")
        for channel_index, channel in enumerate(diagnostic):
            parsed = _integer(
                channel,
                f"{attempt_label}.desktopGdiDiagnosticSrgb8[{channel_index}]",
                minimum=0,
            )
            if parsed > 255:
                raise ValidationError(f"{attempt_label} GDI diagnostic exceeds uint8")
            if (
                expected_gdi_srgb is not None
                and abs(parsed - expected_gdi_srgb[channel_index]) > 1
            ):
                raise ValidationError(
                    f"{attempt_label} baseline GDI diagnostic does not match the controlled background"
                )

        sample = _object(attempt["sample"], f"{attempt_label}.sample")
        _require_fields(
            sample, {"generation", "capturedAtNs", "contentSize", "pixel"}, f"{attempt_label}.sample"
        )
        generation = _integer(
            sample["generation"], f"{attempt_label}.sample.generation", minimum=1
        )
        if generations and generation <= generations[-1]:
            raise ValidationError("WGC sample generations must increase globally")
        generations.append(generation)
        captured_at = _integer(
            sample["capturedAtNs"], f"{attempt_label}.sample.capturedAtNs", minimum=0
        )
        if captured_at <= marker:
            raise ValidationError(f"{attempt_label} sample is not newer than Present")
        size = _object(sample["contentSize"], f"{attempt_label}.sample.contentSize")
        sample_size = (
            _integer(size.get("width"), f"{attempt_label}.sample.contentSize.width", minimum=1),
            _integer(size.get("height"), f"{attempt_label}.sample.contentSize.height", minimum=1),
        )
        if sample_size != content_size:
            raise ValidationError(f"{attempt_label} WGC content size differs from monitor")
        sample_pixel = _pixel(sample["pixel"], f"{attempt_label}.sample.pixel")
        if abs(sample_pixel["a"] - 1.0) > OPAQUE_ALPHA_TOLERANCE:
            raise ValidationError(f"{attempt_label} monitor sample alpha is not opaque")
        attempts.append({"sample_pixel": sample_pixel})

    pair_value = _array(presentation["stablePair"], f"{label}.stablePair")
    if len(pair_value) != 2:
        raise ValidationError(f"{label}.stablePair must contain two indices")
    pair = (
        _integer(pair_value[0], f"{label}.stablePair[0]", minimum=0),
        _integer(pair_value[1], f"{label}.stablePair[1]", minimum=0),
    )
    if pair[1] != pair[0] + 1 or pair[1] >= len(attempts):
        raise ValidationError(f"{label}.stablePair must select adjacent samples")
    if (
        _pixel_difference(
            attempts[pair[0]]["sample_pixel"], attempts[pair[1]]["sample_pixel"]
        )
        > stable_tolerance
    ):
        raise ValidationError(f"{label}.stablePair exceeds the stability tolerance")
    return _mean_selected_pixel(attempts, pair), len(attempts)


def validate_capture(document: Any) -> VerificationResult:
    capture = _object(document, "capture")
    required = {
        "schemaVersion",
        "spikeId",
        "applicationVersion",
        "revision",
        "capturedAtUtc",
        "timeoutMs",
        "contract",
        "osVersion",
        "monitorBounds",
        "probeBounds",
        "rendererDevice",
        "observerFeatureLevel",
        "display",
        "captureAffinity",
        "wgcCapabilities",
        "backgrounds",
    }
    _require_fields(capture, required, "capture")
    _exact(capture["schemaVersion"], EXPECTED_SCHEMA, "schemaVersion")
    _exact(capture["spikeId"], EXPECTED_SPIKE, "spikeId")
    revision = _string(capture["revision"], "revision")
    _string(capture["applicationVersion"], "applicationVersion")
    _string(capture["capturedAtUtc"], "capturedAtUtc")
    _integer(capture["timeoutMs"], "timeoutMs", minimum=1)

    stable_tolerance = _validate_contract(capture)
    width, height, display_mode = _validate_environment(capture)
    backgrounds = _array(capture["backgrounds"], "backgrounds")
    if len(backgrounds) != len(BACKGROUND_DEFINITIONS):
        raise ValidationError("backgrounds must contain the SPK-001 SDR matrix")

    generations: list[int] = []
    degradations: list[str] = []
    formula_checks = 0
    presentation_count = 0
    maximum_error = 0.0
    critical_pixels: dict[tuple[str, str], dict[str, float]] = {}

    for background_index, (background_name, expected_srgb) in enumerate(
        BACKGROUND_DEFINITIONS
    ):
        label = f"backgrounds[{background_index}]"
        background = _object(backgrounds[background_index], label)
        _require_fields(background, {"name", "srgb8", "baseline", "sources"}, label)
        _exact(background["name"], background_name, f"{label}.name")
        srgb = _array(background["srgb8"], f"{label}.srgb8")
        if tuple(srgb) != expected_srgb:
            raise ValidationError(f"{label}.srgb8 drifted from the matrix")

        baseline, _ = _validate_presentation(
            background["baseline"],
            f"{label}.baseline",
            "baseline",
            (0.0, 0.0, 0.0, 0.0),
            stable_tolerance,
            (width, height),
            expected_srgb,
            generations,
        )
        presentation_count += 1
        sources = _array(background["sources"], f"{label}.sources")
        if len(sources) != len(SOURCE_DEFINITIONS):
            raise ValidationError(f"{label}.sources must contain the fixed matrix")

        for source_index, (source_name, source_values) in enumerate(SOURCE_DEFINITIONS):
            source_label = f"{label}.sources[{source_index}]"
            actual, _ = _validate_presentation(
                sources[source_index],
                source_label,
                source_name,
                source_values,
                stable_tolerance,
                (width, height),
                None,
                generations,
            )
            presentation_count += 1
            critical_pixels[(background_name, source_name)] = actual
            for channel_index, channel in enumerate(RGB_CHANNELS):
                expected = source_values[channel_index] + (1.0 - source_values[3]) * baseline[channel]
                error = abs(actual[channel] - expected)
                maximum_error = max(maximum_error, error)
                tolerance = _formula_tolerance(expected)
                formula_checks += 1
                if error <= tolerance:
                    continue
                if _is_allowed_sdr_degradation(
                    background_name,
                    source_name,
                    baseline[channel],
                    expected,
                    actual[channel],
                    tolerance,
                ):
                    degradations.append(
                        f"{background_name}/{source_name}/{channel}: expected={expected:.6g}, actual={actual[channel]:.6g}"
                    )
                    continue
                raise ValidationError(
                    f"{source_label}.{channel} violates source-over: expected={expected:.6g}, "
                    f"actual={actual[channel]:.6g}, tolerance={tolerance:.6g}"
                )

    black_baseline = critical_pixels[("black", "transparent")]
    additive = critical_pixels[("black", "additive-0.25")]
    extended = critical_pixels[("black", "extended-1-alpha-0.25")]
    for channel in RGB_CHANNELS:
        if additive[channel] - black_baseline[channel] < CRITICAL_SIGNAL_MINIMUM:
            raise ValidationError(f"black additive probe lost A=0 RGB energy in {channel}")
        if extended[channel] - black_baseline[channel] < 0.8:
            raise ValidationError(f"black extended-premultiplied probe was canonicalized in {channel}")

    return VerificationResult(
        schema_version=1,
        spike_id=EXPECTED_SPIKE,
        capture_revision=revision,
        matrix_cell="sdr",
        display_mode=display_mode,
        status="sdr-accepted-with-degradation" if degradations else "sdr-accepted",
        backgrounds=len(backgrounds),
        presentations=presentation_count,
        formula_channel_checks=formula_checks,
        maximum_formula_absolute_error=maximum_error,
        degradations=tuple(degradations),
    )


def validate_path(path: Path) -> VerificationResult:
    try:
        with path.open("r", encoding="utf-8") as stream:
            document = json.load(stream, object_pairs_hook=_reject_duplicate_keys)
    except OSError as error:
        raise ValidationError(f"unable to read {path}: {error}") from error
    except json.JSONDecodeError as error:
        raise ValidationError(f"invalid JSON in {path}: {error}") from error
    return validate_capture(document)


def _write_report(path: Path, result: VerificationResult) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    # Evidence hashes must remain stable when the verifier runs on Windows.
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(json.dumps(asdict(result), indent=2, ensure_ascii=False) + "\n")
    temporary.replace(path)


def _parse_args(arguments: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path, help="SPK-001 capture.json")
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
        f"{result.backgrounds} backgrounds, {result.presentations} presentations, "
        f"{result.formula_channel_checks} channel checks, "
        f"max error={result.maximum_formula_absolute_error:.6g}"
    )
    for degradation in result.degradations:
        print(f"DEGRADED: {degradation}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
