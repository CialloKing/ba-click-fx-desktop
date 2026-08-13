#!/usr/bin/env python3
"""Validate the Unity FX_Touch particle-state fixture v2.

The fixture is an observation from Unity, not a native RNG golden.  This
validator deliberately checks only the serialization contract and numerical
shape needed to consume that observation deterministically.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import sys
from typing import Any


EXPECTED_SCHEMA = 2
EXPECTED_FIXTURE = "UnityParticleStateV2"
EXPECTED_WIDTH = 1950
EXPECTED_HEIGHT = 1097
EXPECTED_TIME_SECONDS = 0.05
EXPECTED_TIME_MILLISECONDS = 50
EXPECTED_SEED_BASE = 20260716
EXPECTED_SEED_STRIDE = 7919
EXPECTED_SEED_FORMULA = "seedBase + index * seedStride"
EXPECTED_SPACES = {"Local", "World", "Custom"}


class ValidationError(ValueError):
    """Raised when a fixture is malformed or violates the v1 contract."""


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


def _integer(value: Any, label: str, *, minimum: int | None = None) -> int:
    if type(value) is not int:
        raise ValidationError(f"{label} must be an integer")
    if minimum is not None and value < minimum:
        raise ValidationError(f"{label} must be >= {minimum}")
    return value


def _uint32(value: Any, label: str) -> int:
    result = _integer(value, label, minimum=0)
    if result > 0xFFFFFFFF:
        raise ValidationError(f"{label} must fit uint32")
    return result


def _finite(value: Any, label: str) -> float:
    if type(value) not in (int, float) or isinstance(value, bool):
        raise ValidationError(f"{label} must be a number")
    result = float(value)
    if not math.isfinite(result):
        raise ValidationError(f"{label} must be finite")
    return result


def _exact_number(value: Any, expected: float, label: str) -> None:
    actual = _finite(value, label)
    if actual != expected:
        raise ValidationError(f"{label} must equal {expected:g}")


def _vector(value: Any, dimensions: int, label: str) -> None:
    values = _array(value, label)
    if len(values) != dimensions:
        raise ValidationError(f"{label} must contain exactly {dimensions} values")
    for index, item in enumerate(values):
        _finite(item, f"{label}[{index}]")


def _named_vector(value: Any, names: tuple[str, ...], label: str) -> None:
    item = _object(value, label)
    if set(item) != set(names):
        raise ValidationError(f"{label} fields must be {','.join(names)}")
    for name in names:
        _finite(item[name], f"{label}.{name}")


def _validate_particle(value: Any, system_label: str, expected_index: int) -> None:
    particle = _object(value, f"{system_label}.particles[{expected_index}]")
    label = f"{system_label}.particles[{expected_index}]"
    required = {
        "index",
        "randomSeed",
        "atlasFrame",
        "position",
        "worldPosition",
        "projectedPixel",
        "velocity",
        "startLifetime",
        "remainingLifetime",
        "size",
        "rotation",
        "color",
        "custom1",
    }
    if set(particle) != required:
        raise ValidationError(f"{label} fields differ from the v1 contract")
    if _integer(particle["index"], f"{label}.index", minimum=0) != expected_index:
        raise ValidationError(f"{label}.index is not in serialized order")
    _uint32(particle["randomSeed"], f"{label}.randomSeed")
    _integer(particle["atlasFrame"], f"{label}.atlasFrame", minimum=0)
    _named_vector(particle["position"], ("x", "y", "z"), f"{label}.position")
    _named_vector(
        particle["worldPosition"], ("x", "y", "z"), f"{label}.worldPosition"
    )
    _named_vector(
        particle["projectedPixel"], ("x", "y"), f"{label}.projectedPixel"
    )
    _named_vector(particle["velocity"], ("x", "y", "z"), f"{label}.velocity")
    for name in ("startLifetime", "remainingLifetime", "size"):
        if _finite(particle[name], f"{label}.{name}") < 0:
            raise ValidationError(f"{label}.{name} must be non-negative")
    _named_vector(
        particle["rotation"], ("x", "y", "z", "w"), f"{label}.rotation"
    )
    _named_vector(
        particle["color"], ("r", "g", "b", "a"), f"{label}.color"
    )
    _named_vector(particle["custom1"], ("x", "y", "z", "w"), f"{label}.custom1")


def _validate_system(value: Any, expected_index: int) -> None:
    system = _object(value, f"systems[{expected_index}]")
    label = f"systems[{expected_index}]"
    required = {
        "index",
        "path",
        "name",
        "seed",
        "simulationSpace",
        "localToWorldMatrix",
        "particleCount",
        "particles",
    }
    if set(system) != required:
        raise ValidationError(f"{label} fields differ from the v1 contract")
    if _integer(system["index"], f"{label}.index", minimum=0) != expected_index:
        raise ValidationError(f"{label}.index is not in serialized order")
    path = _string(system["path"], f"{label}.path")
    if path.startswith("/") or "\\" in path or ".." in path.split("/"):
        raise ValidationError(f"{label}.path must be a normalized hierarchy path")
    _string(system["name"], f"{label}.name")
    seed = _uint32(system["seed"], f"{label}.seed")
    expected_seed = EXPECTED_SEED_BASE + expected_index * EXPECTED_SEED_STRIDE
    if seed != expected_seed:
        raise ValidationError(f"{label}.seed must equal seedBase + index * seedStride")
    space = _string(system["simulationSpace"], f"{label}.simulationSpace")
    if space not in EXPECTED_SPACES:
        raise ValidationError(f"{label}.simulationSpace is unsupported")
    _vector(system["localToWorldMatrix"], 16, f"{label}.localToWorldMatrix")
    count = _integer(system["particleCount"], f"{label}.particleCount", minimum=0)
    particles = _array(system["particles"], f"{label}.particles")
    if len(particles) != count:
        raise ValidationError(f"{label}.particleCount does not match particles length")
    for particle_index, particle in enumerate(particles):
        _validate_particle(particle, label, particle_index)
        frame = particle["atlasFrame"]
        if system["name"] in {"Ring (3)", "Ring (4)"}:
            if frame > 1:
                raise ValidationError(f"{label}.particles[{particle_index}].atlasFrame must be 0 or 1")
        elif frame != 0:
            raise ValidationError(f"{label}.particles[{particle_index}].atlasFrame must be 0")


def validate_fixture(value: Any) -> dict[str, Any]:
    fixture = _object(value, "fixture")
    required = {
        "schema",
        "fixture",
        "unityVersion",
        "renderSize",
        "captureTimeSeconds",
        "captureTimeMilliseconds",
        "seedBase",
        "seedStride",
        "seedFormula",
        "deterministic",
        "systems",
    }
    if set(fixture) != required:
        raise ValidationError("fixture fields differ from the v1 contract")
    if _integer(fixture["schema"], "fixture.schema") != EXPECTED_SCHEMA:
        raise ValidationError("fixture.schema must be 2")
    if _string(fixture["fixture"], "fixture.fixture") != EXPECTED_FIXTURE:
        raise ValidationError("fixture.fixture is unsupported")
    _string(fixture["unityVersion"], "fixture.unityVersion")
    render_size = _object(fixture["renderSize"], "fixture.renderSize")
    if set(render_size) != {"width", "height"}:
        raise ValidationError("fixture.renderSize fields differ from the v1 contract")
    if _integer(render_size["width"], "fixture.renderSize.width") != EXPECTED_WIDTH:
        raise ValidationError("fixture.renderSize.width must be 1950")
    if _integer(render_size["height"], "fixture.renderSize.height") != EXPECTED_HEIGHT:
        raise ValidationError("fixture.renderSize.height must be 1097")
    _exact_number(fixture["captureTimeSeconds"], EXPECTED_TIME_SECONDS, "fixture.captureTimeSeconds")
    if _integer(fixture["captureTimeMilliseconds"], "fixture.captureTimeMilliseconds") != EXPECTED_TIME_MILLISECONDS:
        raise ValidationError("fixture.captureTimeMilliseconds must be 50")
    if _integer(fixture["seedBase"], "fixture.seedBase") != EXPECTED_SEED_BASE:
        raise ValidationError("fixture.seedBase differs from the locked seed")
    if _integer(fixture["seedStride"], "fixture.seedStride") != EXPECTED_SEED_STRIDE:
        raise ValidationError("fixture.seedStride must be 7919")
    if _string(fixture["seedFormula"], "fixture.seedFormula") != EXPECTED_SEED_FORMULA:
        raise ValidationError("fixture.seedFormula differs from the locked formula")
    deterministic = _object(fixture["deterministic"], "fixture.deterministic")
    if set(deterministic) != {"runs", "byteIdentical"}:
        raise ValidationError(
            "fixture.deterministic fields must be runs,byteIdentical"
        )
    if _integer(deterministic["runs"], "fixture.deterministic.runs") != 2:
        raise ValidationError("fixture.deterministic.runs must be 2")
    if deterministic["byteIdentical"] is not True:
        raise ValidationError("fixture.deterministic.byteIdentical must be true")
    systems = _array(fixture["systems"], "fixture.systems")
    if not systems:
        raise ValidationError("fixture.systems must not be empty")
    for system_index, system in enumerate(systems):
        _validate_system(system, system_index)
    return fixture


def _read_fixture(path: Path) -> bytes:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise ValidationError(f"unable to read fixture {path}: {error}") from error
    if not data:
        raise ValidationError("fixture is empty")
    return data


def validate_path(path: Path) -> None:
    first_bytes = _read_fixture(path)
    try:
        first = json.loads(
            first_bytes.decode("utf-8"), object_pairs_hook=_reject_duplicate_keys
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValidationError(f"invalid UTF-8 JSON fixture: {error}") from error
    validate_fixture(first)

    # Unity writes two independent captures and compares their UTF-8 bytes.
    # Accept an optional sibling ``.repeat`` only as a diagnostic check; the
    # production fixture itself must remain a single stable JSON document.
    repeat_path = path.with_name(path.name + ".repeat")
    if repeat_path.exists():
        repeat_bytes = _read_fixture(repeat_path)
        if first_bytes != repeat_bytes:
            raise ValidationError("deterministic repeat bytes differ")
        try:
            validate_fixture(
                json.loads(
                    repeat_bytes.decode("utf-8"),
                    object_pairs_hook=_reject_duplicate_keys,
                )
            )
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ValidationError(f"invalid repeat JSON fixture: {error}") from error


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", type=Path)
    args = parser.parse_args(argv)
    try:
        validate_path(args.fixture)
    except ValidationError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(f"Unity particle fixture verified: {args.fixture}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
