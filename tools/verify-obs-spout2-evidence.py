#!/usr/bin/env python3
"""Verify the bounded local OBS Spout2 recording and key frames."""

from __future__ import annotations

import argparse
from collections import deque
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from typing import BinaryIO

import numpy as np
from PIL import Image


OUTPUT_CONTRACT = "bgra8-srgb-extended-premultiplied-fx-only-v4"
MIN_RECORDING_DURATION_SECONDS = 5.0
MAX_RECORDING_DURATION_SECONDS = 10.0


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("evidence_directory", type=Path)
    parser.add_argument("video", type=Path)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    return parser.parse_args()


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _artifact(path: Path) -> dict[str, object]:
    return {
        "path": path.name,
        "bytes": path.stat().st_size,
        "sha256": _sha256(path),
    }


def _image(path: Path) -> np.ndarray:
    with Image.open(path) as source:
        return np.asarray(source.convert("RGB"), dtype=np.uint8)


def _changed_pixels(left: np.ndarray, right: np.ndarray, threshold: int) -> np.ndarray:
    difference = np.abs(left.astype(np.int16) - right.astype(np.int16))
    return np.max(difference, axis=2) > threshold


def _bounding_box(mask: np.ndarray) -> list[int]:
    rows, columns = np.nonzero(mask)
    if rows.size == 0:
        return []
    return [
        int(columns.min()),
        int(rows.min()),
        int(columns.max()) + 1,
        int(rows.max()) + 1,
    ]


def _verify_screenshots(directory: Path) -> tuple[dict[str, object], Path]:
    baseline_path = directory / "frame-baseline.png"
    if not baseline_path.exists():
        baseline_path = directory / "frame-baseline-no-sender.png"
    idle_path = directory / "frame-idle-sender-connected.png"
    final_path = directory / "frame-final-transparent.png"
    candidates = sorted(directory.glob("frame-active-*.png"))
    if not candidates:
        raise RuntimeError("no active OBS screenshot candidates were found")

    baseline = _image(baseline_path)
    idle = _image(idle_path)
    final = _image(final_path)
    if idle.shape != baseline.shape or final.shape != baseline.shape:
        raise RuntimeError("OBS screenshots have inconsistent dimensions")
    if not np.array_equal(idle, baseline):
        raise RuntimeError("sender-connected idle screenshot changed the background")
    if not np.array_equal(final, baseline):
        raise RuntimeError("final transparent screenshot did not restore the background")

    best_path: Path | None = None
    best_image: np.ndarray | None = None
    best_mask: np.ndarray | None = None
    for candidate_path in candidates:
        candidate = _image(candidate_path)
        if candidate.shape != baseline.shape:
            raise RuntimeError(f"active screenshot has the wrong dimensions: {candidate_path}")
        mask = _changed_pixels(candidate, baseline, 0)
        if best_mask is None or np.count_nonzero(mask) > np.count_nonzero(best_mask):
            best_path = candidate_path
            best_image = candidate
            best_mask = mask
    assert best_path is not None and best_image is not None and best_mask is not None

    changed_count = int(np.count_nonzero(best_mask))
    total_pixels = int(best_mask.size)
    bounds = _bounding_box(best_mask)
    black_pixels = int(np.count_nonzero(np.max(best_image, axis=2) < 8))
    if changed_count < 100:
        raise RuntimeError("active OBS screenshot contains no visible effect")
    if changed_count >= total_pixels // 4:
        raise RuntimeError("active OBS screenshot changed too much of the background")
    if black_pixels != 0:
        raise RuntimeError("active OBS screenshot contains black pixels")
    if bounds and (bounds[0] == 0 or bounds[1] == 0
                   or bounds[2] == baseline.shape[1]
                   or bounds[3] == baseline.shape[0]):
        raise RuntimeError("active OBS effect reaches a frame edge")

    return (
        {
            "dimensions": [int(baseline.shape[1]), int(baseline.shape[0])],
            "backgroundRgb": [int(value) for value in baseline[0, 0]],
            "idleEqualsBaseline": True,
            "finalEqualsBaseline": True,
            "activeFrame": best_path.name,
            "activeChangedPixels": changed_count,
            "activeChangedFraction": changed_count / total_pixels,
            "activeBounds": bounds,
            "activeBlackPixels": black_pixels,
            "artifacts": [
                _artifact(baseline_path),
                _artifact(idle_path),
                _artifact(best_path),
                _artifact(final_path),
            ],
        },
        best_path,
    )


def _probe_video(ffprobe: str, video: Path) -> dict[str, object]:
    completed = subprocess.run(
        [
            ffprobe,
            "-v",
            "error",
            "-show_entries",
            "format=duration,size,format_name:stream=index,codec_name,codec_type,width,height,pix_fmt,r_frame_rate,nb_frames",
            "-of",
            "json",
            str(video),
        ],
        check=True,
        capture_output=True,
        text=True,
        timeout=15,
        encoding="utf-8",
        errors="replace",
    )
    return json.loads(completed.stdout)


def _require_video_only(probe: dict[str, object]) -> dict[str, object]:
    streams = probe.get("streams", [])
    if not isinstance(streams, list):
        raise RuntimeError("OBS recording streams metadata is invalid")
    audio_streams = [
        stream for stream in streams
        if isinstance(stream, dict) and stream.get("codec_type") == "audio"
    ]
    if audio_streams:
        # Retaining audio is unnecessary for a visual gate and can capture user data.
        raise RuntimeError("OBS recording must not contain audio streams")
    video_streams = [
        stream for stream in streams
        if isinstance(stream, dict) and stream.get("codec_type") == "video"
    ]
    if len(video_streams) != 1:
        raise RuntimeError("OBS recording must contain exactly one video stream")
    return video_streams[0]


def _read_frame(stream: BinaryIO, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = stream.read(size - len(chunks))
        if not chunk:
            break
        chunks.extend(chunk)
    return bytes(chunks)


def _verify_video(
    directory: Path,
    video: Path,
    ffmpeg: str,
    ffprobe: str,
) -> dict[str, object]:
    probe = _probe_video(ffprobe, video)
    stream = _require_video_only(probe)
    width = int(stream["width"])
    height = int(stream["height"])
    duration = float(probe["format"]["duration"])
    frame_rate_parts = str(stream.get("r_frame_rate", "0/0")).split("/", 1)
    if len(frame_rate_parts) != 2 or int(frame_rate_parts[1]) == 0:
        raise RuntimeError("OBS recording has an invalid frame rate")
    frame_rate = int(frame_rate_parts[0]) / int(frame_rate_parts[1])
    if frame_rate <= 0:
        raise RuntimeError("OBS recording has an invalid frame rate")
    if (width, height) != (1280, 720):
        raise RuntimeError("OBS recording is not 1280x720")
    if (duration < MIN_RECORDING_DURATION_SECONDS
            or duration > MAX_RECORDING_DURATION_SECONDS):
        raise RuntimeError("OBS recording duration is outside the bounded gate")

    command = [
        ffmpeg,
        "-v",
        "error",
        "-i",
        str(video),
        "-map",
        "0:v:0",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgb24",
        "-",
    ]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert process.stdout is not None
    frame_bytes = width * height * 3
    initial_frames: list[np.ndarray] = []
    tail: deque[tuple[int, np.ndarray]] = deque(maxlen=30)
    reference: np.ndarray | None = None
    active_frame: np.ndarray | None = None
    active_index = -1
    active_changed = -1
    active_mask: np.ndarray | None = None
    frame_index = 0
    while True:
        encoded = _read_frame(process.stdout, frame_bytes)
        if not encoded:
            break
        if len(encoded) != frame_bytes:
            process.kill()
            raise RuntimeError("ffmpeg returned a partial video frame")
        frame = np.frombuffer(encoded, dtype=np.uint8).reshape((height, width, 3)).copy()
        if frame_index < 20:
            initial_frames.append(frame)
            if frame_index == 19:
                reference = np.median(
                    np.stack(initial_frames[5:20]).astype(np.int16),
                    axis=0,
                ).astype(np.uint8)
        if reference is not None:
            mask = _changed_pixels(frame, reference, 6)
            changed = int(np.count_nonzero(mask))
            if changed > active_changed:
                active_changed = changed
                active_index = frame_index
                active_frame = frame
                active_mask = mask
            tail.append((changed, frame))
        frame_index += 1

    _, stderr = process.communicate(timeout=10)
    if process.returncode != 0:
        raise RuntimeError(
            "ffmpeg could not decode the OBS recording: "
            + stderr.decode("utf-8", errors="replace")
        )
    if reference is None or active_frame is None or active_mask is None or not tail:
        raise RuntimeError("OBS recording did not contain enough video frames")

    initial_spread = max(
        int(np.count_nonzero(_changed_pixels(frame, reference, 6)))
        for frame in initial_frames
    )
    final_changed = max(changed for changed, _ in tail)
    black_pixels = int(np.count_nonzero(np.max(active_frame, axis=2) < 8))
    active_bounds = _bounding_box(active_mask)
    if initial_spread != 0:
        raise RuntimeError("OBS recording baseline is not stable")
    if active_changed < 1_000:
        raise RuntimeError("OBS recording contains no visible effect frames")
    if final_changed != 0:
        raise RuntimeError("OBS recording did not return to its background")
    if black_pixels != 0:
        raise RuntimeError("OBS recording active frame contains black pixels")
    if active_index <= 30 or active_index >= frame_index - 30:
        raise RuntimeError("OBS recording active frame is outside the middle phase")

    idle_path = directory / "recording-frame-idle.png"
    active_path = directory / "recording-frame-active.png"
    final_path = directory / "recording-frame-final.png"
    Image.fromarray(initial_frames[15], mode="RGB").save(idle_path)
    Image.fromarray(active_frame, mode="RGB").save(active_path)
    Image.fromarray(tail[-1][1], mode="RGB").save(final_path)

    return {
        "artifact": _artifact(video),
        "durationSeconds": duration,
        "dimensions": [width, height],
        "codec": stream.get("codec_name"),
        "pixelFormat": stream.get("pix_fmt"),
        "frameRate": stream.get("r_frame_rate"),
        "audioStreamCount": 0,
        "decodedFrames": frame_index,
        "baselineMaximumChangedPixels": initial_spread,
        "activeFrameIndex": active_index,
        "activeTimeSeconds": active_index / frame_rate,
        "activeChangedPixels": active_changed,
        "activeBounds": active_bounds,
        "activeBlackPixels": black_pixels,
        "finalMaximumChangedPixels": final_changed,
        "keyFrames": [
            _artifact(idle_path),
            _artifact(active_path),
            _artifact(final_path),
        ],
        "ffprobe": probe,
    }


def main() -> int:
    options = _arguments()
    directory = options.evidence_directory.resolve()
    video = options.video.resolve()
    report_path = directory / "verification.json"
    if report_path.exists():
        raise RuntimeError(f"verification report already exists: {report_path}")
    if video.parent != directory:
        raise RuntimeError("recording must be inside the evidence directory")

    screenshot_report, _ = _verify_screenshots(directory)
    video_report = _verify_video(
        directory,
        video,
        options.ffmpeg,
        options.ffprobe,
    )
    report = {
        "schema": 1,
        "status": "pass",
        "contract": OUTPUT_CONTRACT,
        "screenshots": screenshot_report,
        "recording": video_report,
        "notRun": [
            "HDR",
            "multiple-displays",
            "cross-GPU",
            "Windows-11",
            "other-OBS-or-plugin-versions",
            "reproducible-Spout2-CI-build",
        ],
    }
    report_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"OBS Spout2 evidence verified: {report_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"OBS Spout2 evidence verification failed: {error}", file=sys.stderr)
        raise SystemExit(1)
