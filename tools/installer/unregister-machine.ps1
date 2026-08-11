[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InstallDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Administrator
{
    $principal = New-Object Security.Principal.WindowsPrincipal(
        [Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
    {
        throw 'Uninstall requires administrator privileges.'
    }
}

function Resolve-ProtectedInstallRoot
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $resolved = [IO.Path]::GetFullPath($Path)
    $roots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}) |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        ForEach-Object { [IO.Path]::GetFullPath($_).TrimEnd('\') + '\' }
    foreach ($root in $roots)
    {
        if ($resolved.StartsWith($root, [StringComparison]::OrdinalIgnoreCase))
        {
            return $resolved
        }
    }
    throw "The uninstall directory is outside Program Files: $resolved"
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
    $writeRights = [int]([Security.AccessControl.FileSystemRights]::Write -bor
        [Security.AccessControl.FileSystemRights]::Modify -bor
        [Security.AccessControl.FileSystemRights]::FullControl -bor
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

Assert-Administrator
if ($PSVersionTable.PSEdition -ne 'Desktop')
{
    throw 'Uninstall requires Windows PowerShell 5.1.'
}

$installRoot = Resolve-ProtectedInstallRoot -Path $InstallDirectory
$statePath = Join-Path $installRoot 'Installer\INSTALL-STATE.json'
if (-not (Test-Path -LiteralPath $statePath -PathType Leaf))
{
    throw 'Protected install state is missing; refusing an imprecise uninstall.'
}
Assert-ProtectedStateAcl -Path $statePath
$state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
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
    'packageFile'))
{
    if ($null -eq $state.PSObject.Properties[$propertyName])
    {
        throw "Protected install state is missing: $propertyName"
    }
}
$stateInvalid = `
    ([int]$state.schema -ne 1) -or `
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

Assert-ExpectedProcessIsStopped -ExecutablePath (Join-Path $installRoot 'ba-click-fx-desktop.exe')
Assert-ExpectedProcessIsStopped -ExecutablePath (Join-Path $installRoot 'BAFX.ControlCenter.exe')

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
    Remove-AppxPackage `
        -Package $target.PackageFullName `
        -User ([string]$state.installedUserSid) `
        -ErrorAction Stop
}

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

$otherUserPackages = @(
    Get-AppxPackage -AllUsers -Name ([string]$state.packageName) -ErrorAction Stop
)
if ($otherUserPackages.Count -gt 0)
{
    throw 'Another user package registration remains; keeping shared files and certificate.'
}

if ([bool]$state.certificateInstalledBySetup)
{
    $certificate = Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
        Where-Object { $_.Thumbprint -eq [string]$state.certificateThumbprint } |
        Select-Object -First 1
    if ($null -ne $certificate)
    {
        if ($certificate.Subject -ne [string]$state.publisher)
        {
            throw 'Refusing to remove a certificate with an unexpected subject.'
        }
        Remove-Item -LiteralPath $certificate.PSPath -Force
    }

    if ($null -ne (Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
            Where-Object { $_.Thumbprint -eq [string]$state.certificateThumbprint } |
            Select-Object -First 1))
    {
        throw 'The installer-owned certificate remains after uninstall.'
    }
}

Remove-Item -LiteralPath $statePath -Force
