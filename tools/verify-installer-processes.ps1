[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installer/installer-processes.ps1')

function Assert-True([bool]$Condition, [string]$Message)
{
    if (-not $Condition) { throw $Message }
}

function New-Probe([int]$Reply, [bool]$Graceful)
{
    $probe = [pscustomobject]@{ ProcessId = 100; Reply = $Reply; Graceful = $Graceful;
        Closed = $false; Terminated = $false; Exited = $false }
    $probe | Add-Member ScriptMethod RequestMaintenanceClose {
        param($Timeout)
        if ($this.Reply -eq 1 -and $this.Graceful) { $this.Exited = $true }
        return $this.Reply
    }
    $probe | Add-Member ScriptMethod RequestClose {
        $this.Closed = $true
        if ($this.Graceful) { $this.Exited = $true }
        return 1
    }
    $probe | Add-Member ScriptMethod WaitExited { param($Timeout) return $this.Exited }
    $probe | Add-Member ScriptMethod Terminate { $this.Terminated = $true; $this.Exited = $true }
    return $probe
}

# Exercise decisions independently from Windows timing; real handle checks below
# ensure the process boundary is also used, without closing a user's application.
foreach ($reply in @(1, 0, 2, -1))
{
    foreach ($graceful in @($true, $false))
    {
        $probe = New-Probe $reply $graceful
        $failed = $false
        try { Stop-VerifiedProcesses -Processes @($probe) -ControlCenter -GracefulTimeoutMilliseconds 1 -ForceTimeoutMilliseconds 1 }
        catch { $failed = $true }
        $expectedFailure = $reply -notin @(0, 1) -or ($reply -eq 0 -and -not $graceful)
        Assert-True ($failed -eq $expectedFailure) "Wrong save policy: reply=$reply graceful=$graceful"
        Assert-True ($probe.Terminated -eq ($reply -eq 1 -and -not $graceful)) 'An unconfirmed draft was force-terminated.'
    }
}
$hostProbe = New-Probe 0 $false
Stop-VerifiedProcesses -Processes @($hostProbe) -GracefulTimeoutMilliseconds 1 -ForceTimeoutMilliseconds 1
Assert-True ($hostProbe.Closed -and $hostProbe.Terminated) 'Host did not use bounded shutdown.'

Initialize-InstallerProcessInterop
$current = Get-Process -Id $PID
$verified = [Bafx.Installer.RuntimeProcess]::Open($PID, $current.Path, $false)
try
{
    Assert-True ($null -ne $verified -and -not $verified.WaitExited(0)) 'Current process handle was not verified.'
    $other = [Bafx.Installer.RuntimeProcess]::Open($PID, (Join-Path $env:TEMP 'BAFX.ControlCenter.exe'), $false)
    Assert-True ($null -eq $other) 'Another directory was accepted as the installed process.'
}
finally { if ($null -ne $verified) { $verified.Dispose() } }
Write-Host 'Installer process identity and save/exit policies verified.'
