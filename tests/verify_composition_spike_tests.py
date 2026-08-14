#!/usr/bin/env python3
"""Contract tests for the SPK-001 composition verifier."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT_PATH = (
    Path(__file__).resolve().parents[1] / "tools" / "verify-composition-spike.py"
)
SPEC = importlib.util.spec_from_file_location("verify_composition_spike", SCRIPT_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"unable to load {SCRIPT_PATH}")
VERIFY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = VERIFY
SPEC.loader.exec_module(VERIFY)


def pixel(values):
    return dict(zip(VERIFY.CHANNELS, values, strict=True))


def presentation(name, source, result, generation_start, diagnostic_srgb):
    attempts = []
    for offset in range(2):
        marker = 1_000_000 + generation_start * 100 + offset * 10
        attempts.append(
            {
                "presentMarkerNs": marker,
                "prePresentPixel": pixel(source),
                "desktopGdiDiagnosticSrgb8": list(diagnostic_srgb),
                "sample": {
                    "generation": generation_start + offset,
                    "capturedAtNs": marker + 5,
                    "contentSize": {"width": 3840, "height": 2160},
                    "pixel": pixel((*result, 1.0)),
                },
            }
        )
    return {
        "name": name,
        "requestedSource": pixel(source),
        "attempts": attempts,
        "stablePair": [0, 1],
    }


def valid_capture():
    generation = 1
    backgrounds = []
    baseline_values = {
        "black": (0.0, 0.0, 0.0),
        "gray-18-percent": (0.37, 0.37, 0.37),
        "color": (0.07, 0.38, 1.32),
        "white": (2.0, 2.0, 2.0),
    }
    for background_name, srgb in VERIFY.BACKGROUND_DEFINITIONS:
        baseline = baseline_values[background_name]
        baseline_capture = presentation(
            "baseline", (0.0, 0.0, 0.0, 0.0), baseline, generation, srgb
        )
        generation += 2
        sources = []
        for source_name, source in VERIFY.SOURCE_DEFINITIONS:
            result = tuple(
                source[index] + (1.0 - source[3]) * baseline[index]
                for index in range(3)
            )
            sources.append(
                presentation(source_name, source, result, generation, srgb)
            )
            generation += 2
        backgrounds.append(
            {
                "name": background_name,
                "srgb8": list(srgb),
                "baseline": baseline_capture,
                "sources": sources,
            }
        )
    return {
        "schemaVersion": 1,
        "spikeId": "SPK-001",
        "applicationVersion": "0.1.0-test",
        "revision": "test",
        "capturedAtUtc": "2026-08-14T00:00:00.000Z",
        "timeoutMs": 20000,
        "contract": {
            "surfaceFormat": "DXGI_FORMAT_R16G16B16A16_FLOAT",
            "swapChainAlphaMode": "premultiplied",
            "swapChainColorSpace": "rgb-full-g10-p709",
            "observerFormat": "DXGI_FORMAT_R16G16B16A16_FLOAT",
            "observerExcludesOwnOverlay": False,
            "cursorExcluded": True,
            "systemBorderAllowed": True,
            "sourceInjection": "ClearRenderTargetView-production-swap-chain",
            "desktopGdiDiagnosticSemantic": "diagnostic-only-unsynchronized",
            "nonFiniteJsonEncoding": "null",
            "formula": "C=S.rgb+(1-S.a)*B",
            "stableSampleTolerance": 0.01,
        },
        "osVersion": {"major": 10, "minor": 0, "build": 19045},
        "monitorBounds": {"left": 0, "top": 0, "right": 3840, "bottom": 2160},
        "probeBounds": {"left": 1792, "top": 952, "right": 2048, "bottom": 1208},
        "rendererDevice": {
            "driverType": "hardware",
            "adapter": "Test GPU",
            "adapterLuid": {"low": 1, "high": 0},
            "vendorId": 1,
            "deviceId": 2,
            "driverVersion": 3,
            "featureLevel": 45312,
        },
        "observerFeatureLevel": 45312,
        "display": {
            "colorSpace": 0,
            "colorSpaceName": "rgb-full-g22-p709",
            "bitsPerColor": 8,
            "minimumLuminanceNits": 0.5,
            "maximumLuminanceNits": 270.0,
            "maximumFullFrameLuminanceNits": 270.0,
            "luminanceMetadataValid": True,
        },
        "captureAffinity": {"requested": 0, "observed": 0, "confirmed": True},
        "wgcCapabilities": {"borderHidden": False, "cursorExcluded": True},
        "backgrounds": backgrounds,
    }


class CompositionSpikeContractTests(unittest.TestCase):
    def test_valid_matrix_is_accepted(self):
        result = VERIFY.validate_capture(valid_capture())
        self.assertEqual("sdr-accepted", result.status)
        self.assertEqual("sdr", result.matrix_cell)
        self.assertEqual(4, result.backgrounds)
        self.assertEqual(20, result.presentations)
        self.assertEqual(48, result.formula_channel_checks)
        self.assertEqual((), result.degradations)

    def test_observer_must_include_overlay(self):
        capture = valid_capture()
        capture["contract"]["observerExcludesOwnOverlay"] = True
        with self.assertRaisesRegex(VERIFY.ValidationError, "include the probe overlay"):
            VERIFY.validate_capture(capture)

    def test_stable_pair_is_required(self):
        capture = valid_capture()
        capture["backgrounds"][0]["baseline"]["stablePair"] = None
        with self.assertRaisesRegex(VERIFY.ValidationError, "stablePair"):
            VERIFY.validate_capture(capture)

    def test_samples_must_be_stable(self):
        capture = valid_capture()
        capture["backgrounds"][0]["baseline"]["attempts"][1]["sample"]["pixel"]["r"] = 0.1
        with self.assertRaisesRegex(VERIFY.ValidationError, "stability tolerance"):
            VERIFY.validate_capture(capture)

    def test_baseline_gdi_diagnostic_must_match_controlled_background(self):
        capture = valid_capture()
        diagnostic = capture["backgrounds"][1]["baseline"]["attempts"][0][
            "desktopGdiDiagnosticSrgb8"
        ]
        diagnostic[0] = 0
        with self.assertRaisesRegex(
            VERIFY.ValidationError, "controlled background"
        ):
            VERIFY.validate_capture(capture)

    def test_generation_must_increase_globally(self):
        capture = valid_capture()
        capture["backgrounds"][0]["sources"][0]["attempts"][0]["sample"]["generation"] = 2
        with self.assertRaisesRegex(VERIFY.ValidationError, "generations"):
            VERIFY.validate_capture(capture)

    def test_sample_must_be_newer_than_present(self):
        capture = valid_capture()
        attempt = capture["backgrounds"][0]["baseline"]["attempts"][0]
        attempt["sample"]["capturedAtNs"] = attempt["presentMarkerNs"]
        with self.assertRaisesRegex(VERIFY.ValidationError, "newer than Present"):
            VERIFY.validate_capture(capture)

    def test_pre_present_readback_must_match_requested_source(self):
        capture = valid_capture()
        capture["backgrounds"][0]["sources"][1]["attempts"][0]["prePresentPixel"]["r"] = 0.0
        with self.assertRaisesRegex(VERIFY.ValidationError, "prePresentPixel.r drifted"):
            VERIFY.validate_capture(capture)

    def test_additive_a_zero_energy_cannot_disappear(self):
        capture = valid_capture()
        source = capture["backgrounds"][0]["sources"][1]
        for attempt in source["attempts"]:
            attempt["sample"]["pixel"].update({"r": 0.0, "g": 0.0, "b": 0.0})
        with self.assertRaisesRegex(VERIFY.ValidationError, "violates source-over"):
            VERIFY.validate_capture(capture)

    def test_extended_premultiplied_energy_cannot_be_canonicalized(self):
        capture = valid_capture()
        source = capture["backgrounds"][0]["sources"][2]
        for attempt in source["attempts"]:
            attempt["sample"]["pixel"].update({"r": 0.25, "g": 0.25, "b": 0.25})
        with self.assertRaisesRegex(VERIFY.ValidationError, "violates source-over"):
            VERIFY.validate_capture(capture)

    def test_high_energy_saturation_is_reported_as_sdr_degradation(self):
        capture = valid_capture()
        source = capture["backgrounds"][0]["sources"][3]
        for attempt in source["attempts"]:
            attempt["sample"]["pixel"].update({"r": 1.0, "g": 1.0, "b": 1.0})
        result = VERIFY.validate_capture(capture)
        self.assertEqual("sdr-accepted-with-degradation", result.status)
        self.assertEqual(3, len(result.degradations))

    def test_white_background_saturation_is_reported_as_degradation(self):
        capture = valid_capture()
        white = capture["backgrounds"][3]
        source = white["sources"][1]
        for attempt in source["attempts"]:
            attempt["sample"]["pixel"].update({"r": 2.0, "g": 2.0, "b": 2.0})
        result = VERIFY.validate_capture(capture)
        self.assertEqual("sdr-accepted-with-degradation", result.status)
        self.assertEqual(3, len(result.degradations))

    def test_null_non_finite_encoding_is_rejected_as_a_number(self):
        capture = valid_capture()
        capture["backgrounds"][0]["baseline"]["attempts"][0]["sample"]["pixel"]["r"] = None
        with self.assertRaisesRegex(VERIFY.ValidationError, "finite number"):
            VERIFY.validate_capture(capture)

    def test_hdr_capture_is_not_mislabeled_as_sdr_evidence(self):
        capture = valid_capture()
        capture["display"]["colorSpaceName"] = "rgb-full-pq-p2020"
        with self.assertRaisesRegex(VERIFY.ValidationError, "SDR verification requires"):
            VERIFY.validate_capture(capture)

    def test_duplicate_json_fields_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.json"
            path.write_text(
                '{"schemaVersion":1,"schemaVersion":1}', encoding="utf-8"
            )
            with self.assertRaisesRegex(VERIFY.ValidationError, "duplicate JSON"):
                VERIFY.validate_path(path)

    def test_cli_writes_a_structured_report(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture_path = root / "capture.json"
            report_path = root / "verification.json"
            capture_path.write_text(json.dumps(valid_capture()), encoding="utf-8")
            result = subprocess.run(
                [
                    sys.executable,
                    "-B",
                    str(SCRIPT_PATH),
                    str(capture_path),
                    f"--report={report_path}",
                ],
                capture_output=True,
                text=True,
                timeout=5,
                check=False,
            )
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertNotIn(b"\r\n", report_path.read_bytes())
            report = json.loads(report_path.read_text(encoding="utf-8"))
            self.assertEqual("sdr-accepted", report["status"])
            self.assertIn("PASS: SPK-001", result.stdout)


if __name__ == "__main__":
    raise SystemExit(unittest.main())
