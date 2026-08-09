[CmdletBinding()]
param(
    [string]$OutputDirectory = 'artifacts\local',

    [string]$MSBuild,

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

function Resolve-MSBuild
{
    param(
        [string]$RequestedPath
    )

    if (-not [string]::IsNullOrWhiteSpace($RequestedPath))
    {
        $resolvedRequestedPath = [IO.Path]::GetFullPath($RequestedPath)
        if (-not (Test-Path -LiteralPath $resolvedRequestedPath -PathType Leaf))
        {
            throw "MSBuild executable not found: $resolvedRequestedPath"
        }
        return $resolvedRequestedPath
    }

    $command = Get-Command MSBuild.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -ne $command)
    {
        return $command.Source
    }

    $vswhereCandidates = @(
        'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe',
        'C:\Program Files\Microsoft Visual Studio\Installer\vswhere.exe'
    )
    foreach ($vswhere in $vswhereCandidates)
    {
        if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf))
        {
            continue
        }

        $installationPath = (& $vswhere -latest -products '*' `
            -requires Microsoft.Component.MSBuild -property installationPath).Trim()
        if ([string]::IsNullOrWhiteSpace($installationPath))
        {
            continue
        }

        $candidate = Join-Path $installationPath 'MSBuild\Current\Bin\amd64\MSBuild.exe'
        if (Test-Path -LiteralPath $candidate -PathType Leaf)
        {
            return $candidate
        }
    }

    throw 'MSBuild.exe was not found. Pass -MSBuild with the x64 MSBuild path.'
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

function Assert-ControlCenterPayload
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Directory
    )

    $requiredFiles = @(
        'BAFX.ControlCenter.exe',
        'BAFX.ControlCenter.pri',
        'Microsoft.WindowsAppRuntime.Bootstrap.dll',
        'Microsoft.WindowsAppRuntime.dll',
        'MRM.dll',
        'Microsoft.UI.pri',
        'Microsoft.UI.Xaml.Controls.pri',
        'Microsoft.ui.xaml.dll'
    )
    foreach ($relativePath in $requiredFiles)
    {
        $candidate = Join-Path $Directory $relativePath
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf))
        {
            throw "Control Center direct-deployment file is missing: $relativePath"
        }
    }

    $assetsDirectory = Join-Path $Directory 'Microsoft.UI.Xaml\Assets'
    if (-not (Test-Path -LiteralPath $assetsDirectory -PathType Container))
    {
        throw 'Control Center XAML asset directory is missing.'
    }
    if ($null -eq (Get-ChildItem -LiteralPath $assetsDirectory -File | Select-Object -First 1))
    {
        throw 'Control Center XAML asset directory is empty.'
    }
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if ($null -eq $cmake)
{
    throw 'cmake.exe was not found on PATH.'
}

$version = Get-BafxVersion -VersionFile (Join-Path $repositoryRoot 'cmake\Version.cmake')
$bundleName = "ba-click-fx-desktop-$version-test-windows-x64"
$outputRoot = Get-FullPath -Path $OutputDirectory -BaseDirectory $repositoryRoot
$archivePath = Join-Path $outputRoot "$bundleName.zip"
$checksumPath = "$archivePath.sha256"

if (Test-Path -LiteralPath $archivePath -PathType Leaf)
{
    throw "Refusing to overwrite an existing test bundle: $archivePath"
}
if (Test-Path -LiteralPath $checksumPath -PathType Leaf)
{
    throw "Refusing to overwrite an existing test bundle checksum: $checksumPath"
}

New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

Invoke-Checked -Description 'CMake configure' -FilePath $cmake.Source `
    -Arguments @('--preset', 'alpha-x64') -WorkingDirectory $repositoryRoot
Invoke-Checked -Description 'Host Release build' -FilePath $cmake.Source `
    -Arguments @('--build', '--preset', 'alpha-release') -WorkingDirectory $repositoryRoot

$msbuildPath = Resolve-MSBuild -RequestedPath $MSBuild
$controlCenterProject = Join-Path $repositoryRoot 'src\control-center\BAFX.ControlCenter.vcxproj'
Invoke-Checked -Description 'Control Center Release build' -FilePath $msbuildPath `
    -Arguments @(
        $controlCenterProject,
        '/restore',
        '/p:Configuration=Release',
        '/p:Platform=x64',
        '/m:1'
    ) -WorkingDirectory $repositoryRoot

$hostExecutable = Join-Path $repositoryRoot 'build\alpha-x64\src\desktop\Release\ba-click-fx-desktop.exe'
$controlCenterOutput = Join-Path $repositoryRoot 'build\control-center\x64\Release'
if (-not (Test-Path -LiteralPath $hostExecutable -PathType Leaf))
{
    throw "Host Release executable is missing: $hostExecutable"
}
if (-not (Test-Path -LiteralPath $controlCenterOutput -PathType Container))
{
    throw "Control Center Release output is missing: $controlCenterOutput"
}
Assert-ControlCenterPayload -Directory $controlCenterOutput

Add-Type -AssemblyName System.IO.Compression.FileSystem
$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$temporaryRoot = Join-Path $temporaryParent ("bafx-test-bundle-" + [Guid]::NewGuid().ToString('N'))
$stageRoot = Join-Path $temporaryRoot $bundleName

try
{
    New-Item -ItemType Directory -Path $stageRoot | Out-Null

    Copy-Item -LiteralPath $hostExecutable -Destination (Join-Path $stageRoot 'ba-click-fx-desktop.exe')
    # Windows App SDK direct deployment resolves resources beside the EXE, so keep its tree intact.
    Copy-Item -Path (Join-Path $controlCenterOutput '*') -Destination $stageRoot -Recurse -Force
    $pdbFiles = @(Get-ChildItem -LiteralPath $stageRoot -Recurse -File -Filter '*.pdb')
    foreach ($pdbFile in $pdbFiles)
    {
        Remove-Item -LiteralPath $pdbFile.FullName -Force
    }
    $startupLog = Join-Path $stageRoot 'BAFX.ControlCenter.startup-error.log'
    if (Test-Path -LiteralPath $startupLog -PathType Leaf)
    {
        # This is a local diagnostic emitted by a prior launch, not a runtime dependency.
        Remove-Item -LiteralPath $startupLog -Force
    }

    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'LICENSE') `
        -Destination (Join-Path $stageRoot 'LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'SUPPORT.md') -Destination $stageRoot
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'ASSET-MANIFEST.md') -Destination $stageRoot
    Assert-ControlCenterPayload -Directory $stageRoot

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

    [IO.Compression.ZipFile]::CreateFromDirectory(
        $stageRoot,
        $archivePath,
        [IO.Compression.CompressionLevel]::Optimal,
        $true)
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
    $linker = Get-CMakeLinker -CacheFile (Join-Path $repositoryRoot 'build\alpha-x64\CMakeCache.txt')
    $verifier = Join-Path $PSScriptRoot 'verify-test-bundle.ps1'
    # Hashtable splatting preserves named PowerShell parameters; array splatting does not.
    $verificationArguments = @{
        Package = $archivePath
        ExpectedVersion = $version
        Linker = $linker
    }
    & $verifier @verificationArguments
    if ($LASTEXITCODE -ne 0)
    {
        throw "Test bundle verification failed with exit code $LASTEXITCODE"
    }
}

Write-Host "Test bundle created: $archivePath"
Write-Host "SHA-256: $archiveHash"
