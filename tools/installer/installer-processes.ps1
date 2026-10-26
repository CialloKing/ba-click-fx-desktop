[CmdletBinding()]
param(
    [Alias('Mode')][ValidateSet('Check', 'Stop')][string]$ProcessMode,
    [Alias('InstallDirectory')][string]$ProcessInstallDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installer-diagnostics.ps1')
. (Join-Path $PSScriptRoot 'protected-paths.ps1')

function Initialize-InstallerProcessInterop
{
    if ($null -ne ('Bafx.Installer.RuntimeProcess' -as [type]))
    {
        return
    }
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace Bafx.Installer
{
    public sealed class RuntimeProcess : IDisposable
    {
        private readonly SafeWaitHandle handle;
        public int ProcessId { get; private set; }
        public int LastCloseError { get; private set; }

        private RuntimeProcess(int processId, SafeWaitHandle processHandle)
        {
            ProcessId = processId;
            handle = processHandle;
        }

        public static RuntimeProcess Open(int processId, string expectedPath, bool allowTerminate)
        {
            // Reuse one verified handle throughout shutdown to avoid PID reuse.
            // SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE.
            SafeWaitHandle processHandle = OpenProcess(0x00101000 | (allowTerminate ? 1U : 0U), false, processId);
            bool retained = false;
            try
            {
                if (processHandle.IsInvalid)
                {
                    int error = Marshal.GetLastWin32Error();
                    if (error == 87 || error == 1168)
                    {
                        return null;
                    }
                    throw new Win32Exception(error, "Cannot open process " + processId + "; win32=" + error);
                }
                StringBuilder imagePath = new StringBuilder(32768);
                int length = imagePath.Capacity;
                if (!QueryFullProcessImageName(processHandle, 0, imagePath, ref length))
                {
                    int error = Marshal.GetLastWin32Error();
                    if (WaitForSingleObject(processHandle, 0) == 0)
                    {
                        return null;
                    }
                    throw new Win32Exception(error, "Cannot verify process " + processId + "; win32=" + error);
                }
                if (!String.Equals(Path.GetFullPath(imagePath.ToString()),
                    Path.GetFullPath(expectedPath), StringComparison.OrdinalIgnoreCase))
                {
                    return null;
                }
                RuntimeProcess result = new RuntimeProcess(processId, processHandle);
                retained = true;
                return result;
            }
            finally
            {
                if (!retained)
                {
                    processHandle.Dispose();
                }
            }
        }

        // 0 is the legacy DefWindowProc reply; never treat it as saved data.
        public int RequestMaintenanceClose(int timeoutMilliseconds)
        {
            int reply = -1;
            uint message = RegisterWindowMessage("BAFX.PrepareForMaintenance.v1");
            if (message == 0)
            {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            EnumWindowProc callback = delegate(IntPtr window, IntPtr unused)
            {
                uint owner;
                GetWindowThreadProcessId(window, out owner);
                StringBuilder name = new StringBuilder(256);
                GetClassName(window, name, name.Capacity);
                if (owner != (uint)ProcessId || name.ToString() != "BAFX.NativeControlCenter.Window.v1")
                {
                    return true;
                }
                if (WaitExited(0))
                {
                    return false;
                }
                UIntPtr result;
                // Do not broadcast: each unrelated window would get its own timeout.
                if (SendMessageTimeout(window, message, UIntPtr.Zero, IntPtr.Zero,
                    0x0002, (uint)Math.Max(1, timeoutMilliseconds), out result) != IntPtr.Zero)
                {
                    reply = unchecked((int)result.ToUInt64());
                }
                return false;
            };
            EnumWindows(callback, IntPtr.Zero);
            return reply;
        }

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "RegisterWindowMessageW")]
        private static extern uint RegisterWindowMessage(string name);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "SendMessageTimeoutW")]
        private static extern IntPtr SendMessageTimeout(IntPtr window, uint message, UIntPtr wParam,
            IntPtr lParam, uint flags, uint timeout, out UIntPtr result);

        public int RequestClose()
        {
            int posted = 0;
            LastCloseError = 0;
            if (WaitExited(0))
            {
                return posted;
            }
            // Hidden tray windows are included. Other sessions still participate
            // in the bounded termination fallback when their desktop is absent.
            EnumWindowProc callback = delegate(IntPtr window, IntPtr unused)
            {
                uint owner;
                GetWindowThreadProcessId(window, out owner);
                if (owner == (uint)ProcessId)
                {
                    StringBuilder className = new StringBuilder(256);
                    GetClassName(window, className, className.Capacity);
                    if (className.ToString() == "BAFX.NativeControlCenter.Window.v1"
                        || className.ToString() == "BaClickFxDesktopOverlay")
                    {
                        if (PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero))
                        {
                            ++posted;
                        }
                        else
                        {
                            LastCloseError = Marshal.GetLastWin32Error();
                        }
                    }
                }
                return true;
            };
            if (!EnumWindows(callback, IntPtr.Zero))
            {
                int error = Marshal.GetLastWin32Error();
                throw new Win32Exception(error, "Cannot enumerate windows for process " + ProcessId + "; win32=" + error);
            }
            return posted;
        }

        public bool WaitExited(int milliseconds)
        {
            uint result = WaitForSingleObject(handle, (uint)milliseconds);
            if (result == 0)
            {
                return true;
            }
            if (result == 258)
            {
                return false;
            }
            int error = Marshal.GetLastWin32Error();
            throw new Win32Exception(error, "Cannot wait for process " + ProcessId + "; win32=" + error);
        }

        public void Terminate()
        {
            if (!WaitExited(0) && !TerminateProcess(handle, 1))
            {
                int error = Marshal.GetLastWin32Error();
                if (!WaitExited(0))
                {
                    throw new Win32Exception(error, "Cannot terminate process " + ProcessId + "; win32=" + error);
                }
            }
        }

        public void Dispose()
        {
            handle.Dispose();
        }

        private delegate bool EnumWindowProc(IntPtr window, IntPtr parameter);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern SafeWaitHandle OpenProcess(uint access, bool inherit, int processId);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "QueryFullProcessImageNameW")]
        private static extern bool QueryFullProcessImageName(SafeWaitHandle process, uint flags, StringBuilder path, ref int length);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint WaitForSingleObject(SafeWaitHandle process, uint milliseconds);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool TerminateProcess(SafeWaitHandle process, uint exitCode);
        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool EnumWindows(EnumWindowProc callback, IntPtr parameter);
        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetClassNameW")]
        private static extern int GetClassName(IntPtr window, StringBuilder name, int count);
        [DllImport("user32.dll", SetLastError = true, EntryPoint = "PostMessageW")]
        private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    }
}
'@
}

function Open-ExpectedProcess
{
    param([int]$ProcessId, [string]$ExecutablePath, [bool]$AllowTerminate = $false)

    Initialize-InstallerProcessInterop
    return [Bafx.Installer.RuntimeProcess]::Open($ProcessId, $ExecutablePath, $AllowTerminate)
}

function Get-ExpectedProcesses
{
    param([Parameter(Mandatory = $true)][string]$ExecutablePath, [switch]$AllowTerminate)

    $expected = [IO.Path]::GetFullPath($ExecutablePath)
    $processes = New-Object Collections.Generic.List[object]
    try
    {
        foreach ($candidate in @(Get-CimInstance Win32_Process `
                -Filter "Name='$([IO.Path]::GetFileName($expected))'" -ErrorAction Stop))
        {
            if (-not [string]::IsNullOrWhiteSpace($candidate.ExecutablePath) -and
                [IO.Path]::GetFullPath($candidate.ExecutablePath) -ne $expected)
            {
                continue
            }
            # CIM only supplies candidates. Unknown paths require a handle query
            # instead of silently skipping a potentially installed process.
            $pathDetail = ConvertTo-BafxInstallerSummaryValue -Value $expected
            Write-Host "BAFX_INSTALL_PROCESS: path=$pathDetail; pid=$($candidate.ProcessId); action=verify-identity"
            $process = Open-ExpectedProcess `
                -ProcessId ([int]$candidate.ProcessId) -ExecutablePath $expected -AllowTerminate ([bool]$AllowTerminate)
            if ($null -ne $process)
            {
                $processes.Add($process)
            }
        }
        return $processes.ToArray()
    }
    catch
    {
        foreach ($process in $processes)
        {
            $process.Dispose()
        }
        throw
    }
}

function Assert-ExpectedProcessIsStopped
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExecutablePath
    )

    $processes = @(Get-ExpectedProcesses -ExecutablePath $ExecutablePath)
    try
    {
        foreach ($process in $processes)
        {
            if (-not $process.WaitExited(0))
            {
                throw "Application is still running before uninstall: $ExecutablePath (PID $($process.ProcessId))"
            }
        }
    }
    finally
    {
        foreach ($process in $processes)
        {
            $process.Dispose()
        }
    }
}

function Assert-InstalledProcessesStopped
{
    param([Parameter(Mandatory = $true)][string]$InstallRoot)
    Assert-ExpectedProcessIsStopped -ExecutablePath (Join-Path $InstallRoot 'BAFX.ControlCenter.exe')
    Assert-ExpectedProcessIsStopped -ExecutablePath (Join-Path $InstallRoot 'ba-click-fx-desktop.exe')
}

function Stop-VerifiedProcesses
{
    param([object[]]$Processes, [switch]$ControlCenter,
        [ValidateRange(1, 10000)][int]$GracefulTimeoutMilliseconds = 10000,
        [ValidateRange(1, 3000)][int]$ForceTimeoutMilliseconds = 3000)

    $timer = [Diagnostics.Stopwatch]::StartNew()
    $mayTerminate = @{}
    foreach ($process in $Processes)
    {
        if ($process.WaitExited(0)) { continue }
        $allowed = -not $ControlCenter
        if ($ControlCenter)
        {
            $remaining = [Math]::Max(1, $GracefulTimeoutMilliseconds - [int]$timer.ElapsedMilliseconds)
            $reply = $process.RequestMaintenanceClose($remaining)
            Write-Host "BAFX_INSTALL_PROCESS: pid=$($process.ProcessId); action=save-reply; result=$reply"
            if ($reply -eq 1)
            {
                $allowed = $true
            }
            elseif ($reply -eq 0)
            {
                # Old versions may show their existing save/discard dialog.
                # A missing acknowledgement never authorizes forced data loss.
                $null = $process.RequestClose()
            }
            elseif (-not $process.WaitExited(0))
            {
                throw "Control Center did not confirm saved changes (PID $($process.ProcessId)); close it manually and retry."
            }
        }
        else
        {
            $null = $process.RequestClose()
        }
        $mayTerminate[$process.ProcessId] = $allowed
    }
    foreach ($process in $Processes)
    {
        $remaining = [Math]::Max(0, $GracefulTimeoutMilliseconds - [int]$timer.ElapsedMilliseconds)
        if (-not $process.WaitExited($remaining))
        {
            if (-not $mayTerminate[$process.ProcessId])
            {
                throw "Control Center is still running without a save acknowledgement (PID $($process.ProcessId))."
            }
            Write-Host "BAFX_INSTALL_PROCESS: pid=$($process.ProcessId); action=force-terminate"
            $process.Terminate()
        }
    }
    $timer.Restart()
    foreach ($process in $Processes)
    {
        $remaining = [Math]::Max(0, $ForceTimeoutMilliseconds - [int]$timer.ElapsedMilliseconds)
        if (-not $process.WaitExited($remaining))
        {
            throw "Process did not exit after termination (PID $($process.ProcessId))."
        }
        Write-Host "BAFX_INSTALL_PROCESS: pid=$($process.ProcessId); action=exit-confirmed"
    }
}

function Stop-InstalledProcesses
{
    param([Parameter(Mandatory = $true)][string]$InstallRoot)
    $centers = @()
    $hosts = @()
    try
    {
        # Verify every target before requesting any exit, and keep the handles.
        $centers = @(Get-ExpectedProcesses -ExecutablePath (Join-Path $InstallRoot 'BAFX.ControlCenter.exe') -AllowTerminate)
        $hosts = @(Get-ExpectedProcesses -ExecutablePath (Join-Path $InstallRoot 'ba-click-fx-desktop.exe') -AllowTerminate)
        Stop-VerifiedProcesses -Processes $centers -ControlCenter
        Stop-VerifiedProcesses -Processes $hosts
        Assert-InstalledProcessesStopped -InstallRoot $InstallRoot
    }
    finally
    {
        foreach ($process in @($centers) + @($hosts)) { $process.Dispose() }
    }
}

function Enter-InstallerMaintenance
{
    param([string]$EventName = 'Global\BAFX.InstallerMaintenance.v1')
    # Standalone repair commands own the same event as Inno. Only query access
    # is granted to normal users; the event lives exactly as long as its handles.
    $security = New-Object Security.AccessControl.EventWaitHandleSecurity
    foreach ($sid in @('S-1-5-18', 'S-1-5-32-544'))
    {
        $security.AddAccessRule((New-Object Security.AccessControl.EventWaitHandleAccessRule(
            (New-Object Security.Principal.SecurityIdentifier($sid)),
            [Security.AccessControl.EventWaitHandleRights]::FullControl,
            [Security.AccessControl.AccessControlType]::Allow)))
    }
    $security.AddAccessRule((New-Object Security.AccessControl.EventWaitHandleAccessRule(
        (New-Object Security.Principal.SecurityIdentifier('S-1-5-11')),
        [Security.AccessControl.EventWaitHandleRights]::Synchronize,
        [Security.AccessControl.AccessControlType]::Allow)))
    $created = $false
    $handle = New-Object Threading.EventWaitHandle($true, [Threading.EventResetMode]::ManualReset,
        $EventName, [ref]$created, $security)
    if (-not $created)
    {
        $handle.Dispose()
        throw 'Another installation, uninstall or repair is already in progress.'
    }
    return $handle
}

function Assert-InstallerMaintenanceActive
{
    $event = [Threading.EventWaitHandle]::OpenExisting('Global\BAFX.InstallerMaintenance.v1',
        [Security.AccessControl.EventWaitHandleRights]::Synchronize)
    try
    {
        if (-not $event.WaitOne(0)) { throw 'Installer maintenance is not active.' }
    }
    finally { $event.Dispose() }
}

if (-not [string]::IsNullOrWhiteSpace($ProcessMode))
{
    try
    {
        if ($PSVersionTable.PSEdition -ne 'Desktop') { throw 'Windows PowerShell 5.1 is required.' }
        $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
        if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
        {
            throw 'Installer process management requires administrator privileges.'
        }
        $root = Resolve-ProtectedProgramFilesPath -Path $ProcessInstallDirectory -Description 'installation directory'
        Assert-NoReparsePath -Path $root -AllowMissing
        if ($ProcessMode -eq 'Stop')
        {
            Assert-InstallerMaintenanceActive
            Stop-InstalledProcesses -InstallRoot $root
        }
        else { Assert-InstalledProcessesStopped -InstallRoot $root }
    }
    catch
    {
        Write-BafxInstallerFailure -ErrorRecord $_ -Phase 'RuntimeProcesses' -Step $ProcessMode
        exit 1
    }
}
