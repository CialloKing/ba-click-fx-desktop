[CmdletBinding()]
param(
    [string]$OutputDirectory = 'artifacts\local\host-visual-review',

    [switch]$Slim,

    [switch]$SkipWorkflow,

    [switch]$SkipVerification
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

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

function Invoke-Checked
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Description,

        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,

        [Parameter(Mandatory = $true)]
        [string]$WorkingDirectory
    )

    Push-Location -LiteralPath $WorkingDirectory
    try
    {
        & $FilePath @Arguments
        if ($LASTEXITCODE -ne 0)
        {
            throw "$Description failed with exit code $LASTEXITCODE"
        }
    }
    finally
    {
        Pop-Location
    }
}

function Get-BafxVersion
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$VersionFile
    )

    $contents = Get-Content -LiteralPath $VersionFile -Raw
    $match = [regex]::Match(
        $contents,
        'set\s*\(\s*BAFX_VERSION\s+"([^"]+)"\s*\)',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)
    if (-not $match.Success)
    {
        throw "Could not find BAFX_VERSION in $VersionFile"
    }

    return $match.Groups[1].Value
}

function Get-CMakeLinker
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$CacheFile
    )

    if (-not (Test-Path -LiteralPath $CacheFile -PathType Leaf))
    {
        throw "CMake cache not found: $CacheFile"
    }

    $match = Select-String -LiteralPath $CacheFile -Pattern '^CMAKE_LINKER:FILEPATH=(.+)$' |
        Select-Object -First 1
    if ($null -eq $match)
    {
        throw "CMAKE_LINKER is missing from $CacheFile"
    }

    $linker = $match.Matches[0].Groups[1].Value
    if (-not (Test-Path -LiteralPath $linker -PathType Leaf))
    {
        throw "CMake selected linker is unavailable: $linker"
    }

    return $linker
}

function Get-RevisionDirectoryName
{
    $git = Get-Command git.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $git)
    {
        return 'local'
    }

    $revision = (& $git.Source rev-parse --short=12 HEAD 2>$null).Trim()
    if ([string]::IsNullOrWhiteSpace($revision))
    {
        return 'local'
    }
    return $revision
}

function Assert-HostOnlyArchive
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$PackagePath
    )

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($PackagePath)
    try
    {
        $rootName = [IO.Path]::GetFileNameWithoutExtension($PackagePath)
        $rootPrefix = "$rootName/"
        $expectedEntries = @(
            "${rootPrefix}ba-click-fx-desktop.exe",
            "${rootPrefix}LICENSE.txt",
            "${rootPrefix}SUPPORT.md"
        )
        $actualEntries = @(
            $archive.Entries |
                Where-Object { -not $_.FullName.EndsWith('/') } |
                ForEach-Object { $_.FullName } |
                Sort-Object
        )
        $difference = @(Compare-Object $expectedEntries $actualEntries)
        if ($difference.Count -ne 0)
        {
            throw "Host review archive is not the three-file contract: $($difference -join ', ')"
        }

        foreach ($entry in $actualEntries)
        {
            if ($entry -match '(?i)(ControlCenter|Microsoft\.UI|WindowsAppRuntime|\.pri$|TEST-BUNDLE)')
            {
                throw "Host review archive contains Control Center payload: $entry"
            }
        }
    }
    finally
    {
        $archive.Dispose()
    }
}

function Invoke-IsolatedVerification
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Verifier,

        [Parameter(Mandatory = $true)]
        [string]$PackagePath,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedVersion,

        [Parameter(Mandatory = $true)]
        [string]$Linker,

        [Parameter(Mandatory = $true)]
        [string]$PortableVerifier
    )

    # The portable Host owns its data beside the extracted EXE, so there is no
    # profile directory to redirect or clean during package verification.
    & $Verifier `
        -Package $PackagePath `
        -ExpectedVersion $ExpectedVersion `
        -Linker $Linker `
        -PortableVerifier $PortableVerifier
    if ($LASTEXITCODE -ne 0)
    {
        throw "Host review package verification failed with exit code $LASTEXITCODE"
    }
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workflowPreset = 'alpha-release-verify'
$packagePreset = 'alpha-release'
$buildRootName = 'alpha-x64'
$variantSuffix = ''
if ($Slim)
{
    $workflowPreset = 'alpha-slim-release-verify'
    $packagePreset = 'alpha-slim-release'
    $buildRootName = 'alpha-x64-slim'
    $variantSuffix = '-slim'
}
if (-not $Slim -and -not $SkipWorkflow -and [string]::IsNullOrWhiteSpace($env:VCPKG_ROOT))
{
    $localVcpkgRoot = Join-Path $repositoryRoot '..\SDK\vcpkg'
    if (Test-Path -LiteralPath $localVcpkgRoot -PathType Container)
    {
        $env:VCPKG_ROOT = [IO.Path]::GetFullPath($localVcpkgRoot)
    }
    else
    {
        throw 'VCPKG_ROOT is required to build the standard Spout2-enabled Host review package.'
    }
}
$version = Get-BafxVersion -VersionFile (Join-Path $repositoryRoot 'cmake\Version.cmake')
$packageName = "ba-click-fx-desktop-$version$variantSuffix-windows-x64"
$sourcePackage = Join-Path $repositoryRoot "build\$buildRootName\packages\$packageName.zip"
$sourceChecksum = "$sourcePackage.sha256"

if (-not $SkipWorkflow)
{
    $cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1
    $cpack = Get-Command cpack.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $cmake -or $null -eq $cpack)
    {
        throw 'cmake.exe and cpack.exe must both be available on PATH.'
    }
    Invoke-Checked -Description 'Host review build and test workflow' -FilePath $cmake.Source `
        -Arguments @('--workflow', '--preset', $workflowPreset) `
        -WorkingDirectory $repositoryRoot
    Invoke-Checked -Description 'Host review CPack generation' -FilePath $cpack.Source `
        -Arguments @('--preset', $packagePreset) `
        -WorkingDirectory $repositoryRoot
}

if (-not (Test-Path -LiteralPath $sourcePackage -PathType Leaf))
{
    throw "CPack Host-only package is missing: $sourcePackage"
}
if (-not (Test-Path -LiteralPath $sourceChecksum -PathType Leaf))
{
    throw "CPack Host-only checksum is missing: $sourceChecksum"
}

$outputRoot = Get-FullPath -Path $OutputDirectory -BaseDirectory $repositoryRoot
$revisionDirectory = Join-Path $outputRoot (Get-RevisionDirectoryName)
if (Test-Path -LiteralPath $revisionDirectory)
{
    throw "Refusing to overwrite an existing Host review directory: $revisionDirectory"
}
New-Item -ItemType Directory -Path $revisionDirectory -Force | Out-Null
$packagePath = Join-Path $revisionDirectory ([IO.Path]::GetFileName($sourcePackage))
$checksumPath = "$packagePath.sha256"
Copy-Item -LiteralPath $sourcePackage -Destination $packagePath
Copy-Item -LiteralPath $sourceChecksum -Destination $checksumPath
Assert-HostOnlyArchive -PackagePath $packagePath

if (-not $SkipVerification)
{
    $linker = Get-CMakeLinker -CacheFile (
        Join-Path $repositoryRoot "build\$buildRootName\CMakeCache.txt")
    Invoke-IsolatedVerification `
        -Verifier (Join-Path $PSScriptRoot 'verify-alpha-package.ps1') `
        -PackagePath $packagePath `
        -ExpectedVersion $version `
        -Linker $linker `
        -PortableVerifier (Join-Path $PSScriptRoot 'verify-portable-pe.ps1')
}

$hash = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash
Write-Host "Host review package created: $packagePath"
Write-Host "SHA-256: $hash"
