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
EXPECTED_SCENARIO = "p0-static-click-message-pressure-v1"
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
    message_count = _manifest_integer(manifest, "messageCount")
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

    return {
        "schemaVersion": 1,
        "status": "passed",
        "scenarioId": EXPECTED_SCENARIO,
        "revision": manifest.get("revision"),
        "capturedAtUtc": manifest.get("capturedAtUtc"),
        "durationMs": duration_ms,
        "messageCount": message_count,
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


def render_markdown(report: dict[str, Any]) -> str:
    identity = report["identity"]
    lines = [
        "# P0 paired performance baseline",
        "",
        f"- Status: `{report['status']}`",
        f"- Scenario: `{report['scenarioId']}`",
        f"- Revision: `{report['revision']}`",
        f"- Captured at: `{report['capturedAtUtc']}`",
        f"- Workload: fixed 130 ms click, {report['durationMs']} ms, "
        f"{report['messageCount']} harmless thread messages",
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
    lines.extend(("", "## Interpretation", ""))
    lines.append(
        "The report separates input/message pressure, WGC, GPU copy, Bloom, "
        "and Present metrics. A passed report also proves that both modes kept "
        "`GPU.PendingFrames.Max <= 1` while message wakes were observed."
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
