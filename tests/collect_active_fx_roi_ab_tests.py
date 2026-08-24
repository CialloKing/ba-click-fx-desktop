#!/usr/bin/env python3
"""Source and capability contracts for the ROI ABBA PowerShell collector."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import unittest


if len(sys.argv) < 3:
    raise RuntimeError("expected PowerShell executable and collector script paths")

POWERSHELL = Path(sys.argv.pop(1))
SCRIPT = Path(sys.argv.pop(1))


class ActiveFxRoiAbCollectorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = SCRIPT.read_text(encoding="utf-8-sig")

    def test_script_has_valid_powershell_syntax(self) -> None:
        parser = (
            "$tokens=$null; $errors=$null; "
            "[System.Management.Automation.Language.Parser]::ParseFile("
            "$env:BAFX_SCRIPT_UNDER_TEST,[ref]$tokens,[ref]$errors) | Out-Null; "
            "if(@($errors).Count -ne 0) { "
            "$errors | ForEach-Object { Write-Error $_.Message }; exit 1 }"
        )
        result = subprocess.run(
            [
                str(POWERSHELL),
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-Command",
                parser,
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
            encoding="utf-8",
            errors="replace",
            env={**os.environ, "BAFX_SCRIPT_UNDER_TEST": str(SCRIPT.resolve())},
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_encodes_the_fixed_abba_timing_and_identity_contract(self) -> None:
        for token in (
            "$manifestSchemaVersion = 2",
            "$configSchemaVersion = 19",
            "$environmentContract = 'rtx-4060-4k170-sdr-v1'",
            "$requiredAdapterNameFragment = 'RTX 4060'",
            "$requiredOutputWidth = 3840",
            "$requiredOutputHeight = 2160",
            "$requiredRefreshRateNumerator = 170",
            "$requiredRefreshRateDenominator = 1",
            "$blockCount = 5",
            "$runCount = 20",
            "$warmupMilliseconds = 5000",
            "$sampleMilliseconds = 30000",
            "$hostDurationMilliseconds = 40500",
            "$performanceIntervalMilliseconds = 10000",
            "$discardCompleteIntervals = 1",
            "$selectCompleteIntervals = 3",
            "pattern = 'ABBA'",
            "differenceContract = 'performance.activeFxRoiEnabled-only'",
            "Get-FileHash -LiteralPath $runExecutable -Algorithm SHA256",
            "Get-FileHash -LiteralPath $runConfig -Algorithm SHA256",
            "$finalConfigSha256 -ne $initialConfigSha256",
            '"--demo-scenario=$Scenario"',
            "--demo-age-ms=$script:demoAgeMilliseconds",
            "--demo-delay-ms=$script:warmupMilliseconds",
            "--quit-after-ms=$script:hostDurationMilliseconds",
        ):
            self.assertIn(token, self.source)

    def test_extracts_and_compares_every_raw_log_environment(self) -> None:
        for token in (
            "function Get-OnlyStructuredEvent",
            "-Name 'SupportReport'",
            "-Name 'Configuration.Applied'",
            "function New-EnvironmentIdentity",
            "Confirm-RequiredEnvironment -Identity $identity -Context $Path",
            "function Confirm-SameEnvironmentIdentity",
            "environment = [ordered]@{",
            "identity = $null",
            "$manifest.environment.identity = $result.environmentIdentity",
            "-Expected $manifest.environment.identity",
            "-Actual $result.environmentIdentity",
        ):
            self.assertIn(token, self.source)

    def test_supports_each_deterministic_host_scenario(self) -> None:
        for token in (
            "[ValidateSet('center-click', 'interior-trail', 'boundary-top-left')]",
            "'center-click' = 'fixed-age-center-click'",
            "'interior-trail' = 'fixed-age-interior-trail'",
            "'boundary-top-left' = 'fixed-age-boundary-top-left'",
            "driver = 'host-demo-interior-trail-fixed-age-v1'",
            "driver = 'host-demo-boundary-top-left-fixed-age-v1'",
            "workload = $scenarioWorkloads[$Scenario]",
        ):
            self.assertIn(token, self.source)

    def test_recording_rebuild_explicitly_enables_spout2(self) -> None:
        self.assertIn("if ($MeasurementPath -eq 'recording-rebuild')", self.source)
        self.assertIn("$arguments += '--spout2'", self.source)

    def test_starts_hidden_and_only_reclaims_its_owned_process(self) -> None:
        self.assertIn("Start-Process", self.source)
        self.assertIn("-WindowStyle Hidden", self.source)
        self.assertIn("Stop-OwnedProcess -Process $process", self.source)
        self.assertIn("Stop-Process -Id $Process.Id", self.source)
        self.assertNotIn("Stop-Process -Name", self.source)
        self.assertNotIn("taskkill", self.source.lower())

    def test_exposes_applied_and_fallback_measurement_paths(self) -> None:
        for token in (
            "[ValidateSet('primary', 'recording-rebuild')]",
            "'background-differential-bloom'",
            "'context1-unavailable'",
            "'shared-target-full-write'",
            "'area-too-large'",
            "'benefit-too-small'",
            "'touches-boundary'",
            "'boundary-fallback'",
            "'renderer-fallback'",
            "expectation = $expectation",
            "expectedDecisionReason = $manifestDecisionReason",
        ):
            self.assertIn(token, self.source)

if __name__ == "__main__":
    unittest.main()
