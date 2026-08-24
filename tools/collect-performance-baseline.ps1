[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [ValidateSet('paired-render-v1', 'p0-raw-input-down-v1')]
    [string]$Scenario = 'paired-render-v1',

    [ValidateRange(10000, 60000)]
    [int]$DurationMilliseconds = 10500,

    [ValidateRange(1, 100000)]
    [int]$MessageCount = 705,

    [ValidateRange(1000, 15000)]
    [int]$ReadyTimeoutMilliseconds = 5000,

    [ValidateRange(15000, 120000)]
    [int]$ProcessTimeoutMilliseconds = 30000,

    [ValidateRange(1, 32)]
    [int]$ClickCount = 6,

    [ValidateRange(10, 250)]
    [int]$ClickHoldMilliseconds = 30,

    [ValidateRange(100, 2000)]
    [int]$ClickIntervalMilliseconds = 300,

    [ValidateRange(100, 1000)]
    [int]$InputConfirmationTimeoutMilliseconds = 250,

    [ValidateRange(1000, 5000)]
    [int]$ReceiverReadyTimeoutMilliseconds = 2000,

    [ValidateRange(1000, 5000)]
    [int]$ReceiverStopTimeoutMilliseconds = 2000,

    [ValidateRange(100, 2000)]
    [int]$CursorRestoreTimeoutMilliseconds = 500
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$hostName = 'ba-click-fx-desktop.exe'
$configName = 'BAFX.config.json'
$logName = 'ba-click-fx-desktop-support.log'
$pairedRenderScenario = 'paired-render-v1'
$rawInputScenario = 'p0-raw-input-down-v1'
$scenarioId = if ($Scenario -eq $rawInputScenario)
{
    $rawInputScenario
}
else
{
    'p0-static-click-message-pressure-v3'
}
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
        display = [ordered]@{
            hdrEnabled = $false
            overrides = @()
        }
        effects = [ordered]@{
            bloomClamp = 65472
            bloomDiffusion = 7
            bloomIntensity = 1.7
            bloomLayerEnabled = $true
            bloomSoftKnee = 0
            bloomThreshold = 1
            clickEnabled = $true
            clickShardsLayerEnabled = $true
            clickTimeScale = 1
            diskLayerEnabled = $true
            diskLifetimeMs = 200
            diskRadius = 64.8
            enabled = $true
            globalScale = 1
            opacity = 1
            ringsAngularVelocityMultiplier = 11.170107
            ringsCount = 2
            ringsHdrIntensity = 5.992157
            ringsLayerEnabled = $true
            ringsLifetimeMs = 600
            ringsRadiusMax = 80.41333104
            ringsRadiusMin = 68.92571232
            ringsRotationDirection = -1
            shardsClickCount = 4
            shardsClickLifetimeMaxMs = 700
            shardsClickLifetimeMinMs = 600
            shardsClickRadius = 49.8769488
            shardsClickSpeedMax = 66.5025984
            shardsClickSpeedMin = 49.8769488
            shardsHdrIntensity = 5.992157
            shardsSizeMax = 33.2512992
            shardsSizeMin = 16.6256496
            themeColor = '#4ca7ff'
            trailEnabled = $true
            trailLayerEnabled = $true
            trailLength = 1
            trailLifetimeMs = 300
            trailOpacity = 1
            trailShardsLayerEnabled = $true
            trailTimeScale = 1
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
            activeFxRoiEnabled = $false
            effectsMode = 'full'
            framePacing = 'match-display'
            idleOptimization = $true
        }
        schemaVersion = 19
        system = [ordered]@{
            closeToTray = $true
            spout2Enabled = $false
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

function ConvertTo-PointEvidence
{
    param(
        [Parameter(Mandatory = $true)]
        [int]$X,

        [Parameter(Mandatory = $true)]
        [int]$Y
    )

    return [ordered]@{
        x = $X
        y = $Y
    }
}

function ConvertTo-RectangleEvidence
{
    param(
        [Parameter(Mandatory = $true)]
        [int]$Left,

        [Parameter(Mandatory = $true)]
        [int]$Top,

        [Parameter(Mandatory = $true)]
        [int]$Right,

        [Parameter(Mandatory = $true)]
        [int]$Bottom
    )

    return [ordered]@{
        left = $Left
        top = $Top
        right = $Right
        bottom = $Bottom
        width = $Right - $Left
        height = $Bottom - $Top
    }
}

function Wait-ForRawInputHostExit
{
    param(
        [Parameter(Mandatory = $true)]
        [Diagnostics.Process]$Process,

        [Parameter(Mandatory = $true)]
        [object]$Receiver,

        [Parameter(Mandatory = $true)]
        [int]$TimeoutMilliseconds
    )

    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.ElapsedMilliseconds -lt $TimeoutMilliseconds)
    {
        if ($Process.HasExited)
        {
            return
        }
        if (-not [string]::IsNullOrWhiteSpace($Receiver.Failure))
        {
            throw $Receiver.Failure
        }
        if (-not $Receiver.CursorUnchanged)
        {
            throw "Cursor moved during Raw Input capture to ($($Receiver.ObservedCursorX), $($Receiver.ObservedCursorY))"
        }
        Start-Sleep -Milliseconds 10
    }
    throw "Host exceeded the $TimeoutMilliseconds ms process timeout"
}

function Wait-RawInputInterval
{
    param(
        [Parameter(Mandatory = $true)]
        [Diagnostics.Process]$Process,

        [Parameter(Mandatory = $true)]
        [object]$Receiver,

        [Parameter(Mandatory = $true)]
        [int]$Milliseconds
    )

    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.ElapsedMilliseconds -lt $Milliseconds)
    {
        if ($Process.HasExited)
        {
            throw "Host exited during the Raw Input click sequence with code $($Process.ExitCode)"
        }
        if (-not [string]::IsNullOrWhiteSpace($Receiver.Failure))
        {
            throw $Receiver.Failure
        }
        if (-not $Receiver.CursorUnchanged)
        {
            throw "Cursor moved during Raw Input capture to ($($Receiver.ObservedCursorX), $($Receiver.ObservedCursorY))"
        }

        $remaining = $Milliseconds - $timer.ElapsedMilliseconds
        Start-Sleep -Milliseconds ([Math]::Min(10, [Math]::Max(1, $remaining)))
    }
}

function Wait-ForRawInputHostReady
{
    param(
        [Parameter(Mandatory = $true)]
        [Diagnostics.Process]$Process,

        [Parameter(Mandatory = $true)]
        [string]$LogPath,

        [Parameter(Mandatory = $true)]
        [object]$Receiver,

        [Parameter(Mandatory = $true)]
        [int]$TimeoutMilliseconds
    )

    $timer = [Diagnostics.Stopwatch]::StartNew()
    $nextLogPollMilliseconds = 0
    while ($timer.ElapsedMilliseconds -lt $TimeoutMilliseconds)
    {
        if ($Process.HasExited)
        {
            throw "Host exited before becoming ready with code $($Process.ExitCode)"
        }
        if (-not [string]::IsNullOrWhiteSpace($Receiver.Failure))
        {
            throw $Receiver.Failure
        }
        if (-not $Receiver.CursorUnchanged)
        {
            throw "Cursor moved while the Host was starting to ($($Receiver.ObservedCursorX), $($Receiver.ObservedCursorY))"
        }

        if ($timer.ElapsedMilliseconds -ge $nextLogPollMilliseconds)
        {
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
            $nextLogPollMilliseconds = $timer.ElapsedMilliseconds + 25
        }
        Start-Sleep -Milliseconds 10
    }
    throw "Host did not become ready within $TimeoutMilliseconds ms"
}

function Get-RequiredEventInteger
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Event,

        [Parameter(Mandatory = $true)]
        [string]$Field,

        [Parameter(Mandatory = $true)]
        [string]$Context
    )

    $match = [regex]::Match(
        $Event,
        "(?m)^$([regex]::Escape($Field))=(-?\d+)\r?$")
    if (-not $match.Success)
    {
        throw "$Context has no integer field $Field"
    }
    return [int64]$match.Groups[1].Value
}

function Confirm-RawInputPerformanceEvidence
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$LogPath,

        [Parameter(Mandatory = $true)]
        [int]$ClickCount
    )

    $events = @(Get-StructuredEventBlocks -Path $LogPath)
    $intervals = @(
        $events |
            Where-Object {
                $_ -match '(?m)^Event.Name=Performance.Interval\r?$' -and
                $_ -match '(?m)^Window.Final=false\r?$'
            }
    )
    if ($intervals.Count -ne 1)
    {
        throw "Raw Input log must contain one complete performance interval; found $($intervals.Count)"
    }

    $context = "Raw Input performance interval in $LogPath"
    $expectedEdges = 2 * $ClickCount
    $checks = [ordered]@{
        'Input.RawMessages' = $expectedEdges
        'Input.MoveEvents' = 0
        'Input.ButtonEdges' = $expectedEdges
        'Input.CancelEvents' = 0
        'Input.MessageTimeUnavailable' = 0
        'Input.DispatchToPresentReturn.Samples' = $ClickCount
        'Input.MessageToPresentReturn.Samples' = $ClickCount
    }
    $observed = [ordered]@{}
    foreach ($check in $checks.GetEnumerator())
    {
        $value = Get-RequiredEventInteger `
            -Event $intervals[0] `
            -Field $check.Key `
            -Context $context
        $observed[$check.Key] = $value
        if ($check.Key -eq 'Input.RawMessages' -and $value -eq 0)
        {
            throw "$context observed no WM_INPUT from SendInput; this environment does not support the raw-input capability probe and requires a real or virtual HID device"
        }
        if ($value -ne $check.Value)
        {
            throw "$context expected $($check.Key)=$($check.Value), observed $value"
        }
    }
    return $observed
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

function New-RawInputModeEvidence
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$BackgroundMode,

        [Parameter(Mandatory = $true)]
        [int]$ClickCount
    )

    return [ordered]@{
        backgroundMode = $BackgroundMode
        captureStatus = 'pending'
        failure = $null
        commandLine = @()
        startedAtUtc = $null
        elapsedMs = 0
        threadId = $null
        exitCode = $null
        targetRectangle = $null
        targetPoint = $null
        primaryWorkArea = $null
        cursorClipRectangle = $null
        originalCursor = $null
        restoredCursor = $null
        plannedSendInputCount = 2 * $ClickCount
        attemptedSendInputCount = 0
        acceptedSendInputCount = 0
        taggedDownCount = 0
        taggedUpCount = 0
        unexpectedButtonMessages = 0
        unexpectedMoveMessages = 0
        captureLossCount = 0
        emergencyUpCount = 0
        cursorRestored = $false
        captureReleased = $false
        receiverStopped = $false
        hostStarted = $false
        hostStopRequested = $false
        hostStopped = $false
        hostExitedNormally = $false
        cleanupSuccess = $false
        cleanupFailure = $null
        performanceEvidence = $null
    }
}

function Update-RawInputReceiverEvidence
{
    param(
        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$ModeEvidence,

        [Parameter(Mandatory = $true)]
        [object]$Receiver
    )

    $ModeEvidence.targetRectangle = ConvertTo-RectangleEvidence `
        -Left $Receiver.TargetLeft `
        -Top $Receiver.TargetTop `
        -Right $Receiver.TargetRight `
        -Bottom $Receiver.TargetBottom
    $ModeEvidence.targetPoint = ConvertTo-PointEvidence `
        -X $Receiver.TargetX `
        -Y $Receiver.TargetY
    $ModeEvidence.primaryWorkArea = ConvertTo-RectangleEvidence `
        -Left $Receiver.WorkLeft `
        -Top $Receiver.WorkTop `
        -Right $Receiver.WorkRight `
        -Bottom $Receiver.WorkBottom
    $ModeEvidence.cursorClipRectangle = ConvertTo-RectangleEvidence `
        -Left $Receiver.ClipLeft `
        -Top $Receiver.ClipTop `
        -Right $Receiver.ClipRight `
        -Bottom $Receiver.ClipBottom
    $ModeEvidence.originalCursor = ConvertTo-PointEvidence `
        -X $Receiver.OriginalCursorX `
        -Y $Receiver.OriginalCursorY
    $ModeEvidence.restoredCursor = ConvertTo-PointEvidence `
        -X $Receiver.RestoredCursorX `
        -Y $Receiver.RestoredCursorY
    $ModeEvidence.attemptedSendInputCount = $Receiver.AttemptedSendInputCount
    $ModeEvidence.acceptedSendInputCount = $Receiver.AcceptedSendInputCount
    $ModeEvidence.taggedDownCount = $Receiver.TaggedDownCount
    $ModeEvidence.taggedUpCount = $Receiver.TaggedUpCount
    $ModeEvidence.unexpectedButtonMessages = $Receiver.UnexpectedButtonMessages
    $ModeEvidence.unexpectedMoveMessages = $Receiver.UnexpectedMoveMessages
    $ModeEvidence.captureLossCount = $Receiver.CaptureLossCount
    $ModeEvidence.emergencyUpCount = $Receiver.EmergencyUpCount
    $ModeEvidence.cursorRestored = $Receiver.CursorRestored
    $ModeEvidence.captureReleased = $Receiver.CaptureReleased
    $ModeEvidence.receiverStopped = $Receiver.ReceiverStopped
}

function Invoke-RawInputModeCapture
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
        [int]$ReadyTimeoutMilliseconds,

        [Parameter(Mandatory = $true)]
        [int]$ProcessTimeoutMilliseconds,

        [Parameter(Mandatory = $true)]
        [int]$ClickCount,

        [Parameter(Mandatory = $true)]
        [int]$ClickHoldMilliseconds,

        [Parameter(Mandatory = $true)]
        [int]$ClickIntervalMilliseconds,

        [Parameter(Mandatory = $true)]
        [int]$InputConfirmationTimeoutMilliseconds,

        [Parameter(Mandatory = $true)]
        [int]$ReceiverReadyTimeoutMilliseconds,

        [Parameter(Mandatory = $true)]
        [int]$ReceiverStopTimeoutMilliseconds,

        [Parameter(Mandatory = $true)]
        [int]$CursorRestoreTimeoutMilliseconds,

        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$ModeEvidence
    )

    $modeRoot = Join-Path $CaptureRoot $Name
    $modeExecutable = Join-Path $modeRoot $script:hostName
    $configPath = Join-Path $modeRoot $script:configName
    $logPath = Join-Path $modeRoot $script:logName
    $arguments = @("--quit-after-ms=$DurationMilliseconds")
    $process = $null
    $receiver = $null
    $failure = $null
    $timer = [Diagnostics.Stopwatch]::StartNew()

    $ModeEvidence.captureStatus = 'preparing'
    $ModeEvidence.commandLine = @($modeExecutable) + $arguments
    $ModeEvidence.startedAtUtc = [DateTime]::UtcNow.ToString(
        'yyyy-MM-ddTHH:mm:ss.fffZ')
    try
    {
        $null = New-Item -ItemType Directory -Path $modeRoot
        Copy-Item -LiteralPath $SourceExecutable -Destination $modeExecutable
        $configuration = New-BaselineConfiguration -BackgroundMode $BackgroundMode
        Write-Utf8Text -Path $configPath -Text (
            ($configuration | ConvertTo-Json -Depth 10) + "`n")

        $receiver = [BafxRawInputReceiver]::new()
        Update-RawInputReceiverEvidence `
            -ModeEvidence $ModeEvidence `
            -Receiver $receiver
        $receiver.Start(
            $ReceiverReadyTimeoutMilliseconds,
            $InputConfirmationTimeoutMilliseconds,
            $CursorRestoreTimeoutMilliseconds)
        Update-RawInputReceiverEvidence `
            -ModeEvidence $ModeEvidence `
            -Receiver $receiver

        $process = Start-Process `
            -FilePath $modeExecutable `
            -ArgumentList $arguments `
            -WorkingDirectory $modeRoot `
            -WindowStyle Hidden `
            -PassThru
        $ModeEvidence.hostStarted = $true
        $ModeEvidence.captureStatus = 'running'
        $ModeEvidence.threadId = Wait-ForRawInputHostReady `
            -Process $process `
            -LogPath $logPath `
            -Receiver $receiver `
            -TimeoutMilliseconds $ReadyTimeoutMilliseconds

        for ($index = 0; $index -lt $ClickCount; ++$index)
        {
            $clickTimer = [Diagnostics.Stopwatch]::StartNew()
            $receiver.SendClick(
                $ClickHoldMilliseconds,
                $InputConfirmationTimeoutMilliseconds)
            if (-not [string]::IsNullOrWhiteSpace($receiver.Failure))
            {
                throw $receiver.Failure
            }

            if ($index + 1 -lt $ClickCount)
            {
                $remaining = $ClickIntervalMilliseconds -
                    [int]$clickTimer.ElapsedMilliseconds
                if ($remaining -gt 0)
                {
                    Wait-RawInputInterval `
                        -Process $process `
                        -Receiver $receiver `
                        -Milliseconds $remaining
                }
            }
        }

        Wait-ForRawInputHostExit `
            -Process $process `
            -Receiver $receiver `
            -TimeoutMilliseconds $ProcessTimeoutMilliseconds
        if ($process.ExitCode -ne 0)
        {
            throw "Host exited with code $($process.ExitCode)"
        }
        $ModeEvidence.hostExitedNormally = $true
        $ModeEvidence.exitCode = $process.ExitCode

        if (-not [string]::IsNullOrWhiteSpace($receiver.Failure))
        {
            throw $receiver.Failure
        }
        if (-not $receiver.CursorUnchanged)
        {
            throw "Cursor moved during Raw Input capture to ($($receiver.ObservedCursorX), $($receiver.ObservedCursorY))"
        }

        $events = @(Get-StructuredEventBlocks -Path $logPath)
        if (-not ($events -match '(?m)^Event.Name=Process.Exited\r?$'))
        {
            throw "$Name log has no Process.Exited event"
        }
        $ModeEvidence.performanceEvidence = Confirm-RawInputPerformanceEvidence `
            -LogPath $logPath `
            -ClickCount $ClickCount
        if ($receiver.TaggedDownCount -ne $ClickCount -or
            $receiver.TaggedUpCount -ne $ClickCount)
        {
            throw "Receiver expected $ClickCount tagged Down/Up messages; observed $($receiver.TaggedDownCount)/$($receiver.TaggedUpCount)"
        }
        $expectedSendInputCount = 2 * $ClickCount
        if ($receiver.AttemptedSendInputCount -ne $expectedSendInputCount -or
            $receiver.AcceptedSendInputCount -ne $expectedSendInputCount)
        {
            throw "Receiver expected $expectedSendInputCount accepted SendInput calls; attempted/accepted $($receiver.AttemptedSendInputCount)/$($receiver.AcceptedSendInputCount)"
        }
        if ($receiver.UnexpectedButtonMessages -ne 0 -or
            $receiver.UnexpectedMoveMessages -ne 0 -or
            $receiver.CaptureLossCount -ne 0 -or
            $receiver.EmergencyUpCount -ne 0)
        {
            throw 'Receiver observed unexpected input or capture cleanup during the workload'
        }
        $ModeEvidence.captureStatus = 'captured'
    }
    catch
    {
        $failure = $_.Exception
        $ModeEvidence.captureStatus = 'failed'
        $ModeEvidence.failure = $failure.Message
    }
    finally
    {
        $cleanupProblems = [Collections.Generic.List[string]]::new()
        if ($null -ne $process)
        {
            try
            {
                if (-not $process.HasExited)
                {
                    $ModeEvidence.hostStopRequested = $true
                    Stop-OwnedProcess -Process $process
                }
                $ModeEvidence.hostStopped = $process.HasExited
                if ($process.HasExited)
                {
                    $ModeEvidence.exitCode = $process.ExitCode
                }
            }
            catch
            {
                $cleanupProblems.Add("Host stop failed: $($_.Exception.Message)")
            }
        }
        else
        {
            $ModeEvidence.hostStopped = $true
        }

        if ($null -ne $receiver)
        {
            # Host is stopped before emergency input or cursor restoration so
            # failed cleanup cannot contaminate the latency evidence.
            try
            {
                if (-not $receiver.Cancel($InputConfirmationTimeoutMilliseconds) -and
                    -not $receiver.ReceiverStopped)
                {
                    $cleanupProblems.Add('Receiver cancel did not complete')
                }
            }
            catch
            {
                $cleanupProblems.Add("Receiver cancel failed: $($_.Exception.Message)")
            }
            try
            {
                if (-not $receiver.Stop($ReceiverStopTimeoutMilliseconds))
                {
                    $cleanupProblems.Add('Receiver thread did not stop')
                }
            }
            catch
            {
                $cleanupProblems.Add("Receiver stop failed: $($_.Exception.Message)")
            }
            if ($ModeEvidence.hostStopped)
            {
                try
                {
                    if (-not $receiver.RestoreCursor($CursorRestoreTimeoutMilliseconds))
                    {
                        $cleanupProblems.Add('Original cursor position was not restored')
                    }
                }
                catch
                {
                    $cleanupProblems.Add("Cursor restore failed: $($_.Exception.Message)")
                }
            }
            else
            {
                $cleanupProblems.Add(
                    'Cursor restore skipped because the Host is still running')
            }
            try
            {
                Update-RawInputReceiverEvidence `
                    -ModeEvidence $ModeEvidence `
                    -Receiver $receiver
            }
            catch
            {
                $cleanupProblems.Add("Receiver evidence update failed: $($_.Exception.Message)")
            }
        }
        $timer.Stop()
        $ModeEvidence.elapsedMs = $timer.ElapsedMilliseconds
        $ModeEvidence.cleanupSuccess =
            $ModeEvidence.hostStopped -and
            $ModeEvidence.captureReleased -and
            $ModeEvidence.receiverStopped -and
            $ModeEvidence.cursorRestored

        if (-not $ModeEvidence.cleanupSuccess -and $cleanupProblems.Count -eq 0)
        {
            $cleanupProblems.Add('Raw Input cleanup contract was not satisfied')
        }
        if ($cleanupProblems.Count -ne 0)
        {
            $ModeEvidence.cleanupFailure = $cleanupProblems -join '; '
        }
        if (-not $ModeEvidence.cleanupSuccess)
        {
            $ModeEvidence.captureStatus = 'failed'
            if ($null -eq $failure)
            {
                $failure = [InvalidOperationException]::new(
                    "$Name Raw Input cleanup did not complete: $($ModeEvidence.cleanupFailure)")
                $ModeEvidence.failure = $failure.Message
            }
        }
    }

    if ($null -ne $failure)
    {
        throw $failure
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
if ($Scenario -eq $rawInputScenario -and
    $InputConfirmationTimeoutMilliseconds -lt $ClickHoldMilliseconds + 25)
{
    throw 'InputConfirmationTimeoutMilliseconds must exceed ClickHoldMilliseconds by at least 25 ms'
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

if ($Scenario -eq $rawInputScenario -and
    $null -eq ('BafxRawInputReceiver' -as [type]))
{
    Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

public sealed class BafxRawInputReceiver
{
    private const uint WM_DESTROY = 0x0002;
    private const uint WM_PAINT = 0x000F;
    private const uint WM_CLOSE = 0x0010;
    private const uint WM_CANCELMODE = 0x001F;
    private const uint WM_SETCURSOR = 0x0020;
    private const uint WM_TIMER = 0x0113;
    private const uint WM_MOUSEMOVE = 0x0200;
    private const uint WM_LBUTTONDOWN = 0x0201;
    private const uint WM_LBUTTONUP = 0x0202;
    private const uint WM_RBUTTONDOWN = 0x0204;
    private const uint WM_RBUTTONUP = 0x0205;
    private const uint WM_MBUTTONDOWN = 0x0207;
    private const uint WM_MBUTTONUP = 0x0208;
    private const uint WM_XBUTTONDOWN = 0x020B;
    private const uint WM_XBUTTONUP = 0x020C;
    private const uint WM_CAPTURECHANGED = 0x0215;
    private const uint WM_NCHITTEST = 0x0084;
    private const uint WM_QUIT = 0x0012;
    private const uint WM_MOUSEACTIVATE = 0x0021;
    private const uint WM_APP_ARM = 0x8001;
    private const uint WM_APP_BEGIN_CLICK = 0x8002;
    private const uint WM_APP_CANCEL = 0x8003;

    private const uint WS_POPUP = 0x80000000;
    private const uint WS_EX_TOPMOST = 0x00000008;
    private const uint WS_EX_TOOLWINDOW = 0x00000080;
    private const uint WS_EX_NOACTIVATE = 0x08000000;
    private const int SW_SHOWNOACTIVATE = 4;
    private const uint SWP_NOACTIVATE = 0x0010;
    private const uint SWP_SHOWWINDOW = 0x0040;
    private const uint MONITOR_DEFAULTTOPRIMARY = 1;
    private const uint PM_NOREMOVE = 0x0000;
    private const uint PM_REMOVE = 0x0001;
    private const uint SMTO_BLOCK = 0x0001;
    private const uint SMTO_ABORTIFHUNG = 0x0002;
    private const uint SMTO_ERRORONEXIT = 0x0020;
    private const int MA_NOACTIVATE = 3;
    private const uint INPUT_MOUSE = 0;
    private const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    private const uint MOUSEEVENTF_LEFTUP = 0x0004;
    private const int VK_LBUTTON = 0x01;
    private const int VK_RBUTTON = 0x02;
    private const int VK_MBUTTON = 0x04;
    private const int VK_XBUTTON1 = 0x05;
    private const int VK_XBUTTON2 = 0x06;
    private const int IDC_ARROW = 32512;
    private static readonly IntPtr DpiAwarenessPerMonitorV2 = new IntPtr(-4);
    private const UInt64 InputMarker = 0xBAF00001UL;
    private const UInt64 MonitorTimerId = 1UL;
    private const UInt64 ClickTimerId = 2UL;

    private readonly object sync = new object();
    private readonly ManualResetEvent ready = new ManualResetEvent(false);
    private readonly ManualResetEvent clickCompleted = new ManualResetEvent(false);
    private readonly ManualResetEvent stopped = new ManualResetEvent(false);
    private readonly string className;
    private Thread windowThread;
    private WindowProcedure windowProcedure;
    private IntPtr windowHandle;
    private IntPtr backgroundBrush;
    private IntPtr moduleHandle;
    private uint windowThreadId;
    private string failure;
    private int started;
    private int armed;
    private int clickInProgress;
    private int clickSawDown;
    private int injectedDownActive;
    private int captureHeld;
    private int captureReleaseVerified = 1;
    private int cursorMoved;
    private int observedCursorX;
    private int observedCursorY;
    private int restoredCursorX;
    private int restoredCursorY;
    private int cursorRestored;
    private int attemptedSendInputCount;
    private int acceptedSendInputCount;
    private int taggedDownCount;
    private int taggedUpCount;
    private int unexpectedButtonMessages;
    private int unexpectedMoveMessages;
    private int captureLossCount;
    private int emergencyUpCount;

    public BafxRawInputReceiver()
    {
        // Query and store screen coordinates in the same PMv2 space used by
        // the receiver window, even when the PowerShell host is DPI-unaware.
        if (SetThreadDpiAwarenessContext(DpiAwarenessPerMonitorV2) == IntPtr.Zero)
        {
            ThrowLastError("SetThreadDpiAwarenessContext failed");
        }
        EnsureMouseButtonsReleased();

        POINT original;
        if (!GetCursorPos(out original))
        {
            ThrowLastError("GetCursorPos failed");
        }
        OriginalCursorX = original.X;
        OriginalCursorY = original.Y;
        restoredCursorX = original.X;
        restoredCursorY = original.Y;

        RECT clip;
        if (!GetClipCursor(out clip))
        {
            ThrowLastError("GetClipCursor failed");
        }
        ClipLeft = clip.Left;
        ClipTop = clip.Top;
        ClipRight = clip.Right;
        ClipBottom = clip.Bottom;

        MONITORINFO monitor = new MONITORINFO();
        monitor.Size = (uint)Marshal.SizeOf(typeof(MONITORINFO));
        IntPtr primary = MonitorFromPoint(new POINT(0, 0), MONITOR_DEFAULTTOPRIMARY);
        if (primary == IntPtr.Zero || !GetMonitorInfo(primary, ref monitor))
        {
            ThrowLastError("Could not query the primary monitor work area");
        }
        WorkLeft = monitor.Work.Left;
        WorkTop = monitor.Work.Top;
        WorkRight = monitor.Work.Right;
        WorkBottom = monitor.Work.Bottom;

        RECT targetBounds;
        if (!IntersectRect(out targetBounds, ref monitor.Work, ref clip) ||
            targetBounds.Right <= targetBounds.Left ||
            targetBounds.Bottom <= targetBounds.Top)
        {
            throw new InvalidOperationException(
                "The existing cursor clip rectangle does not intersect the primary work area");
        }

        TargetX = targetBounds.Left + (targetBounds.Right - targetBounds.Left) / 2;
        TargetY = targetBounds.Top + (targetBounds.Bottom - targetBounds.Top) / 2;
        int windowWidth = Math.Min(96, monitor.Work.Right - monitor.Work.Left);
        int windowHeight = Math.Min(96, monitor.Work.Bottom - monitor.Work.Top);
        if (windowWidth <= 0 || windowHeight <= 0)
        {
            throw new InvalidOperationException("The primary monitor work area is empty");
        }

        TargetLeft = Clamp(
            TargetX - windowWidth / 2,
            monitor.Work.Left,
            monitor.Work.Right - windowWidth);
        TargetTop = Clamp(
            TargetY - windowHeight / 2,
            monitor.Work.Top,
            monitor.Work.Bottom - windowHeight);
        TargetRight = TargetLeft + windowWidth;
        TargetBottom = TargetTop + windowHeight;
        observedCursorX = TargetX;
        observedCursorY = TargetY;
        className = "BAFX.RawInputReceiver." +
            Process.GetCurrentProcess().Id.ToString() + "." + Guid.NewGuid().ToString("N");
    }

    public int OriginalCursorX { get; private set; }
    public int OriginalCursorY { get; private set; }
    public int TargetX { get; private set; }
    public int TargetY { get; private set; }
    public int TargetLeft { get; private set; }
    public int TargetTop { get; private set; }
    public int TargetRight { get; private set; }
    public int TargetBottom { get; private set; }
    public int WorkLeft { get; private set; }
    public int WorkTop { get; private set; }
    public int WorkRight { get; private set; }
    public int WorkBottom { get; private set; }
    public int ClipLeft { get; private set; }
    public int ClipTop { get; private set; }
    public int ClipRight { get; private set; }
    public int ClipBottom { get; private set; }

    public string Failure
    {
        get
        {
            lock (sync)
            {
                return failure;
            }
        }
    }

    public bool CursorUnchanged
    {
        get { return Volatile.Read(ref cursorMoved) == 0; }
    }

    public int ObservedCursorX
    {
        get { return Volatile.Read(ref observedCursorX); }
    }

    public int ObservedCursorY
    {
        get { return Volatile.Read(ref observedCursorY); }
    }

    public int RestoredCursorX
    {
        get { return Volatile.Read(ref restoredCursorX); }
    }

    public int RestoredCursorY
    {
        get { return Volatile.Read(ref restoredCursorY); }
    }

    public bool CursorRestored
    {
        get { return Volatile.Read(ref cursorRestored) != 0; }
    }

    public bool CaptureHeld
    {
        get { return Volatile.Read(ref captureHeld) != 0; }
    }

    public bool CaptureReleased
    {
        get
        {
            return Volatile.Read(ref captureHeld) == 0 &&
                Volatile.Read(ref captureReleaseVerified) != 0;
        }
    }

    public bool ReceiverStopped
    {
        get { return stopped.WaitOne(0); }
    }

    public int AttemptedSendInputCount
    {
        get { return Volatile.Read(ref attemptedSendInputCount); }
    }

    public int AcceptedSendInputCount
    {
        get { return Volatile.Read(ref acceptedSendInputCount); }
    }

    public int TaggedDownCount
    {
        get { return Volatile.Read(ref taggedDownCount); }
    }

    public int TaggedUpCount
    {
        get { return Volatile.Read(ref taggedUpCount); }
    }

    public int UnexpectedButtonMessages
    {
        get { return Volatile.Read(ref unexpectedButtonMessages); }
    }

    public int UnexpectedMoveMessages
    {
        get { return Volatile.Read(ref unexpectedMoveMessages); }
    }

    public int CaptureLossCount
    {
        get { return Volatile.Read(ref captureLossCount); }
    }

    public int EmergencyUpCount
    {
        get { return Volatile.Read(ref emergencyUpCount); }
    }

    public void Start(
        int readyTimeoutMilliseconds,
        int controlTimeoutMilliseconds,
        int cursorTimeoutMilliseconds)
    {
        if (readyTimeoutMilliseconds <= 0 ||
            controlTimeoutMilliseconds <= 0 ||
            cursorTimeoutMilliseconds <= 0)
        {
            throw new ArgumentOutOfRangeException("Timeouts must be positive");
        }
        if (Interlocked.Exchange(ref started, 1) != 0)
        {
            throw new InvalidOperationException("Raw Input receiver was already started");
        }

        windowThread = new Thread(WindowThreadMain);
        windowThread.IsBackground = true;
        windowThread.Name = "BAFX Raw Input receiver";
        windowThread.SetApartmentState(ApartmentState.STA);
        windowThread.Start();
        if (!ready.WaitOne(readyTimeoutMilliseconds))
        {
            throw new TimeoutException(
                "Raw Input receiver window did not become ready within " +
                readyTimeoutMilliseconds.ToString() + " ms");
        }
        ThrowIfFailed();

        EnsureMouseButtonsReleased();
        if (!SetCursorPos(TargetX, TargetY))
        {
            ThrowLastError("SetCursorPos failed before Raw Input capture");
        }
        if (!WaitForCursor(TargetX, TargetY, cursorTimeoutMilliseconds))
        {
            throw new TimeoutException(
                "Cursor did not reach the Raw Input receiver target within " +
                cursorTimeoutMilliseconds.ToString() + " ms");
        }

        // Allow the positioning WM_MOUSEMOVE to reach the receiver before its
        // workload counters are armed.
        Thread.Sleep(Math.Min(50, controlTimeoutMilliseconds));
        EnsureMouseButtonsReleased();
        SendControlMessage(WM_APP_ARM, 0, controlTimeoutMilliseconds, true);
        ThrowIfFailed();
    }

    public void SendClick(int holdMilliseconds, int timeoutMilliseconds)
    {
        if (holdMilliseconds <= 0 || timeoutMilliseconds <= 0)
        {
            throw new ArgumentOutOfRangeException("Click timing must be positive");
        }
        ThrowIfFailed();
        if (!CursorUnchanged)
        {
            throw new InvalidOperationException("Cursor moved before the next click");
        }

        clickCompleted.Reset();
        Stopwatch timer = Stopwatch.StartNew();
        SendControlMessage(
            WM_APP_BEGIN_CLICK,
            holdMilliseconds,
            timeoutMilliseconds,
            true);
        int remaining = timeoutMilliseconds - (int)timer.ElapsedMilliseconds;
        if (remaining <= 0 || !clickCompleted.WaitOne(remaining))
        {
            Cancel(timeoutMilliseconds);
            throw new TimeoutException(
                "Injected click was not confirmed within " +
                timeoutMilliseconds.ToString() + " ms");
        }
        ThrowIfFailed();
    }

    public bool Cancel(int timeoutMilliseconds)
    {
        if (timeoutMilliseconds <= 0 || stopped.WaitOne(0))
        {
            return stopped.WaitOne(0);
        }

        try
        {
            return SendControlMessage(
                WM_APP_CANCEL,
                0,
                timeoutMilliseconds,
                false);
        }
        catch
        {
            return false;
        }
    }

    public bool Stop(int timeoutMilliseconds)
    {
        if (timeoutMilliseconds <= 0)
        {
            return stopped.WaitOne(0);
        }
        if (stopped.WaitOne(0))
        {
            return true;
        }

        Stopwatch timer = Stopwatch.StartNew();
        IntPtr handle = GetWindowHandle();
        if (handle != IntPtr.Zero)
        {
            IntPtr result;
            bool sent = SendMessageTimeout(
                handle,
                WM_CLOSE,
                UIntPtr.Zero,
                IntPtr.Zero,
                SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
                (uint)timeoutMilliseconds,
                out result) != IntPtr.Zero;
            if (!sent)
            {
                PostMessage(handle, WM_CLOSE, UIntPtr.Zero, IntPtr.Zero);
            }
        }
        else if (windowThreadId != 0)
        {
            PostThreadMessage(windowThreadId, WM_QUIT, UIntPtr.Zero, IntPtr.Zero);
        }

        int remaining = timeoutMilliseconds - (int)timer.ElapsedMilliseconds;
        if (remaining <= 0 || !stopped.WaitOne(remaining))
        {
            return false;
        }
        if (windowThread != null)
        {
            remaining = timeoutMilliseconds - (int)timer.ElapsedMilliseconds;
            if (remaining <= 0 || !windowThread.Join(remaining))
            {
                return false;
            }
        }
        return true;
    }

    public bool RestoreCursor(int timeoutMilliseconds)
    {
        if (timeoutMilliseconds <= 0 || !stopped.WaitOne(0))
        {
            return false;
        }

        try
        {
            EnsureMouseButtonsReleased();
            if (!SetCursorPos(OriginalCursorX, OriginalCursorY))
            {
                return false;
            }
            bool restored = WaitForCursor(
                OriginalCursorX,
                OriginalCursorY,
                timeoutMilliseconds);
            POINT observed;
            if (GetCursorPos(out observed))
            {
                Volatile.Write(ref restoredCursorX, observed.X);
                Volatile.Write(ref restoredCursorY, observed.Y);
            }
            Volatile.Write(ref cursorRestored, restored ? 1 : 0);
            return restored;
        }
        catch
        {
            return false;
        }
    }

    private void WindowThreadMain()
    {
        try
        {
            windowThreadId = GetCurrentThreadId();
            // Keep target coordinates in physical pixels on mixed-DPI systems.
            if (SetThreadDpiAwarenessContext(DpiAwarenessPerMonitorV2) == IntPtr.Zero)
            {
                ThrowLastError("SetThreadDpiAwarenessContext failed on receiver thread");
            }
            MSG seed;
            PeekMessage(out seed, IntPtr.Zero, 0, 0, PM_NOREMOVE);

            moduleHandle = GetModuleHandle(null);
            backgroundBrush = CreateSolidBrush(0x00B06030);
            if (backgroundBrush == IntPtr.Zero)
            {
                ThrowLastError("CreateSolidBrush failed");
            }

            windowProcedure = WindowProc;
            WNDCLASSEX windowClass = new WNDCLASSEX();
            windowClass.Size = (uint)Marshal.SizeOf(typeof(WNDCLASSEX));
            windowClass.WindowProcedure = windowProcedure;
            windowClass.Instance = moduleHandle;
            windowClass.Cursor = LoadCursor(IntPtr.Zero, new IntPtr(IDC_ARROW));
            windowClass.Background = backgroundBrush;
            windowClass.ClassName = className;
            if (RegisterClassEx(ref windowClass) == 0)
            {
                ThrowLastError("RegisterClassEx failed");
            }

            IntPtr handle = CreateWindowEx(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                className,
                "BAFX Raw Input Receiver",
                WS_POPUP,
                TargetLeft,
                TargetTop,
                TargetRight - TargetLeft,
                TargetBottom - TargetTop,
                IntPtr.Zero,
                IntPtr.Zero,
                moduleHandle,
                IntPtr.Zero);
            if (handle == IntPtr.Zero)
            {
                ThrowLastError("CreateWindowEx failed");
            }
            SetWindowHandle(handle);
            ShowWindow(handle, SW_SHOWNOACTIVATE);
            if (!SetWindowPos(
                    handle,
                    new IntPtr(-1),
                    TargetLeft,
                    TargetTop,
                    TargetRight - TargetLeft,
                    TargetBottom - TargetTop,
                    SWP_NOACTIVATE | SWP_SHOWWINDOW))
            {
                ThrowLastError("SetWindowPos failed");
            }
            UpdateWindow(handle);
            ready.Set();

            MSG message;
            int messageResult;
            while ((messageResult = GetMessage(
                    out message,
                    IntPtr.Zero,
                    0,
                    0)) > 0)
            {
                TranslateMessage(ref message);
                DispatchMessage(ref message);
            }
            if (messageResult < 0)
            {
                ThrowLastError("GetMessage failed");
            }
        }
        catch (Exception error)
        {
            SetFailure(error.Message);
            ready.Set();
            clickCompleted.Set();
        }
        finally
        {
            IntPtr handle = GetWindowHandle();
            if (handle != IntPtr.Zero)
            {
                ReleaseOwnedCapture(handle);
                if (IsWindow(handle))
                {
                    DestroyWindow(handle);
                }
                if (GetCapture() != handle)
                {
                    Volatile.Write(ref captureReleaseVerified, 1);
                }
            }
            SetWindowHandle(IntPtr.Zero);
            if (moduleHandle != IntPtr.Zero)
            {
                UnregisterClass(className, moduleHandle);
            }
            if (backgroundBrush != IntPtr.Zero)
            {
                DeleteObject(backgroundBrush);
                backgroundBrush = IntPtr.Zero;
            }
            stopped.Set();
        }
    }

    private IntPtr WindowProc(
        IntPtr window,
        uint message,
        UIntPtr wParam,
        IntPtr lParam)
    {
        try
        {
            switch (message)
            {
            case WM_MOUSEACTIVATE:
                return new IntPtr(MA_NOACTIVATE);

            case WM_NCHITTEST:
                return new IntPtr(1);

            case WM_SETCURSOR:
                SetCursor(LoadCursor(IntPtr.Zero, new IntPtr(IDC_ARROW)));
                return new IntPtr(1);

            case WM_PAINT:
                PAINTSTRUCT paint;
                IntPtr deviceContext = BeginPaint(window, out paint);
                if (deviceContext != IntPtr.Zero)
                {
                    RECT client;
                    if (GetClientRect(window, out client))
                    {
                        FillRect(deviceContext, ref client, backgroundBrush);
                    }
                }
                EndPaint(window, ref paint);
                return IntPtr.Zero;

            case WM_APP_ARM:
                return Arm(window) ? new IntPtr(1) : IntPtr.Zero;

            case WM_APP_BEGIN_CLICK:
                return BeginClick(window, unchecked((int)wParam.ToUInt64()))
                    ? new IntPtr(1)
                    : IntPtr.Zero;

            case WM_APP_CANCEL:
                Volatile.Write(ref armed, 0);
                KillTimer(window, new UIntPtr(MonitorTimerId));
                CancelActiveClick(window, null);
                return new IntPtr(1);

            case WM_TIMER:
                if (wParam.ToUInt64() == MonitorTimerId)
                {
                    CheckCursor(window);
                }
                else if (wParam.ToUInt64() == ClickTimerId)
                {
                    CompleteInjectedClick(window);
                }
                return IntPtr.Zero;

            case WM_MOUSEMOVE:
                if (Volatile.Read(ref armed) != 0)
                {
                    Interlocked.Increment(ref unexpectedMoveMessages);
                    SetFailure("Receiver observed an unexpected WM_MOUSEMOVE");
                    CancelActiveClick(window, null);
                }
                return IntPtr.Zero;

            case WM_LBUTTONDOWN:
                HandleLeftButton(window, true);
                return IntPtr.Zero;

            case WM_LBUTTONUP:
                HandleLeftButton(window, false);
                return IntPtr.Zero;

            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
                if (Volatile.Read(ref armed) != 0)
                {
                    Interlocked.Increment(ref unexpectedButtonMessages);
                    SetFailure("Receiver observed an unexpected non-left mouse button message");
                    CancelActiveClick(window, null);
                }
                return IntPtr.Zero;

            case WM_CAPTURECHANGED:
                if (Volatile.Read(ref captureHeld) != 0 && lParam != window)
                {
                    Volatile.Write(ref captureHeld, 0);
                    Volatile.Write(ref captureReleaseVerified, 1);
                    Interlocked.Increment(ref captureLossCount);
                    SetFailure("Receiver lost mouse capture during an injected click");
                    CancelActiveClick(window, null);
                }
                return IntPtr.Zero;

            case WM_CANCELMODE:
                if (Volatile.Read(ref captureHeld) != 0)
                {
                    Interlocked.Increment(ref captureLossCount);
                    SetFailure("Receiver got WM_CANCELMODE during an injected click");
                    CancelActiveClick(window, null);
                }
                return IntPtr.Zero;

            case WM_CLOSE:
                Volatile.Write(ref armed, 0);
                KillTimer(window, new UIntPtr(MonitorTimerId));
                CancelActiveClick(window, null);
                DestroyWindow(window);
                return IntPtr.Zero;

            case WM_DESTROY:
                SetWindowHandle(IntPtr.Zero);
                PostQuitMessage(0);
                return IntPtr.Zero;
            }
            return DefWindowProc(window, message, wParam, lParam);
        }
        catch (Exception error)
        {
            SetFailure("Receiver window callback failed: " + error.Message);
            try
            {
                CancelActiveClick(window, null);
            }
            catch
            {
            }
            clickCompleted.Set();
            return IntPtr.Zero;
        }
    }

    private bool Arm(IntPtr window)
    {
        MSG ignored;
        int preparationButtonMessages = 0;
        while (PeekMessage(
                out ignored,
                window,
                WM_MOUSEMOVE,
                WM_XBUTTONUP,
                PM_REMOVE))
        {
            if (ignored.Message != WM_MOUSEMOVE)
            {
                ++preparationButtonMessages;
            }
        }
        if (preparationButtonMessages != 0)
        {
            Volatile.Write(
                ref unexpectedButtonMessages,
                preparationButtonMessages);
            SetFailure("Receiver found mouse button input queued before arming");
            return false;
        }

        POINT cursor;
        if (!GetCursorPos(out cursor) || cursor.X != TargetX || cursor.Y != TargetY)
        {
            SetFailure("Cursor is not at the receiver target while arming");
            return false;
        }
        Volatile.Write(ref observedCursorX, cursor.X);
        Volatile.Write(ref observedCursorY, cursor.Y);
        Volatile.Write(ref cursorMoved, 0);
        Volatile.Write(ref attemptedSendInputCount, 0);
        Volatile.Write(ref acceptedSendInputCount, 0);
        Volatile.Write(ref taggedDownCount, 0);
        Volatile.Write(ref taggedUpCount, 0);
        Volatile.Write(ref unexpectedButtonMessages, 0);
        Volatile.Write(ref unexpectedMoveMessages, 0);
        Volatile.Write(ref captureLossCount, 0);
        Volatile.Write(ref emergencyUpCount, 0);
        Volatile.Write(ref armed, 1);
        if (SetTimer(window, new UIntPtr(MonitorTimerId), 10, IntPtr.Zero) == UIntPtr.Zero)
        {
            Volatile.Write(ref armed, 0);
            SetFailure("SetTimer failed while arming cursor monitoring");
            return false;
        }
        return true;
    }

    private bool BeginClick(IntPtr window, int holdMilliseconds)
    {
        if (Volatile.Read(ref armed) == 0)
        {
            SetFailure("Receiver is not armed");
            clickCompleted.Set();
            return false;
        }
        if (Volatile.Read(ref clickInProgress) != 0)
        {
            SetFailure("A Raw Input click is already in progress");
            clickCompleted.Set();
            return false;
        }
        if (!CheckCursor(window) || !AreMouseButtonsReleased())
        {
            SetFailure("Mouse state changed before an injected click");
            clickCompleted.Set();
            return false;
        }

        Volatile.Write(ref clickInProgress, 1);
        Volatile.Write(ref clickSawDown, 0);
        SetCapture(window);
        if (GetCapture() != window)
        {
            SetFailure("SetCapture did not assign capture to the receiver window");
            Volatile.Write(ref clickInProgress, 0);
            clickCompleted.Set();
            return false;
        }
        Volatile.Write(ref captureHeld, 1);
        Volatile.Write(ref captureReleaseVerified, 0);

        if (!SendMouseButton(MOUSEEVENTF_LEFTDOWN))
        {
            SetFailure("SendInput rejected the injected left-button Down");
            CancelActiveClick(window, null);
            return false;
        }
        Volatile.Write(ref injectedDownActive, 1);
        if (SetTimer(
                window,
                new UIntPtr(ClickTimerId),
                unchecked((uint)holdMilliseconds),
                IntPtr.Zero) == UIntPtr.Zero)
        {
            SetFailure("SetTimer failed for the injected click hold interval");
            CancelActiveClick(window, null);
            return false;
        }
        return true;
    }

    private void CompleteInjectedClick(IntPtr window)
    {
        KillTimer(window, new UIntPtr(ClickTimerId));
        if (Volatile.Read(ref clickInProgress) == 0)
        {
            return;
        }
        if (!SendMouseButton(MOUSEEVENTF_LEFTUP))
        {
            SetFailure("SendInput rejected the injected left-button Up");
            CancelActiveClick(window, null);
            return;
        }
        Volatile.Write(ref injectedDownActive, 0);
        // Completion waits for the tagged WM_LBUTTONUP, proving that the
        // ordinary input stream accepted the injected edge pair.
    }

    private void HandleLeftButton(IntPtr window, bool down)
    {
        if (Volatile.Read(ref armed) == 0)
        {
            return;
        }
        if (unchecked((UInt64)GetMessageExtraInfo().ToInt64()) != InputMarker)
        {
            Interlocked.Increment(ref unexpectedButtonMessages);
            SetFailure("Receiver observed an untagged left-button message");
            CancelActiveClick(window, null);
            return;
        }

        if (down)
        {
            Interlocked.Increment(ref taggedDownCount);
            if (Volatile.Read(ref clickInProgress) != 0)
            {
                Volatile.Write(ref clickSawDown, 1);
            }
            return;
        }

        Interlocked.Increment(ref taggedUpCount);
        if (Volatile.Read(ref clickInProgress) == 0)
        {
            return;
        }
        if (Volatile.Read(ref clickSawDown) == 0)
        {
            SetFailure("Receiver observed tagged Up before tagged Down");
            CancelActiveClick(window, null);
            return;
        }

        Volatile.Write(ref clickInProgress, 0);
        ReleaseOwnedCapture(window);
        clickCompleted.Set();
    }

    private bool CheckCursor(IntPtr window)
    {
        POINT cursor;
        if (!GetCursorPos(out cursor))
        {
            SetFailure("GetCursorPos failed while monitoring Raw Input capture");
            CancelActiveClick(window, null);
            return false;
        }
        Volatile.Write(ref observedCursorX, cursor.X);
        Volatile.Write(ref observedCursorY, cursor.Y);
        if (cursor.X == TargetX && cursor.Y == TargetY)
        {
            return true;
        }

        Volatile.Write(ref cursorMoved, 1);
        SetFailure(
            "Cursor moved during Raw Input capture to (" +
            cursor.X.ToString() + ", " + cursor.Y.ToString() + ")");
        CancelActiveClick(window, null);
        return false;
    }

    private void CancelActiveClick(IntPtr window, string reason)
    {
        KillTimer(window, new UIntPtr(ClickTimerId));
        if (!String.IsNullOrEmpty(reason))
        {
            SetFailure(reason);
        }
        if (Volatile.Read(ref injectedDownActive) != 0)
        {
            Interlocked.Increment(ref emergencyUpCount);
            SendMouseButton(MOUSEEVENTF_LEFTUP);
            Volatile.Write(ref injectedDownActive, 0);
        }
        Volatile.Write(ref clickInProgress, 0);
        ReleaseOwnedCapture(window);
        clickCompleted.Set();
    }

    private void ReleaseOwnedCapture(IntPtr window)
    {
        bool owned = GetCapture() == window;
        if (Volatile.Read(ref captureHeld) == 0 && !owned)
        {
            Volatile.Write(ref captureReleaseVerified, 1);
            return;
        }
        Volatile.Write(ref captureHeld, 0);
        if (owned)
        {
            ReleaseCapture();
        }
        bool released = GetCapture() != window;
        Volatile.Write(ref captureReleaseVerified, released ? 1 : 0);
        if (!released)
        {
            Volatile.Write(ref captureHeld, 1);
            SetFailure("Receiver could not release mouse capture");
        }
    }

    private bool SendMouseButton(uint flags)
    {
        INPUT input = new INPUT();
        input.Type = INPUT_MOUSE;
        input.Union.Mouse.Flags = flags;
        input.Union.Mouse.ExtraInfo = new UIntPtr(InputMarker);
        Interlocked.Increment(ref attemptedSendInputCount);
        uint accepted = SendInput(
            1,
            new INPUT[] { input },
            Marshal.SizeOf(typeof(INPUT)));
        if (accepted == 1)
        {
            Interlocked.Increment(ref acceptedSendInputCount);
            return true;
        }
        return false;
    }

    private bool SendControlMessage(
        uint message,
        int parameter,
        int timeoutMilliseconds,
        bool requireHandled)
    {
        IntPtr handle = GetWindowHandle();
        if (handle == IntPtr.Zero)
        {
            throw new InvalidOperationException("Raw Input receiver window is unavailable");
        }

        IntPtr result;
        IntPtr sent = SendMessageTimeout(
            handle,
            message,
            new UIntPtr(unchecked((uint)parameter)),
            IntPtr.Zero,
            SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
            unchecked((uint)timeoutMilliseconds),
            out result);
        if (sent == IntPtr.Zero)
        {
            int error = Marshal.GetLastWin32Error();
            throw new TimeoutException(
                "Receiver control message 0x" + message.ToString("X4") +
                " failed or timed out; Win32 error " + error.ToString());
        }
        return !requireHandled || result != IntPtr.Zero;
    }

    private void ThrowIfFailed()
    {
        string current = Failure;
        if (!String.IsNullOrEmpty(current))
        {
            throw new InvalidOperationException(current);
        }
    }

    private void SetFailure(string message)
    {
        if (String.IsNullOrEmpty(message))
        {
            return;
        }
        lock (sync)
        {
            if (String.IsNullOrEmpty(failure))
            {
                failure = message;
            }
        }
    }

    private IntPtr GetWindowHandle()
    {
        lock (sync)
        {
            return windowHandle;
        }
    }

    private void SetWindowHandle(IntPtr value)
    {
        lock (sync)
        {
            windowHandle = value;
        }
    }

    private static bool WaitForCursor(int x, int y, int timeoutMilliseconds)
    {
        Stopwatch timer = Stopwatch.StartNew();
        do
        {
            POINT cursor;
            if (GetCursorPos(out cursor) && cursor.X == x && cursor.Y == y)
            {
                return true;
            }
            Thread.Sleep(5);
        }
        while (timer.ElapsedMilliseconds < timeoutMilliseconds);
        return false;
    }

    private static bool AreMouseButtonsReleased()
    {
        return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0 &&
            (GetAsyncKeyState(VK_RBUTTON) & 0x8000) == 0 &&
            (GetAsyncKeyState(VK_MBUTTON) & 0x8000) == 0 &&
            (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) == 0 &&
            (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) == 0;
    }

    private static void EnsureMouseButtonsReleased()
    {
        if (!AreMouseButtonsReleased())
        {
            throw new InvalidOperationException(
                "Release every mouse button before Raw Input capture");
        }
    }

    private static int Clamp(int value, int minimum, int maximum)
    {
        return Math.Max(minimum, Math.Min(maximum, value));
    }

    private static void ThrowLastError(string message)
    {
        throw new InvalidOperationException(
            message + "; Win32 error " + Marshal.GetLastWin32Error().ToString());
    }

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate IntPtr WindowProcedure(
        IntPtr window,
        uint message,
        UIntPtr wParam,
        IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT
    {
        public int X;
        public int Y;

        public POINT(int x, int y)
        {
            X = x;
            Y = y;
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MONITORINFO
    {
        public uint Size;
        public RECT Monitor;
        public RECT Work;
        public uint Flags;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WNDCLASSEX
    {
        public uint Size;
        public uint Style;
        public WindowProcedure WindowProcedure;
        public int ClassExtraBytes;
        public int WindowExtraBytes;
        public IntPtr Instance;
        public IntPtr Icon;
        public IntPtr Cursor;
        public IntPtr Background;
        [MarshalAs(UnmanagedType.LPWStr)]
        public string MenuName;
        [MarshalAs(UnmanagedType.LPWStr)]
        public string ClassName;
        public IntPtr SmallIcon;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MSG
    {
        public IntPtr Window;
        public uint Message;
        public UIntPtr WParam;
        public IntPtr LParam;
        public uint Time;
        public POINT Point;
        public uint Private;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PAINTSTRUCT
    {
        public IntPtr DeviceContext;
        [MarshalAs(UnmanagedType.Bool)]
        public bool Erase;
        public RECT Paint;
        [MarshalAs(UnmanagedType.Bool)]
        public bool Restore;
        [MarshalAs(UnmanagedType.Bool)]
        public bool IncrementalUpdate;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)]
        public byte[] Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct INPUT
    {
        public uint Type;
        public INPUTUNION Union;
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct INPUTUNION
    {
        [FieldOffset(0)]
        public MOUSEINPUT Mouse;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MOUSEINPUT
    {
        public int X;
        public int Y;
        public uint MouseData;
        public uint Flags;
        public uint Time;
        public UIntPtr ExtraInfo;
    }

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetCursorPos(out POINT point);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetClipCursor(out RECT rectangle);

    [DllImport("user32.dll")]
    private static extern short GetAsyncKeyState(int key);

    [DllImport("user32.dll")]
    private static extern IntPtr MonitorFromPoint(
        POINT point,
        uint flags);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetMonitorInfo(
        IntPtr monitor,
        ref MONITORINFO information);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IntersectRect(
        out RECT destination,
        ref RECT first,
        ref RECT second);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr GetModuleHandle(string moduleName);

    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();

    [DllImport("gdi32.dll")]
    private static extern IntPtr CreateSolidBrush(uint color);

    [DllImport("gdi32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeleteObject(IntPtr value);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern ushort RegisterClassEx(ref WNDCLASSEX windowClass);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UnregisterClass(
        string className,
        IntPtr instance);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateWindowEx(
        uint extendedStyle,
        string className,
        string windowName,
        uint style,
        int x,
        int y,
        int width,
        int height,
        IntPtr parent,
        IntPtr menu,
        IntPtr instance,
        IntPtr parameter);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ShowWindow(IntPtr window, int command);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWindowPos(
        IntPtr window,
        IntPtr insertAfter,
        int x,
        int y,
        int width,
        int height,
        uint flags);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UpdateWindow(IntPtr window);

    [DllImport("user32.dll")]
    private static extern int GetMessage(
        out MSG message,
        IntPtr window,
        uint minimum,
        uint maximum);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool PeekMessage(
        out MSG message,
        IntPtr window,
        uint minimum,
        uint maximum,
        uint remove);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool TranslateMessage(ref MSG message);

    [DllImport("user32.dll")]
    private static extern IntPtr DispatchMessage(ref MSG message);

    [DllImport("user32.dll")]
    private static extern IntPtr DefWindowProc(
        IntPtr window,
        uint message,
        UIntPtr wParam,
        IntPtr lParam);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DestroyWindow(IntPtr window);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(IntPtr window);

    [DllImport("user32.dll")]
    private static extern void PostQuitMessage(int exitCode);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool PostMessage(
        IntPtr window,
        uint message,
        UIntPtr wParam,
        IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool PostThreadMessage(
        uint threadId,
        uint message,
        UIntPtr wParam,
        IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SendMessageTimeout(
        IntPtr window,
        uint message,
        UIntPtr wParam,
        IntPtr lParam,
        uint flags,
        uint timeout,
        out IntPtr result);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern UIntPtr SetTimer(
        IntPtr window,
        UIntPtr timerId,
        uint interval,
        IntPtr callback);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool KillTimer(IntPtr window, UIntPtr timerId);

    [DllImport("user32.dll")]
    private static extern IntPtr SetCapture(IntPtr window);

    [DllImport("user32.dll")]
    private static extern IntPtr GetCapture();

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ReleaseCapture();

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(
        uint inputCount,
        INPUT[] inputs,
        int inputSize);

    [DllImport("user32.dll")]
    private static extern IntPtr GetMessageExtraInfo();

    [DllImport("user32.dll")]
    private static extern IntPtr LoadCursor(IntPtr instance, IntPtr cursorName);

    [DllImport("user32.dll")]
    private static extern IntPtr SetThreadDpiAwarenessContext(IntPtr dpiContext);

    [DllImport("user32.dll")]
    private static extern IntPtr SetCursor(IntPtr cursor);

    [DllImport("user32.dll")]
    private static extern IntPtr BeginPaint(
        IntPtr window,
        out PAINTSTRUCT paint);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EndPaint(
        IntPtr window,
        ref PAINTSTRUCT paint);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetClientRect(IntPtr window, out RECT rectangle);

    [DllImport("user32.dll")]
    private static extern int FillRect(
        IntPtr deviceContext,
        ref RECT rectangle,
        IntPtr brush);
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
    readyTimeoutMs = $ReadyTimeoutMilliseconds
    processTimeoutMs = $ProcessTimeoutMilliseconds
    modes = [ordered]@{}
}
if ($Scenario -eq $rawInputScenario)
{
    $manifest['clickCount'] = $ClickCount
    $manifest['clickHoldMs'] = $ClickHoldMilliseconds
    $manifest['clickIntervalMs'] = $ClickIntervalMilliseconds
    $manifest['inputConfirmationTimeoutMs'] =
        $InputConfirmationTimeoutMilliseconds
    $manifest['receiverReadyTimeoutMs'] = $ReceiverReadyTimeoutMilliseconds
    $manifest['receiverStopTimeoutMs'] = $ReceiverStopTimeoutMilliseconds
    $manifest['cursorRestoreTimeoutMs'] = $CursorRestoreTimeoutMilliseconds
    $manifest['inputInjection'] =
        'SendInput-capability-probe-no-WM_INPUT-fallback'
    $manifest['rawInputVerification'] =
        'post-run-performance-interval'
    $manifest['rawInputRegistration'] =
        'enabled-inputsink-devnotify'
}
else
{
    $manifest['demoAgeMs'] = $demoAgeMilliseconds
    $manifest['demoDelayMs'] = $demoDelayMilliseconds
    $manifest['messageCount'] = $MessageCount
    $manifest['messageBatchSize'] = $messageBatchSize
    $manifest['messageBatchIntervalMs'] =
        $messageBatchIntervalMilliseconds
    $manifest['rawInputRegistration'] = 'disabled'
}
Write-CaptureManifest -Path $manifestPath -Manifest $manifest

try
{
    foreach ($mode in @(
            [ordered]@{ name = 'fx-only'; backgroundMode = 'recording-compatible' },
            [ordered]@{ name = 'background-aware'; backgroundMode = 'background-aware' }))
    {
        if ($Scenario -eq $rawInputScenario)
        {
            $modeEvidence = New-RawInputModeEvidence `
                -BackgroundMode $mode.backgroundMode `
                -ClickCount $ClickCount
            # Attach the entry before starting work so a failure in any
            # cleanup stage remains visible in the top-level manifest.
            $manifest.modes[$mode.name] = $modeEvidence
            Write-CaptureManifest -Path $manifestPath -Manifest $manifest
            Invoke-RawInputModeCapture `
                -Name $mode.name `
                -BackgroundMode $mode.backgroundMode `
                -SourceExecutable $executablePath `
                -CaptureRoot $outputRoot `
                -DurationMilliseconds $DurationMilliseconds `
                -ReadyTimeoutMilliseconds $ReadyTimeoutMilliseconds `
                -ProcessTimeoutMilliseconds $ProcessTimeoutMilliseconds `
                -ClickCount $ClickCount `
                -ClickHoldMilliseconds $ClickHoldMilliseconds `
                -ClickIntervalMilliseconds $ClickIntervalMilliseconds `
                -InputConfirmationTimeoutMilliseconds $InputConfirmationTimeoutMilliseconds `
                -ReceiverReadyTimeoutMilliseconds $ReceiverReadyTimeoutMilliseconds `
                -ReceiverStopTimeoutMilliseconds $ReceiverStopTimeoutMilliseconds `
                -CursorRestoreTimeoutMilliseconds $CursorRestoreTimeoutMilliseconds `
                -ModeEvidence $modeEvidence
        }
        else
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
        }
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

if ($Scenario -eq $rawInputScenario)
{
    Write-Host "Raw Input performance capture completed: $outputRoot"
    Write-Host 'The raw-input scenario requires a matching raw-input report validator.'
}
else
{
    Write-Host "Paired performance capture completed: $outputRoot"
    Write-Host "Validate with: python -B tools\report-performance-baseline.py `"$outputRoot`""
}
