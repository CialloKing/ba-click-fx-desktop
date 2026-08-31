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

    def test_encodes_the_release_matrix_and_environment_contract(self) -> None:
        for token in (
            "$manifestSchemaVersion = 4",
            "$configSchemaVersion = 19",
            "$environmentContract = 'rtx-4060-4k170-sdr-v1'",
            "$requiredAdapterNameFragment = 'RTX 4060'",
            "$requiredOutputWidth = 3840",
            "$requiredOutputHeight = 2160",
            "$requiredRefreshRateNumerator = 170",
            "$requiredRefreshRateDenominator = 1",
            "$blockCount = 5",
            "$runCount = 20",
            "pattern = 'ABBA'",
            "differenceContract = 'performance.activeFxRoiEnabled-only'",
            "Get-FileHash -LiteralPath $runExecutable -Algorithm SHA256",
            "Get-FileHash -LiteralPath $runConfig -Algorithm SHA256",
            "$finalConfigSha256 -ne $initialConfigSha256",
        ):
            self.assertIn(token, self.source)

    def test_run_configuration_changes_only_the_roi_switch(self) -> None:
        self.run_function_test(
            ("New-RunConfiguration",),
            r"""
$base = [pscustomobject]@{
    schemaVersion = 19
    performance = [pscustomobject]@{
        activeFxRoiEnabled = $false
        marker = 'unchanged'
    }
    marker = 'unchanged'
}
$off = New-RunConfiguration -BaseConfiguration $base -RoiEnabled $false
$on = New-RunConfiguration -BaseConfiguration $base -RoiEnabled $true
if (-not $on.performance.activeFxRoiEnabled -or
    $off.performance.activeFxRoiEnabled)
{
    throw 'ROI arm values are incorrect'
}
$on.performance.activeFxRoiEnabled = $false
if (($on | ConvertTo-Json -Depth 20 -Compress) -cne
    ($off | ConvertTo-Json -Depth 20 -Compress))
{
    throw 'the A/B configurations differ outside activeFxRoiEnabled'
}
""",
        )

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

    def test_host_idle_preflight_uses_generic_system_cpu_time_before_capture_and_blocks(
        self,
    ) -> None:
        for token in (
            "$hostIdleSampleCount = 5",
            "$hostIdleSampleMilliseconds = 1000",
            "$hostIdleMaximumBusyPercent = 10.0",
            "private static extern bool GetSystemTimes(",
            "function ConvertFrom-SystemCpuTimeDelta",
            "function Measure-SystemCpuBusySamples",
            "function Confirm-HostIdleSamples",
            "Confirm-HostIdle -Context 'capture start'",
            'Confirm-HostIdle -Context "block $block"',
            "sustained system CPU busy exceeded",
        ):
            self.assertIn(token, self.source)
        self.assertNotIn("winrar", self.source.lower())
        self.assertNotIn("general.rar", self.source.lower())

        initial_check = self.source.index("Confirm-HostIdle -Context 'capture start'")
        output_creation = self.source.index(
            "$null = New-Item -ItemType Directory -Path $outputRoot",
            initial_check,
        )
        block_check = self.source.index('Confirm-HostIdle -Context "block $block"')
        block_pattern = self.source.index("$blockPattern = if", block_check)
        self.assertLess(initial_check, output_creation)
        self.assertLess(block_check, block_pattern)

    def test_system_cpu_delta_and_sustained_idle_policy_are_fixture_driven(self) -> None:
        self.run_function_test(
            (
                "ConvertFrom-SystemCpuTimeDelta",
                "Confirm-HostIdleSamples",
            ),
            r"""
function New-Snapshot
{
    param(
        [decimal]$Idle,
        [decimal]$Kernel,
        [decimal]$User)
    return [ordered]@{
        idle = $Idle
        kernel = $Kernel
        user = $User
    }
}
function Confirm-Rejected
{
    param(
        [scriptblock]$Action,
        [string]$ExpectedMessage)
    try
    {
        & $Action
    }
    catch
    {
        if ($_.Exception.Message -notlike "*$ExpectedMessage*")
        {
            throw "unexpected idle preflight error: $($_.Exception.Message)"
        }
        return
    }
    throw "idle preflight unexpectedly accepted: $ExpectedMessage"
}

$before = New-Snapshot -Idle 100 -Kernel 200 -User 100
$after = New-Snapshot -Idle 175 -Kernel 300 -User 100
$busy = ConvertFrom-SystemCpuTimeDelta -Before $before -After $after
if ([Math]::Abs($busy - 25.0) -gt 0.0001)
{
    throw "unexpected busy percentage: $busy"
}
$boundary = ConvertFrom-SystemCpuTimeDelta `
    -Before $before `
    -After (New-Snapshot -Idle 180 -Kernel 300 -User 100)
if ([Math]::Abs($boundary - 20.0) -gt 0.0001)
{
    throw "unexpected boundary percentage: $boundary"
}
Confirm-Rejected `
    -Action {
        ConvertFrom-SystemCpuTimeDelta `
            -Before $before `
            -After (New-Snapshot -Idle 99 -Kernel 300 -User 100)
    } `
    -ExpectedMessage 'counter regressed at idle'
Confirm-Rejected `
    -Action {
        ConvertFrom-SystemCpuTimeDelta -Before $before -After $before
    } `
    -ExpectedMessage 'did not advance'
Confirm-Rejected `
    -Action {
        ConvertFrom-SystemCpuTimeDelta `
            -Before $before `
            -After (New-Snapshot -Idle 250 -Kernel 300 -User 100)
    } `
    -ExpectedMessage 'idle time exceeds total time'
Confirm-Rejected `
    -Action {
        ConvertFrom-SystemCpuTimeDelta `
            -Before ([ordered]@{ idle = 1; kernel = 2 }) `
            -After $after
    } `
    -ExpectedMessage 'missing user'

# Two transient spikes do not establish sustained load; the median remains at
# the fixed acceptance boundary.
Confirm-HostIdleSamples `
    -BusyPercentSamples @(0.0, 5.0, 10.0, 50.0, 100.0) `
    -ExpectedSampleCount 5 `
    -MaximumBusyPercent 10.0 `
    -Context 'fixture'
Confirm-Rejected `
    -Action {
        Confirm-HostIdleSamples `
            -BusyPercentSamples @(1.0, 9.0, 10.1, 20.0, 80.0) `
            -ExpectedSampleCount 5 `
            -MaximumBusyPercent 10.0 `
            -Context 'fixture block'
    } `
    -ExpectedMessage 'fixture block requires an idle Host'
Confirm-Rejected `
    -Action {
        Confirm-HostIdleSamples `
            -BusyPercentSamples @(1.0, 2.0) `
            -ExpectedSampleCount 5 `
            -MaximumBusyPercent 10.0 `
            -Context 'fixture'
    } `
    -ExpectedMessage '2 samples instead of 5'
Confirm-Rejected `
    -Action {
        Confirm-HostIdleSamples `
            -BusyPercentSamples @(0.0, 1.0, [double]::NaN, 2.0, 3.0) `
            -ExpectedSampleCount 5 `
            -MaximumBusyPercent 10.0 `
            -Context 'fixture'
    } `
    -ExpectedMessage 'invalid CPU busy sample'
""",
        )

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

    def test_dirty_present_interval_contract_fails_closed(self) -> None:
        self.run_function_test(
            (
                "Get-RequiredEventString",
                "Get-RequiredEventInteger",
                "Get-RequiredEventInt64",
                "Confirm-DirtyPresentIntervalContract",
            ),
            r"""
function Confirm-Rejected
{
    param(
        [Collections.IDictionary]$Event,
        [string]$MeasurementPath,
        [bool]$RoiEnabled,
        [string]$ExpectedDecisionReason,
        [string]$ExpectedMessage)
    try
    {
        Confirm-DirtyPresentIntervalContract `
            -Event $Event `
            -MeasurementPath $MeasurementPath `
            -RoiEnabled $RoiEnabled `
            -ExpectedDecisionReason $ExpectedDecisionReason `
            -Context 'fixture'
    }
    catch
    {
        if ($_.Exception.Message -notlike "*$ExpectedMessage*")
        {
            throw "unexpected dirty Present error: $($_.Exception.Message)"
        }
        return
    }
    throw "dirty Present drift unexpectedly accepted: $ExpectedMessage"
}

$off = [ordered]@{
    'ROI.FinalCompositePath' = 'full-screen'
    'ROI.Present.DirtyFrames' = '0'
    'ROI.Present.DirtyPixels.Total' = '0'
}
Confirm-DirtyPresentIntervalContract `
    -Event $off `
    -MeasurementPath 'primary' `
    -RoiEnabled $false `
    -ExpectedDecisionReason 'applied' `
    -Context 'off'

$on = [ordered]@{
    'ROI.FinalCompositePath' = 'dirty-present-verified-resolve-scissor-with-full-screen-fallback'
    'ROI.Present.DirtyFrames' = '9'
    'ROI.Present.DirtyPixels.Total' = '4294967296'
    'ROI.Primary.AppliedFrames' = '10'
    'ROI.Primary.WarmupFrames' = '1'
}
Confirm-DirtyPresentIntervalContract `
    -Event $on `
    -MeasurementPath 'primary' `
    -RoiEnabled $true `
    -ExpectedDecisionReason 'applied' `
    -Context 'on'

$on['ROI.Present.DirtyFrames'] = '8'
Confirm-Rejected $on 'primary' $true 'applied' 'do not match'
$on['ROI.Present.DirtyFrames'] = '9'
$on['ROI.Present.DirtyPixels.Total'] = '0'
Confirm-Rejected $on 'primary' $true 'applied' 'must be positive'
$on['ROI.Present.DirtyPixels.Total'] = '4294967296'
$on['ROI.FinalCompositePath'] = 'full-clear-verified-resolve-scissor-with-full-screen-fallback'
Confirm-Rejected $on 'primary' $true 'applied' 'path mismatch'

$recording = [ordered]@{
    'ROI.FinalCompositePath' = 'dirty-present-verified-resolve-scissor-with-full-screen-fallback'
    'ROI.Present.DirtyFrames' = '0'
    'ROI.Present.DirtyPixels.Total' = '0'
}
Confirm-DirtyPresentIntervalContract `
    -Event $recording `
    -MeasurementPath 'recording-rebuild' `
    -RoiEnabled $true `
    -ExpectedDecisionReason 'applied' `
    -Context 'recording'
$recording['ROI.Present.DirtyFrames'] = '1'
Confirm-Rejected $recording 'recording-rebuild' $true 'applied' 'unexpectedly used'
""",
        )

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

if __name__ == "__main__":
    unittest.main()
