#!/usr/bin/env python3
"""Verify SPK-002 ffmpeg/gdigrab external-recording evidence.

The verifier proves that one four-cell collector run is internally consistent.
Pixel metrics describe only the recorded files; they never imply support for a
different recorder, capture API, machine, display mode, or operating system.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys
from typing import Any


EXPECTED_SCHEMA = 1
EXPECTED_SCENARIO = "spk-002-external-recording-gdigrab-v1"
EVIDENCE_SCOPE = "single-machine-ffmpeg-gdigrab-observation-only"
HOST_NAME = "ba-click-fx-desktop.exe"
CONFIG_NAME = "BAFX.config.json"
LOG_NAME = "ba-click-fx-desktop-support.log"
VIDEO_NAME = "capture.mkv"
FFPROBE_NAME = "ffprobe.json"
ANALYSIS_WIDTH = 160
ANALYSIS_HEIGHT = 160
CHANGE_THRESHOLD = 8

EXPECTED_CASES = {
    "desktop-background-aware": ("desktop", "background-aware"),
    "desktop-recording-compatible": ("desktop", "recording-compatible"),
    "title-background-aware": ("title", "background-aware"),
    "title-recording-compatible": ("title", "recording-compatible"),
}
EXPECTED_FILES = {
    HOST_NAME,
    CONFIG_NAME,
    LOG_NAME,
    "host.stdout.log",
    "host.stderr.log",
    "ffmpeg.stdout.log",
    "ffmpeg.stderr.log",
    VIDEO_NAME,
    FFPROBE_NAME,
    "ffprobe.stderr.log",
}
NONEMPTY_FILES = {
    HOST_NAME,
    CONFIG_NAME,
    LOG_NAME,
    VIDEO_NAME,
    FFPROBE_NAME,
}
LEDGER_PAIRS = (
    ("FramesAcquired", "FramesClosed"),
    ("FramePoolsCreated", "FramePoolsClosed"),
    ("SessionsCreated", "SessionsClosed"),
    ("FrameArrivedRegistrations", "FrameArrivedUnregistrations"),
    ("ItemClosedRegistrations", "ItemClosedUnregistrations"),
)
LIVE_LEDGER_FIELDS = (
    "LiveFrames",
    "LiveFramePools",
    "LiveSessions",
    "LiveFrameArrivedRegistrations",
    "LiveItemClosedRegistrations",
)


class ValidationError(ValueError):
    """Raised when evidence is malformed or internally inconsistent."""


@dataclass(frozen=True)
class VideoMetrics:
    decoded_frames: int
    analysis_width: int
    analysis_height: int
    baseline_frame: int
    baseline_seconds: float
    baseline_frame_sha256: str
    peak_baseline_frame: int
    peak_baseline_seconds: float
    peak_mean_absolute_luma_delta: float
    peak_changed_pixel_fraction: float
    peak_maximum_luma_delta: int
    peak_adjacent_frame: int
    peak_adjacent_seconds: float
    peak_adjacent_mean_absolute_luma_delta: float


@dataclass(frozen=True)
class CaseResult:
    case_id: str
    source_kind: str
    background_mode: str
    recorder: str
    codec: str
    pixel_format: str
    width: int
    height: int
    frame_rate: float
    duration_seconds: float
    host_event_count: int
    wgc_resource_state: str
    background_participation_events: int
    video_metrics: VideoMetrics


@dataclass(frozen=True)
class VerificationResult:
    schema_version: int
    scenario_id: str
    capture_revision: str
    status: str
    evidence_scope: str
    cases: tuple[CaseResult, ...]


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValidationError(f"duplicate JSON field: {key}")
        result[key] = value
    return result


def _reject_nonfinite_constant(value: str) -> None:
    raise ValidationError(f"non-finite JSON number: {value}")


def _load_json(path: Path, label: str) -> Any:
    try:
        with path.open("r", encoding="utf-8-sig") as stream:
            return json.load(
                stream,
                object_pairs_hook=_reject_duplicate_keys,
                parse_constant=_reject_nonfinite_constant,
            )
    except OSError as error:
        raise ValidationError(f"unable to read {label} {path}: {error}") from error
    except json.JSONDecodeError as error:
        raise ValidationError(f"invalid JSON in {label} {path}: {error}") from error


def _object(value: Any, label: str) -> dict[str, Any]:
    if type(value) is not dict:
        raise ValidationError(f"{label} must be an object")
    return value


def _list(value: Any, label: str) -> list[Any]:
    if type(value) is not list:
        raise ValidationError(f"{label} must be an array")
    return value


def _require_fields(
    value: dict[str, Any],
    required: set[str],
    label: str,
    optional: set[str] | None = None,
) -> None:
    optional = optional or set()
    missing = required - value.keys()
    unknown = value.keys() - required - optional
    if missing:
        raise ValidationError(f"{label} missing fields: {sorted(missing)}")
    if unknown:
        raise ValidationError(f"{label} has unknown fields: {sorted(unknown)}")


def _string(value: Any, label: str) -> str:
    if type(value) is not str or not value:
        raise ValidationError(f"{label} must be a non-empty string")
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


def _boolean(value: Any, label: str) -> bool:
    if type(value) is not bool:
        raise ValidationError(f"{label} must be a boolean")
    return value


def _exact(value: Any, expected: Any, label: str) -> None:
    if value != expected:
        raise ValidationError(f"{label} must be {expected!r}, got {value!r}")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
    except OSError as error:
        raise ValidationError(f"unable to hash {path}: {error}") from error
    return digest.hexdigest()


def _sha256_string(value: Any, label: str) -> str:
    result = _string(value, label).lower()
    if re.fullmatch(r"[0-9a-f]{64}", result) is None:
        raise ValidationError(f"{label} must be a SHA-256 digest")
    return result


def _safe_case_directory(root: Path, case_id: str) -> Path:
    path = root / case_id
    if Path(case_id).name != case_id or Path(case_id).is_absolute():
        raise ValidationError(f"case directory is not local: {case_id!r}")
    if path.is_symlink() or not path.is_dir():
        raise ValidationError(f"case directory is missing or symbolic: {path}")
    return path


def _safe_file(root: Path, name: str, label: str) -> Path:
    relative = Path(name)
    if relative.name != name or relative.is_absolute() or name in {".", ".."}:
        raise ValidationError(f"{label} must be a local file name")
    path = root / relative
    if path.is_symlink():
        raise ValidationError(f"{label} must not be a symbolic link")
    return path


def _validate_artifacts(case_root: Path, value: Any) -> None:
    files = _object(value, "case.files")
    if set(files) != EXPECTED_FILES:
        raise ValidationError(
            "case.files must contain exactly the collector artifact set"
        )
    for name in sorted(EXPECTED_FILES):
        label = f"case.files[{name!r}]"
        record = _object(files[name], label)
        _require_fields(record, {"exists", "bytes", "sha256"}, label)
        if not _boolean(record["exists"], f"{label}.exists"):
            raise ValidationError(f"{label}.exists must be true")
        expected_bytes = _integer(record["bytes"], f"{label}.bytes")
        expected_hash = _sha256_string(record["sha256"], f"{label}.sha256")
        path = _safe_file(case_root, name, label)
        if not path.is_file():
            raise ValidationError(f"artifact is missing: {path}")
        actual_bytes = path.stat().st_size
        if actual_bytes != expected_bytes:
            raise ValidationError(
                f"{label}.bytes mismatch: manifest={expected_bytes}, file={actual_bytes}"
            )
        if name in NONEMPTY_FILES and actual_bytes == 0:
            raise ValidationError(f"required artifact is empty: {path}")
        actual_hash = _sha256(path)
        if actual_hash != expected_hash:
            raise ValidationError(f"{label}.sha256 does not match the artifact")


def _validate_process(value: Any, label: str, is_host: bool) -> int:
    process = _object(value, label)
    required = {
        "state",
        "pid",
        "startedAtUtc",
        "readyAtUtc",
        "timeoutMs",
        "timedOut",
        "exitedAtUtc",
        "exitCode",
    }
    if is_host:
        required.add("threadId")
    _require_fields(process, required, label)
    _exact(_string(process["state"], f"{label}.state"), "exited", f"{label}.state")
    pid = _integer(process["pid"], f"{label}.pid", 1)
    _string(process["startedAtUtc"], f"{label}.startedAtUtc")
    _string(process["exitedAtUtc"], f"{label}.exitedAtUtc")
    _integer(process["timeoutMs"], f"{label}.timeoutMs", 1)
    if _boolean(process["timedOut"], f"{label}.timedOut"):
        raise ValidationError(f"{label} timed out")
    _exact(_integer(process["exitCode"], f"{label}.exitCode"), 0, f"{label}.exitCode")
    if is_host:
        _string(process["readyAtUtc"], f"{label}.readyAtUtc")
        _integer(process["threadId"], f"{label}.threadId", 1)
    elif process["readyAtUtc"] is not None:
        raise ValidationError(f"{label}.readyAtUtc must be null")
    return pid


def _command_argv(value: Any, label: str) -> list[str]:
    command = _object(value, label)
    _require_fields(command, {"argv", "display"}, label)
    argv = _list(command["argv"], f"{label}.argv")
    if not argv or any(type(item) is not str or not item for item in argv):
        raise ValidationError(f"{label}.argv must contain non-empty strings")
    _string(command["display"], f"{label}.display")
    return argv


def _require_argument_pair(argv: list[str], option: str, expected: str, label: str) -> None:
    matches = [index for index, item in enumerate(argv[:-1]) if item == option]
    if len(matches) != 1 or argv[matches[0] + 1] != expected:
        raise ValidationError(f"{label} must contain {option} {expected}")


def _validate_commands(
    value: Any,
    source_input: str,
    root_host: dict[str, Any],
    root_recorder: dict[str, Any],
    capture_region: tuple[int, int, int, int],
) -> None:
    commands = _object(value, "case.commands")
    _require_fields(commands, {"host", "ffmpeg", "ffprobe"}, "case.commands")
    host = _command_argv(commands["host"], "case.commands.host")
    expected_host_arguments = {
        f"--demo-age-ms={root_host['demoAgeMs']}",
        f"--demo-delay-ms={root_host['demoDelayMs']}",
        f"--quit-after-ms={root_host['quitAfterMs']}",
        "--disable-raw-input",
    }
    if Path(host[0]).name.lower() != HOST_NAME.lower():
        raise ValidationError("case.commands.host must execute the copied Host")
    if set(host[1:]) != expected_host_arguments or len(host[1:]) != 4:
        raise ValidationError("case.commands.host arguments do not match the contract")

    ffmpeg = _command_argv(commands["ffmpeg"], "case.commands.ffmpeg")
    _require_argument_pair(ffmpeg, "-f", "gdigrab", "case.commands.ffmpeg")
    _require_argument_pair(ffmpeg, "-framerate", str(root_recorder["frameRate"]), "case.commands.ffmpeg")
    _require_argument_pair(ffmpeg, "-draw_mouse", "0", "case.commands.ffmpeg")
    _require_argument_pair(
        ffmpeg,
        "-offset_x",
        str(capture_region[0]),
        "case.commands.ffmpeg",
    )
    _require_argument_pair(
        ffmpeg,
        "-offset_y",
        str(capture_region[1]),
        "case.commands.ffmpeg",
    )
    _require_argument_pair(
        ffmpeg,
        "-video_size",
        f"{capture_region[2]}x{capture_region[3]}",
        "case.commands.ffmpeg",
    )
    _require_argument_pair(ffmpeg, "-i", source_input, "case.commands.ffmpeg")
    _require_argument_pair(ffmpeg, "-c:v", root_recorder["videoCodec"], "case.commands.ffmpeg")
    _require_argument_pair(ffmpeg, "-pix_fmt", root_recorder["pixelFormat"], "case.commands.ffmpeg")
    if "-an" not in ffmpeg or ffmpeg[-1] != VIDEO_NAME:
        raise ValidationError("case.commands.ffmpeg must record video-only capture.mkv")

    ffprobe = _command_argv(commands["ffprobe"], "case.commands.ffprobe")
    required_probe_arguments = {"-show_streams", "-show_format", VIDEO_NAME}
    if not required_probe_arguments.issubset(ffprobe):
        raise ValidationError("case.commands.ffprobe is incomplete")


def _parse_log_event(block: str, path: Path) -> dict[str, str]:
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
        if key in event:
            raise ValidationError(f"{path}: duplicate event field {key}")
        event[key] = value
    return event


def _load_log_events(path: Path, host_pid: int) -> list[dict[str, str]]:
    try:
        text = path.read_text(encoding="utf-8-sig")
    except OSError as error:
        raise ValidationError(f"unable to read Host log {path}: {error}") from error
    blocks = re.split(r"^---\s*$", text, flags=re.MULTILINE)
    events = [_parse_log_event(block, path) for block in blocks if block.strip()]
    if not events:
        raise ValidationError(f"{path}: no structured events")
    sessions = {event.get("Log.SessionId") for event in events}
    if None in sessions or len(sessions) != 1:
        raise ValidationError(f"{path}: expected exactly one Host log session")
    for expected_sequence, event in enumerate(events, 1):
        if event.get("Log.SchemaVersion") != "2":
            raise ValidationError(f"{path}: Log.SchemaVersion must be 2")
        try:
            sequence = int(event["Event.Sequence"])
            process_id = int(event["Event.ProcessId"])
        except (KeyError, ValueError) as error:
            raise ValidationError(f"{path}: invalid sequence or process ID") from error
        if sequence != expected_sequence:
            raise ValidationError(
                f"{path}: Event.Sequence must be {expected_sequence}, got {sequence}"
            )
        if process_id != host_pid:
            raise ValidationError(f"{path}: Event.ProcessId differs from the case manifest")
        if event.get("Event.Level") in {"Error", "Fatal"}:
            raise ValidationError(
                f"{path}: contains {event['Event.Level']} event {sequence}"
            )
    exited = [event for event in events if event.get("Event.Name") == "Process.Exited"]
    if len(exited) != 1 or events[-1] is not exited[0]:
        raise ValidationError(f"{path}: Process.Exited must be the final unique event")
    return events


def _message_fields(message: str, prefix: str) -> dict[str, str] | None:
    if not message.startswith(prefix):
        return None
    fields: dict[str, str] = {}
    for item in message.split(";"):
        if "=" not in item:
            continue
        key, value = item.split("=", 1)
        if key in fields:
            raise ValidationError(f"duplicate message field: {key}")
        fields[key] = value
    return fields


def _validate_ledger(fields: dict[str, str], label: str) -> None:
    def value(name: str) -> str:
        # The production diagnostic prefixes its first counter and then uses
        # compact names for the remaining semicolon-delimited counters.
        result = fields.get(name, fields.get(f"WGC.ResourceLedger.{name}"))
        if result is None:
            raise KeyError(name)
        return result

    for left, right in LEDGER_PAIRS:
        try:
            left_value = int(value(left))
            right_value = int(value(right))
        except (KeyError, ValueError) as error:
            raise ValidationError(f"{label}: invalid ledger field {left}/{right}") from error
        if left_value != right_value:
            raise ValidationError(f"{label}: {left}/{right} count mismatch")
    for name in LIVE_LEDGER_FIELDS:
        try:
            live_value = int(value(name))
        except (KeyError, ValueError) as error:
            raise ValidationError(f"{label}: invalid ledger field {name}") from error
        if live_value != 0:
            raise ValidationError(f"{label}: {name} must be zero")
    if value("Failures") != "0":
        raise ValidationError(f"{label}: Failures must be zero")
    if value("AllReleased") != "true":
        raise ValidationError(f"{label}: AllReleased must be true")


def _validate_host_log(
    path: Path,
    host_pid: int,
    background_mode: str,
) -> tuple[int, str, int]:
    events = _load_log_events(path, host_pid)
    configurations = [
        event for event in events if event.get("Event.Name") == "Configuration.Applied"
    ]
    if not configurations or any(
        event.get("Background.Mode") != background_mode for event in configurations
    ):
        raise ValidationError(f"{path}: configuration background mode mismatch")
    support = [event for event in events if event.get("Event.Name") == "SupportReport"]
    if not support:
        raise ValidationError(f"{path}: no SupportReport event")

    messages = [event.get("Event.Message", "") for event in events]
    affinity = [
        fields
        for message in messages
        if (
            fields := _message_fields(
                message, "Capture.Exclusion.Requested="
            )
        )
        is not None
    ]
    participation_count = sum(
        event.get("Event.Name") == "BackgroundComposite.Participated"
        for event in events
    )
    ledgers = [
        fields
        for message in messages
        if (
            fields := _message_fields(
                message, "BackgroundCapture.ResourceLedger.Phase=shutdown;"
            )
        )
        is not None
    ]

    if background_mode == "background-aware":
        if not any(event.get("Support.WGC") == "active" for event in support):
            raise ValidationError(f"{path}: background-aware WGC was not active")
        if not any(
            fields.get("Capture.Exclusion.Requested") == "0x00000011"
            and fields.get("Observed") == "0x00000011"
            and fields.get("Set") == "succeeded"
            and fields.get("Query") == "succeeded"
            for fields in affinity
        ):
            raise ValidationError(f"{path}: WDA_EXCLUDEFROMCAPTURE was not confirmed")
        if participation_count == 0:
            raise ValidationError(f"{path}: no background composite participation")
        if len(ledgers) != 1:
            raise ValidationError(f"{path}: expected one shutdown WGC resource ledger")
        _validate_ledger(ledgers[0], str(path))
        resource_state = "balanced"
    else:
        if any(event.get("Support.WGC") != "fallback-fx-only" for event in support):
            raise ValidationError(f"{path}: recording-compatible must use FX-only")
        if not any(
            fields.get("Capture.Exclusion.Requested") == "0x00000000"
            and fields.get("Observed") == "0x00000000"
            and fields.get("Set") == "succeeded"
            and fields.get("Query") == "succeeded"
            for fields in affinity
        ):
            raise ValidationError(f"{path}: WDA_NONE was not confirmed")
        if participation_count != 0:
            raise ValidationError(
                f"{path}: recording-compatible unexpectedly used background samples"
            )
        if any("WGC capture session active" in message for message in messages):
            raise ValidationError(f"{path}: recording-compatible started WGC")
        if len(ledgers) > 1:
            raise ValidationError(f"{path}: duplicate shutdown WGC resource ledgers")
        if ledgers:
            _validate_ledger(ledgers[0], str(path))
            resource_state = "balanced-empty"
        else:
            # No ledger is expected when the FX-only process never creates WGC.
            resource_state = "not-created"
    return len(events), resource_state, participation_count


def _fraction(value: Any, label: str) -> float:
    text = _string(value, label)
    parts = text.split("/")
    try:
        if len(parts) == 1:
            result = float(parts[0])
        elif len(parts) == 2:
            denominator = float(parts[1])
            if denominator == 0.0:
                raise ValueError
            result = float(parts[0]) / denominator
        else:
            raise ValueError
    except ValueError as error:
        raise ValidationError(f"{label} is not a valid fraction") from error
    if not math.isfinite(result) or result <= 0.0:
        raise ValidationError(f"{label} must be positive")
    return result


def _video_metadata(value: Any, label: str) -> dict[str, Any]:
    document = _object(value, label)
    streams = _list(document.get("streams"), f"{label}.streams")
    video_streams = [
        _object(stream, f"{label}.streams[]")
        for stream in streams
        if type(stream) is dict and stream.get("codec_type") == "video"
    ]
    if len(video_streams) != 1 or len(streams) != 1:
        raise ValidationError(f"{label} must describe exactly one video stream")
    stream = video_streams[0]
    format_record = _object(document.get("format"), f"{label}.format")
    codec = _string(stream.get("codec_name"), f"{label}.codec_name")
    pixel_format = _string(stream.get("pix_fmt"), f"{label}.pix_fmt")
    width = _integer(stream.get("width"), f"{label}.width", 2)
    height = _integer(stream.get("height"), f"{label}.height", 2)
    frame_rate = _fraction(stream.get("avg_frame_rate"), f"{label}.avg_frame_rate")
    duration_value = stream.get("duration", format_record.get("duration"))
    try:
        duration = float(duration_value)
    except (TypeError, ValueError) as error:
        raise ValidationError(f"{label}.duration must be numeric") from error
    if not math.isfinite(duration) or duration <= 0.0:
        raise ValidationError(f"{label}.duration must be positive")
    return {
        "codec": codec,
        "pixel_format": pixel_format,
        "width": width,
        "height": height,
        "frame_rate": frame_rate,
        "duration": duration,
        "r_frame_rate": _string(stream.get("r_frame_rate"), f"{label}.r_frame_rate"),
        "time_base": _string(stream.get("time_base"), f"{label}.time_base"),
        "format_name": _string(format_record.get("format_name"), f"{label}.format_name"),
        "nb_frames": stream.get("nb_frames"),
    }


def _run_json_tool(command: list[str], timeout_seconds: float, label: str) -> Any:
    try:
        result = subprocess.run(
            command,
            capture_output=True,
            check=False,
            timeout=timeout_seconds,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ValidationError(f"{label} failed: {error}") from error
    if result.returncode != 0:
        stderr = result.stderr.decode("utf-8", errors="replace").strip()
        raise ValidationError(f"{label} exited with {result.returncode}: {stderr}")
    try:
        return json.loads(
            result.stdout.decode("utf-8-sig"),
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=_reject_nonfinite_constant,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValidationError(f"{label} returned invalid JSON: {error}") from error


def _probe_video(path: Path, executable: str, timeout_seconds: float) -> dict[str, Any]:
    document = _run_json_tool(
        [
            executable,
            "-v",
            "error",
            "-print_format",
            "json",
            "-show_format",
            "-show_streams",
            str(path),
        ],
        timeout_seconds,
        "ffprobe",
    )
    return _video_metadata(document, "fresh ffprobe")


def _compare_metadata(stored: dict[str, Any], fresh: dict[str, Any]) -> None:
    for name in (
        "codec",
        "pixel_format",
        "width",
        "height",
        "r_frame_rate",
        "time_base",
        "format_name",
        "nb_frames",
    ):
        if stored[name] != fresh[name]:
            raise ValidationError(f"stored ffprobe metadata differs at {name}")
    for name, tolerance in (("frame_rate", 1.0e-6), ("duration", 0.01)):
        if not math.isclose(stored[name], fresh[name], abs_tol=tolerance):
            raise ValidationError(f"stored ffprobe metadata differs at {name}")


def _analyze_video(
    path: Path,
    executable: str,
    timeout_seconds: float,
    frame_rate: float,
) -> VideoMetrics:
    command = [
        executable,
        "-v",
        "error",
        "-nostdin",
        "-i",
        str(path),
        "-map",
        "0:v:0",
        "-vf",
        f"scale={ANALYSIS_WIDTH}:{ANALYSIS_HEIGHT}:flags=area,format=gray",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "gray",
        "pipe:1",
    ]
    try:
        result = subprocess.run(
            command,
            capture_output=True,
            check=False,
            timeout=timeout_seconds,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ValidationError(f"ffmpeg frame decode failed: {error}") from error
    if result.returncode != 0:
        stderr = result.stderr.decode("utf-8", errors="replace").strip()
        raise ValidationError(
            f"ffmpeg frame decode exited with {result.returncode}: {stderr}"
        )
    frame_bytes = ANALYSIS_WIDTH * ANALYSIS_HEIGHT
    if len(result.stdout) % frame_bytes != 0:
        raise ValidationError("ffmpeg returned a truncated gray frame")
    frame_count = len(result.stdout) // frame_bytes
    if frame_count < 2:
        raise ValidationError("video must decode to at least two frames")

    frames = [
        result.stdout[offset : offset + frame_bytes]
        for offset in range(0, len(result.stdout), frame_bytes)
    ]
    # GDI capture may expose an incomplete initialization frame. A one-second
    # warm-up remains before the delayed demo click and gives every cell the
    # same stable baseline without treating recorder startup as visual change.
    baseline_index = min(max(math.ceil(frame_rate), 1), frame_count - 1)
    baseline = frames[baseline_index]
    peak_baseline = (baseline_index, -1.0, 0.0, 0)
    peak_adjacent = (baseline_index, -1.0)
    previous = baseline
    for index in range(baseline_index, frame_count):
        frame = frames[index]
        differences = [abs(left - right) for left, right in zip(frame, baseline)]
        mean_delta = sum(differences) / frame_bytes
        changed_fraction = sum(
            difference > CHANGE_THRESHOLD for difference in differences
        ) / frame_bytes
        maximum_delta = max(differences)
        if mean_delta > peak_baseline[1]:
            peak_baseline = (
                index,
                mean_delta,
                changed_fraction,
                maximum_delta,
            )
        if index > baseline_index:
            adjacent = sum(
                abs(left - right) for left, right in zip(frame, previous)
            ) / frame_bytes
            if adjacent > peak_adjacent[1]:
                peak_adjacent = (index, adjacent)
        previous = frame

    return VideoMetrics(
        decoded_frames=frame_count,
        analysis_width=ANALYSIS_WIDTH,
        analysis_height=ANALYSIS_HEIGHT,
        baseline_frame=baseline_index,
        baseline_seconds=baseline_index / frame_rate,
        baseline_frame_sha256=hashlib.sha256(baseline).hexdigest(),
        peak_baseline_frame=peak_baseline[0],
        peak_baseline_seconds=peak_baseline[0] / frame_rate,
        peak_mean_absolute_luma_delta=peak_baseline[1],
        peak_changed_pixel_fraction=peak_baseline[2],
        peak_maximum_luma_delta=peak_baseline[3],
        peak_adjacent_frame=peak_adjacent[0],
        peak_adjacent_seconds=peak_adjacent[0] / frame_rate,
        peak_adjacent_mean_absolute_luma_delta=peak_adjacent[1],
    )


def _validate_root(
    document: Any,
) -> tuple[dict[str, Any], dict[str, Any], tuple[int, int, int, int], str]:
    root = _object(document, "capture")
    _require_fields(
        root,
        {
            "schemaVersion",
            "scenarioId",
            "evidenceScope",
            "captureStatus",
            "revision",
            "workingTreeDirty",
            "capturedAtUtc",
            "host",
            "recorder",
            "capture",
            "matrix",
            "cases",
            "completedAtUtc",
        },
        "capture",
    )
    _exact(_integer(root["schemaVersion"], "capture.schemaVersion", 1), EXPECTED_SCHEMA, "capture.schemaVersion")
    _exact(_string(root["scenarioId"], "capture.scenarioId"), EXPECTED_SCENARIO, "capture.scenarioId")
    _exact(
        _string(root["evidenceScope"], "capture.evidenceScope"),
        EVIDENCE_SCOPE,
        "capture.evidenceScope",
    )
    _exact(_string(root["captureStatus"], "capture.captureStatus"), "captured", "capture.captureStatus")
    revision = _string(root["revision"], "capture.revision")
    if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
        raise ValidationError("capture.revision must be a full Git object ID")
    if _boolean(root["workingTreeDirty"], "capture.workingTreeDirty"):
        raise ValidationError("official evidence requires a clean working tree")
    _string(root["capturedAtUtc"], "capture.capturedAtUtc")
    _string(root["completedAtUtc"], "capture.completedAtUtc")

    host = _object(root["host"], "capture.host")
    _require_fields(
        host,
        {
            "sourcePath",
            "sha256",
            "demoAgeMs",
            "demoDelayMs",
            "quitAfterMs",
            "rawInputRegistration",
            "readyTimeoutMs",
            "processTimeoutMs",
        },
        "capture.host",
    )
    _string(host["sourcePath"], "capture.host.sourcePath")
    _sha256_string(host["sha256"], "capture.host.sha256")
    _exact(_integer(host["demoAgeMs"], "capture.host.demoAgeMs", 1), 130, "capture.host.demoAgeMs")
    _exact(_integer(host["demoDelayMs"], "capture.host.demoDelayMs", 1), 3000, "capture.host.demoDelayMs")
    _exact(_integer(host["quitAfterMs"], "capture.host.quitAfterMs", 1), 7500, "capture.host.quitAfterMs")
    _exact(_string(host["rawInputRegistration"], "capture.host.rawInputRegistration"), "disabled", "capture.host.rawInputRegistration")
    _integer(host["readyTimeoutMs"], "capture.host.readyTimeoutMs", 1)
    _integer(host["processTimeoutMs"], "capture.host.processTimeoutMs", 1)

    recorder = _object(root["recorder"], "capture.recorder")
    _require_fields(
        recorder,
        {
            "implementation",
            "ffmpegPath",
            "ffmpegSha256",
            "ffprobePath",
            "ffprobeSha256",
            "frameRate",
            "durationMs",
            "processTimeoutMs",
            "probeTimeoutMs",
            "videoCodec",
            "pixelFormat",
        },
        "capture.recorder",
    )
    _exact(_string(recorder["implementation"], "capture.recorder.implementation"), "ffmpeg-gdigrab", "capture.recorder.implementation")
    _string(recorder["ffmpegPath"], "capture.recorder.ffmpegPath")
    _sha256_string(recorder["ffmpegSha256"], "capture.recorder.ffmpegSha256")
    _string(recorder["ffprobePath"], "capture.recorder.ffprobePath")
    _sha256_string(recorder["ffprobeSha256"], "capture.recorder.ffprobeSha256")
    _exact(_integer(recorder["frameRate"], "capture.recorder.frameRate", 1), 10, "capture.recorder.frameRate")
    _exact(_integer(recorder["durationMs"], "capture.recorder.durationMs", 1), 6000, "capture.recorder.durationMs")
    _integer(recorder["processTimeoutMs"], "capture.recorder.processTimeoutMs", 1)
    _integer(recorder["probeTimeoutMs"], "capture.recorder.probeTimeoutMs", 1)
    _exact(_string(recorder["videoCodec"], "capture.recorder.videoCodec"), "ffv1", "capture.recorder.videoCodec")
    _exact(_string(recorder["pixelFormat"], "capture.recorder.pixelFormat"), "bgr0", "capture.recorder.pixelFormat")

    capture = _object(root["capture"], "capture.capture")
    _require_fields(
        capture,
        {
            "displayWidth",
            "displayHeight",
            "left",
            "top",
            "width",
            "height",
            "frameRate",
            "durationMs",
        },
        "capture.capture",
    )
    display_size = (
        _integer(capture["displayWidth"], "capture.capture.displayWidth", 2),
        _integer(capture["displayHeight"], "capture.capture.displayHeight", 2),
    )
    capture_region = (
        _integer(capture["left"], "capture.capture.left"),
        _integer(capture["top"], "capture.capture.top"),
        _integer(capture["width"], "capture.capture.width", 2),
        _integer(capture["height"], "capture.capture.height", 2),
    )
    expected_edge = min(1024, *display_size)
    expected_edge -= expected_edge % 2
    expected_region = (
        (display_size[0] - expected_edge) // 2,
        (display_size[1] - expected_edge) // 2,
        expected_edge,
        expected_edge,
    )
    _exact(capture_region, expected_region, "capture.capture centered region")
    _exact(
        capture["frameRate"],
        recorder["frameRate"],
        "capture.capture.frameRate",
    )
    _exact(
        capture["durationMs"],
        recorder["durationMs"],
        "capture.capture.durationMs",
    )

    expected_matrix = [
        {"caseId": case_id, "sourceKind": source, "backgroundMode": mode}
        for case_id, (source, mode) in EXPECTED_CASES.items()
    ]
    if root["matrix"] != expected_matrix:
        raise ValidationError("capture.matrix does not match the locked four-cell matrix")
    return host, recorder, capture_region, revision


def _validate_case(
    root: Path,
    summary: Any,
    root_host: dict[str, Any],
    root_recorder: dict[str, Any],
    capture_region: tuple[int, int, int, int],
    ffmpeg: str,
    ffprobe: str,
    timeout_seconds: float,
) -> CaseResult:
    case_summary = _object(summary, "capture.cases[]")
    _require_fields(
        case_summary,
        {
            "caseId",
            "sourceKind",
            "backgroundMode",
            "status",
            "manifest",
            "manifestSha256",
            "failure",
        },
        "capture.cases[]",
    )
    case_id = _string(case_summary["caseId"], "capture.cases[].caseId")
    if case_id not in EXPECTED_CASES:
        raise ValidationError(f"unexpected case ID: {case_id}")
    source_kind, background_mode = EXPECTED_CASES[case_id]
    _exact(case_summary["sourceKind"], source_kind, f"{case_id}.sourceKind")
    _exact(case_summary["backgroundMode"], background_mode, f"{case_id}.backgroundMode")
    _exact(case_summary["status"], "captured", f"{case_id}.status")
    if case_summary["failure"] is not None:
        raise ValidationError(f"{case_id}.failure must be null")
    expected_manifest = f"{case_id}/case.json"
    _exact(case_summary["manifest"], expected_manifest, f"{case_id}.manifest")
    expected_manifest_hash = _sha256_string(
        case_summary["manifestSha256"], f"{case_id}.manifestSha256"
    )
    case_root = _safe_case_directory(root, case_id)
    case_path = _safe_file(case_root, "case.json", f"{case_id}.manifest")
    if _sha256(case_path) != expected_manifest_hash:
        raise ValidationError(f"{case_id}: case.json hash mismatch")
    case = _object(_load_json(case_path, "case manifest"), case_id)
    _require_fields(
        case,
        {
            "schemaVersion",
            "scenarioId",
            "caseId",
            "source",
            "backgroundMode",
            "status",
            "startedAtUtc",
            "completedAtUtc",
            "commands",
            "processes",
            "files",
            "cleanup",
            "failure",
        },
        case_id,
    )
    _exact(case["schemaVersion"], EXPECTED_SCHEMA, f"{case_id}.schemaVersion")
    _exact(case["scenarioId"], EXPECTED_SCENARIO, f"{case_id}.scenarioId")
    _exact(case["caseId"], case_id, f"{case_id}.caseId")
    _exact(case["backgroundMode"], background_mode, f"{case_id}.backgroundMode")
    _exact(case["status"], "captured", f"{case_id}.status")
    if case["failure"] is not None:
        raise ValidationError(f"{case_id}.failure must be null")
    _string(case["startedAtUtc"], f"{case_id}.startedAtUtc")
    _string(case["completedAtUtc"], f"{case_id}.completedAtUtc")

    source = _object(case["source"], f"{case_id}.source")
    _require_fields(
        source,
        {
            "kind",
            "gdigrabInput",
            "captureLeft",
            "captureTop",
            "captureWidth",
            "captureHeight",
            "windowTitle",
        },
        f"{case_id}.source",
    )
    _exact(source["kind"], source_kind, f"{case_id}.source.kind")
    expected_input = "desktop" if source_kind == "desktop" else "title=ba-click-fx-desktop"
    expected_title = None if source_kind == "desktop" else "ba-click-fx-desktop"
    _exact(source["gdigrabInput"], expected_input, f"{case_id}.source.gdigrabInput")
    _exact(source["windowTitle"], expected_title, f"{case_id}.source.windowTitle")
    _exact(source["captureLeft"], capture_region[0], f"{case_id}.source.captureLeft")
    _exact(source["captureTop"], capture_region[1], f"{case_id}.source.captureTop")
    _exact(source["captureWidth"], capture_region[2], f"{case_id}.source.captureWidth")
    _exact(source["captureHeight"], capture_region[3], f"{case_id}.source.captureHeight")

    _validate_commands(
        case["commands"],
        expected_input,
        root_host,
        root_recorder,
        capture_region,
    )
    processes = _object(case["processes"], f"{case_id}.processes")
    _require_fields(processes, {"host", "ffmpeg", "ffprobe"}, f"{case_id}.processes")
    host_pid = _validate_process(processes["host"], f"{case_id}.processes.host", True)
    _validate_process(processes["ffmpeg"], f"{case_id}.processes.ffmpeg", False)
    _validate_process(processes["ffprobe"], f"{case_id}.processes.ffprobe", False)
    cleanup = _object(case["cleanup"], f"{case_id}.cleanup")
    _require_fields(
        cleanup,
        {
            "ownedHostRemaining",
            "ownedFfmpegRemaining",
            "allOwnedProcessesExited",
        },
        f"{case_id}.cleanup",
    )
    if _boolean(cleanup["ownedHostRemaining"], f"{case_id}.cleanup.ownedHostRemaining"):
        raise ValidationError(f"{case_id}: owned Host process remained alive")
    if _boolean(cleanup["ownedFfmpegRemaining"], f"{case_id}.cleanup.ownedFfmpegRemaining"):
        raise ValidationError(f"{case_id}: owned ffmpeg process remained alive")
    if not _boolean(
        cleanup["allOwnedProcessesExited"],
        f"{case_id}.cleanup.allOwnedProcessesExited",
    ):
        raise ValidationError(f"{case_id}: owned process cleanup is incomplete")
    _validate_artifacts(case_root, case["files"])
    if _sha256(case_root / HOST_NAME) != root_host["sha256"]:
        raise ValidationError(f"{case_id}: Host hash differs from the root manifest")

    configuration = _object(
        _load_json(case_root / CONFIG_NAME, "Host configuration"),
        f"{case_id}.configuration",
    )
    try:
        configured_mode = configuration["background"]["mode"]
    except (KeyError, TypeError) as error:
        raise ValidationError(f"{case_id}: configuration has no background.mode") from error
    _exact(configured_mode, background_mode, f"{case_id}.configuration.background.mode")

    event_count, resource_state, participation_count = _validate_host_log(
        case_root / LOG_NAME,
        host_pid,
        background_mode,
    )
    stored_metadata = _video_metadata(
        _load_json(case_root / FFPROBE_NAME, "stored ffprobe output"),
        f"{case_id}.stored ffprobe",
    )
    fresh_metadata = _probe_video(case_root / VIDEO_NAME, ffprobe, timeout_seconds)
    _compare_metadata(stored_metadata, fresh_metadata)
    expected_codec = root_recorder["videoCodec"]
    _exact(fresh_metadata["codec"], expected_codec, f"{case_id}.codec")
    _exact(fresh_metadata["pixel_format"], root_recorder["pixelFormat"], f"{case_id}.pixelFormat")
    _exact(fresh_metadata["width"], capture_region[2], f"{case_id}.width")
    _exact(fresh_metadata["height"], capture_region[3], f"{case_id}.height")
    if not math.isclose(
        fresh_metadata["frame_rate"],
        float(root_recorder["frameRate"]),
        abs_tol=0.01,
    ):
        raise ValidationError(f"{case_id}: frame rate differs from the recorder contract")
    expected_duration = root_recorder["durationMs"] / 1000.0
    if abs(fresh_metadata["duration"] - expected_duration) > 0.25:
        raise ValidationError(f"{case_id}: duration differs from the recorder contract")
    metrics = _analyze_video(
        case_root / VIDEO_NAME,
        ffmpeg,
        timeout_seconds,
        fresh_metadata["frame_rate"],
    )
    return CaseResult(
        case_id=case_id,
        source_kind=source_kind,
        background_mode=background_mode,
        recorder="ffmpeg-gdigrab",
        codec=fresh_metadata["codec"],
        pixel_format=fresh_metadata["pixel_format"],
        width=fresh_metadata["width"],
        height=fresh_metadata["height"],
        frame_rate=fresh_metadata["frame_rate"],
        duration_seconds=fresh_metadata["duration"],
        host_event_count=event_count,
        wgc_resource_state=resource_state,
        background_participation_events=participation_count,
        video_metrics=metrics,
    )


def validate_path(
    path: Path,
    ffmpeg: str = "ffmpeg",
    ffprobe: str = "ffprobe",
    timeout_seconds: float = 20.0,
) -> VerificationResult:
    document = _load_json(path, "capture manifest")
    root_host, root_recorder, capture_region, revision = _validate_root(document)
    root = path.parent
    summaries = _list(document["cases"], "capture.cases")
    ids = [
        _object(summary, "capture.cases[]").get("caseId") for summary in summaries
    ]
    if len(ids) != len(EXPECTED_CASES) or set(ids) != set(EXPECTED_CASES):
        raise ValidationError("capture.cases must contain each matrix cell exactly once")
    results = tuple(
        _validate_case(
            root,
            summary,
            root_host,
            root_recorder,
            capture_region,
            ffmpeg,
            ffprobe,
            timeout_seconds,
        )
        for summary in summaries
    )
    return VerificationResult(
        schema_version=EXPECTED_SCHEMA,
        scenario_id=EXPECTED_SCENARIO,
        capture_revision=revision,
        status="verified-observation",
        evidence_scope=EVIDENCE_SCOPE,
        cases=results,
    )


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
    parser.add_argument("capture", type=Path, help="collector capture.json")
    parser.add_argument("--report", type=Path, help="optional verification JSON")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="bounded frame decoder")
    parser.add_argument("--ffprobe", default="ffprobe", help="metadata probe")
    parser.add_argument(
        "--tool-timeout-seconds",
        type=float,
        default=20.0,
        help="per-tool timeout, 1..30 seconds",
    )
    options = parser.parse_args(arguments)
    if not 1.0 <= options.tool_timeout_seconds <= 30.0:
        parser.error("--tool-timeout-seconds must be between 1 and 30")
    return options


def main(arguments: list[str] | None = None) -> int:
    options = _parse_args(sys.argv[1:] if arguments is None else arguments)
    try:
        result = validate_path(
            options.capture,
            options.ffmpeg,
            options.ffprobe,
            options.tool_timeout_seconds,
        )
        if options.report is not None:
            _write_report(options.report, result)
    except ValidationError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(
        f"PASS: {result.scenario_id} {result.status}; "
        f"cases={len(result.cases)}; scope={result.evidence_scope}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
