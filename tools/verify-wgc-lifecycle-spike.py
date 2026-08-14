#!/usr/bin/env python3
"""Validate SPK-002 WGC lifecycle evidence offline.

The collector records observations only. This verifier owns the strict schema,
event ordering, epoch/generation progress, and resource-release acceptance.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import sys
from typing import Any


EXPECTED_SCHEMA = 1
EXPECTED_SPIKE = "SPK-002-LIFECYCLE"
EXPECTED_SCOPE = "controlled-window-lifecycle-only"
EXPECTED_CAPTURE_TARGET = "HWND"
EXPECTED_OWNER_THREAD = "single"
EXPECTED_CALLBACKS = "notification-only"
EXPECTED_SURFACE_FORMAT = "DXGI_FORMAT_R16G16B16A16_FLOAT"

ROOT_FIELDS = {
    "schemaVersion",
    "spikeId",
    "applicationVersion",
    "revision",
    "capturedAtUtc",
    "timeoutMs",
    "contract",
    "device",
    "scenarios",
}
CONTRACT_FIELDS = {
    "scope",
    "captureTarget",
    "ownerThread",
    "callbacks",
    "surfaceFormat",
    "systemBorderAllowed",
    "cursorExcluded",
}
DEVICE_FIELDS = {
    "driverType",
    "adapter",
    "adapterLuid",
    "vendorId",
    "deviceId",
    "featureLevel",
}
CAPABILITY_FIELDS = {"borderHidden", "cursorExcluded"}
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
EVENT_FIELDS = {
    "target-created": {"sequence", "kind", "size"},
    "sensor-started": {"sequence", "kind", "size", "epoch"},
    "reconfigure-required": {"sequence", "kind", "size", "epoch"},
    "frame-pool-recreated": {"sequence", "kind", "size", "epoch"},
    "frame-updated": {"sequence", "kind", "size", "generation", "epoch"},
    "resize-requested": {"sequence", "kind", "size"},
    "target-closed": {"sequence", "kind"},
    "sensor-stop-requested": {"sequence", "kind", "epoch"},
    "sensor-stopped": {"sequence", "kind", "epoch"},
    "sensor-stop-repeated": {"sequence", "kind", "epoch"},
    "sensor-destroyed": {"sequence", "kind"},
}


class ValidationError(ValueError):
    """Raised when lifecycle evidence is malformed or unacceptable."""


@dataclass(frozen=True)
class FrameRecord:
    generation: int
    epoch: int
    size: tuple[int, int]


@dataclass(frozen=True)
class EventRecord:
    kind: str
    size: tuple[int, int] | None
    generation: int | None
    epoch: int | None


@dataclass(frozen=True)
class ReconfigureRecord:
    size: tuple[int, int]
    epoch_before: int
    epoch_after: int


@dataclass(frozen=True)
class ScenarioResult:
    event_count: int
    reconfiguration_count: int
    frames_acquired: int


@dataclass(frozen=True)
class VerificationResult:
    schema_version: int
    spike_id: str
    capture_revision: str
    status: str
    resize_close_events: int
    restart_stop_events: int
    resize_close_reconfigurations: int
    restart_stop_reconfigurations: int
    total_frames_acquired: int


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


def _size(value: Any, label: str) -> tuple[int, int]:
    size = _object(value, label)
    _require_fields(size, {"width", "height"}, label)
    return (
        _integer(size["width"], f"{label}.width", minimum=1),
        _integer(size["height"], f"{label}.height", minimum=1),
    )


def _frame(value: Any, label: str) -> FrameRecord:
    frame = _object(value, label)
    _require_fields(frame, {"generation", "epoch", "size"}, label)
    return FrameRecord(
        generation=_integer(
            frame["generation"], f"{label}.generation", minimum=1
        ),
        epoch=_integer(frame["epoch"], f"{label}.epoch", minimum=1),
        size=_size(frame["size"], f"{label}.size"),
    )


def _event(value: Any, label: str, expected_sequence: int) -> EventRecord:
    event = _object(value, label)
    kind = _string(event.get("kind"), f"{label}.kind")
    if kind not in EVENT_FIELDS:
        raise ValidationError(f"{label}.kind is unknown: {kind!r}")
    _require_fields(event, EVENT_FIELDS[kind], label)
    sequence = _integer(event["sequence"], f"{label}.sequence", minimum=0)
    if sequence != expected_sequence:
        raise ValidationError(
            f"{label}.sequence must be {expected_sequence}, observed {sequence}"
        )
    return EventRecord(
        kind=kind,
        size=_size(event["size"], f"{label}.size")
        if "size" in event
        else None,
        generation=_integer(
            event["generation"], f"{label}.generation", minimum=1
        )
        if "generation" in event
        else None,
        epoch=_integer(event["epoch"], f"{label}.epoch", minimum=1)
        if "epoch" in event
        else None,
    )


def _events(value: Any, label: str) -> list[EventRecord]:
    raw_events = _array(value, label)
    if not raw_events:
        raise ValidationError(f"{label} must not be empty")
    return [
        _event(raw_event, f"{label}[{index}]", index)
        for index, raw_event in enumerate(raw_events)
    ]


def _expect_event(
    events: list[EventRecord], index: int, expected_kind: str, label: str
) -> tuple[EventRecord, int]:
    if index >= len(events):
        raise ValidationError(
            f"{label} expected {expected_kind!r}, but the event stream ended"
        )
    event = events[index]
    if event.kind != expected_kind:
        raise ValidationError(
            f"{label} expected {expected_kind!r}, observed {event.kind!r}"
        )
    return event, index + 1


def _consume_reconfigure_pairs(
    events: list[EventRecord],
    index: int,
    current_epoch: int,
    label: str,
) -> tuple[int, int, list[ReconfigureRecord]]:
    records: list[ReconfigureRecord] = []
    while index < len(events) and events[index].kind == "reconfigure-required":
        required = events[index]
        recreated, next_index = _expect_event(
            events,
            index + 1,
            "frame-pool-recreated",
            f"{label}.events[{index + 1}]",
        )
        if required.epoch != current_epoch:
            raise ValidationError(
                f"{label} reconfigure-required epoch must equal {current_epoch}"
            )
        if recreated.epoch is None or recreated.epoch <= current_epoch:
            raise ValidationError(f"{label} recreated epoch did not advance")
        if required.size != recreated.size:
            raise ValidationError(
                f"{label} reconfigure/recreate sizes must match"
            )
        if required.size is None:
            raise ValidationError(f"{label} reconfigure size is missing")
        records.append(
            ReconfigureRecord(
                size=required.size,
                epoch_before=current_epoch,
                epoch_after=recreated.epoch,
            )
        )
        current_epoch = recreated.epoch
        index = next_index
    return index, current_epoch, records


def _require_same_frame(
    summary: FrameRecord, event: EventRecord, label: str
) -> None:
    observed = FrameRecord(
        generation=event.generation or 0,
        epoch=event.epoch or 0,
        size=event.size or (0, 0),
    )
    if summary != observed:
        raise ValidationError(f"{label} does not match its frame-updated event")


def _validate_capabilities(value: Any, label: str) -> None:
    capabilities = _object(value, label)
    _require_fields(capabilities, CAPABILITY_FIELDS, label)
    _boolean(capabilities["borderHidden"], f"{label}.borderHidden")
    if not _boolean(
        capabilities["cursorExcluded"], f"{label}.cursorExcluded"
    ):
        raise ValidationError(f"{label}.cursorExcluded must be true")


def _validate_ledger(
    value: Any,
    label: str,
    *,
    minimum_recreated: int,
    observed_recreated: int,
) -> dict[str, int]:
    ledger = _object(value, label)
    _require_fields(ledger, LEDGER_FIELDS, label)
    counts = {
        name: _integer(ledger[name], f"{label}.{name}", minimum=0)
        for name in LEDGER_COUNT_FIELDS
    }
    if not _boolean(ledger["allReleased"], f"{label}.allReleased"):
        raise ValidationError(f"{label}.allReleased must be true")
    if counts["failures"] != 0:
        raise ValidationError(f"{label}.failures must be zero")
    for name in LIVE_LEDGER_FIELDS:
        if counts[name] != 0:
            raise ValidationError(f"{label}.{name} must be zero")
    for acquired, released in LEDGER_PAIRS:
        if counts[acquired] != counts[released]:
            raise ValidationError(
                f"{label} count mismatch: {acquired}={counts[acquired]}, "
                f"{released}={counts[released]}"
            )

    for name in (
        "framesAcquired",
    ):
        if counts[name] < 1:
            raise ValidationError(f"{label}.{name} must be at least 1")
    for name in (
        "framePoolsCreated",
        "sessionsCreated",
        "frameArrivedRegistrations",
        "itemClosedRegistrations",
    ):
        if counts[name] != 1:
            raise ValidationError(f"{label}.{name} must equal one session resource")
    if counts["framePoolsRecreated"] < minimum_recreated:
        raise ValidationError(
            f"{label}.framePoolsRecreated must be at least {minimum_recreated}"
        )
    if counts["framePoolsRecreated"] != observed_recreated:
        raise ValidationError(
            f"{label}.framePoolsRecreated does not match lifecycle events"
        )
    return counts


def _parse_reconfigurations(
    value: Any, label: str
) -> list[ReconfigureRecord]:
    records: list[ReconfigureRecord] = []
    for index, raw_record in enumerate(_array(value, label)):
        record_label = f"{label}[{index}]"
        record = _object(raw_record, record_label)
        _require_fields(
            record, {"size", "epochBefore", "epochAfter"}, record_label
        )
        epoch_before = _integer(
            record["epochBefore"], f"{record_label}.epochBefore", minimum=1
        )
        epoch_after = _integer(
            record["epochAfter"], f"{record_label}.epochAfter", minimum=1
        )
        if epoch_after <= epoch_before:
            raise ValidationError(f"{record_label} epoch did not advance")
        records.append(
            ReconfigureRecord(
                size=_size(record["size"], f"{record_label}.size"),
                epoch_before=epoch_before,
                epoch_after=epoch_after,
            )
        )
    return records


def _validate_resize_close(value: Any) -> ScenarioResult:
    label = "scenarios.resizeClose"
    scenario = _object(value, label)
    _require_fields(
        scenario,
        {
            "events",
            "initialFrame",
            "requestedResize",
            "reconfigurations",
            "resizedFrame",
            "capabilities",
            "ledger",
        },
        label,
    )
    events = _events(scenario["events"], f"{label}.events")
    index = 0
    target_created, index = _expect_event(
        events, index, "target-created", f"{label}.events[{index}]"
    )
    sensor_started, index = _expect_event(
        events, index, "sensor-started", f"{label}.events[{index}]"
    )
    if target_created.size != sensor_started.size:
        raise ValidationError(f"{label} target and sensor start sizes must match")
    if sensor_started.epoch is None:
        raise ValidationError(f"{label} sensor start epoch is missing")

    index, current_epoch, startup_records = _consume_reconfigure_pairs(
        events, index, sensor_started.epoch, label
    )
    initial_event, index = _expect_event(
        events, index, "frame-updated", f"{label}.events[{index}]"
    )
    if initial_event.epoch != current_epoch:
        raise ValidationError(f"{label} initial frame epoch is stale")
    expected_initial_size = (
        startup_records[-1].size if startup_records else target_created.size
    )
    if initial_event.size != expected_initial_size:
        raise ValidationError(
            f"{label} initial frame size differs from the active pool"
        )

    resize_event, index = _expect_event(
        events, index, "resize-requested", f"{label}.events[{index}]"
    )
    index, current_epoch, resize_records = _consume_reconfigure_pairs(
        events, index, current_epoch, label
    )
    if not resize_records:
        raise ValidationError(
            f"{label} resize-requested must be followed by a "
            "reconfigure-required/frame-pool-recreated pair"
        )
    resized_event, index = _expect_event(
        events, index, "frame-updated", f"{label}.events[{index}]"
    )
    if resized_event.epoch != current_epoch:
        raise ValidationError(f"{label} resized frame epoch is stale")
    if resize_event.size != resized_event.size:
        raise ValidationError(f"{label} final frame size must equal requested resize")
    if resize_records[-1].size != resized_event.size:
        raise ValidationError(
            f"{label} final recreated pool size must equal resized frame"
        )
    if resized_event.size == initial_event.size:
        raise ValidationError(f"{label} resized frame size did not change")
    if (
        resized_event.generation is None
        or initial_event.generation is None
        or resized_event.generation <= initial_event.generation
    ):
        raise ValidationError(f"{label} resized frame generation did not advance")
    if resized_event.epoch is None or initial_event.epoch is None:
        raise ValidationError(f"{label} frame epoch is missing")
    if resized_event.epoch <= initial_event.epoch:
        raise ValidationError(f"{label} resized frame epoch did not advance")

    _, index = _expect_event(
        events, index, "target-closed", f"{label}.events[{index}]"
    )
    stopped_event, index = _expect_event(
        events, index, "sensor-stopped", f"{label}.events[{index}]"
    )
    if stopped_event.epoch != current_epoch:
        raise ValidationError(f"{label} stopped epoch differs from the active epoch")
    _, index = _expect_event(
        events, index, "sensor-destroyed", f"{label}.events[{index}]"
    )
    if index != len(events):
        raise ValidationError(f"{label} has unexpected events after sensor-destroyed")

    initial_frame = _frame(scenario["initialFrame"], f"{label}.initialFrame")
    resized_frame = _frame(scenario["resizedFrame"], f"{label}.resizedFrame")
    _require_same_frame(initial_frame, initial_event, f"{label}.initialFrame")
    _require_same_frame(resized_frame, resized_event, f"{label}.resizedFrame")
    requested_resize = _size(
        scenario["requestedResize"], f"{label}.requestedResize"
    )
    if requested_resize != resize_event.size:
        raise ValidationError(
            f"{label}.requestedResize does not match resize-requested"
        )

    observed_records = startup_records + resize_records
    declared_records = _parse_reconfigurations(
        scenario["reconfigurations"], f"{label}.reconfigurations"
    )
    if declared_records != observed_records:
        raise ValidationError(
            f"{label}.reconfigurations do not match lifecycle events"
        )
    _validate_capabilities(scenario["capabilities"], f"{label}.capabilities")
    ledger = _validate_ledger(
        scenario["ledger"],
        f"{label}.ledger",
        minimum_recreated=1,
        observed_recreated=len(observed_records),
    )
    return ScenarioResult(
        event_count=len(events),
        reconfiguration_count=len(observed_records),
        frames_acquired=ledger["framesAcquired"],
    )


def _validate_restart_stop(value: Any) -> ScenarioResult:
    label = "scenarios.restartStop"
    scenario = _object(value, label)
    _require_fields(
        scenario, {"events", "initialFrame", "capabilities", "ledger"}, label
    )
    events = _events(scenario["events"], f"{label}.events")
    index = 0
    target_created, index = _expect_event(
        events, index, "target-created", f"{label}.events[{index}]"
    )
    sensor_started, index = _expect_event(
        events, index, "sensor-started", f"{label}.events[{index}]"
    )
    if target_created.size != sensor_started.size:
        raise ValidationError(f"{label} target and sensor start sizes must match")
    if sensor_started.epoch is None:
        raise ValidationError(f"{label} sensor start epoch is missing")

    index, current_epoch, startup_records = _consume_reconfigure_pairs(
        events, index, sensor_started.epoch, label
    )
    initial_event, index = _expect_event(
        events, index, "frame-updated", f"{label}.events[{index}]"
    )
    if initial_event.epoch != current_epoch:
        raise ValidationError(f"{label} initial frame epoch is stale")
    expected_initial_size = (
        startup_records[-1].size if startup_records else target_created.size
    )
    if initial_event.size != expected_initial_size:
        raise ValidationError(
            f"{label} initial frame size differs from the active pool"
        )

    stop_requested, index = _expect_event(
        events, index, "sensor-stop-requested", f"{label}.events[{index}]"
    )
    stopped, index = _expect_event(
        events, index, "sensor-stopped", f"{label}.events[{index}]"
    )
    repeated, index = _expect_event(
        events, index, "sensor-stop-repeated", f"{label}.events[{index}]"
    )
    for event_name, event in (
        ("sensor-stop-requested", stop_requested),
        ("sensor-stopped", stopped),
        ("sensor-stop-repeated", repeated),
    ):
        if event.epoch != current_epoch:
            raise ValidationError(f"{label} {event_name} epoch is stale")
    _, index = _expect_event(
        events, index, "sensor-destroyed", f"{label}.events[{index}]"
    )
    _, index = _expect_event(
        events, index, "target-closed", f"{label}.events[{index}]"
    )
    if index != len(events):
        raise ValidationError(f"{label} has unexpected events after target-closed")

    initial_frame = _frame(scenario["initialFrame"], f"{label}.initialFrame")
    _require_same_frame(initial_frame, initial_event, f"{label}.initialFrame")
    _validate_capabilities(scenario["capabilities"], f"{label}.capabilities")
    ledger = _validate_ledger(
        scenario["ledger"],
        f"{label}.ledger",
        minimum_recreated=0,
        observed_recreated=len(startup_records),
    )
    return ScenarioResult(
        event_count=len(events),
        reconfiguration_count=len(startup_records),
        frames_acquired=ledger["framesAcquired"],
    )


def _validate_contract(value: Any) -> None:
    contract = _object(value, "contract")
    _require_fields(contract, CONTRACT_FIELDS, "contract")
    _exact(contract["scope"], EXPECTED_SCOPE, "contract.scope")
    _exact(
        contract["captureTarget"],
        EXPECTED_CAPTURE_TARGET,
        "contract.captureTarget",
    )
    _exact(
        contract["ownerThread"], EXPECTED_OWNER_THREAD, "contract.ownerThread"
    )
    _exact(contract["callbacks"], EXPECTED_CALLBACKS, "contract.callbacks")
    _exact(
        contract["surfaceFormat"],
        EXPECTED_SURFACE_FORMAT,
        "contract.surfaceFormat",
    )
    if not _boolean(
        contract["systemBorderAllowed"], "contract.systemBorderAllowed"
    ):
        raise ValidationError("contract.systemBorderAllowed must be true")
    if not _boolean(contract["cursorExcluded"], "contract.cursorExcluded"):
        raise ValidationError("contract.cursorExcluded must be true")


def _validate_device(value: Any) -> None:
    device = _object(value, "device")
    _require_fields(device, DEVICE_FIELDS, "device")
    if device["driverType"] != "hardware":
        raise ValidationError("device.driverType must be hardware")
    _string(device["adapter"], "device.adapter")
    luid = _object(device["adapterLuid"], "device.adapterLuid")
    _require_fields(luid, {"low", "high"}, "device.adapterLuid")
    _integer(luid["low"], "device.adapterLuid.low", minimum=0)
    _integer(luid["high"], "device.adapterLuid.high")
    _integer(device["vendorId"], "device.vendorId", minimum=0)
    _integer(device["deviceId"], "device.deviceId", minimum=0)
    _integer(device["featureLevel"], "device.featureLevel", minimum=1)


def validate_capture(document: Any) -> VerificationResult:
    capture = _object(document, "capture")
    _require_fields(capture, ROOT_FIELDS, "capture")
    schema_version = _integer(
        capture["schemaVersion"], "schemaVersion", minimum=1
    )
    _exact(schema_version, EXPECTED_SCHEMA, "schemaVersion")
    spike_id = _string(capture["spikeId"], "spikeId")
    _exact(spike_id, EXPECTED_SPIKE, "spikeId")
    _string(capture["applicationVersion"], "applicationVersion")
    revision = _string(capture["revision"], "revision")
    _string(capture["capturedAtUtc"], "capturedAtUtc")
    _integer(capture["timeoutMs"], "timeoutMs", minimum=1)
    _validate_contract(capture["contract"])
    _validate_device(capture["device"])

    scenarios = _object(capture["scenarios"], "scenarios")
    _require_fields(scenarios, {"resizeClose", "restartStop"}, "scenarios")
    resize_close = _validate_resize_close(scenarios["resizeClose"])
    restart_stop = _validate_restart_stop(scenarios["restartStop"])
    return VerificationResult(
        schema_version=schema_version,
        spike_id=spike_id,
        capture_revision=revision,
        status="accepted",
        resize_close_events=resize_close.event_count,
        restart_stop_events=restart_stop.event_count,
        resize_close_reconfigurations=resize_close.reconfiguration_count,
        restart_stop_reconfigurations=restart_stop.reconfiguration_count,
        total_frames_acquired=(
            resize_close.frames_acquired + restart_stop.frames_acquired
        ),
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
    temporary = path.with_name(path.name + ".tmp")
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        # Keeping the temporary beside the result makes replacement atomic.
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
    parser.add_argument("capture", type=Path, help="SPK-002 lifecycle.json")
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
        f"resizeClose={result.resize_close_events} events, "
        f"restartStop={result.restart_stop_events} events, "
        f"reconfigurations="
        f"{result.resize_close_reconfigurations + result.restart_stop_reconfigurations}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
