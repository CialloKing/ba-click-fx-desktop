[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InstallDirectory,

    [Parameter(Mandatory = $true)]
    [string]$MachineStatePath,

    [Parameter(Mandatory = $true)]
    [string]$ResultPath,

    [switch]$Rollback
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installer-diagnostics.ps1')
$script:InstallerStep = 'initialize'
$script:InstallerPhase = if ($Rollback)
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

        [string]$ResultErrorMessage = ''
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
    exit 1
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
        $sid = $rule.IdentityReference.Translate(
            [Security.Principal.SecurityIdentifier]).Value
        if ($sid -in @('S-1-5-18', 'S-1-5-32-544'))
        {
            continue
        }
        if (([int]$rule.FileSystemRights -band $writeRights) -ne 0)
        {
            throw 'Protected pending state grants write access to a non-administrator.'
        }
    }
}

function Assert-PendingState
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot
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
        'templateSha256',
        'packagePath',
        'packageFile',
        'ownedCertificateThumbprints',
        'ownedPackageFiles',
        'preexistingPackageFullNames',
        'oldInstallState'))
    {
        if ($null -eq $State.PSObject.Properties[$propertyName])
        {
            throw "Protected pending state is missing: $propertyName"
        }
    }
    if ([int]$State.schema -ne 1 -or [string]$State.stateKind -ne 'prepare')
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
    if ([string]$State.templateSha256 -notmatch '^[0-9A-Fa-f]{64}$')
    {
        throw 'Protected pending state has an invalid template hash.'
    }
    foreach ($thumbprint in (([string]$State.ownedCertificateThumbprints) -split ',' |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }))
    {
        if ($thumbprint -notmatch '^[0-9A-Fa-f]{40}$')
        {
            throw 'Protected pending state has an invalid certificate ledger entry.'
        }
    }
    foreach ($ownedFile in (([string]$State.ownedPackageFiles) -split '\|' |
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
    if ([IO.Path]::GetFullPath([string]$State.packagePath) -ne $expectedPackagePath)
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
    $existing = @(
        Get-AppxPackage -Name ([string]$State.packageName) -ErrorAction Stop |
            Where-Object { [string]$_.PackageFullName -eq [string]$oldState.packageFullName }
    )
    if ($existing.Count -gt 0)
    {
        return
    }
    $oldPackageFile = [string]$oldState.packageFile
    if ([IO.Path]::IsPathRooted($oldPackageFile) -or
        $oldPackageFile.Contains('..') -or
        [IO.Path]::GetFileName($oldPackageFile) -ne $oldPackageFile -or
        $oldPackageFile -notmatch '\.msix$')
    {
        throw 'Previous protected install state has an unsafe package file name.'
    }
    $oldPackagePath = Join-Path (Join-Path $InstallRoot 'Identity') $oldPackageFile
    if (-not (Test-Path -LiteralPath $oldPackagePath -PathType Leaf))
    {
        throw 'The previous package file is unavailable for rollback.'
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
        -Step $script:InstallerStep
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
Assert-PendingState -State $machineState -InstallRoot $installRoot
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

if ($Rollback)
{
    $script:InstallerStep = 'remove-new-package-registrations'
    Remove-NewPackages -State $machineState
    $script:InstallerStep = 'restore-previous-package-registration'
    Restore-PreviousPackage -State $machineState -InstallRoot $installRoot
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
        $script:InstallerStep = 'rollback-previous-package-registration'
        Restore-PreviousPackage -State $machineState -InstallRoot $installRoot
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
