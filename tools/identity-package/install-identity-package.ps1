[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDirectory,

    [string]$InstallDirectory = "$env:ProgramFiles\ba-click-fx-desktop",
    [string]$PackageName = 'CialloKing.BaClickFxDesktop',
    [string]$ApplicationId = 'BaClickFxDesktop',
    [string]$Publisher = 'CN=BaClickFx.Local',
    [string]$PublisherDisplayName = 'ba-click-fx-desktop contributors',
    [string]$DisplayName = 'ba-click-fx-desktop',
    [string]$PackageVersion = '0.1.0.7',
    [switch]$DisableSystemBorder,
    [switch]$AllowUserWritableInstall,
    [switch]$Launch
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Administrator
{
    $principal = New-Object Security.Principal.WindowsPrincipal(
        [Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
    {
        throw '方案 C 安装必须在管理员 PowerShell 中运行，以便写入 LocalMachine\TrustedPeople。'
    }
}

function Assert-WindowsPowerShell
{
    if ($PSVersionTable.PSEdition -ne 'Desktop')
    {
        throw '注册 Sparse Package 需要 Windows PowerShell 5.1，请使用 powershell.exe 而不是 pwsh.exe。'
    }
    foreach ($commandName in @(
        'Add-AppxPackage',
        'Get-AppxPackage',
        'Remove-AppxPackage',
        'Import-Certificate',
        'New-SelfSignedCertificate',
        'Export-Certificate'))
    {
        if ($null -eq (Get-Command $commandName -ErrorAction SilentlyContinue))
        {
            throw "Required Appx cmdlet is unavailable: $commandName"
        }
    }
    $addAppx = Get-Command Add-AppxPackage -ErrorAction Stop
    if (-not $addAppx.Parameters.ContainsKey('ExternalLocation'))
    {
        throw 'Add-AppxPackage does not expose -ExternalLocation on this Windows build.'
    }
}

function Assert-SafeIdentityValues
{
    if ($PackageName -notmatch '^[A-Za-z0-9.\-]{3,50}$')
    {
        throw "Invalid PackageName: $PackageName"
    }
    if ($ApplicationId -notmatch '^[A-Za-z0-9.\-]{1,50}$')
    {
        throw "Invalid ApplicationId: $ApplicationId"
    }
    if ($PackageVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$')
    {
        throw "Invalid PackageVersion: $PackageVersion"
    }
    if ([string]::IsNullOrWhiteSpace($Publisher) -or $Publisher.Contains("`n") -or $Publisher.Contains("`r"))
    {
        throw 'Publisher must be a single non-empty certificate subject.'
    }
}

function Grant-DataDirectoryAccess
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    New-Item -ItemType Directory -Path $Path -Force | Out-Null
    $acl = Get-Acl -LiteralPath $Path
    $currentUser = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $rule = New-Object Security.AccessControl.FileSystemAccessRule(
        $currentUser,
        'Modify',
        'ContainerInherit,ObjectInherit',
        'None',
        'Allow')
    $acl.SetAccessRule($rule)
    Set-Acl -LiteralPath $Path -AclObject $acl
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
                throw "Host is running from the target install directory (PID $($process.Id)); exit it before installing."
            }
        }
        catch [System.ComponentModel.Win32Exception]
        {
            # Access to another process path can be denied; do not stop an
            # unrelated process merely because its path cannot be inspected.
        }
    }
}

function Write-Utf8NoBom
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Content
    )

    # The native JSON parser consumes UTF-8 bytes directly and does not need a
    # PowerShell 5.1 BOM.  Keep generated runtime files identical to files
    # written by the C++ configuration layer.
    $encoding = New-Object -TypeName System.Text.UTF8Encoding -ArgumentList $false
    [IO.File]::WriteAllText($Path, $Content, $encoding)
}

function Assert-ConfigObjectFields
{
    param(
        [Parameter(Mandatory = $true)]
        [AllowNull()]
        [object]$Value,

        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string[]]$ExpectedFields
    )

    if ($null -eq $Value -or $Value -isnot [PSCustomObject])
    {
        throw "Generated configuration field '$Path' must be an object."
    }

    $actualFields = @(
        $Value.PSObject.Properties |
            ForEach-Object { $_.Name }
    )
    $missingFields = @(
        $ExpectedFields |
            Where-Object { $actualFields -notcontains $_ }
    )
    $unknownFields = @(
        $actualFields |
            Where-Object { $ExpectedFields -notcontains $_ }
    )
    if ($missingFields.Count -eq 0 -and $unknownFields.Count -eq 0)
    {
        return
    }

    $details = New-Object Collections.Generic.List[string]
    if ($missingFields.Count -gt 0)
    {
        $details.Add("missing=$($missingFields -join ',')")
    }
    if ($unknownFields.Count -gt 0)
    {
        $details.Add("unknown=$($unknownFields -join ',')")
    }
    throw "Generated configuration field '$Path' does not match schemaVersion 12 ($($details -join '; '))."
}

function Set-IdentityInstallConfig
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$HostPath,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [Parameter(Mandatory = $true)]
        [string]$DataPath
    )

    # Directly starting an external-location EXE is not guaranteed to carry
    # Package Identity.  Run the bootstrap from the install directory and
    # copy any portable-root files into data before editing the configuration.
    # This keeps the final install layout correct even before the first Shell
    # package activation has established identity for the process.
    $supportInfoName = 'identity-install-support.txt'
    $exitCode = 0
    Push-Location -LiteralPath $InstallRoot
    try
    {
        & $HostPath "--support-info=$supportInfoName" | Out-Null
        $exitCode = $LASTEXITCODE
    }
    finally
    {
        Pop-Location
    }
    if ($exitCode -ne 0)
    {
        throw "Host configuration bootstrap failed with exit code $exitCode."
    }

    foreach ($fileName in @('BAFX.config.json', 'ba-click-fx-desktop-support.log'))
    {
        $rootPath = Join-Path $InstallRoot $fileName
        $destinationPath = Join-Path $DataPath $fileName
        if (Test-Path -LiteralPath $rootPath -PathType Leaf)
        {
            if (Test-Path -LiteralPath $destinationPath -PathType Leaf)
            {
                Remove-Item -LiteralPath $rootPath -Force
            }
            else
            {
                # Moving preserves the protected root ACL.  Copying into the
                # already writable data directory makes the file inherit its
                # user Modify rule before the root bootstrap file is removed.
                Copy-Item -LiteralPath $rootPath -Destination $destinationPath
                Remove-Item -LiteralPath $rootPath -Force
            }
        }
    }

    $configPath = Join-Path $DataPath 'BAFX.config.json'
    if (-not (Test-Path -LiteralPath $configPath -PathType Leaf))
    {
        throw "Host did not create the identity configuration: $configPath"
    }
    $config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
    $schemaVersionProperty = $config.PSObject.Properties['schemaVersion']
    if ($null -eq $schemaVersionProperty `
        -or -not ($schemaVersionProperty.Value -is [ValueType]) `
        -or [double]$schemaVersionProperty.Value -ne 12.0)
    {
        throw 'Generated configuration must use schemaVersion 12.'
    }

    # The Host keeps an invalid persisted document while using defaults only in
    # memory. Verify the exact shape before editing so installation cannot make
    # a stale document look like a valid current-schema configuration.
    Assert-ConfigObjectFields `
        -Value $config `
        -Path '$' `
        -ExpectedFields @(
            'schemaVersion',
            'effects',
            'background',
            'display',
            'input',
            'performance',
            'system'
        )
    Assert-ConfigObjectFields `
        -Value $config.effects `
        -Path 'effects' `
        -ExpectedFields @(
            'enabled',
            'globalScale',
            'opacity',
            'clickEnabled',
            'trailEnabled',
            'trailLength',
            'trailWidth',
            'clickTimeScale',
            'trailTimeScale',
            'trailLifetimeMs',
            'diskLifetimeMs',
            'diskRadius',
            'ringsCount',
            'ringsLifetimeMs',
            'ringsRadiusMin',
            'ringsRadiusMax',
            'ringsAngularVelocityMultiplier',
            'ringsRotationDirection',
            'ringsHdrIntensity',
            'shardsHdrIntensity',
            'trailOpacity',
            'bloomIntensity',
            'bloomDiffusion',
            'bloomThreshold',
            'bloomSoftKnee',
            'bloomClamp'
        )
    Assert-ConfigObjectFields `
        -Value $config.background `
        -Path 'background' `
        -ExpectedFields @('mode', 'cursorExcluded', 'allowSystemBorder')
    Assert-ConfigObjectFields `
        -Value $config.display `
        -Path 'display' `
        -ExpectedFields @('hdrEnabled')
    Assert-ConfigObjectFields `
        -Value $config.input `
        -Path 'input' `
        -ExpectedFields @(
            'leftClick',
            'rightClick',
            'middleClick',
            'trailOnlyWhilePressed',
            'samplingRateHz'
        )
    Assert-ConfigObjectFields `
        -Value $config.performance `
        -Path 'performance' `
        -ExpectedFields @('idleOptimization', 'framePacing')
    Assert-ConfigObjectFields `
        -Value $config.system `
        -Path 'system' `
        -ExpectedFields @('startWithWindows', 'startMinimized', 'closeToTray')

    if ($DisableSystemBorder)
    {
        $config.background.allowSystemBorder = $false
    }
    $configJson = $config | ConvertTo-Json -Depth 12
    Write-Utf8NoBom -Path $configPath -Content $configJson
    foreach ($reportRoot in @($InstallRoot, $DataPath))
    {
        $supportInfo = Join-Path $reportRoot $supportInfoName
        if (Test-Path -LiteralPath $supportInfo -PathType Leaf)
        {
            Remove-Item -LiteralPath $supportInfo -Force
        }
    }
}

function Remove-RegisteredPackagesForRollback
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [string[]]$PreexistingFullNames = @()
    )

    $failed = $false
    try
    {
        $candidates = @(Get-AppxPackage -Name $Name -ErrorAction Stop)
        foreach ($candidate in $candidates)
        {
            if ($PreexistingFullNames -contains [string]$candidate.PackageFullName)
            {
                continue
            }
            try
            {
                Remove-AppxPackage -Package $candidate.PackageFullName -ErrorAction Stop
            }
            catch
            {
                $failed = $true
                Write-Warning "Could not remove partially registered package $($candidate.PackageFullName): $($_.Exception.Message)"
            }
        }

        # Deployment can finish asynchronously.  Do not delete the external
        # location while a new registration is still visible to Appx.
        for ($attempt = 0; $attempt -lt 10; ++$attempt)
        {
            $remaining = @(Get-AppxPackage -Name $Name -ErrorAction Stop |
                Where-Object { $PreexistingFullNames -notcontains [string]$_.PackageFullName })
            if ($remaining.Count -eq 0)
            {
                break
            }
            Start-Sleep -Milliseconds 200
        }
        $remaining = @(Get-AppxPackage -Name $Name -ErrorAction Stop |
            Where-Object { $PreexistingFullNames -notcontains [string]$_.PackageFullName })
        if ($remaining.Count -gt 0)
        {
            $failed = $true
            Write-Warning "Package registration remains after rollback: $($remaining.PackageFullName -join ', ')"
        }
    }
    catch
    {
        $failed = $true
        Write-Warning "Could not inspect Appx registration during rollback: $($_.Exception.Message)"
    }
    return (-not $failed)
}

Assert-Administrator
Assert-WindowsPowerShell
Assert-SafeIdentityValues

$sourceRoot = [IO.Path]::GetFullPath($SourceDirectory)
$installRoot = [IO.Path]::GetFullPath($InstallDirectory)
$hostSource = Join-Path $sourceRoot 'ba-click-fx-desktop.exe'
if (-not (Test-Path -LiteralPath $hostSource -PathType Leaf))
{
    throw "Source Host was not found: $hostSource"
}

$programFilesRoots = @(
    $env:ProgramFiles,
    ${env:ProgramFiles(x86)}
) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
    ForEach-Object { [IO.Path]::GetFullPath($_) }
$isProtectedRoot = $false
foreach ($root in $programFilesRoots)
{
    if ($installRoot.StartsWith($root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase))
    {
        $isProtectedRoot = $true
    }
}
if (-not $isProtectedRoot -and -not $AllowUserWritableInstall)
{
    throw 'Identity EXE must be installed below Program Files; use -AllowUserWritableInstall only for a disposable local Spike.'
}

Assert-HostIsNotRunning -Path (Join-Path $installRoot 'ba-click-fx-desktop.exe')
if (Test-Path -LiteralPath $installRoot -PathType Container)
{
    $entries = @(Get-ChildItem -LiteralPath $installRoot -Force)
    if ($entries.Count -gt 0)
    {
        throw "Install directory is not empty: $installRoot"
    }
}

$identityDirectory = Join-Path $installRoot 'Identity'
$dataDirectory = Join-Path $installRoot 'data'
$installRootExisted = Test-Path -LiteralPath $installRoot -PathType Container
$createdInstallRoot = -not $installRootExisted
$registeredPackage = $null
$importedThumbprint = $null
$existingPackageFullNames = @()
$registrationAttempted = $false

try
{
    New-Item -ItemType Directory -Path $installRoot -Force | Out-Null
    Copy-Item -LiteralPath $hostSource -Destination (Join-Path $installRoot 'ba-click-fx-desktop.exe')
    foreach ($optionalFile in @('BAFX.ControlCenter.exe', 'LICENSE.txt', 'SUPPORT.md'))
    {
        $sourcePath = Join-Path $sourceRoot $optionalFile
        if (Test-Path -LiteralPath $sourcePath -PathType Leaf)
        {
            Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $installRoot $optionalFile)
        }
    }
    Grant-DataDirectoryAccess -Path $dataDirectory
    New-Item -ItemType Directory -Path $identityDirectory -Force | Out-Null

    $buildScript = Join-Path $PSScriptRoot 'build-identity-package.ps1'
    & $buildScript `
        -HostExecutable (Join-Path $installRoot 'ba-click-fx-desktop.exe') `
        -OutputDirectory $identityDirectory `
        -PackageName $PackageName `
        -ApplicationId $ApplicationId `
        -Publisher $Publisher `
        -PublisherDisplayName $PublisherDisplayName `
        -DisplayName $DisplayName `
        -PackageVersion $PackageVersion
    if (-not $?)
    {
        throw 'Sparse package build failed.'
    }

    $packagePath = Join-Path $identityDirectory "$PackageName-$PackageVersion.msix"
    $certificatePath = Join-Path $identityDirectory "$PackageName-$PackageVersion.cer"
    $buildMetadataPath = Join-Path $identityDirectory "$PackageName-$PackageVersion.identity.json"
    $buildMetadata = Get-Content -LiteralPath $buildMetadataPath -Raw | ConvertFrom-Json
    $thumbprintProperty = $buildMetadata.PSObject.Properties['certificateThumbprint']
    if ($null -eq $thumbprintProperty)
    {
        throw 'Identity package metadata has no certificate thumbprint.'
    }
    $importedThumbprint = [string]$thumbprintProperty.Value
    if ($importedThumbprint -notmatch '^[0-9A-Fa-f]{40}$')
    {
        throw 'Identity package metadata contains an invalid certificate thumbprint.'
    }
    $imported = Import-Certificate `
        -FilePath $certificatePath `
        -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople'
    if ($imported.Thumbprint -ne $importedThumbprint)
    {
        throw 'Imported certificate thumbprint does not match the signed package metadata.'
    }

    $existing = @(Get-AppxPackage -Name $PackageName -ErrorAction Stop)
    $existingPackageFullNames = @($existing |
        ForEach-Object { [string]$_.PackageFullName })
    if ($existing.Count -gt 0)
    {
        throw "Package $PackageName is already registered; run uninstall-identity-package.ps1 first."
    }
    $registrationAttempted = $true
    Add-AppxPackage -Path $packagePath -ExternalLocation $installRoot
    $registeredPackage = Get-AppxPackage -Name $PackageName -ErrorAction Stop |
        Select-Object -First 1
    if ($null -eq $registeredPackage)
    {
        throw 'Add-AppxPackage returned without a registered package.'
    }

    $installMetadata = [ordered]@{
        schema = 1
        packageName = $PackageName
        applicationId = $ApplicationId
        publisher = $Publisher
        packageVersion = $PackageVersion
        packageFullName = $registeredPackage.PackageFullName
        packageFamilyName = $registeredPackage.PackageFamilyName
        certificateThumbprint = $importedThumbprint
        externalLocation = $installRoot
        installedUtc = [DateTime]::UtcNow.ToString('o')
    }
    $metadataPath = Join-Path $dataDirectory 'identity-install.json'
    $installMetadataJson = $installMetadata | ConvertTo-Json -Depth 5
    Write-Utf8NoBom -Path $metadataPath -Content $installMetadataJson
    Set-IdentityInstallConfig `
        -HostPath (Join-Path $installRoot 'ba-click-fx-desktop.exe') `
        -InstallRoot $installRoot `
        -DataPath $dataDirectory

    Write-Host "Sparse package registered: $($registeredPackage.PackageFullName)"
    Write-Host "External location: $installRoot"
    Write-Host "Certificate thumbprint: $importedThumbprint"
    if ($Launch)
    {
        $activation = "shell:AppsFolder\$($registeredPackage.PackageFamilyName)!$ApplicationId"
        Start-Process -FilePath 'explorer.exe' -ArgumentList $activation
    }
}
catch
{
    $installationError = $_
    $packageRollbackSucceeded = $true
    if ($registrationAttempted)
    {
        if ($existingPackageFullNames.Count -gt 0)
        {
            $packageRollbackSucceeded = Remove-RegisteredPackagesForRollback `
                -Name $PackageName `
                -PreexistingFullNames $existingPackageFullNames
        }
        else
        {
            $packageRollbackSucceeded = Remove-RegisteredPackagesForRollback `
                -Name $PackageName
        }
    }
    $certificateRollbackSucceeded = $true
    if ($packageRollbackSucceeded -and $null -ne $importedThumbprint)
    {
        try
        {
            $certificate = Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
                Where-Object { $_.Thumbprint -eq $importedThumbprint } |
                Select-Object -First 1
            if ($null -ne $certificate)
            {
                Remove-Item -LiteralPath $certificate.PSPath -Force -ErrorAction Stop
            }
        }
        catch
        {
            $certificateRollbackSucceeded = $false
            Write-Warning "Could not remove imported certificate ${importedThumbprint}: $($_.Exception.Message)"
        }
    }
    $installRollbackSucceeded = $true
    if ($packageRollbackSucceeded -and $certificateRollbackSucceeded -and (Test-Path -LiteralPath $installRoot -PathType Container))
    {
        try
        {
            if ($createdInstallRoot)
            {
                Remove-Item -LiteralPath $installRoot -Recurse -Force -ErrorAction Stop
            }
            else
            {
                $remainingEntries = @(Get-ChildItem -LiteralPath $installRoot -Force)
                if ($remainingEntries.Count -eq 0)
                {
                    Remove-Item -LiteralPath $installRoot -Force -ErrorAction Stop
                }
                else
                {
                    $installRollbackSucceeded = $false
                    Write-Warning "Partially created install directory was not empty after rollback: $installRoot"
                }
            }
        }
        catch
        {
            $installRollbackSucceeded = $false
            Write-Warning "Could not remove the partially created install directory: $($_.Exception.Message)"
        }
    }
    elseif ($createdInstallRoot)
    {
        $installRollbackSucceeded = $false
    }
    if (-not ($packageRollbackSucceeded -and $certificateRollbackSucceeded -and $installRollbackSucceeded))
    {
        throw "Installation failed: $($installationError.Exception.Message). Rollback was incomplete; inspect $installRoot and remove any remaining Appx registration before retrying."
    }
    throw $installationError.Exception
}
