[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [string]$Ffmpeg = 'ffmpeg.exe',

    [string]$Ffprobe = 'ffprobe.exe',

    [ValidateRange(1000, 15000)]
    [int]$ReadyTimeoutMilliseconds = 5000,

    [ValidateRange(12500, 120000)]
    [int]$HostTimeoutMilliseconds = 20000,

    [ValidateRange(11000, 120000)]
    [int]$RecorderTimeoutMilliseconds = 15000,

    [ValidateRange(1000, 60000)]
    [int]$ProbeTimeoutMilliseconds = 10000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$hostName = 'ba-click-fx-desktop.exe'
$configName = 'BAFX.config.json'
$logName = 'ba-click-fx-desktop-support.log'
$scenarioId = 'spk-002-external-recording-gdigrab-v1'
$windowTitle = 'ba-click-fx-desktop'
$demoAgeMilliseconds = 130
$demoDelayMilliseconds = 3000
$hostDurationMilliseconds = 7500
$recordingDurationMilliseconds = 6000
$frameRate = 10
$maximumCaptureEdge = 1024
$backgroundSrgb8 = @(30, 82, 146)
$processPollMilliseconds = 25
$ownedTreeStopTimeoutMilliseconds = 5000
$ownedRootStopTimeoutMilliseconds = 2000
$backgroundStopTimeoutMilliseconds = 5000
$gitTimeoutMilliseconds = 5000

function Get-PrimaryDisplayMode
{
    if ($null -eq ('BafxExternalRecordingNativeMethods' -as [type]))
    {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class BafxExternalRecordingNativeMethods
{
    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr GetDC(IntPtr window);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern int ReleaseDC(IntPtr window, IntPtr deviceContext);

    [DllImport("gdi32.dll", SetLastError = true)]
    public static extern int GetDeviceCaps(IntPtr deviceContext, int index);
}

'@
    }

    $desktopDeviceContext = [BafxExternalRecordingNativeMethods]::GetDC(
        [IntPtr]::Zero)
    if ($desktopDeviceContext -eq [IntPtr]::Zero)
    {
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "GetDC(desktop) failed with Win32 error $errorCode"
    }

    try
    {
        # DESKTOP*RES bypasses DPI virtualization in the PowerShell host.
        $width = [BafxExternalRecordingNativeMethods]::GetDeviceCaps(
            $desktopDeviceContext,
            118)
        $height = [BafxExternalRecordingNativeMethods]::GetDeviceCaps(
            $desktopDeviceContext,
            117)
        $refreshRate = [BafxExternalRecordingNativeMethods]::GetDeviceCaps(
            $desktopDeviceContext,
            116)
        $bitsPerPixel = [BafxExternalRecordingNativeMethods]::GetDeviceCaps(
            $desktopDeviceContext,
            12)
        $planes = [BafxExternalRecordingNativeMethods]::GetDeviceCaps(
            $desktopDeviceContext,
            14)
    }
    finally
    {
        $released = [BafxExternalRecordingNativeMethods]::ReleaseDC(
            [IntPtr]::Zero,
            $desktopDeviceContext)
    }
    if ($released -eq 0)
    {
        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "ReleaseDC(desktop) failed with Win32 error $errorCode"
    }
    if ($width -le 0 -or $height -le 0)
    {
        throw 'GetDeviceCaps returned an empty primary display mode'
    }

    return [ordered]@{
        width = $width
        height = $height
        refreshRateHz = $refreshRate
        bitsPerPixel = $bitsPerPixel * $planes
    }
}

function Start-ControlledBackground
{
    param(
        [Parameter(Mandatory = $true)]
        [int]$Width,

        [Parameter(Mandatory = $true)]
        [int]$Height,

        [Parameter(Mandatory = $true)]
        [int[]]$Srgb8,

        [Parameter(Mandatory = $true)]
        [int]$TimeoutMilliseconds
    )

    if ($Srgb8.Count -ne 3)
    {
        throw 'Controlled background requires exactly three sRGB8 channels'
    }
    if ($null -eq ('BafxExternalRecordingBackgroundHost' -as [type]))
    {
        Add-Type -AssemblyName System.Drawing
        Add-Type -AssemblyName System.Windows.Forms
        $referenceDirectory = Join-Path $PSHOME 'ref'
        if (Test-Path -LiteralPath $referenceDirectory -PathType Container)
        {
            # PowerShell 7 replaces its default references when WinForms is added.
            $referenceAssemblies = @(
                Get-ChildItem -LiteralPath $referenceDirectory -Filter '*.dll' |
                    Select-Object -ExpandProperty FullName
                (Join-Path $PSHOME 'System.Drawing.Common.dll')
                (Join-Path $PSHOME 'System.Drawing.Primitives.dll')
                (Join-Path $PSHOME 'System.Windows.Forms.dll')
                (Join-Path $PSHOME 'System.Windows.Forms.Primitives.dll')
            ) | Select-Object -Unique
        }
        else
        {
            $referenceAssemblies = @(
                'System.Drawing.dll',
                'System.Windows.Forms.dll')
        }
        Add-Type -ReferencedAssemblies $referenceAssemblies -TypeDefinition @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

public sealed class BafxExternalRecordingBackgroundForm : Form
{
    private const int WmNcHitTest = 0x0084;
    private static readonly IntPtr HtTransparent = new IntPtr(-1);
    private const int WsExTransparent = 0x00000020;
    private const int WsExToolWindow = 0x00000080;
    private const int WsExNoActivate = 0x08000000;

    public BafxExternalRecordingBackgroundForm(Rectangle bounds, Color color)
    {
        AutoScaleMode = AutoScaleMode.None;
        BackColor = color;
        Bounds = bounds;
        Enabled = false;
        FormBorderStyle = FormBorderStyle.None;
        ShowInTaskbar = false;
        StartPosition = FormStartPosition.Manual;
        TopMost = true;
    }

    protected override CreateParams CreateParams
    {
        get
        {
            CreateParams parameters = base.CreateParams;
            parameters.ExStyle |= WsExTransparent | WsExToolWindow | WsExNoActivate;
            return parameters;
        }
    }

    protected override bool ShowWithoutActivation
    {
        get { return true; }
    }

    protected override void WndProc(ref Message message)
    {
        if (message.Msg == WmNcHitTest)
        {
            // The evidence fixture must never consume user input while visible.
            message.Result = HtTransparent;
            return;
        }
        base.WndProc(ref message);
    }
}

public sealed class BafxExternalRecordingBackgroundHost
{
    private static readonly IntPtr PerMonitorAwareV2 = new IntPtr(-4);
    private readonly ManualResetEventSlim ready = new ManualResetEventSlim(false);
    private Thread thread;
    private BafxExternalRecordingBackgroundForm form;
    private Exception startupError;

    public int ObservedRed { get; private set; }
    public int ObservedGreen { get; private set; }
    public int ObservedBlue { get; private set; }
    public int SampleCount { get; private set; }
    public bool InputDisabled { get; private set; }

    [DllImport("user32.dll")]
    private static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);

    [DllImport("user32.dll")]
    private static extern IntPtr GetDC(IntPtr window);

    [DllImport("user32.dll")]
    private static extern int ReleaseDC(IntPtr window, IntPtr deviceContext);

    [DllImport("user32.dll")]
    private static extern bool IsWindowEnabled(IntPtr window);

    [DllImport("user32.dll")]
    private static extern bool EnableWindow(IntPtr window, bool enable);

    [DllImport("gdi32.dll")]
    private static extern uint GetPixel(IntPtr deviceContext, int x, int y);

    [DllImport("dwmapi.dll")]
    private static extern int DwmFlush();

    private void VerifyPresentedBackground(
        int width,
        int height)
    {
        EnableWindow(form.Handle, false);
        int flushResult = DwmFlush();
        if (flushResult < 0)
        {
            throw new InvalidOperationException(
                string.Format("DwmFlush failed with HRESULT 0x{0:X8}", flushResult));
        }
        InputDisabled = !IsWindowEnabled(form.Handle);
        if (!InputDisabled)
        {
            throw new InvalidOperationException("controlled background accepts input");
        }

        IntPtr deviceContext = GetDC(IntPtr.Zero);
        if (deviceContext == IntPtr.Zero)
        {
            throw new InvalidOperationException("GetDC(desktop) failed");
        }
        try
        {
            int offset = Math.Max(1, Math.Min(128, Math.Min(width, height) / 4));
            int centerX = width / 2;
            int centerY = height / 2;
            int[,] points = new int[,]
            {
                { centerX, centerY },
                { centerX - offset, centerY },
                { centerX + offset, centerY },
                { centerX, centerY - offset },
                { centerX, centerY + offset },
            };
            uint? firstSample = null;
            for (int index = 0; index < points.GetLength(0); ++index)
            {
                uint observed = GetPixel(deviceContext, points[index, 0], points[index, 1]);
                if (observed == 0xFFFFFFFF)
                {
                    throw new InvalidOperationException(
                        string.Format(
                            "controlled background readback failed at ({0},{1})",
                            points[index, 0],
                            points[index, 1]));
                }
                if (!firstSample.HasValue)
                {
                    ObservedRed = (int)(observed & 0xFF);
                    ObservedGreen = (int)((observed >> 8) & 0xFF);
                    ObservedBlue = (int)((observed >> 16) & 0xFF);
                    firstSample = observed;
                }
                else if (observed != firstSample.Value)
                {
                    throw new InvalidOperationException(
                        string.Format(
                            "controlled background is not uniform at ({0},{1}): " +
                            "first 0x{2:X6}, observed 0x{3:X6}",
                            points[index, 0],
                            points[index, 1],
                            firstSample.Value,
                            observed));
                }
            }
            SampleCount = points.GetLength(0);
        }
        finally
        {
            ReleaseDC(IntPtr.Zero, deviceContext);
        }
    }

    public IntPtr Start(
        int width,
        int height,
        int red,
        int green,
        int blue,
        int timeoutMilliseconds)
    {
        if (thread != null)
        {
            throw new InvalidOperationException("background fixture already started");
        }

        thread = new Thread(
            () =>
            {
                try
                {
                    SetThreadDpiAwarenessContext(PerMonitorAwareV2);
                    form = new BafxExternalRecordingBackgroundForm(
                        new Rectangle(0, 0, width, height),
                        Color.FromArgb(red, green, blue));
                    form.Shown += (_, __) =>
                    {
                        form.BeginInvoke(
                            new Action(
                                () =>
                                {
                                    try
                                    {
                                        form.Refresh();
                                        VerifyPresentedBackground(
                                            width,
                                            height);
                                    }
                                    catch (Exception error)
                                    {
                                        startupError = error;
                                    }
                                    finally
                                    {
                                        ready.Set();
                                    }
                                }));
                    };
                    Application.Run(form);
                }
                catch (Exception error)
                {
                    startupError = error;
                    ready.Set();
                }
            });
        thread.IsBackground = true;
        thread.Name = "BAFX external recording background";
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();

        if (!ready.Wait(timeoutMilliseconds))
        {
            Stop(timeoutMilliseconds);
            throw new TimeoutException("controlled background did not become ready");
        }
        if (startupError != null)
        {
            Stop(timeoutMilliseconds);
            throw new InvalidOperationException(
                "controlled background failed to start",
                startupError);
        }
        return form.Handle;
    }

    public bool Stop(int timeoutMilliseconds)
    {
        if (thread == null)
        {
            return true;
        }
        try
        {
            if (form != null && !form.IsDisposed && form.IsHandleCreated)
            {
                form.BeginInvoke(new Action(form.Close));
            }
        }
        catch (InvalidOperationException)
        {
            // The UI thread may have completed between the state check and invoke.
        }
        return thread.Join(timeoutMilliseconds);
    }
}
'@ -ErrorAction Stop
    }

    $backgroundHost = New-Object BafxExternalRecordingBackgroundHost
    try
    {
        $handle = $backgroundHost.Start(
            $Width,
            $Height,
            $Srgb8[0],
            $Srgb8[1],
            $Srgb8[2],
            $TimeoutMilliseconds)
        return [ordered]@{
            host = $backgroundHost
            handle = $handle
        }
    }
    catch
    {
        $null = $backgroundHost.Stop($TimeoutMilliseconds)
        throw
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

function Resolve-ApplicationPath
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$BaseDirectory
    )

    if ([IO.Path]::IsPathRooted($Path) `
        -or $Path.Contains([IO.Path]::DirectorySeparatorChar) `
        -or $Path.Contains([IO.Path]::AltDirectorySeparatorChar))
    {
        $resolved = Get-FullPath -Path $Path -BaseDirectory $BaseDirectory
    }
    else
    {
        $command = Get-Command -Name $Path -CommandType Application `
            -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($null -eq $command)
        {
            throw "Application is not available on PATH: $Path"
        }
        $resolved = [IO.Path]::GetFullPath($command.Source)
    }

    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf))
    {
        throw "Application is missing: $resolved"
    }
    return $resolved
}

function Write-Utf8Text
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Text
    )

    # Host configuration and evidence use UTF-8 without a PowerShell 5.1 BOM.
    $encoding = New-Object Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($Path, $Text, $encoding)
}

function Write-JsonFile
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [object]$Value
    )

    $json = $Value | ConvertTo-Json -Depth 12
    Write-Utf8Text -Path $Path -Text ($json + "`n")
}

function ConvertTo-CommandDisplayArgument
{
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Value
    )

    if ($Value -notmatch '[\s"]')
    {
        return $Value
    }

    # argv is authoritative; this escaped form is only for readable transcripts.
    return '"' + $Value.Replace('"', '\"') + '"'
}

function New-CommandRecord
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExecutablePath,

        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]]$Arguments
    )

    $argv = @($ExecutablePath) + @($Arguments)
    $display = ($argv |
        ForEach-Object { ConvertTo-CommandDisplayArgument -Value $_ }) -join ' '
    return [ordered]@{
        argv = $argv
        display = $display
    }
}

function New-ProcessRecord
{
    param(
        [Parameter(Mandatory = $true)]
        [int]$TimeoutMilliseconds
    )

    return [ordered]@{
        state = 'not-started'
        pid = $null
        startedAtUtc = $null
        readyAtUtc = $null
        timeoutMs = $TimeoutMilliseconds
        timedOut = $false
        exitedAtUtc = $null
        exitCode = $null
    }
}

function Complete-ProcessRecord
{
    param(
        [AllowNull()]
        [Diagnostics.Process]$Process,

        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Record
    )

    if ($null -eq $Process)
    {
        return
    }

    try
    {
        if ($Process.HasExited)
        {
            $Record['state'] = 'exited'
            $Record['exitedAtUtc'] = $Process.ExitTime.ToUniversalTime().ToString(
                'yyyy-MM-ddTHH:mm:ss.fffZ')
            $Record['exitCode'] = $Process.ExitCode
        }
        else
        {
            $Record['state'] = 'running'
        }
    }
    catch
    {
        $Record['observationError'] = $_.Exception.Message
    }
}

function Stop-OwnedProcessTree
{
    param(
        [AllowNull()]
        [Diagnostics.Process]$Process,

        [Parameter(Mandatory = $true)]
        [string]$Reason
    )

    $result = [ordered]@{
        reason = $Reason
        targetPid = if ($null -eq $Process) { $null } else { $Process.Id }
        attempted = $false
        taskkillTimedOut = $false
        taskkillExitCode = $null
        rootExited = if ($null -eq $Process) { $true } else { $false }
        error = $null
    }
    if ($null -eq $Process)
    {
        return $result
    }

    try
    {
        if ($Process.HasExited)
        {
            $result['rootExited'] = $true
            return $result
        }

        $result['attempted'] = $true
        $taskkillPath = Join-Path `
            ([Environment]::GetFolderPath([Environment+SpecialFolder]::Windows)) `
            'System32\taskkill.exe'
        $taskkillArguments = @(
            '/PID',
            $Process.Id.ToString([Globalization.CultureInfo]::InvariantCulture),
            '/T',
            '/F'
        )
        $killer = Start-Process `
            -FilePath $taskkillPath `
            -ArgumentList $taskkillArguments `
            -WindowStyle Hidden `
            -PassThru
        if (-not $killer.WaitForExit($script:ownedTreeStopTimeoutMilliseconds))
        {
            $result['taskkillTimedOut'] = $true
            Stop-Process -Id $killer.Id -Force -ErrorAction SilentlyContinue
            $null = $killer.WaitForExit($script:ownedRootStopTimeoutMilliseconds)
        }
        else
        {
            $result['taskkillExitCode'] = $killer.ExitCode
        }

        if (-not $Process.WaitForExit($script:ownedRootStopTimeoutMilliseconds))
        {
            # The fallback still targets only the exact root process we started.
            Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
            $null = $Process.WaitForExit($script:ownedRootStopTimeoutMilliseconds)
        }
        $result['rootExited'] = $Process.HasExited
    }
    catch
    {
        $result['error'] = $_.Exception.Message
        try
        {
            $result['rootExited'] = $Process.HasExited
        }
        catch
        {
            $result['rootExited'] = $false
        }
    }
    return $result
}

function Invoke-BoundedTextProcess
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExecutablePath,

        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]]$Arguments,

        [Parameter(Mandatory = $true)]
        [string]$WorkingDirectory,

        [Parameter(Mandatory = $true)]
        [int]$TimeoutMilliseconds
    )

    $token = [Guid]::NewGuid().ToString('N')
    $stdoutPath = Join-Path ([IO.Path]::GetTempPath()) "$token.stdout"
    $stderrPath = Join-Path ([IO.Path]::GetTempPath()) "$token.stderr"
    $process = $null
    try
    {
        $process = Start-Process `
            -FilePath $ExecutablePath `
            -ArgumentList $Arguments `
            -WorkingDirectory $WorkingDirectory `
            -WindowStyle Hidden `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath `
            -PassThru
        if (-not $process.WaitForExit($TimeoutMilliseconds))
        {
            $null = Stop-OwnedProcessTree `
                -Process $process `
                -Reason 'bounded-text-process-timeout'
            throw "Process exceeded the $TimeoutMilliseconds ms timeout: $ExecutablePath"
        }
        $stdout = if (Test-Path -LiteralPath $stdoutPath -PathType Leaf)
        {
            Get-Content -LiteralPath $stdoutPath -Raw
        }
        else
        {
            ''
        }
        $stderr = if (Test-Path -LiteralPath $stderrPath -PathType Leaf)
        {
            Get-Content -LiteralPath $stderrPath -Raw
        }
        else
        {
            ''
        }
        return [ordered]@{
            exitCode = $process.ExitCode
            stdout = $stdout
            stderr = $stderr
        }
    }
    finally
    {
        if ($null -ne $process -and -not $process.HasExited)
        {
            $null = Stop-OwnedProcessTree `
                -Process $process `
                -Reason 'bounded-text-process-finally'
        }
        Remove-Item -LiteralPath $stdoutPath -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $stderrPath -Force -ErrorAction SilentlyContinue
    }
}

function New-RecordingConfiguration
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
        schemaVersion = 8
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
        Start-Sleep -Milliseconds $script:processPollMilliseconds
    }
    throw "Host did not become ready within $TimeoutMilliseconds ms"
}

function Wait-ForCaptureProcesses
{
    param(
        [Parameter(Mandatory = $true)]
        [Diagnostics.Process]$HostProcess,

        [Parameter(Mandatory = $true)]
        [Diagnostics.Stopwatch]$HostTimer,

        [Parameter(Mandatory = $true)]
        [int]$HostTimeoutMilliseconds,

        [Parameter(Mandatory = $true)]
        [Diagnostics.Process]$RecorderProcess,

        [Parameter(Mandatory = $true)]
        [Diagnostics.Stopwatch]$RecorderTimer,

        [Parameter(Mandatory = $true)]
        [int]$RecorderTimeoutMilliseconds
    )

    while ($true)
    {
        $hostExited = $HostProcess.HasExited
        $recorderExited = $RecorderProcess.HasExited
        if ($hostExited -and $recorderExited)
        {
            return $null
        }
        if (-not $hostExited `
            -and $HostTimer.ElapsedMilliseconds -ge $HostTimeoutMilliseconds)
        {
            return 'host'
        }
        if (-not $recorderExited `
            -and $RecorderTimer.ElapsedMilliseconds -ge $RecorderTimeoutMilliseconds)
        {
            return 'ffmpeg'
        }
        Start-Sleep -Milliseconds $script:processPollMilliseconds
    }
}

function Invoke-BoundedProbe
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$FfprobePath,

        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,

        [Parameter(Mandatory = $true)]
        [string]$WorkingDirectory,

        [Parameter(Mandatory = $true)]
        [string]$StdoutPath,

        [Parameter(Mandatory = $true)]
        [string]$StderrPath,

        [Parameter(Mandatory = $true)]
        [int]$TimeoutMilliseconds,

        [Parameter(Mandatory = $true)]
        [Collections.IDictionary]$Record
    )

    $startedAt = [DateTime]::UtcNow
    $process = Start-Process `
        -FilePath $FfprobePath `
        -ArgumentList $Arguments `
        -WorkingDirectory $WorkingDirectory `
        -WindowStyle Hidden `
        -RedirectStandardOutput $StdoutPath `
        -RedirectStandardError $StderrPath `
        -PassThru
    $Record['state'] = 'running'
    $Record['pid'] = $process.Id
    $Record['startedAtUtc'] = $startedAt.ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
    try
    {
        if (-not $process.WaitForExit($TimeoutMilliseconds))
        {
            $Record['timedOut'] = $true
            throw "ffprobe exceeded the $TimeoutMilliseconds ms timeout"
        }
    }
    finally
    {
        if (-not $process.HasExited)
        {
            $Record['cleanup'] = Stop-OwnedProcessTree `
                -Process $process `
                -Reason 'ffprobe-timeout-or-failure'
        }
        Complete-ProcessRecord -Process $process -Record $Record
    }
}

function Get-ArtifactRecord
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf))
    {
        return [ordered]@{
            exists = $false
            bytes = $null
            sha256 = $null
        }
    }

    $item = Get-Item -LiteralPath $Path
    return [ordered]@{
        exists = $true
        bytes = $item.Length
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).
            Hash.ToLowerInvariant()
    }
}

function Invoke-RecordingCase
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$CaseId,

        [Parameter(Mandatory = $true)]
        [string]$SourceKind,

        [Parameter(Mandatory = $true)]
        [string]$BackgroundMode,

        [Parameter(Mandatory = $true)]
        [string]$SourceExecutable,

        [Parameter(Mandatory = $true)]
        [string]$FfmpegPath,

        [Parameter(Mandatory = $true)]
        [string]$FfprobePath,

        [Parameter(Mandatory = $true)]
        [string]$CaptureRoot,

        [Parameter(Mandatory = $true)]
        [int]$CaptureLeft,

        [Parameter(Mandatory = $true)]
        [int]$CaptureTop,

        [Parameter(Mandatory = $true)]
        [int]$CaptureWidth,

        [Parameter(Mandatory = $true)]
        [int]$CaptureHeight,

        [Parameter(Mandatory = $true)]
        [int]$ReadyTimeoutMilliseconds,

        [Parameter(Mandatory = $true)]
        [int]$HostTimeoutMilliseconds,

        [Parameter(Mandatory = $true)]
        [int]$RecorderTimeoutMilliseconds,

        [Parameter(Mandatory = $true)]
        [int]$ProbeTimeoutMilliseconds
    )

    $caseRoot = Join-Path $CaptureRoot $CaseId
    $null = New-Item -ItemType Directory -Path $caseRoot
    $caseExecutable = Join-Path $caseRoot $script:hostName
    $configPath = Join-Path $caseRoot $script:configName
    $logPath = Join-Path $caseRoot $script:logName
    $hostStdoutPath = Join-Path $caseRoot 'host.stdout.log'
    $hostStderrPath = Join-Path $caseRoot 'host.stderr.log'
    $ffmpegStdoutPath = Join-Path $caseRoot 'ffmpeg.stdout.log'
    $ffmpegStderrPath = Join-Path $caseRoot 'ffmpeg.stderr.log'
    $capturePath = Join-Path $caseRoot 'capture.mkv'
    $ffprobeOutputPath = Join-Path $caseRoot 'ffprobe.json'
    $ffprobeStderrPath = Join-Path $caseRoot 'ffprobe.stderr.log'
    $caseManifestPath = Join-Path $caseRoot 'case.json'
    Copy-Item -LiteralPath $SourceExecutable -Destination $caseExecutable

    $configuration = New-RecordingConfiguration -BackgroundMode $BackgroundMode
    Write-JsonFile -Path $configPath -Value $configuration

    $gdigrabInput = if ($SourceKind -eq 'desktop')
    {
        'desktop'
    }
    else
    {
        "title=$($script:windowTitle)"
    }
    $hostArguments = @(
        "--demo-age-ms=$($script:demoAgeMilliseconds)",
        "--demo-delay-ms=$($script:demoDelayMilliseconds)",
        '--disable-raw-input',
        "--quit-after-ms=$($script:hostDurationMilliseconds)"
    )
    $recordingSeconds = ($script:recordingDurationMilliseconds / 1000.0).
        ToString('0.000', [Globalization.CultureInfo]::InvariantCulture)
    $videoSize = '{0}x{1}' -f $CaptureWidth, $CaptureHeight
    $ffmpegArguments = @(
        '-hide_banner',
        '-nostdin',
        '-y',
        '-f',
        'gdigrab',
        '-framerate',
        $script:frameRate.ToString([Globalization.CultureInfo]::InvariantCulture),
        '-draw_mouse',
        '0',
        '-thread_queue_size',
        '1024',
        '-rtbufsize',
        '128M',
        '-offset_x',
        $CaptureLeft.ToString([Globalization.CultureInfo]::InvariantCulture),
        '-offset_y',
        $CaptureTop.ToString([Globalization.CultureInfo]::InvariantCulture),
        '-video_size',
        $videoSize,
        '-i',
        $gdigrabInput,
        '-t',
        $recordingSeconds,
        '-an',
        '-c:v',
        'ffv1',
        '-level',
        '3',
        '-pix_fmt',
        'bgr0',
        'capture.mkv'
    )
    $ffprobeArguments = @(
        '-v',
        'error',
        '-print_format',
        'json',
        '-show_format',
        '-show_streams',
        'capture.mkv'
    )
    $caseManifest = [ordered]@{
        schemaVersion = 1
        scenarioId = $script:scenarioId
        caseId = $CaseId
        source = [ordered]@{
            kind = $SourceKind
            gdigrabInput = $gdigrabInput
            captureLeft = $CaptureLeft
            captureTop = $CaptureTop
            captureWidth = $CaptureWidth
            captureHeight = $CaptureHeight
            windowTitle = if ($SourceKind -eq 'title')
            {
                $script:windowTitle
            }
            else
            {
                $null
            }
        }
        backgroundMode = $BackgroundMode
        status = 'collecting'
        startedAtUtc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
        completedAtUtc = $null
        commands = [ordered]@{
            host = New-CommandRecord `
                -ExecutablePath $caseExecutable `
                -Arguments $hostArguments
            ffmpeg = New-CommandRecord `
                -ExecutablePath $FfmpegPath `
                -Arguments $ffmpegArguments
            ffprobe = New-CommandRecord `
                -ExecutablePath $FfprobePath `
                -Arguments $ffprobeArguments
        }
        processes = [ordered]@{
            host = New-ProcessRecord -TimeoutMilliseconds $HostTimeoutMilliseconds
            ffmpeg = New-ProcessRecord `
                -TimeoutMilliseconds $RecorderTimeoutMilliseconds
            ffprobe = New-ProcessRecord `
                -TimeoutMilliseconds $ProbeTimeoutMilliseconds
        }
        files = [ordered]@{}
        failure = $null
    }
    Write-JsonFile -Path $caseManifestPath -Value $caseManifest

    $hostProcess = $null
    $ffmpegProcess = $null
    $hostTimer = $null
    $ffmpegTimer = $null
    $failures = [Collections.Generic.List[string]]::new()
    try
    {
        $hostStartedAt = [DateTime]::UtcNow
        $hostTimer = [Diagnostics.Stopwatch]::StartNew()
        $hostProcess = Start-Process `
            -FilePath $caseExecutable `
            -ArgumentList $hostArguments `
            -WorkingDirectory $caseRoot `
            -WindowStyle Hidden `
            -RedirectStandardOutput $hostStdoutPath `
            -RedirectStandardError $hostStderrPath `
            -PassThru
        $caseManifest.processes.host['state'] = 'running'
        $caseManifest.processes.host['pid'] = $hostProcess.Id
        $caseManifest.processes.host['startedAtUtc'] = $hostStartedAt.ToString(
            'yyyy-MM-ddTHH:mm:ss.fffZ')
        $threadId = Wait-ForHostReady `
            -Process $hostProcess `
            -LogPath $logPath `
            -TimeoutMilliseconds $ReadyTimeoutMilliseconds
        $caseManifest.processes.host['readyAtUtc'] = [DateTime]::UtcNow.ToString(
            'yyyy-MM-ddTHH:mm:ss.fffZ')
        $caseManifest.processes.host['threadId'] = $threadId

        $ffmpegStartedAt = [DateTime]::UtcNow
        $ffmpegTimer = [Diagnostics.Stopwatch]::StartNew()
        $ffmpegProcess = Start-Process `
            -FilePath $FfmpegPath `
            -ArgumentList $ffmpegArguments `
            -WorkingDirectory $caseRoot `
            -WindowStyle Hidden `
            -RedirectStandardOutput $ffmpegStdoutPath `
            -RedirectStandardError $ffmpegStderrPath `
            -PassThru
        $caseManifest.processes.ffmpeg['state'] = 'running'
        $caseManifest.processes.ffmpeg['pid'] = $ffmpegProcess.Id
        $caseManifest.processes.ffmpeg['startedAtUtc'] = $ffmpegStartedAt.ToString(
            'yyyy-MM-ddTHH:mm:ss.fffZ')

        $timedOutProcess = Wait-ForCaptureProcesses `
            -HostProcess $hostProcess `
            -HostTimer $hostTimer `
            -HostTimeoutMilliseconds $HostTimeoutMilliseconds `
            -RecorderProcess $ffmpegProcess `
            -RecorderTimer $ffmpegTimer `
            -RecorderTimeoutMilliseconds $RecorderTimeoutMilliseconds
        if ($timedOutProcess -eq 'host')
        {
            $caseManifest.processes.host['timedOut'] = $true
            throw "Host exceeded the $HostTimeoutMilliseconds ms timeout"
        }
        if ($timedOutProcess -eq 'ffmpeg')
        {
            $caseManifest.processes.ffmpeg['timedOut'] = $true
            throw "ffmpeg exceeded the $RecorderTimeoutMilliseconds ms timeout"
        }

        Complete-ProcessRecord `
            -Process $hostProcess `
            -Record $caseManifest.processes.host
        Complete-ProcessRecord `
            -Process $ffmpegProcess `
            -Record $caseManifest.processes.ffmpeg
        if ($hostProcess.ExitCode -ne 0)
        {
            $failures.Add("Host exited with code $($hostProcess.ExitCode)")
        }
        if ($ffmpegProcess.ExitCode -ne 0)
        {
            $failures.Add("ffmpeg exited with code $($ffmpegProcess.ExitCode)")
        }

        if (Test-Path -LiteralPath $capturePath -PathType Leaf)
        {
            try
            {
                Invoke-BoundedProbe `
                    -FfprobePath $FfprobePath `
                    -Arguments $ffprobeArguments `
                    -WorkingDirectory $caseRoot `
                    -StdoutPath $ffprobeOutputPath `
                    -StderrPath $ffprobeStderrPath `
                    -TimeoutMilliseconds $ProbeTimeoutMilliseconds `
                    -Record $caseManifest.processes.ffprobe
                if ($caseManifest.processes.ffprobe.exitCode -ne 0)
                {
                    $failures.Add(
                        "ffprobe exited with code $($caseManifest.processes.ffprobe.exitCode)")
                }
            }
            catch
            {
                $failures.Add($_.Exception.Message)
            }
        }
        else
        {
            $caseManifest.processes.ffprobe['state'] = 'skipped'
            $caseManifest.processes.ffprobe['skipReason'] = 'capture-missing'
            Write-Utf8Text -Path $ffprobeOutputPath -Text ''
            Write-Utf8Text -Path $ffprobeStderrPath -Text ''
            $failures.Add('capture.mkv was not created')
        }

        $events = @(Get-StructuredEventBlocks -Path $logPath)
        if (-not ($events -match '(?m)^Event.Name=Process.Exited\r?$'))
        {
            $failures.Add('Host log has no Process.Exited event')
        }
    }
    catch
    {
        $failures.Add($_.Exception.Message)
    }
    finally
    {
        if ($null -ne $ffmpegTimer)
        {
            $ffmpegTimer.Stop()
        }
        if ($null -ne $hostTimer)
        {
            $hostTimer.Stop()
        }

        if ($null -ne $ffmpegProcess -and -not $ffmpegProcess.HasExited)
        {
            $caseManifest.processes.ffmpeg['cleanup'] = Stop-OwnedProcessTree `
                -Process $ffmpegProcess `
                -Reason 'case-finalization'
        }
        if ($null -ne $hostProcess -and -not $hostProcess.HasExited)
        {
            $caseManifest.processes.host['cleanup'] = Stop-OwnedProcessTree `
                -Process $hostProcess `
                -Reason 'case-finalization'
        }
        Complete-ProcessRecord `
            -Process $ffmpegProcess `
            -Record $caseManifest.processes.ffmpeg
        Complete-ProcessRecord `
            -Process $hostProcess `
            -Record $caseManifest.processes.host

        $ownedHostRemaining = $null -ne $hostProcess -and -not $hostProcess.HasExited
        $ownedFfmpegRemaining = `
            $null -ne $ffmpegProcess -and -not $ffmpegProcess.HasExited
        $caseManifest['cleanup'] = [ordered]@{
            ownedHostRemaining = $ownedHostRemaining
            ownedFfmpegRemaining = $ownedFfmpegRemaining
            allOwnedProcessesExited = `
                -not $ownedHostRemaining -and -not $ownedFfmpegRemaining
        }
        if ($ownedHostRemaining -or $ownedFfmpegRemaining)
        {
            $failures.Add('One or more owned Host/ffmpeg processes remained alive')
        }

        foreach ($artifactName in @(
                $script:hostName,
                $script:configName,
                $script:logName,
                'host.stdout.log',
                'host.stderr.log',
                'ffmpeg.stdout.log',
                'ffmpeg.stderr.log',
                'capture.mkv',
                'ffprobe.json',
                'ffprobe.stderr.log'))
        {
            $caseManifest.files[$artifactName] = Get-ArtifactRecord `
                -Path (Join-Path $caseRoot $artifactName)
        }
        $caseManifest['completedAtUtc'] = [DateTime]::UtcNow.ToString(
            'yyyy-MM-ddTHH:mm:ss.fffZ')
        if ($failures.Count -eq 0)
        {
            $caseManifest['status'] = 'captured'
        }
        else
        {
            $caseManifest['status'] = 'failed'
            $caseManifest['failure'] = $failures -join '; '
        }
        Write-JsonFile -Path $caseManifestPath -Value $caseManifest
    }

    return [ordered]@{
        caseId = $CaseId
        sourceKind = $SourceKind
        backgroundMode = $BackgroundMode
        status = $caseManifest.status
        manifest = "$CaseId/case.json"
        manifestSha256 = (Get-FileHash `
            -LiteralPath $caseManifestPath `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        failure = $caseManifest.failure
        containmentLost = -not $caseManifest.cleanup.allOwnedProcessesExited
    }
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$executablePath = Get-FullPath -Path $Executable -BaseDirectory $repositoryRoot
$outputRoot = Get-FullPath -Path $OutputDirectory -BaseDirectory $repositoryRoot
$ffmpegPath = Resolve-ApplicationPath -Path $Ffmpeg -BaseDirectory $repositoryRoot
$ffprobePath = Resolve-ApplicationPath -Path $Ffprobe -BaseDirectory $repositoryRoot
$primaryDisplayMode = Get-PrimaryDisplayMode
$captureEdge = [Math]::Min(
    $maximumCaptureEdge,
    [Math]::Min($primaryDisplayMode.width, $primaryDisplayMode.height))
$captureEdge -= $captureEdge % 2
if ($captureEdge -lt 2)
{
    throw 'Primary display is too small for a centered recording region'
}
# Match the verifier's centered-region contract on odd display dimensions.
$captureLeft = [int][Math]::Floor(
    ($primaryDisplayMode.width - $captureEdge) / 2.0)
$captureTop = [int][Math]::Floor(
    ($primaryDisplayMode.height - $captureEdge) / 2.0)
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
if ($ReadyTimeoutMilliseconds -ge $HostTimeoutMilliseconds)
{
    throw 'ReadyTimeoutMilliseconds must be less than HostTimeoutMilliseconds'
}
if ($HostTimeoutMilliseconds -lt $hostDurationMilliseconds + 5000)
{
    throw 'HostTimeoutMilliseconds must exceed the Host workload by at least 5000 ms'
}
if ($RecorderTimeoutMilliseconds -lt $recordingDurationMilliseconds + 5000)
{
    throw 'RecorderTimeoutMilliseconds must exceed recording by at least 5000 ms'
}

$existingHosts = @(Get-Process -Name 'ba-click-fx-desktop' -ErrorAction SilentlyContinue)
if ($existingHosts.Count -ne 0)
{
    throw "Close the existing BAFX Host before capture: PID $($existingHosts.Id -join ', ')"
}

$gitPath = Resolve-ApplicationPath -Path 'git.exe' -BaseDirectory $repositoryRoot
$revisionResult = Invoke-BoundedTextProcess `
    -ExecutablePath $gitPath `
    -Arguments @('-C', $repositoryRoot, 'rev-parse', 'HEAD') `
    -WorkingDirectory $repositoryRoot `
    -TimeoutMilliseconds $gitTimeoutMilliseconds
$revision = $revisionResult.stdout.Trim()
if ($revisionResult.exitCode -ne 0 -or $revision -notmatch '^[0-9a-f]{40}$')
{
    throw "Could not resolve the source revision: $($revisionResult.stderr.Trim())"
}
$statusResult = Invoke-BoundedTextProcess `
    -ExecutablePath $gitPath `
    -Arguments @('-C', $repositoryRoot, 'status', '--porcelain', '--untracked-files=all') `
    -WorkingDirectory $repositoryRoot `
    -TimeoutMilliseconds $gitTimeoutMilliseconds
if ($statusResult.exitCode -ne 0)
{
    throw "Could not inspect the working tree: $($statusResult.stderr.Trim())"
}
if (-not [string]::IsNullOrWhiteSpace($statusResult.stdout))
{
    throw 'Official external recording collection requires a clean working tree.'
}

$backgroundFixture = Start-ControlledBackground `
    -Width $primaryDisplayMode.width `
    -Height $primaryDisplayMode.height `
    -Srgb8 $backgroundSrgb8 `
    -TimeoutMilliseconds $backgroundStopTimeoutMilliseconds
$manifestPath = Join-Path $outputRoot 'capture.json'
$manifest = $null
$collectionException = $null
try
{
$null = New-Item -ItemType Directory -Path $outputRoot
$matrix = @(
    [ordered]@{
        caseId = 'desktop-background-aware'
        sourceKind = 'desktop'
        backgroundMode = 'background-aware'
    },
    [ordered]@{
        caseId = 'desktop-recording-compatible'
        sourceKind = 'desktop'
        backgroundMode = 'recording-compatible'
    },
    [ordered]@{
        caseId = 'title-background-aware'
        sourceKind = 'title'
        backgroundMode = 'background-aware'
    },
    [ordered]@{
        caseId = 'title-recording-compatible'
        sourceKind = 'title'
        backgroundMode = 'recording-compatible'
    }
)
$manifest = [ordered]@{
    schemaVersion = 1
    scenarioId = $scenarioId
    evidenceScope = 'single-machine-ffmpeg-gdigrab-observation-only'
    captureStatus = 'collecting'
    revision = $revision
    workingTreeDirty = $false
    capturedAtUtc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
    backgroundFixture = [ordered]@{
        kind = 'solid-disabled-window'
        requestedSrgb8 = $backgroundSrgb8
        presentedSrgb8 = @(
            $backgroundFixture.host.ObservedRed,
            $backgroundFixture.host.ObservedGreen,
            $backgroundFixture.host.ObservedBlue)
        left = 0
        top = 0
        width = $primaryDisplayMode.width
        height = $primaryDisplayMode.height
        topmost = $true
        noActivate = $true
        inputPolicy = 'disabled-window'
        windowEnabled = -not $backgroundFixture.host.InputDisabled
        handle = ('0x{0:X}' -f [int64]$backgroundFixture.handle)
        startupSampleCount = $backgroundFixture.host.SampleCount
        startupSamplesUniform = $true
        stopTimeoutMs = $backgroundStopTimeoutMilliseconds
        stopped = $false
        stoppedAtUtc = $null
        stopFailure = $null
    }
    host = [ordered]@{
        sourcePath = $executablePath
        sha256 = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).
            Hash.ToLowerInvariant()
        demoAgeMs = $demoAgeMilliseconds
        demoDelayMs = $demoDelayMilliseconds
        quitAfterMs = $hostDurationMilliseconds
        rawInputRegistration = 'disabled'
        readyTimeoutMs = $ReadyTimeoutMilliseconds
        processTimeoutMs = $HostTimeoutMilliseconds
    }
    recorder = [ordered]@{
        implementation = 'ffmpeg-gdigrab'
        ffmpegPath = $ffmpegPath
        ffmpegSha256 = (Get-FileHash -LiteralPath $ffmpegPath -Algorithm SHA256).
            Hash.ToLowerInvariant()
        ffprobePath = $ffprobePath
        ffprobeSha256 = (Get-FileHash -LiteralPath $ffprobePath -Algorithm SHA256).
            Hash.ToLowerInvariant()
        frameRate = $frameRate
        durationMs = $recordingDurationMilliseconds
        processTimeoutMs = $RecorderTimeoutMilliseconds
        probeTimeoutMs = $ProbeTimeoutMilliseconds
        videoCodec = 'ffv1'
        pixelFormat = 'bgr0'
    }
    capture = [ordered]@{
        displayWidth = $primaryDisplayMode.width
        displayHeight = $primaryDisplayMode.height
        left = $captureLeft
        top = $captureTop
        width = $captureEdge
        height = $captureEdge
        frameRate = $frameRate
        durationMs = $recordingDurationMilliseconds
    }
    matrix = $matrix
    cases = @()
}
Write-JsonFile -Path $manifestPath -Value $manifest

foreach ($entry in $matrix)
{
    $continueMatrix = $true
    try
    {
        $caseOutcome = Invoke-RecordingCase `
            -CaseId $entry.caseId `
            -SourceKind $entry.sourceKind `
            -BackgroundMode $entry.backgroundMode `
            -SourceExecutable $executablePath `
            -FfmpegPath $ffmpegPath `
            -FfprobePath $ffprobePath `
            -CaptureRoot $outputRoot `
            -CaptureLeft $captureLeft `
            -CaptureTop $captureTop `
            -CaptureWidth $captureEdge `
            -CaptureHeight $captureEdge `
            -ReadyTimeoutMilliseconds $ReadyTimeoutMilliseconds `
            -HostTimeoutMilliseconds $HostTimeoutMilliseconds `
            -RecorderTimeoutMilliseconds $RecorderTimeoutMilliseconds `
            -ProbeTimeoutMilliseconds $ProbeTimeoutMilliseconds
        $caseResult = [ordered]@{
            caseId = $caseOutcome.caseId
            sourceKind = $caseOutcome.sourceKind
            backgroundMode = $caseOutcome.backgroundMode
            status = $caseOutcome.status
            manifest = $caseOutcome.manifest
            manifestSha256 = $caseOutcome.manifestSha256
            failure = $caseOutcome.failure
        }
        $continueMatrix = -not $caseOutcome.containmentLost
    }
    catch
    {
        # Without a finalized case manifest, containment cannot be proven.
        $continueMatrix = $false
        $caseResult = [ordered]@{
            caseId = $entry.caseId
            sourceKind = $entry.sourceKind
            backgroundMode = $entry.backgroundMode
            status = 'failed'
            manifest = "$($entry.caseId)/case.json"
            manifestSha256 = $null
            failure = $_.Exception.Message
        }
    }
    $manifest.cases += $caseResult
    Write-JsonFile -Path $manifestPath -Value $manifest
    if (-not $continueMatrix)
    {
        break
    }
}

$failedCases = @($manifest.cases | Where-Object { $_.status -ne 'captured' })
$matrixIncomplete = $manifest.cases.Count -ne $matrix.Count
if ($failedCases.Count -eq 0 -and -not $matrixIncomplete)
{
    $manifest.captureStatus = 'captured-pending-background-stop'
}
else
{
    $manifest.captureStatus = 'failed'
    $manifest['failure'] = if ($matrixIncomplete)
    {
        'Matrix collection stopped because owned-process containment was not proven'
    }
    else
    {
        "$($failedCases.Count) matrix case(s) failed"
    }
}
$manifest['completedAtUtc'] = [DateTime]::UtcNow.ToString(
    'yyyy-MM-ddTHH:mm:ss.fffZ')
Write-JsonFile -Path $manifestPath -Value $manifest

if ($failedCases.Count -ne 0 -or $matrixIncomplete)
{
    $failedNames = if ($failedCases.Count -eq 0)
    {
        'incomplete-matrix'
    }
    else
    {
        $failedCases.caseId -join ', '
    }
    throw "External recording collection failed: $failedNames. Evidence: $outputRoot"
}

}
catch
{
    $collectionException = $_.Exception
}

$backgroundStopException = $null
$backgroundStopped = $false
try
{
    $backgroundStopped = $backgroundFixture.host.Stop(
        $backgroundStopTimeoutMilliseconds)
    if (-not $backgroundStopped)
    {
        $backgroundStopException = [TimeoutException]::new(
            'Controlled background did not stop within its 5000 ms timeout')
    }
}
catch
{
    $backgroundStopException = $_.Exception
}

if ($null -ne $manifest)
{
    $manifest.backgroundFixture.stopped = $backgroundStopped
    $manifest.backgroundFixture.stoppedAtUtc = if ($backgroundStopped)
    {
        [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
    }
    else
    {
        $null
    }
    $manifest.backgroundFixture.stopFailure = if ($null -eq $backgroundStopException)
    {
        $null
    }
    else
    {
        $backgroundStopException.Message
    }
    if ($null -ne $collectionException)
    {
        $manifest.captureStatus = 'failed'
        if (-not $manifest.Contains('failure'))
        {
            $manifest['failure'] = $collectionException.Message
        }
    }
    elseif ($null -ne $backgroundStopException)
    {
        $manifest.captureStatus = 'failed'
        $manifest['failure'] = $backgroundStopException.Message
    }
    elseif ($manifest.captureStatus -eq 'captured-pending-background-stop')
    {
        $manifest.captureStatus = 'captured'
    }
    $manifest['completedAtUtc'] = [DateTime]::UtcNow.ToString(
        'yyyy-MM-ddTHH:mm:ss.fffZ')
    Write-JsonFile -Path $manifestPath -Value $manifest
}

if ($null -ne $collectionException)
{
    if ($null -ne $backgroundStopException)
    {
        $collectionException.Data['BackgroundStopFailure'] =
            $backgroundStopException.Message
    }
    throw $collectionException
}
if ($null -ne $backgroundStopException)
{
    throw $backgroundStopException
}

Write-Host "External recording matrix completed: $outputRoot"
