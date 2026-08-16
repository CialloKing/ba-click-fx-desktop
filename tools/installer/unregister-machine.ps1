[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InstallDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installer-diagnostics.ps1')
. (Join-Path $PSScriptRoot 'protected-paths.ps1')
$script:InstallerStep = 'initialize'
$script:InstallerProductVersion = ''
$script:InstallerPackageVersion = ''

function Assert-Administrator
{
    $principal = New-Object Security.Principal.WindowsPrincipal(
        [Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
    {
        throw 'Uninstall requires administrator privileges.'
    }
}

function Assert-ExpectedProcessIsStopped
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExecutablePath
    )

    $expected = [IO.Path]::GetFullPath($ExecutablePath)
    $running = Get-CimInstance Win32_Process -Filter "Name='$([IO.Path]::GetFileName($expected))'" |
        Where-Object {
            -not [string]::IsNullOrWhiteSpace($_.ExecutablePath)
                -and [IO.Path]::GetFullPath($_.ExecutablePath) -eq $expected
        } |
        Select-Object -First 1
    if ($null -ne $running)
    {
        throw "Close the running application before uninstalling: $expected"
    }
}

function Assert-ProtectedStateAcl
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $acl = Get-Acl -LiteralPath $Path
    if (-not $acl.AreAccessRulesProtected)
    {
        throw 'Protected install state still inherits writable access rules.'
    }
    $writeRights = [int]([Security.AccessControl.FileSystemRights]::WriteData -bor
        [Security.AccessControl.FileSystemRights]::AppendData -bor
        [Security.AccessControl.FileSystemRights]::WriteExtendedAttributes -bor
        [Security.AccessControl.FileSystemRights]::WriteAttributes -bor
        [Security.AccessControl.FileSystemRights]::Delete -bor
        [Security.AccessControl.FileSystemRights]::ChangePermissions -bor
        [Security.AccessControl.FileSystemRights]::TakeOwnership)
    foreach ($rule in $acl.Access)
    {
        if ($rule.AccessControlType -ne [Security.AccessControl.AccessControlType]::Allow)
        {
            continue
        }
        $sid = $rule.IdentityReference.Translate(
            [Security.Principal.SecurityIdentifier]).Value
        if ($sid -in @('S-1-5-18', 'S-1-5-32-544'))
        {
            continue
        }
        if (([int]$rule.FileSystemRights -band $writeRights) -ne 0)
        {
            throw 'Protected install state grants write access to a non-administrator.'
        }
    }
}

function Split-Ledger
{
    param(
        [AllowNull()]
        [object]$Value,

        [Parameter(Mandatory = $true)]
        [ValidateSet('Comma', 'Pipe')]
        [string]$Separator
    )

    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace([string]$Value))
    {
        return @()
    }
    $pattern = if ($Separator -eq 'Comma') { ',' } else { '\|' }
    return @(
        ([string]$Value -split $pattern) |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            ForEach-Object { $_.Trim() }
    )
}

function Read-InstallStateWithBackup
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot
    )

    $backupPath = "$Path.bak"
    $candidates = @(
        @($Path, $backupPath) |
            Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }
    )
    if ($candidates.Count -eq 0)
    {
        throw 'Protected install state is missing; refusing an imprecise uninstall.'
    }
    $errors = New-Object Collections.Generic.List[string]
    foreach ($candidate in $candidates)
    {
        try
        {
            Assert-ProtectedStateAcl -Path $candidate
            $candidateState = Get-Content -LiteralPath $candidate -Raw | ConvertFrom-Json
            if ([int]$candidateState.schema -notin @(1, 2) -or
                [string]$candidateState.packageName -ne 'CialloKing.BaClickFxDesktop' -or
                [string]$candidateState.applicationId -ne 'BaClickFxDesktop' -or
                [string]$candidateState.publisher -ne 'CN=BaClickFx.Local')
            {
                throw 'Protected install state has unsupported identity data.'
            }
            if ($null -eq $candidateState.PSObject.Properties['ownedCertificateThumbprints'])
            {
                $candidateState | Add-Member -NotePropertyName ownedCertificateThumbprints `
                    -NotePropertyValue ([string]$candidateState.certificateThumbprint)
            }
            if ($null -eq $candidateState.PSObject.Properties['ownedPackageFiles'])
            {
                $candidateState | Add-Member -NotePropertyName ownedPackageFiles `
                    -NotePropertyValue ([string]$candidateState.packageFile)
            }
            # Keep the backup immutable until the full state validation and
            # uninstall transaction succeed. Rewriting a corrupt primary here
            # would destroy the evidence needed for a later recovery attempt.
            return $candidateState
        }
        catch
        {
            $errors.Add("$candidate`: $($_.Exception.Message)")
        }
    }
    throw "Protected install state and its backup are invalid: $($errors -join ' | ')"
}

$script:InstallerStep = 'validate-administrator'
trap
{
    Write-BafxInstallerFailure `
        -ErrorRecord $_ `
        -Phase 'UninstallMachine' `
        -Step $script:InstallerStep `
        -ProductVersion $script:InstallerProductVersion `
        -PackageVersion $script:InstallerPackageVersion
    exit 1
}

Assert-Administrator
$script:InstallerStep = 'validate-powershell'
if ($PSVersionTable.PSEdition -ne 'Desktop')
{
    throw 'Uninstall requires Windows PowerShell 5.1.'
}

$script:InstallerStep = 'resolve-install-root'
$installRoot = Resolve-ProtectedProgramFilesPath `
    -Path $InstallDirectory `
    -Description 'uninstall directory'
$statePath = Join-Path $installRoot 'Installer\INSTALL-STATE.json'
$pendingPath = Join-Path $installRoot 'Installer\PREPARE-STATE.json'
$script:InstallerStep = 'check-pending-installation-transaction'
if (Test-Path -LiteralPath $pendingPath -PathType Leaf)
{
    throw 'A pending installation transaction remains; complete rollback before uninstalling.'
}
$script:InstallerStep = 'read-protected-install-state'
$state = Read-InstallStateWithBackup -Path $statePath -InstallRoot $installRoot
$script:InstallerStep = 'validate-protected-install-state'
foreach ($propertyName in @(
    'schema',
    'packageName',
    'applicationId',
    'publisher',
    'productVersion',
    'packageVersion',
    'packageFullName',
    'packageFamilyName',
    'certificateThumbprint',
    'certificateInstalledBySetup',
    'externalLocation',
    'installedUserSid',
    'packageFile',
    'ownedCertificateThumbprints',
    'ownedPackageFiles'))
{
    if ($null -eq $state.PSObject.Properties[$propertyName])
    {
        throw "Protected install state is missing: $propertyName"
    }
}
$script:InstallerProductVersion = [string]$state.productVersion
$script:InstallerPackageVersion = [string]$state.packageVersion
$stateInvalid = `
    ([int]$state.schema -notin @(1, 2)) -or `
    ([string]$state.packageName -ne 'CialloKing.BaClickFxDesktop') -or `
    ([string]$state.applicationId -ne 'BaClickFxDesktop') -or `
    ([string]$state.publisher -ne 'CN=BaClickFx.Local') -or `
    ([string]$state.productVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+-alpha\.[0-9]+$') -or `
    ([string]$state.packageVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$') -or `
    ([string]$state.packageFullName -notmatch '^CialloKing\.BaClickFxDesktop_[A-Za-z0-9._-]+$') -or `
    ([string]$state.packageFamilyName -notmatch '^CialloKing\.BaClickFxDesktop_[A-Za-z0-9-]+$') -or `
    ([string]$state.certificateThumbprint -notmatch '^[0-9A-Fa-f]{40}$') -or `
    ($state.certificateInstalledBySetup -isnot [bool]) -or `
    ([string]$state.installedUserSid -notmatch '^S-1-[0-9-]+$') -or `
    ([IO.Path]::GetFullPath([string]$state.externalLocation) -ne $installRoot)
if ($stateInvalid)
{
    throw 'Protected install state failed safety validation.'
}
$packageFile = [string]$state.packageFile
if ([IO.Path]::IsPathRooted($packageFile) -or
    $packageFile.Contains('..') -or
    [IO.Path]::GetFileName($packageFile) -ne $packageFile -or
    $packageFile -notmatch '\.msix$')
{
    throw 'Protected install state has an unsafe package file name.'
}

$script:InstallerStep = 'ensure-host-process-stopped'
Assert-ExpectedProcessIsStopped -ExecutablePath (Join-Path $installRoot 'ba-click-fx-desktop.exe')
$script:InstallerStep = 'ensure-control-center-process-stopped'
Assert-ExpectedProcessIsStopped -ExecutablePath (Join-Path $installRoot 'BAFX.ControlCenter.exe')

$script:InstallerStep = 'query-installed-user-package'
$registered = @(
    Get-AppxPackage `
        -User ([string]$state.installedUserSid) `
        -Name ([string]$state.packageName) `
        -ErrorAction Stop
)
$target = $registered |
    Where-Object { $_.PackageFullName -eq [string]$state.packageFullName } |
    Select-Object -First 1
if ($null -ne $target)
{
    $script:InstallerStep = 'remove-installed-user-package'
    Remove-AppxPackage `
        -Package $target.PackageFullName `
        -User ([string]$state.installedUserSid) `
        -ErrorAction Stop
}

$script:InstallerStep = 'wait-for-user-package-removal'
$remaining = @()
for ($attempt = 0; $attempt -lt 15; ++$attempt)
{
    $remaining = @(
        Get-AppxPackage `
            -User ([string]$state.installedUserSid) `
            -Name ([string]$state.packageName) `
            -ErrorAction Stop |
            Where-Object { $_.PackageFullName -eq [string]$state.packageFullName }
    )
    if ($remaining.Count -eq 0)
    {
        break
    }
    Start-Sleep -Milliseconds 200
}
if ($remaining.Count -gt 0)
{
    throw 'Sparse package registration remains after uninstall.'
}

$script:InstallerStep = 'query-other-user-packages'
$otherUserPackages = @(
    Get-AppxPackage -AllUsers -Name ([string]$state.packageName) -ErrorAction Stop
)
if ($otherUserPackages.Count -gt 0)
{
    throw 'Another user package registration remains; keeping shared files and certificate.'
}

$script:InstallerStep = 'remove-owned-identity-packages'
$ownedFiles = Split-Ledger -Value $state.ownedPackageFiles -Separator Pipe
foreach ($ownedFile in $ownedFiles)
{
    if ([IO.Path]::IsPathRooted($ownedFile) -or
        $ownedFile.Contains('..') -or
        [IO.Path]::GetFileName($ownedFile) -ne $ownedFile -or
        $ownedFile -notmatch '\.msix$')
    {
        throw 'Protected install state has an unsafe package ledger entry.'
    }
    $ownedPath = Join-Path (Join-Path $installRoot 'Identity') $ownedFile
    if (Test-Path -LiteralPath $ownedPath -PathType Leaf)
    {
        Remove-Item -LiteralPath $ownedPath -Force
    }
    if (Test-Path -LiteralPath $ownedPath -PathType Leaf)
    {
        throw "The installer-owned identity package remains after uninstall: $ownedFile"
    }
}

$script:InstallerStep = 'remove-owned-certificates'
foreach ($thumbprint in (Split-Ledger `
        -Value $state.ownedCertificateThumbprints `
        -Separator Comma))
{
    if ($thumbprint -notmatch '^[0-9A-Fa-f]{40}$')
    {
        throw 'Protected install state has an unsafe certificate ledger entry.'
    }
    $certificate = Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
        Where-Object { $_.Thumbprint -eq $thumbprint } |
        Select-Object -First 1
    if ($null -ne $certificate)
    {
        if ($certificate.Subject -ne [string]$state.publisher)
        {
            throw 'Refusing to remove a certificate with an unexpected subject.'
        }
        Remove-Item -LiteralPath $certificate.PSPath -Force
    }
    $privateCertificate = Get-ChildItem -Path 'Cert:\LocalMachine\My' |
        Where-Object { $_.Thumbprint -eq $thumbprint } |
        Select-Object -First 1
    if ($null -ne $privateCertificate)
    {
        if ($privateCertificate.Subject -ne [string]$state.publisher)
        {
            throw 'Refusing to remove a private certificate with an unexpected subject.'
        }
        Remove-Item -LiteralPath $privateCertificate.PSPath -DeleteKey -Force
    }
    if ($null -ne (Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
            Where-Object { $_.Thumbprint -eq $thumbprint } |
            Select-Object -First 1))
    {
        throw "The installer-owned certificate remains after uninstall: $thumbprint"
    }
}

$script:InstallerStep = 'delete-protected-install-state'
foreach ($installStatePath in @($statePath, "$statePath.bak"))
{
    if (Test-Path -LiteralPath $installStatePath -PathType Leaf)
    {
        Remove-Item -LiteralPath $installStatePath -Force
    }
}
