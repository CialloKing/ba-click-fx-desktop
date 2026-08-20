param(
    [Parameter(Mandatory = $true)]
    [string]$Package,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedVersion,

    [Parameter(Mandatory = $true)]
    [string]$Linker,

    [Parameter(Mandatory = $true)]
    [string]$PortableVerifier,

    [switch]$HostOnly
)

$ErrorActionPreference = 'Stop'

$packagePath = [IO.Path]::GetFullPath($Package)
$checksumPath = "$packagePath.sha256"
if (-not (Test-Path -LiteralPath $packagePath -PathType Leaf))
{
    throw "Release ZIP not found: $packagePath"
}
if (-not (Test-Path -LiteralPath $checksumPath -PathType Leaf))
{
    throw "Release ZIP checksum not found: $checksumPath"
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
    throw "Release ZIP SHA-256 mismatch: expected $expectedHash, got $actualHash"
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($packagePath)
try
{
    $rootName = [IO.Path]::GetFileNameWithoutExtension($packagePath)
    $expectedEntries = @(
        "$rootName/LICENSE.txt",
        "$rootName/SUPPORT.md",
        "$rootName/ba-click-fx-desktop.exe"
    )
    if (-not $HostOnly)
    {
        $expectedEntries += "$rootName/BAFX.ControlCenter.exe"
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
        $packageKind = $HostOnly ? 'Host-only' : 'full portable'
        throw "Release ZIP file list differs from the locked $packageKind package contract: $($difference -join ', ')"
    }
}
finally
{
    $archive.Dispose()
}

$tempParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$tempRoot = Join-Path $tempParent ("bafx-release-package-" + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $tempRoot
try
{
    [IO.Compression.ZipFile]::ExtractToDirectory($packagePath, $tempRoot)
    $installRoot = Join-Path $tempRoot $rootName
    $executable = Join-Path $installRoot 'ba-click-fx-desktop.exe'
    $controlCenter = Join-Path $installRoot 'BAFX.ControlCenter.exe'
    $productVersion = (Get-Item -LiteralPath $executable).VersionInfo.ProductVersion
    if ($productVersion -ne $ExpectedVersion)
    {
        throw "Extracted executable version mismatch: expected $ExpectedVersion, got $productVersion"
    }
    if (-not $HostOnly)
    {
        if (-not (Test-Path -LiteralPath $controlCenter -PathType Leaf))
        {
            throw 'Extracted package is missing BAFX.ControlCenter.exe.'
        }
        $controlCenterVersion = (Get-Item -LiteralPath $controlCenter).VersionInfo.ProductVersion
        if ($controlCenterVersion -ne $ExpectedVersion)
        {
            throw "Extracted Control Center version mismatch: expected $ExpectedVersion, got $controlCenterVersion"
        }
    }

    & $PortableVerifier -Executable $executable -Linker $Linker

    $process = Start-Process `
        -FilePath $executable `
        -ArgumentList '--smoke-test' `
        -WorkingDirectory $tempRoot `
        -WindowStyle Hidden `
        -Wait `
        -PassThru
    if ($process.ExitCode -ne 0)
    {
        throw "Extracted desktop smoke test failed with exit code $($process.ExitCode)"
    }

    foreach ($portableFile in @(
            'BAFX.config.json',
            'ba-click-fx-desktop-support.log'))
    {
        $portablePath = Join-Path $installRoot $portableFile
        if (-not (Test-Path -LiteralPath $portablePath -PathType Leaf))
        {
            throw "Portable runtime file is missing beside the executable: $portableFile"
        }
        $workingDirectoryPath = Join-Path $tempRoot $portableFile
        if (Test-Path -LiteralPath $workingDirectoryPath)
        {
            throw "Portable runtime file escaped the executable directory: $workingDirectoryPath"
        }
    }
    $escapedFiles = @(
        Get-ChildItem -LiteralPath $tempRoot -Recurse -File |
            Where-Object {
                -not $_.FullName.StartsWith(
                    ([IO.Path]::GetFullPath($installRoot).TrimEnd('\') + '\'),
                    [StringComparison]::OrdinalIgnoreCase)
            })
    if ($escapedFiles.Count -ne 0)
    {
        throw "Runtime data escaped the executable directory: $($escapedFiles.FullName -join ', ')"
    }

    $outsideReport = Join-Path $tempRoot 'requested-support.txt'
    $reportProcess = Start-Process `
        -FilePath $executable `
        -ArgumentList "--support-info=$outsideReport" `
        -WorkingDirectory $tempRoot `
        -WindowStyle Hidden `
        -Wait `
        -PassThru
    if ($reportProcess.ExitCode -ne 0)
    {
        throw "Extracted support report command failed with exit code $($reportProcess.ExitCode)"
    }
    $clampedReport = Join-Path $installRoot 'requested-support.txt'
    if (-not (Test-Path -LiteralPath $clampedReport -PathType Leaf))
    {
        throw 'Support report was not written beside the executable.'
    }
    if (Test-Path -LiteralPath $outsideReport)
    {
        throw "Support report escaped the executable directory: $outsideReport"
    }
    $escapedFiles = @(
        Get-ChildItem -LiteralPath $tempRoot -Recurse -File |
            Where-Object {
                -not $_.FullName.StartsWith(
                    ([IO.Path]::GetFullPath($installRoot).TrimEnd('\') + '\'),
                    [StringComparison]::OrdinalIgnoreCase)
            })
    if ($escapedFiles.Count -ne 0)
    {
        throw "Runtime data escaped the executable directory: $($escapedFiles.FullName -join ', ')"
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
    $cleanupSucceeded = $false
    for ($attempt = 0; $attempt -lt 10; $attempt++)
    {
        try
        {
            Remove-Item -LiteralPath $resolvedRoot -Recurse -Force -ErrorAction Stop
            $cleanupSucceeded = $true
            break
        }
        catch
        {
            if ($attempt -eq 9)
            {
                throw
            }
            Start-Sleep -Milliseconds 200
        }
    }
    if (-not $cleanupSucceeded)
    {
        throw "Failed to clean temporary verification directory: $resolvedRoot"
    }
}

Write-Host "Release package verified: $([IO.Path]::GetFileName($packagePath))"
Write-Host "SHA-256: $actualHash"
