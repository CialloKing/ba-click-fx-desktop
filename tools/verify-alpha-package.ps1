param(
    [Parameter(Mandatory = $true)]
    [string]$Package,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedVersion,

    [Parameter(Mandatory = $true)]
    [string]$Linker,

    [Parameter(Mandatory = $true)]
    [string]$PortableVerifier
)

$ErrorActionPreference = 'Stop'

$packagePath = [IO.Path]::GetFullPath($Package)
$checksumPath = "$packagePath.sha256"
if (-not (Test-Path -LiteralPath $packagePath -PathType Leaf))
{
    throw "Alpha ZIP not found: $packagePath"
}
if (-not (Test-Path -LiteralPath $checksumPath -PathType Leaf))
{
    throw "Alpha ZIP checksum not found: $checksumPath"
}

$checksumLine = (Get-Content -LiteralPath $checksumPath -Raw).Trim()
if ($checksumLine -notmatch '^([0-9A-Fa-f]{64})\s+\*?(.+)$')
{
    throw "Invalid SHA-256 sidecar format: $checksumPath"
}
if ($Matches[2] -ne [IO.Path]::GetFileName($packagePath))
{
    throw "Checksum sidecar names a different package: $($Matches[2])"
}

$expectedHash = $Matches[1].ToUpperInvariant()
$actualHash = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash
if ($actualHash -ne $expectedHash)
{
    throw "Alpha ZIP SHA-256 mismatch: expected $expectedHash, got $actualHash"
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($packagePath)
try
{
    $rootName = [IO.Path]::GetFileNameWithoutExtension($packagePath)
    $expectedEntries = @(
        "$rootName/ASSET-MANIFEST.md",
        "$rootName/LICENSE.txt",
        "$rootName/SUPPORT.md",
        "$rootName/ba-click-fx-desktop.exe"
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
        throw "Alpha ZIP file list differs from the locked four-file package contract: $($difference -join ', ')"
    }
}
finally
{
    $archive.Dispose()
}

$tempParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$tempRoot = Join-Path $tempParent ("bafx-alpha-package-" + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $tempRoot
try
{
    [IO.Compression.ZipFile]::ExtractToDirectory($packagePath, $tempRoot)
    $installRoot = Join-Path $tempRoot $rootName
    $executable = Join-Path $installRoot 'ba-click-fx-desktop.exe'
    $productVersion = (Get-Item -LiteralPath $executable).VersionInfo.ProductVersion
    if ($productVersion -ne $ExpectedVersion)
    {
        throw "Extracted executable version mismatch: expected $ExpectedVersion, got $productVersion"
    }

    & $PortableVerifier -Executable $executable -Linker $Linker

    $process = Start-Process `
        -FilePath $executable `
        -ArgumentList '--smoke-test' `
        -WorkingDirectory $installRoot `
        -WindowStyle Hidden `
        -Wait `
        -PassThru
    if ($process.ExitCode -ne 0)
    {
        throw "Extracted desktop smoke test failed with exit code $($process.ExitCode)"
    }
}
finally
{
    $resolvedParent = $tempParent.TrimEnd('\') + '\'
    $resolvedRoot = [IO.Path]::GetFullPath($tempRoot)
    if (-not $resolvedRoot.StartsWith($resolvedParent, [StringComparison]::OrdinalIgnoreCase))
    {
        throw "Refusing to clean a path outside the temporary directory: $resolvedRoot"
    }
    Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
}

Write-Host "Alpha package verified: $([IO.Path]::GetFileName($packagePath))"
Write-Host "SHA-256: $actualHash"
