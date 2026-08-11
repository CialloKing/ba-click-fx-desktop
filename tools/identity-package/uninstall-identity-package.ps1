[CmdletBinding()]
param(
    [string]$InstallDirectory = "$env:ProgramFiles\ba-click-fx-desktop",
    [string]$PackageName = 'CialloKing.BaClickFxDesktop',
    [switch]$RemoveUserData,
    [switch]$RemoveProgramFiles,
    [switch]$AllowUserWritableInstall
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Administrator
{
    $principal = New-Object Security.Principal.WindowsPrincipal(
        [Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
    {
        throw '方案 C 卸载必须在管理员 PowerShell 中运行。'
    }
}

function Assert-WindowsPowerShell
{
    if ($PSVersionTable.PSEdition -ne 'Desktop')
    {
        throw '卸载 Sparse Package 需要 Windows PowerShell 5.1，请使用 powershell.exe。'
    }
    foreach ($commandName in @('Get-AppxPackage', 'Remove-AppxPackage'))
    {
        if ($null -eq (Get-Command $commandName -ErrorAction SilentlyContinue))
        {
            throw "Required Appx cmdlet is unavailable: $commandName"
        }
    }
}

function Assert-SafeIdentityValues
{
    if ($PackageName -notmatch '^[A-Za-z0-9.\-]{3,50}$')
    {
        throw "Invalid PackageName: $PackageName"
    }
}

function Get-OptionalMetadataValue
{
    param(
        [AllowNull()]
        [object]$Metadata,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    if ($null -eq $Metadata)
    {
        return $null
    }
    $property = $Metadata.PSObject.Properties[$Name]
    if ($null -eq $property)
    {
        return $null
    }
    return $property.Value
}

function Resolve-SafeInstallRoot
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $resolved = [IO.Path]::GetFullPath($Path)
    $filesystemRoot = [IO.Path]::GetPathRoot($resolved)
    if ([string]::IsNullOrWhiteSpace($filesystemRoot) -or $resolved.TrimEnd('\') -eq $filesystemRoot.TrimEnd('\'))
    {
        throw "Refusing to operate on a filesystem root: $resolved"
    }
    return $resolved
}

function Assert-ProgramFilesInstallRoot
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $programFilesRoots = @(
        $env:ProgramFiles,
        ${env:ProgramFiles(x86)}
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        ForEach-Object { [IO.Path]::GetFullPath($_) }
    foreach ($root in $programFilesRoots)
    {
        if ($Path.StartsWith($root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase))
        {
            return
        }
    }
    if (-not $AllowUserWritableInstall)
    {
        throw 'Refusing to remove a program directory outside Program Files; use -AllowUserWritableInstall only for a disposable local Spike.'
    }
}

function Assert-HostIsNotRunning
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $resolvedPath = [IO.Path]::GetFullPath($Path)
    $running = Get-Process -Name 'ba-click-fx-desktop' -ErrorAction SilentlyContinue
    foreach ($process in $running)
    {
        try
        {
            if ($process.Path -eq $resolvedPath)
            {
                throw "Host is still running from the identity install (PID $($process.Id))."
            }
        }
        catch [System.ComponentModel.Win32Exception]
        {
            # An unrelated process path may be inaccessible; leave it alone.
        }
    }
}

function Read-InstallMetadata
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$MetadataPath,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedPackageName,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedInstallRoot
    )

    if (-not (Test-Path -LiteralPath $MetadataPath -PathType Leaf))
    {
        return $null
    }
    try
    {
        $metadata = Get-Content -LiteralPath $MetadataPath -Raw | ConvertFrom-Json
    }
    catch
    {
        throw "Install metadata is not valid JSON: $MetadataPath"
    }
    $metadataPackageName = Get-OptionalMetadataValue -Metadata $metadata -Name 'packageName'
    if ([string]::IsNullOrWhiteSpace([string]$metadataPackageName))
    {
        throw 'Install metadata has no package name.'
    }
    if ([string]$metadataPackageName -ne $ExpectedPackageName)
    {
        throw 'Install metadata belongs to a different package name.'
    }
    $metadataExternalLocation = Get-OptionalMetadataValue -Metadata $metadata -Name 'externalLocation'
    if ([string]::IsNullOrWhiteSpace([string]$metadataExternalLocation))
    {
        throw 'Install metadata has no external location.'
    }
    if (-not [string]::IsNullOrWhiteSpace([string]$metadataExternalLocation))
    {
        $recordedRoot = [IO.Path]::GetFullPath([string]$metadataExternalLocation)
        if ($recordedRoot.TrimEnd('\') -ne $ExpectedInstallRoot.TrimEnd('\'))
        {
            throw 'Install metadata points to a different external location.'
        }
    }
    return $metadata
}

function Read-CertificateMetadata
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$IdentityDirectory,

        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [object]$InstallMetadata
    )

    if ($null -ne (Get-OptionalMetadataValue -Metadata $InstallMetadata -Name 'certificateThumbprint'))
    {
        return $InstallMetadata
    }
    if (-not (Test-Path -LiteralPath $IdentityDirectory -PathType Container))
    {
        return $null
    }
    $metadataFile = Get-ChildItem -LiteralPath $IdentityDirectory -Filter '*.identity.json' -File |
        Select-Object -First 1
    if ($null -eq $metadataFile)
    {
        return $null
    }
    try
    {
        return Get-Content -LiteralPath $metadataFile.FullName -Raw | ConvertFrom-Json
    }
    catch
    {
        throw "Identity package metadata is not valid JSON: $($metadataFile.FullName)"
    }
}

function Remove-AppxRegistrationAndVerify
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$PackageFullName
    )

    $registered = @(Get-AppxPackage -Name $Name -ErrorAction Stop)
    $otherPackages = @($registered |
        Where-Object { [string]$_.PackageFullName -ne $PackageFullName })
    if ($otherPackages.Count -gt 0)
    {
        throw "Refusing to remove package $Name because another registration remains: $($otherPackages.PackageFullName -join ', ')"
    }
    $target = @($registered |
        Where-Object { [string]$_.PackageFullName -eq $PackageFullName })
    if ($target.Count -eq 0)
    {
        return
    }
    Remove-AppxPackage -Package $PackageFullName -ErrorAction Stop
    for ($attempt = 0; $attempt -lt 15; ++$attempt)
    {
        $remaining = @(Get-AppxPackage -Name $Name -ErrorAction Stop |
            Where-Object { [string]$_.PackageFullName -eq $PackageFullName })
        if ($remaining.Count -eq 0)
        {
            $otherPackages = @(Get-AppxPackage -Name $Name -ErrorAction Stop |
                Where-Object { [string]$_.PackageFullName -ne $PackageFullName })
            if ($otherPackages.Count -eq 0)
            {
                return
            }
            throw "Another Appx package registration remains after uninstall: $($otherPackages.PackageFullName -join ', ')"
        }
        Start-Sleep -Milliseconds 200
    }
    $remaining = @(Get-AppxPackage -Name $Name -ErrorAction Stop |
        Where-Object { [string]$_.PackageFullName -eq $PackageFullName })
    if ($remaining.Count -gt 0)
    {
        throw "Appx package registration remains after uninstall: $($remaining.PackageFullName -join ', ')"
    }
}

Assert-Administrator
Assert-WindowsPowerShell
Assert-SafeIdentityValues
if ($RemoveProgramFiles -and -not $RemoveUserData)
{
    throw 'Removing the complete install directory also removes data; pass -RemoveUserData explicitly to confirm.'
}

$installRoot = Resolve-SafeInstallRoot -Path $InstallDirectory
if ($RemoveProgramFiles)
{
    Assert-ProgramFilesInstallRoot -Path $installRoot
}
$dataDirectory = Join-Path $installRoot 'data'
$metadataPath = Join-Path $dataDirectory 'identity-install.json'
$identityDirectory = Join-Path $installRoot 'Identity'
$metadata = Read-InstallMetadata `
    -MetadataPath $metadataPath `
    -ExpectedPackageName $PackageName `
    -ExpectedInstallRoot $installRoot
$certificateMetadata = Read-CertificateMetadata `
    -IdentityDirectory $identityDirectory `
    -InstallMetadata $metadata
if (($RemoveUserData -or $RemoveProgramFiles) -and $null -eq $metadata)
{
    throw 'Install metadata is required before removing user data or the complete program directory.'
}
if ($null -eq $metadata -and $null -ne $certificateMetadata)
{
    $fallbackPackageName = Get-OptionalMetadataValue `
        -Metadata $certificateMetadata `
        -Name 'packageName'
    $fallbackHostDirectory = Get-OptionalMetadataValue `
        -Metadata $certificateMetadata `
        -Name 'hostDirectory'
    if ([string]$fallbackPackageName -ne $PackageName -or
        [string]::IsNullOrWhiteSpace([string]$fallbackHostDirectory) -or
        [IO.Path]::GetFullPath([string]$fallbackHostDirectory).TrimEnd('\') -ne $installRoot.TrimEnd('\'))
    {
        throw 'Identity package metadata does not match the requested install directory.'
    }
}
if ($null -eq $metadata -and $null -eq $certificateMetadata -and
    (Test-Path -LiteralPath $identityDirectory -PathType Container))
{
    throw 'Refusing to remove an Identity directory without verifiable package metadata.'
}

Assert-HostIsNotRunning -Path (Join-Path $installRoot 'ba-click-fx-desktop.exe')
$packageFullNameValue = Get-OptionalMetadataValue -Metadata $metadata -Name 'packageFullName'
if ([string]::IsNullOrWhiteSpace([string]$packageFullNameValue))
{
    $untrackedPackages = @(Get-AppxPackage -Name $PackageName -ErrorAction Stop)
    if ($untrackedPackages.Count -gt 0)
    {
        throw 'Install metadata has no package full name; refusing to remove an untracked Appx registration.'
    }
}
else
{
    Remove-AppxRegistrationAndVerify `
        -Name $PackageName `
        -PackageFullName ([string]$packageFullNameValue)
}

$thumbprint = $null
$publisher = $null
if ($null -ne $certificateMetadata)
{
    $metadataThumbprint = Get-OptionalMetadataValue `
        -Metadata $certificateMetadata `
        -Name 'certificateThumbprint'
    $metadataPublisher = Get-OptionalMetadataValue `
        -Metadata $certificateMetadata `
        -Name 'publisher'
    if ($null -ne $metadataThumbprint)
    {
        $thumbprint = ([string]$metadataThumbprint).ToUpperInvariant()
    }
    if ($null -ne $metadataPublisher)
    {
        $publisher = [string]$metadataPublisher
    }
}
if (-not [string]::IsNullOrWhiteSpace($thumbprint))
{
    if ($thumbprint -notmatch '^[0-9A-F]{40}$')
    {
        throw 'Install metadata contains an invalid certificate thumbprint.'
    }
    $certificate = Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
        Where-Object { $_.Thumbprint -eq $thumbprint } |
        Select-Object -First 1
    if ($null -ne $certificate)
    {
        if (-not [string]::IsNullOrWhiteSpace($publisher) -and $certificate.Subject -ne $publisher)
        {
            throw 'Refusing to remove a certificate whose subject does not match install metadata.'
        }
        Remove-Item -LiteralPath $certificate.PSPath -Force -ErrorAction Stop
        $remainingCertificate = Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
            Where-Object { $_.Thumbprint -eq $thumbprint }
        if ($null -ne $remainingCertificate)
        {
            throw "Certificate $thumbprint remains in LocalMachine\TrustedPeople."
        }
    }
}

if (Test-Path -LiteralPath $identityDirectory -PathType Container)
{
    Remove-Item -LiteralPath $identityDirectory -Recurse -Force -ErrorAction Stop
}
if (Test-Path -LiteralPath $metadataPath -PathType Leaf)
{
    Remove-Item -LiteralPath $metadataPath -Force -ErrorAction Stop
}
if ($RemoveUserData -and (Test-Path -LiteralPath $dataDirectory -PathType Container))
{
    Remove-Item -LiteralPath $dataDirectory -Recurse -Force -ErrorAction Stop
}
if ($RemoveProgramFiles -and (Test-Path -LiteralPath $installRoot -PathType Container))
{
    Remove-Item -LiteralPath $installRoot -Recurse -Force -ErrorAction Stop
}

Write-Host "Sparse package unregistered: $PackageName"
