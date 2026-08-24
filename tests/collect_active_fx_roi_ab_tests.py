#!/usr/bin/env python3
"""Source and capability contracts for the ROI ABBA PowerShell collector."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
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
            "$configSchemaVersion = 19",
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
            "--demo-age-ms=$script:demoAgeMilliseconds",
            "--demo-delay-ms=$script:warmupMilliseconds",
            "--quit-after-ms=$script:hostDurationMilliseconds",
        ):
            self.assertIn(token, self.source)

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

    def test_interior_trail_fails_with_machine_readable_capability(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "ba-click-fx-desktop.exe"
            executable.write_bytes(b"not executed")
            config = root / "BAFX.config.json"
            config.write_text(
                json.dumps(
                    {
                        "schemaVersion": 19,
                        "performance": {"activeFxRoiEnabled": False},
                    }
                ),
                encoding="utf-8",
            )
            output = root / "unsupported-evidence"
            result = subprocess.run(
                [
                    str(POWERSHELL),
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(SCRIPT),
                    "-Executable",
                    str(executable),
                    "-Configuration",
                    str(config),
                    "-OutputDirectory",
                    str(output),
                    "-Scenario",
                    "interior-trail",
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=10,
                encoding="utf-8",
                errors="replace",
            )
            self.assertNotEqual(result.returncode, 0)
            manifest = json.loads((output / "capture.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["schemaVersion"], 1)
            self.assertEqual(manifest["captureStatus"], "unsupported")
            capability = manifest["capabilities"]["interior-trail"]
            self.assertFalse(capability["supported"])
            self.assertIsNone(capability["driver"])
            self.assertEqual(
                capability["failureCode"],
                "host-has-no-deterministic-trail-driver",
            )


if __name__ == "__main__":
    unittest.main()
