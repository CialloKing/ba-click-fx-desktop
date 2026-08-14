#!/usr/bin/env python3
"""Generate the capture-only C++ observation table from Unity fixture v2."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
from pathlib import Path
import sys
from typing import Any


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
VALIDATOR_PATH = REPOSITORY_ROOT / "tools" / "verify-unity-particle-fixture.py"
DEFAULT_OUTPUT = (
    REPOSITORY_ROOT
    / "src"
    / "reference"
    / "src"
    / "unity_particle_fixture_v2.generated.inc"
)

SPEC = importlib.util.spec_from_file_location("verify_unity_particle_fixture", VALIDATOR_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"unable to load fixture validator: {VALIDATOR_PATH}")
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)


SYSTEM_CONTRACT = {
    "FX_Touch_ParticleFixture": None,
    "MeshTri": "DissolveRing",
    "ring": "CenterDisk",
    "Ring (3)": "Triangle",
    "Ring (4)": "Triangle",
}
NATIVE_RENDER_ORDER = {
    "ring": 0,
    "MeshTri": 1,
    "Ring (3)": 2,
    "Ring (4)": 3,
}


def _float_literal(value: Any) -> str:
    number = float(value)
    text = format(number, ".10g")
    if "e" not in text.lower() and "." not in text:
        text += ".0"
    return text + "F"


def _require_fixture_shape(fixture: dict[str, Any]) -> list[dict[str, Any]]:
    systems = fixture["systems"]
    actual_names = [system["name"] for system in systems]
    if actual_names != list(SYSTEM_CONTRACT):
        raise VERIFY.ValidationError(
            "fixture systems must keep the locked root, MeshTri, ring, Ring (3), Ring (4) order"
        )

    if systems[0]["particleCount"] != 0:
        raise VERIFY.ValidationError("fixture root must not contain particles")
    for system in systems:
        if system["name"] != "FX_Touch_ParticleFixture" and system["simulationSpace"] != "World":
            raise VERIFY.ValidationError(
                f"system {system['name']} must use World simulation space"
            )
    return systems


def _system_scale(system: dict[str, Any]) -> float:
    matrix = system["localToWorldMatrix"]
    scale_x = float(matrix[0])
    scale_y = float(matrix[5])
    if scale_x <= 0.0 or abs(scale_x - scale_y) > 1.0e-6:
        raise VERIFY.ValidationError(
            f"system {system['name']} must have a positive uniform XY scale"
        )
    return scale_x


def _particle_row(system: dict[str, Any], particle: dict[str, Any]) -> list[str]:
    rotation = particle["rotation"]
    if abs(float(rotation["x"])) > 1.0e-6 or abs(float(rotation["y"])) > 1.0e-6:
        raise VERIFY.ValidationError(
            f"system {system['name']} contains a non-planar particle rotation"
        )

    projected = particle["projectedPixel"]
    color = particle["color"]
    custom1 = particle["custom1"]
    kind = SYSTEM_CONTRACT[system["name"]]
    return [
        "    UnityParticleObservation{",
        f"        UnityParticleSystemKind::{kind},",
        "        FixturePoint{" + _float_literal(projected["x"]) + ", "
        + _float_literal(projected["y"]) + "},",
        f"        {_float_literal(_system_scale(system))},",
        f"        {_float_literal(particle['size'])},",
        "        FixtureRotation{" + _float_literal(rotation["z"]) + ", "
        + _float_literal(rotation["w"]) + "},",
        "        FixtureColor{"
        + ", ".join(
            _float_literal(color[channel]) for channel in ("r", "g", "b", "a")
        )
        + "},",
        f"        {_float_literal(custom1['x'])},",
        f"        {particle['atlasFrame']}U}},",
    ]


def generate_include(fixtures: list[tuple[dict[str, Any], bytes]]) -> bytes:
    fixtures_by_age: dict[int, tuple[dict[str, Any], bytes]] = {}
    for fixture, fixture_bytes in fixtures:
        VERIFY.validate_fixture(fixture)
        age = int(fixture["captureTimeMilliseconds"])
        if age in fixtures_by_age:
            raise VERIFY.ValidationError(f"duplicate fixture age: {age}")
        fixtures_by_age[age] = (fixture, fixture_bytes)

    expected_ages = tuple(VERIFY.EXPECTED_TIMES_MILLISECONDS)
    actual_ages = tuple(sorted(fixtures_by_age))
    if actual_ages != expected_ages:
        raise VERIFY.ValidationError(
            f"fixture ages must be {','.join(str(age) for age in expected_ages)}"
        )

    descriptor_rows: list[str] = []
    observation_rows: list[str] = []
    observation_offsets = [0]
    for age in expected_ages:
        fixture, fixture_bytes = fixtures_by_age[age]
        systems = _require_fixture_shape(fixture)
        render_systems = sorted(
            (
                system
                for system in systems
                if system["name"] in NATIVE_RENDER_ORDER
            ),
            key=lambda system: NATIVE_RENDER_ORDER[system["name"]],
        )
        particle_count = sum(
            system["particleCount"] for system in render_systems
        )
        digest = hashlib.sha256(fixture_bytes).hexdigest().upper()
        descriptor_rows.extend(
            [
                "    UnityParticleFixtureDescriptor{",
                "        fixtureSchema,",
                "        fixtureName,",
                "        \"Reference/Diagnostics/ParticleStates/"
                f"FX_Touch_{age:04d}ms_particle-state-v2.json\",",
                f'        "{digest}",',
                "        fixtureViewport,",
                f"        {age}U,",
                f"        {particle_count}U}},",
            ]
        )
        for system in render_systems:
            for particle in system["particles"]:
                observation_rows.extend(_particle_row(system, particle))
        observation_offsets.append(observation_offsets[-1] + particle_count)

    offset_literals = ", ".join(f"{offset}U" for offset in observation_offsets)
    lines = [
        "// Generated by tools/generate-unity-particle-fixture.py. Do not edit.",
        "constexpr std::array<UnityParticleFixtureDescriptor, "
        f"{len(expected_ages)}U>",
        "    unityParticleFixtureDescriptors{{",
        *descriptor_rows,
        "}};",
        "constexpr std::array<std::size_t, "
        f"{len(observation_offsets)}U>",
        f"    unityParticleFixtureObservationOffsets{{{{{offset_literals}}}}};",
        "constexpr std::array<UnityParticleObservation, "
        f"{observation_offsets[-1]}U>",
        "    unityParticleObservations{{",
        *observation_rows,
        "}};",
        "",
    ]
    return "\n".join(lines).encode("ascii")


def run(fixture_paths: list[Path], output_path: Path, check: bool) -> None:
    fixtures = [VERIFY.load_fixture(path) for path in fixture_paths]
    generated = generate_include(fixtures)
    if check:
        try:
            current = output_path.read_bytes()
        except OSError as error:
            raise VERIFY.ValidationError(
                f"unable to read generated fixture {output_path}: {error}"
            ) from error
        if current != generated:
            raise VERIFY.ValidationError(
                f"generated fixture is stale: {output_path}"
            )
        return

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(generated)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixtures", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    try:
        run(args.fixtures, args.output, args.check)
    except (OSError, VERIFY.ValidationError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    action = "verified" if args.check else "generated"
    print(f"Unity particle C++ fixture {action}: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
