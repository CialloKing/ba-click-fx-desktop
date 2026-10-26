[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -ne 'Desktop')
{
    & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $PSCommandPath
    if ($LASTEXITCODE -ne 0) { throw "Installer process checks failed: $LASTEXITCODE" }
    return
}
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
$hostProbe = New-Probe 0 $false
$hostProbe | Add-Member ScriptMethod Terminate { throw 'Termination denied.' } -Force
$rejected = $false
try { Stop-VerifiedProcesses -Processes @($hostProbe) -GracefulTimeoutMilliseconds 1 }
catch { $rejected = $true }
Assert-True $rejected 'A failed termination was reported as successful.'

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
$eventName = 'Local\BAFX.MaintenanceProbe.' + [Guid]::NewGuid().ToString('N')
$lease = Enter-InstallerMaintenance -EventName $eventName
try
{
    $secondRejected = $false
    try { $second = Enter-InstallerMaintenance -EventName $eventName; $second.Dispose() }
    catch { $secondRejected = $true }
    Assert-True $secondRejected 'A second maintenance owner was accepted.'
    $query = [Threading.EventWaitHandle]::OpenExisting($eventName,
        [Security.AccessControl.EventWaitHandleRights]::Synchronize)
    try { Assert-True ($query.WaitOne(0)) 'Maintenance event was not signaled.' }
    finally { $query.Dispose() }
    $workerLease = Open-InstallerMaintenance -EventName $eventName
}
finally { $lease.Dispose() }
try
{
    $secondRejected = $false
    try { $second = Enter-InstallerMaintenance -EventName $eventName; $second.Dispose() }
    catch { $secondRejected = $true }
    Assert-True $secondRejected 'Maintenance ended while a phase still held its lease.'
}
finally { $workerLease.Dispose() }
$lease = Enter-InstallerMaintenance -EventName $eventName
$lease.Dispose()

$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('bafx-process-test-' + [Guid]::NewGuid().ToString('N'))
$owned = New-Object Collections.Generic.List[object]
function Start-Fixture([string]$Path, [string]$Scenario, [string]$Ready)
{
    $child = Start-Process -FilePath $Path -ArgumentList @($Scenario, ('"' + $Ready + '"')) -WindowStyle Hidden -PassThru
    $null = $child.Handle
    $owned.Add($child)
    $deadline = [Diagnostics.Stopwatch]::StartNew()
    while (-not (Test-Path -LiteralPath $Ready))
    {
        if ($child.HasExited -or $deadline.ElapsedMilliseconds -gt 5000) { throw "Fixture did not start: $Scenario" }
        Start-Sleep -Milliseconds 20
    }
    return $child
}
try
{
    New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
    $fixture = Join-Path $fixtureRoot 'fixture.exe'
    Add-Type -Path (Join-Path $PSScriptRoot '../tests/installer_process_fixture.cs') `
        -OutputAssembly $fixture -OutputType WindowsApplication
    foreach ($scenario in @('host-graceful', 'host-hung', 'center-saved', 'center-saved-hung',
        'center-refuse', 'center-legacy', 'center-legacy-hung', 'center-timeout'))
    {
        $center = $scenario.StartsWith('center')
        $directory = Join-Path $fixtureRoot $scenario
        New-Item -ItemType Directory -Path $directory | Out-Null
        $path = Join-Path $directory $(if ($center) { 'BAFX.ControlCenter.exe' } else { 'ba-click-fx-desktop.exe' })
        Copy-Item -LiteralPath $fixture -Destination $path
        $ready = Join-Path $directory 'ready'
        $child = Start-Fixture $path $scenario $ready
        $count = 1
        if ($scenario -eq 'host-graceful')
        {
            $null = Start-Fixture $path $scenario ($ready + '-second')
            $count = 2
        }
        if ($scenario -eq 'center-refuse')
        {
            $hostPath = Join-Path $directory 'ba-click-fx-desktop.exe'
            Copy-Item -LiteralPath $fixture -Destination $hostPath
            $hostChild = Start-Fixture $hostPath 'host-graceful' ($ready + '-host')
            $rejected = $false
            try { Stop-InstalledProcesses -InstallRoot $directory }
            catch { $rejected = $true }
            Assert-True ($rejected -and -not $hostChild.HasExited -and -not $child.HasExited) 'Host exited before Control Center saved.'
        }
        $handles = @(Get-ExpectedProcesses -ExecutablePath $path -AllowTerminate)
        try
        {
            Assert-True ($handles.Count -eq $count) "Wrong target set: $scenario"
            $rejected = $false
            try { Stop-VerifiedProcesses -Processes $handles -ControlCenter:$center -GracefulTimeoutMilliseconds 200 }
            catch { $rejected = $true }
            Assert-True ($rejected -eq ($scenario -in @('center-refuse', 'center-legacy-hung', 'center-timeout'))) "Unexpected native stop result: $scenario"
            if ($rejected) { Assert-True (-not $child.HasExited) 'Refused save was terminated.' }
            else
            {
                Assert-True ($child.WaitForExit(3000)) "Native process remained: $scenario"
                $expectedCode = if ($scenario.EndsWith('hung')) { 1 } else { 0 }
                Assert-True ($child.ExitCode -eq $expectedCode) "Wrong native exit method: $scenario"
            }
        }
        finally { foreach ($handle in $handles) { $handle.Dispose() } }
    }
}
finally
{
    foreach ($child in $owned)
    {
        if (-not $child.HasExited) { $child.Kill(); $null = $child.WaitForExit(3000) }
        $child.Dispose()
    }
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    $parent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($parent, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notlike 'bafx-process-test-*')
    {
        throw 'Refusing fixture cleanup outside its temporary directory.'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
Write-Host 'Installer process identity, save/exit policies and maintenance ownership verified.'
