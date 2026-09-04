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
        [string]$PackagePath,

        [switch]$Spout2Notice
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
        if ($Spout2Notice)
        {
            $expectedEntries += "${rootPrefix}THIRD-PARTY-NOTICES.txt"
        }
        $actualEntries = @(
            $archive.Entries |
                Where-Object { -not $_.FullName.EndsWith('/') } |
                ForEach-Object { $_.FullName } |
                Sort-Object
        )
        $difference = @(Compare-Object $expectedEntries $actualEntries)
        if ($difference.Count -ne 0)
        {
            throw "Host review archive differs from the locked file contract: $($difference -join ', ')"
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
        [string]$PortableVerifier,

        [switch]$Spout2Notice
    )

    # The portable Host owns its data beside the extracted EXE, so there is no
    # profile directory to redirect or clean during package verification.
    & $Verifier `
        -Package $PackagePath `
        -ExpectedVersion $ExpectedVersion `
        -Linker $Linker `
        -PortableVerifier $PortableVerifier `
        -HostOnly `
        -Spout2Notice:$Spout2Notice
    if ($LASTEXITCODE -ne 0)
    {
        throw "Host review package verification failed with exit code $LASTEXITCODE"
    }
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workflowPreset = 'release-verify'
$packagePreset = 'release'
$buildRootName = 'x64'
$variantSuffix = ''
if ($Slim)
{
    $workflowPreset = 'slim-release-verify'
    $packagePreset = 'slim-release'
    $buildRootName = 'x64-slim'
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
$hostExecutable = Join-Path $repositoryRoot "build\$buildRootName\src\desktop\Release\ba-click-fx-desktop.exe"

if (-not $SkipWorkflow)
{
    $cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $cmake)
    {
        throw 'cmake.exe was not found on PATH.'
    }
    Invoke-Checked -Description 'Host review build and test workflow' -FilePath $cmake.Source `
        -Arguments @('--workflow', '--preset', $workflowPreset) `
        -WorkingDirectory $repositoryRoot
}

if (-not (Test-Path -LiteralPath $hostExecutable -PathType Leaf))
{
    throw "Host review executable is missing: $hostExecutable"
}

$outputRoot = Get-FullPath -Path $OutputDirectory -BaseDirectory $repositoryRoot
$revisionDirectory = Join-Path $outputRoot (Get-RevisionDirectoryName)
if (Test-Path -LiteralPath $revisionDirectory)
{
    throw "Refusing to overwrite an existing Host review directory: $revisionDirectory"
}
New-Item -ItemType Directory -Path $revisionDirectory -Force | Out-Null
$packagePath = Join-Path $revisionDirectory "$packageName.zip"
$checksumPath = "$packagePath.sha256"
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'bafx-host-review-' + [Guid]::NewGuid().ToString('N'))
$stageRoot = Join-Path $temporaryRoot $packageName
try
{
    New-Item -ItemType Directory -Path $stageRoot | Out-Null
    Copy-Item -LiteralPath $hostExecutable -Destination (Join-Path $stageRoot 'ba-click-fx-desktop.exe')
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'LICENSE') `
        -Destination (Join-Path $stageRoot 'LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'SUPPORT.md') -Destination $stageRoot
    if (-not $Slim)
    {
        Copy-Item `
            -LiteralPath (Join-Path $repositoryRoot 'THIRD-PARTY-NOTICES.txt') `
            -Destination $stageRoot
    }
    [IO.Compression.ZipFile]::CreateFromDirectory(
        $stageRoot,
        $packagePath,
        [IO.Compression.CompressionLevel]::Optimal,
        $true)
}
finally
{
    if (Test-Path -LiteralPath $temporaryRoot -PathType Container)
    {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
$hash = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash
Set-Content -LiteralPath $checksumPath `
    -Value "$hash  *$([IO.Path]::GetFileName($packagePath))" `
    -Encoding ascii
Assert-HostOnlyArchive `
    -PackagePath $packagePath `
    -Spout2Notice:(-not $Slim)

if (-not $SkipVerification)
{
    $linker = Get-CMakeLinker -CacheFile (
        Join-Path $repositoryRoot "build\$buildRootName\CMakeCache.txt")
    Invoke-IsolatedVerification `
        -Verifier (Join-Path $PSScriptRoot 'verify-release-package.ps1') `
        -PackagePath $packagePath `
        -ExpectedVersion $version `
        -Linker $linker `
        -PortableVerifier (Join-Path $PSScriptRoot 'verify-portable-pe.ps1') `
        -Spout2Notice:(-not $Slim)
}

Write-Host "Host review package created: $packagePath"
Write-Host "SHA-256: $hash"
