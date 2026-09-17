[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InstallDirectory,

    [Parameter(Mandatory = $true)]
    [string]$MachineStatePath,

    [Parameter(Mandatory = $true)]
    [string]$ResultPath,

    [string]$PayloadDirectory = '',

    [switch]$Rollback,

    [ValidateSet('RemoveNew', 'RestorePrevious', 'Both')]
    [string]$RollbackAction = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installer-diagnostics.ps1')
. (Join-Path $PSScriptRoot 'protected-paths.ps1')
. (Join-Path $PSScriptRoot 'installer-state.ps1')
$script:InstallerStep = 'initialize'
$effectiveRollbackAction = $RollbackAction
if ($Rollback -and [string]::IsNullOrWhiteSpace($effectiveRollbackAction))
{
    $effectiveRollbackAction = 'Both'
}
$script:InstallerPhase = if (-not [string]::IsNullOrWhiteSpace($effectiveRollbackAction))
{
    'RollbackUserPackage'
}
else
{
    'RegisterUserPackage'
}
$script:InstallerDiagnosticPath = "$ResultPath.diagnostic.txt"
$script:InstallerState = $null
$script:InstallerProductVersion = ''
$script:InstallerPackageVersion = ''
$script:InstallerExitCode = 1
$script:InstallerRelatedFailures = New-Object Collections.Generic.List[object]

function Add-InstallerRelatedFailure
{
    param(
        [Parameter(Mandatory = $true)]
        [Management.Automation.ErrorRecord]$ErrorRecord,

        [Parameter(Mandatory = $true)]
        [string]$Step
    )

    $script:InstallerRelatedFailures.Add((New-BafxInstallerRelatedFailure `
            -ErrorRecord $ErrorRecord `
            -Step $Step))
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

function Get-InstallerFileHash
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    # ExecAsOriginalUser may run with module auto-loading disabled. Keep the
    # integrity check independent of Microsoft.PowerShell.Utility.
    $stream = [IO.File]::OpenRead($Path)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try
    {
        return ([BitConverter]::ToString($hasher.ComputeHash($stream))).Replace('-', '')
    }
    finally
    {
        $hasher.Dispose()
        $stream.Dispose()
    }
}

function Get-InstallStatePairStatus
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [Parameter(Mandatory = $true)]
        [object]$PendingState
    )

    $statePath = Join-Path $InstallRoot 'Installer\INSTALL-STATE.json'
    $backupPath = "$statePath.bak"
    $primaryExists = Test-Path -LiteralPath $statePath -PathType Leaf
    $backupExists = Test-Path -LiteralPath $backupPath -PathType Leaf
    if (-not $primaryExists -and -not $backupExists)
    {
        return [pscustomobject]@{
            present = $false
            valid = $false
            transactionId = ''
            reason = 'absent'
        }
    }
    if (-not $primaryExists -or -not $backupExists)
    {
        return [pscustomobject]@{
            present = $true
            valid = $false
            transactionId = ''
            reason = 'primary-and-backup-are-not-a-pair'
        }
    }

    try
    {
        Assert-ProtectedStateAcl -Path $statePath
        Assert-ProtectedStateAcl -Path $backupPath
        Assert-InstallStateRawPair `
            -PrimaryPath $statePath `
            -BackupPath $backupPath
        $primary = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
        $backup = Get-Content -LiteralPath $backupPath -Raw | ConvertFrom-Json
        $primaryTransaction = if ($null -eq $primary.PSObject.Properties['transactionId'])
        {
            ''
        }
        else
        {
            [string]$primary.transactionId
        }
        $backupTransaction = if ($null -eq $backup.PSObject.Properties['transactionId'])
        {
            ''
        }
        else
        {
            [string]$backup.transactionId
        }
        if ($primaryTransaction -notmatch '^[0-9a-fA-F]{32}$' -or
            $primaryTransaction -ne $backupTransaction)
        {
            throw 'Install-state transactions differ.'
        }

        $primaryDigest = if ($null -eq $primary.PSObject.Properties['stateDigest'])
        {
            ''
        }
        else
        {
            [string]$primary.stateDigest
        }
        $backupDigest = if ($null -eq $backup.PSObject.Properties['stateDigest'])
        {
            ''
        }
        else
        {
            [string]$backup.stateDigest
        }
        if ($primaryDigest -match '^[0-9A-Fa-f]{64}$' -and
            $primaryDigest -eq $backupDigest)
        {
            if ((Get-StateDigest -Value $primary) -ne $primaryDigest -or
                (Get-StateDigest -Value $backup) -ne $backupDigest)
            {
                throw 'Install-state digest does not match its content.'
            }
        }
        elseif ([string]::IsNullOrWhiteSpace($primaryDigest) -and
            [string]::IsNullOrWhiteSpace($backupDigest))
        {
            $primaryCanonical = Get-StatePropertiesWithoutDigest -Value $primary |
                ConvertTo-Json -Depth 12
            $backupCanonical = Get-StatePropertiesWithoutDigest -Value $backup |
                ConvertTo-Json -Depth 12
            if ($primaryCanonical -ne $backupCanonical)
            {
                throw 'Legacy install-state contents differ.'
            }
        }
        else
        {
            throw 'Install-state digests differ.'
        }

        return [pscustomobject]@{
            present = $true
            valid = $true
            transactionId = $primaryTransaction
            reason = 'valid'
        }
    }
    catch
    {
        return [pscustomobject]@{
            present = $true
            valid = $false
            transactionId = ''
            reason = $_.Exception.Message
        }
    }
}

function Assert-PendingRollbackMayRemoveNewPackage
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot
    )

    $commitState = if ($null -ne $State.PSObject.Properties['commitState'])
    {
        [string]$State.commitState
    }
    else
    {
        'prepared'
    }
    $pair = Get-InstallStatePairStatus `
        -InstallRoot $InstallRoot `
        -PendingState $State
    $sameTransaction = $pair.valid -and
        [string]$pair.transactionId -eq [string]$State.transactionId
    if ($commitState -eq 'committed')
    {
        if ($sameTransaction)
        {
            return $false
        }
        if ($null -ne $State.PSObject.Properties['committedInstallState'])
        {
            # Finalize records a complete recovery snapshot before publishing
            # the committed marker. Let the elevated phase repair a torn pair;
            # removing the new package here would destroy a valid commit.
            return $false
        }
        $script:InstallerExitCode = 1001
        throw 'The pending transaction is marked committed, but its install-state pair cannot prove that commit.'
    }
    if ($sameTransaction)
    {
        # Finalize writes INSTALL-STATE before its committed journal marker. A
        # crash in that interval is already committed and must only finish
        # cleanup; removing its new registration would destroy the install.
        return $false
    }
    if ($pair.present -and -not $pair.valid)
    {
        # A present but damaged pair could be the result of a torn state
        # commit. Preserve the package and recovery evidence until repair can
        # establish which transaction is authoritative.
        $script:InstallerExitCode = 1001
        throw "The protected install-state pair cannot establish rollback safety: $($pair.reason)"
    }
    return $true
}

function Write-Result
{
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Succeeded,

        [AllowNull()]
        [object]$Package,

        [Parameter(Mandatory = $true)]
        [object]$State,

        [string]$ErrorMessage = ''
    )

    $result = [ordered]@{
        schema = 1
        transactionId = [string]$State.transactionId
        succeeded = $Succeeded
        installedUserSid = [string]$State.userSid
        packageName = [string]$State.packageName
        packageVersion = [string]$State.packageVersion
        packageFullName = if ($null -eq $Package) { $null } else { [string]$Package.PackageFullName }
        packageFamilyName = if ($null -eq $Package) { $null } else { [string]$Package.PackageFamilyName }
        error = $ErrorMessage
        completedUtc = [DateTime]::UtcNow.ToString('o')
    }
    Write-Utf8NoBom -Path $ResultPath -Content ($result | ConvertTo-Json -Depth 4)
}

function Stop-RegistrationWithFailure
{
    param(
        [Parameter(Mandatory = $true)]
        [Management.Automation.ErrorRecord]$ErrorRecord,

        [Parameter(Mandatory = $true)]
        [string]$Step,

        [string]$ResultErrorMessage = '',

        [int]$ExitCode = 1
    )

    $failureMessage = if ([string]::IsNullOrWhiteSpace($ResultErrorMessage))
    {
        $ErrorRecord.Exception.Message
    }
    else
    {
        $ResultErrorMessage
    }
    try
    {
        if (-not (Test-Path -LiteralPath $ResultPath -PathType Leaf))
        {
            if ($null -ne $script:InstallerState)
            {
                Write-Result `
                    -Succeeded $false `
                    -Package $null `
                    -State $script:InstallerState `
                    -ErrorMessage $failureMessage
            }
            else
            {
                # Validation may fail before protected state is readable. Keep
                # a minimal result so the elevated parent can still distinguish
                # script failure from a process launch failure.
                $diagnosticResult = [ordered]@{
                    schema = 1
                    succeeded = $false
                    error = $failureMessage
                    completedUtc = [DateTime]::UtcNow.ToString('o')
                }
                Write-Utf8NoBom `
                    -Path $ResultPath `
                    -Content ($diagnosticResult | ConvertTo-Json -Depth 3)
            }
        }
    }
    catch
    {
        Add-InstallerRelatedFailure `
            -ErrorRecord $_ `
            -Step 'write-fallback-registration-result'
    }

    Write-BafxInstallerFailure `
        -ErrorRecord $ErrorRecord `
        -Phase $script:InstallerPhase `
        -Step $Step `
        -ProductVersion $script:InstallerProductVersion `
        -PackageVersion $script:InstallerPackageVersion `
        -DiagnosticPath $script:InstallerDiagnosticPath `
        -RelatedFailures $script:InstallerRelatedFailures.ToArray()
    exit $ExitCode
}

function Assert-ProtectedStateAcl
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    # ExecAsOriginalUser may not be able to load Microsoft.PowerShell.Security;
    # use the framework ACL API while preserving the same protection checks.
    $fileInfo = New-Object System.IO.FileInfo($Path)
    $acl = $fileInfo.GetAccessControl()
    if (-not $acl.AreAccessRulesProtected)
    {
        throw 'Protected pending state still inherits access rules.'
    }
    $writeRights = [int]([Security.AccessControl.FileSystemRights]::WriteData -bor
        [Security.AccessControl.FileSystemRights]::AppendData -bor
        [Security.AccessControl.FileSystemRights]::WriteExtendedAttributes -bor
        [Security.AccessControl.FileSystemRights]::WriteAttributes -bor
        [Security.AccessControl.FileSystemRights]::Delete -bor
        [Security.AccessControl.FileSystemRights]::ChangePermissions -bor
        [Security.AccessControl.FileSystemRights]::TakeOwnership)
    foreach ($rule in $acl.Access)
    {
        if ($rule.AccessControlType -ne [Security.AccessControl.AccessControlType]::Allow)
        {
            continue
        }
        $sid = Resolve-InstallerAclIdentity `
            -Rule $rule `
            -WriteRights $writeRights `
            -Path $Path
        if ([string]::IsNullOrWhiteSpace($sid))
        {
            continue
        }
        if (Test-InstallerTrustedPrincipal -Sid $sid)
        {
            continue
        }
        if (([int]$rule.FileSystemRights -band $writeRights) -ne 0)
        {
            throw 'Protected pending state grants write access to a non-administrator.'
        }
    }
}

function Assert-PayloadFileSetLedger
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State
    )

    if ($null -eq $State.PSObject.Properties['payloadFileSet'])
    {
        return
    }
    $ledgerValue = $State.payloadFileSet
    if ($ledgerValue -isnot [string] -or
        [string]::IsNullOrWhiteSpace([string]$ledgerValue))
    {
        throw 'Protected pending state has an invalid payload file-set ledger.'
    }

    $seen = @{}
    foreach ($rawPath in @(([string]$ledgerValue) -split '\|'))
    {
        $path = $rawPath.Trim().Replace('/', '\')
        if ([string]::IsNullOrWhiteSpace($path) -or
            [IO.Path]::IsPathRooted($path) -or
            $path.StartsWith('\') -or
            $path.EndsWith('\') -or
            $path -match '(^|\\)(\.|\.\.)(\\|$)' -or
            $path -match '[<>:"|?*\x00-\x1f]')
        {
            throw "Protected pending state has an unsafe payload file path: $rawPath"
        }
        $key = $path.ToUpperInvariant()
        if ($seen.ContainsKey($key))
        {
            throw "Protected pending state has a duplicate payload file path: $rawPath"
        }
        $seen[$key] = $true
    }
}

function Assert-PendingState
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [string]$PayloadDirectory = ''
    )

    foreach ($propertyName in @(
        'schema',
        'stateKind',
        'transactionId',
        'userSid',
        'packageName',
        'applicationId',
        'publisher',
        'productVersion',
        'packageVersion',
        'preexistingPackageFullNames',
        'oldInstallState'))
    {
        if ($null -eq $State.PSObject.Properties[$propertyName])
        {
            throw "Protected pending state is missing: $propertyName"
        }
    }
    $schema = [int]$State.schema
    if ($schema -notin @(1, 2) -or [string]$State.stateKind -ne 'prepare')
    {
        throw 'Protected pending state has an unsupported schema.'
    }
    if ([string]$State.userSid -notmatch '^S-1-[0-9-]+$' -or
        [string]$State.packageName -ne 'CialloKing.BaClickFxDesktop' -or
        [string]$State.applicationId -ne 'BaClickFxDesktop' -or
        [string]$State.publisher -ne 'CN=BaClickFx.Local' -or
        [string]$State.productVersion -notmatch
            '^[0-9]+\.[0-9]+\.[0-9]+$' -or
        [string]$State.packageVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$')
    {
        throw 'Protected pending state contains invalid identity data.'
    }
    if ([string]$State.transactionId -notmatch '^[0-9a-fA-F]{32}$')
    {
        throw 'Protected pending state has an invalid transaction identifier.'
    }
    if ($null -ne $State.PSObject.Properties['stateDigest'])
    {
        $stateDigest = [string]$State.stateDigest
        if ($stateDigest -notmatch '^[0-9A-Fa-f]{64}$' -or
            (Get-StateDigest -Value $State) -ne $stateDigest)
        {
            throw 'Protected pending state digest does not match its content.'
        }
    }
    Assert-PayloadFileSetLedger -State $State
    if ($schema -eq 2 -and
        ($null -eq $State.PSObject.Properties['templateSha256'] -or
            [string]$State.templateSha256 -notmatch '^[0-9A-Fa-f]{64}$'))
    {
        throw 'Schema 2 pending state has an invalid template hash.'
    }
    if ($null -ne $State.PSObject.Properties['templateSha256'] -and
        [string]$State.templateSha256 -notmatch '^[0-9A-Fa-f]{64}$')
    {
        throw 'Protected pending state has an invalid template hash.'
    }
    $certificatePhase = if ($null -ne $State.PSObject.Properties['certificatePhase'])
    {
        [string]$State.certificatePhase
    }
    else
    {
        'ready'
    }
    if ($certificatePhase -eq 'creating')
    {
        if ([string]$State.transactionId -notmatch '^[0-9a-fA-F]{32}$' -or
            $null -eq $State.PSObject.Properties['certificateSanUri'] -or
            [string]$State.certificateSanUri -cne
                "urn:bafx:installer:$([string]$State.transactionId)")
        {
            throw 'Protected pending state has an invalid certificate creation marker.'
        }
        if ([string]::IsNullOrWhiteSpace($effectiveRollbackAction))
        {
            throw 'A certificate-creating transaction cannot register a package.'
        }
        return
    }
    foreach ($propertyName in @(
        'packagePath',
        'packageFile',
        'ownedCertificateThumbprints',
        'ownedPackageFiles'))
    {
        if ($null -eq $State.PSObject.Properties[$propertyName])
        {
            throw "Protected pending state is missing: $propertyName"
        }
    }
    foreach ($thumbprint in (([string]$State.ownedCertificateThumbprints) -split ',' |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }))
    {
        if ($thumbprint -notmatch '^[0-9A-Fa-f]{40}$')
        {
            throw 'Protected pending state has an invalid certificate ledger entry.'
        }
    }
    # Accept the short-lived PowerShell 5.1 space-delimited ledger so an
    # interrupted upgrade can still complete rollback under the original user.
    foreach ($ownedFile in (([string]$State.ownedPackageFiles) -split '[|\s]+' |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }))
    {
        if ([IO.Path]::IsPathRooted($ownedFile) -or
            $ownedFile.Contains('..') -or
            [IO.Path]::GetFileName($ownedFile) -ne $ownedFile -or
            $ownedFile -notmatch '\.msix$')
        {
            throw 'Protected pending state has an unsafe package ledger entry.'
        }
    }
    $packageFile = [string]$State.packageFile
    if ([IO.Path]::IsPathRooted($packageFile) -or
        $packageFile.Contains('..') -or
        [IO.Path]::GetFileName($packageFile) -ne $packageFile -or
        $packageFile -notmatch '\.msix$')
    {
        throw 'Protected pending state has an unsafe package file name.'
    }
    $expectedPackagePath = [IO.Path]::GetFullPath(
        (Join-Path (Join-Path $InstallRoot 'Identity') $packageFile))
    $actualPackagePath = [IO.Path]::GetFullPath([string]$State.packagePath)
    $packagePathMatches = $actualPackagePath -eq $expectedPackagePath
    $stagedRoot = if ([string]::IsNullOrWhiteSpace($PayloadDirectory))
    {
        [IO.Path]::GetFullPath((Join-Path $InstallRoot '.staging\current'))
    }
    else
    {
        $PayloadDirectory
    }
    if (-not [string]::IsNullOrWhiteSpace($stagedRoot))
    {
        $stagedPackagePath = [IO.Path]::GetFullPath(
            (Join-Path (Join-Path $stagedRoot 'Identity') $packageFile))
        $packagePathMatches = $packagePathMatches -or
            $actualPackagePath -eq $stagedPackagePath
    }
    if (-not $packagePathMatches)
    {
        throw 'Protected pending state points to a different package file.'
    }
    foreach ($fullName in @($State.preexistingPackageFullNames))
    {
        if ([string]$fullName -notmatch '^CialloKing\.BaClickFxDesktop_[A-Za-z0-9._-]+$')
        {
            throw 'Protected pending state contains an invalid previous package name.'
        }
    }
}

function Remove-NewPackages
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State
    )

    $preexisting = @($State.preexistingPackageFullNames)
    if ($null -ne $State.oldInstallState)
    {
        $oldFullName = [string]$State.oldInstallState.packageFullName
        $sameIdentityReplacement = Get-AppxPackage `
            -Name ([string]$State.packageName) `
            -ErrorAction Stop |
            Where-Object { [string]$_.PackageFullName -eq $oldFullName } |
            Select-Object -First 1
        if ($null -ne $sameIdentityReplacement)
        {
            # A same-version repair keeps PackageFullName unchanged. Remove the
            # candidate registration before restoring the immutable old file.
            Remove-AppxPackage `
                -Package $oldFullName `
                -ErrorAction Stop
        }
    }
    $newPackages = @(
        Get-AppxPackage -Name ([string]$State.packageName) -ErrorAction Stop |
            Where-Object { $preexisting -notcontains [string]$_.PackageFullName }
    )
    foreach ($package in $newPackages)
    {
        Remove-AppxPackage -Package $package.PackageFullName -ErrorAction Stop
    }
}

function Restore-PreviousPackage
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot
    )

    if ($null -eq $State.oldInstallState)
    {
        return
    }
    $oldState = $State.oldInstallState
    $oldPackagePath = Assert-PreviousPackageMaterial `
        -State $oldState `
        -InstallRoot $InstallRoot
    $existing = @(
        Get-AppxPackage -Name ([string]$State.packageName) -ErrorAction Stop |
            Where-Object { [string]$_.PackageFullName -eq [string]$oldState.packageFullName }
    )
    if ($existing.Count -gt 0)
    {
        return
    }
    Add-AppxPackage `
        -Path $oldPackagePath `
        -ExternalLocation $InstallRoot `
        -ForceApplicationShutdown `
        -ForceUpdateFromAnyVersion
    $restored = @(
        Get-AppxPackage -Name ([string]$State.packageName) -ErrorAction Stop |
            Where-Object { [string]$_.PackageFullName -eq [string]$oldState.packageFullName }
    )
    if ($restored.Count -ne 1)
    {
        throw 'The previous package registration was not restored.'
    }
}

function Assert-PreviousPackageMaterial
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot
    )

    foreach ($propertyName in @(
            'packageFullName', 'packageFile', 'packageSha256',
            'hostFile', 'hostSha256'))
    {
        if ($null -eq $State.PSObject.Properties[$propertyName])
        {
            throw "Previous protected install state is missing: $propertyName"
        }
    }
    if ([string]$State.packageFullName -notmatch
            '^CialloKing\.BaClickFxDesktop_[A-Za-z0-9._-]+$' -or
        [string]$State.hostFile -ne 'ba-click-fx-desktop.exe' -or
        [string]$State.packageSha256 -notmatch '^[0-9A-Fa-f]{64}$' -or
        [string]$State.hostSha256 -notmatch '^[0-9A-Fa-f]{64}$')
    {
        throw 'Previous protected install state has invalid payload identity data.'
    }
    if ($null -ne $State.PSObject.Properties['externalLocation'] -and
        [IO.Path]::GetFullPath([string]$State.externalLocation) -ne
            [IO.Path]::GetFullPath($InstallRoot))
    {
        throw 'Previous protected install state points outside the install directory.'
    }

    $hostPath = Join-Path $InstallRoot 'ba-click-fx-desktop.exe'
    if (-not (Test-Path -LiteralPath $hostPath -PathType Leaf))
    {
        throw 'The restored previous Host is unavailable for rollback.'
    }
    $hostItem = Get-Item -LiteralPath $hostPath -Force
    if (($hostItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        (Get-InstallerFileHash -Path $hostPath) -ine
            [string]$State.hostSha256)
    {
        throw 'The restored previous Host does not match its protected install state.'
    }

    $packageFile = [string]$State.packageFile
    if ([IO.Path]::IsPathRooted($packageFile) -or
        $packageFile.Contains('..') -or
        [IO.Path]::GetFileName($packageFile) -ne $packageFile -or
        $packageFile -notmatch '\.msix$')
    {
        throw 'Previous protected install state has an unsafe package file name.'
    }
    $packagePath = Join-Path (Join-Path $InstallRoot 'Identity') $packageFile
    if (-not (Test-Path -LiteralPath $packagePath -PathType Leaf))
    {
        throw 'The previous package file is unavailable for rollback.'
    }
    $packageItem = Get-Item -LiteralPath $packagePath -Force
    if (($packageItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        (Get-InstallerFileHash -Path $packagePath) -ine
            [string]$State.packageSha256)
    {
        throw 'The restored previous package does not match its protected install state.'
    }
    return $packagePath
}

function Remove-PreviousPackageForReplacement
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State
    )

    if ($null -eq $State.oldInstallState)
    {
        return
    }
    $oldFullName = [string]$State.oldInstallState.packageFullName
    $oldPackage = Get-AppxPackage `
        -User ([string]$State.userSid) `
        -Name ([string]$State.packageName) `
        -ErrorAction Stop |
        Where-Object { [string]$_.PackageFullName -eq $oldFullName } |
        Select-Object -First 1
    if ($null -ne $oldPackage)
    {
        # AppX keeps the same PackageFullName for a same-version repair. Remove
        # the old registration explicitly so Add-AppxPackage cannot treat the
        # replacement as an already installed package.
        Remove-AppxPackage `
            -Package $oldFullName `
            -User ([string]$State.userSid) `
            -ErrorAction Stop
    }
}

trap
{
    Stop-RegistrationWithFailure `
        -ErrorRecord $_ `
        -Step $script:InstallerStep `
        -ExitCode $script:InstallerExitCode
}

$script:InstallerStep = 'validate-powershell'
if ($PSVersionTable.PSEdition -ne 'Desktop')
{
    throw 'Package registration requires Windows PowerShell 5.1.'
}

$script:InstallerStep = 'resolve-installer-paths'
$installRoot = [IO.Path]::GetFullPath($InstallDirectory)
$machineStateFullPath = [IO.Path]::GetFullPath($MachineStatePath)
$resultFullPath = [IO.Path]::GetFullPath($ResultPath)
$payloadDirectoryFullPath = ''
if (-not [string]::IsNullOrWhiteSpace($PayloadDirectory))
{
    $payloadDirectoryFullPath = [IO.Path]::GetFullPath($PayloadDirectory)
    $expectedPayloadDirectory = [IO.Path]::GetFullPath(
        (Join-Path $installRoot '.staging\current'))
    if ($payloadDirectoryFullPath -ne $expectedPayloadDirectory)
    {
        throw 'Installer payload directory is outside protected staging.'
    }
}
$script:InstallerDiagnosticPath = "$resultFullPath.diagnostic.txt"
$expectedStatePath = [IO.Path]::GetFullPath(
    (Join-Path $installRoot 'Installer\PREPARE-STATE.json'))
if ($machineStateFullPath -ne $expectedStatePath)
{
    throw 'Protected pending state must remain in the Installer directory.'
}
$script:InstallerStep = 'validate-protected-pending-state'
Assert-ProtectedStateAcl -Path $machineStateFullPath
$machineState = Get-Content -LiteralPath $machineStateFullPath -Raw | ConvertFrom-Json
Assert-PendingState `
    -State $machineState `
    -InstallRoot $installRoot `
    -PayloadDirectory $payloadDirectoryFullPath
$script:InstallerState = $machineState
$script:InstallerProductVersion = [string]$machineState.productVersion
$script:InstallerPackageVersion = [string]$machineState.packageVersion

$script:InstallerStep = 'validate-original-user'
$currentIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
if ($null -eq $currentIdentity.User -or
    $currentIdentity.User.Value -ne [string]$machineState.userSid)
{
    throw 'Package registration is not running as the original user.'
}

if (-not [string]::IsNullOrWhiteSpace($effectiveRollbackAction))
{
    $script:InstallerStep = 'validate-rollback-commit-state'
    $mayRollbackPackage = Assert-PendingRollbackMayRemoveNewPackage `
        -State $machineState `
        -InstallRoot $installRoot
    if ($effectiveRollbackAction -in @('RemoveNew', 'Both'))
    {
        if ($mayRollbackPackage)
        {
            $script:InstallerStep = 'remove-new-package-registrations'
            Remove-NewPackages -State $machineState
        }
    }
    if ($effectiveRollbackAction -in @('RestorePrevious', 'Both'))
    {
        if ($mayRollbackPackage)
        {
            $script:InstallerStep = 'restore-previous-package-registration'
            Restore-PreviousPackage -State $machineState -InstallRoot $installRoot
        }
    }
    exit 0
}

$packageName = [string]$machineState.packageName
$packagePath = [string]$machineState.packagePath
$registeredPackage = $null
try
{
    $script:InstallerStep = 'inspect-current-package-registrations'
    $currentFullNames = @(
        Get-AppxPackage -Name $packageName -ErrorAction Stop |
            ForEach-Object { [string]$_.PackageFullName }
    )
    $unexpectedCurrent = @(
        $currentFullNames |
            Where-Object { @($machineState.preexistingPackageFullNames) -notcontains $_ }
    )
    if ($unexpectedCurrent.Count -gt 0)
    {
        throw 'Package registrations changed after the protected prepare phase.'
    }

    $script:InstallerStep = 'remove-previous-package-registration'
    Remove-PreviousPackageForReplacement -State $machineState

    $script:InstallerStep = 'register-identity-package'
    Add-AppxPackage `
        -Path $packagePath `
        -ExternalLocation $installRoot `
        -ForceApplicationShutdown `
        -ForceUpdateFromAnyVersion

    $script:InstallerStep = 'verify-package-registration'
    $registered = @(
        Get-AppxPackage -Name $packageName -ErrorAction Stop |
            Where-Object { [string]$_.Version -eq [string]$machineState.packageVersion }
    )
    if ($registered.Count -ne 1)
    {
        throw 'Add-AppxPackage did not produce exactly one expected registration.'
    }
    $registeredPackage = $registered[0]
    $script:InstallerStep = 'write-registration-result'
    Write-Result `
        -Succeeded $true `
        -Package $registeredPackage `
        -State $machineState
}
catch
{
    $registrationErrorRecord = $_
    $registrationFailureStep = $script:InstallerStep
    $registrationError = $_.Exception.Message
    try
    {
        $script:InstallerStep = 'rollback-new-package-registrations'
        Remove-NewPackages -State $machineState
    }
    catch
    {
        $registrationError =
            "$registrationError Rollback failed: $($_.Exception.Message)"
        Add-InstallerRelatedFailure `
            -ErrorRecord $_ `
            -Step $script:InstallerStep
    }
    try
    {
        $script:InstallerStep = 'write-registration-failure-result'
        Write-Result `
            -Succeeded $false `
            -Package $null `
            -State $machineState `
            -ErrorMessage $registrationError
    }
    catch
    {
        $registrationError =
            "$registrationError Result write failed: $($_.Exception.Message)"
        Add-InstallerRelatedFailure `
            -ErrorRecord $_ `
            -Step 'write-registration-failure-result'
    }
    $script:InstallerStep = $registrationFailureStep
    Stop-RegistrationWithFailure `
        -ErrorRecord $registrationErrorRecord `
        -Step $registrationFailureStep `
        -ResultErrorMessage $registrationError
}
