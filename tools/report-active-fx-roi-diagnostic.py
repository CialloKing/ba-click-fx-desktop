#!/usr/bin/env python3
"""Validate and summarize a non-release Active-FX ROI diagnostic capture."""

from __future__ import annotations

import argparse
import csv
from datetime import datetime, timedelta, timezone
import importlib.util
import json
import math
from pathlib import Path
import re
import sys
from types import ModuleType
from typing import Any


CAPTURE_SCHEMA_VERSION = 2
REPORT_SCHEMA_VERSION = 2
CAPTURE_KIND = "bafx-active-fx-roi-diagnostic-capture"
REPORT_KIND = "bafx-active-fx-roi-diagnostic-report"
DIAGNOSTIC_NOTICE = "NON-RELEASE: short matrix for causal investigation only"
RUN_COUNT = 8
BLOCK_PATTERNS = (
    ("ABBA", (("A", False), ("B", True), ("B", True), ("A", False))),
    ("BAAB", (("B", True), ("A", False), ("A", False), ("B", True))),
)
TELEMETRY_INTERVAL_MS = 200
TELEMETRY_ENDPOINT_SLACK_MS = max(2_000, 10 * TELEMETRY_INTERVAL_MS)
TELEMETRY_MAX_GAP_MS = TELEMETRY_ENDPOINT_SLACK_MS
TELEMETRY_FIELDS = (
    "timestamp",
    "index",
    "uuid",
    "name",
    "pstate",
    "clocks.current.sm",
    "clocks.current.memory",
    "power.draw.instant",
    "temperature.gpu",
    "utilization.gpu",
    "utilization.memory",
)
TELEMETRY_RANGES = {
    "smClockMHz": "clocks.current.sm",
    "memoryClockMHz": "clocks.current.memory",
    "instantPowerWatts": "power.draw.instant",
    "temperatureCelsius": "temperature.gpu",
    "gpuUtilizationPercent": "utilization.gpu",
    "memoryUtilizationPercent": "utilization.memory",
}
BASE_RUN_FIELDS = {
    "ordinal",
    "block",
    "position",
    "blockPattern",
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
}
PRE_PRESENT_SEMANTIC = (
    "fx-render-return-to-Present-call-entry-including-roi-diagnostics-spout-"
    "gpu-query-end-and-readback"
)
FRAME_PACING_WAIT_SEMANTIC = (
    "owner-thread-qpc-around-waitForAnyFrameOpportunity-including-handle-prepoll-"
    "and-message-wait-excluding-wait-set-build-and-post-wake-work"
)
WAIT_WAKE_FIELDS = (
    "FramePacing.FrameReadyWakes",
    "FramePacing.DeviceRemovedWakes",
    "FramePacing.CadenceWakes",
    "FramePacing.MessageWakes",
    "FramePacing.Timeouts",
    "FramePacing.Failures",
)
WAIT_BUCKET_FIELDS = (
    ("lt100Us", "FramePacing.Wait.Lt100Us"),
    ("from100To999Us", "FramePacing.Wait.100To999Us"),
    ("from1000To3999Us", "FramePacing.Wait.1000To3999Us"),
    ("from4000To7999Us", "FramePacing.Wait.4000To7999Us"),
    ("ge8000Us", "FramePacing.Wait.Ge8000Us"),
)


def _load_release_reporter() -> ModuleType:
    # Reuse the release reporter's strict parser and metric semantics so the
    # diagnostic view cannot silently reinterpret the same raw evidence.
    path = Path(__file__).with_name("report-active-fx-roi-ab.py")
    spec = importlib.util.spec_from_file_location("bafx_roi_release_reporter", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load release reporter {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


RELEASE = _load_release_reporter()
ValidationError = RELEASE.ValidationError


def _validate_schedule(value: Any) -> None:
    schedule = RELEASE._dict(value, "manifest.schedule")
    expected = {
        "pattern": "ABBA+BAAB",
        "blockPatterns": ["ABBA", "BAAB"],
        "a": "roi-off",
        "b": "roi-on",
        "blocks": 2,
        "runs": RUN_COUNT,
        "warmupMs": RELEASE.WARMUP_MS,
        "sampleMs": RELEASE.SAMPLE_MS,
        "hostDurationMs": RELEASE.HOST_DURATION_MS,
        "performanceIntervalMs": RELEASE.PERFORMANCE_INTERVAL_MS,
        "discardCompleteIntervals": RELEASE.DISCARD_COMPLETE_INTERVALS,
        "selectCompleteIntervals": RELEASE.SELECT_COMPLETE_INTERVALS,
    }
    RELEASE._require_keys(schedule, set(expected), "manifest.schedule")
    if schedule != expected:
        raise ValidationError("manifest.schedule does not match diagnostic contract v2")


def _validate_gpu_identity(value: Any, context: str) -> dict[str, Any]:
    gpu = RELEASE._dict(value, context)
    RELEASE._require_keys(gpu, {"index", "uuid", "name"}, context)
    index = RELEASE._integer(gpu["index"], f"{context}.index")
    uuid = RELEASE._string(gpu["uuid"], f"{context}.uuid")
    name = RELEASE._string(gpu["name"], f"{context}.name")
    if index < 0:
        raise ValidationError(f"{context}.index must be non-negative")
    if re.fullmatch(r"GPU-[0-9A-Fa-f-]+", uuid) is None:
        raise ValidationError(f"{context}.uuid must be a physical GPU UUID")
    if "RTX 4060" not in name.upper():
        raise ValidationError(f"{context}.name must identify the RTX 4060")
    return {"index": index, "uuid": uuid, "name": name}


def _validate_telemetry_contract(value: Any) -> dict[str, Any]:
    telemetry = RELEASE._dict(value, "manifest.nvidiaTelemetry")
    enabled = RELEASE._boolean(
        telemetry.get("enabled"), "manifest.nvidiaTelemetry.enabled"
    )
    if not enabled:
        RELEASE._require_keys(telemetry, {"enabled"}, "manifest.nvidiaTelemetry")
        return {"enabled": False}

    expected_fields = {
        "enabled",
        "provider",
        "intervalMs",
        "fields",
        "executable",
        "gpu",
    }
    RELEASE._require_keys(telemetry, expected_fields, "manifest.nvidiaTelemetry")
    if telemetry["provider"] != "nvidia-smi":
        raise ValidationError("manifest.nvidiaTelemetry.provider must be nvidia-smi")
    if (
        RELEASE._integer(
            telemetry["intervalMs"], "manifest.nvidiaTelemetry.intervalMs"
        )
        != TELEMETRY_INTERVAL_MS
    ):
        raise ValidationError("manifest.nvidiaTelemetry.intervalMs must be 200")
    fields = RELEASE._list(telemetry["fields"], "manifest.nvidiaTelemetry.fields")
    if tuple(fields) != TELEMETRY_FIELDS:
        raise ValidationError("manifest.nvidiaTelemetry.fields differ from contract v2")

    executable = RELEASE._dict(
        telemetry["executable"], "manifest.nvidiaTelemetry.executable"
    )
    RELEASE._require_keys(
        executable,
        {"fileName", "sha256", "companyName", "fileDescription", "productVersion"},
        "manifest.nvidiaTelemetry.executable",
    )
    if executable["fileName"].lower() != "nvidia-smi.exe":
        raise ValidationError("NVIDIA telemetry executable name is invalid")
    digest = RELEASE._string(
        executable["sha256"], "manifest.nvidiaTelemetry.executable.sha256"
    )
    if RELEASE.SHA256_PATTERN.fullmatch(digest) is None:
        raise ValidationError("NVIDIA telemetry executable SHA-256 is malformed")
    if executable["companyName"] != "NVIDIA Corporation":
        raise ValidationError("NVIDIA telemetry executable company is invalid")
    description = RELEASE._string(
        executable["fileDescription"],
        "manifest.nvidiaTelemetry.executable.fileDescription",
    )
    if not description.startswith("NVIDIA-SMI"):
        raise ValidationError("NVIDIA telemetry executable description is invalid")
    RELEASE._string(
        executable["productVersion"],
        "manifest.nvidiaTelemetry.executable.productVersion",
    )
    return {
        "enabled": True,
        "provider": "nvidia-smi",
        "intervalMs": TELEMETRY_INTERVAL_MS,
        "fields": list(TELEMETRY_FIELDS),
        "executable": executable,
        "gpu": _validate_gpu_identity(
            telemetry["gpu"], "manifest.nvidiaTelemetry.gpu"
        ),
    }


def _validate_manifest(
    root: Path,
) -> tuple[dict[str, Any], dict[str, Any], str, str, str, dict[str, Any]]:
    manifest = RELEASE._load_json(root / "capture.json")
    expected_fields = {
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
        "releaseEligible",
        "diagnosticNotice",
        "nvidiaTelemetry",
    }
    RELEASE._require_keys(manifest, expected_fields, "manifest")
    if (
        RELEASE._integer(manifest["schemaVersion"], "manifest.schemaVersion")
        != CAPTURE_SCHEMA_VERSION
    ):
        raise ValidationError("manifest.schemaVersion must be 2")
    if manifest["kind"] != CAPTURE_KIND:
        raise ValidationError(f"manifest.kind must be {CAPTURE_KIND}")
    if manifest["captureStatus"] != "diagnostic-captured":
        raise ValidationError("manifest.captureStatus must be diagnostic-captured")
    if RELEASE._boolean(manifest["releaseEligible"], "manifest.releaseEligible"):
        raise ValidationError("diagnostic evidence must not be release eligible")
    if manifest["diagnosticNotice"] != DIAGNOSTIC_NOTICE:
        raise ValidationError("manifest.diagnosticNotice differs from contract v2")
    revision = RELEASE._string(manifest["revision"], "manifest.revision")
    if RELEASE.REVISION_PATTERN.fullmatch(revision) is None:
        raise ValidationError("manifest.revision must be a lowercase commit SHA")
    if RELEASE._boolean(manifest["workingTreeDirty"], "manifest.workingTreeDirty"):
        raise ValidationError("diagnostic comparison requires a clean tracked tree")
    RELEASE._string(manifest["capturedAtUtc"], "manifest.capturedAtUtc")
    environment_identity = RELEASE._validate_environment(manifest["environment"])
    RELEASE._validate_capabilities(manifest["capabilities"])
    _validate_schedule(manifest["schedule"])
    scenario_id, measurement_path, _, expected_reason = RELEASE._validate_scenario(
        manifest["scenario"]
    )
    telemetry = _validate_telemetry_contract(manifest["nvidiaTelemetry"])
    return (
        manifest,
        environment_identity,
        scenario_id,
        measurement_path,
        expected_reason,
        telemetry,
    )


def _number(raw: str, context: str) -> float:
    try:
        value = float(raw)
    except ValueError as error:
        raise ValidationError(f"{context} must be numeric") from error
    if not math.isfinite(value):
        raise ValidationError(f"{context} must be finite")
    return value


def _metric(
    event: dict[str, str], prefix: str, context: str
) -> dict[str, int | float]:
    if not RELEASE._event_bool(event, f"{prefix}.Available", context):
        raise ValidationError(f"{context}: {prefix} must be available")
    if RELEASE._event_string(event, f"{prefix}.Unit", context) != "us":
        raise ValidationError(f"{context}: {prefix}.Unit must be us")
    samples = RELEASE._event_int(event, f"{prefix}.Samples", context)
    recorded = RELEASE._event_int(event, f"{prefix}.RecordedSamples", context)
    dropped = RELEASE._event_int(event, f"{prefix}.DroppedSamples", context)
    if samples <= 0:
        raise ValidationError(f"{context}: {prefix}.Samples must be positive")
    if recorded != samples:
        raise ValidationError(
            f"{context}: {prefix}.RecordedSamples does not match Samples"
        )
    if dropped != 0:
        raise ValidationError(f"{context}: {prefix}.DroppedSamples must be zero")

    values = {
        name: RELEASE._event_number(event, f"{prefix}.{field}", context)
        for name, field in (
            ("minimum", "Min"),
            ("average", "Average"),
            ("p50", "P50"),
            ("p95", "P95"),
            ("p99", "P99"),
            ("maximum", "Max"),
        )
    }
    if any(value < 0.0 for value in values.values()):
        raise ValidationError(f"{context}: {prefix} values must be non-negative")
    if not (
        values["minimum"]
        <= values["p50"]
        <= values["p95"]
        <= values["p99"]
        <= values["maximum"]
        and values["minimum"] <= values["average"] <= values["maximum"]
    ):
        raise ValidationError(f"{context}: {prefix} distribution is inconsistent")
    return {
        "samples": samples,
        "recordedSamples": recorded,
        "droppedSamples": dropped,
        **values,
    }


def _causal_metrics(
    intervals: list[dict[str, str]], context: str
) -> dict[str, Any]:
    pre_present: list[dict[str, int | float]] = []
    waits: list[dict[str, int | float]] = []
    bucket_totals = {name: 0 for name, _ in WAIT_BUCKET_FIELDS}
    for index, event in enumerate(intervals, 1):
        interval_context = f"{context} selected interval {index}"
        if (
            RELEASE._event_string(
                event, "Timing.PrePresentSemantic", interval_context
            )
            != PRE_PRESENT_SEMANTIC
        ):
            raise ValidationError(
                f"{interval_context}: Timing.PrePresentSemantic differs from contract v2"
            )
        if (
            RELEASE._event_string(
                event, "Timing.FramePacingWaitSemantic", interval_context
            )
            != FRAME_PACING_WAIT_SEMANTIC
        ):
            raise ValidationError(
                f"{interval_context}: Timing.FramePacingWaitSemantic differs from contract v2"
            )

        pre = _metric(event, "Cpu.PrePresent", interval_context)
        frame_count = RELEASE._event_int(event, "Window.FrameCount", interval_context)
        if pre["samples"] != frame_count:
            raise ValidationError(
                f"{interval_context}: Cpu.PrePresent.Samples does not match "
                "Window.FrameCount"
            )
        wait = _metric(event, "FramePacing.Wait", interval_context)

        wake_count = 0
        for field in WAIT_WAKE_FIELDS:
            count = RELEASE._event_int(event, field, interval_context)
            if count < 0:
                raise ValidationError(f"{interval_context}: {field} must be non-negative")
            wake_count += count
        if wait["samples"] != wake_count:
            raise ValidationError(
                f"{interval_context}: FramePacing.Wait.Samples does not match wake count"
            )

        interval_bucket_count = 0
        for name, field in WAIT_BUCKET_FIELDS:
            count = RELEASE._event_int(event, field, interval_context)
            if count < 0:
                raise ValidationError(f"{interval_context}: {field} must be non-negative")
            interval_bucket_count += count
            bucket_totals[name] += count
        if wait["samples"] != interval_bucket_count:
            raise ValidationError(
                f"{interval_context}: FramePacing.Wait.Samples does not match bucket count"
            )
        pre_present.append(pre)
        waits.append(wait)

    wait_samples = sum(int(metric["samples"]) for metric in waits)
    return {
        "cpuPrePresentSamples": sum(
            int(metric["samples"]) for metric in pre_present
        ),
        "cpuPrePresentP50Us": RELEASE._median(
            [float(metric["p50"]) for metric in pre_present]
        ),
        "cpuPrePresentP95Us": RELEASE._median(
            [float(metric["p95"]) for metric in pre_present]
        ),
        "cpuPrePresentP99Us": RELEASE._median(
            [float(metric["p99"]) for metric in pre_present]
        ),
        "framePacingWaitSamples": wait_samples,
        "framePacingWaitP50Us": RELEASE._median(
            [float(metric["p50"]) for metric in waits]
        ),
        "framePacingWaitP95Us": RELEASE._median(
            [float(metric["p95"]) for metric in waits]
        ),
        "framePacingWaitP99Us": RELEASE._median(
            [float(metric["p99"]) for metric in waits]
        ),
        "framePacingWaitBuckets": bucket_totals,
        "framePacingWaitBucketRatios": {
            name: count / wait_samples for name, count in bucket_totals.items()
        },
    }


def _range(values: list[float]) -> dict[str, float]:
    return {"min": min(values), "max": max(values)}


def _parse_utc(value: Any, context: str) -> datetime:
    text = RELEASE._string(value, context)
    try:
        return datetime.strptime(text, "%Y-%m-%dT%H:%M:%S.%fZ").replace(
            tzinfo=timezone.utc
        )
    except ValueError as error:
        raise ValidationError(f"{context} must be a millisecond UTC timestamp") from error


def _delta_ms(left: datetime, right: datetime) -> float:
    return (left - right).total_seconds() * 1000.0


def _validate_run_telemetry(
    root: Path,
    value: Any,
    ordinal: int,
    directory: Path,
    contract: dict[str, Any],
    run_started_at: datetime,
    run_elapsed_ms: int,
) -> dict[str, Any]:
    context = f"run {ordinal}.nvidiaTelemetry"
    telemetry = RELEASE._dict(value, context)
    expected_fields = {
        "file",
        "stderr",
        "sha256",
        "samples",
        "intervalMs",
        "arguments",
        "startedAtUtc",
        "stoppedAtUtc",
        "collectorStoppedProcess",
    }
    RELEASE._require_keys(telemetry, expected_fields, context)
    csv_path = RELEASE._relative_file(root, telemetry["file"], f"{context}.file")
    stderr_path = RELEASE._relative_file(
        root, telemetry["stderr"], f"{context}.stderr"
    )
    for path in (csv_path, stderr_path):
        if directory not in path.relative_to(root.resolve()).parents:
            raise ValidationError(f"{context}: evidence file is outside the run directory")
    digest = RELEASE._string(telemetry["sha256"], f"{context}.sha256")
    if RELEASE.SHA256_PATTERN.fullmatch(digest) is None:
        raise ValidationError(f"{context}.sha256 is malformed")
    if RELEASE._sha256(csv_path) != digest:
        raise ValidationError(f"{context}: CSV SHA-256 mismatch")
    try:
        stderr = stderr_path.read_text(encoding="utf-8-sig")
    except (OSError, UnicodeError) as error:
        raise ValidationError(f"cannot read {stderr_path}: {error}") from error
    if stderr.strip():
        raise ValidationError(f"{context}: nvidia-smi stderr is not empty")
    sample_count = RELEASE._integer(telemetry["samples"], f"{context}.samples")
    if sample_count <= 0:
        raise ValidationError(f"{context}.samples must be positive")
    if (
        RELEASE._integer(telemetry["intervalMs"], f"{context}.intervalMs")
        != TELEMETRY_INTERVAL_MS
    ):
        raise ValidationError(f"{context}.intervalMs must be 200")
    expected_arguments = [
        f"--id={contract['gpu']['uuid']}",
        f"--query-gpu={','.join(TELEMETRY_FIELDS)}",
        "--format=csv,noheader,nounits",
        f"--loop-ms={TELEMETRY_INTERVAL_MS}",
    ]
    if RELEASE._list(telemetry["arguments"], f"{context}.arguments") != expected_arguments:
        raise ValidationError(f"{context}.arguments differ from contract v2")
    started_at = _parse_utc(telemetry["startedAtUtc"], f"{context}.startedAtUtc")
    stopped_at = _parse_utc(telemetry["stoppedAtUtc"], f"{context}.stoppedAtUtc")
    if stopped_at < started_at:
        raise ValidationError(f"{context}: stoppedAtUtc precedes startedAtUtc")
    run_stopped_at = run_started_at + timedelta(milliseconds=run_elapsed_ms)
    if abs(_delta_ms(started_at, run_started_at)) > TELEMETRY_ENDPOINT_SLACK_MS:
        raise ValidationError(f"{context}: startedAtUtc does not match run start")
    if abs(_delta_ms(stopped_at, run_stopped_at)) > TELEMETRY_ENDPOINT_SLACK_MS:
        raise ValidationError(f"{context}: stoppedAtUtc does not match run end")
    session_elapsed_ms = _delta_ms(stopped_at, started_at)
    minimum_samples = max(
        2, math.floor(session_elapsed_ms / (2 * TELEMETRY_INTERVAL_MS))
    )
    if sample_count < minimum_samples:
        raise ValidationError(
            f"{context}: {sample_count} samples are below the minimum "
            f"{minimum_samples} for {session_elapsed_ms:.0f} ms"
        )
    if not RELEASE._boolean(
        telemetry["collectorStoppedProcess"], f"{context}.collectorStoppedProcess"
    ):
        raise ValidationError(f"{context}: collector must own sampler shutdown")

    try:
        with csv_path.open("r", encoding="utf-8-sig", newline="") as stream:
            rows = list(csv.reader(stream))
    except (OSError, UnicodeError, csv.Error) as error:
        raise ValidationError(f"cannot read {csv_path}: {error}") from error
    if not rows or tuple(rows[0]) != TELEMETRY_FIELDS:
        raise ValidationError(f"{context}: CSV header differs from contract v2")
    samples = rows[1:]
    if len(samples) != sample_count:
        raise ValidationError(
            f"{context}: CSV has {len(samples)} samples, manifest records {sample_count}"
        )

    pstates: set[int] = set()
    numeric: dict[str, list[float]] = {
        field: [] for field in TELEMETRY_RANGES.values()
    }
    timestamps: list[datetime] = []
    timestamp_text: list[str] = []
    for row_index, row in enumerate(samples, 1):
        row_context = f"{context} sample {row_index}"
        if len(row) != len(TELEMETRY_FIELDS) or any(not item.strip() for item in row):
            raise ValidationError(f"{row_context} must contain eleven non-empty fields")
        sample = dict(zip(TELEMETRY_FIELDS, (item.strip() for item in row)))
        try:
            local_timestamp = datetime.strptime(
                sample["timestamp"], "%Y/%m/%d %H:%M:%S.%f"
            )
        except ValueError as error:
            raise ValidationError(f"{row_context}.timestamp has an invalid format") from error
        # nvidia-smi emits local wall-clock timestamps without an offset. The
        # collector and reporter both bind them to the machine's local zone.
        timestamp = local_timestamp.astimezone(timezone.utc)
        if timestamps and timestamp < timestamps[-1]:
            raise ValidationError(f"{row_context}.timestamp moves backwards")
        timestamps.append(timestamp)
        timestamp_text.append(sample["timestamp"])
        try:
            gpu_index = int(sample["index"])
        except ValueError as error:
            raise ValidationError(f"{row_context}.index must be an integer") from error
        gpu = contract["gpu"]
        if (
            gpu_index != gpu["index"]
            or sample["uuid"] != gpu["uuid"]
            or sample["name"] != gpu["name"]
        ):
            raise ValidationError(f"{row_context}: GPU identity changed")
        match = re.fullmatch(r"P([0-9]+)", sample["pstate"])
        if match is None:
            raise ValidationError(f"{row_context}.pstate is invalid")
        pstates.add(int(match.group(1)))
        for field in numeric:
            numeric[field].append(_number(sample[field], f"{row_context}.{field}"))

    first_timestamp = timestamps[0]
    last_timestamp = timestamps[-1]
    if abs(_delta_ms(first_timestamp, started_at)) > TELEMETRY_ENDPOINT_SLACK_MS:
        raise ValidationError(f"{context}: first CSV sample is outside start slack")
    if abs(_delta_ms(last_timestamp, stopped_at)) > TELEMETRY_ENDPOINT_SLACK_MS:
        raise ValidationError(f"{context}: last CSV sample is outside stop slack")
    coverage_span_ms = _delta_ms(last_timestamp, first_timestamp)
    minimum_span_ms = max(0.0, session_elapsed_ms - 2 * TELEMETRY_ENDPOINT_SLACK_MS)
    if coverage_span_ms < minimum_span_ms:
        raise ValidationError(
            f"{context}: CSV span {coverage_span_ms:.0f} ms is below the minimum "
            f"{minimum_span_ms:.0f} ms"
        )
    maximum_gap_ms = max(
        _delta_ms(current, previous)
        for previous, current in zip(timestamps, timestamps[1:])
    )
    if maximum_gap_ms > TELEMETRY_MAX_GAP_MS:
        raise ValidationError(
            f"{context}: CSV gap {maximum_gap_ms:.0f} ms exceeds "
            f"{TELEMETRY_MAX_GAP_MS} ms"
        )

    ordered_pstates = sorted(pstates)
    result: dict[str, Any] = {
        "samples": sample_count,
        "timestamp": {
            "first": timestamp_text[0],
            "last": timestamp_text[-1],
            "spanMs": coverage_span_ms,
            "maximumGapMs": maximum_gap_ms,
            "minimumSamples": minimum_samples,
            "endpointSlackMs": TELEMETRY_ENDPOINT_SLACK_MS,
        },
        "pstate": {
            "min": f"P{ordered_pstates[0]}",
            "max": f"P{ordered_pstates[-1]}",
            "values": [f"P{value}" for value in ordered_pstates],
        },
    }
    for output_name, field in TELEMETRY_RANGES.items():
        result[output_name] = _range(numeric[field])
    return result


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
    telemetry_contract: dict[str, Any],
) -> dict[str, Any]:
    context = f"run {ordinal}"
    run = RELEASE._dict(value, f"runs[{ordinal - 1}]")
    expected_fields = set(BASE_RUN_FIELDS)
    if telemetry_contract["enabled"]:
        expected_fields.add("nvidiaTelemetry")
    RELEASE._require_keys(run, expected_fields, context)

    block = (ordinal - 1) // 4 + 1
    position = (ordinal - 1) % 4 + 1
    pattern_name, pattern = BLOCK_PATTERNS[block - 1]
    expected_arm, expected_enabled = pattern[position - 1]
    if RELEASE._integer(run["ordinal"], f"{context}.ordinal") != ordinal:
        raise ValidationError(f"{context}: ordinal mismatch")
    if RELEASE._integer(run["block"], f"{context}.block") != block:
        raise ValidationError(f"{context}: block mismatch")
    if RELEASE._integer(run["position"], f"{context}.position") != position:
        raise ValidationError(f"{context}: position mismatch")
    if run["blockPattern"] != pattern_name:
        raise ValidationError(f"{context}: block pattern mismatch")
    if run["arm"] != expected_arm:
        raise ValidationError(f"{context}: {pattern_name} arm mismatch")
    roi_enabled = RELEASE._boolean(run["roiEnabled"], f"{context}.roiEnabled")
    if roi_enabled != expected_enabled:
        raise ValidationError(f"{context}: {pattern_name} ROI value mismatch")
    if RELEASE._integer(run["exitCode"], f"{context}.exitCode") != 0:
        raise ValidationError(f"{context}: Host did not exit successfully")
    run_elapsed_ms = RELEASE._integer(run["elapsedMs"], f"{context}.elapsedMs")
    if run_elapsed_ms < RELEASE.HOST_DURATION_MS:
        raise ValidationError(f"{context}: elapsed time is shorter than workload")
    run_started_at = _parse_utc(run["startedAtUtc"], f"{context}.startedAtUtc")
    expected_arguments = [
        f"--demo-scenario={scenario_id}",
        "--demo-age-ms=130",
        "--demo-delay-ms=5000",
        "--disable-raw-input",
        "--quit-after-ms=40500",
    ]
    if measurement_path == "recording-rebuild":
        expected_arguments.append("--spout2")
    if RELEASE._list(run["arguments"], f"{context}.arguments") != expected_arguments:
        raise ValidationError(f"{context}: workload or measurement-path arguments mismatch")

    directory = Path(RELEASE._string(run["directory"], f"{context}.directory"))
    expected_directory = (
        f"run-{ordinal:02d}-{expected_arm.lower()}-roi-"
        f"{'on' if roi_enabled else 'off'}"
    )
    if directory.as_posix() != expected_directory:
        raise ValidationError(f"{context}: directory name mismatch")
    executable_path = RELEASE._relative_file(
        root, run["executable"], f"{context}.executable"
    )
    config_path = RELEASE._relative_file(root, run["config"], f"{context}.config")
    log_path = RELEASE._relative_file(root, run["log"], f"{context}.log")
    for path in (executable_path, config_path, log_path):
        if directory not in path.relative_to(root.resolve()).parents:
            raise ValidationError(f"{context}: evidence file is outside its run directory")

    run_executable_digest = RELEASE._string(
        run["executableSha256"], f"{context}.executableSha256"
    )
    if (
        run_executable_digest != executable_sha256
        or RELEASE._sha256(executable_path) != executable_sha256
    ):
        raise ValidationError(f"{context}: executable identity mismatch")
    run_config_digest = RELEASE._string(
        run["configSha256"], f"{context}.configSha256"
    )
    if (
        RELEASE.SHA256_PATTERN.fullmatch(run_config_digest) is None
        or RELEASE._sha256(config_path) != run_config_digest
    ):
        raise ValidationError(f"{context}: configuration SHA-256 mismatch")
    config = RELEASE._load_json(config_path)
    if (
        RELEASE._canonical_without_roi(config, f"{context} configuration")
        != normalized_config
    ):
        raise ValidationError(
            f"{context}: configurations differ outside performance.activeFxRoiEnabled"
        )
    configured = RELEASE._dict(config["performance"], f"{context}.performance")[
        "activeFxRoiEnabled"
    ]
    if configured != roi_enabled:
        raise ValidationError(f"{context}: configuration ROI value mismatch")

    events = RELEASE._load_events(log_path)
    actual_environment = RELEASE._environment_identity_from_events(events, log_path)
    RELEASE._require_same_environment_identity(
        environment_identity, actual_environment, context
    )
    intervals = RELEASE._intervals(events, log_path, roi_enabled, measurement_path)
    metrics = RELEASE._run_metrics(
        intervals, measurement_path, expected_reason, context
    )
    metrics.update(_causal_metrics(intervals, context))
    gpu_prefix = (
        "GPU.Primary" if measurement_path == "primary" else "GPU.RecordingRebuild"
    )
    metrics["finalCompositeP99Us"] = RELEASE._median(
        [
            RELEASE._event_number(
                event, f"{gpu_prefix}.FinalComposite.P99", context
            )
            for event in intervals
        ]
    )
    telemetry = None
    if telemetry_contract["enabled"]:
        telemetry = _validate_run_telemetry(
            root,
            run["nvidiaTelemetry"],
            ordinal,
            directory,
            telemetry_contract,
            run_started_at,
            run_elapsed_ms,
        )
    return {
        "ordinal": ordinal,
        "block": block,
        "position": position,
        "blockPattern": pattern_name,
        "arm": expected_arm,
        "roiEnabled": roi_enabled,
        "metrics": metrics,
        "nvidiaTelemetry": telemetry,
    }


def _aggregate(runs: list[dict[str, Any]]) -> dict[str, Any]:
    aggregate = RELEASE._aggregate(runs)
    aggregate["finalCompositeP99Us"] = RELEASE._median(
        [float(run["metrics"]["finalCompositeP99Us"]) for run in runs]
    )
    for name in (
        "cpuPrePresentP50Us",
        "cpuPrePresentP95Us",
        "cpuPrePresentP99Us",
        "framePacingWaitP50Us",
        "framePacingWaitP95Us",
        "framePacingWaitP99Us",
    ):
        aggregate[name] = RELEASE._median(
            [float(run["metrics"][name]) for run in runs]
        )
    aggregate["cpuPrePresentSamples"] = sum(
        int(run["metrics"]["cpuPrePresentSamples"]) for run in runs
    )
    wait_samples = sum(
        int(run["metrics"]["framePacingWaitSamples"]) for run in runs
    )
    aggregate["framePacingWaitSamples"] = wait_samples
    buckets = {
        name: sum(
            int(run["metrics"]["framePacingWaitBuckets"][name]) for run in runs
        )
        for name, _ in WAIT_BUCKET_FIELDS
    }
    aggregate["framePacingWaitBuckets"] = buckets
    aggregate["framePacingWaitBucketRatios"] = {
        name: count / wait_samples for name, count in buckets.items()
    }
    return aggregate


def _comparison(off: dict[str, Any], on: dict[str, Any]) -> dict[str, Any]:
    keys = (
        "bloomFinalP95Us",
        "finalCompositeP95Us",
        "finalCompositeP99Us",
        "gpuCommandP99Us",
        "cpuFrameP95Us",
        "cpuFrameP99Us",
        "cpuPresentP95Us",
        "cpuPresentP99Us",
        "cpuPrePresentP50Us",
        "cpuPrePresentP95Us",
        "cpuPrePresentP99Us",
        "framePacingWaitP50Us",
        "framePacingWaitP95Us",
        "framePacingWaitP99Us",
    )
    return {key: RELEASE._reduction(float(off[key]), float(on[key])) for key in keys}


def build_report(root: Path) -> dict[str, Any]:
    root = root.resolve()
    (
        manifest,
        environment_identity,
        scenario_id,
        measurement_path,
        expected_reason,
        telemetry_contract,
    ) = _validate_manifest(root)
    executable_sha256 = RELEASE._validate_executable(root, manifest)
    normalized_config, base_config_sha256 = RELEASE._validate_configuration_contract(
        root, manifest
    )
    values = RELEASE._list(manifest["runs"], "manifest.runs")
    if len(values) != RUN_COUNT:
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
            telemetry_contract,
        )
        for ordinal, value in enumerate(values, 1)
    ]
    roi_off = _aggregate([run for run in runs if not run["roiEnabled"]])
    roi_on = _aggregate([run for run in runs if run["roiEnabled"]])
    return {
        "schemaVersion": REPORT_SCHEMA_VERSION,
        "kind": REPORT_KIND,
        "captureSchemaVersion": CAPTURE_SCHEMA_VERSION,
        "releaseEligible": False,
        "diagnosticNotice": DIAGNOSTIC_NOTICE,
        "revision": manifest["revision"],
        "executableSha256": executable_sha256,
        "baseConfigSha256": base_config_sha256,
        "environment": manifest["environment"],
        "scenario": manifest["scenario"],
        "schedule": manifest["schedule"],
        "nvidiaTelemetry": telemetry_contract,
        "aggregationSemantic": {
            "runPercentiles": "median-of-three-selected-complete-10s-windows",
            "armPercentiles": "median-of-four-run-percentiles",
            "waitBuckets": "summed-selected-window-counts-divided-by-summed-wait-samples",
            "order": "ABBA block followed by BAAB block",
        },
        "runs": runs,
        "roiOff": roi_off,
        "roiOn": roi_on,
        "comparisons": _comparison(roi_off, roi_on),
        "limitations": [
            "This eight-run report is causal diagnostic evidence, not a release gate.",
            "The five-block, twenty-run release reporter must be run independently.",
            "FramePacing.Wait samples are wakeups and do not correspond one-to-one with presented frames.",
            "Window percentiles are distribution summaries and must not be subtracted to infer per-frame stage duration.",
        ],
    }


def _format_number(value: Any) -> str:
    if value is None:
        return "unavailable"
    if isinstance(value, float):
        return f"{value:.3f}"
    return str(value)


def _format_range(value: dict[str, Any] | None, unit: str = "") -> str:
    if value is None:
        return "unavailable"
    return f"{_format_number(value['min'])}-{_format_number(value['max'])}{unit}"


def _format_reduction(value: dict[str, Any]) -> str:
    percent = value["percent"]
    percent_text = "unavailable" if percent is None else f"{percent:.3f}%"
    return f"{_format_number(value['absolute'])} us / {percent_text}"


def _format_ratio(value: float) -> str:
    return f"{value * 100.0:.3f}%"


def render_markdown(report: dict[str, Any]) -> str:
    identity = report["environment"]["identity"]
    scenario = report["scenario"]
    path_label = (
        "Primary"
        if scenario["measurementPath"] == "primary"
        else "RecordingRebuild"
    )
    lines = [
        "# Active-FX ROI non-release diagnostic",
        "",
        f"> {report['diagnosticNotice']}",
        "",
        f"- Sequence: `{report['schedule']['pattern']}` / `{len(report['runs'])}` runs",
        f"- Scenario: `{scenario['id']}` / `{scenario['measurementPath']}`",
        f"- Revision: `{report['revision']}`",
        f"- Adapter: `{identity['adapter']}` / driver `{identity['driverVersion']}`",
        "",
        "## ROI arm aggregate",
        "",
        "| Metric | ROI off | ROI on |",
        "|---|---:|---:|",
    ]
    off = report["roiOff"]
    on = report["roiOn"]
    for label, key in (
        ("Bloom/final p95 (us)", "bloomFinalP95Us"),
        (f"{path_label} FinalComposite p95 (us)", "finalCompositeP95Us"),
        (f"{path_label} FinalComposite p99 (us)", "finalCompositeP99Us"),
        ("GPU RenderCommandSpan p99 (us)", "gpuCommandP99Us"),
        ("CPU frame p95 (us)", "cpuFrameP95Us"),
        ("CPU frame p99 (us)", "cpuFrameP99Us"),
        ("CPU Present p95 (us)", "cpuPresentP95Us"),
        ("CPU Present p99 (us)", "cpuPresentP99Us"),
    ):
        lines.append(
            f"| {label} | {_format_number(off[key])} | {_format_number(on[key])} |"
        )

    lines.extend(
        (
            "",
            "## Causal timing",
            "",
            "| Metric | ROI off | ROI on | Reduction |",
            "|---|---:|---:|---:|",
        )
    )
    for label, key in (
        ("CPU PrePresent p50 (us)", "cpuPrePresentP50Us"),
        ("CPU PrePresent p95 (us)", "cpuPrePresentP95Us"),
        ("CPU PrePresent p99 (us)", "cpuPrePresentP99Us"),
        ("Frame pacing wait p50 (us)", "framePacingWaitP50Us"),
        ("Frame pacing wait p95 (us)", "framePacingWaitP95Us"),
        ("Frame pacing wait p99 (us)", "framePacingWaitP99Us"),
    ):
        lines.append(
            f"| {label} | {_format_number(off[key])} | {_format_number(on[key])} | "
            f"{_format_reduction(report['comparisons'][key])} |"
        )

    lines.extend(
        (
            "",
            "## Frame pacing wait buckets",
            "",
            "| Wait | ROI off count | ROI off share | ROI on count | ROI on share |",
            "|---|---:|---:|---:|---:|",
        )
    )
    for label, key in (
        ("<100 us", "lt100Us"),
        ("100-999 us", "from100To999Us"),
        ("1000-3999 us", "from1000To3999Us"),
        ("4000-7999 us", "from4000To7999Us"),
        (">=8000 us", "ge8000Us"),
    ):
        lines.append(
            f"| {label} | {off['framePacingWaitBuckets'][key]} | "
            f"{_format_ratio(off['framePacingWaitBucketRatios'][key])} | "
            f"{on['framePacingWaitBuckets'][key]} | "
            f"{_format_ratio(on['framePacingWaitBucketRatios'][key])} |"
        )

    lines.extend(
        (
            "",
            "## Causal timing by run",
            "",
            "| Run | Pattern/arm | PrePresent p50/p95/p99 | "
            "Wait p50/p95/p99 | Wait samples |",
            "|---:|---|---:|---:|---:|",
        )
    )
    for run in report["runs"]:
        metrics = run["metrics"]
        lines.append(
            f"| {run['ordinal']} | {run['blockPattern']}/{run['arm']} | "
            f"{_format_number(metrics['cpuPrePresentP50Us'])}/"
            f"{_format_number(metrics['cpuPrePresentP95Us'])}/"
            f"{_format_number(metrics['cpuPrePresentP99Us'])} | "
            f"{_format_number(metrics['framePacingWaitP50Us'])}/"
            f"{_format_number(metrics['framePacingWaitP95Us'])}/"
            f"{_format_number(metrics['framePacingWaitP99Us'])} | "
            f"{metrics['framePacingWaitSamples']} |"
        )

    lines.extend(
        (
            "",
            "## Ordered runs",
            "",
            "| Run | Block | Pattern | Arm | ROI | Bloom/final p95 | "
            "FinalComposite p95 | FinalComposite p99 | RenderCommandSpan p99 | "
            "CPU frame p95/p99 | Present p95/p99 |",
            "|---:|---:|---|---|---|---:|---:|---:|---:|---:|---:|",
        )
    )
    for run in report["runs"]:
        metrics = run["metrics"]
        lines.append(
            f"| {run['ordinal']} | {run['block']} | {run['blockPattern']} | "
            f"{run['arm']} | {'on' if run['roiEnabled'] else 'off'} | "
            f"{_format_number(metrics['bloomFinalP95Us'])} | "
            f"{_format_number(metrics['finalCompositeP95Us'])} | "
            f"{_format_number(metrics['finalCompositeP99Us'])} | "
            f"{_format_number(metrics['gpuCommandP99Us'])} | "
            f"{_format_number(metrics['cpuFrameP95Us'])}/"
            f"{_format_number(metrics['cpuFrameP99Us'])} | "
            f"{_format_number(metrics['cpuPresentP95Us'])}/"
            f"{_format_number(metrics['cpuPresentP99Us'])} |"
        )

    lines.extend(("", "## NVIDIA telemetry by run", ""))
    if not report["nvidiaTelemetry"]["enabled"]:
        lines.extend(("NVIDIA telemetry was not captured.", ""))
    else:
        lines.extend(
            (
                "| Run | Pattern/arm | Samples | P-state | SM clock | "
                "Memory clock | Instant power | GPU util | Memory util | Temperature |",
                "|---:|---|---:|---|---:|---:|---:|---:|---:|---:|",
            )
        )
        for run in report["runs"]:
            telemetry = run["nvidiaTelemetry"]
            lines.append(
                f"| {run['ordinal']} | {run['blockPattern']}/{run['arm']} | "
                f"{telemetry['samples']} | "
                f"{telemetry['pstate']['min']}-{telemetry['pstate']['max']} | "
                f"{_format_range(telemetry['smClockMHz'], ' MHz')} | "
                f"{_format_range(telemetry['memoryClockMHz'], ' MHz')} | "
                f"{_format_range(telemetry['instantPowerWatts'], ' W')} | "
                f"{_format_range(telemetry['gpuUtilizationPercent'], '%')} | "
                f"{_format_range(telemetry['memoryUtilizationPercent'], '%')} | "
                f"{_format_range(telemetry['temperatureCelsius'], ' C')} |"
            )
        lines.append("")
    lines.extend(("## Limitations", ""))
    lines.extend(f"- {item}" for item in report["limitations"])
    lines.append("")
    return "\n".join(lines)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, help="diagnostic capture directory")
    parser.add_argument("--json", type=Path, dest="json_path")
    parser.add_argument("--markdown", type=Path, dest="markdown_path")
    return parser.parse_args()


def main() -> int:
    arguments = _parse_args()
    try:
        report = build_report(arguments.root)
        json_path = arguments.json_path or arguments.root / "diagnostic-summary.json"
        markdown_path = (
            arguments.markdown_path or arguments.root / "diagnostic-summary.md"
        )
        json_path.write_text(
            json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
            encoding="utf-8",
        )
        markdown_path.write_text(render_markdown(report), encoding="utf-8")
    except (OSError, ValidationError) as error:
        print(f"Active-FX ROI diagnostic validation failed: {error}", file=sys.stderr)
        return 1
    print(f"Active-FX ROI diagnostic report: {markdown_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
