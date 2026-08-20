#!/usr/bin/env python3
"""Contract tests for the explicit OBS Spout2 scene repair tool."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


if len(sys.argv) < 3:
    raise RuntimeError("expected PowerShell and repair script paths")

POWERSHELL = Path(sys.argv.pop(1))
REPAIR_SCRIPT = Path(sys.argv.pop(1))
WIDTH = 1280
HEIGHT = 720
TARGET_SENDER = "ba-click-fx-desktop"


def _fixture(*, target_mode: int = 3, target_width: int = 0) -> dict[str, object]:
    return {
        "sources": [
            {
                "uuid": "target-spout",
                "id": "spout_capture",
                "name": "BAFX",
                "settings": {
                    "spoutsenders": TARGET_SENDER,
                    "compositemode": target_mode,
                },
            },
            {
                "uuid": "other-spout",
                "id": "spout_capture",
                "name": "Other",
                "settings": {
                    "spoutsenders": "another-sender",
                    "compositemode": 3,
                },
            },
            {
                "uuid": "target-scene",
                "id": "scene",
                "name": "Target Scene",
                "settings": {
                    "items": [
                        {
                            "source_uuid": "target-spout",
                            "bounds_type": 2 if target_width else 0,
                            "bounds": {
                                "x": target_width,
                                "y": HEIGHT if target_width else 0,
                            },
                            "bounds_rel": {"x": 0, "y": 0},
                        }
                    ]
                },
            },
            {
                "uuid": "other-scene",
                "id": "scene",
                "name": "Other Scene",
                "settings": {
                    "items": [
                        {
                            "source_uuid": "other-spout",
                            "bounds_type": 1,
                            "bounds": {"x": 7, "y": 9},
                            "bounds_rel": {"x": 1, "y": 1},
                        }
                    ]
                },
            },
        ]
    }


def _write_fixture(path: Path, document: dict[str, object]) -> bytes:
    path.write_text(json.dumps(document, ensure_ascii=False), encoding="utf-8")
    return path.read_bytes()


def _run(scene: Path, *extra: str) -> subprocess.CompletedProcess[str]:
    command = [
        str(POWERSHELL),
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(REPAIR_SCRIPT),
        "-ScenePath",
        str(scene),
        "-Width",
        str(WIDTH),
        "-Height",
        str(HEIGHT),
        *extra,
    ]
    return subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        timeout=10,
        encoding="utf-8",
        errors="replace",
    )


def _read_json(path: Path) -> dict[str, object]:
    return json.loads(path.read_text(encoding="utf-8-sig"))


class RepairObsSpout2SceneTests(unittest.TestCase):
    def test_check_only_reports_without_writing(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            scene = Path(directory) / "scene.json"
            original = _write_fixture(scene, _fixture())

            result = _run(scene, "-CheckOnly")

            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertEqual(scene.read_bytes(), original)
            self.assertFalse(Path(f"{scene}.bak").exists())

    def test_repair_backs_up_and_only_changes_the_named_sender(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            scene = Path(directory) / "scene.json"
            original = _write_fixture(scene, _fixture())

            result = _run(scene)

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(Path(f"{scene}.bak").read_bytes(), original)
            repaired = _read_json(scene)
            sources = {source["uuid"]: source for source in repaired["sources"]}
            self.assertEqual(sources["target-spout"]["settings"]["compositemode"], 4)
            self.assertEqual(sources["other-spout"]["settings"]["compositemode"], 3)

            target_items = sources["target-scene"]["settings"]["items"]
            other_items = sources["other-scene"]["settings"]["items"]
            self.assertIsInstance(target_items, list)
            self.assertIsInstance(other_items, list)
            self.assertEqual(len(target_items), 1)
            self.assertEqual(len(other_items), 1)
            self.assertEqual(target_items[0]["bounds_type"], 2)
            self.assertEqual(target_items[0]["bounds"], {"x": WIDTH, "y": HEIGHT})
            self.assertEqual(other_items[0]["bounds"], {"x": 7, "y": 9})

    def test_valid_scene_is_not_rewritten(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            scene = Path(directory) / "scene.json"
            original = _write_fixture(
                scene,
                _fixture(target_mode=4, target_width=WIDTH),
            )

            result = _run(scene)

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(scene.read_bytes(), original)
            self.assertFalse(Path(f"{scene}.bak").exists())

    def test_unrelated_spout_source_is_rejected_without_writing(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            scene = Path(directory) / "scene.json"
            document = _fixture()
            document["sources"][0]["settings"]["spoutsenders"] = "wrong-sender"
            original = _write_fixture(scene, document)

            result = _run(scene)

            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(scene.read_bytes(), original)
            self.assertFalse(Path(f"{scene}.bak").exists())


if __name__ == "__main__":
    unittest.main()
