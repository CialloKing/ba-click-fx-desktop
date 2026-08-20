#!/usr/bin/env python3
"""Contract tests for the bounded OBS Spout2 lifecycle verifier."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


if len(sys.argv) < 2:
    raise RuntimeError("expected verifier path")

VERIFIER = Path(sys.argv.pop(1))
SPEC = importlib.util.spec_from_file_location("verify_obs_spout2_evidence", VERIFIER)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"could not load verifier: {VERIFIER}")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def _video_stream() -> dict[str, object]:
    return {
        "index": 0,
        "codec_name": "h264",
        "codec_type": "video",
        "width": 1280,
        "height": 720,
        "pix_fmt": "yuv420p",
        "r_frame_rate": "30/1",
    }


class VerifyObsSpout2EvidenceTests(unittest.TestCase):
    def test_accepts_one_video_stream_without_audio(self) -> None:
        stream = MODULE._require_video_only({"streams": [_video_stream()]})

        self.assertEqual(stream["codec_type"], "video")

    def test_rejects_any_audio_stream(self) -> None:
        probe = {
            "streams": [
                _video_stream(),
                {"index": 1, "codec_name": "aac", "codec_type": "audio"},
            ]
        }

        with self.assertRaisesRegex(RuntimeError, "must not contain audio"):
            MODULE._require_video_only(probe)

    def test_keeps_v2_contract_and_bounded_duration(self) -> None:
        self.assertEqual(
            MODULE.OUTPUT_CONTRACT,
            "bgra8-srgb-extended-premultiplied-fx-only-v4",
        )
        self.assertEqual(MODULE.MIN_RECORDING_DURATION_SECONDS, 5.0)
        self.assertEqual(MODULE.MAX_RECORDING_DURATION_SECONDS, 10.0)


if __name__ == "__main__":
    unittest.main()
