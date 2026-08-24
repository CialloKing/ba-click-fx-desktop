#!/usr/bin/env python3
"""Validate and report an Active-FX ROI ABBA performance capture."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import sys
from typing import Any


MANIFEST_SCHEMA_VERSION = 3
REPORT_SCHEMA_VERSION = 2
CAPTURE_KIND = "bafx-active-fx-roi-ab-capture"
REPORT_KIND = "bafx-active-fx-roi-ab-report"
ENVIRONMENT_CONTRACT = "rtx-4060-4k170-sdr-v1"
ENVIRONMENT_IDENTITY_FIELDS = (
    "productVersion",
    "driverType",
    "adapter",
    "adapterLuid",
    "driverVersion",
    "hardwareFallback",
    "primaryDisplay",
    "primaryDpi",
    "refreshRateNumerator",
    "refreshRateDenominator",
    "outputWidth",
    "outputHeight",
    "hdrEnabled",
    "outputMapping",
)
CONFIG_SCHEMA_VERSION = 19
BLOCK_COUNT = 5
RUN_COUNT = 20
WARMUP_MS = 5_000
SAMPLE_MS = 30_000
HOST_DURATION_MS = 40_500
PERFORMANCE_INTERVAL_MS = 10_000
DISCARD_COMPLETE_INTERVALS = 1
SELECT_COMPLETE_INTERVALS = 3
ABBA_PATTERN = (("A", False), ("B", True), ("B", True), ("A", False))
SCENARIO_CONTRACTS = {
    "center-click": {
        "workload": "fixed-age-center-click",
        "driver": "host-demo-click-fixed-age-v1",
    },
    "interior-trail": {
        "workload": "fixed-age-interior-trail",
        "driver": "host-demo-interior-trail-fixed-age-v1",
    },
    "boundary-top-left": {
        "workload": "fixed-age-boundary-top-left",
        "driver": "host-demo-boundary-top-left-fixed-age-v1",
    },
}
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
REVISION_PATTERN = re.compile(r"^[0-9a-f]{40}$")
DECISION_REASONS = frozenset(
    {
        "disabled",
        "no-content",
        "background-differential-bloom",
        "context1-unavailable",
        "shared-target-full-write",
        "area-too-large",
        "benefit-too-small",
        "applied",
        "renderer-fallback",
        "boundary-fallback",
        "touches-boundary",
    }
)
ERROR_FIELDS = (
    "GPU.DisjointSamples",
    "GPU.QueryFailures",
    "GPU.StateErrors",
    "GPU.RingFullSkipped",
    "GPU.AutoSkippedStageFrames",
    "FramePacing.Timeouts",
    "FramePacing.Failures",
)


class ValidationError(ValueError):
    """Raised when capture evidence does not satisfy the strict contract."""


def _reject_constant(value: str) -> None:
    raise ValidationError(f"JSON non-finite number is forbidden: {value}")


def _strict_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValidationError(f"JSON object contains duplicate field {key!r}")
        result[key] = value
    return result


def _load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(
            path.read_text(encoding="utf-8-sig"),
            object_pairs_hook=_strict_object,
            parse_constant=_reject_constant,
        )
    except (OSError, json.JSONDecodeError) as error:
        raise ValidationError(f"cannot read {path}: {error}") from error
    if type(value) is not dict:
        raise ValidationError(f"{path} must contain a JSON object")
    return value


def _require_keys(value: dict[str, Any], expected: set[str], context: str) -> None:
    actual = set(value)
    if actual != expected:
        missing = sorted(expected - actual)
        unknown = sorted(actual - expected)
        raise ValidationError(
            f"{context} fields differ; missing={missing}, unknown={unknown}"
        )


def _dict(value: Any, context: str) -> dict[str, Any]:
    if type(value) is not dict:
        raise ValidationError(f"{context} must be an object")
    return value


def _list(value: Any, context: str) -> list[Any]:
    if type(value) is not list:
        raise ValidationError(f"{context} must be an array")
    return value


def _string(value: Any, context: str) -> str:
    if type(value) is not str or not value:
        raise ValidationError(f"{context} must be a non-empty string")
    return value


def _integer(value: Any, context: str) -> int:
    if type(value) is not int:
        raise ValidationError(f"{context} must be an integer")
    return value


def _boolean(value: Any, context: str) -> bool:
    if type(value) is not bool:
        raise ValidationError(f"{context} must be a boolean")
    return value


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        raise ValidationError(f"cannot hash {path}: {error}") from error
    return digest.hexdigest()


def _relative_file(root: Path, value: Any, context: str) -> Path:
    text = _string(value, context)
    relative = Path(text)
    if relative.is_absolute() or ".." in relative.parts:
        raise ValidationError(f"{context} must stay below the capture root")
    root_resolved = root.resolve()
    path = (root_resolved / relative).resolve()
    if path != root_resolved and root_resolved not in path.parents:
        raise ValidationError(f"{context} escapes the capture root")
    if not path.is_file():
        raise ValidationError(f"{context} is missing: {path}")
    return path


def _parse_event(block: str, path: Path) -> dict[str, str]:
    event: dict[str, str] = {}
    for line_number, raw_line in enumerate(block.splitlines(), 1):
        line = raw_line.rstrip("\r")
        if not line:
            continue
        if "=" not in line:
            raise ValidationError(
                f"{path}: malformed event line {line_number}: {line!r}"
            )
        key, value = line.split("=", 1)
        if not key:
            raise ValidationError(f"{path}: empty event field")
        if key in event and event[key] != value:
            raise ValidationError(f"{path}: conflicting duplicate event field {key!r}")
        # SupportReport repeats envelope fields with identical values. They
        # remain unambiguous evidence; conflicting duplicates are rejected.
        event[key] = value
    return event


def _load_events(path: Path) -> list[dict[str, str]]:
    try:
        text = path.read_text(encoding="utf-8-sig")
    except OSError as error:
        raise ValidationError(f"cannot read {path}: {error}") from error
    blocks = re.split(r"^---\s*$", text, flags=re.MULTILINE)
    events = [_parse_event(block, path) for block in blocks if block.strip()]
    if not events:
        raise ValidationError(f"{path}: no structured events")
    sessions = {event.get("Log.SessionId", "") for event in events}
    if "" in sessions or len(sessions) != 1:
        raise ValidationError(f"{path}: expected exactly one complete log session")
    return events


def _event_int(event: dict[str, str], key: str, context: str) -> int:
    raw = event.get(key)
    if raw is None or re.fullmatch(r"-?[0-9]+", raw) is None:
        raise ValidationError(f"{context}: {key} must be an integer")
    return int(raw)


def _event_number(event: dict[str, str], key: str, context: str) -> float:
    raw = event.get(key)
    try:
        value = float(raw) if raw is not None else math.nan
    except ValueError as error:
        raise ValidationError(f"{context}: {key} must be numeric") from error
    if not math.isfinite(value):
        raise ValidationError(f"{context}: {key} must be finite")
    return value


def _event_bool(event: dict[str, str], key: str, context: str) -> bool:
    raw = event.get(key)
    if raw == "true":
        return True
    if raw == "false":
        return False
    raise ValidationError(f"{context}: {key} must be true or false")


def _event_string(event: dict[str, str], key: str, context: str) -> str:
    value = event.get(key)
    if value is None or not value:
        raise ValidationError(f"{context}: {key} must be a non-empty string")
    return value


def _one_event(
    events: list[dict[str, str]], name: str, path: Path
) -> dict[str, str]:
    matches = [event for event in events if event.get("Event.Name") == name]
    if len(matches) != 1:
        raise ValidationError(f"{path}: expected one {name} event, found {len(matches)}")
    return matches[0]


def _validate_environment_identity(value: Any, context: str) -> dict[str, Any]:
    identity = _dict(value, context)
    _require_keys(identity, set(ENVIRONMENT_IDENTITY_FIELDS), context)
    normalized = {
        "productVersion": _string(identity["productVersion"], f"{context}.productVersion"),
        "driverType": _string(identity["driverType"], f"{context}.driverType"),
        "adapter": _string(identity["adapter"], f"{context}.adapter"),
        "adapterLuid": _string(identity["adapterLuid"], f"{context}.adapterLuid"),
        "driverVersion": _string(identity["driverVersion"], f"{context}.driverVersion"),
        "hardwareFallback": _string(
            identity["hardwareFallback"], f"{context}.hardwareFallback"
        ),
        "primaryDisplay": _string(
            identity["primaryDisplay"], f"{context}.primaryDisplay"
        ),
        "primaryDpi": _integer(identity["primaryDpi"], f"{context}.primaryDpi"),
        "refreshRateNumerator": _integer(
            identity["refreshRateNumerator"], f"{context}.refreshRateNumerator"
        ),
        "refreshRateDenominator": _integer(
            identity["refreshRateDenominator"], f"{context}.refreshRateDenominator"
        ),
        "outputWidth": _integer(identity["outputWidth"], f"{context}.outputWidth"),
        "outputHeight": _integer(identity["outputHeight"], f"{context}.outputHeight"),
        "hdrEnabled": _boolean(identity["hdrEnabled"], f"{context}.hdrEnabled"),
        "outputMapping": _string(
            identity["outputMapping"], f"{context}.outputMapping"
        ),
    }
    if normalized["driverType"] != "Hardware":
        raise ValidationError(f"{context}: performance evidence requires hardware D3D11")
    if "RTX 4060" not in normalized["adapter"].upper():
        raise ValidationError(f"{context}: adapter must contain RTX 4060")
    if normalized["hardwareFallback"] != "none":
        raise ValidationError(f"{context}: hardware fallback invalidates the capture")
    if (normalized["outputWidth"], normalized["outputHeight"]) != (3840, 2160):
        raise ValidationError(f"{context}: output must be 3840x2160")
    if (
        normalized["refreshRateNumerator"],
        normalized["refreshRateDenominator"],
    ) != (170, 1):
        raise ValidationError(f"{context}: refresh rate must be 170/1 Hz")
    if normalized["hdrEnabled"]:
        raise ValidationError(f"{context}: Display.HdrEnabled must be false")
    if normalized["outputMapping"] != "conservative-sdr":
        raise ValidationError(
            f"{context}: Graphics.OutputMapping must be conservative-sdr"
        )
    if normalized["primaryDpi"] <= 0:
        raise ValidationError(f"{context}: primaryDpi must be positive")
    return normalized


def _environment_identity_from_events(
    events: list[dict[str, str]], path: Path
) -> dict[str, Any]:
    support = _one_event(events, "SupportReport", path)
    configuration = _one_event(events, "Configuration.Applied", path)
    context = f"{path} environment"
    return _validate_environment_identity(
        {
            "productVersion": _event_string(
                support, "Product.Version", context
            ),
            "driverType": _event_string(
                support, "Graphics.DriverType", context
            ),
            "adapter": _event_string(support, "Graphics.Adapter", context),
            "adapterLuid": _event_string(
                support, "Graphics.AdapterLuid", context
            ),
            "driverVersion": _event_string(
                support, "Graphics.DriverVersion", context
            ),
            "hardwareFallback": _event_string(
                support, "Graphics.HardwareFallback", context
            ),
            "primaryDisplay": _event_string(
                support, "Display.Primary", context
            ),
            "primaryDpi": _event_int(support, "Display.PrimaryDpi", context),
            "refreshRateNumerator": _event_int(
                support, "Display.RefreshRateNumerator", context
            ),
            "refreshRateDenominator": _event_int(
                support, "Display.RefreshRateDenominator", context
            ),
            "outputWidth": _event_int(configuration, "Output.Width", context),
            "outputHeight": _event_int(configuration, "Output.Height", context),
            "hdrEnabled": _event_bool(
                configuration, "Display.HdrEnabled", context
            ),
            "outputMapping": _event_string(
                support, "Graphics.OutputMapping", context
            ),
        },
        context,
    )


def _require_same_environment_identity(
    expected: dict[str, Any], actual: dict[str, Any], context: str
) -> None:
    for name in ENVIRONMENT_IDENTITY_FIELDS:
        if actual[name] != expected[name]:
            raise ValidationError(
                f"{context}: environment identity drift at {name}: "
                f"{actual[name]!r} != {expected[name]!r}"
            )


def _median(values: list[float]) -> float:
    if not values:
        raise ValidationError("cannot calculate a median without samples")
    return float(statistics.median(values))


def _canonical_without_roi(config: dict[str, Any], context: str) -> str:
    if _integer(config.get("schemaVersion"), f"{context}.schemaVersion") != 19:
        raise ValidationError(f"{context}.schemaVersion must be 19")
    performance = _dict(config.get("performance"), f"{context}.performance")
    _boolean(
        performance.get("activeFxRoiEnabled"),
        f"{context}.performance.activeFxRoiEnabled",
    )
    normalized = copy.deepcopy(config)
    del normalized["performance"]["activeFxRoiEnabled"]
    return json.dumps(
        normalized,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    )


def _validate_capabilities(value: Any) -> None:
    capabilities = _dict(value, "manifest.capabilities")
    _require_keys(capabilities, set(SCENARIO_CONTRACTS), "capabilities")
    for name, scenario_contract in SCENARIO_CONTRACTS.items():
        capability = _dict(capabilities[name], f"capabilities.{name}")
        _require_keys(
            capability,
            {"supported", "driver", "failureCode"},
            f"capabilities.{name}",
        )
        contract = {
            "supported": True,
            "driver": scenario_contract["driver"],
            "failureCode": None,
        }
        if capability != contract:
            raise ValidationError(f"capabilities.{name} does not match contract v1")


def _validate_schedule(value: Any) -> None:
    schedule = _dict(value, "manifest.schedule")
    expected = {
        "pattern": "ABBA",
        "a": "roi-off",
        "b": "roi-on",
        "blocks": BLOCK_COUNT,
        "runs": RUN_COUNT,
        "warmupMs": WARMUP_MS,
        "sampleMs": SAMPLE_MS,
        "hostDurationMs": HOST_DURATION_MS,
        "performanceIntervalMs": PERFORMANCE_INTERVAL_MS,
        "discardCompleteIntervals": DISCARD_COMPLETE_INTERVALS,
        "selectCompleteIntervals": SELECT_COMPLETE_INTERVALS,
    }
    _require_keys(schedule, set(expected), "manifest.schedule")
    if schedule != expected:
        raise ValidationError("manifest.schedule does not match ABBA contract v1")


def _validate_environment(value: Any) -> dict[str, Any]:
    environment = _dict(value, "manifest.environment")
    _require_keys(environment, {"contract", "identity"}, "manifest.environment")
    if environment["contract"] != ENVIRONMENT_CONTRACT:
        raise ValidationError(
            f"manifest.environment.contract must be {ENVIRONMENT_CONTRACT}"
        )
    return _validate_environment_identity(
        environment["identity"], "manifest.environment.identity"
    )


def _validate_scenario(value: Any) -> tuple[str, str, str, str]:
    scenario = _dict(value, "manifest.scenario")
    _require_keys(
        scenario,
        {
            "id",
            "workload",
            "measurementPath",
            "expectation",
            "expectedDecisionReason",
        },
        "manifest.scenario",
    )
    scenario_id = _string(scenario["id"], "scenario.id")
    contract = SCENARIO_CONTRACTS.get(scenario_id)
    if contract is None:
        raise ValidationError("scenario.id is unsupported")
    if scenario["workload"] != contract["workload"]:
        raise ValidationError(
            f"scenario.workload must be {contract['workload']} for {scenario_id}"
        )
    measurement_path = _string(
        scenario["measurementPath"], "scenario.measurementPath"
    )
    if measurement_path not in {"primary", "recording-rebuild"}:
        raise ValidationError("scenario.measurementPath is unsupported")
    expectation = _string(scenario["expectation"], "scenario.expectation")
    reason = scenario["expectedDecisionReason"]
    if expectation == "applied":
        if reason != "applied":
            raise ValidationError("applied scenario must expect reason applied")
        return scenario_id, measurement_path, expectation, "applied"
    if expectation != "fallback" or type(reason) is not str:
        raise ValidationError("scenario.expectation must be applied or fallback")
    if reason not in DECISION_REASONS - {"applied", "disabled"}:
        raise ValidationError("fallback expectedDecisionReason is unsupported")
    return scenario_id, measurement_path, expectation, reason


def _validate_manifest(
    root: Path,
) -> tuple[dict[str, Any], dict[str, Any], str, str, str, str]:
    manifest = _load_json(root / "capture.json")
    _require_keys(
        manifest,
        {
            "schemaVersion",
            "kind",
            "captureStatus",
            "revision",
            "workingTreeDirty",
            "capturedAtUtc",
            "executable",
            "environment",
            "configuration",
            "scenario",
            "capabilities",
            "schedule",
            "runs",
        },
        "manifest",
    )
    if (
        _integer(manifest["schemaVersion"], "manifest.schemaVersion")
        != MANIFEST_SCHEMA_VERSION
    ):
        raise ValidationError(
            f"manifest.schemaVersion must be {MANIFEST_SCHEMA_VERSION}"
        )
    if manifest["kind"] != CAPTURE_KIND:
        raise ValidationError(f"manifest.kind must be {CAPTURE_KIND}")
    if manifest["captureStatus"] != "captured":
        raise ValidationError("manifest.captureStatus must be captured")
    revision = _string(manifest["revision"], "manifest.revision")
    if REVISION_PATTERN.fullmatch(revision) is None:
        raise ValidationError("manifest.revision must be a lowercase commit SHA")
    if _boolean(manifest["workingTreeDirty"], "manifest.workingTreeDirty"):
        raise ValidationError("official ROI A/B evidence requires a clean tree")
    _string(manifest["capturedAtUtc"], "manifest.capturedAtUtc")
    environment_identity = _validate_environment(manifest["environment"])
    _validate_capabilities(manifest["capabilities"])
    _validate_schedule(manifest["schedule"])
    scenario_id, measurement_path, expectation, reason = _validate_scenario(
        manifest["scenario"]
    )
    return (
        manifest,
        environment_identity,
        scenario_id,
        measurement_path,
        expectation,
        reason,
    )


def _validate_executable(root: Path, manifest: dict[str, Any]) -> str:
    executable = _dict(manifest["executable"], "manifest.executable")
    _require_keys(executable, {"fileName", "sha256"}, "manifest.executable")
    if executable["fileName"] != "ba-click-fx-desktop.exe":
        raise ValidationError("manifest executable has the wrong file name")
    digest = _string(executable["sha256"], "manifest.executable.sha256")
    if SHA256_PATTERN.fullmatch(digest) is None:
        raise ValidationError("manifest executable SHA-256 is malformed")
    return digest


def _validate_configuration_contract(
    root: Path, manifest: dict[str, Any]
) -> tuple[str, str]:
    contract = _dict(manifest["configuration"], "manifest.configuration")
    _require_keys(
        contract,
        {"schemaVersion", "baseConfig", "baseSha256", "differenceContract"},
        "manifest.configuration",
    )
    if _integer(contract["schemaVersion"], "configuration.schemaVersion") != 19:
        raise ValidationError("configuration.schemaVersion must be 19")
    if contract["differenceContract"] != "performance.activeFxRoiEnabled-only":
        raise ValidationError("configuration difference contract is invalid")
    base_path = _relative_file(root, contract["baseConfig"], "configuration.baseConfig")
    base_digest = _string(contract["baseSha256"], "configuration.baseSha256")
    if SHA256_PATTERN.fullmatch(base_digest) is None or _sha256(base_path) != base_digest:
        raise ValidationError("base configuration SHA-256 mismatch")
    base_config = _load_json(base_path)
    return _canonical_without_roi(base_config, "base configuration"), base_digest


def _intervals(
    events: list[dict[str, str]], path: Path, roi_enabled: bool
) -> list[dict[str, str]]:
    exited = [event for event in events if event.get("Event.Name") == "Process.Exited"]
    if len(exited) != 1:
        raise ValidationError(f"{path}: expected one Process.Exited event")
    complete = [
        event
        for event in events
        if event.get("Event.Name") == "Performance.Interval"
        and not _event_bool(event, "Window.Final", str(path))
    ]
    expected_count = DISCARD_COMPLETE_INTERVALS + SELECT_COMPLETE_INTERVALS
    if len(complete) != expected_count:
        raise ValidationError(
            f"{path}: expected {expected_count} complete intervals, found {len(complete)}"
        )
    selected = complete[DISCARD_COMPLETE_INTERVALS:]
    duration_us = 0
    for index, event in enumerate(selected, 1):
        context = f"{path} selected interval {index}"
        duration = _event_int(event, "Window.DurationUs", context)
        if duration < 8_000_000 or duration > 12_000_000:
            raise ValidationError(f"{context}: duration is outside the 10 s window")
        duration_us += duration
        if _event_int(event, "Configuration.SchemaVersion", context) != 19:
            raise ValidationError(f"{context}: config schema is not 19")
        if _event_bool(event, "Performance.ActiveFxRoiEnabled", context) != roi_enabled:
            raise ValidationError(f"{context}: ROI configuration does not match run")
        expected_path = (
            "active-fx-pyramid-with-full-screen-fallback"
            if roi_enabled
            else "disabled-full-screen"
        )
        if event.get("ROI.ProductionPath") != expected_path:
            raise ValidationError(f"{context}: ROI production path mismatch")
    if duration_us < 29_000_000 or duration_us > 32_000_000:
        raise ValidationError(f"{path}: selected windows do not provide a 30 s sample")
    return selected


def _run_metrics(
    intervals: list[dict[str, str]],
    measurement_path: str,
    expected_reason: str,
    context: str,
) -> dict[str, Any]:
    roi_prefix = (
        "ROI.Primary" if measurement_path == "primary" else "ROI.RecordingRebuild"
    )
    gpu_prefix = (
        "GPU.Primary" if measurement_path == "primary" else "GPU.RecordingRebuild"
    )

    def total(field: str) -> int:
        return sum(_event_int(event, field, context) for event in intervals)

    def maximum(field: str) -> int:
        return max(_event_int(event, field, context) for event in intervals)

    def median(field: str) -> float:
        return _median([_event_number(event, field, context) for event in intervals])

    duration_us = total("Window.DurationUs")
    frame_count = total("Window.FrameCount")
    if duration_us <= 0 or frame_count <= 0:
        raise ValidationError(f"{context}: selected sample has no presented frames")
    boundary_reason = expected_reason in {"boundary-fallback", "touches-boundary"}
    expected_reason_field = (
        "ROI.Active.Reason.boundary-fallback.Frames"
        if boundary_reason
        else f"{roi_prefix}.Reason.{expected_reason}.Frames"
    )
    metrics: dict[str, Any] = {
        "durationUs": duration_us,
        "frameCount": frame_count,
        "fps": frame_count * 1_000_000.0 / duration_us,
        "observedFrames": total(f"{roi_prefix}.ObservedFrames"),
        "requestedFrames": total(f"{roi_prefix}.RequestedFrames"),
        "eligibleFrames": total(f"{roi_prefix}.EligibleFrames"),
        "appliedFrames": total(f"{roi_prefix}.AppliedFrames"),
        "warmupFrames": total(f"{roi_prefix}.WarmupFrames"),
        "fullPixels": total(f"{roi_prefix}.FullPixels.Total"),
        "candidatePixels": total(f"{roi_prefix}.CandidatePixels.Total"),
        "drawnPixels": total(f"{roi_prefix}.DrawnPixels.Total"),
        "clearedPixels": total(f"{roi_prefix}.ClearedPixels.Total"),
        "prefilterFullPixels": total(
            f"{roi_prefix}.Stage.Prefilter.FullPixels.Total"
        ),
        "prefilterCandidatePixels": total(
            f"{roi_prefix}.Stage.Prefilter.CandidatePixels.Total"
        ),
        "prefilterDrawnPixels": total(
            f"{roi_prefix}.Stage.Prefilter.DrawnPixels.Total"
        ),
        "prefilterClearedPixels": total(
            f"{roi_prefix}.Stage.Prefilter.ClearedPixels.Total"
        ),
        "pyramidFullPixels": total(
            f"{roi_prefix}.Stage.Downsample.FullPixels.Total"
        )
        + total(f"{roi_prefix}.Stage.Upsample.FullPixels.Total"),
        "pyramidCandidatePixels": total(
            f"{roi_prefix}.Stage.Downsample.CandidatePixels.Total"
        )
        + total(f"{roi_prefix}.Stage.Upsample.CandidatePixels.Total"),
        "pyramidDrawnPixels": total(
            f"{roi_prefix}.Stage.Downsample.DrawnPixels.Total"
        )
        + total(f"{roi_prefix}.Stage.Upsample.DrawnPixels.Total"),
        "pyramidClearedPixels": total(
            f"{roi_prefix}.Stage.Downsample.ClearedPixels.Total"
        )
        + total(f"{roi_prefix}.Stage.Upsample.ClearedPixels.Total"),
        "resolveFullPixels": total(
            f"{roi_prefix}.Stage.Resolve.FullPixels.Total"
        ),
        "resolveCandidatePixels": total(
            f"{roi_prefix}.Stage.Resolve.CandidatePixels.Total"
        ),
        "resolveDrawnPixels": total(
            f"{roi_prefix}.Stage.Resolve.DrawnPixels.Total"
        ),
        "resolveClearedPixels": total(
            f"{roi_prefix}.Stage.Resolve.ClearedPixels.Total"
        ),
        "expectedReasonFrames": total(expected_reason_field),
        "reasonObservedFrames": (
            total("ROI.RequestedFrames")
            if boundary_reason
            else total(f"{roi_prefix}.ObservedFrames")
        ),
        "prefilterP95Us": median(f"{gpu_prefix}.Prefilter.P95"),
        "pyramidP95Us": median(f"{gpu_prefix}.Pyramid.P95"),
        "finalCompositeP95Us": median(f"{gpu_prefix}.FinalComposite.P95"),
        "bloomFinalP95Us": median("GPU.BloomAndFinalComposite.P95"),
        "cpuFrameP95Us": median("Cpu.FrameTotal.P95"),
        "cpuFrameP99Us": median("Cpu.FrameTotal.P99"),
        "cpuPresentP95Us": median("Cpu.PresentCall.P95"),
        "cpuPresentP99Us": median("Cpu.PresentCall.P99"),
        "gpuCommandP99Us": median("GPU.RenderCommandSpan.P99"),
        "gpuPendingMax": maximum("GPU.PendingFrames.Max"),
    }
    stages = ("prefilter", "pyramid", "resolve")
    for semantic in ("Full", "Candidate", "Drawn", "Cleared"):
        aggregate = metrics[f"{semantic.lower()}Pixels"]
        staged = sum(metrics[f"{stage}{semantic}Pixels"] for stage in stages)
        if aggregate != staged:
            raise ValidationError(
                f"{context}: aggregate {semantic.lower()} pixel total does not equal "
                "the staged total"
            )
    for stage in stages:
        full = metrics[f"{stage}FullPixels"]
        for semantic in ("Candidate", "Drawn", "Cleared"):
            if metrics[f"{stage}{semantic}Pixels"] > full:
                raise ValidationError(
                    f"{context}: {stage} {semantic.lower()} pixels exceed full pixels"
                )
    for field in ERROR_FIELDS:
        metrics[field] = total(field)
    return metrics


def _validate_run(
    root: Path,
    value: Any,
    ordinal: int,
    executable_sha256: str,
    normalized_config: str,
    environment_identity: dict[str, Any],
    scenario_id: str,
    measurement_path: str,
    expected_reason: str,
) -> dict[str, Any]:
    run = _dict(value, f"runs[{ordinal - 1}]")
    _require_keys(
        run,
        {
            "ordinal",
            "block",
            "position",
            "arm",
            "roiEnabled",
            "directory",
            "executable",
            "config",
            "log",
            "arguments",
            "startedAtUtc",
            "elapsedMs",
            "exitCode",
            "executableSha256",
            "configSha256",
        },
        f"run {ordinal}",
    )
    block = (ordinal - 1) // 4 + 1
    position = (ordinal - 1) % 4 + 1
    expected_arm, expected_enabled = ABBA_PATTERN[position - 1]
    if _integer(run["ordinal"], f"run {ordinal}.ordinal") != ordinal:
        raise ValidationError(f"run {ordinal}: ordinal mismatch")
    if _integer(run["block"], f"run {ordinal}.block") != block:
        raise ValidationError(f"run {ordinal}: block mismatch")
    if _integer(run["position"], f"run {ordinal}.position") != position:
        raise ValidationError(f"run {ordinal}: position mismatch")
    if run["arm"] != expected_arm:
        raise ValidationError(f"run {ordinal}: ABBA arm mismatch")
    roi_enabled = _boolean(run["roiEnabled"], f"run {ordinal}.roiEnabled")
    if roi_enabled != expected_enabled:
        raise ValidationError(f"run {ordinal}: ABBA ROI value mismatch")
    if _integer(run["exitCode"], f"run {ordinal}.exitCode") != 0:
        raise ValidationError(f"run {ordinal}: Host did not exit successfully")
    if _integer(run["elapsedMs"], f"run {ordinal}.elapsedMs") < HOST_DURATION_MS:
        raise ValidationError(f"run {ordinal}: elapsed time is shorter than workload")
    _string(run["startedAtUtc"], f"run {ordinal}.startedAtUtc")
    arguments = _list(run["arguments"], f"run {ordinal}.arguments")
    expected_arguments = [
        f"--demo-scenario={scenario_id}",
        "--demo-age-ms=130",
        "--demo-delay-ms=5000",
        "--disable-raw-input",
        "--quit-after-ms=40500",
    ]
    if measurement_path == "recording-rebuild":
        expected_arguments.append("--spout2")
    if arguments != expected_arguments:
        raise ValidationError(
            f"run {ordinal}: workload or measurement-path arguments mismatch"
        )

    directory = Path(_string(run["directory"], f"run {ordinal}.directory"))
    expected_directory = f"run-{ordinal:02d}-{expected_arm.lower()}-roi-{'on' if roi_enabled else 'off'}"
    if directory.as_posix() != expected_directory:
        raise ValidationError(f"run {ordinal}: directory name mismatch")
    executable_path = _relative_file(root, run["executable"], f"run {ordinal}.executable")
    config_path = _relative_file(root, run["config"], f"run {ordinal}.config")
    log_path = _relative_file(root, run["log"], f"run {ordinal}.log")
    for path in (executable_path, config_path, log_path):
        if directory not in path.relative_to(root.resolve()).parents:
            raise ValidationError(f"run {ordinal}: evidence file is outside its run directory")

    run_executable_digest = _string(
        run["executableSha256"], f"run {ordinal}.executableSha256"
    )
    if run_executable_digest != executable_sha256 or _sha256(executable_path) != executable_sha256:
        raise ValidationError(f"run {ordinal}: executable identity mismatch")
    run_config_digest = _string(run["configSha256"], f"run {ordinal}.configSha256")
    if SHA256_PATTERN.fullmatch(run_config_digest) is None or _sha256(config_path) != run_config_digest:
        raise ValidationError(f"run {ordinal}: configuration SHA-256 mismatch")
    config = _load_json(config_path)
    if _canonical_without_roi(config, f"run {ordinal} configuration") != normalized_config:
        raise ValidationError(
            f"run {ordinal}: configurations differ outside performance.activeFxRoiEnabled"
        )
    configured = _dict(config["performance"], f"run {ordinal}.performance")[
        "activeFxRoiEnabled"
    ]
    if configured != roi_enabled:
        raise ValidationError(f"run {ordinal}: configuration ROI value mismatch")
    events = _load_events(log_path)
    actual_environment_identity = _environment_identity_from_events(events, log_path)
    _require_same_environment_identity(
        environment_identity,
        actual_environment_identity,
        f"run {ordinal}",
    )
    intervals = _intervals(events, log_path, roi_enabled)
    return {
        "ordinal": ordinal,
        "block": block,
        "position": position,
        "arm": expected_arm,
        "roiEnabled": roi_enabled,
        "metrics": _run_metrics(
            intervals,
            measurement_path,
            expected_reason,
            f"run {ordinal}",
        ),
    }


def _aggregate(runs: list[dict[str, Any]]) -> dict[str, Any]:
    metrics = [run["metrics"] for run in runs]

    def total(name: str) -> int:
        return sum(int(item[name]) for item in metrics)

    def median(name: str) -> float:
        return _median([float(item[name]) for item in metrics])

    duration_us = total("durationUs")
    frame_count = total("frameCount")
    result: dict[str, Any] = {
        "runCount": len(runs),
        "durationUs": duration_us,
        "frameCount": frame_count,
        "fps": frame_count * 1_000_000.0 / duration_us,
        "observedFrames": total("observedFrames"),
        "requestedFrames": total("requestedFrames"),
        "eligibleFrames": total("eligibleFrames"),
        "appliedFrames": total("appliedFrames"),
        "warmupFrames": total("warmupFrames"),
        "fullPixels": total("fullPixels"),
        "candidatePixels": total("candidatePixels"),
        "drawnPixels": total("drawnPixels"),
        "clearedPixels": total("clearedPixels"),
        "prefilterFullPixels": total("prefilterFullPixels"),
        "prefilterCandidatePixels": total("prefilterCandidatePixels"),
        "prefilterDrawnPixels": total("prefilterDrawnPixels"),
        "prefilterClearedPixels": total("prefilterClearedPixels"),
        "pyramidFullPixels": total("pyramidFullPixels"),
        "pyramidCandidatePixels": total("pyramidCandidatePixels"),
        "pyramidDrawnPixels": total("pyramidDrawnPixels"),
        "pyramidClearedPixels": total("pyramidClearedPixels"),
        "resolveFullPixels": total("resolveFullPixels"),
        "resolveCandidatePixels": total("resolveCandidatePixels"),
        "resolveDrawnPixels": total("resolveDrawnPixels"),
        "resolveClearedPixels": total("resolveClearedPixels"),
        "expectedReasonFrames": total("expectedReasonFrames"),
        "reasonObservedFrames": total("reasonObservedFrames"),
        "prefilterP95Us": median("prefilterP95Us"),
        "pyramidP95Us": median("pyramidP95Us"),
        "finalCompositeP95Us": median("finalCompositeP95Us"),
        "bloomFinalP95Us": median("bloomFinalP95Us"),
        "cpuFrameP95Us": median("cpuFrameP95Us"),
        "cpuFrameP99Us": median("cpuFrameP99Us"),
        "cpuPresentP95Us": median("cpuPresentP95Us"),
        "cpuPresentP99Us": median("cpuPresentP99Us"),
        "gpuCommandP99Us": median("gpuCommandP99Us"),
        "gpuPendingMax": max(int(item["gpuPendingMax"]) for item in metrics),
    }
    result["appliedRequestedRatio"] = (
        result["appliedFrames"] / result["requestedFrames"]
        if result["requestedFrames"] > 0
        else None
    )
    result["prefilterCandidateFullRatio"] = (
        result["prefilterCandidatePixels"] / result["prefilterFullPixels"]
        if result["prefilterFullPixels"] > 0
        else None
    )
    result["prefilterDrawnFullRatio"] = (
        result["prefilterDrawnPixels"] / result["prefilterFullPixels"]
        if result["prefilterFullPixels"] > 0
        else None
    )
    result["pyramidCandidateFullRatio"] = (
        result["pyramidCandidatePixels"] / result["pyramidFullPixels"]
        if result["pyramidFullPixels"] > 0
        else None
    )
    result["pyramidDrawnFullRatio"] = (
        result["pyramidDrawnPixels"] / result["pyramidFullPixels"]
        if result["pyramidFullPixels"] > 0
        else None
    )
    result["expectedReasonRatio"] = (
        result["expectedReasonFrames"] / result["reasonObservedFrames"]
        if result["reasonObservedFrames"] > 0
        else None
    )
    result["errors"] = {field: total(field) for field in ERROR_FIELDS}
    result["errorCount"] = sum(result["errors"].values())
    return result


def _reduction(off: float, on: float) -> dict[str, float | None]:
    return {
        "absolute": off - on,
        "percent": (off - on) * 100.0 / off if off > 0.0 else None,
    }


def _regression_within(on: float, off: float, fraction: float, floor: float) -> bool:
    return on - off <= max(off * fraction, floor) + 1e-9


def _gate(identifier: str, passed: bool, actual: Any, required: str) -> dict[str, Any]:
    return {
        "id": identifier,
        "passed": bool(passed),
        "actual": actual,
        "required": required,
    }


def _paired_results(runs: list[dict[str, Any]]) -> dict[str, Any]:
    by_ordinal = {run["ordinal"]: run for run in runs}
    pairs: list[dict[str, Any]] = []
    for block in range(1, BLOCK_COUNT + 1):
        base = (block - 1) * 4
        for pair_index, (off_ordinal, on_ordinal) in enumerate(
            ((base + 1, base + 2), (base + 4, base + 3)), 1
        ):
            off = by_ordinal[off_ordinal]["metrics"]["bloomFinalP95Us"]
            on = by_ordinal[on_ordinal]["metrics"]["bloomFinalP95Us"]
            pairs.append(
                {
                    "block": block,
                    "pair": pair_index,
                    "offOrdinal": off_ordinal,
                    "onOrdinal": on_ordinal,
                    "offBloomFinalP95Us": off,
                    "onBloomFinalP95Us": on,
                    "roiNotSlower": on <= off + 1e-9,
                }
            )
    return {
        "count": len(pairs),
        "roiNotSlowerCount": sum(1 for pair in pairs if pair["roiNotSlower"]),
        "pairs": pairs,
    }


def _build_gates(
    off: dict[str, Any],
    on: dict[str, Any],
    paired: dict[str, Any],
    expectation: str,
) -> list[dict[str, Any]]:
    gates = [
        _gate(
            "gpu-pending-max",
            max(off["gpuPendingMax"], on["gpuPendingMax"]) <= 1,
            max(off["gpuPendingMax"], on["gpuPendingMax"]),
            "<= 1",
        ),
        _gate(
            "runtime-errors",
            off["errorCount"] == 0 and on["errorCount"] == 0,
            {"roiOff": off["errors"], "roiOn": on["errors"]},
            "all query, throttle, pacing and state error counters equal 0",
        ),
        _gate(
            "fps-regression",
            on["fps"] >= off["fps"] * 0.99,
            _reduction(off["fps"], on["fps"]),
            "ROI FPS decrease <= 1%",
        ),
    ]
    if expectation == "fallback":
        gates.extend(
            (
                _gate(
                    "fallback-reason-coverage",
                    on["expectedReasonRatio"] is not None
                    and on["expectedReasonRatio"] >= 1.0,
                    on["expectedReasonRatio"],
                    "100% of ROI-on observed frames use the expected fallback reason",
                ),
                _gate(
                    "fallback-not-applied",
                    on["appliedFrames"] == 0,
                    on["appliedFrames"],
                    "0 applied frames",
                ),
            )
        )
        for name in (
            "prefilterP95Us",
            "pyramidP95Us",
            "bloomFinalP95Us",
            "cpuFrameP95Us",
            "cpuFrameP99Us",
            "cpuPresentP95Us",
            "cpuPresentP99Us",
            "gpuCommandP99Us",
        ):
            gates.append(
                _gate(
                    f"fallback-{name}-regression",
                    _regression_within(on[name], off[name], 0.03, 100.0),
                    {"roiOff": off[name], "roiOn": on[name]},
                    "regression <= max(3%, 100 us)",
                )
            )
        return gates

    prefilter_reduction = _reduction(off["prefilterP95Us"], on["prefilterP95Us"])
    pyramid_reduction = _reduction(off["pyramidP95Us"], on["pyramidP95Us"])
    bloom_reduction = _reduction(off["bloomFinalP95Us"], on["bloomFinalP95Us"])
    gates.extend(
        (
            _gate(
                "applied-requested-ratio",
                on["appliedRequestedRatio"] is not None
                and on["appliedRequestedRatio"] >= 0.95,
                on["appliedRequestedRatio"],
                ">= 0.95",
            ),
            _gate(
                "prefilter-drawn-full-ratio",
                on["prefilterDrawnFullRatio"] is not None
                and on["prefilterDrawnFullRatio"] <= 0.45,
                on["prefilterDrawnFullRatio"],
                "<= 0.45",
            ),
            _gate(
                "prefilter-p95-reduction",
                prefilter_reduction["percent"] is not None
                and prefilter_reduction["percent"] >= 25.0,
                prefilter_reduction,
                ">= 25%",
            ),
            _gate(
                "pyramid-drawn-full-ratio",
                on["pyramidDrawnFullRatio"] is not None
                and on["pyramidDrawnFullRatio"] <= 0.45,
                on["pyramidDrawnFullRatio"],
                "<= 0.45",
            ),
            _gate(
                "pyramid-p95-reduction",
                pyramid_reduction["percent"] is not None
                and pyramid_reduction["percent"] >= 25.0,
                pyramid_reduction,
                ">= 25%",
            ),
            _gate(
                "bloom-final-p95-reduction",
                bloom_reduction["absolute"]
                >= max(off["bloomFinalP95Us"] * 0.05, 100.0),
                bloom_reduction,
                ">= max(5%, 100 us)",
            ),
            _gate(
                "paired-roi-not-slower",
                paired["roiNotSlowerCount"] >= 8,
                paired["roiNotSlowerCount"],
                ">= 8 of 10 adjacent ABBA pairs",
            ),
        )
    )
    for name in (
        "cpuFrameP95Us",
        "cpuFrameP99Us",
        "cpuPresentP95Us",
        "cpuPresentP99Us",
        "gpuCommandP99Us",
    ):
        gates.append(
            _gate(
                f"{name}-regression",
                on[name] <= off[name] * 1.05 + 1e-9,
                {"roiOff": off[name], "roiOn": on[name]},
                "ROI regression <= 5%",
            )
        )
    return gates


def build_report(root: Path) -> dict[str, Any]:
    root = root.resolve()
    (
        manifest,
        environment_identity,
        scenario_id,
        measurement_path,
        expectation,
        expected_reason,
    ) = _validate_manifest(root)
    executable_sha256 = _validate_executable(root, manifest)
    normalized_config, base_config_sha256 = _validate_configuration_contract(
        root, manifest
    )
    run_values = _list(manifest["runs"], "manifest.runs")
    if len(run_values) != RUN_COUNT:
        raise ValidationError(f"manifest.runs must contain exactly {RUN_COUNT} runs")
    runs = [
        _validate_run(
            root,
            value,
            ordinal,
            executable_sha256,
            normalized_config,
            environment_identity,
            scenario_id,
            measurement_path,
            expected_reason,
        )
        for ordinal, value in enumerate(run_values, 1)
    ]
    roi_off = _aggregate([run for run in runs if not run["roiEnabled"]])
    roi_on = _aggregate([run for run in runs if run["roiEnabled"]])
    paired = _paired_results(runs)
    gates = _build_gates(roi_off, roi_on, paired, expectation)
    return {
        "schemaVersion": REPORT_SCHEMA_VERSION,
        "kind": REPORT_KIND,
        "captureSchemaVersion": MANIFEST_SCHEMA_VERSION,
        "revision": manifest["revision"],
        "executableSha256": executable_sha256,
        "baseConfigSha256": base_config_sha256,
        "environment": {
            "contract": ENVIRONMENT_CONTRACT,
            "identity": environment_identity,
        },
        "scenario": manifest["scenario"],
        "schedule": manifest["schedule"],
        "aggregationSemantic": {
            "counts": "sum-selected-three-complete-10s-windows",
            "fps": "total-presented-frames-divided-by-total-duration",
            "percentiles": "median-of-three-window-percentiles-then-median-of-ten-runs",
            "pairs": "adjacent-A1-B1-and-B2-A2-within-each-ABBA-block",
        },
        "roiOff": roi_off,
        "roiOn": roi_on,
        "comparisons": {
            "prefilterP95": _reduction(
                roi_off["prefilterP95Us"], roi_on["prefilterP95Us"]
            ),
            "pyramidP95": _reduction(
                roi_off["pyramidP95Us"], roi_on["pyramidP95Us"]
            ),
            "bloomFinalP95": _reduction(
                roi_off["bloomFinalP95Us"], roi_on["bloomFinalP95Us"]
            ),
            "fps": _reduction(roi_off["fps"], roi_on["fps"]),
        },
        "paired": paired,
        "gates": gates,
        "passed": all(gate["passed"] for gate in gates),
        "limitations": [
            "Candidate coverage is planner output; drawn coverage is executed command coverage.",
            "Fallback pixel exactness is established by WARP tests, not inferred from timing logs.",
        ],
    }


def _format_number(value: Any) -> str:
    if value is None:
        return "unavailable"
    if isinstance(value, float):
        return f"{value:.3f}"
    return str(value)


def render_markdown(report: dict[str, Any]) -> str:
    scenario = report["scenario"]
    identity = report["environment"]["identity"]
    lines = [
        "# Active-FX ROI A/B report",
        "",
        f"- Result: `{'PASS' if report['passed'] else 'FAIL'}`",
        f"- Scenario: `{scenario['id']}` / `{scenario['expectation']}`",
        f"- Measurement path: `{scenario['measurementPath']}`",
        f"- Revision: `{report['revision']}`",
        f"- Adapter: `{identity['adapter']}` / driver `{identity['driverVersion']}`",
        f"- Output: `{identity['outputWidth']}x{identity['outputHeight']}` at "
        f"`{identity['refreshRateNumerator']}/{identity['refreshRateDenominator']} Hz`, "
        f"HDR `{'on' if identity['hdrEnabled'] else 'off'}`, "
        f"mapping `{identity['outputMapping']}`",
        "",
        "## Aggregate",
        "",
        "| Metric | ROI off | ROI on |",
        "|---|---:|---:|",
    ]
    off = report["roiOff"]
    on = report["roiOn"]
    for label, key in (
        ("Applied/requested", "appliedRequestedRatio"),
        ("Prefilter candidate/full", "prefilterCandidateFullRatio"),
        ("Prefilter drawn/full", "prefilterDrawnFullRatio"),
        ("Prefilter p95 (us)", "prefilterP95Us"),
        ("Pyramid candidate/full", "pyramidCandidateFullRatio"),
        ("Pyramid drawn/full", "pyramidDrawnFullRatio"),
        ("Pyramid p95 (us)", "pyramidP95Us"),
        ("Bloom/final p95 (us)", "bloomFinalP95Us"),
        ("FPS", "fps"),
        ("CPU frame p95 (us)", "cpuFrameP95Us"),
        ("CPU frame p99 (us)", "cpuFrameP99Us"),
        ("CPU Present p95 (us)", "cpuPresentP95Us"),
        ("CPU Present p99 (us)", "cpuPresentP99Us"),
        ("GPU command p99 (us)", "gpuCommandP99Us"),
        ("GPU pending max", "gpuPendingMax"),
        ("Error count", "errorCount"),
    ):
        lines.append(
            f"| {label} | {_format_number(off[key])} | {_format_number(on[key])} |"
        )
    lines.extend(("", "## Gates", ""))
    for gate in report["gates"]:
        lines.append(
            f"- `{'PASS' if gate['passed'] else 'FAIL'}` {gate['id']}: "
            f"actual `{_format_number(gate['actual'])}`, required {gate['required']}"
        )
    lines.extend(
        (
            "",
            f"Adjacent non-slow pairs: `{report['paired']['roiNotSlowerCount']}/10`.",
            "",
            "## Limitations",
            "",
        )
    )
    lines.extend(f"- {item}" for item in report["limitations"])
    lines.append("")
    return "\n".join(lines)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, help="Active-FX ROI ABBA capture directory")
    parser.add_argument("--json", type=Path, dest="json_path")
    parser.add_argument("--markdown", type=Path, dest="markdown_path")
    return parser.parse_args()


def main() -> int:
    arguments = _parse_args()
    try:
        report = build_report(arguments.root)
        json_path = arguments.json_path or arguments.root / "summary.json"
        markdown_path = arguments.markdown_path or arguments.root / "summary.md"
        json_path.write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        markdown_path.write_text(render_markdown(report), encoding="utf-8")
    except (OSError, ValidationError) as error:
        print(f"Active-FX ROI A/B validation failed: {error}", file=sys.stderr)
        return 1
    if not report["passed"]:
        print(f"Active-FX ROI A/B gates failed: {markdown_path}", file=sys.stderr)
        return 2
    print(f"Active-FX ROI A/B passed: {markdown_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
