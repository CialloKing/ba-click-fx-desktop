#!/usr/bin/env python3
"""Regression tests for the native FP16 Golden layer contract."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


SCRIPT_PATH = (
    Path(__file__).resolve().parents[1] / "tools" / "verify-golden-metrics.py"
)
SPEC = importlib.util.spec_from_file_location("verify_golden_metrics", SCRIPT_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"unable to load {SCRIPT_PATH}")
VERIFY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = VERIFY
SPEC.loader.exec_module(VERIFY)


def valid_metrics(**overrides):
    values = {
        "finite": True,
        "non_negative_rgb": True,
        "direct_seed_min_delta": 0.0,
        "final_direct_min_delta": 0.0,
        "final_direct_alpha_min_delta": 0.25,
        "direct_seed_alpha_min_delta": 0.50,
        "direct_rgb_sum": 4.0,
        "seed_rgb_sum": 3.0,
        "final_rgb_sum": 5.0,
        "bloom_delta_rgb_sum": 1.0,
        "up00_rgb_sum": 1.0,
        "composite_gain_ratio": 1.0,
        "down_mean_ratios": (1.0, 0.99, 1.01),
        "up_mean_monotonic": True,
    }
    values.update(overrides)
    return VERIFY.LayerMetrics(**values)


class LayerContractTests(unittest.TestCase):
    def test_alpha_layers_are_ordered_instead_of_identical(self):
        self.assertEqual((), VERIFY._layer_contract_failures(valid_metrics()))

    def test_bloom_seed_cannot_add_alpha_outside_direct_surface(self):
        failures = VERIFY._layer_contract_failures(
            valid_metrics(direct_seed_alpha_min_delta=-0.003)
        )
        self.assertTrue(any("BloomSeed Alpha exceeds" in item for item in failures))

    def test_final_overlay_cannot_erase_direct_alpha(self):
        failures = VERIFY._layer_contract_failures(
            valid_metrics(final_direct_alpha_min_delta=-0.003)
        )
        self.assertTrue(any("FinalOverlay Alpha falls below" in item for item in failures))

    def test_fp16_tolerance_boundary_remains_accepted(self):
        metrics = valid_metrics(
            direct_seed_alpha_min_delta=-VERIFY.FP16_LAYER_TOLERANCE,
            final_direct_alpha_min_delta=-VERIFY.FP16_LAYER_TOLERANCE,
        )
        self.assertEqual((), VERIFY._layer_contract_failures(metrics))


class TrailDeltaContractTests(unittest.TestCase):
    def valid_metrics(self, **overrides):
        values = {
            "energy": VERIFY.TRAIL_DELTA_REFERENCE_ENERGY,
            "coverage_pixels": VERIFY.TRAIL_DELTA_REFERENCE_COVERAGE,
            "centroid_x": VERIFY.TRAIL_DELTA_REFERENCE_CENTROID[0],
            "centroid_y": VERIFY.TRAIL_DELTA_REFERENCE_CENTROID[1],
            "chromaticity": VERIFY.TRAIL_DELTA_REFERENCE_CHROMATICITY,
            "bounds": VERIFY.TRAIL_DELTA_REFERENCE_BOUNDS,
            "negative_energy": 0,
        }
        values.update(overrides)
        return VERIFY.TrailDeltaMetrics(**values)

    def test_unity_reference_delta_passes_the_preregistered_contract(self):
        self.assertEqual((), VERIFY._trail_delta_failures(self.valid_metrics()))

    def test_additive_trail_cannot_dark_en_the_paired_frame(self):
        failures = VERIFY._trail_delta_failures(
            self.valid_metrics(negative_energy=1)
        )
        self.assertTrue(any("darker" in item for item in failures))

    def test_relative_threshold_boundary_is_accepted(self):
        energy = int(
            VERIFY.TRAIL_DELTA_REFERENCE_ENERGY
            * (1.0 + VERIFY.TRAIL_DELTA_ENERGY_RELATIVE_TOLERANCE)
        )
        self.assertEqual(
            (),
            VERIFY._trail_delta_failures(self.valid_metrics(energy=energy)),
        )

    def test_spatial_drift_outside_the_locked_bounds_is_rejected(self):
        bounds = list(VERIFY.TRAIL_DELTA_REFERENCE_BOUNDS)
        bounds[2] += VERIFY.TRAIL_DELTA_BOUNDS_TOLERANCE_PX + 1
        failures = VERIFY._trail_delta_failures(
            self.valid_metrics(bounds=tuple(bounds))
        )
        self.assertTrue(any("bounds differ" in item for item in failures))


if __name__ == "__main__":
    unittest.main()
