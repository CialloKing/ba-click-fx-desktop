#!/usr/bin/env python3
"""Validate and summarize a paired desktop performance baseline.

The collector owns process orchestration. This tool only consumes its manifest
and structured Host logs so captured evidence can be rechecked offline.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any


EXPECTED_SCHEMA = 1
EXPECTED_SCENARIO = "p0-static-click-message-pressure-v3"
MODE_DIRECTORIES = ("fx-only", "background-aware")
EXPECTED_BACKGROUND_MODES = {
    "fx-only": "recording-compatible",
    "background-aware": "background-aware",
}
LOG_NAME = "ba-click-fx-desktop-support.log"
HOST_NAME = "ba-click-fx-desktop.exe"
CONFIG_NAME = "BAFX.config.json"

IDENTITY_FIELDS = (
    "Product.Version",
    "Graphics.DriverType",
    "Graphics.Adapter",
    "Graphics.AdapterLuid",
    "Graphics.DriverVersion",
    "Display.Primary",
    "Display.PrimaryDpi",
    "Display.RefreshRateNumerator",
    "Display.RefreshRateDenominator",
    "Display.RefreshRateHz",
    "Output.Width",
    "Output.Height",
)

REPORT_METRICS = (
    ("Frames", "Window.FrameCount"),
    ("Presented FPS", "Window.PresentedFps"),
    ("Raw Input messages", "Input.RawMessages"),
    ("Input queue age p95 (ms)", "Input.Win32QueueAge.P95"),
    ("Input queue age max (ms)", "Input.Win32QueueAge.Max"),
    ("Message-to-Present p95 (ms)", "Input.MessageToPresentReturn.P95"),
    ("Message-to-Present max (ms)", "Input.MessageToPresentReturn.Max"),
    ("Other messages dispatched", "MessagePump.OtherDispatched"),
    ("Frame-ready wakes", "FramePacing.FrameReadyWakes"),
    ("Message wakes", "FramePacing.MessageWakes"),
    ("Frame pacing timeouts", "FramePacing.Timeouts"),
    ("Frame pacing failures", "FramePacing.Failures"),
    ("WGC producer FPS", "WGC.ProducerCallbackFps"),
    ("WGC accepted FPS", "WGC.AcceptedFps"),
    ("WGC accepted samples", "WGC.SamplesAccepted"),
    ("WGC sample age p95 (us)", "WGC.SampleAge.P95"),
    ("CPU WGC drain p95 (us)", "Cpu.WgcDrainInclusive.P95"),
    ("CPU Present p50 (us)", "Cpu.PresentCall.P50"),
    ("CPU Present p95 (us)", "Cpu.PresentCall.P95"),
    ("CPU Present p99 (us)", "Cpu.PresentCall.P99"),
    ("CPU Present max (us)", "Cpu.PresentCall.Max"),
    ("GPU pending frames max", "GPU.PendingFrames.Max"),
    ("GPU WGC drain/copy p95 (us)", "GPU.WgcDrainAndCopy.P95"),
    ("GPU background snapshot p95 (us)", "GPU.BackgroundSnapshot.P95"),
    ("GPU FX materials p95 (us)", "GPU.FxMaterials.P95"),
    ("GPU Bloom/final p50 (us)", "GPU.BloomAndFinalComposite.P50"),
    ("GPU Bloom/final p95 (us)", "GPU.BloomAndFinalComposite.P95"),
    ("GPU Bloom/final p99 (us)", "GPU.BloomAndFinalComposite.P99"),
    ("GPU Bloom/final max (us)", "GPU.BloomAndFinalComposite.Max"),
    ("GPU FX total p95 (us)", "GPU.FxTotal.P95"),
    ("GPU command span p95 (us)", "GPU.RenderCommandSpan.P95"),
    ("GPU command span max (us)", "GPU.RenderCommandSpan.Max"),
)


class ValidationError(ValueError):
    """Raised when a capture cannot support a paired comparison."""


@dataclass(frozen=True)
class ModeEvidence:
    name: str
    log_path: Path
    session_id: str
    startup: dict[str, str]
    support: dict[str, str]
    configuration: dict[str, str]
    interval: dict[str, str]


def _load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValidationError(f"cannot read {path}: {error}") from error
    if type(value) is not dict:
        raise ValidationError(f"{path} must contain a JSON object")
    return value


def _parse_event(block: str, path: Path) -> dict[str, str]:
    event: dict[str, str] = {}
    for line_number, raw_line in enumerate(block.splitlines(), 1):
        line = raw_line.strip("\r")
        if not line:
            continue
        if "=" not in line:
            raise ValidationError(
                f"{path}: malformed event line {line_number}: {line!r}"
            )
        key, value = line.split("=", 1)
        if key in event and event[key] != value:
            raise ValidationError(f"{path}: conflicting duplicate field {key}")
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


def _one_event(
    events: list[dict[str, str]], name: str, path: Path
) -> dict[str, str]:
    matches = [event for event in events if event.get("Event.Name") == name]
    if len(matches) != 1:
        raise ValidationError(
            f"{path}: expected one {name} event, found {len(matches)}"
        )
    return matches[0]


def _integer(fields: dict[str, str], key: str, context: str) -> int:
    try:
        return int(fields[key])
    except (KeyError, ValueError) as error:
        raise ValidationError(f"{context}: {key} must be an integer") from error


def _number(fields: dict[str, str], key: str, context: str) -> float:
    try:
        return float(fields[key])
    except (KeyError, ValueError) as error:
        raise ValidationError(f"{context}: {key} must be numeric") from error


def _boolean(fields: dict[str, str], key: str, context: str) -> bool:
    value = fields.get(key)
    if value == "true":
        return True
    if value == "false":
        return False
    raise ValidationError(f"{context}: {key} must be true or false")


def _select_interval(
    events: list[dict[str, str]], path: Path, minimum_duration_us: int
) -> dict[str, str]:
    intervals = [
        event
        for event in events
        if event.get("Event.Name") == "Performance.Interval"
        and event.get("Window.Final") == "false"
    ]
    if len(intervals) != 1:
        raise ValidationError(
            f"{path}: expected one complete performance interval, found {len(intervals)}"
        )
    interval = intervals[0]
    duration = _integer(interval, "Window.DurationUs", str(path))
    if duration < minimum_duration_us:
        raise ValidationError(
            f"{path}: complete interval is too short ({duration} us)"
        )
    return interval


def _load_mode(
    root: Path, name: str, minimum_duration_us: int
) -> ModeEvidence:
    path = root / name / LOG_NAME
    events = _load_events(path)
    startup = _one_event(events, "Process.Startup", path)
    support = _one_event(events, "SupportReport", path)
    configuration = _one_event(events, "Configuration.Applied", path)
    interval = _select_interval(events, path, minimum_duration_us)
    return ModeEvidence(
        name=name,
        log_path=path,
        session_id=startup["Log.SessionId"],
        startup=startup,
        support=support,
        configuration=configuration,
        interval=interval,
    )


def _manifest_integer(manifest: dict[str, Any], key: str) -> int:
    value = manifest.get(key)
    if type(value) is not int:
        raise ValidationError(f"manifest field {key} must be an integer")
    return value


def _validate_manifest(manifest: dict[str, Any]) -> tuple[int, int]:
    if manifest.get("schemaVersion") != EXPECTED_SCHEMA:
        raise ValidationError(
            f"manifest schemaVersion must be {EXPECTED_SCHEMA}"
        )
    if manifest.get("scenarioId") != EXPECTED_SCENARIO:
        raise ValidationError(f"manifest scenarioId must be {EXPECTED_SCENARIO}")
    if manifest.get("captureStatus") != "captured":
        raise ValidationError("manifest captureStatus must be captured")
    revision = manifest.get("revision")
    if type(revision) is not str or not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise ValidationError("manifest revision must be a full Git object ID")
    if manifest.get("workingTreeDirty") is not False:
        raise ValidationError("official baseline requires a clean working tree")
    executable_hash = manifest.get("executableSha256")
    if type(executable_hash) is not str or not re.fullmatch(
        r"[0-9a-f]{64}", executable_hash
    ):
        raise ValidationError("manifest executableSha256 must be lowercase SHA-256")
    duration_ms = _manifest_integer(manifest, "durationMs")
    if _manifest_integer(manifest, "demoAgeMs") != 130:
        raise ValidationError("manifest demoAgeMs must be 130")
    if _manifest_integer(manifest, "demoDelayMs") != 50:
        raise ValidationError("manifest demoDelayMs must be 50")
    message_count = _manifest_integer(manifest, "messageCount")
    if _manifest_integer(manifest, "messageBatchSize") != 5:
        raise ValidationError("manifest messageBatchSize must be 5")
    if _manifest_integer(manifest, "messageBatchIntervalMs") != 25:
        raise ValidationError("manifest messageBatchIntervalMs must be 25")
    if manifest.get("rawInputRegistration") != "disabled":
        raise ValidationError("manifest rawInputRegistration must be disabled")
    if duration_ms < 10_000:
        raise ValidationError("manifest durationMs must cover a full 10 second window")
    if message_count <= 0:
        raise ValidationError("manifest messageCount must be positive")
    modes = manifest.get("modes")
    if type(modes) is not dict or set(modes) != set(MODE_DIRECTORIES):
        raise ValidationError("manifest modes must contain exactly the paired modes")
    for name in MODE_DIRECTORIES:
        mode = modes[name]
        if type(mode) is not dict:
            raise ValidationError(f"manifest mode {name} must be an object")
        if mode.get("backgroundMode") != EXPECTED_BACKGROUND_MODES[name]:
            raise ValidationError(f"manifest mode {name} has the wrong backgroundMode")
        if mode.get("exitCode") != 0:
            raise ValidationError(f"manifest mode {name} did not exit successfully")
        if mode.get("postedMessages") != message_count:
            raise ValidationError(f"manifest mode {name} did not post every message")
        command_line = mode.get("commandLine")
        if type(command_line) is not list or not all(
            type(argument) is str and argument for argument in command_line
        ):
            raise ValidationError(f"manifest mode {name} has no exact command line")
        if "--disable-raw-input" not in command_line:
            raise ValidationError(f"manifest mode {name} did not isolate Raw Input")
    return duration_ms, message_count


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
    except OSError as error:
        raise ValidationError(f"cannot hash {path}: {error}") from error
    return digest.hexdigest()


def _load_and_validate_configs(
    root: Path, manifest: dict[str, Any]
) -> dict[str, dict[str, Any]]:
    expected_hash = manifest["executableSha256"]
    configurations: dict[str, dict[str, Any]] = {}
    for name in MODE_DIRECTORIES:
        executable = root / name / HOST_NAME
        if _sha256(executable) != expected_hash:
            raise ValidationError(f"{name}: executable hash does not match manifest")
        configurations[name] = _load_json(root / name / CONFIG_NAME)

    comparable = json.loads(json.dumps(configurations["fx-only"]))
    background = json.loads(json.dumps(configurations["background-aware"]))
    try:
        comparable["background"]["mode"] = None
        background["background"]["mode"] = None
    except (KeyError, TypeError) as error:
        raise ValidationError("paired configurations need background.mode") from error
    if comparable != background:
        raise ValidationError(
            "paired configurations differ outside background.mode"
        )
    for name in MODE_DIRECTORIES:
        try:
            actual_mode = configurations[name]["background"]["mode"]
        except (KeyError, TypeError) as error:
            raise ValidationError(
                f"{name}: configuration file has no background.mode"
            ) from error
        if actual_mode != EXPECTED_BACKGROUND_MODES[name]:
            raise ValidationError(f"{name}: configuration file has the wrong mode")
    return configurations


def _require_same_capture_identity(evidence: dict[str, ModeEvidence]) -> None:
    first = evidence[MODE_DIRECTORIES[0]]
    second = evidence[MODE_DIRECTORIES[1]]
    for key in IDENTITY_FIELDS:
        first_fields = first.interval if key.startswith("Output.") else first.support
        second_fields = second.interval if key.startswith("Output.") else second.support
        if first_fields.get(key) != second_fields.get(key):
            raise ValidationError(
                f"paired logs differ at {key}: "
                f"{first_fields.get(key)!r} != {second_fields.get(key)!r}"
            )


def _validate_mode(mode: ModeEvidence, message_count: int) -> None:
    context = mode.name
    expected_background = EXPECTED_BACKGROUND_MODES[mode.name]
    if mode.configuration.get("Background.Mode") != expected_background:
        raise ValidationError(f"{context}: configuration background mode mismatch")
    if mode.interval.get("Background.Mode") != expected_background:
        raise ValidationError(f"{context}: interval background mode mismatch")
    if mode.support.get("Graphics.DriverType") != "Hardware":
        raise ValidationError(f"{context}: performance evidence requires hardware D3D11")
    if mode.support.get("Graphics.HardwareFallback") != "none":
        raise ValidationError(f"{context}: hardware fallback invalidates the comparison")
    if _integer(mode.interval, "Input.RawMessages", context) != 0:
        raise ValidationError(
            f"{context}: real Raw Input changed the deterministic workload"
        )
    if _integer(mode.interval, "MessagePump.OtherDispatched", context) < message_count:
        raise ValidationError(f"{context}: not all pressure messages were dispatched")
    if _integer(mode.interval, "FramePacing.MessageWakes", context) <= 0:
        raise ValidationError(f"{context}: message pressure did not wake frame pacing")
    if _integer(mode.interval, "FramePacing.Timeouts", context) != 0:
        raise ValidationError(f"{context}: frame pacing timed out")
    if _integer(mode.interval, "FramePacing.Failures", context) != 0:
        raise ValidationError(f"{context}: frame pacing failed")
    if _integer(mode.interval, "Window.FrameCount", context) != _integer(
        mode.interval, "FramePacing.FrameReadyWakes", context
    ):
        raise ValidationError(f"{context}: rendered frames and frame-ready wakes differ")
    if _integer(mode.interval, "GPU.PendingFrames.Max", context) > 1:
        raise ValidationError(f"{context}: GPU submissions queued beyond one frame")
    if not _boolean(mode.interval, "GPU.TimestampProfiler.Available", context):
        raise ValidationError(f"{context}: GPU timestamp profiler is unavailable")
    if not _boolean(mode.interval, "GPU.TimestampProfiler.Observed", context):
        raise ValidationError(f"{context}: GPU timestamp profiler produced no observation")
    for key in (
        "Input.OverflowMoveDrops",
        "MessagePump.InputBudgetExhaustions",
        "MessagePump.OtherBudgetExhaustions",
        "GPU.RingFullSkipped",
        "GPU.DisjointSamples",
        "GPU.QueryFailures",
        "GPU.StateErrors",
    ):
        if _integer(mode.interval, key, context) != 0:
            raise ValidationError(f"{context}: {key} must be zero")
    dropped_fields = [
        key for key in mode.interval if key.endswith(".DroppedSamples")
    ]
    if not dropped_fields:
        raise ValidationError(f"{context}: no bounded metric diagnostics found")
    for key in dropped_fields:
        if _integer(mode.interval, key, context) != 0:
            raise ValidationError(f"{context}: {key} must be zero")
    submitted = _integer(mode.interval, "GPU.FramesSubmitted", context)
    completed = _integer(mode.interval, "GPU.SamplesCompleted", context)
    if submitted <= 0 or completed / submitted < 0.95:
        raise ValidationError(f"{context}: GPU sample coverage is below 95 percent")
    if _integer(mode.interval, "Cpu.FrameTotal.Max", context) >= 100_000:
        raise ValidationError(f"{context}: CPU frame exceeded 100 ms")
    if _integer(mode.interval, "Cpu.PresentCall.Max", context) >= 50_000:
        raise ValidationError(f"{context}: Present call exceeded 50 ms")

    if mode.name == "fx-only":
        for key in (
            "WGC.ActiveFrames",
            "WGC.DrainAttemptedFrames",
            "WGC.FramesAcquired",
            "WGC.FramesSuperseded",
            "WGC.TimestampRejectedFrames",
            "WGC.OwnedCopiesSubmitted",
            "WGC.SamplesAccepted",
            "Background.SnapshotAttempts",
            "Background.SnapshotsRefreshed",
            "Background.ParticipatingFrames",
        ):
            if _integer(mode.interval, key, context) != 0:
                raise ValidationError(f"fx-only: {key} must be zero")
        if _boolean(mode.interval, "GPU.WgcDrainAndCopy.Available", context):
            raise ValidationError("fx-only: WGC GPU timing unexpectedly available")
    else:
        if mode.support.get("Support.WGC") != "active":
            raise ValidationError("background-aware: WGC session is not active")
        if mode.interval.get("Background.CompositeStatus") != "participating":
            raise ValidationError("background-aware: captured background did not participate")
        if _integer(mode.interval, "WGC.SamplesAccepted", context) <= 0:
            raise ValidationError("background-aware: no WGC sample was accepted")
        if _integer(mode.interval, "Background.ParticipatingFrames", context) <= 0:
            raise ValidationError("background-aware: no frame used the captured background")
        if not _boolean(mode.interval, "GPU.WgcDrainAndCopy.Available", context):
            raise ValidationError("background-aware: WGC GPU timing is unavailable")
        acquired = _integer(mode.interval, "WGC.FramesAcquired", context)
        classified = sum(
            _integer(mode.interval, key, context)
            for key in (
                "WGC.FramesSuperseded",
                "WGC.TimestampRejectedFrames",
                "WGC.SamplesAccepted",
            )
        )
        if acquired != classified:
            raise ValidationError("background-aware: WGC acquired-frame ledger is unbalanced")
        if _integer(mode.interval, "WGC.OwnedCopiesSubmitted", context) != _integer(
            mode.interval, "WGC.SamplesAccepted", context
        ):
            raise ValidationError("background-aware: accepted/copy ledger is unbalanced")


def _metric_value(fields: dict[str, str], key: str) -> int | float | None:
    raw = fields.get(key)
    if raw is None:
        return None
    if raw in {"true", "false"}:
        return 1 if raw == "true" else 0
    try:
        return int(raw)
    except ValueError:
        try:
            return float(raw)
        except ValueError:
            return None


def _mode_result(mode: ModeEvidence) -> dict[str, Any]:
    metrics = {
        key: _metric_value(mode.interval, key)
        for _, key in REPORT_METRICS
    }
    return {
        "sessionId": mode.session_id,
        "log": str(mode.log_path),
        "backgroundMode": mode.interval["Background.Mode"],
        "metrics": metrics,
        "support": mode.support,
        "configuration": mode.configuration,
        "interval": mode.interval,
    }


def _required_metric(
    modes: dict[str, dict[str, Any]], mode: str, key: str
) -> int | float:
    value = modes[mode]["metrics"].get(key)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValidationError(f"{mode}: interpretation requires numeric {key}")
    return value


def _relative_percent(delta: int | float, baseline: int | float) -> float | None:
    if baseline == 0:
        return None
    return delta / baseline * 100.0


def _paired_component(
    fx_only: int | float, background_aware: int | float
) -> dict[str, int | float | str | None]:
    change = background_aware - fx_only
    if change > 0:
        status = "increased"
    elif change < 0:
        status = "reduced"
    else:
        status = "unchanged"
    return {
        "fxOnly": fx_only,
        "backgroundAware": background_aware,
        "absoluteChange": change,
        "percentChange": _relative_percent(change, fx_only),
        "status": status,
    }


def _introduced_component(
    background_aware: int | float,
) -> dict[str, int | float | str | None]:
    return {
        "fxOnly": None,
        "backgroundAware": background_aware,
        "absoluteChange": background_aware,
        "percentChange": None,
        "status": "introduced",
    }


def _build_interpretation(
    modes: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    fx_fps = _required_metric(modes, "fx-only", "Window.PresentedFps")
    background_fps = _required_metric(
        modes, "background-aware", "Window.PresentedFps"
    )
    fx_bloom = _required_metric(
        modes, "fx-only", "GPU.BloomAndFinalComposite.P95"
    )
    background_bloom = _required_metric(
        modes, "background-aware", "GPU.BloomAndFinalComposite.P95"
    )
    fx_command = _required_metric(
        modes, "fx-only", "GPU.RenderCommandSpan.P95"
    )
    background_command = _required_metric(
        modes, "background-aware", "GPU.RenderCommandSpan.P95"
    )
    fx_present = _required_metric(modes, "fx-only", "Cpu.PresentCall.P95")
    background_present = _required_metric(
        modes, "background-aware", "Cpu.PresentCall.P95"
    )
    fx_present_max = _required_metric(
        modes, "fx-only", "Cpu.PresentCall.Max"
    )
    background_present_max = _required_metric(
        modes, "background-aware", "Cpu.PresentCall.Max"
    )
    wgc_copy = _required_metric(
        modes, "background-aware", "GPU.WgcDrainAndCopy.P95"
    )
    background_snapshot = _required_metric(
        modes, "background-aware", "GPU.BackgroundSnapshot.P95"
    )

    throughput = _paired_component(fx_fps, background_fps)
    wgc_component = _introduced_component(wgc_copy)
    snapshot_component = _introduced_component(background_snapshot)
    bloom_component = _paired_component(fx_bloom, background_bloom)
    command_component = _paired_component(fx_command, background_command)
    present_component = _paired_component(fx_present, background_present)
    present_max_component = _paired_component(
        fx_present_max, background_present_max
    )

    incremental_stages = (
        ("wgc-drain-and-copy", max(0, wgc_copy)),
        ("background-snapshot", max(0, background_snapshot)),
        (
            "bloom-and-final-composite",
            max(0, bloom_component["absoluteChange"]),
        ),
    )
    largest_stage, largest_stage_cost = max(
        incremental_stages, key=lambda item: item[1]
    )
    if present_component["absoluteChange"] > max(
        0, command_component["absoluteChange"]
    ):
        bottleneck = "present-wait"
    elif command_component["absoluteChange"] > 0:
        bottleneck = "gpu-command-path"
    else:
        bottleneck = "no-steady-state-regression"

    return {
        "inputBacklog": {
            "status": "not-measured",
            "reason": "Raw Input registration was disabled for the paired render baseline.",
        },
        "components": {
            "throughputFps": throughput,
            "wgcDrainAndCopyP95Us": wgc_component,
            "backgroundSnapshotP95Us": snapshot_component,
            "bloomAndFinalCompositeP95Us": bloom_component,
            "gpuCommandSpanP95Us": command_component,
            "presentCallP95Us": present_component,
            "presentCallMaxUs": present_max_component,
        },
        "tail": {
            "presentMaxRegressionObserved": (
                present_max_component["absoluteChange"] > 0
            ),
        },
        "bottleneck": {
            "classification": bottleneck,
            "largestListedIncrementalGpuStage": largest_stage,
            "largestListedIncrementalGpuStageP95Us": largest_stage_cost,
            "scope": (
                "Largest positive p95 change among WGC drain/copy, background "
                "snapshot, and Bloom/final. Percentiles are not additive and "
                "do not prove per-frame causality."
            ),
        },
    }


def build_report(root: Path) -> dict[str, Any]:
    manifest_path = root / "capture.json"
    manifest = _load_json(manifest_path)
    duration_ms, message_count = _validate_manifest(manifest)
    configurations = _load_and_validate_configs(root, manifest)
    minimum_duration_us = min(9_500_000, (duration_ms - 250) * 1000)
    evidence = {
        name: _load_mode(root, name, minimum_duration_us)
        for name in MODE_DIRECTORIES
    }
    _require_same_capture_identity(evidence)
    for mode in evidence.values():
        _validate_mode(mode, message_count)

    modes = {name: _mode_result(evidence[name]) for name in MODE_DIRECTORIES}
    comparisons: dict[str, dict[str, int | float | None]] = {}
    for _, key in REPORT_METRICS:
        fx_value = modes["fx-only"]["metrics"][key]
        background_value = modes["background-aware"]["metrics"][key]
        delta = None
        ratio = None
        if fx_value is not None and background_value is not None:
            delta = background_value - fx_value
            if fx_value != 0:
                ratio = background_value / fx_value
        comparisons[key] = {
            "fxOnly": fx_value,
            "backgroundAware": background_value,
            "delta": delta,
            "ratio": ratio,
        }

    interpretation = _build_interpretation(modes)
    return {
        "schemaVersion": 1,
        "status": "passed",
        "scenarioId": EXPECTED_SCENARIO,
        "revision": manifest.get("revision"),
        "capturedAtUtc": manifest.get("capturedAtUtc"),
        "durationMs": duration_ms,
        "demoAgeMs": manifest["demoAgeMs"],
        "demoDelayMs": manifest["demoDelayMs"],
        "messageCount": message_count,
        "messageBatchSize": manifest["messageBatchSize"],
        "messageBatchIntervalMs": manifest["messageBatchIntervalMs"],
        "identity": {
            key: (
                evidence["fx-only"].interval.get(key)
                if key.startswith("Output.")
                else evidence["fx-only"].support.get(key)
            )
            for key in IDENTITY_FIELDS
        },
        "modes": modes,
        "configurations": configurations,
        "comparisons": comparisons,
        "interpretation": interpretation,
        "limitations": [
            "The deterministic pressure uses harmless thread messages, not synthetic Raw Input.",
            "Input-to-Present-return is not DWM, scanout, panel, or photon latency.",
            "This paired run is a local SDR primary-monitor baseline, not hardware-matrix acceptance.",
        ],
    }


def _format_value(value: int | float | None) -> str:
    if value is None:
        return "unavailable"
    if isinstance(value, float):
        return f"{value:.3f}"
    return str(value)


def _format_signed(value: int | float, suffix: str = "") -> str:
    if isinstance(value, float):
        return f"{value:+.3f}{suffix}"
    return f"{value:+d}{suffix}"


def _format_percent(value: int | float | None) -> str:
    if value is None:
        return "unavailable"
    return _format_signed(value, "%")


def render_markdown(report: dict[str, Any]) -> str:
    identity = report["identity"]
    lines = [
        "# P0 paired performance baseline",
        "",
        f"- Status: `{report['status']}`",
        f"- Scenario: `{report['scenarioId']}`",
        f"- Revision: `{report['revision']}`",
        f"- Captured at: `{report['capturedAtUtc']}`",
        f"- Workload: fixed {report['demoAgeMs']} ms click after "
        f"{report['demoDelayMs']} ms WGC warm-up, {report['durationMs']} ms",
        f"- Message pressure: {report['messageCount']} harmless thread messages, "
        f"{report['messageBatchSize']} per batch every "
        f"{report['messageBatchIntervalMs']} ms",
        f"- Adapter: `{identity['Graphics.Adapter']}`",
        f"- Driver: `{identity['Graphics.DriverVersion']}`",
        f"- Output: `{identity['Output.Width']}x{identity['Output.Height']}`",
        "",
        "| Metric | FX-only | background-aware | delta | ratio |",
        "|---|---:|---:|---:|---:|",
    ]
    for label, key in REPORT_METRICS:
        comparison = report["comparisons"][key]
        lines.append(
            "| "
            + " | ".join(
                (
                    label,
                    _format_value(comparison["fxOnly"]),
                    _format_value(comparison["backgroundAware"]),
                    _format_value(comparison["delta"]),
                    _format_value(comparison["ratio"]),
                )
            )
            + " |"
        )
    interpretation = report["interpretation"]
    components = interpretation["components"]
    throughput = components["throughputFps"]
    wgc = components["wgcDrainAndCopyP95Us"]
    snapshot = components["backgroundSnapshotP95Us"]
    bloom = components["bloomAndFinalCompositeP95Us"]
    command = components["gpuCommandSpanP95Us"]
    present = components["presentCallP95Us"]
    present_max = components["presentCallMaxUs"]
    bottleneck = interpretation["bottleneck"]
    lines.extend(("", "## Interpretation", ""))
    lines.extend(
        (
            f"- Primary incremental cost: `{bottleneck['classification']}`; "
            f"largest listed incremental GPU stage is "
            f"`{bottleneck['largestListedIncrementalGpuStage']}` at "
            f"{bottleneck['largestListedIncrementalGpuStageP95Us']} us p95.",
            f"- GPU command span p95 changed by "
            f"{_format_signed(command['absoluteChange'], ' us')} "
            f"({_format_percent(command['percentChange'])}).",
            f"- WGC drain/copy p95: `{wgc['status']}`, "
            f"{wgc['backgroundAware']} us; percent change unavailable.",
            f"- Background snapshot p95: `{snapshot['status']}`, "
            f"{snapshot['backgroundAware']} us; percent change unavailable.",
            f"- Bloom/final p95 changed by "
            f"{_format_signed(bloom['absoluteChange'], ' us')} "
            f"({_format_percent(bloom['percentChange'])}); status "
            f"`{bloom['status']}`.",
            f"- Present p95 changed by "
            f"{_format_signed(present['absoluteChange'], ' us')} "
            f"({_format_percent(present['percentChange'])}); max changed "
            f"by {_format_signed(present_max['absoluteChange'], ' us')}. "
            f"Tail regression observed: "
            f"`{str(interpretation['tail']['presentMaxRegressionObserved']).lower()}`.",
            f"- Presented FPS changed by "
            f"{_format_signed(throughput['absoluteChange'])} "
            f"({_format_percent(throughput['percentChange'])}).",
            "- Input backlog: `not-measured`; Raw Input registration was disabled "
            "for this paired render baseline.",
            f"- Scope: {bottleneck['scope']}",
            "- Both modes kept `GPU.PendingFrames.Max <= 1` while message wakes "
            "were observed.",
        )
    )
    lines.extend(("", "## Limitations", ""))
    lines.extend(f"- {item}" for item in report["limitations"])
    lines.append("")
    return "\n".join(lines)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, help="paired capture directory")
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
        print(f"performance baseline validation failed: {error}", file=sys.stderr)
        return 1
    print(f"performance baseline passed: {markdown_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
