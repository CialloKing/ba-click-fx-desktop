[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [ValidateRange(10000, 60000)]
    [int]$DurationMilliseconds = 10500,

    [ValidateRange(1, 100000)]
    [int]$MessageCount = 705,

    [ValidateRange(1000, 15000)]
    [int]$ReadyTimeoutMilliseconds = 5000,

    [ValidateRange(15000, 120000)]
    [int]$ProcessTimeoutMilliseconds = 30000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$hostName = 'ba-click-fx-desktop.exe'
$configName = 'BAFX.config.json'
$logName = 'ba-click-fx-desktop-support.log'
$scenarioId = 'p0-static-click-message-pressure-v3'
$demoAgeMilliseconds = 130
$demoDelayMilliseconds = 50
$messageBatchSize = 5
$messageBatchIntervalMilliseconds = 25
$pressureMessageId = 0x80B0

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

    # Host configuration and evidence use UTF-8 without a PowerShell 5.1 BOM.
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

    $json = $Manifest | ConvertTo-Json -Depth 10
    Write-Utf8Text -Path $Path -Text ($json + "`n")
}

function New-BaselineConfiguration
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$BackgroundMode
    )

    return [ordered]@{
        background = [ordered]@{
            allowSystemBorder = $true
            cursorExcluded = $true
            mode = $BackgroundMode
        }
        effects = [ordered]@{
            bloomIntensity = 1
            bloomQuality = 'high'
            clickEnabled = $true
            enabled = $true
            globalScale = 1
            trailEnabled = $true
            trailLength = 1
            trailWidth = 1
        }
        input = [ordered]@{
            leftClick = $true
            middleClick = $false
            rightClick = $true
            samplingRateHz = 0
            trailOnlyWhilePressed = $true
        }
        performance = [ordered]@{
            framePacing = 'match-display'
            idleOptimization = $true
        }
        schemaVersion = 7
        system = [ordered]@{
            closeToTray = $true
            startMinimized = $false
            startWithWindows = $false
        }
    }
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
        $startup = $blocks |
            Where-Object { $_ -match '(?m)^Event.Name=Process.Startup\r?$' } |
            Select-Object -First 1
        $configuration = $blocks |
            Where-Object { $_ -match '(?m)^Event.Name=Configuration.Applied\r?$' } |
            Select-Object -First 1
        if ($null -ne $startup -and $null -ne $configuration)
        {
            $thread = [regex]::Match(
                $startup,
                '(?m)^Event.ThreadId=(\d+)\r?$')
            if ($thread.Success)
            {
                return [uint32]$thread.Groups[1].Value
            }
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
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        $null = $Process.WaitForExit(5000)
    }
}

function Invoke-ModeCapture
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$BackgroundMode,

        [Parameter(Mandatory = $true)]
        [string]$SourceExecutable,

        [Parameter(Mandatory = $true)]
        [string]$CaptureRoot,

        [Parameter(Mandatory = $true)]
        [int]$DurationMilliseconds,

        [Parameter(Mandatory = $true)]
        [int]$MessageCount,

        [Parameter(Mandatory = $true)]
        [int]$ReadyTimeoutMilliseconds,

        [Parameter(Mandatory = $true)]
        [int]$ProcessTimeoutMilliseconds
    )

    $modeRoot = Join-Path $CaptureRoot $Name
    $null = New-Item -ItemType Directory -Path $modeRoot
    $modeExecutable = Join-Path $modeRoot $script:hostName
    $configPath = Join-Path $modeRoot $script:configName
    $logPath = Join-Path $modeRoot $script:logName
    Copy-Item -LiteralPath $SourceExecutable -Destination $modeExecutable

    $configuration = New-BaselineConfiguration -BackgroundMode $BackgroundMode
    Write-Utf8Text -Path $configPath -Text (
        ($configuration | ConvertTo-Json -Depth 10) + "`n")

    $arguments = @(
        "--demo-age-ms=$script:demoAgeMilliseconds",
        "--demo-delay-ms=$script:demoDelayMilliseconds",
        '--disable-raw-input',
        "--quit-after-ms=$DurationMilliseconds"
    )
    $startedAt = [DateTime]::UtcNow
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process `
        -FilePath $modeExecutable `
        -ArgumentList $arguments `
        -WorkingDirectory $modeRoot `
        -WindowStyle Hidden `
        -PassThru

    $postedMessages = 0
    try
    {
        $threadId = Wait-ForHostReady `
            -Process $process `
            -LogPath $logPath `
            -TimeoutMilliseconds $ReadyTimeoutMilliseconds
        for ($index = 0; $index -lt $MessageCount; ++$index)
        {
            $posted = [BafxPerformanceBaselineNativeMethods]::PostThreadMessage(
                $threadId,
                $script:pressureMessageId,
                [UIntPtr]::Zero,
                [IntPtr]::Zero)
            if (-not $posted)
            {
                $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
                throw "PostThreadMessage failed at $index with Win32 error $errorCode"
            }
            ++$postedMessages
            $batchComplete = ($index + 1) % $script:messageBatchSize -eq 0
            $moreMessagesRemain = $index + 1 -lt $MessageCount
            if ($batchComplete -and $moreMessagesRemain)
            {
                # Small batches avoid both PowerShell's sub-tick sleep drift
                # and the Host's bounded dispatch overflow path.
                Start-Sleep -Milliseconds $script:messageBatchIntervalMilliseconds
            }
        }

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
        Stop-OwnedProcess -Process $process
        throw
    }
    finally
    {
        $timer.Stop()
    }

    $events = @(Get-StructuredEventBlocks -Path $logPath)
    if (-not ($events -match '(?m)^Event.Name=Process.Exited\r?$'))
    {
        throw "$Name log has no Process.Exited event"
    }
    return [ordered]@{
        backgroundMode = $BackgroundMode
        commandLine = @($modeExecutable) + $arguments
        startedAtUtc = $startedAt.ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
        elapsedMs = $timer.ElapsedMilliseconds
        threadId = $threadId
        messageId = ('0x{0:X4}' -f $script:pressureMessageId)
        postedMessages = $postedMessages
        exitCode = $process.ExitCode
    }
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$executablePath = Get-FullPath -Path $Executable -BaseDirectory $repositoryRoot
$outputRoot = Get-FullPath -Path $OutputDirectory -BaseDirectory $repositoryRoot
if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf))
{
    throw "Host executable is missing: $executablePath"
}
if ([IO.Path]::GetFileName($executablePath) -ne $hostName)
{
    throw "Host executable must be named $hostName"
}
if (Test-Path -LiteralPath $outputRoot)
{
    throw "Refusing to overwrite an existing capture path: $outputRoot"
}
if ($ProcessTimeoutMilliseconds -lt $DurationMilliseconds + 5000)
{
    throw 'ProcessTimeoutMilliseconds must exceed the workload by at least 5000 ms'
}

$existingHosts = @(Get-Process -Name 'ba-click-fx-desktop' -ErrorAction SilentlyContinue)
if ($existingHosts.Count -ne 0)
{
    throw "Close the existing BAFX Host before capture: PID $($existingHosts.Id -join ', ')"
}

$revision = (& git.exe -C $repositoryRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $revision -notmatch '^[0-9a-f]{40}$')
{
    throw 'Could not resolve the source revision.'
}
$trackedChanges = @(& git.exe -C $repositoryRoot status --porcelain --untracked-files=all)
if ($LASTEXITCODE -ne 0)
{
    throw 'Could not inspect the working tree.'
}
if ($trackedChanges.Count -ne 0)
{
    throw 'Official baseline collection requires a clean working tree.'
}

if ($null -eq ('BafxPerformanceBaselineNativeMethods' -as [type]))
{
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class BafxPerformanceBaselineNativeMethods
{
    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PostThreadMessage(
        uint threadId,
        uint message,
        UIntPtr wParam,
        IntPtr lParam);
}
'@
}

$null = New-Item -ItemType Directory -Path $outputRoot
$manifestPath = Join-Path $outputRoot 'capture.json'
$manifest = [ordered]@{
    schemaVersion = 1
    scenarioId = $scenarioId
    captureStatus = 'collecting'
    revision = $revision
    workingTreeDirty = $false
    executableSha256 = (
        Get-FileHash -LiteralPath $executablePath -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    capturedAtUtc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
    durationMs = $DurationMilliseconds
    demoAgeMs = $demoAgeMilliseconds
    demoDelayMs = $demoDelayMilliseconds
    messageCount = $MessageCount
    messageBatchSize = $messageBatchSize
    messageBatchIntervalMs = $messageBatchIntervalMilliseconds
    rawInputRegistration = 'disabled'
    readyTimeoutMs = $ReadyTimeoutMilliseconds
    processTimeoutMs = $ProcessTimeoutMilliseconds
    modes = [ordered]@{}
}
Write-CaptureManifest -Path $manifestPath -Manifest $manifest

try
{
    foreach ($mode in @(
            [ordered]@{ name = 'fx-only'; backgroundMode = 'recording-compatible' },
            [ordered]@{ name = 'background-aware'; backgroundMode = 'background-aware' }))
    {
        $manifest.modes[$mode.name] = Invoke-ModeCapture `
            -Name $mode.name `
            -BackgroundMode $mode.backgroundMode `
            -SourceExecutable $executablePath `
            -CaptureRoot $outputRoot `
            -DurationMilliseconds $DurationMilliseconds `
            -MessageCount $MessageCount `
            -ReadyTimeoutMilliseconds $ReadyTimeoutMilliseconds `
            -ProcessTimeoutMilliseconds $ProcessTimeoutMilliseconds
        Write-CaptureManifest -Path $manifestPath -Manifest $manifest
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

Write-Host "Paired performance capture completed: $outputRoot"
Write-Host "Validate with: python -B tools\report-performance-baseline.py `"$outputRoot`""
