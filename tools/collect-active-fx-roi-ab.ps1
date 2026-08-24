[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [Parameter(Mandatory = $true)]
    [string]$Configuration,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [ValidateSet('center-click', 'interior-trail')]
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
$manifestSchemaVersion = 1
$configSchemaVersion = 19
$blockCount = 5
$runCount = 20
$warmupMilliseconds = 5000
$sampleMilliseconds = 30000
$hostDurationMilliseconds = 40500
$performanceIntervalMilliseconds = 10000
$discardCompleteIntervals = 1
$selectCompleteIntervals = 3
$demoAgeMilliseconds = 130
$capabilities = [ordered]@{
    'center-click' = [ordered]@{
        supported = $true
        driver = 'host-demo-click-fixed-age-v1'
        failureCode = $null
    }
    'interior-trail' = [ordered]@{
        supported = $false
        driver = $null
        failureCode = 'host-has-no-deterministic-trail-driver'
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
        [string]$Path
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

    $arguments = @(
        "--demo-age-ms=$script:demoAgeMilliseconds",
        "--demo-delay-ms=$script:warmupMilliseconds",
        '--disable-raw-input',
        "--quit-after-ms=$script:hostDurationMilliseconds"
    )
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

    Confirm-RunLogContract -Path $runLog
    $copiedExecutableSha256 = (
        Get-FileHash -LiteralPath $runExecutable -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    if ($copiedExecutableSha256 -ne $ExecutableSha256)
    {
        throw "$directoryName executable differs from the source executable"
    }
    return [ordered]@{
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
        configSha256 = (
            Get-FileHash -LiteralPath $runConfig -Algorithm SHA256
        ).Hash.ToLowerInvariant()
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
        workload = 'fixed-age-center-click'
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

try
{
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
            $run = Invoke-AbbaRun `
                -Ordinal $ordinal `
                -Block $block `
                -Position $position `
                -Arm $entry.arm `
                -RoiEnabled $entry.enabled `
                -BaseConfiguration $baseConfiguration `
                -SourceExecutable $executablePath `
                -ExecutableSha256 $executableSha256 `
                -CaptureRoot $outputRoot `
                -ReadyTimeoutMilliseconds $ReadyTimeoutMilliseconds `
                -ProcessTimeoutMilliseconds $ProcessTimeoutMilliseconds
            $manifest.runs += $run
            Write-CaptureManifest -Path $manifestPath -Manifest $manifest
        }
    }
    if ($manifest.runs.Count -ne $runCount)
    {
        throw "Collector produced $($manifest.runs.Count) runs instead of $runCount"
    }
    $manifest.captureStatus = 'captured'
    Write-CaptureManifest -Path $manifestPath -Manifest $manifest
}
catch
{
    $manifest.captureStatus = 'failed'
    $manifest['failure'] = $_.Exception.Message
    Write-CaptureManifest -Path $manifestPath -Manifest $manifest
    throw
}

Write-Host "Active-FX ROI ABBA capture completed: $outputRoot"
Write-Host "Validate with: python -B tools\report-active-fx-roi-ab.py `"$outputRoot`""
