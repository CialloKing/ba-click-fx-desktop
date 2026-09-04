[CmdletBinding()]
param(
    [string]$OutputDirectory = 'artifacts\local',

    # Retained for callers of the former WinUI packaging contract. CMake now
    # selects and invokes the native toolchain, so no separate MSBuild path is needed.
    [string]$MSBuild,

    [switch]$Slim,

    [switch]$SkipBuild,

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

function Assert-ControlCenterExecutable
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Executable
    )

    if (-not (Test-Path -LiteralPath $Executable -PathType Leaf))
    {
        throw "Control Center executable is missing: $Executable"
    }
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$configurePreset = 'x64'
$buildPreset = 'release'
$buildRootName = 'x64'
$variantSuffix = ''
if ($Slim)
{
    $configurePreset = 'x64-slim'
    $buildPreset = 'slim-release'
    $buildRootName = 'x64-slim'
    $variantSuffix = '-slim'
}
$localVcpkgRoot = Join-Path $repositoryRoot '..\SDK\vcpkg'
if (-not $Slim -and -not $SkipBuild -and [string]::IsNullOrWhiteSpace($env:VCPKG_ROOT))
{
    if (Test-Path -LiteralPath $localVcpkgRoot -PathType Container)
    {
        $env:VCPKG_ROOT = [IO.Path]::GetFullPath($localVcpkgRoot)
    }
    else
    {
        throw 'VCPKG_ROOT is required to build the standard Spout2-enabled portable package.'
    }
}
$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if ($null -eq $cmake)
{
    throw 'cmake.exe was not found on PATH.'
}

$version = Get-BafxVersion -VersionFile (Join-Path $repositoryRoot 'cmake\Version.cmake')
$bundleName = "ba-click-fx-desktop-$version$variantSuffix-Portable-windows-x64"
$outputRoot = Get-FullPath -Path $OutputDirectory -BaseDirectory $repositoryRoot
$archivePath = Join-Path $outputRoot "$bundleName.zip"
$checksumPath = "$archivePath.sha256"

if (Test-Path -LiteralPath $archivePath -PathType Leaf)
{
    throw "Refusing to overwrite an existing portable bundle: $archivePath"
}
if (Test-Path -LiteralPath $checksumPath -PathType Leaf)
{
    throw "Refusing to overwrite an existing portable bundle checksum: $checksumPath"
}

New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

if (-not $SkipBuild)
{
    Invoke-Checked -Description 'CMake configure' -FilePath $cmake.Source `
        -Arguments @('--preset', $configurePreset) -WorkingDirectory $repositoryRoot
    Invoke-Checked -Description 'Host Release build' -FilePath $cmake.Source `
        -Arguments @('--build', '--preset', $buildPreset) -WorkingDirectory $repositoryRoot
}

$hostExecutable = Join-Path $repositoryRoot "build\$buildRootName\src\desktop\Release\ba-click-fx-desktop.exe"
$controlCenterExecutable = Join-Path $repositoryRoot "build\$buildRootName\src\control-center\Release\BAFX.ControlCenter.exe"
if (-not (Test-Path -LiteralPath $hostExecutable -PathType Leaf))
{
    throw "Host Release executable is missing: $hostExecutable"
}
Assert-ControlCenterExecutable -Executable $controlCenterExecutable

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$temporaryRoot = Join-Path $temporaryParent ("bafx-test-bundle-" + [Guid]::NewGuid().ToString('N'))
$stageRoot = Join-Path $temporaryRoot $bundleName

try
{
    New-Item -ItemType Directory -Path $stageRoot | Out-Null

    Copy-Item -LiteralPath $hostExecutable -Destination (Join-Path $stageRoot 'ba-click-fx-desktop.exe')
    Copy-Item -LiteralPath $controlCenterExecutable `
        -Destination (Join-Path $stageRoot 'BAFX.ControlCenter.exe')

    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'LICENSE') `
        -Destination (Join-Path $stageRoot 'LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'SUPPORT.md') -Destination $stageRoot
    if (-not $Slim)
    {
        Copy-Item `
            -LiteralPath (Join-Path $repositoryRoot 'THIRD-PARTY-NOTICES.txt') `
            -Destination $stageRoot
    }
    Assert-ControlCenterExecutable -Executable (Join-Path $stageRoot 'BAFX.ControlCenter.exe')

    $manifestFiles = @(
        Get-ChildItem -LiteralPath $stageRoot -Recurse -File |
            Sort-Object FullName |
            ForEach-Object {
                $relativePath = $_.FullName.Substring($stageRoot.Length).TrimStart('\').Replace('\', '/')
                [ordered]@{
                    path = $relativePath
                    bytes = [Int64]$_.Length
                    sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
                }
            }
    )
    $manifest = [ordered]@{
        schema = 1
        version = $version
        files = $manifestFiles
    }
    # The manifest intentionally excludes itself, avoiding an impossible self-referential hash.
    $manifest | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (Join-Path $stageRoot 'TEST-BUNDLE-MANIFEST.json') -Encoding UTF8

    $archive = [IO.Compression.ZipFile]::Open(
        $archivePath,
        [IO.Compression.ZipArchiveMode]::Create)
    try
    {
        foreach ($file in Get-ChildItem -LiteralPath $stageRoot -Recurse -File |
                Sort-Object FullName)
        {
            $relativePath = $file.FullName.Substring($stageRoot.Length)
            $relativePath = $relativePath.TrimStart('\').Replace('\', '/')
            # ZIP entry names use '/' on every platform. CreateFromDirectory
            # preserved Windows separators and produced a non-portable archive.
            $entryName = "$bundleName/$relativePath"
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $archive,
                $file.FullName,
                $entryName,
                [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    }
    finally
    {
        $archive.Dispose()
    }
}
finally
{
    if (Test-Path -LiteralPath $temporaryRoot -PathType Container)
    {
        $resolvedParent = $temporaryParent.TrimEnd('\') + '\'
        $resolvedRoot = [IO.Path]::GetFullPath($temporaryRoot)
        if (-not $resolvedRoot.StartsWith($resolvedParent, [StringComparison]::OrdinalIgnoreCase))
        {
            throw "Refusing to clean a path outside the temporary directory: $resolvedRoot"
        }
        Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
    }
}

$archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
Set-Content -LiteralPath $checksumPath -Encoding Ascii -NoNewline `
    -Value "$archiveHash *$([IO.Path]::GetFileName($archivePath))"

if (-not $SkipVerification)
{
    $linker = Get-CMakeLinker -CacheFile (Join-Path $repositoryRoot "build\$buildRootName\CMakeCache.txt")
    $verifier = Join-Path $PSScriptRoot 'verify-test-bundle.ps1'
    # Hashtable splatting preserves named PowerShell parameters; array splatting does not.
    $verificationArguments = @{
        Package = $archivePath
        ExpectedVersion = $version
        Linker = $linker
    }
    if (-not $Slim)
    {
        $verificationArguments.Spout2Notice = $true
    }
    & $verifier @verificationArguments
    if ($LASTEXITCODE -ne 0)
    {
        throw "Test bundle verification failed with exit code $LASTEXITCODE"
    }
}

Write-Host "Portable bundle created: $archivePath"
Write-Host "SHA-256: $archiveHash"
