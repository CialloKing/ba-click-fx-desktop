#!/usr/bin/env python3
"""Validate and summarize the controlled Raw Input baseline.

This report is intentionally separate from the paired render baseline.  A
machine may accept SendInput while not exposing the injected edge through Raw
Input (for example, a remote or restricted input stack).  Such a capture is
reported as ``unsupported`` rather than being treated as a passing latency
measurement.
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
EXPECTED_SCENARIO = "p0-raw-input-down-v1"
MODE_DIRECTORIES = ("fx-only", "background-aware")
EXPECTED_BACKGROUND_MODES = {
    "fx-only": "recording-compatible",
    "background-aware": "background-aware",
}
LOG_NAME = "ba-click-fx-desktop-support.log"
HOST_NAME = "ba-click-fx-desktop.exe"
CONFIG_NAME = "BAFX.config.json"
RAW_INPUT_REGISTRATION = "enabled-inputsink-devnotify"

LATENCY_METRICS = (
    ("Win32 queue age p50 (ms)", "Input.Win32QueueAge.P50"),
    ("Win32 queue age p95 (ms)", "Input.Win32QueueAge.P95"),
    ("Win32 queue age p99 (ms)", "Input.Win32QueueAge.P99"),
    ("Win32 queue age max (ms)", "Input.Win32QueueAge.Max"),
    ("Pending events p95", "Input.PendingEvents.P95"),
    ("Pending events max", "Input.PendingEvents.Max"),
    ("Dispatch-to-Present p50 (us)", "Input.DispatchToPresentReturn.P50"),
    ("Dispatch-to-Present p95 (us)", "Input.DispatchToPresentReturn.P95"),
    ("Dispatch-to-Present p99 (us)", "Input.DispatchToPresentReturn.P99"),
    ("Dispatch-to-Present max (us)", "Input.DispatchToPresentReturn.Max"),
    ("Message-to-Present p50 (ms)", "Input.MessageToPresentReturn.P50"),
    ("Message-to-Present p95 (ms)", "Input.MessageToPresentReturn.P95"),
    ("Message-to-Present p99 (ms)", "Input.MessageToPresentReturn.P99"),
    ("Message-to-Present max (ms)", "Input.MessageToPresentReturn.Max"),
    ("CPU Present p95 (us)", "Cpu.PresentCall.P95"),
    ("CPU Present max (us)", "Cpu.PresentCall.Max"),
    ("Presented FPS", "Window.PresentedFps"),
)


class ValidationError(ValueError):
    """Raised when a capture cannot support the requested interpretation."""


@dataclass(frozen=True)
class ModeEvidence:
    name: str
    log_path: Path
    session_id: str
    startup: dict[str, str]
    support: dict[str, str]
    configuration: dict[str, str]
    interval: dict[str, str]
    process_exited: dict[str, str]


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


def _metric_value(fields: dict[str, str], key: str) -> int | float | None:
    raw = fields.get(key)
    if raw is None:
        return None
    try:
        return int(raw)
    except ValueError:
        try:
            return float(raw)
        except ValueError:
            return None


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
    except OSError as error:
        raise ValidationError(f"cannot hash {path}: {error}") from error
    return digest.hexdigest()


def _point(value: Any, name: str) -> tuple[int, int]:
    if type(value) is not dict or type(value.get("x")) is not int or type(value.get("y")) is not int:
        raise ValidationError(f"mode field {name} must be an integer point")
    return value["x"], value["y"]


def _rectangle(value: Any, name: str) -> tuple[int, int, int, int]:
    if type(value) is not dict:
        raise ValidationError(f"mode field {name} must be a rectangle")
    keys = ("left", "top", "right", "bottom")
    if any(type(value.get(key)) is not int for key in keys):
        raise ValidationError(f"mode field {name} has non-integer bounds")
    left, top, right, bottom = (value[key] for key in keys)
    if right <= left or bottom <= top:
        raise ValidationError(f"mode field {name} is empty")
    return left, top, right, bottom


def _validate_geometry(mode: dict[str, Any], context: str) -> None:
    work = _rectangle(mode.get("primaryWorkArea"), f"{context}.primaryWorkArea")
    target = _rectangle(mode.get("targetRectangle"), f"{context}.targetRectangle")
    clip = _rectangle(mode.get("cursorClipRectangle"), f"{context}.cursorClipRectangle")
    target_point = _point(mode.get("targetPoint"), f"{context}.targetPoint")
    original = _point(mode.get("originalCursor"), f"{context}.originalCursor")
    restored = _point(mode.get("restoredCursor"), f"{context}.restoredCursor")
    if not (work[0] <= target[0] < target[2] <= work[2] and
            work[1] <= target[1] < target[3] <= work[3]):
        raise ValidationError(f"{context}: receiver target is outside primary work area")
    if not (target[0] <= target_point[0] < target[2] and
            target[1] <= target_point[1] < target[3]):
        raise ValidationError(f"{context}: target point is outside receiver window")
    if not (clip[0] < target[2] and target[0] < clip[2] and
            clip[1] < target[3] and target[1] < clip[3]):
        raise ValidationError(f"{context}: receiver target does not intersect cursor clip")
    if mode.get("cursorRestored") is not True or original != restored:
        raise ValidationError(f"{context}: cursor was not restored exactly")


def _validate_manifest(manifest: dict[str, Any]) -> tuple[int, int]:
    if manifest.get("schemaVersion") != EXPECTED_SCHEMA:
        raise ValidationError(f"manifest schemaVersion must be {EXPECTED_SCHEMA}")
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
    if type(executable_hash) is not str or not re.fullmatch(r"[0-9a-f]{64}", executable_hash):
        raise ValidationError("manifest executableSha256 must be lowercase SHA-256")
    duration = manifest.get("durationMs")
    click_count = manifest.get("clickCount")
    if type(duration) is not int or duration < 10_000:
        raise ValidationError("manifest durationMs must cover a full 10 second window")
    if type(click_count) is not int or not 1 <= click_count <= 32:
        raise ValidationError("manifest clickCount must be between 1 and 32")
    for key, minimum in (("clickHoldMs", 10), ("clickIntervalMs", 100),
                         ("inputConfirmationTimeoutMs", 100),
                         ("receiverReadyTimeoutMs", 1000),
                         ("receiverStopTimeoutMs", 1000),
                         ("cursorRestoreTimeoutMs", 100)):
        value = manifest.get(key)
        if type(value) is not int or value < minimum:
            raise ValidationError(f"manifest {key} is outside its safe range")
    if manifest.get("rawInputRegistration") != RAW_INPUT_REGISTRATION:
        raise ValidationError("manifest rawInputRegistration is not the controlled sink")
    modes = manifest.get("modes")
    if type(modes) is not dict or set(modes) != set(MODE_DIRECTORIES):
        raise ValidationError("manifest modes must contain exactly the paired modes")
    for name in MODE_DIRECTORIES:
        if type(modes[name]) is not dict:
            raise ValidationError(f"manifest mode {name} must be an object")
    return duration, click_count


def _load_and_validate_configs(root: Path, manifest: dict[str, Any]) -> dict[str, dict[str, Any]]:
    expected_hash = manifest["executableSha256"]
    configurations: dict[str, dict[str, Any]] = {}
    for name in MODE_DIRECTORIES:
        executable = root / name / HOST_NAME
        if _sha256(executable) != expected_hash:
            raise ValidationError(f"{name}: executable hash does not match manifest")
        configurations[name] = _load_json(root / name / CONFIG_NAME)
        try:
            actual_mode = configurations[name]["background"]["mode"]
        except (KeyError, TypeError) as error:
            raise ValidationError(f"{name}: configuration has no background.mode") from error
        if actual_mode != EXPECTED_BACKGROUND_MODES[name]:
            raise ValidationError(f"{name}: configuration has the wrong background.mode")
    comparable = json.loads(json.dumps(configurations["fx-only"]))
    background = json.loads(json.dumps(configurations["background-aware"]))
    comparable["background"]["mode"] = None
    background["background"]["mode"] = None
    if comparable != background:
        raise ValidationError("paired configurations differ outside background.mode")
    return configurations


def _load_mode(root: Path, name: str, duration_ms: int) -> ModeEvidence:
    path = root / name / LOG_NAME
    events = _load_events(path)
    startup = _one_event(events, "Process.Startup", path)
    support = _one_event(events, "SupportReport", path)
    configuration = _one_event(events, "Configuration.Applied", path)
    process_exited = _one_event(events, "Process.Exited", path)
    intervals = [
        event for event in events
        if event.get("Event.Name") == "Performance.Interval"
        and event.get("Window.Final") == "false"
    ]
    if len(intervals) != 1:
        raise ValidationError(f"{path}: expected one complete performance interval")
    interval = intervals[0]
    minimum_us = (duration_ms - 250) * 1000
    if _integer(interval, "Window.DurationUs", str(path)) < minimum_us:
        raise ValidationError(f"{path}: performance interval is too short")
    return ModeEvidence(
        name=name,
        log_path=path,
        session_id=startup["Log.SessionId"],
        startup=startup,
        support=support,
        configuration=configuration,
        interval=interval,
        process_exited=process_exited,
    )


def _validate_receiver(mode: dict[str, Any], click_count: int, context: str) -> None:
    if mode.get("captureStatus") != "captured":
        raise ValidationError(f"{context}: receiver capture did not complete")
    if mode.get("exitCode") != 0 or mode.get("hostExitedNormally") is not True:
        raise ValidationError(f"{context}: Host did not exit normally")
    expected_edges = click_count * 2
    exact = {
        "plannedSendInputCount": expected_edges,
        "attemptedSendInputCount": expected_edges,
        "acceptedSendInputCount": expected_edges,
        "taggedDownCount": click_count,
        "taggedUpCount": click_count,
        "unexpectedButtonMessages": 0,
        "unexpectedMoveMessages": 0,
        "captureLossCount": 0,
        "emergencyUpCount": 0,
    }
    for key, expected in exact.items():
        if mode.get(key) != expected:
            raise ValidationError(f"{context}: {key} must be {expected}")
    for key in ("captureReleased", "receiverStopped", "cursorRestored", "cleanupSuccess"):
        if mode.get(key) is not True:
            raise ValidationError(f"{context}: {key} must be true")
    command_line = mode.get("commandLine")
    if type(command_line) is not list or not command_line:
        raise ValidationError(f"{context}: commandLine is missing")
    if not any(argument == "--quit-after-ms=10500" for argument in command_line):
        raise ValidationError(f"{context}: commandLine has no bounded Host lifetime")
    forbidden_prefixes = ("--demo-age-ms=", "--demo-delay-ms=", "--disable-raw-input")
    if any(argument.startswith(prefix) for argument in command_line for prefix in forbidden_prefixes):
        raise ValidationError(f"{context}: commandLine contains a synthetic or disabled-input option")
    _validate_geometry(mode, context)


def _require_metric(
    fields: dict[str, str], prefix: str, context: str, unit: str, samples: int | None = None
) -> None:
    if not _boolean(fields, f"{prefix}.Available", context):
        raise ValidationError(f"{context}: {prefix} is unavailable")
    if fields.get(f"{prefix}.Unit") != unit:
        raise ValidationError(f"{context}: {prefix} has the wrong unit")
    actual_samples = _integer(fields, f"{prefix}.Samples", context)
    recorded = _integer(fields, f"{prefix}.RecordedSamples", context)
    dropped = _integer(fields, f"{prefix}.DroppedSamples", context)
    if actual_samples <= 0 or recorded != actual_samples or dropped != 0:
        raise ValidationError(f"{context}: {prefix} has incomplete samples")
    if samples is not None and actual_samples != samples:
        raise ValidationError(f"{context}: {prefix}.Samples must be {samples}")


def _validate_log(
    evidence: ModeEvidence, click_count: int
) -> dict[str, Any]:
    context = evidence.name
    expected_background = EXPECTED_BACKGROUND_MODES[evidence.name]
    if evidence.configuration.get("Background.Mode") != expected_background:
        raise ValidationError(f"{context}: configuration background mode mismatch")
    if evidence.interval.get("Background.Mode") != expected_background:
        raise ValidationError(f"{context}: interval background mode mismatch")
    if evidence.support.get("Graphics.DriverType") != "Hardware":
        raise ValidationError(f"{context}: evidence requires hardware D3D11")
    if evidence.support.get("Graphics.HardwareFallback") != "none":
        raise ValidationError(f"{context}: hardware fallback invalidates evidence")
    expected_edges = click_count * 2
    raw_messages = _integer(evidence.interval, "Input.RawMessages", context)
    button_edges = _integer(evidence.interval, "Input.ButtonEdges", context)
    if button_edges != raw_messages:
        raise ValidationError(f"{context}: RawMessages and ButtonEdges differ")
    for key in (
        "Input.MoveEvents",
        "Input.CancelEvents",
        "Input.CompactedMoveEvents",
        "Input.OverflowMoveDrops",
        "Input.MessageTimeUnavailable",
        "MessagePump.InputBudgetExhaustions",
    ):
        if _integer(evidence.interval, key, context) != 0:
            raise ValidationError(f"{context}: {key} must be zero")
    input_dispatched = _integer(evidence.interval, "MessagePump.InputDispatched", context)
    if input_dispatched != raw_messages:
        raise ValidationError(f"{context}: InputDispatched does not match RawMessages")
    _require_metric(evidence.interval, "Input.Win32QueueAge", context, "ms")
    _require_metric(evidence.interval, "Input.PendingEvents", context, "events")
    _require_metric(evidence.interval, "Cpu.PresentCall", context, "us")
    raw_status = "passed" if raw_messages == expected_edges else "unsupported"
    if raw_messages not in (0, expected_edges):
        raise ValidationError(
            f"{context}: expected either zero Raw Input messages or {expected_edges}, got {raw_messages}"
        )
    if raw_status == "passed":
        _require_metric(evidence.interval, "Input.DispatchToPresentReturn", context, "us", click_count)
        _require_metric(evidence.interval, "Input.MessageToPresentReturn", context, "ms", click_count)
    if evidence.name == "fx-only":
        if evidence.support.get("Support.WGC") != "fallback-fx-only":
            raise ValidationError("fx-only: unexpected WGC support state")
        for key in ("WGC.SamplesAccepted", "Background.ParticipatingFrames"):
            if _integer(evidence.interval, key, context) != 0:
                raise ValidationError(f"fx-only: {key} must be zero")
    else:
        if evidence.support.get("Support.WGC") != "active":
            raise ValidationError("background-aware: WGC session is not active")
        if evidence.interval.get("Background.CompositeStatus") != "participating":
            raise ValidationError("background-aware: captured background did not participate")
        if _integer(evidence.interval, "WGC.SamplesAccepted", context) <= 0:
            raise ValidationError("background-aware: no WGC sample was accepted")
        if _integer(evidence.interval, "Background.ParticipatingFrames", context) <= 0:
            raise ValidationError("background-aware: no frame used the captured background")
    return {
        "status": raw_status,
        "rawMessages": raw_messages,
        "buttonEdges": button_edges,
        "inputDispatched": input_dispatched,
        "latencyMetrics": {
            key: _metric_value(evidence.interval, key) for _, key in LATENCY_METRICS
        },
        "support": evidence.support,
        "configuration": evidence.configuration,
        "interval": evidence.interval,
    }


def build_report(root: Path) -> dict[str, Any]:
    manifest = _load_json(root / "capture.json")
    duration_ms, click_count = _validate_manifest(manifest)
    for name in MODE_DIRECTORIES:
        _validate_receiver(manifest["modes"][name], click_count, name)
    configurations = _load_and_validate_configs(root, manifest)
    evidence = {
        name: _load_mode(root, name, duration_ms) for name in MODE_DIRECTORIES
    }
    if evidence["fx-only"].session_id == evidence["background-aware"].session_id:
        raise ValidationError("paired modes unexpectedly share a log session")
    identity_fields = (
        "Graphics.Adapter", "Graphics.AdapterLuid", "Graphics.DriverVersion",
        "Display.Primary", "Display.PrimaryDpi", "Display.RefreshRateHz",
        "Output.Width", "Output.Height",
    )
    for key in identity_fields:
        first = evidence["fx-only"].interval.get(key) if key.startswith("Output.") else evidence["fx-only"].support.get(key)
        second = evidence["background-aware"].interval.get(key) if key.startswith("Output.") else evidence["background-aware"].support.get(key)
        if first != second:
            raise ValidationError(f"paired logs differ at {key}: {first!r} != {second!r}")
    modes = {name: _validate_log(evidence[name], click_count) for name in MODE_DIRECTORIES}
    statuses = {mode["status"] for mode in modes.values()}
    if statuses == {"passed"}:
        status = "passed"
        reason = None
    elif statuses == {"unsupported"}:
        status = "unsupported"
        reason = "Host did not observe injected edges through Raw Input on this environment."
    else:
        status = "failed"
        reason = "Paired modes disagreed about Raw Input capability."
    return {
        "schemaVersion": EXPECTED_SCHEMA,
        "status": status,
        "scenarioId": EXPECTED_SCENARIO,
        "revision": manifest["revision"],
        "capturedAtUtc": manifest.get("capturedAtUtc"),
        "durationMs": duration_ms,
        "clickCount": click_count,
        "clickHoldMs": manifest["clickHoldMs"],
        "clickIntervalMs": manifest["clickIntervalMs"],
        "rawInputRegistration": manifest["rawInputRegistration"],
        "identity": {
            key: (evidence["fx-only"].interval.get(key) if key.startswith("Output.") else evidence["fx-only"].support.get(key))
            for key in identity_fields
        },
        "modes": modes,
        "receiver": {
            name: {
                key: manifest["modes"][name].get(key)
                for key in (
                    "targetRectangle", "targetPoint", "primaryWorkArea",
                    "cursorClipRectangle", "originalCursor", "restoredCursor",
                    "plannedSendInputCount", "attemptedSendInputCount",
                    "acceptedSendInputCount", "taggedDownCount", "taggedUpCount",
                    "unexpectedButtonMessages", "unexpectedMoveMessages",
                    "captureLossCount", "emergencyUpCount", "cursorRestored",
                    "captureReleased", "receiverStopped", "cleanupSuccess",
                )
            }
            for name in MODE_DIRECTORIES
        },
        "configurations": configurations,
        "reason": reason,
        "limitations": [
            "Input-to-Present-return is not DWM, scanout, panel, or photon latency.",
            "SendInput-to-WM_INPUT is environment-dependent; unsupported is an evidence result, not a passing latency baseline.",
            "This is a local SDR primary-monitor baseline, not hardware-matrix acceptance.",
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
        "# P0 Raw Input baseline",
        "",
        f"- Status: `{report['status']}`",
        f"- Scenario: `{report['scenarioId']}`",
        f"- Revision: `{report['revision']}`",
        f"- Captured at: `{report['capturedAtUtc']}`",
        f"- Click contract: {report['clickCount']} clicks, {report['clickHoldMs']} ms hold, {report['clickIntervalMs']} ms interval",
        f"- Adapter: `{identity.get('Graphics.Adapter')}`",
        f"- Output: `{identity.get('Output.Width')}x{identity.get('Output.Height')}`",
        "",
        "| Metric | FX-only | background-aware |",
        "|---|---:|---:|",
    ]
    for label, key in LATENCY_METRICS:
        lines.append(
            f"| {label} | {_format_value(report['modes']['fx-only']['latencyMetrics'].get(key))} | "
            f"{_format_value(report['modes']['background-aware']['latencyMetrics'].get(key))} |"
        )
    lines.extend(("", "## Input contract", ""))
    for name in MODE_DIRECTORIES:
        mode = report["modes"][name]
        receiver = report["receiver"][name]
        lines.append(
            f"- `{name}`: `{mode['status']}`, RawMessages={mode['rawMessages']}, "
            f"ButtonEdges={mode['buttonEdges']}, InputDispatched={mode['inputDispatched']}; "
            f"receiver Down/Up={receiver['taggedDownCount']}/{receiver['taggedUpCount']}."
        )
    if report.get("reason"):
        lines.extend(("", f"- Note: {report['reason']}"))
    lines.extend(("", "## Limitations", ""))
    lines.extend(f"- {item}" for item in report["limitations"])
    lines.append("")
    return "\n".join(lines)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, help="raw input capture directory")
    parser.add_argument("--json", type=Path, dest="json_path")
    parser.add_argument("--markdown", type=Path, dest="markdown_path")
    parser.add_argument(
        "--require-supported",
        action="store_true",
        help="return failure when the environment cannot expose injected WM_INPUT",
    )
    return parser.parse_args()


def main() -> int:
    arguments = _parse_args()
    try:
        report = build_report(arguments.root)
        json_path = arguments.json_path or arguments.root / "summary.json"
        markdown_path = arguments.markdown_path or arguments.root / "summary.md"
        json_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        markdown_path.write_text(render_markdown(report), encoding="utf-8")
    except (OSError, ValidationError) as error:
        print(f"raw input baseline validation failed: {error}", file=sys.stderr)
        return 1
    print(f"raw input baseline {report['status']}: {markdown_path}")
    if arguments.require_supported and report["status"] != "passed":
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
