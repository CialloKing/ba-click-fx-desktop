[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [Parameter(Mandatory = $true)]
    [string]$Configuration,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [ValidateSet('center-click', 'interior-trail', 'boundary-top-left')]
    [string]$Scenario = 'center-click',

    [ValidateSet('primary', 'recording-rebuild')]
    [string]$MeasurementPath = 'primary',

    [ValidateSet(
        'applied',
        'no-content',
        'background-differential-bloom',
        'context1-unavailable',
        'shared-target-full-write',
        'area-too-large',
        'benefit-too-small',
        'touches-boundary',
        'boundary-fallback',
        'renderer-fallback')]
    [string]$ExpectedDecisionReason = 'applied',

    [ValidateRange(1000, 15000)]
    [int]$ReadyTimeoutMilliseconds = 5000,

    [ValidateRange(45500, 120000)]
    [int]$ProcessTimeoutMilliseconds = 55000,

    [ValidateSet(0, 2)]
    [int]$DiagnosticBlocks = 0,

    [switch]$CaptureNvidiaTelemetry,

    [string]$NvidiaSmiExecutable = 'nvidia-smi.exe'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$hostName = 'ba-click-fx-desktop.exe'
$configName = 'BAFX.config.json'
$logName = 'ba-click-fx-desktop-support.log'
$manifestName = 'capture.json'
$manifestSchemaVersion = 4
$diagnosticManifestSchemaVersion = 3
$supportedConfigSchemaVersions = @(19, 20)
$environmentContract = 'rtx-4060-4k170-sdr-v1'
$requiredAdapterNameFragment = 'RTX 4060'
$requiredOutputWidth = 3840
$requiredOutputHeight = 2160
$requiredRefreshRateNumerator = 170
$requiredRefreshRateDenominator = 1
$blockCount = 5
$runCount = 20
$diagnosticBlockCount = 2
$diagnosticRunCount = 8
$warmupMilliseconds = 5000
$sampleMilliseconds = 30000
# Process lifetime starts before renderer initialization, while performance
# intervals start afterwards. Leave enough bounded tail for four complete
# windows even when adapter discovery or shader setup takes more than 500 ms.
$hostDurationMilliseconds = 45000
$performanceIntervalMilliseconds = 10000
$discardCompleteIntervals = 1
$selectCompleteIntervals = 3
$demoAgeMilliseconds = 130
$nvidiaTelemetryIntervalMilliseconds = 200
$nvidiaTelemetryCoverageMinimumSlackMilliseconds = 2000
$nvidiaTelemetryCoverageIntervalMultiplier = 10
$nvidiaTelemetryMinimumSampleIntervalMultiplier = 2
# Five one-second samples reject sustained gross load while allowing one-off
# scheduler spikes. This is a collection-integrity guard, not proof that GPU,
# storage, DPC, or individual cores are idle.
$hostIdleSampleCount = 5
$hostIdleSampleMilliseconds = 1000
$hostIdleMaximumBusyPercent = 10.0
$nvidiaTelemetryFields = @(
    'timestamp',
    'index',
    'uuid',
    'name',
    'pstate',
    'clocks.current.sm',
    'clocks.current.memory',
    'power.draw.instant',
    'temperature.gpu',
    'utilization.gpu',
    'utilization.memory')
$scenarioWorkloads = [ordered]@{
    'center-click' = 'fixed-age-center-click'
    'interior-trail' = 'fixed-age-interior-trail'
    'boundary-top-left' = 'fixed-age-boundary-top-left'
}
$capabilities = [ordered]@{
    'center-click' = [ordered]@{
        supported = $true
        driver = 'host-demo-click-fixed-age-v1'
        failureCode = $null
    }
    'interior-trail' = [ordered]@{
        supported = $true
        driver = 'host-demo-interior-trail-fixed-age-v1'
        failureCode = $null
    }
    'boundary-top-left' = [ordered]@{
        supported = $true
        driver = 'host-demo-boundary-top-left-fixed-age-v1'
        failureCode = $null
    }
}

if ($null -eq ('Bafx.Tools.CapturePowerRequest' -as [type]))
{
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace Bafx.Tools
{
    public static class CapturePowerRequest
    {
        [StructLayout(LayoutKind.Sequential)]
        private struct ReasonContext
        {
            public uint Version;
            public uint Flags;
            public IntPtr SimpleReasonString;
        }

        private enum PowerRequestType
        {
            DisplayRequired = 0,
            SystemRequired = 1
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr PowerCreateRequest(
            ref ReasonContext context);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool PowerSetRequest(
            IntPtr request,
            PowerRequestType type);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool PowerClearRequest(
            IntPtr request,
            PowerRequestType type);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CloseHandle(IntPtr handle);

        [DllImport("user32.dll")]
        private static extern void keybd_event(
            byte virtualKey,
            byte scanCode,
            uint flags,
            UIntPtr extraInfo);

        public static IntPtr AcquireAndWake(string reason)
        {
            IntPtr reasonText = Marshal.StringToHGlobalUni(reason);
            try
            {
                ReasonContext context = new ReasonContext
                {
                    Version = 0,
                    Flags = 1,
                    SimpleReasonString = reasonText
                };
                IntPtr request = PowerCreateRequest(ref context);
                if (request == IntPtr.Zero || request == new IntPtr(-1))
                {
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                }
                if (!PowerSetRequest(request, PowerRequestType.DisplayRequired))
                {
                    int error = Marshal.GetLastWin32Error();
                    CloseHandle(request);
                    throw new Win32Exception(error);
                }
                if (!PowerSetRequest(request, PowerRequestType.SystemRequired))
                {
                    int error = Marshal.GetLastWin32Error();
                    PowerClearRequest(
                        request,
                        PowerRequestType.DisplayRequired);
                    CloseHandle(request);
                    throw new Win32Exception(error);
                }

                // A power request prevents the next idle transition but does
                // not wake an already dark display. F24 has no product binding.
                keybd_event(0x87, 0, 0, UIntPtr.Zero);
                keybd_event(0x87, 0, 2, UIntPtr.Zero);
                return request;
            }
            finally
            {
                Marshal.FreeHGlobal(reasonText);
            }
        }

        public static void Release(IntPtr request)
        {
            bool system = PowerClearRequest(
                request,
                PowerRequestType.SystemRequired);
            int systemError = system ? 0 : Marshal.GetLastWin32Error();
            bool display = PowerClearRequest(
                request,
                PowerRequestType.DisplayRequired);
            int displayError = display ? 0 : Marshal.GetLastWin32Error();
            bool closed = CloseHandle(request);
            int closeError = closed ? 0 : Marshal.GetLastWin32Error();
            if (!system || !display || !closed)
            {
                int error = systemError != 0
                    ? systemError
                    : displayError != 0
                        ? displayError
                        : closeError;
                throw new Win32Exception(error);
            }
        }
    }

    public sealed class SystemCpuTimeSnapshot
    {
        public ulong Idle { get; private set; }
        public ulong Kernel { get; private set; }
        public ulong User { get; private set; }

        public SystemCpuTimeSnapshot(ulong idle, ulong kernel, ulong user)
        {
            Idle = idle;
            Kernel = kernel;
            User = user;
        }
    }

    public static class SystemCpuTimes
    {
        [StructLayout(LayoutKind.Sequential)]
        private struct FileTime
        {
            public uint Low;
            public uint High;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetSystemTimes(
            out FileTime idle,
            out FileTime kernel,
            out FileTime user);

        private static ulong ToUInt64(FileTime value)
        {
            return ((ulong)value.High << 32) | value.Low;
        }

        public static SystemCpuTimeSnapshot Capture()
        {
            FileTime idle;
            FileTime kernel;
            FileTime user;
            if (!GetSystemTimes(out idle, out kernel, out user))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            return new SystemCpuTimeSnapshot(
                ToUInt64(idle),
                ToUInt64(kernel),
                ToUInt64(user));
        }
    }
}
'@
}

function Enable-CapturePowerRequest
{
    return [Bafx.Tools.CapturePowerRequest]::AcquireAndWake(
        'BAFX Active-FX ROI ABBA capture')
}

function Disable-CapturePowerRequest
{
    param(
        [Parameter(Mandatory = $true)]
        [IntPtr]$Request
    )

    [Bafx.Tools.CapturePowerRequest]::Release($Request)
}

function Get-SystemCpuTimeSnapshot
{
    $snapshot = [Bafx.Tools.SystemCpuTimes]::Capture()
    return [ordered]@{
        idle = [decimal]$snapshot.Idle
        kernel = [decimal]$snapshot.Kernel
        user = [decimal]$snapshot.User
    }
}

function ConvertFrom-SystemCpuTimeDelta
{
    param(
        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Before,

        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$After
    )

    foreach ($name in @('idle', 'kernel', 'user'))
    {
        if (-not $Before.Contains($name) -or -not $After.Contains($name))
        {
            throw "System CPU time snapshot is missing $name"
        }
        if ([decimal]$After[$name] -lt [decimal]$Before[$name])
        {
            throw "System CPU time counter regressed at $name"
        }
    }

    $idleDelta = [decimal]$After.idle - [decimal]$Before.idle
    $kernelDelta = [decimal]$After.kernel - [decimal]$Before.kernel
    $userDelta = [decimal]$After.user - [decimal]$Before.user
    # GetSystemTimes includes idle time in the kernel counter. Subtract it once
    # so the result represents aggregate busy time across all logical CPUs.
    $totalDelta = $kernelDelta + $userDelta
    if ($totalDelta -le 0)
    {
        throw 'System CPU time did not advance during the idle preflight sample'
    }
    if ($idleDelta -gt $totalDelta)
    {
        throw 'System CPU idle time exceeds total time during the idle preflight sample'
    }
    return [double](
        (($totalDelta - $idleDelta) * [decimal]100.0) / $totalDelta)
}

function Measure-SystemCpuBusySamples
{
    param(
        [Parameter(Mandatory = $true)]
        [ValidateRange(1, 9)]
        [int]$SampleCount,

        [Parameter(Mandatory = $true)]
        [ValidateRange(100, 5000)]
        [int]$SampleMilliseconds
    )

    $samples = [Collections.Generic.List[double]]::new()
    $before = Get-SystemCpuTimeSnapshot
    for ($index = 0; $index -lt $SampleCount; ++$index)
    {
        Start-Sleep -Milliseconds $SampleMilliseconds
        $after = Get-SystemCpuTimeSnapshot
        $samples.Add((ConvertFrom-SystemCpuTimeDelta -Before $before -After $after))
        $before = $after
    }
    return @($samples)
}

function Confirm-HostIdleSamples
{
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [double[]]$BusyPercentSamples,

        [Parameter(Mandatory = $true)]
        [ValidateRange(1, 9)]
        [int]$ExpectedSampleCount,

        [Parameter(Mandatory = $true)]
        [ValidateRange(0.0, 100.0)]
        [double]$MaximumBusyPercent,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    if ($BusyPercentSamples.Count -ne $ExpectedSampleCount)
    {
        throw "$Context host idle preflight produced $($BusyPercentSamples.Count) samples instead of $ExpectedSampleCount"
    }
    foreach ($sample in $BusyPercentSamples)
    {
        if ([double]::IsNaN($sample) -or
            [double]::IsInfinity($sample) -or
            $sample -lt 0.0 -or
            $sample -gt 100.0)
        {
            throw "$Context host idle preflight produced an invalid CPU busy sample"
        }
    }

    $orderedSamples = @($BusyPercentSamples | Sort-Object)
    $median = $orderedSamples[[int][Math]::Floor($orderedSamples.Count / 2)]
    if ($median -gt $MaximumBusyPercent)
    {
        $culture = [Globalization.CultureInfo]::InvariantCulture
        $formattedSamples = @(
            $BusyPercentSamples |
                ForEach-Object {
                    $_.ToString('F1', $culture) + '%'
                }) -join ', '
        throw "$Context requires an idle Host; sustained system CPU busy exceeded $($MaximumBusyPercent.ToString('F1', $culture))% (samples: $formattedSamples)"
    }
}

function Confirm-HostIdle
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $samples = @(Measure-SystemCpuBusySamples `
        -SampleCount $script:hostIdleSampleCount `
        -SampleMilliseconds $script:hostIdleSampleMilliseconds)
    Confirm-HostIdleSamples `
        -BusyPercentSamples $samples `
        -ExpectedSampleCount $script:hostIdleSampleCount `
        -MaximumBusyPercent $script:hostIdleMaximumBusyPercent `
        -Context $Context
    Write-Verbose (
        "$Context host idle preflight accepted system CPU busy samples: " +
        (($samples | ForEach-Object { '{0:F1}%' -f $_ }) -join ', '))
}

function Get-FullPath
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$BaseDirectory
    )

    if ([IO.Path]::IsPathRooted($Path))
    {
        return [IO.Path]::GetFullPath($Path)
    }
    return [IO.Path]::GetFullPath((Join-Path $BaseDirectory $Path))
}

function Write-Utf8Text
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Text
    )

    # PowerShell 5.1 writes a BOM by default, while product JSON is BOM-free.
    $encoding = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($Path, $Text, $encoding)
}

function Write-CaptureManifest
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Manifest
    )

    Write-Utf8Text `
        -Path $Path `
        -Text (($Manifest | ConvertTo-Json -Depth 20) + "`n")
}

function Get-StructuredEventBlocks
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf))
    {
        return @()
    }
    $text = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
    return @(
        [regex]::Split($text, '(?m)^---\s*$') |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    )
}

function ConvertFrom-StructuredEventBlock
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Block,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $event = [ordered]@{}
    $lineNumber = 0
    foreach ($rawLine in ($Block -split "`r?`n"))
    {
        ++$lineNumber
        $line = $rawLine.TrimEnd("`r")
        if ([string]::IsNullOrWhiteSpace($line))
        {
            continue
        }
        $separator = $line.IndexOf('=')
        if ($separator -le 0)
        {
            throw "$Context has a malformed field at line $lineNumber"
        }
        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        if ($event.Contains($name) -and $event[$name] -cne $value)
        {
            throw "$Context contains conflicting duplicate field $name"
        }
        # SupportReport intentionally repeats envelope fields with the same
        # value. Preserve that format while still rejecting ambiguous logs.
        $event[$name] = $value
    }
    return $event
}

function Get-OnlyStructuredEvent
{
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Blocks,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $matches = @(
        $Blocks |
            Where-Object { $_ -match "(?m)^Event.Name=$([regex]::Escape($Name))\r?$" }
    )
    if ($matches.Count -ne 1)
    {
        throw "$Path must contain exactly one $Name event; found $($matches.Count)"
    }
    return ConvertFrom-StructuredEventBlock `
        -Block $matches[0] `
        -Context "$Path $Name"
}

function Get-RequiredEventString
{
    param(
        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Event,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    if (-not $Event.Contains($Name) -or
        [string]::IsNullOrEmpty([string]$Event[$Name]))
    {
        throw "$Context requires non-empty field $Name"
    }
    return [string]$Event[$Name]
}

function Get-RequiredEventInteger
{
    param(
        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Event,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $text = Get-RequiredEventString `
        -Event $Event `
        -Name $Name `
        -Context $Context
    $value = 0
    if (-not [int]::TryParse($text, [ref]$value))
    {
        throw "$Context field $Name must be an integer"
    }
    return $value
}

function Get-RequiredEventInt64
{
    param(
        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Event,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $text = Get-RequiredEventString `
        -Event $Event `
        -Name $Name `
        -Context $Context
    $value = 0L
    if (-not [long]::TryParse($text, [ref]$value))
    {
        throw "$Context field $Name must be a 64-bit integer"
    }
    return $value
}

function Get-RequiredEventBoolean
{
    param(
        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Event,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $text = Get-RequiredEventString `
        -Event $Event `
        -Name $Name `
        -Context $Context
    if ($text -ceq 'true')
    {
        return $true
    }
    if ($text -ceq 'false')
    {
        return $false
    }
    throw "$Context field $Name must be true or false"
}

function Get-RequiredEventNumber
{
    param(
        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Event,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $text = Get-RequiredEventString `
        -Event $Event `
        -Name $Name `
        -Context $Context
    $value = 0.0
    if (-not [double]::TryParse(
            $text,
            [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$value) -or
        [double]::IsNaN($value) -or
        [double]::IsInfinity($value))
    {
        throw "$Context field $Name must be a finite number"
    }
    return $value
}

function Confirm-CausalMetricContract
{
    param(
        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Event,

        [Parameter(Mandatory = $true)]
        [string]$Prefix,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    if (-not (Get-RequiredEventBoolean `
            -Event $Event `
            -Name "$Prefix.Available" `
            -Context $Context))
    {
        throw "$Context metric $Prefix must be available"
    }
    $unit = Get-RequiredEventString `
        -Event $Event `
        -Name "$Prefix.Unit" `
        -Context $Context
    if ($unit -cne 'us')
    {
        throw "$Context metric $Prefix must use us"
    }
    $samples = Get-RequiredEventInteger `
        -Event $Event `
        -Name "$Prefix.Samples" `
        -Context $Context
    $recordedSamples = Get-RequiredEventInteger `
        -Event $Event `
        -Name "$Prefix.RecordedSamples" `
        -Context $Context
    $droppedSamples = Get-RequiredEventInteger `
        -Event $Event `
        -Name "$Prefix.DroppedSamples" `
        -Context $Context
    if ($samples -le 0)
    {
        throw "$Context metric $Prefix must contain samples"
    }
    if ($recordedSamples -ne $samples)
    {
        throw "$Context metric $Prefix recorded samples do not match samples"
    }
    if ($droppedSamples -ne 0)
    {
        throw "$Context metric $Prefix dropped samples must be zero"
    }

    $minimum = Get-RequiredEventNumber `
        -Event $Event `
        -Name "$Prefix.Min" `
        -Context $Context
    $average = Get-RequiredEventNumber `
        -Event $Event `
        -Name "$Prefix.Average" `
        -Context $Context
    $p50 = Get-RequiredEventNumber `
        -Event $Event `
        -Name "$Prefix.P50" `
        -Context $Context
    $p95 = Get-RequiredEventNumber `
        -Event $Event `
        -Name "$Prefix.P95" `
        -Context $Context
    $p99 = Get-RequiredEventNumber `
        -Event $Event `
        -Name "$Prefix.P99" `
        -Context $Context
    $maximum = Get-RequiredEventNumber `
        -Event $Event `
        -Name "$Prefix.Max" `
        -Context $Context
    if ($minimum -lt 0.0 -or
        $average -lt 0.0 -or
        $p50 -lt 0.0 -or
        $p95 -lt 0.0 -or
        $p99 -lt 0.0 -or
        $maximum -lt 0.0)
    {
        throw "$Context metric $Prefix values must be non-negative"
    }
    if ($minimum -gt $p50 -or
        $p50 -gt $p95 -or
        $p95 -gt $p99 -or
        $p99 -gt $maximum -or
        $average -lt $minimum -or
        $average -gt $maximum)
    {
        throw "$Context metric $Prefix distribution is inconsistent"
    }
    return [ordered]@{
        samples = $samples
        p50 = $p50
        p95 = $p95
        p99 = $p99
    }
}

function Confirm-CausalTimingIntervalContract
{
    param(
        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Event,

        [Parameter(Mandatory = $true)]
        [int]$FrameCount,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $prePresentSemantic = Get-RequiredEventString `
        -Event $Event `
        -Name 'Timing.PrePresentSemantic' `
        -Context $Context
    if ($prePresentSemantic -cne 'fx-render-return-to-Present-call-entry-including-roi-diagnostics-spout-gpu-query-end-and-readback')
    {
        throw "$Context PrePresent timing semantic differs from the diagnostic contract"
    }
    $waitSemantic = Get-RequiredEventString `
        -Event $Event `
        -Name 'Timing.FramePacingWaitSemantic' `
        -Context $Context
    if ($waitSemantic -cne 'owner-thread-qpc-around-waitForAnyFrameOpportunity-including-handle-prepoll-and-message-wait-excluding-wait-set-build-and-post-wake-work')
    {
        throw "$Context frame-pacing wait semantic differs from the diagnostic contract"
    }

    $prePresent = Confirm-CausalMetricContract `
        -Event $Event `
        -Prefix 'Cpu.PrePresent' `
        -Context $Context
    if ($prePresent.samples -ne $FrameCount)
    {
        throw "$Context Cpu.PrePresent samples do not match Window.FrameCount"
    }
    $wait = Confirm-CausalMetricContract `
        -Event $Event `
        -Prefix 'FramePacing.Wait' `
        -Context $Context

    [long]$wakeCount = 0
    foreach ($name in @(
            'FramePacing.FrameReadyWakes',
            'FramePacing.DeviceRemovedWakes',
            'FramePacing.CadenceWakes',
            'FramePacing.MessageWakes',
            'FramePacing.Timeouts',
            'FramePacing.Failures'))
    {
        $count = Get-RequiredEventInteger `
            -Event $Event `
            -Name $name `
            -Context $Context
        if ($count -lt 0)
        {
            throw "$Context field $name must be non-negative"
        }
        $wakeCount += $count
    }
    if ($wait.samples -ne $wakeCount)
    {
        throw "$Context FramePacing.Wait samples do not match wake count"
    }

    [long]$bucketCount = 0
    foreach ($name in @(
            'FramePacing.Wait.Lt100Us',
            'FramePacing.Wait.100To999Us',
            'FramePacing.Wait.1000To3999Us',
            'FramePacing.Wait.4000To7999Us',
            'FramePacing.Wait.Ge8000Us'))
    {
        $count = Get-RequiredEventInteger `
            -Event $Event `
            -Name $name `
            -Context $Context
        if ($count -lt 0)
        {
            throw "$Context field $name must be non-negative"
        }
        $bucketCount += $count
    }
    if ($wait.samples -ne $bucketCount)
    {
        throw "$Context FramePacing.Wait samples do not match bucket count"
    }
}

function New-EnvironmentIdentity
{
    param(
        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Support,

        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Configuration,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    return [ordered]@{
        productVersion = Get-RequiredEventString -Event $Support -Name 'Product.Version' -Context $Context
        driverType = Get-RequiredEventString -Event $Support -Name 'Graphics.DriverType' -Context $Context
        adapter = Get-RequiredEventString -Event $Support -Name 'Graphics.Adapter' -Context $Context
        adapterLuid = Get-RequiredEventString -Event $Support -Name 'Graphics.AdapterLuid' -Context $Context
        driverVersion = Get-RequiredEventString -Event $Support -Name 'Graphics.DriverVersion' -Context $Context
        hardwareFallback = Get-RequiredEventString -Event $Support -Name 'Graphics.HardwareFallback' -Context $Context
        primaryDisplay = Get-RequiredEventString -Event $Support -Name 'Display.Primary' -Context $Context
        primaryDpi = Get-RequiredEventInteger -Event $Support -Name 'Display.PrimaryDpi' -Context $Context
        refreshRateNumerator = Get-RequiredEventInteger -Event $Support -Name 'Display.RefreshRateNumerator' -Context $Context
        refreshRateDenominator = Get-RequiredEventInteger -Event $Support -Name 'Display.RefreshRateDenominator' -Context $Context
        outputWidth = Get-RequiredEventInteger -Event $Configuration -Name 'Output.Width' -Context $Context
        outputHeight = Get-RequiredEventInteger -Event $Configuration -Name 'Output.Height' -Context $Context
        hdrEnabled = Get-RequiredEventBoolean -Event $Configuration -Name 'Display.HdrEnabled' -Context $Context
        outputMapping = Get-RequiredEventString -Event $Support -Name 'Graphics.OutputMapping' -Context $Context
    }
}

function Confirm-RequiredEnvironment
{
    param(
        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Identity,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    if ($Identity.driverType -cne 'Hardware')
    {
        throw "$Context requires hardware D3D11"
    }
    if ($Identity.adapter.IndexOf(
            $script:requiredAdapterNameFragment,
            [StringComparison]::OrdinalIgnoreCase) -lt 0)
    {
        throw "$Context requires an adapter containing $script:requiredAdapterNameFragment"
    }
    if ($Identity.hardwareFallback -cne 'none')
    {
        throw "$Context hardware fallback invalidates the capture"
    }
    if ($Identity.outputWidth -ne $script:requiredOutputWidth -or
        $Identity.outputHeight -ne $script:requiredOutputHeight)
    {
        throw "$Context requires a 3840x2160 output"
    }
    if ($Identity.refreshRateNumerator -ne $script:requiredRefreshRateNumerator -or
        $Identity.refreshRateDenominator -ne $script:requiredRefreshRateDenominator)
    {
        throw "$Context requires refresh rate 170/1 Hz"
    }
    if ($Identity.hdrEnabled)
    {
        throw "$Context requires SDR with Display.HdrEnabled=false"
    }
    if ($Identity.outputMapping -cne 'conservative-sdr')
    {
        throw "$Context requires Graphics.OutputMapping=conservative-sdr"
    }
}

function Confirm-SameEnvironmentIdentity
{
    param(
        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Expected,

        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Actual,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    if ($Expected.Count -ne $Actual.Count)
    {
        throw "$Context environment identity field count changed"
    }
    foreach ($name in $Expected.Keys)
    {
        if (-not $Actual.Contains($name))
        {
            throw "$Context environment identity is missing $name"
        }
        if (-not [object]::Equals($Expected[$name], $Actual[$name]))
        {
            throw "$Context environment identity drifted at $name"
        }
    }
}

function Wait-ForHostReady
{
    param(
        [Parameter(Mandatory = $true)]
        [Diagnostics.Process]$Process,

        [Parameter(Mandatory = $true)]
        [string]$LogPath,

        [Parameter(Mandatory = $true)]
        [int]$TimeoutMilliseconds
    )

    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.ElapsedMilliseconds -lt $TimeoutMilliseconds)
    {
        if ($Process.HasExited)
        {
            throw "Host exited before becoming ready with code $($Process.ExitCode)"
        }
        $blocks = @(Get-StructuredEventBlocks -Path $LogPath)
        $startup = @(
            $blocks |
                Where-Object { $_ -match '(?m)^Event.Name=Process.Startup\r?$' }
        )
        $configuration = @(
            $blocks |
                Where-Object { $_ -match '(?m)^Event.Name=Configuration.Applied\r?$' }
        )
        if ($startup.Count -eq 1 -and $configuration.Count -eq 1)
        {
            return
        }
        Start-Sleep -Milliseconds 25
    }
    throw "Host did not become ready within $TimeoutMilliseconds ms"
}

function Stop-OwnedProcess
{
    param(
        [Parameter(Mandatory = $true)]
        [Diagnostics.Process]$Process
    )

    $processId = $Process.Id
    if (-not $Process.HasExited)
    {
        try
        {
            # Kill through the retained Process handle so PID reuse cannot
            # redirect cleanup to an unrelated process.
            $Process.Kill()
        }
        catch
        {
            if (-not $Process.HasExited)
            {
                throw "Could not stop owned process PID ${processId}: $($_.Exception.Message)"
            }
        }
    }

    try
    {
        $exited = $Process.WaitForExit(5000)
    }
    catch
    {
        throw "Could not wait for owned process PID ${processId}: $($_.Exception.Message)"
    }
    if (-not $exited -or -not $Process.HasExited)
    {
        throw "Owned process PID $processId did not exit within 5000 ms"
    }
}

function Join-RunFailureMessages
{
    param(
        [AllowNull()]
        [Management.Automation.ErrorRecord]$PrimaryFailure,

        [AllowEmptyCollection()]
        [string[]]$CleanupFailures = @()
    )

    $messages = [Collections.Generic.List[string]]::new()
    if ($null -ne $PrimaryFailure)
    {
        $messages.Add($PrimaryFailure.Exception.Message)
    }
    foreach ($failure in $CleanupFailures)
    {
        $messages.Add($failure)
    }
    if ($messages.Count -eq 0)
    {
        return $null
    }
    return $messages -join '; '
}

function Resolve-NvidiaSmiExecutable
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$BaseDirectory
    )

    $resolvedPath = $null
    if ([IO.Path]::IsPathRooted($Path) -or
        -not [string]::IsNullOrEmpty([IO.Path]::GetDirectoryName($Path)))
    {
        $resolvedPath = Get-FullPath -Path $Path -BaseDirectory $BaseDirectory
    }
    else
    {
        $command = Get-Command `
            -Name $Path `
            -CommandType Application `
            -ErrorAction Stop
        $resolvedPath = [IO.Path]::GetFullPath($command.Source)
    }
    if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf))
    {
        throw "NVIDIA telemetry executable is missing: $resolvedPath"
    }
    if (-not [string]::Equals(
            [IO.Path]::GetFileName($resolvedPath),
            'nvidia-smi.exe',
            [StringComparison]::OrdinalIgnoreCase))
    {
        throw 'NVIDIA telemetry executable must be named nvidia-smi.exe'
    }

    $versionInfo = (Get-Item -LiteralPath $resolvedPath).VersionInfo
    if (-not [string]::Equals(
            $versionInfo.CompanyName,
            'NVIDIA Corporation',
            [StringComparison]::OrdinalIgnoreCase) -or
        -not $versionInfo.FileDescription.StartsWith(
            'NVIDIA-SMI',
            [StringComparison]::OrdinalIgnoreCase))
    {
        # Name-only validation would allow an unrelated executable on PATH to
        # become part of otherwise trusted diagnostic evidence.
        throw 'NVIDIA telemetry executable has unexpected version metadata'
    }
    return $resolvedPath
}

function ConvertFrom-NvidiaTelemetryLine
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Line,

        [Parameter(Mandatory = $true)]
        [string[]]$Fields,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $values = @($Line -split ',')
    if ($values.Count -ne $Fields.Count)
    {
        throw "$Context must contain exactly $($Fields.Count) fields"
    }
    $row = [ordered]@{}
    for ($index = 0; $index -lt $Fields.Count; ++$index)
    {
        $value = $values[$index].Trim()
        if ([string]::IsNullOrEmpty($value))
        {
            throw "$Context field $($Fields[$index]) must not be empty"
        }
        $row[$Fields[$index]] = $value
    }

    $gpuIndex = 0
    if (-not [int]::TryParse($row.index, [ref]$gpuIndex) -or $gpuIndex -lt 0)
    {
        throw "$Context field index must be a non-negative integer"
    }
    if ($row.uuid -notmatch '^GPU-[0-9A-Fa-f-]+$')
    {
        throw "$Context field uuid must be a physical GPU UUID"
    }
    if ($row.pstate -notmatch '^P[0-9]+$')
    {
        throw "$Context field pstate must be an NVIDIA performance state"
    }
    if ($row.timestamp -notmatch '^\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2}\.\d{3}$')
    {
        throw "$Context field timestamp has an unexpected format"
    }
    foreach ($name in @(
            'clocks.current.sm',
            'clocks.current.memory',
            'power.draw.instant',
            'temperature.gpu',
            'utilization.gpu',
            'utilization.memory'))
    {
        $number = 0.0
        if (-not [double]::TryParse(
                $row[$name],
                [Globalization.NumberStyles]::Float,
                [Globalization.CultureInfo]::InvariantCulture,
                [ref]$number) -or
            [double]::IsNaN($number) -or
            [double]::IsInfinity($number))
        {
            throw "$Context field $name must be a finite number"
        }
    }
    return $row
}

function Confirm-NvidiaTelemetryCoverage
{
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$TimestampValues,

        [Parameter(Mandatory = $true)]
        [DateTime]$StartedAtUtc,

        [Parameter(Mandatory = $true)]
        [DateTime]$StoppedAtUtc,

        [Parameter(Mandatory = $true)]
        [int]$IntervalMilliseconds,

        [Parameter(Mandatory = $true)]
        [int]$MinimumSlackMilliseconds,

        [Parameter(Mandatory = $true)]
        [int]$SlackIntervalMultiplier,

        [Parameter(Mandatory = $true)]
        [int]$MinimumSampleIntervalMultiplier
    )

    if ($StartedAtUtc.Kind -ne [DateTimeKind]::Utc -or
        $StoppedAtUtc.Kind -ne [DateTimeKind]::Utc)
    {
        throw 'nvidia-smi telemetry session bounds must be UTC'
    }
    if ($IntervalMilliseconds -le 0 -or
        $MinimumSlackMilliseconds -lt 0 -or
        $SlackIntervalMultiplier -le 0 -or
        $MinimumSampleIntervalMultiplier -le 0)
    {
        throw 'nvidia-smi telemetry coverage settings must be positive'
    }

    $elapsedMilliseconds = ($StoppedAtUtc - $StartedAtUtc).TotalMilliseconds
    if ($elapsedMilliseconds -le 0.0)
    {
        throw 'nvidia-smi telemetry session must have positive elapsed time'
    }
    $minimumSamples = [Math]::Max(
        2,
        [int][Math]::Floor(
            $elapsedMilliseconds /
                ($IntervalMilliseconds * $MinimumSampleIntervalMultiplier)))
    if ($TimestampValues.Count -lt $minimumSamples)
    {
        throw "nvidia-smi telemetry produced $($TimestampValues.Count) samples; at least $minimumSamples are required"
    }

    $timestampsUtc = [Collections.Generic.List[DateTime]]::new()
    foreach ($value in $TimestampValues)
    {
        $localTimestamp = [DateTime]::MinValue
        if (-not [DateTime]::TryParseExact(
                $value,
                'yyyy/MM/dd HH:mm:ss.fff',
                [Globalization.CultureInfo]::InvariantCulture,
                [Globalization.DateTimeStyles]::AssumeLocal,
                [ref]$localTimestamp))
        {
            throw "nvidia-smi telemetry timestamp is invalid: $value"
        }
        $timestampsUtc.Add($localTimestamp.ToUniversalTime())
    }

    $slackMilliseconds = [Math]::Max(
        $MinimumSlackMilliseconds,
        $IntervalMilliseconds * $SlackIntervalMultiplier)
    $first = $timestampsUtc[0]
    $last = $timestampsUtc[$timestampsUtc.Count - 1]
    if ($first -lt $StartedAtUtc.AddMilliseconds(-$slackMilliseconds) -or
        $first -gt $StartedAtUtc.AddMilliseconds($slackMilliseconds))
    {
        throw 'nvidia-smi telemetry first sample is outside the session start tolerance'
    }
    if ($last -lt $StoppedAtUtc.AddMilliseconds(-$slackMilliseconds) -or
        $last -gt $StoppedAtUtc.AddMilliseconds($slackMilliseconds))
    {
        throw 'nvidia-smi telemetry last sample is outside the session stop tolerance'
    }

    for ($index = 1; $index -lt $timestampsUtc.Count; ++$index)
    {
        $gapMilliseconds = (
            $timestampsUtc[$index] - $timestampsUtc[$index - 1]
        ).TotalMilliseconds
        if ($gapMilliseconds -lt 0.0)
        {
            throw "nvidia-smi telemetry timestamp $($index + 1) moved backwards"
        }
        if ($gapMilliseconds -gt $slackMilliseconds)
        {
            throw "nvidia-smi telemetry gap before sample $($index + 1) exceeds $slackMilliseconds ms"
        }
    }

    $minimumSpanMilliseconds = [Math]::Max(
        0.0,
        $elapsedMilliseconds - (2.0 * $slackMilliseconds))
    $actualSpanMilliseconds = ($last - $first).TotalMilliseconds
    if ($actualSpanMilliseconds -lt $minimumSpanMilliseconds)
    {
        throw "nvidia-smi telemetry span is $actualSpanMilliseconds ms; at least $minimumSpanMilliseconds ms are required"
    }
}

function Get-NvidiaTelemetryContract
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExecutablePath
    )

    $query = $script:nvidiaTelemetryFields -join ','
    $output = @(
        & $ExecutablePath `
            "--query-gpu=$query" `
            '--format=csv,noheader,nounits' 2>&1)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0)
    {
        throw "nvidia-smi field validation failed with code ${exitCode}: $($output -join '; ')"
    }
    $lines = @(
        $output |
            ForEach-Object { [string]$_ } |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($lines.Count -eq 0)
    {
        throw 'nvidia-smi field validation returned no GPUs'
    }

    $rows = @()
    for ($index = 0; $index -lt $lines.Count; ++$index)
    {
        $rows += ConvertFrom-NvidiaTelemetryLine `
            -Line $lines[$index] `
            -Fields $script:nvidiaTelemetryFields `
            -Context "nvidia-smi validation row $($index + 1)"
    }
    $matching = @(
        $rows |
            Where-Object {
                $_.name.IndexOf(
                    $script:requiredAdapterNameFragment,
                    [StringComparison]::OrdinalIgnoreCase) -ge 0
            })
    if ($matching.Count -ne 1)
    {
        throw "nvidia-smi must expose exactly one $script:requiredAdapterNameFragment GPU; found $($matching.Count)"
    }

    $versionInfo = (Get-Item -LiteralPath $ExecutablePath).VersionInfo
    return [ordered]@{
        executablePath = $ExecutablePath
        gpu = [ordered]@{
            index = [int]$matching[0].index
            uuid = $matching[0].uuid
            name = $matching[0].name
        }
        evidence = [ordered]@{
            enabled = $true
            provider = 'nvidia-smi'
            intervalMs = $script:nvidiaTelemetryIntervalMilliseconds
            fields = @($script:nvidiaTelemetryFields)
            executable = [ordered]@{
                fileName = [IO.Path]::GetFileName($ExecutablePath)
                sha256 = (
                    Get-FileHash -LiteralPath $ExecutablePath -Algorithm SHA256
                ).Hash.ToLowerInvariant()
                companyName = $versionInfo.CompanyName
                fileDescription = $versionInfo.FileDescription
                productVersion = $versionInfo.ProductVersion
            }
            gpu = [ordered]@{
                index = [int]$matching[0].index
                uuid = $matching[0].uuid
                name = $matching[0].name
            }
        }
    }
}

function Start-NvidiaTelemetry
{
    param(
        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Contract,

        [Parameter(Mandatory = $true)]
        [string]$RunRoot
    )

    $rawPath = Join-Path $RunRoot 'nvidia-smi.raw.csv'
    $stderrPath = Join-Path $RunRoot 'nvidia-smi.stderr.txt'
    $query = $script:nvidiaTelemetryFields -join ','
    $arguments = @(
        "--id=$($Contract.gpu.uuid)",
        "--query-gpu=$query",
        '--format=csv,noheader,nounits',
        "--loop-ms=$script:nvidiaTelemetryIntervalMilliseconds")
    $startedAt = [DateTime]::UtcNow
    $process = Start-Process `
        -FilePath $Contract.executablePath `
        -ArgumentList $arguments `
        -WindowStyle Hidden `
        -RedirectStandardOutput $rawPath `
        -RedirectStandardError $stderrPath `
        -PassThru
    return [ordered]@{
        process = $process
        rawPath = $rawPath
        stderrPath = $stderrPath
        arguments = @($arguments)
        startedAtUtc = $startedAt
        gpu = $Contract.gpu
    }
}

function Stop-NvidiaTelemetry
{
    param(
        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Session
    )

    $exitedUnexpectedly = $Session.process.HasExited
    if (-not $exitedUnexpectedly)
    {
        # The process object is the ownership boundary; existing nvidia-smi
        # processes on the machine are never enumerated or terminated.
        Stop-OwnedProcess -Process $Session.process
    }
    $stoppedAt = [DateTime]::UtcNow

    $stderr = if (Test-Path -LiteralPath $Session.stderrPath -PathType Leaf)
    {
        [IO.File]::ReadAllText($Session.stderrPath, [Text.Encoding]::UTF8)
    }
    else
    {
        ''
    }
    if (-not [string]::IsNullOrWhiteSpace($stderr))
    {
        throw "nvidia-smi telemetry wrote stderr: $($stderr.Trim())"
    }
    if (-not (Test-Path -LiteralPath $Session.rawPath -PathType Leaf))
    {
        throw 'nvidia-smi telemetry did not create its CSV stream'
    }
    $lines = @(
        Get-Content -LiteralPath $Session.rawPath |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($lines.Count -eq 0)
    {
        throw 'nvidia-smi telemetry produced no samples'
    }
    $timestampValues = [Collections.Generic.List[string]]::new()
    for ($index = 0; $index -lt $lines.Count; ++$index)
    {
        $row = ConvertFrom-NvidiaTelemetryLine `
            -Line $lines[$index] `
            -Fields $script:nvidiaTelemetryFields `
            -Context "nvidia-smi sample $($index + 1)"
        if ($row.uuid -cne $Session.gpu.uuid -or
            [int]$row.index -ne [int]$Session.gpu.index -or
            $row.name -cne $Session.gpu.name)
        {
            throw "nvidia-smi sample $($index + 1) changed GPU identity"
        }
        $timestampValues.Add([string]$row.timestamp)
    }
    Confirm-NvidiaTelemetryCoverage `
        -TimestampValues $timestampValues `
        -StartedAtUtc $Session.startedAtUtc `
        -StoppedAtUtc $stoppedAt `
        -IntervalMilliseconds $script:nvidiaTelemetryIntervalMilliseconds `
        -MinimumSlackMilliseconds $script:nvidiaTelemetryCoverageMinimumSlackMilliseconds `
        -SlackIntervalMultiplier $script:nvidiaTelemetryCoverageIntervalMultiplier `
        -MinimumSampleIntervalMultiplier $script:nvidiaTelemetryMinimumSampleIntervalMultiplier

    $csvPath = Join-Path (Split-Path -Parent $Session.rawPath) 'nvidia-smi.csv'
    $csvLines = @($script:nvidiaTelemetryFields -join ',') + @($lines)
    Write-Utf8Text -Path $csvPath -Text (($csvLines -join "`n") + "`n")
    # The headerless stream is an implementation detail; the canonical file
    # carries stable field names that do not vary with driver unit labels.
    [IO.File]::Delete($Session.rawPath)
    if ($exitedUnexpectedly)
    {
        throw 'nvidia-smi telemetry exited before the collector stopped it'
    }
    return [ordered]@{
        fileName = [IO.Path]::GetFileName($csvPath)
        stderrFileName = [IO.Path]::GetFileName($Session.stderrPath)
        sha256 = (
            Get-FileHash -LiteralPath $csvPath -Algorithm SHA256
        ).Hash.ToLowerInvariant()
        samples = $lines.Count
        intervalMs = $script:nvidiaTelemetryIntervalMilliseconds
        arguments = @($Session.arguments)
        startedAtUtc = $Session.startedAtUtc.ToString(
            'yyyy-MM-ddTHH:mm:ss.fffZ')
        stoppedAtUtc = $stoppedAt.ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
        collectorStoppedProcess = $true
    }
}

function Confirm-DirtyPresentIntervalContract
{
    param(
        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Event,

        [Parameter(Mandatory = $true)]
        [ValidateSet('primary', 'recording-rebuild')]
        [string]$MeasurementPath,

        [Parameter(Mandatory = $true)]
        [bool]$RoiEnabled,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedDecisionReason,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $finalCompositePath = Get-RequiredEventString `
        -Event $Event `
        -Name 'ROI.FinalCompositePath' `
        -Context $Context
    $expectedFinalCompositePath = if ($RoiEnabled)
    {
        'dirty-present-verified-resolve-scissor-with-full-screen-fallback'
    }
    else
    {
        'full-screen'
    }
    if ($finalCompositePath -cne $expectedFinalCompositePath)
    {
        throw "$Context ROI final composite path mismatch"
    }

    $dirtyPresentFrames = Get-RequiredEventInteger `
        -Event $Event `
        -Name 'ROI.Present.DirtyFrames' `
        -Context $Context
    $dirtyPresentPixels = Get-RequiredEventInt64 `
        -Event $Event `
        -Name 'ROI.Present.DirtyPixels.Total' `
        -Context $Context
    if ($dirtyPresentFrames -lt 0 -or $dirtyPresentPixels -lt 0)
    {
        throw "$Context ROI dirty Present counters must be non-negative"
    }

    $expectsDirtyPresent = $RoiEnabled -and
        $MeasurementPath -eq 'primary' -and
        $ExpectedDecisionReason -eq 'applied'
    if ($expectsDirtyPresent)
    {
        $appliedFrames = Get-RequiredEventInteger `
            -Event $Event `
            -Name 'ROI.Primary.AppliedFrames' `
            -Context $Context
        $warmupFrames = Get-RequiredEventInteger `
            -Event $Event `
            -Name 'ROI.Primary.WarmupFrames' `
            -Context $Context
        $expectedDirtyFrames = $appliedFrames - $warmupFrames
        if ($expectedDirtyFrames -le 0 -or
            $dirtyPresentFrames -ne $expectedDirtyFrames)
        {
            throw "$Context ROI dirty Present frames do not match steady primary applied frames"
        }
        if ($dirtyPresentPixels -le 0)
        {
            throw "$Context ROI dirty Present pixels must be positive"
        }
        return
    }

    if ($dirtyPresentFrames -ne 0 -or $dirtyPresentPixels -ne 0)
    {
        throw "$Context unexpectedly used ROI dirty Present"
    }
}

function Confirm-RunLogContract
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [ValidateSet('primary', 'recording-rebuild')]
        [string]$MeasurementPath,

        [Parameter(Mandatory = $true)]
        [bool]$RoiEnabled,

        [Parameter(Mandatory = $true)]
        [int]$ConfigSchemaVersion,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedDecisionReason,

        [Parameter(Mandatory = $true)]
        [bool]$RequireCausalTiming
    )

    $blocks = @(Get-StructuredEventBlocks -Path $Path)
    $exited = @(
        $blocks |
            Where-Object { $_ -match '(?m)^Event.Name=Process.Exited\r?$' }
    )
    if ($exited.Count -ne 1)
    {
        throw "$Path must contain exactly one Process.Exited event"
    }
    $completeIntervals = @(
        $blocks |
            Where-Object {
                $_ -match '(?m)^Event.Name=Performance.Interval\r?$' -and
                $_ -match '(?m)^Window.Final=false\r?$'
            }
    )
    $expected = $script:discardCompleteIntervals +
        $script:selectCompleteIntervals
    if ($completeIntervals.Count -ne $expected)
    {
        throw "$Path must contain exactly $expected complete performance intervals; found $($completeIntervals.Count)"
    }
    $powerUnavailable = @(
        $blocks |
            Where-Object {
                $_ -match '(?m)^Event.Name=Display.Topology.Invalidated\r?$' -and
                $_ -match '(?m)^PowerUnavailable=true\r?$'
            }
    )
    if ($powerUnavailable.Count -ne 0)
    {
        throw "$Path observed unavailable display power during capture"
    }
    $selectedIntervals = @(
        $completeIntervals |
            Select-Object `
                -Skip $script:discardCompleteIntervals `
                -First $script:selectCompleteIntervals
    )
    $roiPrefix = if ($MeasurementPath -eq 'primary')
    {
        'ROI.Primary'
    }
    else
    {
        'ROI.RecordingRebuild'
    }
    for ($index = 0; $index -lt $selectedIntervals.Count; ++$index)
    {
        $context = "$Path selected interval $($index + 1)"
        $interval = ConvertFrom-StructuredEventBlock `
            -Block $selectedIntervals[$index] `
            -Context $context
        $loggedSchemaVersion = Get-RequiredEventInteger `
            -Event $interval `
            -Name 'Configuration.SchemaVersion' `
            -Context $context
        if ($loggedSchemaVersion -ne $ConfigSchemaVersion)
        {
            throw "$context config schema does not match base configuration"
        }
        $frameCount = Get-RequiredEventInteger `
            -Event $interval `
            -Name 'Window.FrameCount' `
            -Context $context
        if ($frameCount -le 0)
        {
            throw "$context has no presented frames"
        }
        $presentSamples = Get-RequiredEventInteger `
            -Event $interval `
            -Name 'Cpu.PresentCall.Samples' `
            -Context $context
        if ($presentSamples -ne $frameCount)
        {
            throw "$context Present samples do not match Window.FrameCount"
        }
        if ($RequireCausalTiming)
        {
            Confirm-CausalTimingIntervalContract `
                -Event $interval `
                -FrameCount $frameCount `
                -Context $context
        }
        $observedFrames = Get-RequiredEventInteger `
            -Event $interval `
            -Name "$roiPrefix.ObservedFrames" `
            -Context $context
        if ($observedFrames -ne $frameCount)
        {
            throw "$context ROI observed frames do not match Window.FrameCount"
        }
        Confirm-DirtyPresentIntervalContract `
            -Event $interval `
            -MeasurementPath $MeasurementPath `
            -RoiEnabled $RoiEnabled `
            -ExpectedDecisionReason $ExpectedDecisionReason `
            -Context $context
    }
    $support = Get-OnlyStructuredEvent `
        -Blocks $blocks `
        -Name 'SupportReport' `
        -Path $Path
    $configuration = Get-OnlyStructuredEvent `
        -Blocks $blocks `
        -Name 'Configuration.Applied' `
        -Path $Path
    $identity = New-EnvironmentIdentity `
        -Support $support `
        -Configuration $configuration `
        -Context $Path
    Confirm-RequiredEnvironment -Identity $identity -Context $Path
    return $identity
}

function New-RunConfiguration
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$BaseConfiguration,

        [Parameter(Mandatory = $true)]
        [bool]$RoiEnabled
    )

    # Reparse for every run so no state from a previous arm can leak forward.
    $clone = $BaseConfiguration |
        ConvertTo-Json -Depth 20 |
        ConvertFrom-Json -ErrorAction Stop
    $clone.performance.activeFxRoiEnabled = $RoiEnabled
    return $clone
}

function Invoke-AbbaRun
{
    param(
        [Parameter(Mandatory = $true)]
        [int]$Ordinal,

        [Parameter(Mandatory = $true)]
        [int]$Block,

        [Parameter(Mandatory = $true)]
        [int]$Position,

        [Parameter(Mandatory = $true)]
        [string]$BlockPattern,

        [Parameter(Mandatory = $true)]
        [string]$Arm,

        [Parameter(Mandatory = $true)]
        [bool]$RoiEnabled,

        [Parameter(Mandatory = $true)]
        [object]$BaseConfiguration,

        [Parameter(Mandatory = $true)]
        [string]$SourceExecutable,

        [Parameter(Mandatory = $true)]
        [string]$ExecutableSha256,

        [Parameter(Mandatory = $true)]
        [string]$CaptureRoot,

        [Parameter(Mandatory = $true)]
        [string]$Scenario,

        [Parameter(Mandatory = $true)]
        [string]$MeasurementPath,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedDecisionReason,

        [Parameter(Mandatory = $true)]
        [int]$ReadyTimeoutMilliseconds,

        [Parameter(Mandatory = $true)]
        [int]$ProcessTimeoutMilliseconds,

        [Parameter(Mandatory = $true)]
        [bool]$DiagnosticMode,

        [AllowNull()]
        [Collections.IDictionary]$NvidiaTelemetryContract
    )

    $roiToken = if ($RoiEnabled) { 'on' } else { 'off' }
    $directoryName = 'run-{0:D2}-{1}-roi-{2}' -f (
        $Ordinal,
        $Arm.ToLowerInvariant(),
        $roiToken)
    $runRoot = Join-Path $CaptureRoot $directoryName
    $null = New-Item -ItemType Directory -Path $runRoot
    $runExecutable = Join-Path $runRoot $script:hostName
    $runConfig = Join-Path $runRoot $script:configName
    $runLog = Join-Path $runRoot $script:logName
    Copy-Item -LiteralPath $SourceExecutable -Destination $runExecutable

    $configuration = New-RunConfiguration `
        -BaseConfiguration $BaseConfiguration `
        -RoiEnabled $RoiEnabled
    Write-Utf8Text `
        -Path $runConfig `
        -Text (($configuration | ConvertTo-Json -Depth 20) + "`n")
    $initialConfigSha256 = (
        Get-FileHash -LiteralPath $runConfig -Algorithm SHA256
    ).Hash.ToLowerInvariant()

    $arguments = @(
        "--demo-scenario=$Scenario",
        "--demo-age-ms=$script:demoAgeMilliseconds",
        "--demo-delay-ms=$script:warmupMilliseconds",
        '--disable-raw-input',
        "--quit-after-ms=$script:hostDurationMilliseconds"
    )
    if ($MeasurementPath -eq 'recording-rebuild')
    {
        # Recording-rebuild counters exist only while the Spout2 path is active.
        $arguments += '--spout2'
    }
    $startedAt = [DateTime]::UtcNow
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $process = $null
    $telemetrySession = $null
    $telemetryEvidence = $null
    $primaryFailure = $null
    $cleanupFailures = [Collections.Generic.List[string]]::new()
    try
    {
        if ($null -ne $NvidiaTelemetryContract)
        {
            $telemetrySession = Start-NvidiaTelemetry `
                -Contract $NvidiaTelemetryContract `
                -RunRoot $runRoot
        }
        $process = Start-Process `
            -FilePath $runExecutable `
            -ArgumentList $arguments `
            -WorkingDirectory $runRoot `
            -WindowStyle Hidden `
            -PassThru
        Wait-ForHostReady `
            -Process $process `
            -LogPath $runLog `
            -TimeoutMilliseconds $ReadyTimeoutMilliseconds
        if (-not $process.WaitForExit($ProcessTimeoutMilliseconds))
        {
            throw "Host exceeded the $ProcessTimeoutMilliseconds ms process timeout"
        }
        if ($process.ExitCode -ne 0)
        {
            throw "Host exited with code $($process.ExitCode)"
        }
    }
    catch
    {
        $primaryFailure = $_
    }
    if ($null -ne $primaryFailure -and $null -ne $process)
    {
        try
        {
            Stop-OwnedProcess -Process $process
        }
        catch
        {
            $cleanupFailures.Add(
                "Host cleanup failed: $($_.Exception.Message)")
        }
    }
    if ($null -ne $telemetrySession)
    {
        try
        {
            $telemetryEvidence = Stop-NvidiaTelemetry `
                -Session $telemetrySession
        }
        catch
        {
            $cleanupFailures.Add(
                "NVIDIA telemetry stop failed: $($_.Exception.Message)")
        }
    }
    $timer.Stop()

    $failure = Join-RunFailureMessages `
        -PrimaryFailure $primaryFailure `
        -CleanupFailures $cleanupFailures
    if ($null -ne $failure)
    {
        # A Host error is intentionally first; cleanup evidence must not hide
        # the failure that caused this run to abort.
        throw $failure
    }

    $finalConfigSha256 = (
        Get-FileHash -LiteralPath $runConfig -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    if ($finalConfigSha256 -ne $initialConfigSha256)
    {
        # Platform rewrites would make the two A/B arms incomparable.
        throw "$directoryName configuration changed while Host was running"
    }
    $environmentIdentity = Confirm-RunLogContract `
        -Path $runLog `
        -MeasurementPath $MeasurementPath `
        -RoiEnabled $RoiEnabled `
        -ConfigSchemaVersion ([int]$BaseConfiguration.schemaVersion) `
        -ExpectedDecisionReason $ExpectedDecisionReason `
        -RequireCausalTiming $DiagnosticMode
    $copiedExecutableSha256 = (
        Get-FileHash -LiteralPath $runExecutable -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    if ($copiedExecutableSha256 -ne $ExecutableSha256)
    {
        throw "$directoryName executable differs from the source executable"
    }
    $run = [ordered]@{
        ordinal = $Ordinal
        block = $Block
        position = $Position
        arm = $Arm
        roiEnabled = $RoiEnabled
        directory = $directoryName
        executable = "$directoryName/$script:hostName"
        config = "$directoryName/$script:configName"
        log = "$directoryName/$script:logName"
        arguments = @($arguments)
        startedAtUtc = $startedAt.ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
        elapsedMs = $timer.ElapsedMilliseconds
        exitCode = $process.ExitCode
        executableSha256 = $copiedExecutableSha256
        configSha256 = $initialConfigSha256
    }
    if ($DiagnosticMode)
    {
        $run['blockPattern'] = $BlockPattern
        if ($null -ne $telemetryEvidence)
        {
            $telemetryEvidence['file'] =
                "$directoryName/$($telemetryEvidence.fileName)"
            $telemetryEvidence['stderr'] =
                "$directoryName/$($telemetryEvidence.stderrFileName)"
            $telemetryEvidence.Remove('fileName')
            $telemetryEvidence.Remove('stderrFileName')
            $run['nvidiaTelemetry'] = $telemetryEvidence
        }
    }
    return [ordered]@{
        environmentIdentity = $environmentIdentity
        run = $run
    }
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$executablePath = Get-FullPath `
    -Path $Executable `
    -BaseDirectory $repositoryRoot
$configurationPath = Get-FullPath `
    -Path $Configuration `
    -BaseDirectory $repositoryRoot
$outputRoot = Get-FullPath `
    -Path $OutputDirectory `
    -BaseDirectory $repositoryRoot
if (Test-Path -LiteralPath $outputRoot)
{
    throw "Refusing to overwrite an existing capture path: $outputRoot"
}

$diagnosticMode = $DiagnosticBlocks -eq $diagnosticBlockCount
if ($CaptureNvidiaTelemetry -and -not $diagnosticMode)
{
    throw 'CaptureNvidiaTelemetry is restricted to -DiagnosticBlocks 2 non-release captures'
}
if ($PSBoundParameters.ContainsKey('NvidiaSmiExecutable') -and
    -not $CaptureNvidiaTelemetry)
{
    throw 'NvidiaSmiExecutable requires CaptureNvidiaTelemetry'
}
$activeManifestSchemaVersion = if ($diagnosticMode)
{
    $diagnosticManifestSchemaVersion
}
else
{
    $manifestSchemaVersion
}
$activeManifestKind = if ($diagnosticMode)
{
    'bafx-active-fx-roi-diagnostic-capture'
}
else
{
    'bafx-active-fx-roi-ab-capture'
}
$collectingStatus = if ($diagnosticMode)
{
    'diagnostic-collecting'
}
else
{
    'collecting'
}
$capturedStatus = if ($diagnosticMode)
{
    'diagnostic-captured'
}
else
{
    'captured'
}
$failedStatus = if ($diagnosticMode)
{
    'diagnostic-failed'
}
else
{
    'failed'
}
$activeBlockCount = if ($diagnosticMode)
{
    $diagnosticBlockCount
}
else
{
    $blockCount
}
$activeRunCount = if ($diagnosticMode)
{
    $diagnosticRunCount
}
else
{
    $runCount
}

if (-not $capabilities[$Scenario].supported)
{
    $null = New-Item -ItemType Directory -Path $outputRoot
    $unsupported = [ordered]@{
        schemaVersion = $activeManifestSchemaVersion
        kind = $activeManifestKind
        captureStatus = if ($diagnosticMode)
        {
            'diagnostic-unsupported'
        }
        else
        {
            'unsupported'
        }
        capturedAtUtc = [DateTime]::UtcNow.ToString(
            'yyyy-MM-ddTHH:mm:ss.fffZ')
        scenario = $Scenario
        capabilities = $capabilities
        failure = $capabilities[$Scenario].failureCode
    }
    if ($diagnosticMode)
    {
        $unsupported['releaseEligible'] = $false
    }
    Write-CaptureManifest `
        -Path (Join-Path $outputRoot $manifestName) `
        -Manifest $unsupported
    throw "Scenario $Scenario is unsupported: $($capabilities[$Scenario].failureCode)"
}

if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf))
{
    throw "Host executable is missing: $executablePath"
}
if ([IO.Path]::GetFileName($executablePath) -ne $hostName)
{
    throw "Host executable must be named $hostName"
}
if (-not (Test-Path -LiteralPath $configurationPath -PathType Leaf))
{
    throw "Base configuration is missing: $configurationPath"
}
if ($ProcessTimeoutMilliseconds -lt $hostDurationMilliseconds + 5000)
{
    throw 'ProcessTimeoutMilliseconds must exceed the workload by at least 5000 ms'
}

$configurationText = [IO.File]::ReadAllText(
    $configurationPath,
    [Text.Encoding]::UTF8)
try
{
    $baseConfiguration = $configurationText | ConvertFrom-Json -ErrorAction Stop
}
catch
{
    throw "Base configuration is not valid JSON: $($_.Exception.Message)"
}
if ($null -eq $baseConfiguration -or
    $baseConfiguration.schemaVersion -notin $supportedConfigSchemaVersions)
{
    throw 'Base configuration schemaVersion must be 19 or 20'
}
$configSchemaVersion = [int]$baseConfiguration.schemaVersion
if ($null -eq $baseConfiguration.performance -or
    $baseConfiguration.performance.activeFxRoiEnabled -isnot [bool])
{
    throw 'Base configuration must contain boolean performance.activeFxRoiEnabled'
}

$existingHosts = @(
    Get-Process -Name 'ba-click-fx-desktop' -ErrorAction SilentlyContinue)
if ($existingHosts.Count -ne 0)
{
    throw "Close the existing BAFX Host before capture: PID $($existingHosts.Id -join ', ')"
}

$revision = (& git.exe -C $repositoryRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $revision -notmatch '^[0-9a-f]{40}$')
{
    throw 'Could not resolve the source revision.'
}
$trackedChanges = @(
    & git.exe -C $repositoryRoot status --porcelain --untracked-files=no)
if ($LASTEXITCODE -ne 0)
{
    throw 'Could not inspect the working tree.'
}
if ($trackedChanges.Count -ne 0)
{
    throw 'ROI A/B collection requires no tracked working-tree changes.'
}

$executableSha256 = (
    Get-FileHash -LiteralPath $executablePath -Algorithm SHA256
).Hash.ToLowerInvariant()
$nvidiaTelemetryContract = $null
if ($CaptureNvidiaTelemetry)
{
    $nvidiaSmiPath = Resolve-NvidiaSmiExecutable `
        -Path $NvidiaSmiExecutable `
        -BaseDirectory $repositoryRoot
    $nvidiaTelemetryContract = Get-NvidiaTelemetryContract `
        -ExecutablePath $nvidiaSmiPath
}
Confirm-HostIdle -Context 'capture start'
$null = New-Item -ItemType Directory -Path $outputRoot
$baseConfigPath = Join-Path $outputRoot 'base-config.json'
Copy-Item -LiteralPath $configurationPath -Destination $baseConfigPath
$manifestPath = Join-Path $outputRoot $manifestName
$manifestDecisionReason = if ($ExpectedDecisionReason -eq 'touches-boundary')
{
    'boundary-fallback'
}
else
{
    $ExpectedDecisionReason
}
$expectation = if ($manifestDecisionReason -eq 'applied')
{
    'applied'
}
else
{
    'fallback'
}
$schedule = if ($diagnosticMode)
{
    [ordered]@{
        pattern = 'ABBA+BAAB'
        blockPatterns = @('ABBA', 'BAAB')
        a = 'roi-off'
        b = 'roi-on'
        blocks = $diagnosticBlockCount
        runs = $diagnosticRunCount
        warmupMs = $warmupMilliseconds
        sampleMs = $sampleMilliseconds
        hostDurationMs = $hostDurationMilliseconds
        performanceIntervalMs = $performanceIntervalMilliseconds
        discardCompleteIntervals = $discardCompleteIntervals
        selectCompleteIntervals = $selectCompleteIntervals
    }
}
else
{
    [ordered]@{
        pattern = 'ABBA'
        a = 'roi-off'
        b = 'roi-on'
        blocks = $blockCount
        runs = $runCount
        warmupMs = $warmupMilliseconds
        sampleMs = $sampleMilliseconds
        hostDurationMs = $hostDurationMilliseconds
        performanceIntervalMs = $performanceIntervalMilliseconds
        discardCompleteIntervals = $discardCompleteIntervals
        selectCompleteIntervals = $selectCompleteIntervals
    }
}
$manifest = [ordered]@{
    schemaVersion = $activeManifestSchemaVersion
    kind = $activeManifestKind
    captureStatus = $collectingStatus
    revision = $revision
    workingTreeDirty = $false
    capturedAtUtc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
    executable = [ordered]@{
        fileName = $hostName
        sha256 = $executableSha256
    }
    environment = [ordered]@{
        contract = $environmentContract
        identity = $null
    }
    configuration = [ordered]@{
        schemaVersion = $configSchemaVersion
        baseConfig = 'base-config.json'
        baseSha256 = (
            Get-FileHash -LiteralPath $baseConfigPath -Algorithm SHA256
        ).Hash.ToLowerInvariant()
        differenceContract = 'performance.activeFxRoiEnabled-only'
    }
    scenario = [ordered]@{
        id = $Scenario
        workload = $scenarioWorkloads[$Scenario]
        measurementPath = $MeasurementPath
        expectation = $expectation
        expectedDecisionReason = $manifestDecisionReason
    }
    capabilities = $capabilities
    schedule = $schedule
    runs = @()
}
if ($diagnosticMode)
{
    # A distinct envelope makes accidental submission to the release reporter
    # fail closed before any short-matrix statistics can be interpreted.
    $manifest['releaseEligible'] = $false
    $manifest['diagnosticNotice'] =
        'NON-RELEASE: short matrix for causal investigation only'
    $manifest['nvidiaTelemetry'] = if ($null -ne $nvidiaTelemetryContract)
    {
        $nvidiaTelemetryContract.evidence
    }
    else
    {
        [ordered]@{ enabled = $false }
    }
}
Write-CaptureManifest -Path $manifestPath -Manifest $manifest

$capturePowerRequest = [IntPtr]::Zero
try
{
    # Performance evidence is invalid once Windows stops presenting for display
    # power. The handle owns both requests across PowerShell thread switches.
    $capturePowerRequest = Enable-CapturePowerRequest
    $abbaPattern = @(
        [ordered]@{ arm = 'A'; enabled = $false },
        [ordered]@{ arm = 'B'; enabled = $true },
        [ordered]@{ arm = 'B'; enabled = $true },
        [ordered]@{ arm = 'A'; enabled = $false })
    $baabPattern = @(
        [ordered]@{ arm = 'B'; enabled = $true },
        [ordered]@{ arm = 'A'; enabled = $false },
        [ordered]@{ arm = 'A'; enabled = $false },
        [ordered]@{ arm = 'B'; enabled = $true })
    $ordinal = 0
    for ($block = 1; $block -le $activeBlockCount; ++$block)
    {
        # A block-level check catches workloads that begin during the long
        # matrix without inserting asymmetric delays inside an ABBA block.
        Confirm-HostIdle -Context "block $block"
        $blockPattern = if ($diagnosticMode -and $block -eq 2)
        {
            'BAAB'
        }
        else
        {
            'ABBA'
        }
        $pattern = if ($blockPattern -eq 'BAAB')
        {
            $baabPattern
        }
        else
        {
            $abbaPattern
        }
        for ($position = 1; $position -le $pattern.Count; ++$position)
        {
            ++$ordinal
            $entry = $pattern[$position - 1]
            $result = Invoke-AbbaRun `
                -Ordinal $ordinal `
                -Block $block `
                -Position $position `
                -BlockPattern $blockPattern `
                -Arm $entry.arm `
                -RoiEnabled $entry.enabled `
                -BaseConfiguration $baseConfiguration `
                -SourceExecutable $executablePath `
                -ExecutableSha256 $executableSha256 `
                -CaptureRoot $outputRoot `
                -Scenario $Scenario `
                -MeasurementPath $MeasurementPath `
                -ExpectedDecisionReason $ExpectedDecisionReason `
                -ReadyTimeoutMilliseconds $ReadyTimeoutMilliseconds `
                -ProcessTimeoutMilliseconds $ProcessTimeoutMilliseconds `
                -DiagnosticMode $diagnosticMode `
                -NvidiaTelemetryContract $nvidiaTelemetryContract
            if ($null -eq $manifest.environment.identity)
            {
                # The first raw log anchors the canonical identity in the manifest.
                $manifest.environment.identity = $result.environmentIdentity
            }
            else
            {
                Confirm-SameEnvironmentIdentity `
                    -Expected $manifest.environment.identity `
                    -Actual $result.environmentIdentity `
                    -Context "run $ordinal"
            }
            $manifest.runs += $result.run
            Write-CaptureManifest -Path $manifestPath -Manifest $manifest
        }
    }
    if ($manifest.runs.Count -ne $activeRunCount)
    {
        throw "Collector produced $($manifest.runs.Count) runs instead of $activeRunCount"
    }
    Disable-CapturePowerRequest -Request $capturePowerRequest
    $capturePowerRequest = [IntPtr]::Zero
    $manifest.captureStatus = $capturedStatus
    Write-CaptureManifest -Path $manifestPath -Manifest $manifest
}
catch
{
    $failure = $_.Exception.Message
    if ($capturePowerRequest -ne [IntPtr]::Zero)
    {
        try
        {
            Disable-CapturePowerRequest -Request $capturePowerRequest
            $capturePowerRequest = [IntPtr]::Zero
        }
        catch
        {
            $failure += "; power-request cleanup failed: $($_.Exception.Message)"
        }
    }
    $manifest.captureStatus = $failedStatus
    $manifest['failure'] = $failure
    Write-CaptureManifest -Path $manifestPath -Manifest $manifest
    throw $failure
}

if ($diagnosticMode)
{
    Write-Warning 'NON-RELEASE diagnostic capture completed; the release reporter must reject this manifest.'
    Write-Host "Active-FX ROI ABBA+BAAB diagnostic completed: $outputRoot"
}
else
{
    Write-Host "Active-FX ROI ABBA capture completed: $outputRoot"
    Write-Host "Validate with: python -B tools\report-active-fx-roi-ab.py `"$outputRoot`""
}
