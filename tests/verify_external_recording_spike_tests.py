#!/usr/bin/env python3
"""Contract tests for the SPK-002 external-recording verifier."""

from __future__ import annotations

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


SCRIPT_PATH = (
    Path(__file__).resolve().parents[1]
    / "tools"
    / "verify-external-recording-spike.py"
)
SPEC = importlib.util.spec_from_file_location(
    "verify_external_recording_spike", SCRIPT_PATH
)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"unable to load {SCRIPT_PATH}")
VERIFY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = VERIFY
SPEC.loader.exec_module(VERIFY)

FFMPEG = shutil.which("ffmpeg")
FFPROBE = shutil.which("ffprobe")
TOOLS_AVAILABLE = FFMPEG is not None and FFPROBE is not None
WIDTH = 64
HEIGHT = 64
FRAME_RATE = 10
DURATION_SECONDS = 6


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _artifact_record(path: Path) -> dict[str, object]:
    return {
        "exists": True,
        "bytes": path.stat().st_size,
        "sha256": _sha256(path),
    }


def _event(
    session: str,
    sequence: int,
    pid: int,
    name: str,
    fields: dict[str, object] | None = None,
) -> str:
    values: dict[str, object] = {
        "Log.SchemaVersion": 2,
        "Log.SessionId": session,
        "Event.Sequence": sequence,
        "Event.Utc": f"2026-08-15T00:00:{sequence:02d}.000Z",
        "Event.MonotonicUs": sequence * 1000,
        "Event.ProcessId": pid,
        "Event.ThreadId": pid + 1,
        "Event.Level": "Info",
        "Event.Name": name,
    }
    values.update(fields or {})
    return "\n".join(f"{key}={value}" for key, value in values.items())


def _host_log(case_id: str, background_mode: str, pid: int) -> str:
    session = f"session-{case_id}"
    excluded = background_mode == "background-aware"
    support = "active" if excluded else "fallback-fx-only"
    affinity = "0x00000011" if excluded else "0x00000000"
    records = [
        ("Process.Startup", {"Product.Version": "0.1.0-test"}),
        (
            "Message",
            {
                "Event.Message": (
                    f"Capture.Exclusion.Requested={affinity};Observed={affinity};"
                    "Set=succeeded;SetError=0x00000000;Query=succeeded;"
                    "QueryError=0x00000000"
                )
            },
        ),
        ("Configuration.Applied", {"Background.Mode": background_mode}),
        ("SupportReport", {"Support.WGC": support}),
    ]
    if excluded:
        records.extend(
            [
                (
                    "BackgroundComposite.Participated",
                    {"Control.Generation": 1, "Frame.Id": 1},
                ),
                (
                    "Message",
                    {
                        "Event.Message": (
                            "BackgroundCapture.ResourceLedger.Phase=shutdown;"
                            "WGC.ResourceLedger.FramesAcquired=10;FramesClosed=10;"
                            "FramePoolsCreated=1;FramePoolsClosed=1;"
                            "FramePoolsRecreated=0;SessionsCreated=1;SessionsClosed=1;"
                            "FrameArrivedRegistrations=1;FrameArrivedUnregistrations=1;"
                            "ItemClosedRegistrations=1;ItemClosedUnregistrations=1;"
                            "LiveFrames=0;LiveFramePools=0;LiveSessions=0;"
                            "LiveFrameArrivedRegistrations=0;"
                            "LiveItemClosedRegistrations=0;Failures=0;AllReleased=true"
                        )
                    },
                ),
            ]
        )
    records.append(("Process.Exited", {}))
    return "\n---\n".join(
        _event(session, sequence, pid, name, fields)
        for sequence, (name, fields) in enumerate(records, 1)
    ) + "\n---\n"


def _process(pid: int, host: bool = False) -> dict[str, object]:
    record: dict[str, object] = {
        "state": "exited",
        "pid": pid,
        "startedAtUtc": "2026-08-15T00:00:00.000Z",
        "readyAtUtc": "2026-08-15T00:00:00.100Z" if host else None,
        "timeoutMs": 15000,
        "timedOut": False,
        "exitedAtUtc": "2026-08-15T00:00:07.500Z",
        "exitCode": 0,
    }
    if host:
        record["threadId"] = pid + 1
    return record


@unittest.skipUnless(TOOLS_AVAILABLE, "ffmpeg and ffprobe are required")
class ExternalRecordingFixture:
    video_payload: bytes
    probe_document: dict[str, object]

    @classmethod
    def setUpClass(cls) -> None:
        with tempfile.TemporaryDirectory() as directory:
            video = Path(directory) / VERIFY.VIDEO_NAME
            result = subprocess.run(
                [
                    str(FFMPEG),
                    "-v",
                    "error",
                    "-nostdin",
                    "-f",
                    "lavfi",
                    "-i",
                    f"color=c=black:s={WIDTH}x{HEIGHT}:r={FRAME_RATE}:d={DURATION_SECONDS}",
                    "-vf",
                    "drawbox=x=16:y=16:w=32:h=32:color=white:t=fill:enable='between(t,2.3,2.6)'",
                    "-an",
                    "-c:v",
                    "ffv1",
                    "-level",
                    "3",
                    "-pix_fmt",
                    "bgr0",
                    str(video),
                ],
                capture_output=True,
                timeout=10,
                check=False,
            )
            if result.returncode != 0:
                raise RuntimeError(result.stderr.decode(errors="replace"))
            probe = subprocess.run(
                [
                    str(FFPROBE),
                    "-v",
                    "error",
                    "-print_format",
                    "json",
                    "-show_format",
                    "-show_streams",
                    str(video),
                ],
                capture_output=True,
                text=True,
                timeout=5,
                check=False,
            )
            if probe.returncode != 0:
                raise RuntimeError(probe.stderr)
            cls.video_payload = video.read_bytes()
            cls.probe_document = json.loads(probe.stdout)


class CaptureFixture:
    def __init__(
        self,
        root: Path,
        video_payload: bytes,
        probe_document: dict[str, object],
    ) -> None:
        self.root = root
        self.host_payload = b"test-host"
        self.cases: dict[str, dict[str, object]] = {}
        self.root_document: dict[str, object] = {
            "schemaVersion": 1,
            "scenarioId": VERIFY.EXPECTED_SCENARIO,
            "evidenceScope": VERIFY.EVIDENCE_SCOPE,
            "captureStatus": "captured",
            "revision": "0123456789abcdef0123456789abcdef01234567",
            "workingTreeDirty": False,
            "capturedAtUtc": "2026-08-15T00:00:00.000Z",
            "host": {
                "sourcePath": "D:/fixture/ba-click-fx-desktop.exe",
                "sha256": hashlib.sha256(self.host_payload).hexdigest(),
                "demoAgeMs": 130,
                "demoDelayMs": 3000,
                "quitAfterMs": 7500,
                "rawInputRegistration": "disabled",
                "readyTimeoutMs": 5000,
                "processTimeoutMs": 15000,
            },
            "recorder": {
                "implementation": "ffmpeg-gdigrab",
                "ffmpegPath": str(FFMPEG),
                "ffmpegSha256": "1" * 64,
                "ffprobePath": str(FFPROBE),
                "ffprobeSha256": "2" * 64,
                "frameRate": FRAME_RATE,
                "durationMs": DURATION_SECONDS * 1000,
                "processTimeoutMs": 15000,
                "probeTimeoutMs": 5000,
                "videoCodec": "ffv1",
                "pixelFormat": "bgr0",
            },
            "capture": {
                "displayWidth": WIDTH,
                "displayHeight": HEIGHT,
                "left": 0,
                "top": 0,
                "width": WIDTH,
                "height": HEIGHT,
                "frameRate": FRAME_RATE,
                "durationMs": DURATION_SECONDS * 1000,
            },
            "matrix": [
                {
                    "caseId": case_id,
                    "sourceKind": source,
                    "backgroundMode": mode,
                }
                for case_id, (source, mode) in VERIFY.EXPECTED_CASES.items()
            ],
            "cases": [],
            "completedAtUtc": "2026-08-15T00:01:00.000Z",
        }
        for index, (case_id, (source, mode)) in enumerate(
            VERIFY.EXPECTED_CASES.items(), 1
        ):
            self._create_case(
                case_id,
                source,
                mode,
                1000 + index,
                video_payload,
                probe_document,
            )
        self.write_root()

    def _create_case(
        self,
        case_id: str,
        source: str,
        mode: str,
        pid: int,
        video_payload: bytes,
        probe_document: dict[str, object],
    ) -> None:
        directory = self.root / case_id
        directory.mkdir()
        gdigrab_input = "desktop" if source == "desktop" else "title=ba-click-fx-desktop"
        (directory / VERIFY.HOST_NAME).write_bytes(self.host_payload)
        (directory / VERIFY.CONFIG_NAME).write_text(
            json.dumps(
                {"schemaVersion": 7, "background": {"mode": mode}}
            ),
            encoding="utf-8",
        )
        (directory / VERIFY.LOG_NAME).write_text(
            _host_log(case_id, mode, pid), encoding="utf-8"
        )
        for name in (
            "host.stdout.log",
            "host.stderr.log",
            "ffmpeg.stdout.log",
            "ffprobe.stderr.log",
        ):
            (directory / name).write_bytes(b"")
        (directory / "ffmpeg.stderr.log").write_text(
            "fixture recorder output\n", encoding="utf-8"
        )
        (directory / VERIFY.VIDEO_NAME).write_bytes(video_payload)
        (directory / VERIFY.FFPROBE_NAME).write_text(
            json.dumps(probe_document), encoding="utf-8"
        )
        host_argv = [
            str(directory / VERIFY.HOST_NAME),
            "--demo-age-ms=130",
            "--demo-delay-ms=3000",
            "--disable-raw-input",
            "--quit-after-ms=7500",
        ]
        ffmpeg_argv = [
            str(FFMPEG),
            "-hide_banner",
            "-nostdin",
            "-y",
            "-f",
            "gdigrab",
            "-framerate",
            str(FRAME_RATE),
            "-draw_mouse",
            "0",
            "-offset_x",
            "0",
            "-offset_y",
            "0",
            "-video_size",
            f"{WIDTH}x{HEIGHT}",
            "-i",
            gdigrab_input,
            "-t",
            "6.000",
            "-an",
            "-c:v",
            "ffv1",
            "-pix_fmt",
            "bgr0",
            VERIFY.VIDEO_NAME,
        ]
        ffprobe_argv = [
            str(FFPROBE),
            "-v",
            "error",
            "-print_format",
            "json",
            "-show_format",
            "-show_streams",
            VERIFY.VIDEO_NAME,
        ]
        document: dict[str, object] = {
            "schemaVersion": 1,
            "scenarioId": VERIFY.EXPECTED_SCENARIO,
            "caseId": case_id,
            "source": {
                "kind": source,
                "gdigrabInput": gdigrab_input,
                "captureLeft": 0,
                "captureTop": 0,
                "captureWidth": WIDTH,
                "captureHeight": HEIGHT,
                "windowTitle": None if source == "desktop" else "ba-click-fx-desktop",
            },
            "backgroundMode": mode,
            "status": "captured",
            "startedAtUtc": "2026-08-15T00:00:00.000Z",
            "completedAtUtc": "2026-08-15T00:00:08.000Z",
            "commands": {
                "host": {"argv": host_argv, "display": " ".join(host_argv)},
                "ffmpeg": {"argv": ffmpeg_argv, "display": " ".join(ffmpeg_argv)},
                "ffprobe": {"argv": ffprobe_argv, "display": " ".join(ffprobe_argv)},
            },
            "processes": {
                "host": _process(pid, True),
                "ffmpeg": _process(pid + 100),
                "ffprobe": _process(pid + 200),
            },
            "cleanup": {
                "ownedHostRemaining": False,
                "ownedFfmpegRemaining": False,
                "allOwnedProcessesExited": True,
            },
            "files": {},
            "failure": None,
        }
        self.cases[case_id] = document
        self.write_case(case_id)
        self.root_document["cases"].append(
            {
                "caseId": case_id,
                "sourceKind": source,
                "backgroundMode": mode,
                "status": "captured",
                "manifest": f"{case_id}/case.json",
                "manifestSha256": _sha256(directory / "case.json"),
                "failure": None,
            }
        )

    def write_case(self, case_id: str) -> None:
        directory = self.root / case_id
        document = self.cases[case_id]
        document["files"] = {
            name: _artifact_record(directory / name)
            for name in VERIFY.EXPECTED_FILES
        }
        path = directory / "case.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        for summary in self.root_document.get("cases", []):
            if summary["caseId"] == case_id:
                summary["manifestSha256"] = _sha256(path)

    def write_root(self) -> Path:
        path = self.root / "capture.json"
        path.write_text(json.dumps(self.root_document), encoding="utf-8")
        return path

    def rewrite_artifact(self, case_id: str, name: str, payload: bytes) -> None:
        (self.root / case_id / name).write_bytes(payload)
        self.write_case(case_id)
        self.write_root()


@unittest.skipUnless(TOOLS_AVAILABLE, "ffmpeg and ffprobe are required")
class ExternalRecordingSpikeContractTests(ExternalRecordingFixture, unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.fixture = CaptureFixture(
            self.directory,
            self.video_payload,
            copy.deepcopy(self.probe_document),
        )

    def validate(self):
        return VERIFY.validate_path(
            self.directory / "capture.json",
            str(FFMPEG),
            str(FFPROBE),
            10.0,
        )

    def test_valid_four_cell_capture_is_verified(self):
        result = self.validate()
        self.assertEqual("verified-observation", result.status)
        self.assertEqual(VERIFY.EVIDENCE_SCOPE, result.evidence_scope)
        self.assertEqual(4, len(result.cases))
        for case in result.cases:
            self.assertEqual(FRAME_RATE * DURATION_SECONDS, case.video_metrics.decoded_frames)
            self.assertEqual(FRAME_RATE, case.video_metrics.baseline_frame)
            self.assertEqual(1.0, case.video_metrics.baseline_seconds)
            self.assertGreater(case.video_metrics.peak_mean_absolute_luma_delta, 0.0)
        aware = [
            case for case in result.cases if case.background_mode == "background-aware"
        ]
        self.assertTrue(all(case.wgc_resource_state == "balanced" for case in aware))

    def test_tampered_artifact_hash_is_rejected(self):
        path = self.directory / "desktop-background-aware" / VERIFY.VIDEO_NAME
        path.write_bytes(path.read_bytes() + b"tampered")
        with self.assertRaisesRegex(VERIFY.ValidationError, "bytes|sha256"):
            self.validate()

    def test_case_manifest_hash_is_rejected(self):
        summary = self.fixture.root_document["cases"][0]
        summary["manifestSha256"] = "0" * 64
        self.fixture.write_root()
        with self.assertRaisesRegex(VERIFY.ValidationError, "case.json hash"):
            self.validate()

    def test_stored_ffprobe_metadata_is_rechecked(self):
        case_id = "desktop-background-aware"
        probe = copy.deepcopy(self.probe_document)
        probe["streams"][0]["width"] = WIDTH + 2
        self.fixture.rewrite_artifact(
            case_id,
            VERIFY.FFPROBE_NAME,
            json.dumps(probe).encode("utf-8"),
        )
        with self.assertRaisesRegex(VERIFY.ValidationError, "differs at width"):
            self.validate()

    def test_background_aware_requires_participation(self):
        case_id = "desktop-background-aware"
        path = self.directory / case_id / VERIFY.LOG_NAME
        text = path.read_text(encoding="utf-8")
        blocks = [block for block in text.split("---\n") if block.strip()]
        blocks = [
            block
            for block in blocks
            if "Event.Name=BackgroundComposite.Participated" not in block
        ]
        for index, block in enumerate(blocks, 1):
            blocks[index - 1] = re_sub_sequence(block, index)
        self.fixture.rewrite_artifact(
            case_id,
            VERIFY.LOG_NAME,
            ("---\n".join(blocks) + "---\n").encode("utf-8"),
        )
        with self.assertRaisesRegex(VERIFY.ValidationError, "no background composite"):
            self.validate()

    def test_wda_observation_is_required(self):
        case_id = "desktop-recording-compatible"
        path = self.directory / case_id / VERIFY.LOG_NAME
        text = path.read_text(encoding="utf-8").replace(
            "Observed=0x00000000", "Observed=0x00000011"
        )
        self.fixture.rewrite_artifact(
            case_id, VERIFY.LOG_NAME, text.encode("utf-8")
        )
        with self.assertRaisesRegex(VERIFY.ValidationError, "WDA_NONE"):
            self.validate()

    def test_unbalanced_wgc_ledger_is_rejected(self):
        case_id = "desktop-background-aware"
        path = self.directory / case_id / VERIFY.LOG_NAME
        text = path.read_text(encoding="utf-8").replace(
            "FramesClosed=10", "FramesClosed=9"
        )
        self.fixture.rewrite_artifact(
            case_id, VERIFY.LOG_NAME, text.encode("utf-8")
        )
        with self.assertRaisesRegex(VERIFY.ValidationError, "count mismatch"):
            self.validate()

    def test_owned_process_cleanup_is_required(self):
        case_id = "desktop-background-aware"
        self.fixture.cases[case_id]["cleanup"]["ownedHostRemaining"] = True
        self.fixture.cases[case_id]["cleanup"]["allOwnedProcessesExited"] = False
        self.fixture.write_case(case_id)
        self.fixture.write_root()
        with self.assertRaisesRegex(VERIFY.ValidationError, "remained alive"):
            self.validate()

    def test_duplicate_root_field_is_rejected(self):
        path = self.directory / "capture.json"
        path.write_text(
            '{"schemaVersion":1,"schemaVersion":1}', encoding="utf-8"
        )
        with self.assertRaisesRegex(VERIFY.ValidationError, "duplicate JSON field"):
            self.validate()

    def test_cli_writes_scoped_atomic_report(self):
        report = self.directory / "verification.json"
        result = subprocess.run(
            [
                sys.executable,
                "-B",
                str(SCRIPT_PATH),
                str(self.directory / "capture.json"),
                f"--report={report}",
                f"--ffmpeg={FFMPEG}",
                f"--ffprobe={FFPROBE}",
                "--tool-timeout-seconds=10",
            ],
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("single-machine-ffmpeg-gdigrab-observation-only", result.stdout)
        document = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(VERIFY.EVIDENCE_SCOPE, document["evidence_scope"])
        self.assertEqual("verified-observation", document["status"])
        self.assertFalse(report.with_name("verification.json.tmp").exists())
        self.assertNotIn(b"\r\n", report.read_bytes())


def re_sub_sequence(block: str, sequence: int) -> str:
    lines = block.splitlines()
    return "\n".join(
        f"Event.Sequence={sequence}" if line.startswith("Event.Sequence=") else line
        for line in lines
    ) + "\n"


if __name__ == "__main__":
    raise SystemExit(unittest.main())
