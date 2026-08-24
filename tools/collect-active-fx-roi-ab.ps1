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
    [int]$ProcessTimeoutMilliseconds = 55000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$hostName = 'ba-click-fx-desktop.exe'
$configName = 'BAFX.config.json'
$logName = 'ba-click-fx-desktop-support.log'
$manifestName = 'capture.json'
$manifestSchemaVersion = 3
$configSchemaVersion = 19
$environmentContract = 'rtx-4060-4k170-sdr-v1'
$requiredAdapterNameFragment = 'RTX 4060'
$requiredOutputWidth = 3840
$requiredOutputHeight = 2160
$requiredRefreshRateNumerator = 170
$requiredRefreshRateDenominator = 1
$blockCount = 5
$runCount = 20
$warmupMilliseconds = 5000
$sampleMilliseconds = 30000
$hostDurationMilliseconds = 40500
$performanceIntervalMilliseconds = 10000
$discardCompleteIntervals = 1
$selectCompleteIntervals = 3
$demoAgeMilliseconds = 130
$executionStateContinuous = [Convert]::ToUInt32('80000000', 16)
$executionStateSystemRequired = [uint32]0x00000001
$executionStateDisplayRequired = [uint32]0x00000002
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

if ($null -eq ('Bafx.Tools.CaptureExecutionState' -as [type]))
{
    Add-Type -TypeDefinition @'
using System.Runtime.InteropServices;

namespace Bafx.Tools
{
    public static class CaptureExecutionState
    {
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern uint SetThreadExecutionState(uint executionState);
    }
}
'@
}

function Enable-CaptureExecutionState
{
    $requested = $script:executionStateContinuous -bor
        $script:executionStateSystemRequired -bor
        $script:executionStateDisplayRequired
    $previous = [Bafx.Tools.CaptureExecutionState]::SetThreadExecutionState(
        $requested)
    if ($previous -eq 0)
    {
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "Could not keep the display awake for capture; error=$errorCode"
    }
}

function Disable-CaptureExecutionState
{
    $previous = [Bafx.Tools.CaptureExecutionState]::SetThreadExecutionState(
        $script:executionStateContinuous)
    if ($previous -eq 0)
    {
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "Could not release the capture execution state; error=$errorCode"
    }
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

    if (-not $Process.HasExited)
    {
        # Only the Process object started by this run is eligible for cleanup.
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        $null = $Process.WaitForExit(5000)
    }
}

function Confirm-RunLogContract
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [ValidateSet('primary', 'recording-rebuild')]
        [string]$MeasurementPath
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
        $observedFrames = Get-RequiredEventInteger `
            -Event $interval `
            -Name "$roiPrefix.ObservedFrames" `
            -Context $context
        if ($observedFrames -ne $frameCount)
        {
            throw "$context ROI observed frames do not match Window.FrameCount"
        }
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
        [int]$ReadyTimeoutMilliseconds,

        [Parameter(Mandatory = $true)]
        [int]$ProcessTimeoutMilliseconds
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
    try
    {
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
        if ($null -ne $process)
        {
            Stop-OwnedProcess -Process $process
        }
        throw
    }
    finally
    {
        $timer.Stop()
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
        -MeasurementPath $MeasurementPath
    $copiedExecutableSha256 = (
        Get-FileHash -LiteralPath $runExecutable -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    if ($copiedExecutableSha256 -ne $ExecutableSha256)
    {
        throw "$directoryName executable differs from the source executable"
    }
    return [ordered]@{
        environmentIdentity = $environmentIdentity
        run = [ordered]@{
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

if (-not $capabilities[$Scenario].supported)
{
    $null = New-Item -ItemType Directory -Path $outputRoot
    $unsupported = [ordered]@{
        schemaVersion = $manifestSchemaVersion
        kind = 'bafx-active-fx-roi-ab-capture'
        captureStatus = 'unsupported'
        capturedAtUtc = [DateTime]::UtcNow.ToString(
            'yyyy-MM-ddTHH:mm:ss.fffZ')
        scenario = $Scenario
        capabilities = $capabilities
        failure = $capabilities[$Scenario].failureCode
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
    $baseConfiguration.schemaVersion -ne $configSchemaVersion)
{
    throw "Base configuration schemaVersion must be $configSchemaVersion"
}
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
    throw 'Official ROI A/B collection requires no tracked working-tree changes.'
}

$executableSha256 = (
    Get-FileHash -LiteralPath $executablePath -Algorithm SHA256
).Hash.ToLowerInvariant()
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
$manifest = [ordered]@{
    schemaVersion = $manifestSchemaVersion
    kind = 'bafx-active-fx-roi-ab-capture'
    captureStatus = 'collecting'
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
    schedule = [ordered]@{
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
    runs = @()
}
Write-CaptureManifest -Path $manifestPath -Manifest $manifest

$captureExecutionStateHeld = $false
try
{
    # Performance evidence is invalid once Windows stops presenting for display
    # power. Keep both idle timers asserted for the complete ABBA transaction.
    Enable-CaptureExecutionState
    $captureExecutionStateHeld = $true
    $pattern = @(
        [ordered]@{ arm = 'A'; enabled = $false },
        [ordered]@{ arm = 'B'; enabled = $true },
        [ordered]@{ arm = 'B'; enabled = $true },
        [ordered]@{ arm = 'A'; enabled = $false })
    $ordinal = 0
    for ($block = 1; $block -le $blockCount; ++$block)
    {
        for ($position = 1; $position -le $pattern.Count; ++$position)
        {
            ++$ordinal
            $entry = $pattern[$position - 1]
            $result = Invoke-AbbaRun `
                -Ordinal $ordinal `
                -Block $block `
                -Position $position `
                -Arm $entry.arm `
                -RoiEnabled $entry.enabled `
                -BaseConfiguration $baseConfiguration `
                -SourceExecutable $executablePath `
                -ExecutableSha256 $executableSha256 `
                -CaptureRoot $outputRoot `
                -Scenario $Scenario `
                -MeasurementPath $MeasurementPath `
                -ReadyTimeoutMilliseconds $ReadyTimeoutMilliseconds `
                -ProcessTimeoutMilliseconds $ProcessTimeoutMilliseconds
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
    if ($manifest.runs.Count -ne $runCount)
    {
        throw "Collector produced $($manifest.runs.Count) runs instead of $runCount"
    }
    Disable-CaptureExecutionState
    $captureExecutionStateHeld = $false
    $manifest.captureStatus = 'captured'
    Write-CaptureManifest -Path $manifestPath -Manifest $manifest
}
catch
{
    $failure = $_.Exception.Message
    if ($captureExecutionStateHeld)
    {
        try
        {
            Disable-CaptureExecutionState
            $captureExecutionStateHeld = $false
        }
        catch
        {
            $failure += "; execution-state cleanup failed: $($_.Exception.Message)"
        }
    }
    $manifest.captureStatus = 'failed'
    $manifest['failure'] = $failure
    Write-CaptureManifest -Path $manifestPath -Manifest $manifest
    throw $failure
}

Write-Host "Active-FX ROI ABBA capture completed: $outputRoot"
Write-Host "Validate with: python -B tools\report-active-fx-roi-ab.py `"$outputRoot`""
