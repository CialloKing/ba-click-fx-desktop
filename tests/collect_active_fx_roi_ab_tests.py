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

    def run_function_test(
        self,
        function_names: tuple[str, ...],
        body: str,
        *,
        timeout: int = 15,
    ) -> None:
        bootstrap = r"""
$tokens = $null
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    $env:BAFX_SCRIPT_UNDER_TEST,
    [ref]$tokens,
    [ref]$errors)
if (@($errors).Count -ne 0)
{
    throw ($errors | ForEach-Object { $_.Message } | Out-String)
}
$functions = @($ast.FindAll({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst]
}, $true))
foreach ($name in $env:BAFX_TEST_FUNCTIONS.Split(','))
{
    $matches = @($functions | Where-Object { $_.Name -eq $name })
    if ($matches.Count -ne 1)
    {
        throw "Expected one function named $name"
    }
    Invoke-Expression $matches[0].Extent.Text
}
Invoke-Expression $env:BAFX_TEST_BODY
"""
        result = subprocess.run(
            [
                str(POWERSHELL),
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-Command",
                bootstrap,
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
            encoding="utf-8",
            errors="replace",
            env={
                **os.environ,
                "BAFX_SCRIPT_UNDER_TEST": str(SCRIPT.resolve()),
                "BAFX_TEST_FUNCTIONS": ",".join(function_names),
                "BAFX_TEST_BODY": body,
                "BAFX_TEST_POWERSHELL": str(POWERSHELL.resolve()),
            },
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

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
            "$manifestSchemaVersion = 3",
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

    def test_keeps_the_default_release_manifest_and_matrix_unchanged(self) -> None:
        for token in (
            "$manifestSchemaVersion = 3",
            "$blockCount = 5",
            "$runCount = 20",
            "'bafx-active-fx-roi-ab-capture'",
            "pattern = 'ABBA'",
            "blocks = $blockCount",
            "runs = $runCount",
            "'collecting'",
            "'captured'",
            "'failed'",
        ):
            self.assertIn(token, self.source)
        self.assertIn("[int]$DiagnosticBlocks = 0", self.source)
        self.assertIn("$activeManifestSchemaVersion", self.source)

    def test_marks_the_two_block_matrix_as_non_release_diagnostic_evidence(self) -> None:
        for token in (
            "[ValidateSet(0, 2)]",
            "$diagnosticManifestSchemaVersion = 2",
            "$diagnosticBlockCount = 2",
            "$diagnosticRunCount = 8",
            "'bafx-active-fx-roi-diagnostic-capture'",
            "'diagnostic-collecting'",
            "'diagnostic-captured'",
            "'diagnostic-failed'",
            "pattern = 'ABBA+BAAB'",
            "blockPatterns = @('ABBA', 'BAAB')",
            "$manifest['releaseEligible'] = $false",
            "NON-RELEASE: short matrix for causal investigation only",
            "$run['blockPattern'] = $BlockPattern",
            "the release reporter must reject this manifest",
        ):
            self.assertIn(token, self.source)

    def test_diagnostic_schedule_uses_abba_then_baab(self) -> None:
        for token in (
            "$abbaPattern = @(",
            "$baabPattern = @(",
            "$blockPattern = if ($diagnosticMode -and $block -eq 2)",
            "$pattern = if ($blockPattern -eq 'BAAB')",
            "[ordered]@{ arm = 'A'; enabled = $false }",
            "[ordered]@{ arm = 'B'; enabled = $true }",
            "-BlockPattern $blockPattern",
            "-DiagnosticMode $diagnosticMode",
            "-RequireCausalTiming $DiagnosticMode",
        ):
            self.assertIn(token, self.source)

    def test_nvidia_telemetry_is_diagnostic_only_and_validates_its_contract(self) -> None:
        for token in (
            "[switch]$CaptureNvidiaTelemetry",
            "$nvidiaTelemetryIntervalMilliseconds = 200",
            "CaptureNvidiaTelemetry is restricted to -DiagnosticBlocks 2",
            "function Resolve-NvidiaSmiExecutable",
            "NVIDIA telemetry executable must be named nvidia-smi.exe",
            "'NVIDIA Corporation'",
            "'NVIDIA-SMI'",
            "function ConvertFrom-NvidiaTelemetryLine",
            "must contain exactly $($Fields.Count) fields",
            "--format=csv,noheader,nounits",
            "must expose exactly one $script:requiredAdapterNameFragment GPU",
            "Get-FileHash -LiteralPath $ExecutablePath -Algorithm SHA256",
        ):
            self.assertIn(token, self.source)
        for field in (
            "'timestamp'",
            "'index'",
            "'uuid'",
            "'name'",
            "'pstate'",
            "'clocks.current.sm'",
            "'clocks.current.memory'",
            "'power.draw.instant'",
            "'temperature.gpu'",
            "'utilization.gpu'",
            "'utilization.memory'",
        ):
            self.assertIn(field, self.source)
        self.assertNotIn("'clocks.current.graphics'", self.source)
        self.assertNotIn("'power.draw',", self.source)

    def test_nvidia_sampler_is_owned_per_run_and_writes_canonical_csv(self) -> None:
        for token in (
            "function Start-NvidiaTelemetry",
            '"--loop-ms=$script:nvidiaTelemetryIntervalMilliseconds"',
            "-RedirectStandardOutput $rawPath",
            "-RedirectStandardError $stderrPath",
            "function Stop-NvidiaTelemetry",
            "Stop-OwnedProcess -Process $Session.process",
            "nvidia-smi telemetry produced no samples",
            "$csvLines = @($script:nvidiaTelemetryFields -join ',') + @($lines)",
            "collectorStoppedProcess = $true",
            "$run['nvidiaTelemetry'] = $telemetryEvidence",
        ):
            self.assertIn(token, self.source)
        self.assertNotIn("Get-Process -Name 'nvidia-smi'", self.source)
        self.assertNotIn("Stop-Process -Name", self.source)

    def test_nvidia_sampler_requires_tolerant_full_session_coverage(self) -> None:
        for token in (
            "$nvidiaTelemetryCoverageMinimumSlackMilliseconds = 2000",
            "$nvidiaTelemetryCoverageIntervalMultiplier = 10",
            "$nvidiaTelemetryMinimumSampleIntervalMultiplier = 2",
            "function Confirm-NvidiaTelemetryCoverage",
            "$TimestampValues.Count -lt $minimumSamples",
            "$gapMilliseconds -gt $slackMilliseconds",
            "$actualSpanMilliseconds -lt $minimumSpanMilliseconds",
            "-TimestampValues $timestampValues `",
            "-StartedAtUtc $Session.startedAtUtc `",
            "-StoppedAtUtc $stoppedAt `",
        ):
            self.assertIn(token, self.source)

        self.run_function_test(
            ("Confirm-NvidiaTelemetryCoverage",),
            r"""
$culture = [Globalization.CultureInfo]::InvariantCulture
$start = [DateTime]::SpecifyKind(
    [DateTime]::Parse('2026-08-25T04:00:00', $culture),
    [DateTimeKind]::Utc)
$stop = $start.AddSeconds(10)
function Format-NvidiaTimestamp
{
    param([DateTime]$TimestampUtc)
    return $TimestampUtc.ToLocalTime().ToString(
        'yyyy/MM/dd HH:mm:ss.fff',
        $culture)
}
function Confirm-Accepted
{
    param([DateTime[]]$TimestampsUtc)
    Confirm-NvidiaTelemetryCoverage `
        -TimestampValues @(
            $TimestampsUtc |
                ForEach-Object { Format-NvidiaTimestamp $_ }) `
        -StartedAtUtc $start `
        -StoppedAtUtc $stop `
        -IntervalMilliseconds 200 `
        -MinimumSlackMilliseconds 2000 `
        -SlackIntervalMultiplier 10 `
        -MinimumSampleIntervalMultiplier 2
}
function Confirm-Rejected
{
    param(
        [DateTime[]]$TimestampsUtc,
        [string]$ExpectedMessage)
    try
    {
        Confirm-Accepted -TimestampsUtc $TimestampsUtc
    }
    catch
    {
        if ($_.Exception.Message -notlike "*$ExpectedMessage*")
        {
            throw "unexpected coverage error: $($_.Exception.Message)"
        }
        return
    }
    throw "coverage unexpectedly accepted: $ExpectedMessage"
}

# Half the nominal rate plus both endpoints is accepted, so normal scheduler
# jitter does not need to reproduce a precise 200 ms cadence.
$halfRate = @(
    for ($index = 0; $index -le 25; ++$index)
    {
        $start.AddMilliseconds($index * 400)
    })
Confirm-Accepted -TimestampsUtc $halfRate
Confirm-Rejected `
    -TimestampsUtc @($start) `
    -ExpectedMessage 'at least 25 are required'
$stalled = @(
    for ($index = 0; $index -lt 25; ++$index)
    {
        $start
    })
Confirm-Rejected `
    -TimestampsUtc $stalled `
    -ExpectedMessage 'outside the session stop tolerance'
$middleGap = @(
    0..10 | ForEach-Object { $start.AddMilliseconds($_ * 200) }
    25..50 | ForEach-Object { $start.AddMilliseconds($_ * 200) })
Confirm-Rejected `
    -TimestampsUtc $middleGap `
    -ExpectedMessage 'gap before sample 12 exceeds 2000 ms'
""",
        )

    def test_preserves_the_primary_run_failure_before_cleanup_failures(self) -> None:
        for token in (
            "$primaryFailure = $_",
            '"Host cleanup failed: $($_.Exception.Message)"',
            '"NVIDIA telemetry stop failed: $($_.Exception.Message)"',
            "Join-RunFailureMessages `",
            "-PrimaryFailure $primaryFailure `",
            "-CleanupFailures $cleanupFailures",
            "[AllowEmptyCollection()]",
            "[string[]]$CleanupFailures = @()",
        ):
            self.assertIn(token, self.source)
        self.run_function_test(
            ("Join-RunFailureMessages",),
            r"""
try
{
    throw 'Host primary failure'
}
catch
{
    $primary = $_
}
$emptyCleanup = [Collections.Generic.List[string]]::new()
$success = Join-RunFailureMessages `
    -PrimaryFailure $null `
    -CleanupFailures $emptyCleanup
if ($null -ne $success)
{
    throw "unexpected success-path failure: $success"
}
$primaryOnly = Join-RunFailureMessages `
    -PrimaryFailure $primary `
    -CleanupFailures $emptyCleanup
if ($primaryOnly -cne 'Host primary failure')
{
    throw "unexpected primary-only failure: $primaryOnly"
}
$defaultSuccess = Join-RunFailureMessages -PrimaryFailure $null
if ($null -ne $defaultSuccess)
{
    throw "unexpected default success-path failure: $defaultSuccess"
}
$cleanup = [Collections.Generic.List[string]]::new()
$cleanup.Add('Host cleanup failed: host stop failure')
$cleanup.Add('NVIDIA telemetry stop failed: sampler failure')
$message = Join-RunFailureMessages `
    -PrimaryFailure $primary `
    -CleanupFailures $cleanup
$expected = 'Host primary failure; Host cleanup failed: host stop failure; NVIDIA telemetry stop failed: sampler failure'
if ($message -cne $expected)
{
    throw "unexpected combined failure: $message"
}
$cleanupOnly = Join-RunFailureMessages `
    -PrimaryFailure $null `
    -CleanupFailures @('NVIDIA telemetry stop failed: sampler failure')
if ($cleanupOnly -cne 'NVIDIA telemetry stop failed: sampler failure')
{
    throw "unexpected cleanup-only failure: $cleanupOnly"
}
""",
        )

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
        self.assertIn("$Process.Kill()", self.source)
        self.assertIn("$exited = $Process.WaitForExit(5000)", self.source)
        self.assertIn("-not $exited -or -not $Process.HasExited", self.source)
        self.assertNotIn(
            "Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue",
            self.source,
        )
        self.assertNotIn("Stop-Process -Name", self.source)
        self.assertNotIn("taskkill", self.source.lower())

    def test_owned_process_cleanup_waits_for_confirmed_exit(self) -> None:
        self.run_function_test(
            ("Stop-OwnedProcess",),
            r"""
$child = Start-Process `
    -FilePath $env:BAFX_TEST_POWERSHELL `
    -ArgumentList @(
        '-NoProfile',
        '-Command',
        'Start-Sleep -Seconds 30') `
    -WindowStyle Hidden `
    -PassThru
try
{
    Stop-OwnedProcess -Process $child
    if (-not $child.HasExited)
    {
        throw 'owned process remained alive after cleanup'
    }
}
finally
{
    if (-not $child.HasExited)
    {
        $child.Kill()
        $child.WaitForExit()
    }
}
""",
        )

    def test_owns_a_display_power_request_for_the_complete_capture(self) -> None:
        for token in (
            "PowerCreateRequest",
            "PowerSetRequest",
            "PowerClearRequest",
            "PowerRequestType.DisplayRequired",
            "PowerRequestType.SystemRequired",
            "AcquireAndWake",
            "keybd_event(0x87, 0, 0, UIntPtr.Zero)",
            "keybd_event(0x87, 0, 2, UIntPtr.Zero)",
            "Enable-CapturePowerRequest",
            "Disable-CapturePowerRequest",
            "$capturePowerRequest = Enable-CapturePowerRequest",
            "$capturePowerRequest = [IntPtr]::Zero",
        ):
            self.assertIn(token, self.source)

    def test_rejects_interrupted_selected_intervals_before_capture_completes(self) -> None:
        for token in (
            "observed unavailable display power during capture",
            "Select-Object `",
            "-Skip $script:discardCompleteIntervals `",
            "-First $script:selectCompleteIntervals",
            "has no presented frames",
            "Cpu.PresentCall.Samples",
            "Present samples do not match Window.FrameCount",
            '"$roiPrefix.ObservedFrames"',
            "ROI observed frames do not match Window.FrameCount",
            "-MeasurementPath $MeasurementPath",
        ):
            self.assertIn(token, self.source)

    def test_diagnostic_intervals_fail_closed_on_causal_timing_drift(self) -> None:
        for token in (
            "function Confirm-CausalMetricContract",
            "function Confirm-CausalTimingIntervalContract",
            "Timing.PrePresentSemantic",
            "Timing.FramePacingWaitSemantic",
            "Cpu.PrePresent samples do not match Window.FrameCount",
            "FramePacing.Wait samples do not match wake count",
            "FramePacing.Wait samples do not match bucket count",
            "dropped samples must be zero",
        ):
            self.assertIn(token, self.source)

        self.run_function_test(
            (
                "Get-RequiredEventString",
                "Get-RequiredEventInteger",
                "Get-RequiredEventBoolean",
                "Get-RequiredEventNumber",
                "Confirm-CausalMetricContract",
                "Confirm-CausalTimingIntervalContract",
            ),
            r"""
function Add-Metric
{
    param(
        [Collections.IDictionary]$Event,
        [string]$Prefix,
        [int]$Samples,
        [int]$Maximum)
    $Event["$Prefix.Available"] = 'true'
    $Event["$Prefix.Unit"] = 'us'
    $Event["$Prefix.Samples"] = [string]$Samples
    $Event["$Prefix.RecordedSamples"] = [string]$Samples
    $Event["$Prefix.DroppedSamples"] = '0'
    $Event["$Prefix.Min"] = '0'
    $Event["$Prefix.Average"] = [string]($Maximum / 2.0)
    $Event["$Prefix.P50"] = [string]([int]($Maximum / 2))
    $Event["$Prefix.P95"] = [string]($Maximum - 2)
    $Event["$Prefix.P99"] = [string]($Maximum - 1)
    $Event["$Prefix.Max"] = [string]$Maximum
}
function New-Event
{
    $event = [ordered]@{
        'Timing.PrePresentSemantic' = 'fx-render-return-to-Present-call-entry-including-roi-diagnostics-spout-gpu-query-end-and-readback'
        'Timing.FramePacingWaitSemantic' = 'owner-thread-qpc-around-waitForAnyFrameOpportunity-including-handle-prepoll-and-message-wait-excluding-wait-set-build-and-post-wake-work'
        'FramePacing.FrameReadyWakes' = '3'
        'FramePacing.DeviceRemovedWakes' = '0'
        'FramePacing.CadenceWakes' = '1'
        'FramePacing.MessageWakes' = '1'
        'FramePacing.Timeouts' = '0'
        'FramePacing.Failures' = '0'
        'FramePacing.Wait.Lt100Us' = '1'
        'FramePacing.Wait.100To999Us' = '1'
        'FramePacing.Wait.1000To3999Us' = '1'
        'FramePacing.Wait.4000To7999Us' = '1'
        'FramePacing.Wait.Ge8000Us' = '1'
    }
    Add-Metric -Event $event -Prefix 'Cpu.PrePresent' -Samples 10 -Maximum 100
    Add-Metric -Event $event -Prefix 'FramePacing.Wait' -Samples 5 -Maximum 8000
    return $event
}
function Confirm-Rejected
{
    param(
        [Collections.IDictionary]$Event,
        [string]$ExpectedMessage)
    try
    {
        Confirm-CausalTimingIntervalContract `
            -Event $Event `
            -FrameCount 10 `
            -Context 'fixture'
    }
    catch
    {
        if ($_.Exception.Message -notlike "*$ExpectedMessage*")
        {
            throw "unexpected causal timing error: $($_.Exception.Message)"
        }
        return
    }
    throw "causal timing drift unexpectedly accepted: $ExpectedMessage"
}

Confirm-CausalTimingIntervalContract `
    -Event (New-Event) `
    -FrameCount 10 `
    -Context 'fixture'
$missing = New-Event
$missing.Remove('Cpu.PrePresent.P99')
Confirm-Rejected -Event $missing -ExpectedMessage 'Cpu.PrePresent.P99'
$frameDrift = New-Event
$frameDrift['Cpu.PrePresent.Samples'] = '9'
$frameDrift['Cpu.PrePresent.RecordedSamples'] = '9'
Confirm-Rejected -Event $frameDrift -ExpectedMessage 'Window.FrameCount'
$dropped = New-Event
$dropped['Cpu.PrePresent.DroppedSamples'] = '1'
Confirm-Rejected -Event $dropped -ExpectedMessage 'dropped samples must be zero'
$wakeDrift = New-Event
$wakeDrift['FramePacing.FrameReadyWakes'] = '2'
Confirm-Rejected -Event $wakeDrift -ExpectedMessage 'wake count'
$bucketDrift = New-Event
$bucketDrift['FramePacing.Wait.Ge8000Us'] = '0'
Confirm-Rejected -Event $bucketDrift -ExpectedMessage 'bucket count'
$semanticDrift = New-Event
$semanticDrift['Timing.PrePresentSemantic'] = 'unknown'
Confirm-Rejected -Event $semanticDrift -ExpectedMessage 'semantic differs'
$waitSemanticDrift = New-Event
$waitSemanticDrift['Timing.FramePacingWaitSemantic'] = 'unknown'
Confirm-Rejected -Event $waitSemanticDrift -ExpectedMessage 'semantic differs'
$nonFinite = New-Event
$nonFinite['FramePacing.Wait.Average'] = 'NaN'
Confirm-Rejected -Event $nonFinite -ExpectedMessage 'finite number'
""",
        )

    def test_accepts_identical_event_fields_but_rejects_conflicts(self) -> None:
        self.assertIn(
            "if ($event.Contains($name) -and $event[$name] -cne $value)",
            self.source,
        )
        self.assertIn("contains conflicting duplicate field", self.source)

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
