[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$HostExecutable,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [string]$PackageName = 'CialloKing.BaClickFxDesktop',
    [string]$ApplicationId = 'BaClickFxDesktop',
    [string]$Publisher = 'CN=BaClickFx.Local',
    [string]$PublisherDisplayName = 'ba-click-fx-desktop contributors',
    [string]$DisplayName = 'ba-click-fx-desktop',
    [string]$PackageVersion = '0.1.0.7',
    [switch]$UnsignedTemplate,
    [switch]$KeepCertificate
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-SdkTool
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $command = Get-Command $Name -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -ne $command)
    {
        return $command.Source
    }

    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (-not (Test-Path -LiteralPath $kitsRoot -PathType Container))
    {
        throw "Windows SDK bin directory was not found: $kitsRoot"
    }
    $candidates = Get-ChildItem -LiteralPath $kitsRoot -Recurse -Filter $Name -File |
        Where-Object { $_.DirectoryName -match '\\x64$' } |
        Sort-Object FullName -Descending
    $candidate = $candidates | Select-Object -First 1
    if ($null -eq $candidate)
    {
        throw "Windows SDK tool was not found: $Name"
    }
    return $candidate.FullName
}

function Invoke-Checked
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Description,

        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0)
    {
        throw "$Description failed with exit code $LASTEXITCODE"
    }
}

function Assert-SafeValue
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Value,

        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Pattern
    )

    if ($Value -notmatch $Pattern)
    {
        throw "$Name contains unsupported characters: $Value"
    }
}

function ConvertTo-XmlText
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Value
    )

    return [System.Security.SecurityElement]::Escape($Value)
}

function Write-TransparentLogo
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    # The package logo is a generated placeholder, not a game or Unity asset.
    $png = [Convert]::FromBase64String(
        'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=')
    [IO.File]::WriteAllBytes($Path, $png)
}

Assert-SafeValue -Value $PackageName -Name 'PackageName' -Pattern '^[A-Za-z0-9.\-]{3,50}$'
Assert-SafeValue -Value $ApplicationId -Name 'ApplicationId' -Pattern '^[A-Za-z0-9.\-]{1,50}$'
Assert-SafeValue -Value $PackageVersion -Name 'PackageVersion' -Pattern '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$'
if ([string]::IsNullOrWhiteSpace($Publisher) -or $Publisher.Contains("`n") -or $Publisher.Contains("`r"))
{
    throw 'Publisher must be a single non-empty certificate subject.'
}

$hostPath = [IO.Path]::GetFullPath($HostExecutable)
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
if (-not (Test-Path -LiteralPath $hostPath -PathType Leaf))
{
    throw "Host executable was not found: $hostPath"
}
if ([IO.Path]::GetFileName($hostPath) -ne 'ba-click-fx-desktop.exe')
{
    throw 'HostExecutable must point to ba-click-fx-desktop.exe.'
}
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

$makeAppx = Resolve-SdkTool -Name 'makeappx.exe'
$signTool = if ($UnsignedTemplate)
{
    $null
}
else
{
    Resolve-SdkTool -Name 'signtool.exe'
}
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$manifestTemplate = Join-Path $PSScriptRoot 'Package.appxmanifest.in'
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ('bafx-identity-' + [Guid]::NewGuid().ToString('N'))
$stageRoot = Join-Path $temporaryRoot 'package'
$certificate = $null

try
{
    New-Item -ItemType Directory -Path (Join-Path $stageRoot 'Assets') -Force | Out-Null
    Copy-Item -LiteralPath $hostPath -Destination $stageRoot
    Write-TransparentLogo -Path (Join-Path $stageRoot 'Assets\StoreLogo.png')
    Write-TransparentLogo -Path (Join-Path $stageRoot 'Assets\Square150x150Logo.png')
    Write-TransparentLogo -Path (Join-Path $stageRoot 'Assets\Square44x44Logo.png')

    $manifest = Get-Content -LiteralPath $manifestTemplate -Raw
    $replacements = [ordered]@{
        '@PACKAGE_NAME@' = ConvertTo-XmlText -Value $PackageName
        '@PUBLISHER@' = ConvertTo-XmlText -Value $Publisher
        '@PACKAGE_VERSION@' = $PackageVersion
        '@DISPLAY_NAME@' = ConvertTo-XmlText -Value $DisplayName
        '@PUBLISHER_DISPLAY_NAME@' = ConvertTo-XmlText -Value $PublisherDisplayName
        '@APPLICATION_ID@' = ConvertTo-XmlText -Value $ApplicationId
    }
    foreach ($entry in $replacements.GetEnumerator())
    {
        $manifest = $manifest.Replace($entry.Key, $entry.Value)
    }
    Set-Content -LiteralPath (Join-Path $stageRoot 'AppxManifest.xml') -Value $manifest -Encoding UTF8

    $packageBaseName = "$PackageName-$PackageVersion"
    $packageFileName = if ($UnsignedTemplate)
    {
        "$packageBaseName.unsigned.msix"
    }
    else
    {
        "$packageBaseName.msix"
    }
    $packagePath = Join-Path $outputRoot $packageFileName
    if (Test-Path -LiteralPath $packagePath -PathType Leaf)
    {
        throw "Refusing to overwrite an existing package: $packagePath"
    }
    Invoke-Checked -Description 'Sparse package build' -FilePath $makeAppx -Arguments @(
        'pack', '/d', $stageRoot, '/p', $packagePath, '/o'
    )

    if ($UnsignedTemplate)
    {
        $metadataPath = Join-Path $outputRoot "$packageBaseName.identity-template.json"
        $metadata = [ordered]@{
            schema = 2
            identityMode = 'target-machine-self-signed'
            packageName = $PackageName
            applicationId = $ApplicationId
            publisher = $Publisher
            packageVersion = $PackageVersion
            templateFile = $packageFileName
            templateSha256 = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash
            hostSha256 = (Get-FileHash -LiteralPath $hostPath -Algorithm SHA256).Hash
        }
        $metadata | ConvertTo-Json -Depth 3 |
            Set-Content -LiteralPath $metadataPath -Encoding UTF8

        Write-Host "Unsigned identity template created: $packagePath"
        return
    }

    $certificate = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $Publisher `
        -CertStoreLocation 'Cert:\CurrentUser\My' `
        -KeyAlgorithm RSA `
        -KeyLength 2048 `
        -HashAlgorithm SHA256 `
        -KeyExportPolicy NonExportable `
        -NotAfter (Get-Date).AddYears(2)
    $certificatePath = Join-Path $outputRoot "$PackageName-$PackageVersion.cer"
    Export-Certificate -Cert $certificate -FilePath $certificatePath | Out-Null

    Invoke-Checked -Description 'Sparse package signing' -FilePath $signTool -Arguments @(
        'sign', '/fd', 'SHA256', '/sha1', $certificate.Thumbprint, $packagePath
    )

    $metadataPath = Join-Path $outputRoot "$PackageName-$PackageVersion.identity.json"
    $metadata = [ordered]@{
        schema = 1
        packageName = $PackageName
        applicationId = $ApplicationId
        publisher = $Publisher
        packageVersion = $PackageVersion
        certificateThumbprint = $certificate.Thumbprint
        packagePath = $packagePath
        hostDirectory = [IO.Path]::GetDirectoryName($hostPath)
    }
    $metadata | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

    Write-Host "Identity package created: $packagePath"
    Write-Host "Public certificate: $certificatePath"
    Write-Host "Certificate thumbprint: $($certificate.Thumbprint)"
}
finally
{
    if ($null -ne $certificate -and -not $KeepCertificate)
    {
        $certificatePath = "Cert:\CurrentUser\My\$($certificate.Thumbprint)"
        Remove-Item `
            -LiteralPath $certificatePath `
            -DeleteKey `
            -Force `
            -ErrorAction Stop
        if (Test-Path -LiteralPath $certificatePath)
        {
            throw 'The temporary signing certificate remains after cleanup.'
        }
    }
    if (Test-Path -LiteralPath $temporaryRoot -PathType Container)
    {
        $tempParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
        $resolvedRoot = [IO.Path]::GetFullPath($temporaryRoot)
        if (-not $resolvedRoot.StartsWith($tempParent, [StringComparison]::OrdinalIgnoreCase))
        {
            throw "Refusing to clean a path outside the temporary directory: $resolvedRoot"
        }
        Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
    }
}
