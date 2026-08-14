#!/usr/bin/env python3
"""Regression tests for the native FP16 Golden layer contract."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest
from unittest import mock


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
        "bloom_result_rgb_sum": 1.0,
        "up00_rgb_sum": 1.0,
        "composite_rgb_max_absolute_error": 0.001,
        "composite_rgb_mean_absolute_error": 0.0001,
        "composite_rgb_max_tolerance_ratio": 0.5,
        "composite_rgb_first_failure": None,
        "composite_alpha_max_absolute_error": 0.0,
        "composite_alpha_first_failure": None,
        "down_mean_ratios": (1.0, 0.99, 1.01),
        "up_mean_monotonic": True,
    }
    values.update(overrides)
    return VERIFY.LayerMetrics(**values)


class LayerContractTests(unittest.TestCase):
    @staticmethod
    def measured_metrics(final_overlay):
        layers = {
            "DirectSurface": (0.25, 0.5, 0.75, 0.25),
            "BloomSeed": (0.0, 0.0, 0.0, 0.0),
            "BloomResult": (0.125, 0.25, 0.125, 0.75),
            "FinalOverlay": final_overlay,
            "Prefilter_Down00": (0.0, 0.0, 0.0, 0.0),
            "Up00": (0.0, 0.0, 0.0, 0.0),
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, pixel in layers.items():
                (root / f"{name}.rgba16f").write_bytes(
                    struct.pack("<4e", *pixel)
                )
            layer_info = {
                name: {"width": 1, "height": 1}
                for name in layers
            }
            return VERIFY._layer_metrics(root, layer_info)

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

    def test_composite_rgb_reports_the_first_formula_failure(self):
        failures = VERIFY._layer_contract_failures(
            valid_metrics(
                composite_rgb_max_absolute_error=0.01,
                composite_rgb_mean_absolute_error=0.001,
                composite_rgb_max_tolerance_ratio=2.0,
                composite_rgb_first_failure=(12, 34, 2),
            )
        )
        self.assertTrue(any("DirectSurface + BloomResult" in item for item in failures))
        self.assertTrue(any("(12, 34, 2)" in item for item in failures))

    def test_composite_alpha_reports_the_first_formula_failure(self):
        failures = VERIFY._layer_contract_failures(
            valid_metrics(
                composite_alpha_max_absolute_error=0.01,
                composite_alpha_first_failure=(56, 78),
            )
        )
        self.assertTrue(any("max(DirectSurface, BloomResult)" in item for item in failures))
        self.assertTrue(any("(56, 78)" in item for item in failures))

    def test_measured_composite_rgb_failure_reports_pixel_and_channel(self):
        metrics = self.measured_metrics((0.39, 0.75, 0.875, 0.75))
        self.assertEqual((0, 0, 0), metrics.composite_rgb_first_failure)
        self.assertTrue(
            any(
                "first=(0, 0, 0)" in item
                for item in VERIFY._layer_contract_failures(metrics)
            )
        )

    def test_measured_composite_alpha_failure_reports_pixel(self):
        metrics = self.measured_metrics((0.375, 0.75, 0.875, 0.5))
        self.assertEqual((0, 0), metrics.composite_alpha_first_failure)
        self.assertTrue(
            any(
                "first=(0, 0)" in item
                for item in VERIFY._layer_contract_failures(metrics)
            )
        )


class UnityParticleFixturePixelTests(unittest.TestCase):
    def test_pixel_difference_counts_per_pixel_thresholds(self):
        expected = VERIFY.Image8(2, 1, bytes(6))
        actual = VERIFY.Image8(2, 1, bytes((1, 2, 3, 0, 0, 0)))

        metrics = VERIFY._pixel_difference_metrics(actual, expected)

        self.assertEqual(3, metrics.maximum_channel_absolute_error)
        self.assertEqual(1.0, metrics.mean_channel_absolute_error)
        self.assertEqual(1, metrics.pixels_over_one)
        self.assertEqual(1, metrics.pixels_over_two)

    def test_fixture_pixel_threshold_boundaries_are_locked(self):
        boundary = VERIFY.PixelDifferenceMetrics(
            maximum_channel_absolute_error=16,
            mean_channel_absolute_error=0.01,
            pixels_over_one=128,
            pixels_over_two=64,
        )
        self.assertTrue(VERIFY._fixture_pixels_pass(boundary))

        invalid = (
            VERIFY.PixelDifferenceMetrics(17, 0.01, 128, 64),
            VERIFY.PixelDifferenceMetrics(16, 0.010001, 128, 64),
            VERIFY.PixelDifferenceMetrics(16, 0.01, 129, 64),
            VERIFY.PixelDifferenceMetrics(16, 0.01, 128, 65),
        )
        for metrics in invalid:
            with self.subTest(metrics=metrics):
                self.assertFalse(VERIFY._fixture_pixels_pass(metrics))

    def test_fixture_aggregate_tolerance_is_stricter_than_random_stream(self):
        fixture = VERIFY._unity_particle_fixture_tolerance()
        random_stream = VERIFY._tolerance(50)

        self.assertEqual(0.02, fixture.energy_relative)
        self.assertEqual(0.02, fixture.coverage_relative)
        self.assertLess(fixture.centroid_distance_px, random_stream.centroid_distance_px)
        self.assertLess(fixture.radial_histogram_l1, random_stream.radial_histogram_l1)
        self.assertLess(fixture.chromaticity_l1, random_stream.chromaticity_l1)


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

    def test_unity_reference_delta_passes_the_recorded_contract(self):
        self.assertEqual((), VERIFY._trail_delta_failures(self.valid_metrics()))

    def test_interaction_reference_uses_the_locked_high_signal_region(self):
        self.assertEqual(24, VERIFY.TRAIL_SIGNAL_THRESHOLD)
        self.assertEqual(40_598, VERIFY.TRAIL_DELTA_REFERENCE_COVERAGE)
        self.assertEqual((894, 460, 1244, 637), VERIFY.TRAIL_DELTA_REFERENCE_BOUNDS)

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

    def test_paired_pixels_measure_positive_and_negative_energy(self):
        with_trail = VERIFY.Image8(
            2,
            1,
            bytes((0, 5, 9, 2, 3, 4)),
        )
        no_trail = VERIFY.Image8(
            2,
            1,
            bytes((0, 1, 2, 3, 3, 4)),
        )
        metrics = VERIFY._trail_delta_metrics(with_trail, no_trail)
        self.assertEqual(11, metrics.energy)
        self.assertEqual(1, metrics.negative_energy)
        self.assertEqual(1, metrics.coverage_pixels)
        self.assertEqual((0, 0, 0, 0), metrics.bounds)

    def test_paired_images_must_have_the_same_dimensions(self):
        with self.assertRaises(VERIFY.ValidationError):
            VERIFY._trail_delta_metrics(
                VERIFY.Image8(1, 1, bytes((1, 1, 1))),
                VERIFY.Image8(2, 1, bytes((1, 1, 1, 1, 1, 1))),
            )

    def test_signal_threshold_changes_only_coverage_and_bounds(self):
        with_trail = VERIFY.Image8(
            3,
            1,
            bytes((3, 0, 0, 25, 0, 0, 24, 0, 0)),
        )
        no_trail = VERIFY.Image8(3, 1, bytes(9))

        weak_tail = VERIFY._trail_delta_metrics(with_trail, no_trail)
        high_signal = VERIFY._trail_delta_metrics(
            with_trail,
            no_trail,
            signal_threshold=24,
        )

        self.assertEqual(3, weak_tail.coverage_pixels)
        self.assertEqual((0, 0, 2, 0), weak_tail.bounds)
        self.assertEqual(1, high_signal.coverage_pixels)
        self.assertEqual((1, 0, 1, 0), high_signal.bounds)
        self.assertEqual(weak_tail.energy, high_signal.energy)
        self.assertEqual(weak_tail.centroid_x, high_signal.centroid_x)
        self.assertEqual(weak_tail.chromaticity, high_signal.chromaticity)

        masked_signal = VERIFY._trail_delta_metrics(
            with_trail,
            no_trail,
            signal_threshold=24,
            signal_only=True,
        )
        self.assertEqual(25, masked_signal.energy)
        self.assertEqual(1.0, masked_signal.centroid_x)
        self.assertEqual((1.0, 0.0, 0.0), masked_signal.chromaticity)

    def test_signal_threshold_requires_a_non_negative_plain_integer(self):
        image = VERIFY.Image8(1, 1, bytes((3, 0, 0)))
        empty = VERIFY.Image8(1, 1, bytes(3))
        for invalid in (-1, True, 2.0, "2", None):
            with self.subTest(value=invalid):
                with self.assertRaises(VERIFY.ValidationError):
                    VERIFY._trail_delta_metrics(
                        image,
                        empty,
                        signal_threshold=invalid,
                    )


class TrailOnlyContractTests(unittest.TestCase):
    def valid_metrics(self, **overrides):
        values = {
            "energy": VERIFY.TRAIL_ONLY_REFERENCE_ENERGY,
            "coverage_pixels": VERIFY.TRAIL_ONLY_REFERENCE_COVERAGE,
            "centroid_x": VERIFY.TRAIL_ONLY_REFERENCE_CENTROID[0],
            "centroid_y": VERIFY.TRAIL_ONLY_REFERENCE_CENTROID[1],
            "chromaticity": VERIFY.TRAIL_ONLY_REFERENCE_CHROMATICITY,
            "bounds": VERIFY.TRAIL_ONLY_REFERENCE_BOUNDS,
            "negative_energy": 0,
        }
        values.update(overrides)
        return VERIFY.TrailDeltaMetrics(**values)

    def test_unity_trail_only_reference_is_locked(self):
        self.assertEqual(588_979, VERIFY.TRAIL_ONLY_REFERENCE_ENERGY)
        self.assertEqual(25_711, VERIFY.TRAIL_ONLY_REFERENCE_COVERAGE)
        self.assertEqual((977.2882, 547.8723), VERIFY.TRAIL_ONLY_REFERENCE_CENTROID)
        self.assertEqual(
            (0.0, 0.323896, 0.676104),
            VERIFY.TRAIL_ONLY_REFERENCE_CHROMATICITY,
        )
        self.assertEqual((895, 457, 1054, 639), VERIFY.TRAIL_ONLY_REFERENCE_BOUNDS)
        self.assertEqual((), VERIFY._trail_only_failures(self.valid_metrics()))

    def test_trail_only_rejects_darkening(self):
        failures = VERIFY._trail_only_failures(
            self.valid_metrics(negative_energy=1)
        )
        self.assertTrue(any("negative energy" in item for item in failures))

    def test_trail_only_accepts_each_exact_tolerance_boundary(self):
        reference_energy = VERIFY.TRAIL_ONLY_REFERENCE_ENERGY
        reference_coverage = VERIFY.TRAIL_ONLY_REFERENCE_COVERAGE
        centroid_x, centroid_y = VERIFY.TRAIL_ONLY_REFERENCE_CENTROID
        red, green, blue = VERIFY.TRAIL_ONLY_REFERENCE_CHROMATICITY
        minimum_x, minimum_y, maximum_x, maximum_y = (
            VERIFY.TRAIL_ONLY_REFERENCE_BOUNDS
        )
        boundary_metrics = (
            self.valid_metrics(
                energy=int(
                    reference_energy
                    * (1.0 + VERIFY.TRAIL_ONLY_ENERGY_RELATIVE_TOLERANCE)
                )
            ),
            self.valid_metrics(
                coverage_pixels=int(
                    reference_coverage
                    * (1.0 + VERIFY.TRAIL_ONLY_COVERAGE_RELATIVE_TOLERANCE)
                )
            ),
            self.valid_metrics(
                centroid_x=centroid_x + VERIFY.TRAIL_ONLY_CENTROID_TOLERANCE_PX,
                centroid_y=centroid_y - VERIFY.TRAIL_ONLY_CENTROID_TOLERANCE_PX,
            ),
            self.valid_metrics(
                chromaticity=(
                    red + VERIFY.TRAIL_ONLY_CHROMATICITY_L1_TOLERANCE,
                    green,
                    blue,
                )
            ),
            self.valid_metrics(
                bounds=(
                    minimum_x - VERIFY.TRAIL_ONLY_BOUNDS_TOLERANCE_PX,
                    minimum_y,
                    maximum_x + VERIFY.TRAIL_ONLY_BOUNDS_TOLERANCE_PX,
                    maximum_y,
                )
            ),
        )
        for metrics in boundary_metrics:
            with self.subTest(metrics=metrics):
                self.assertEqual((), VERIFY._trail_only_failures(metrics))

    def test_trail_only_rejects_each_tolerance_just_outside_the_boundary(self):
        reference_energy = VERIFY.TRAIL_ONLY_REFERENCE_ENERGY
        reference_coverage = VERIFY.TRAIL_ONLY_REFERENCE_COVERAGE
        centroid_x, centroid_y = VERIFY.TRAIL_ONLY_REFERENCE_CENTROID
        red, green, blue = VERIFY.TRAIL_ONLY_REFERENCE_CHROMATICITY
        minimum_x, minimum_y, maximum_x, maximum_y = (
            VERIFY.TRAIL_ONLY_REFERENCE_BOUNDS
        )
        invalid_metrics = (
            self.valid_metrics(
                energy=(
                    int(
                        reference_energy
                        * (1.0 + VERIFY.TRAIL_ONLY_ENERGY_RELATIVE_TOLERANCE)
                    )
                    + 1
                )
            ),
            self.valid_metrics(
                coverage_pixels=(
                    int(
                        reference_coverage
                        * (1.0 + VERIFY.TRAIL_ONLY_COVERAGE_RELATIVE_TOLERANCE)
                    )
                    + 1
                )
            ),
            self.valid_metrics(
                centroid_x=(
                    centroid_x + VERIFY.TRAIL_ONLY_CENTROID_TOLERANCE_PX + 1.0e-6
                )
            ),
            self.valid_metrics(
                chromaticity=(
                    red + VERIFY.TRAIL_ONLY_CHROMATICITY_L1_TOLERANCE + 1.0e-6,
                    green,
                    blue,
                )
            ),
            self.valid_metrics(
                bounds=(
                    minimum_x,
                    minimum_y,
                    maximum_x + VERIFY.TRAIL_ONLY_BOUNDS_TOLERANCE_PX + 1,
                    maximum_y,
                )
            ),
        )
        for metrics in invalid_metrics:
            with self.subTest(metrics=metrics):
                self.assertNotEqual((), VERIFY._trail_only_failures(metrics))

    def test_trail_only_rejects_spatial_drift_beyond_the_boundary(self):
        bounds = list(VERIFY.TRAIL_ONLY_REFERENCE_BOUNDS)
        bounds[0] -= VERIFY.TRAIL_ONLY_BOUNDS_TOLERANCE_PX + 1
        failures = VERIFY._trail_only_failures(
            self.valid_metrics(bounds=tuple(bounds))
        )
        self.assertTrue(any("bounds differ" in item for item in failures))


class ManifestCaseTests(unittest.TestCase):
    def manifest(self, ages):
        return {
            "schemaVersion": 3,
            "driver": "WARP",
            "captureProfile": "fx-only",
            "compositeFormula": "direct-plus-bloom-result-max-alpha-v1",
            "seed": 20260716,
            "allLayers": False,
            "viewport": {"width": 1950, "height": 1097},
            "rowOrigin": "top-left",
            "bloom": {
                "mipCount": 6,
                "sampleScale": VERIFY.SAMPLE_SCALE,
                "exposureGain": VERIFY.EXPOSURE_GAIN,
            },
            "ages": [
                {
                    "ageMs": age,
                    "layers": [self.comparison_frame("FinalOverlay")],
                }
                for age in ages
            ],
        }

    def drag_manifest(self):
        value = self.manifest((140,))
        value["case"] = {
            "name": "drag-trail",
            "contractVersion": 1,
            "movementPixels": 432,
            "movementSteps": 12,
            "trailOnlyPixels": 20,
            "trailFixture": "two-endpoint-unity-editor-diagnostic",
            "unityReference": {
                "withTrail": VERIFY.DRAG_WITH_TRAIL_REFERENCE,
                "noTrail": VERIFY.DRAG_NO_TRAIL_REFERENCE,
                "trailOnly": VERIFY.TRAIL_ONLY_REFERENCE,
            },
        }
        value["ages"][0]["comparisonFrames"] = [
            self.comparison_frame("FinalOverlay_NoTrail"),
            self.comparison_frame("FinalOverlay_TrailOnly20px"),
        ]
        return value

    def fixture_manifest(self):
        value = self.manifest((50,))
        value["case"] = {
            "name": "unity-particle-fixture",
            "contractVersion": 1,
            "scope": "capture-only-observation",
            "sourceFixture": VERIFY.UNITY_PARTICLE_FIXTURE_REFERENCE,
            "sourceSchema": 2,
            "sourceSha256": VERIFY.UNITY_PARTICLE_FIXTURE_SHA256,
            "sourceParticleCount": 7,
            "coordinateMapping": "bottom-left-to-top-left-y-flip",
            "colorMapping": "sRGB-to-linear-rgb-alpha-unchanged",
            "productionRandomStream": "not-used",
        }
        return value

    @staticmethod
    def comparison_frame(name):
        return {
            "name": name,
            "alphaSemantic": "coverage-union",
            "width": 1950,
            "height": 1097,
            "rawBytes": 17_113_200,
        }

    @staticmethod
    def replaced(value, path, replacement):
        clone = json.loads(json.dumps(value))
        parent = clone
        for component in path[:-1]:
            parent = parent[component]
        parent[path[-1]] = replacement
        return clone

    @staticmethod
    def without(value, path):
        clone = json.loads(json.dumps(value))
        parent = clone
        for component in path[:-1]:
            parent = parent[component]
        del parent[path[-1]]
        return clone

    def load(self, manifest):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            age_directory = path.parent / "0140ms"
            age_directory.mkdir()
            for name in (
                "FinalOverlay_NoTrail",
                "FinalOverlay_TrailOnly20px",
            ):
                with (age_directory / f"{name}.rgba16f").open("wb") as artifact:
                    artifact.truncate(17_113_200)
            return VERIFY._load_manifest(path.parent)

    def test_legacy_manifest_without_case_remains_click_compatible(self):
        loaded = self.load(self.manifest(VERIFY.AGES))
        self.assertNotIn("case", loaded)

    def test_explicit_click_case_remains_compatible(self):
        manifest = self.manifest(VERIFY.AGES)
        manifest["case"] = {"name": "click"}
        loaded = self.load(manifest)
        self.assertEqual("click", loaded["case"]["name"])

    def test_capture_profile_and_composite_formula_are_exact(self):
        valid = self.manifest(VERIFY.AGES)
        for field, value in (
            ("captureProfile", "background-aware"),
            ("compositeFormula", "arbitrary"),
        ):
            with self.subTest(field=field):
                with self.assertRaises(VERIFY.ValidationError):
                    self.load(self.replaced(valid, (field,), value))

    def test_drag_manifest_uses_the_locked_fixture(self):
        loaded = self.load(self.drag_manifest())
        self.assertEqual("drag-trail", loaded["case"]["name"])

    def test_drag_manifest_rejects_arbitrary_ages(self):
        manifest = self.drag_manifest()
        manifest["ages"][0]["ageMs"] = 150
        with self.assertRaises(VERIFY.ValidationError):
            self.load(manifest)

    def test_unity_particle_fixture_manifest_is_accepted(self):
        loaded = self.load(self.fixture_manifest())
        self.assertEqual("unity-particle-fixture", loaded["case"]["name"])

    def test_unity_particle_fixture_rejects_arbitrary_ages(self):
        manifest = self.fixture_manifest()
        manifest["ages"][0]["ageMs"] = 51
        with self.assertRaises(VERIFY.ValidationError):
            self.load(manifest)

    def test_manifest_containers_are_strictly_typed(self):
        valid_click = self.manifest(VERIFY.AGES)
        valid_drag = self.drag_manifest()
        valid_fixture = self.fixture_manifest()
        invalid_manifests = (
            ("root", []),
            ("viewport", self.replaced(valid_click, ("viewport",), [])),
            ("bloom", self.replaced(valid_click, ("bloom",), [])),
            ("ages", self.replaced(valid_click, ("ages",), {})),
            ("age entry", self.replaced(valid_click, ("ages", 0), [])),
            ("case", self.replaced(valid_drag, ("case",), [])),
            ("fixture case", self.replaced(valid_fixture, ("case",), [])),
            (
                "unityReference",
                self.replaced(valid_drag, ("case", "unityReference"), []),
            ),
            (
                "comparisonFrames",
                self.replaced(
                    valid_drag,
                    ("ages", 0, "comparisonFrames"),
                    {},
                ),
            ),
            (
                "comparison frame",
                self.replaced(
                    valid_drag,
                    ("ages", 0, "comparisonFrames", 0),
                    [],
                ),
            ),
        )
        for label, manifest in invalid_manifests:
            with self.subTest(field=label):
                with self.assertRaises(VERIFY.ValidationError):
                    self.load(manifest)

    def test_manifest_numeric_fields_reject_bool_and_string_coercion(self):
        valid_click = self.manifest(VERIFY.AGES)
        valid_drag = self.drag_manifest()
        valid_fixture = self.fixture_manifest()
        numeric_fields = (
            (valid_click, ("schemaVersion",)),
            (valid_click, ("seed",)),
            (valid_click, ("viewport", "width")),
            (valid_click, ("viewport", "height")),
            (valid_click, ("bloom", "mipCount")),
            (valid_click, ("bloom", "sampleScale")),
            (valid_click, ("bloom", "exposureGain")),
            (valid_click, ("ages", 0, "ageMs")),
            (valid_click, ("ages", 0, "layers", 0, "alphaSemantic")),
            (valid_drag, ("case", "movementPixels")),
            (valid_drag, ("case", "movementSteps")),
            (valid_drag, ("case", "trailOnlyPixels")),
            (valid_drag, ("case", "contractVersion")),
            (valid_fixture, ("case", "contractVersion")),
            (valid_fixture, ("case", "sourceSchema")),
            (valid_fixture, ("case", "sourceParticleCount")),
            (valid_drag, ("ages", 0, "comparisonFrames", 0, "width")),
            (valid_drag, ("ages", 0, "comparisonFrames", 0, "height")),
            (valid_drag, ("ages", 0, "comparisonFrames", 0, "rawBytes")),
        )
        for manifest, path in numeric_fields:
            for invalid in (True, "1"):
                with self.subTest(field=path, value=invalid):
                    with self.assertRaises(VERIFY.ValidationError):
                        self.load(self.replaced(manifest, path, invalid))

    def test_all_layers_requires_a_json_boolean(self):
        valid = self.manifest(VERIFY.AGES)
        for invalid in (0, 1, "false", None):
            with self.subTest(value=invalid):
                with self.assertRaises(VERIFY.ValidationError):
                    self.load(self.replaced(valid, ("allLayers",), invalid))

    def test_all_layers_requires_the_complete_locked_layer_set(self):
        valid = self.manifest(VERIFY.AGES)
        valid["allLayers"] = True
        valid_layers = [
            {
                "name": name,
                "alphaSemantic": VERIFY.LAYER_ALPHA_SEMANTICS[name],
                "width": extent[0],
                "height": extent[1],
                "rawBytes": extent[0] * extent[1] * 8,
            }
            for name, extent in VERIFY.EXPECTED_LAYER_EXTENTS.items()
        ]
        for age in valid["ages"]:
            age["layers"] = valid_layers
        self.load(valid)

        missing = self.replaced(valid, ("ages", 0, "layers"), valid_layers[:-1])
        with self.assertRaises(VERIFY.ValidationError):
            self.load(missing)

        wrong_extent = self.replaced(
            valid,
            ("ages", 0, "layers", 2, "width"),
            valid_layers[2]["width"] - 1,
        )
        with self.assertRaises(VERIFY.ValidationError):
            self.load(wrong_extent)

        wrong_semantic = self.replaced(
            valid,
            ("ages", 0, "layers", 0, "alphaSemantic"),
            "coverage-union",
        )
        with self.assertRaises(VERIFY.ValidationError):
            self.load(wrong_semantic)

    def test_required_manifest_fields_cannot_be_omitted(self):
        valid_click = self.manifest(VERIFY.AGES)
        valid_drag = self.drag_manifest()
        valid_fixture = self.fixture_manifest()
        required_fields = (
            (valid_click, ("schemaVersion",)),
            (valid_click, ("driver",)),
            (valid_click, ("captureProfile",)),
            (valid_click, ("compositeFormula",)),
            (valid_click, ("seed",)),
            (valid_click, ("allLayers",)),
            (valid_click, ("viewport",)),
            (valid_click, ("viewport", "width")),
            (valid_click, ("viewport", "height")),
            (valid_click, ("rowOrigin",)),
            (valid_click, ("bloom",)),
            (valid_click, ("bloom", "mipCount")),
            (valid_click, ("bloom", "sampleScale")),
            (valid_click, ("bloom", "exposureGain")),
            (valid_click, ("ages",)),
            (valid_click, ("ages", 0, "ageMs")),
            (valid_drag, ("case", "name")),
            (valid_drag, ("case", "contractVersion")),
            (valid_drag, ("case", "movementPixels")),
            (valid_drag, ("case", "movementSteps")),
            (valid_drag, ("case", "trailOnlyPixels")),
            (valid_drag, ("case", "trailFixture")),
            (valid_drag, ("case", "unityReference")),
            (valid_drag, ("case", "unityReference", "withTrail")),
            (valid_drag, ("case", "unityReference", "noTrail")),
            (valid_drag, ("case", "unityReference", "trailOnly")),
            (valid_drag, ("ages", 0, "comparisonFrames")),
            (
                valid_drag,
                ("ages", 0, "comparisonFrames", 0, "alphaSemantic"),
            ),
            (valid_fixture, ("case", "name")),
            (valid_fixture, ("case", "contractVersion")),
            (valid_fixture, ("case", "scope")),
            (valid_fixture, ("case", "sourceFixture")),
            (valid_fixture, ("case", "sourceSchema")),
            (valid_fixture, ("case", "sourceSha256")),
            (valid_fixture, ("case", "sourceParticleCount")),
            (valid_fixture, ("case", "coordinateMapping")),
            (valid_fixture, ("case", "colorMapping")),
            (valid_fixture, ("case", "productionRandomStream")),
        )
        for manifest, path in required_fields:
            with self.subTest(field=path):
                with self.assertRaises(VERIFY.ValidationError):
                    self.load(self.without(manifest, path))

    def test_drag_case_fields_are_exact(self):
        valid = self.drag_manifest()
        invalid_values = (
            (("case", "name"), "click-drag"),
            (("case", "contractVersion"), 2),
            (("case", "movementPixels"), 431),
            (("case", "movementSteps"), 13),
            (("case", "trailOnlyPixels"), 19),
            (("case", "trailFixture"), "arbitrary"),
            (("case", "unityReference", "withTrail"), "wrong.png"),
            (("case", "unityReference", "noTrail"), "wrong.png"),
            (("case", "unityReference", "trailOnly"), "wrong.png"),
        )
        for path, replacement in invalid_values:
            with self.subTest(field=path):
                with self.assertRaises(VERIFY.ValidationError):
                    self.load(self.replaced(valid, path, replacement))

    def test_unity_particle_fixture_fields_are_exact(self):
        valid = self.fixture_manifest()
        invalid_values = (
            (("case", "contractVersion"), 2),
            (("case", "scope"), "production"),
            (("case", "sourceFixture"), "wrong.json"),
            (("case", "sourceSchema"), 1),
            (("case", "sourceSha256"), "0" * 64),
            (("case", "sourceParticleCount"), 6),
            (("case", "coordinateMapping"), "unchanged"),
            (("case", "colorMapping"), "gamma"),
            (("case", "productionRandomStream"), "used"),
        )
        for path, replacement in invalid_values:
            with self.subTest(field=path):
                with self.assertRaises(VERIFY.ValidationError):
                    self.load(self.replaced(valid, path, replacement))

    def test_comparison_frames_are_an_exact_set(self):
        valid = self.drag_manifest()
        reversed_frames = self.replaced(
            valid,
            ("ages", 0, "comparisonFrames"),
            list(reversed(valid["ages"][0]["comparisonFrames"])),
        )
        self.load(reversed_frames)

        frames = valid["ages"][0]["comparisonFrames"]
        invalid_frame_sets = (
            frames[:1],
            [frames[0], frames[0]],
            frames + [self.comparison_frame("Unexpected")],
        )
        for comparison_frames in invalid_frame_sets:
            with self.subTest(frames=comparison_frames):
                with self.assertRaises(VERIFY.ValidationError):
                    self.load(
                        self.replaced(
                            valid,
                            ("ages", 0, "comparisonFrames"),
                            comparison_frames,
                        )
                    )

    def test_comparison_frame_dimensions_and_storage_are_exact(self):
        valid = self.drag_manifest()
        invalid_values = (
            (
                ("ages", 0, "comparisonFrames", 0, "alphaSemantic"),
                "bloom-transport-coverage",
            ),
            (("ages", 0, "comparisonFrames", 0, "width"), 1949),
            (("ages", 0, "comparisonFrames", 0, "height"), 1096),
            (("ages", 0, "comparisonFrames", 0, "rawBytes"), 17_113_199),
        )
        for path, replacement in invalid_values:
            with self.subTest(field=path):
                with self.assertRaises(VERIFY.ValidationError):
                    self.load(self.replaced(valid, path, replacement))

    def test_preview_transfer_matches_capture_rounding(self):
        self.assertEqual(0, VERIFY._linear_to_srgb_preview_byte(-1.0))
        self.assertEqual(0, VERIFY._linear_to_srgb_preview_byte(0.0))
        self.assertEqual(255, VERIFY._linear_to_srgb_preview_byte(1.0))
        self.assertEqual(255, VERIFY._linear_to_srgb_preview_byte(4.0))
        with self.assertRaises(VERIFY.ValidationError):
            VERIFY._linear_to_srgb_preview_byte(float("nan"))

    def test_bound_preview_rejects_png_that_differs_from_fp16(self):
        if VERIFY.PillowImage is None:
            self.skipTest("Pillow is required to create the compact PNG fixture")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw_path = root / "FinalOverlay.rgba16f"
            raw_path.write_bytes(b"\0" * 8)
            image = VERIFY.PillowImage.new("RGB", (1, 1), (1, 0, 0))
            image.save(root / "FinalOverlay.png")
            with (
                mock.patch.object(VERIFY, "CAPTURE_WIDTH", 1),
                mock.patch.object(VERIFY, "CAPTURE_HEIGHT", 1),
                self.assertRaisesRegex(VERIFY.ValidationError, "does not match"),
            ):
                VERIFY._read_bound_preview(root, "FinalOverlay")

    def test_json_safe_converts_non_finite_numbers_to_null(self):
        value = VERIFY._json_safe({"values": (1.0, float("nan"), float("inf"))})
        self.assertEqual({"values": [1.0, None, None]}, value)


class CommandLineContractTests(unittest.TestCase):
    def run_script(self, *arguments):
        return subprocess.run(
            [sys.executable, str(SCRIPT_PATH), *arguments],
            cwd=SCRIPT_PATH.parent.parent,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_layer_options_are_mutually_exclusive(self):
        result = self.run_script("--require-layers", "--no-layers")
        self.assertEqual(2, result.returncode)
        self.assertIn("not allowed with argument", result.stderr)

    def test_json_argument_error_is_a_single_parseable_envelope(self):
        result = self.run_script(
            "--json",
            "--require-layers",
            "--no-layers",
        )
        self.assertEqual(2, result.returncode)
        report = json.loads(result.stdout)
        self.assertFalse(report["passed"])
        self.assertEqual("arguments", report["errorKind"])
        self.assertTrue(report["errors"])
        self.assertEqual("", result.stderr)

    def test_json_input_error_is_a_single_parseable_envelope(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_script(
                "--json",
                f"--native-root={directory}",
            )
        self.assertEqual(2, result.returncode)
        report = json.loads(result.stdout)
        self.assertEqual(1, report["schemaVersion"])
        self.assertFalse(report["passed"])
        self.assertEqual("input", report["errorKind"])
        self.assertTrue(report["errors"])
        self.assertEqual("", result.stderr)


if __name__ == "__main__":
    unittest.main()
