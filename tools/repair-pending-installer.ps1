[CmdletBinding()]
param(
    [string]$InstallDirectory = (Join-Path $env:ProgramFiles 'ba-click-fx-desktop'),
    [switch]$Apply
)

# This repair is limited to a failed schema 3 upgrade before file commit.
# Without -Apply it only verifies the existing installation and registration.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -ne 'Desktop')
{
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath,
        '-InstallDirectory', $InstallDirectory)
    if ($Apply) { $arguments += '-Apply' }
    & powershell.exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "Pending installation repair failed: $LASTEXITCODE" }
    return
}

$installerSources = Join-Path $PSScriptRoot 'installer'
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $installerSources 'install-machine.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw 'The machine installer source could not be parsed.' }
# Import functions only: do not execute the installer's top-level phases.
$functionText = @($ast.EndBlock.Statements |
    Where-Object { $_ -is [Management.Automation.Language.FunctionDefinitionAst] } |
    ForEach-Object { $_.Extent.Text }) -join "`n"
$module = New-Module -ScriptBlock {
    param($Sources, $Functions)
    Set-StrictMode -Version Latest
    . (Join-Path $Sources 'installer-diagnostics.ps1')
    . (Join-Path $Sources 'protected-paths.ps1')
    . (Join-Path $Sources 'installer-state.ps1')
    . (Join-Path $Sources 'installer-identity.ps1')
    . (Join-Path $Sources 'installer-processes.ps1')
    . ([scriptblock]::Create($Functions))
} -ArgumentList $installerSources, $functionText
$maintenance = $null
. (Join-Path $installerSources 'installer-processes.ps1')
try
{
    if ($Apply)
    {
        & $module { Assert-Administrator }
        $maintenance = Enter-InstallerMaintenance
        $verifiedRoot = Resolve-ProtectedProgramFilesPath -Path $InstallDirectory -Description 'repair directory'
        Assert-NoReparsePath -Path $verifiedRoot
        Stop-InstalledProcesses -InstallRoot $verifiedRoot
    }
    $backupParent = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\artifacts\local'))
    & $module {
        param($InstallDirectory, $Apply, $BackupParent)
        Add-Type -AssemblyName System.IO.Compression
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        Add-Type -AssemblyName System.Security
        $installRoot = Resolve-ProtectedProgramFilesPath -Path $InstallDirectory -Description 'install directory'
        Assert-NoReparsePath -Path $installRoot
        $pendingPath = Join-Path $installRoot 'Installer\PREPARE-STATE.json'
        if (-not (Test-Path -LiteralPath $pendingPath -PathType Leaf))
        {
            Write-Host 'There is no pending installation to repair.'
            return
        }
        Assert-ProtectedStateAcl -Path $pendingPath
        $payloadRoot = Join-Path $installRoot '.staging\current'
        $state = Assert-PendingStateObject -State (Get-Content -LiteralPath $pendingPath -Raw | ConvertFrom-Json) `
            -InstallRoot $installRoot -PayloadDirectory $payloadRoot
        if ([int]$state.schema -ne 3 -or $state.productVersion -ne '0.2.20' -or
            $state.filesCommitted -isnot [bool] -or [bool]$state.filesCommitted -or
            [string]$state.commitState -ne 'prepared' -or
            [string]$state.certificatePhase -ne 'ready' -or $null -eq $state.oldInstallState)
        {
            throw 'This repair requires a 0.2.20 upgrade that stopped before file commit.'
        }
        $currentSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
        if ([string]$state.userSid -ne $currentSid)
        {
            throw 'Run the repair as the same Windows user who installed the application.'
        }
        $old = Read-OldInstallState -InstallRoot $installRoot -UserSid $currentSid -AllowCertificateRepair
        if ((Get-StateDigest -Value $old) -ine (Get-StateDigest -Value $state.oldInstallState))
        {
            throw 'The live installation no longer matches the previous installation in the pending journal.'
        }
        $actualNames = @(Get-AppxPackage -Name ([string]$state.packageName) -ErrorAction Stop |
            ForEach-Object { [string]$_.PackageFullName })
        $expectedNames = @($state.preexistingPackageFullNames)
        if ($actualNames.Count -ne $expectedNames.Count -or
            @($actualNames | Where-Object { $expectedNames -notcontains $_ }).Count -ne 0)
        {
            throw 'The previous package registration changed; recovery material was retained.'
        }
        Write-Host "Verified previous version $($old.productVersion): protected state pair, Host, package and registration."
        Write-Host "Pending transaction: $($state.transactionId); filesCommitted=false."
        if (-not $Apply)
        {
            Write-Host 'Validation only. Close Setup and run this script in an administrator Windows PowerShell with -Apply to repair.'
            return
        }
        Assert-Administrator
        Assert-InstalledProcessesStopped -InstallRoot $installRoot

        $backupRoot = Join-Path $BackupParent ('installer-recovery-' +
            [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N'))
        Assert-NoReparsePath -Path $backupRoot -AllowMissing
        New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
        foreach ($name in @('Installer', 'data'))
        {
            $source = Join-Path $installRoot $name
            if (Test-Path -LiteralPath $source)
            {
                Assert-NoReparseTree -Path $source
                Copy-Item -LiteralPath $source -Destination $backupRoot -Recurse -Force
            }
        }
        Get-ChildItem -LiteralPath ([IO.Path]::GetTempPath()) -Filter 'Setup Log*.txt' -File |
            Copy-Item -Destination $backupRoot -Force
        if ($null -ne $state.rollbackIdentity -and
            ($null -eq $state.PSObject.Properties['rollbackComplete'] -or -not [bool]$state.rollbackComplete))
        {
            $evidenceRoot = Join-Path $installRoot ('.rollback\' + [string]$state.transactionId)
            Assert-NoReparsePath -Path $evidenceRoot
            Ensure-RollbackEvidenceAcl -RollbackRoot $evidenceRoot
            Assert-NoReparseTree -Path $evidenceRoot
            Copy-Item -LiteralPath $evidenceRoot -Destination (Join-Path $backupRoot 'rollback') -Recurse -Force
        }
        Invoke-PendingRollbackCleanup -State $state -InstallRoot $installRoot `
            -PayloadRoot $payloadRoot -PendingPath $pendingPath
        $verified = Read-OldInstallState -InstallRoot $installRoot -UserSid $currentSid -AllowCertificateRepair
        if ((Get-StateDigest -Value $verified) -ine (Get-StateDigest -Value $old) -or
            (Test-Path -LiteralPath $pendingPath))
        {
            throw "Repair verification failed. Backup: $backupRoot"
        }
        Write-Host "Repair completed. Version $($old.productVersion) and its data were preserved."
        Write-Host "Backup: $backupRoot"
        Write-Host 'Use an installer rebuilt with the directory ACL fix for the next upgrade; the original 0.2.20 installer repeats the defect.'
    } $InstallDirectory ([bool]$Apply) $backupParent
}
finally
{
    if ($null -ne $maintenance) { $maintenance.Dispose() }
    Remove-Module -ModuleInfo $module -Force -ErrorAction SilentlyContinue
}
