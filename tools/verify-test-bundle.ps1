[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Package,

    [Parameter(Mandatory = $true)]
    [string]$ExpectedVersion,

    [Parameter(Mandatory = $true)]
    [string]$Linker,

    [ValidateRange(100, 10000)]
    [int]$ControlCenterStartupTimeoutMs = 1500,

    [switch]$SkipControlCenterLaunch
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-SafeRelativePath
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($Path) -or [IO.Path]::IsPathRooted($Path))
    {
        return $false
    }

    $segments = $Path -split '[\\/]'
    foreach ($segment in $segments)
    {
        if ($segment -eq '' -or $segment -eq '.' -or $segment -eq '..')
        {
            return $false
        }
    }
    return $true
}

function Get-RelativePath
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$BaseDirectory,

        [Parameter(Mandatory = $true)]
        [string]$FullPath
    )

    $baseWithSeparator = [IO.Path]::GetFullPath($BaseDirectory).TrimEnd('\') + '\'
    $baseUri = [Uri]::new($baseWithSeparator)
    $fileUri = [Uri]::new([IO.Path]::GetFullPath($FullPath))
    return [Uri]::UnescapeDataString($baseUri.MakeRelativeUri($fileUri).ToString()).Replace('\', '/')
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

function Invoke-ControlCenterLaunchCheck
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Executable,

        [Parameter(Mandatory = $true)]
        [string]$WorkingDirectory,

        [Parameter(Mandatory = $true)]
        [int]$StartupTimeoutMs
    )

    $existingProcess = Get-Process -Name 'BAFX.ControlCenter' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -ne $existingProcess)
    {
        throw 'Close the existing BAFX.ControlCenter process before package verification.'
    }

    $process = $null
    try
    {
        # Startup catches manifest and common-control failures that static PE checks cannot observe.
        $process = Start-Process -FilePath $Executable -WorkingDirectory $WorkingDirectory `
            -WindowStyle Hidden -PassThru
        Start-Sleep -Milliseconds $StartupTimeoutMs
        $process.Refresh()
        if ($process.HasExited)
        {
            throw "Control Center exited during startup with exit code $($process.ExitCode)"
        }
    }
    finally
    {
        if ($null -ne $process)
        {
            $process.Refresh()
            if (-not $process.HasExited)
            {
                $null = $process.CloseMainWindow()
                if (-not $process.WaitForExit(2000))
                {
                    # Hidden validation launches do not expose a reliable
                    # main-window handle. Force termination only as a fallback,
                    # then wait for the image section to be unmapped before the
                    # temporary extraction is removed.
                    Stop-Process -Id $process.Id -Force
                    $null = $process.WaitForExit(5000)
                }
            }
            $process.Dispose()
        }
    }
}

$packagePath = [IO.Path]::GetFullPath($Package)
$checksumPath = "$packagePath.sha256"
if (-not (Test-Path -LiteralPath $packagePath -PathType Leaf))
{
    throw "Test bundle ZIP not found: $packagePath"
}
if (-not (Test-Path -LiteralPath $checksumPath -PathType Leaf))
{
    throw "Test bundle checksum not found: $checksumPath"
}
if (-not (Test-Path -LiteralPath $Linker -PathType Leaf))
{
    throw "MSVC linker not found: $Linker"
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

$expectedArchiveHash = $Matches[1].ToUpperInvariant()
$actualArchiveHash = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash
if ($actualArchiveHash -ne $expectedArchiveHash)
{
    throw "Test bundle SHA-256 mismatch: expected $expectedArchiveHash, got $actualArchiveHash"
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($packagePath)
try
{
    $rootName = [IO.Path]::GetFileNameWithoutExtension($packagePath)
    $rootPrefix = "$rootName/"
    $entries = @(
        $archive.Entries |
            Where-Object { -not $_.FullName.EndsWith('/') }
    )
    if ($entries.Count -eq 0)
    {
        throw 'Test bundle ZIP contains no files.'
    }

    $entryNames = [System.Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $entries)
    {
        if ($entry.FullName.Contains('\') -or -not $entry.FullName.StartsWith(
                $rootPrefix,
                [StringComparison]::OrdinalIgnoreCase))
        {
            throw "ZIP entry is outside the required single top-level directory: $($entry.FullName)"
        }

        $relativePath = $entry.FullName.Substring($rootPrefix.Length)
        if (-not (Test-SafeRelativePath -Path $relativePath))
        {
            throw "ZIP entry has an unsafe relative path: $($entry.FullName)"
        }
        if (-not $entryNames.Add($entry.FullName))
        {
            throw "ZIP contains duplicate entry names: $($entry.FullName)"
        }
        if ([IO.Path]::GetExtension($relativePath) -ieq '.pdb')
        {
            throw "Test bundle must not contain PDB files: $relativePath"
        }
    }

    $requiredFiles = @(
        'ASSET-MANIFEST.md',
        'LICENSE.txt',
        'SUPPORT.md',
        'TEST-BUNDLE-MANIFEST.json',
        'ba-click-fx-desktop.exe',
        'BAFX.ControlCenter.exe'
    )
    foreach ($relativePath in $requiredFiles)
    {
        if (-not $entryNames.Contains("$rootPrefix$relativePath"))
        {
            throw "Test bundle is missing required file: $relativePath"
        }
    }
}
finally
{
    $archive.Dispose()
}

$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$temporaryRoot = Join-Path $temporaryParent ("bafx-test-bundle-verify-" + [Guid]::NewGuid().ToString('N'))
try
{
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    [IO.Compression.ZipFile]::ExtractToDirectory($packagePath, $temporaryRoot)
    $installRoot = Join-Path $temporaryRoot $rootName
    if (-not (Test-Path -LiteralPath $installRoot -PathType Container))
    {
        throw "Extracted test bundle root is missing: $installRoot"
    }

    Assert-ControlCenterExecutable -Executable (Join-Path $installRoot 'BAFX.ControlCenter.exe')

    $manifestPath = Join-Path $installRoot 'TEST-BUNDLE-MANIFEST.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.schema -ne 1)
    {
        throw "Unsupported test bundle manifest schema: $($manifest.schema)"
    }
    if ($manifest.version -ne $ExpectedVersion)
    {
        throw "Test bundle manifest version mismatch: expected $ExpectedVersion, got $($manifest.version)"
    }

    $manifestRecords = @($manifest.files)
    if ($manifestRecords.Count -eq 0)
    {
        throw 'Test bundle manifest contains no payload files.'
    }

    $manifestPaths = [System.Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($record in $manifestRecords)
    {
        $relativePath = [string]$record.path
        if (-not (Test-SafeRelativePath -Path $relativePath))
        {
            throw "Manifest contains an unsafe relative path: $relativePath"
        }
        if (-not $manifestPaths.Add($relativePath))
        {
            throw "Manifest contains duplicate paths: $relativePath"
        }

        $payloadPath = Join-Path $installRoot $relativePath.Replace('/', '\')
        if (-not (Test-Path -LiteralPath $payloadPath -PathType Leaf))
        {
            throw "Manifest payload file is missing: $relativePath"
        }
        $payloadItem = Get-Item -LiteralPath $payloadPath
        if ([Int64]$payloadItem.Length -ne [Int64]$record.bytes)
        {
            throw "Manifest payload size mismatch: $relativePath"
        }
        $payloadHash = (Get-FileHash -LiteralPath $payloadPath -Algorithm SHA256).Hash
        if ($payloadHash -ne [string]$record.sha256)
        {
            throw "Manifest payload SHA-256 mismatch: $relativePath"
        }
    }

    $actualPayloadPaths = @(
        Get-ChildItem -LiteralPath $installRoot -Recurse -File |
            Where-Object { $_.Name -ne 'TEST-BUNDLE-MANIFEST.json' } |
            ForEach-Object { Get-RelativePath -BaseDirectory $installRoot -FullPath $_.FullName } |
            Sort-Object
    )
    $expectedPayloadPaths = @($manifestPaths | Sort-Object)
    $payloadDifference = @(Compare-Object $expectedPayloadPaths $actualPayloadPaths)
    if ($payloadDifference.Count -ne 0)
    {
        $details = @($payloadDifference | ForEach-Object {
            "$($_.SideIndicator) $($_.InputObject)"
        }) -join ', '
        throw "Manifest file list differs from extracted payload: $details"
    }

    $hostExecutable = Join-Path $installRoot 'ba-click-fx-desktop.exe'
    $productVersion = (Get-Item -LiteralPath $hostExecutable).VersionInfo.ProductVersion
    if ($productVersion -ne $ExpectedVersion)
    {
        throw "Extracted Host version mismatch: expected $ExpectedVersion, got $productVersion"
    }

    $portableVerifier = Join-Path $PSScriptRoot 'verify-portable-pe.ps1'
    & $portableVerifier -Executable $hostExecutable -Linker $Linker
    if ($LASTEXITCODE -ne 0)
    {
        throw "Host portable PE verification failed with exit code $LASTEXITCODE"
    }

    $controlCenterExecutable = Join-Path $installRoot 'BAFX.ControlCenter.exe'
    & $portableVerifier -Executable $controlCenterExecutable -Linker $Linker
    if ($LASTEXITCODE -ne 0)
    {
        throw "Control Center portable PE verification failed with exit code $LASTEXITCODE"
    }

    $hostProcess = Start-Process -FilePath $hostExecutable -ArgumentList '--smoke-test' `
        -WorkingDirectory $temporaryRoot -WindowStyle Hidden -Wait -PassThru
    if ($hostProcess.ExitCode -ne 0)
    {
        throw "Extracted Host smoke test failed with exit code $($hostProcess.ExitCode)"
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
        $workingDirectoryPath = Join-Path $temporaryRoot $portableFile
        if (Test-Path -LiteralPath $workingDirectoryPath)
        {
            throw "Portable runtime file escaped the executable directory: $workingDirectoryPath"
        }
    }

    if (-not $SkipControlCenterLaunch)
    {
        Invoke-ControlCenterLaunchCheck `
            -Executable $controlCenterExecutable `
            -WorkingDirectory $installRoot `
            -StartupTimeoutMs $ControlCenterStartupTimeoutMs
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

Write-Host "Test bundle verified: $([IO.Path]::GetFileName($packagePath))"
Write-Host "SHA-256: $actualArchiveHash"
