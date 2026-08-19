[CmdletBinding()]
param(
    [string]$OutputDirectory = 'artifacts\local',
    [string]$ISCC,

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

function Get-NumericVersions
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Version
    )

    $match = [regex]::Match(
        $Version,
        '^([0-9]+)\.([0-9]+)\.([0-9]+)-alpha\.([0-9]+)$',
        [Text.RegularExpressions.RegexOptions]::CultureInvariant)
    if (-not $match.Success)
    {
        throw 'The user installer currently requires an alpha semantic version.'
    }
    $major = [int]$match.Groups[1].Value
    $minor = [int]$match.Groups[2].Value
    $patch = [int]$match.Groups[3].Value
    $alpha = [int]$match.Groups[4].Value
    foreach ($component in @($major, $minor, $patch, $alpha))
    {
        if ($component -lt 0 -or $component -gt 65535)
        {
            throw "Version component is outside the Windows range: $component"
        }
    }
    return [ordered]@{
        numericVersion = "$major.$minor.$patch.$alpha"
        packageVersion = "$major.$minor.$patch.$alpha"
    }
}

function Resolve-Iscc
{
    param(
        [string]$RequestedPath,
        [string]$RepositoryRoot
    )

    $candidates = New-Object Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($RequestedPath))
    {
        $candidates.Add((Get-FullPath -Path $RequestedPath -BaseDirectory $RepositoryRoot))
    }
    $candidates.Add(
        (Join-Path $RepositoryRoot 'artifacts\local\inno-6.7.3\ISCC.exe'))
    $command = Get-Command iscc.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $command)
    {
        $candidates.Add($command.Source)
    }
    foreach ($path in @(
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
    ))
    {
        if (-not [string]::IsNullOrWhiteSpace($path))
        {
            $candidates.Add($path)
        }
    }
    foreach ($candidate in $candidates)
    {
        if (Test-Path -LiteralPath $candidate -PathType Leaf)
        {
            return [IO.Path]::GetFullPath($candidate)
        }
    }
    throw 'ISCC.exe was not found. Install Inno Setup 6 or pass -ISCC.'
}

function Assert-IsccDiagnosticsSupport
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $minimumVersion = [Version]'6.3.0'
    $historyPath = Join-Path ([IO.Path]::GetDirectoryName($Path)) 'whatsnew.htm'
    if (-not (Test-Path -LiteralPath $historyPath -PathType Leaf))
    {
        throw "Cannot verify the Inno Setup version because its revision history is missing: $historyPath"
    }
    $history = Get-Content -LiteralPath $historyPath -Raw
    $versionMatch = [regex]::Match(
        $history,
        '<span\s+class="ver">\s*([0-9]+\.[0-9]+(?:\.[0-9]+)?)\s*</span>',
        [Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if (-not $versionMatch.Success)
    {
        throw "Cannot determine the Inno Setup version from: $historyPath"
    }
    $detectedVersion = [Version]$versionMatch.Groups[1].Value
    if ($detectedVersion -lt $minimumVersion)
    {
        throw "Inno Setup $minimumVersion or newer is required for installer diagnostics; detected $detectedVersion at $Path"
    }
}

function Get-CMakeLinker
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$CacheFile
    )

    $match = Select-String -LiteralPath $CacheFile -Pattern '^CMAKE_LINKER:FILEPATH=(.+)$' |
        Select-Object -First 1
    if ($null -eq $match)
    {
        throw "CMAKE_LINKER is missing from $CacheFile"
    }
    return $match.Matches[0].Groups[1].Value
}

function Write-Utf8NoBom
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Content
    )

    $encoding = New-Object -TypeName System.Text.UTF8Encoding -ArgumentList $false
    [IO.File]::WriteAllText($Path, $Content, $encoding)
}

function Assert-ExecutableVersion
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$ProductVersion,

        [Parameter(Mandatory = $true)]
        [string]$NumericVersion
    )

    $versionInfo = (Get-Item -LiteralPath $Path).VersionInfo
    $actualProductVersion = ([string]$versionInfo.ProductVersion).Trim()
    $actualFileVersion = ([string]$versionInfo.FileVersion).Trim()
    $actualNumericVersion = '{0}.{1}.{2}.{3}' -f `
        $versionInfo.FileMajorPart,
        $versionInfo.FileMinorPart,
        $versionInfo.FileBuildPart,
        $versionInfo.FilePrivatePart
    if ($actualProductVersion -ne $ProductVersion -or
        $actualFileVersion -ne $ProductVersion -or
        $actualNumericVersion -ne $NumericVersion)
    {
        throw "Executable version mismatch for $Path`: expected $ProductVersion / $NumericVersion, got $actualProductVersion / $actualFileVersion / $actualNumericVersion"
    }
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$configurePreset = 'alpha-x64'
$buildPreset = 'alpha-release'
$buildRootName = 'alpha-x64'
$variantSuffix = ''
if ($Slim)
{
    $configurePreset = 'alpha-x64-slim'
    $buildPreset = 'alpha-slim-release'
    $buildRootName = 'alpha-x64-slim'
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
        throw 'VCPKG_ROOT is required to build the standard Spout2-enabled installer.'
    }
}
$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if ($null -eq $cmake)
{
    throw 'cmake.exe was not found on PATH.'
}
$isccPath = Resolve-Iscc -RequestedPath $ISCC -RepositoryRoot $repositoryRoot
Assert-IsccDiagnosticsSupport -Path $isccPath
$version = Get-BafxVersion -VersionFile (Join-Path $repositoryRoot 'cmake\Version.cmake')
$numericVersions = Get-NumericVersions -Version $version
$numericVersion = [string]$numericVersions.numericVersion
$packageVersion = [string]$numericVersions.packageVersion
$outputRoot = Get-FullPath -Path $OutputDirectory -BaseDirectory $repositoryRoot
$outputBaseName = "ba-click-fx-desktop-$version$variantSuffix-setup-windows-x64"
$installerPath = Join-Path $outputRoot "$outputBaseName.exe"
$checksumPath = "$installerPath.sha256"

if (Test-Path -LiteralPath $installerPath -PathType Leaf)
{
    throw "Refusing to overwrite an existing user installer: $installerPath"
}
if (Test-Path -LiteralPath $checksumPath -PathType Leaf)
{
    throw "Refusing to overwrite an existing installer checksum: $checksumPath"
}
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

if (-not $SkipBuild)
{
    Invoke-Checked `
        -Description 'CMake configure' `
        -FilePath $cmake.Source `
        -Arguments @('--preset', $configurePreset) `
        -WorkingDirectory $repositoryRoot
    Invoke-Checked `
        -Description 'Release build' `
        -FilePath $cmake.Source `
        -Arguments @('--build', '--preset', $buildPreset) `
        -WorkingDirectory $repositoryRoot
}

$hostExecutable = Join-Path $repositoryRoot "build\$buildRootName\src\desktop\Release\ba-click-fx-desktop.exe"
$controlCenterExecutable = Join-Path $repositoryRoot "build\$buildRootName\src\control-center\Release\BAFX.ControlCenter.exe"
$identitySignerExecutable = Join-Path $repositoryRoot "build\$buildRootName\src\identity-signer\Release\BAFX.IdentitySigner.exe"
foreach ($required in @($hostExecutable, $controlCenterExecutable))
{
    if (-not (Test-Path -LiteralPath $required -PathType Leaf))
    {
        throw "Required Release executable is missing: $required"
    }

    # Both user-facing executables must expose the same release identity in
    # Explorer. The fixed version also controls Windows upgrade comparisons.
    Assert-ExecutableVersion `
        -Path $required `
        -ProductVersion $version `
        -NumericVersion $numericVersion
}
if (-not (Test-Path -LiteralPath $identitySignerExecutable -PathType Leaf))
{
    throw "Required Release identity signer is missing: $identitySignerExecutable"
}

$temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$temporaryRoot = Join-Path $temporaryParent ('bafx-user-installer-' + [Guid]::NewGuid().ToString('N'))
$stageRoot = Join-Path $temporaryRoot 'stage'
$identityDirectory = Join-Path $stageRoot 'Identity'
$installerDirectory = Join-Path $stageRoot 'Installer'

try
{
    New-Item -ItemType Directory -Path $identityDirectory -Force | Out-Null
    New-Item -ItemType Directory -Path $installerDirectory -Force | Out-Null
    Copy-Item -LiteralPath $hostExecutable -Destination (Join-Path $stageRoot 'ba-click-fx-desktop.exe')
    Copy-Item -LiteralPath $controlCenterExecutable -Destination (Join-Path $stageRoot 'BAFX.ControlCenter.exe')
    Copy-Item `
        -LiteralPath $identitySignerExecutable `
        -Destination (Join-Path $installerDirectory 'BAFX.IdentitySigner.exe')
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'LICENSE') -Destination (Join-Path $stageRoot 'LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'SUPPORT.md') -Destination $stageRoot

    foreach ($scriptName in @(
        'capture-user-context.ps1',
        'installer-diagnostics.ps1',
        'install-machine.ps1',
        'protected-paths.ps1',
        'register-user-package.ps1',
        'unregister-machine.ps1'
    ))
    {
        Copy-Item `
            -LiteralPath (Join-Path $PSScriptRoot "installer\$scriptName") `
            -Destination $installerDirectory
    }

    & (Join-Path $repositoryRoot 'tools\identity-package\build-identity-package.ps1') `
        -HostExecutable (Join-Path $stageRoot 'ba-click-fx-desktop.exe') `
        -OutputDirectory $identityDirectory `
        -PackageVersion $packageVersion `
        -UnsignedTemplate
    if (-not $?)
    {
        throw 'Sparse package build failed.'
    }

    $rawMetadataFile = Get-ChildItem `
        -LiteralPath $identityDirectory `
        -Filter '*.identity-template.json' `
        -File |
        Select-Object -First 1
    if ($null -eq $rawMetadataFile)
    {
        throw 'Sparse package metadata was not created.'
    }
    $rawMetadata = Get-Content -LiteralPath $rawMetadataFile.FullName -Raw | ConvertFrom-Json
    $templateFile = Get-ChildItem `
        -LiteralPath $identityDirectory `
        -Filter '*.unsigned.msix' `
        -File |
        Select-Object -First 1
    if ($null -eq $templateFile)
    {
        throw 'Unsigned sparse-package template is missing.'
    }
    $unexpectedIdentityFiles = @(
        Get-ChildItem -LiteralPath $identityDirectory -File |
            Where-Object {
                $_.Extension -in @('.cer', '.pfx', '.pvk', '.snk', '.key', '.pem') -or
                ($_.Extension -eq '.msix' -and $_.Name -notlike '*.unsigned.msix')
            }
    )
    if ($unexpectedIdentityFiles.Count -ne 0)
    {
        throw "Signed package, certificate, or private key entered the installer stage: $($unexpectedIdentityFiles.Name -join ', ')"
    }
    $templateSignature = Get-AuthenticodeSignature -LiteralPath $templateFile.FullName
    if ($templateSignature.Status -ne [Management.Automation.SignatureStatus]::NotSigned)
    {
        throw "Identity template must remain unsigned: $($templateSignature.Status)"
    }
    $expectedTemplateHash = (Get-FileHash -LiteralPath $templateFile.FullName -Algorithm SHA256).Hash
    if ([int]$rawMetadata.schema -ne 2 -or
        [string]$rawMetadata.identityMode -ne 'target-machine-self-signed' -or
        [string]$rawMetadata.templateFile -ne $templateFile.Name -or
        [string]$rawMetadata.templateSha256 -ne $expectedTemplateHash)
    {
        throw 'Unsigned identity-template metadata is invalid.'
    }
    $signerStagePath = Join-Path $installerDirectory 'BAFX.IdentitySigner.exe'
    $normalizedMetadata = [ordered]@{
        schema = 3
        identityMode = 'target-machine-self-signed'
        productVersion = $version
        packageName = [string]$rawMetadata.packageName
        applicationId = [string]$rawMetadata.applicationId
        publisher = [string]$rawMetadata.publisher
        packageVersion = [string]$rawMetadata.packageVersion
        templateFile = $templateFile.Name
        templateSha256 = $expectedTemplateHash
        hostSha256 = (Get-FileHash -LiteralPath (Join-Path $stageRoot 'ba-click-fx-desktop.exe') -Algorithm SHA256).Hash
        signerFile = 'Installer/BAFX.IdentitySigner.exe'
        signerSha256 = (Get-FileHash -LiteralPath $signerStagePath -Algorithm SHA256).Hash
    }
    Write-Utf8NoBom `
        -Path $rawMetadataFile.FullName `
        -Content ($normalizedMetadata | ConvertTo-Json -Depth 4)

    if (@(
            Get-ChildItem -LiteralPath $stageRoot -Recurse -File |
                Where-Object { $_.Extension -in @('.pfx', '.pvk', '.snk', '.key', '.pem') }
        ).Count -ne 0)
    {
        throw 'A private key container entered the installer stage.'
    }

    $payloadFiles = @(
        Get-ChildItem -LiteralPath $stageRoot -Recurse -File |
            Where-Object { $_.FullName -ne (Join-Path $installerDirectory 'INSTALLER-PAYLOAD.json') } |
            Sort-Object FullName |
            ForEach-Object {
                [ordered]@{
                    path = $_.FullName.Substring($stageRoot.Length).TrimStart('\').Replace('\', '/')
                    bytes = [Int64]$_.Length
                    sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
                }
            }
    )
    $payloadManifest = [ordered]@{
        schema = 2
        version = $version
        identityMode = 'target-machine-self-signed'
        files = $payloadFiles
    }
    Write-Utf8NoBom `
        -Path (Join-Path $installerDirectory 'INSTALLER-PAYLOAD.json') `
        -Content ($payloadManifest | ConvertTo-Json -Depth 5)

    $innoScript = Join-Path $PSScriptRoot 'installer\ba-click-fx-desktop.iss'
    Invoke-Checked `
        -Description 'Inno Setup compile' `
        -FilePath $isccPath `
        -Arguments @(
            "/DStageRoot=$stageRoot",
            "/DOutputRoot=$outputRoot",
            "/DProductVersion=$version",
            "/DNumericVersion=$numericVersion",
            "/DPackageVersion=$packageVersion",
            "/DOutputBaseName=$outputBaseName",
            $innoScript
        ) `
        -WorkingDirectory $repositoryRoot

    if (-not (Test-Path -LiteralPath $installerPath -PathType Leaf))
    {
        throw "Inno Setup did not create the expected installer: $installerPath"
    }

    if (-not $SkipVerification)
    {
        $linker = Get-CMakeLinker -CacheFile (Join-Path $repositoryRoot "build\$buildRootName\CMakeCache.txt")
        & (Join-Path $repositoryRoot 'tools\verify-portable-pe.ps1') `
            -Executable $installerPath `
            -Linker $linker
        if (-not $?)
        {
            throw 'Installer PE dependency verification failed.'
        }
        $versionInfo = (Get-Item -LiteralPath $installerPath).VersionInfo
        $installerProductVersion = ([string]$versionInfo.ProductVersion).Trim()
        if ($installerProductVersion -ne $version)
        {
            throw "Installer ProductVersion mismatch: $installerProductVersion"
        }
    }

    $installerHash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash
    Set-Content -LiteralPath $checksumPath -Encoding Ascii -NoNewline `
        -Value "$installerHash *$([IO.Path]::GetFileName($installerPath))"

    Write-Host "User installer created: $installerPath"
    Write-Host "SHA-256: $installerHash"
    Write-Host 'The installer contains an unsigned identity template and native signer, but no certificate or private key.'
    Write-Host 'The target machine requires no Windows SDK.'
}
finally
{
    if (Test-Path -LiteralPath $temporaryRoot -PathType Container)
    {
        $temporaryPrefix = $temporaryParent.TrimEnd('\') + '\'
        $resolvedTemporaryRoot = [IO.Path]::GetFullPath($temporaryRoot)
        if (-not $resolvedTemporaryRoot.StartsWith(
                $temporaryPrefix,
                [StringComparison]::OrdinalIgnoreCase))
        {
            throw "Refusing to clean a path outside the temporary directory: $resolvedTemporaryRoot"
        }
        Remove-Item -LiteralPath $resolvedTemporaryRoot -Recurse -Force
    }
}
