[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InstallDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installer-diagnostics.ps1')
. (Join-Path $PSScriptRoot 'protected-paths.ps1')
$script:InstallerStep = 'initialize'
$script:InstallerProductVersion = ''
$script:InstallerPackageVersion = ''

function Assert-Administrator
{
    $principal = New-Object Security.Principal.WindowsPrincipal(
        [Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
    {
        throw 'Uninstall requires administrator privileges.'
    }
}

function Assert-ExpectedProcessIsStopped
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExecutablePath
    )

    $expected = [IO.Path]::GetFullPath($ExecutablePath)
    $running = Get-CimInstance Win32_Process -Filter "Name='$([IO.Path]::GetFileName($expected))'" |
        Where-Object {
            (-not [string]::IsNullOrWhiteSpace($_.ExecutablePath)) -and
                ([IO.Path]::GetFullPath($_.ExecutablePath) -eq $expected)
        } |
        Select-Object -First 1
    if ($null -ne $running)
    {
        throw "Close the running application before uninstalling: $expected"
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

    $resolvedPath = [IO.Path]::GetFullPath($Path)
    Assert-NoReparsePath -Path $resolvedPath -AllowMissing
    $encoding = New-Object -TypeName System.Text.UTF8Encoding -ArgumentList $false
    [IO.File]::WriteAllText($resolvedPath, $Content, $encoding)
    Assert-NoReparsePath -Path $resolvedPath
}

function Write-FlushedUtf8NoBom
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Content
    )

    $resolvedPath = [IO.Path]::GetFullPath($Path)
    $parentPath = [IO.Path]::GetDirectoryName($resolvedPath)
    if (-not [string]::IsNullOrWhiteSpace($parentPath))
    {
        Assert-NoReparsePath -Path $parentPath -AllowMissing
    }
    Assert-NoReparsePath -Path $resolvedPath -AllowMissing
    $bytes = [Text.Encoding]::UTF8.GetBytes($Content)
    $stream = [IO.FileStream]::new(
        $resolvedPath,
        [IO.FileMode]::Create,
        [IO.FileAccess]::Write,
        [IO.FileShare]::None)
    try
    {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
        Assert-NoReparsePath -Path $resolvedPath
    }
    finally
    {
        $stream.Dispose()
    }
}

function Assert-ProtectedStateAcl
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    Assert-NoReparsePath -Path $Path
    $acl = Get-Acl -LiteralPath $Path
    if (-not $acl.AreAccessRulesProtected)
    {
        throw 'Protected install state still inherits writable access rules.'
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
            throw 'Protected install state grants write access to a non-administrator.'
        }
    }
}

function Test-RegistryHiveMounted
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$HiveName
    )

    if ([string]::IsNullOrWhiteSpace($HiveName) -or $HiveName.Contains('\'))
    {
        throw 'Registry hive name is unsafe.'
    }

    $usersRoot = $null
    $hiveKey = $null
    try
    {
        $usersRoot = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
            [Microsoft.Win32.RegistryHive]::Users,
            [Microsoft.Win32.RegistryView]::Default)
        $hiveKey = $usersRoot.OpenSubKey($HiveName, $false)
        return $null -ne $hiveKey
    }
    finally
    {
        if ($null -ne $hiveKey)
        {
            $hiveKey.Dispose()
        }
        if ($null -ne $usersRoot)
        {
            $usersRoot.Dispose()
        }
    }
}

function Get-InstalledUserProfileHivePath
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstalledUserSid
    )

    $localMachine = $null
    $profileKey = $null
    try
    {
        # ProfileList is machine-protected and avoids trusting a path supplied
        # by the invoking user or by the uninstall command line.
        $localMachine = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
            [Microsoft.Win32.RegistryHive]::LocalMachine,
            [Microsoft.Win32.RegistryView]::Registry64)
        $profileListPath =
            "SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\$InstalledUserSid"
        $profileKey = $localMachine.OpenSubKey($profileListPath, $false)
        if ($null -eq $profileKey)
        {
            throw 'Installed user profile is missing from the protected ProfileList.'
        }

        $profileImagePath = $profileKey.GetValue(
            'ProfileImagePath',
            $null,
            [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        if ($null -eq $profileImagePath -or
            [string]::IsNullOrWhiteSpace([string]$profileImagePath))
        {
            throw 'Installed user ProfileList entry has no profile path.'
        }
        $profileValueKind = $profileKey.GetValueKind('ProfileImagePath')
        if ($profileValueKind -notin @(
                [Microsoft.Win32.RegistryValueKind]::String,
                [Microsoft.Win32.RegistryValueKind]::ExpandString))
        {
            throw 'Installed user ProfileList path has an unsupported registry type.'
        }

        $profilePathText = [string]$profileImagePath
        $systemDriveToken = '%SystemDrive%'
        if ($profilePathText.StartsWith(
                $systemDriveToken,
                [StringComparison]::OrdinalIgnoreCase))
        {
            # The elevated uninstaller inherits a caller-controlled environment.
            # Derive SystemDrive from Windows itself instead of expanding it.
            $windowsDirectory = [Environment]::GetFolderPath(
                [Environment+SpecialFolder]::Windows)
            $systemDrive = [IO.Path]::GetPathRoot($windowsDirectory).TrimEnd('\')
            $expandedProfilePath =
                $systemDrive + $profilePathText.Substring($systemDriveToken.Length)
        }
        elseif ($profilePathText.StartsWith(
                '%',
                [StringComparison]::Ordinal))
        {
            throw 'Installed user ProfileList path uses an unsupported environment token.'
        }
        else
        {
            $expandedProfilePath = $profilePathText
        }
        if (-not [IO.Path]::IsPathRooted($expandedProfilePath))
        {
            throw 'Installed user ProfileList path is not absolute.'
        }
        $profilePath = [IO.Path]::GetFullPath($expandedProfilePath)
        $hivePath = [IO.Path]::GetFullPath((Join-Path $profilePath 'NTUSER.DAT'))
        if (-not (Test-Path -LiteralPath $hivePath -PathType Leaf))
        {
            throw "Installed user registry hive is missing: $hivePath"
        }
        return $hivePath
    }
    finally
    {
        if ($null -ne $profileKey)
        {
            $profileKey.Dispose()
        }
        if ($null -ne $localMachine)
        {
            $localMachine.Dispose()
        }
    }
}

function Invoke-BoundedRegistryHiveCommand
{
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('Load', 'Unload')]
        [string]$Operation,

        [Parameter(Mandatory = $true)]
        [string]$HiveName,

        [string]$HiveFilePath = '',

        [ValidateRange(1000, 60000)]
        [int]$TimeoutMilliseconds = 10000
    )

    if ($HiveName -notmatch '^BAFX_Uninstall_[0-9]+_[0-9a-f]{32}$')
    {
        throw 'Temporary registry hive name is unsafe.'
    }
    if ($Operation -eq 'Load' -and
        ([string]::IsNullOrWhiteSpace($HiveFilePath) -or $HiveFilePath.Contains('"')))
    {
        throw 'Temporary registry hive file path is unsafe.'
    }

    $systemDirectory = [Environment]::GetFolderPath(
        [Environment+SpecialFolder]::System)
    $registryUtility = Join-Path $systemDirectory 'reg.exe'
    if (-not (Test-Path -LiteralPath $registryUtility -PathType Leaf))
    {
        throw "Windows registry utility is missing: $registryUtility"
    }

    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $registryUtility
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $target = "HKU\$HiveName"
    if ($Operation -eq 'Load')
    {
        $startInfo.Arguments = "load $target `"$HiveFilePath`""
    }
    else
    {
        $startInfo.Arguments = "unload $target"
    }

    $process = $null
    try
    {
        $process = [Diagnostics.Process]::Start($startInfo)
        if ($null -eq $process)
        {
            throw "Registry hive $Operation process did not start."
        }
        if (-not $process.WaitForExit($TimeoutMilliseconds))
        {
            $process.Kill()
            if (-not $process.WaitForExit(1000))
            {
                throw "Registry hive $Operation process could not be stopped after timeout."
            }
            throw "Registry hive $Operation exceeded the $TimeoutMilliseconds ms timeout."
        }
        if ($process.ExitCode -ne 0)
        {
            throw "Registry hive $Operation failed with exit code $($process.ExitCode)."
        }
    }
    finally
    {
        if ($null -ne $process)
        {
            try
            {
                if (-not $process.HasExited)
                {
                    $process.Kill()
                    if (-not $process.WaitForExit(1000))
                    {
                        throw "Registry hive $Operation process remained after cancellation."
                    }
                }
            }
            finally
            {
                $process.Dispose()
            }
        }
    }
}

function Remove-StartupValueFromLoadedHive
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$HiveName
    )

    if ([string]::IsNullOrWhiteSpace($HiveName) -or $HiveName.Contains('\'))
    {
        throw 'Registry hive name is unsafe.'
    }

    $usersRoot = $null
    $hiveKey = $null
    $runKey = $null
    try
    {
        $usersRoot = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
            [Microsoft.Win32.RegistryHive]::Users,
            [Microsoft.Win32.RegistryView]::Default)
        $hiveKey = $usersRoot.OpenSubKey($HiveName, $false)
        if ($null -eq $hiveKey)
        {
            throw "Registry hive is not mounted: HKEY_USERS\$HiveName"
        }
        $runKey = $hiveKey.OpenSubKey(
            'Software\Microsoft\Windows\CurrentVersion\Run',
            $true)
        if ($null -eq $runKey)
        {
            return
        }

        # DeleteValue with throwOnMissingValue=false keeps repeated uninstall
        # attempts idempotent and never removes neighboring startup entries.
        $runKey.DeleteValue('BAFX Control Center', $false)
        if ('BAFX Control Center' -in @($runKey.GetValueNames()))
        {
            throw 'BAFX Control Center startup registration remains after deletion.'
        }
    }
    finally
    {
        if ($null -ne $runKey)
        {
            $runKey.Dispose()
        }
        if ($null -ne $hiveKey)
        {
            $hiveKey.Dispose()
        }
        if ($null -ne $usersRoot)
        {
            $usersRoot.Dispose()
        }
    }
}

function Remove-InstalledUserStartupRegistration
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstalledUserSid
    )

    $sid = [Security.Principal.SecurityIdentifier]::new($InstalledUserSid)
    if ($sid.Value -ne $InstalledUserSid)
    {
        throw 'Protected install state has a non-canonical installed user SID.'
    }

    if (Test-RegistryHiveMounted -HiveName $sid.Value)
    {
        Remove-StartupValueFromLoadedHive -HiveName $sid.Value
        return
    }

    $script:InstallerStep = 'resolve-installed-user-profile-hive'
    $hivePath = Get-InstalledUserProfileHivePath -InstalledUserSid $sid.Value
    $temporaryHiveName =
        "BAFX_Uninstall_${PID}_$([Guid]::NewGuid().ToString('N'))"
    if (Test-RegistryHiveMounted -HiveName $temporaryHiveName)
    {
        throw 'Generated temporary registry hive name is already mounted.'
    }
    $loadAttempted = $false
    $failureStep = ''
    try
    {
        $script:InstallerStep = 'load-installed-user-registry-hive'
        $loadAttempted = $true
        Invoke-BoundedRegistryHiveCommand `
            -Operation Load `
            -HiveName $temporaryHiveName `
            -HiveFilePath $hivePath

        $script:InstallerStep = 'remove-installed-user-startup-registration'
        Remove-StartupValueFromLoadedHive -HiveName $temporaryHiveName
    }
    catch
    {
        $failureStep = $script:InstallerStep
        throw
    }
    finally
    {
        if ($loadAttempted)
        {
            $script:InstallerStep = 'unload-installed-user-registry-hive'
            if (Test-RegistryHiveMounted -HiveName $temporaryHiveName)
            {
                Invoke-BoundedRegistryHiveCommand `
                    -Operation Unload `
                    -HiveName $temporaryHiveName
                if (Test-RegistryHiveMounted -HiveName $temporaryHiveName)
                {
                    throw 'Temporary installed-user registry hive remains after unload.'
                }
            }
            if (-not [string]::IsNullOrWhiteSpace($failureStep))
            {
                $script:InstallerStep = $failureStep
            }
        }
    }
}

function Split-Ledger
{
    param(
        [AllowNull()]
        [object]$Value,

        [Parameter(Mandatory = $true)]
        [ValidateSet('Comma', 'Pipe')]
        [string]$Separator
    )

    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace([string]$Value))
    {
        return @()
    }
    $pattern = if ($Separator -eq 'Comma') { ',' } else { '\|' }
    return @(
        ([string]$Value -split $pattern) |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            ForEach-Object { $_.Trim() }
    )
}

function Get-StatePropertiesWithoutDigest
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$Value
    )

    $ordered = [ordered]@{}
    foreach ($property in $Value.PSObject.Properties)
    {
        if ($property.Name -ne 'stateDigest')
        {
            $ordered[$property.Name] = $property.Value
        }
    }
    return $ordered
}

function Get-StateDigest
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$Value
    )

    $json = Get-StatePropertiesWithoutDigest -Value $Value |
        ConvertTo-Json -Depth 12
    $hasher = [Security.Cryptography.SHA256]::Create()
    try
    {
        return ([BitConverter]::ToString($hasher.ComputeHash(
            [Text.Encoding]::UTF8.GetBytes($json)))).Replace('-', '')
    }
    finally
    {
        $hasher.Dispose()
    }
}

function Assert-InstallStateRawPair
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$PrimaryPath,

        [Parameter(Mandatory = $true)]
        [string]$BackupPath
    )

    $primaryBytes = [IO.File]::ReadAllBytes($PrimaryPath)
    $backupBytes = [IO.File]::ReadAllBytes($BackupPath)
    if ($primaryBytes.Length -ne $backupBytes.Length)
    {
        throw 'Protected install state primary and backup bytes differ.'
    }
    for ($index = 0; $index -lt $primaryBytes.Length; ++$index)
    {
        if ($primaryBytes[$index] -ne $backupBytes[$index])
        {
            throw 'Protected install state primary and backup bytes differ.'
        }
    }
}

function Assert-InstallStatePair
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$Primary,

        [Parameter(Mandatory = $true)]
        [object]$Backup,

        [string]$PrimaryPath = '',

        [string]$BackupPath = ''
    )

    if ([string]::IsNullOrWhiteSpace($PrimaryPath) -xor
        [string]::IsNullOrWhiteSpace($BackupPath))
    {
        throw 'Protected install state raw pair paths must be supplied together.'
    }
    if (-not [string]::IsNullOrWhiteSpace($PrimaryPath))
    {
        Assert-InstallStateRawPair `
            -PrimaryPath $PrimaryPath `
            -BackupPath $BackupPath
    }

    $primaryTransaction = [string]$Primary.transactionId
    $backupTransaction = [string]$Backup.transactionId
    if ($primaryTransaction -notmatch '^[0-9a-fA-F]{32}$' -or
        $primaryTransaction -ne $backupTransaction)
    {
        throw 'Protected install state primary and backup transactions differ.'
    }
    $primaryDigest = [string]$Primary.stateDigest
    $backupDigest = [string]$Backup.stateDigest
    if ([string]::IsNullOrWhiteSpace($primaryDigest) -and
        [string]::IsNullOrWhiteSpace($backupDigest))
    {
        if ((Get-StatePropertiesWithoutDigest -Value $Primary |
                ConvertTo-Json -Depth 12) -ne
            (Get-StatePropertiesWithoutDigest -Value $Backup |
                ConvertTo-Json -Depth 12))
        {
            throw 'Protected install state primary and backup contents differ.'
        }
        return
    }
    if ($primaryDigest -notmatch '^[0-9A-Fa-f]{64}$' -or
        $primaryDigest -ne $backupDigest -or
        (Get-StateDigest -Value $Primary) -ne $primaryDigest -or
        (Get-StateDigest -Value $Backup) -ne $backupDigest)
    {
        throw 'Protected install state primary and backup digests differ.'
    }
}

function Get-UninstallPhaseRank
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Phase
    )

    switch ($Phase)
    {
        'started' { return 0 }
        'user-package-removing' { return 1 }
        'identity-removing' { return 2 }
        'certificate-removing' { return 3 }
        'payload-removing' { return 4 }
        'state-removing' { return 5 }
        default { throw "Unsupported uninstall journal phase: $Phase" }
    }
}

function Read-UninstallJournal
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [AllowNull()]
        [object]$State = $null
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf))
    {
        return $null
    }
    Assert-NoReparsePath -Path $Path
    Assert-ProtectedStateAcl -Path $Path
    $journal = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    foreach ($propertyName in @(
            'schema', 'transactionId', 'stateDigest', 'phase'))
    {
        if ($null -eq $journal.PSObject.Properties[$propertyName])
        {
            throw "Uninstall journal is missing: $propertyName"
        }
    }
    if ([int]$journal.schema -ne 1)
    {
        throw 'Uninstall journal has an unsupported schema.'
    }
    if ([string]$journal.transactionId -notmatch '^[0-9a-fA-F]{32}$')
    {
        throw 'Uninstall journal has an invalid transaction identifier.'
    }
    if (-not [string]::IsNullOrWhiteSpace([string]$journal.stateDigest) -and
        [string]$journal.stateDigest -notmatch '^[0-9A-Fa-f]{64}$')
    {
        throw 'Uninstall journal has an invalid state digest.'
    }
    if ($null -ne $State -and
        ([string]$journal.transactionId -ne [string]$State.transactionId -or
            [string]$journal.stateDigest -ne [string]$State.stateDigest))
    {
        throw 'Uninstall journal does not match the protected install state.'
    }
    [void](Get-UninstallPhaseRank -Phase ([string]$journal.phase))
    return $journal
}

function Set-FileAclFromTemplate
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$TemplatePath
    )

    Assert-NoReparsePath -Path $TemplatePath
    Assert-NoReparsePath -Path $Path
    $templateAcl = Get-Acl -LiteralPath $TemplatePath
    $targetAcl = Get-Acl -LiteralPath $Path
    $targetAcl.SetSecurityDescriptorSddlForm($templateAcl.Sddl)
    Set-Acl -LiteralPath $Path -AclObject $targetAcl
    Assert-NoReparsePath -Path $Path
}

function Write-UninstallJournal
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$StatePath,

        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [ValidateSet(
            'started',
            'user-package-removing',
            'identity-removing',
            'certificate-removing',
            'payload-removing',
            'state-removing')]
        [string]$Phase
    )

    $resolvedPath = [IO.Path]::GetFullPath($Path)
    $resolvedStatePath = [IO.Path]::GetFullPath($StatePath)
    Assert-NoReparsePath -Path $resolvedPath -AllowMissing
    Assert-NoReparsePath -Path $resolvedStatePath
    $journal = [ordered]@{
        schema = 1
        transactionId = [string]$State.transactionId
        stateDigest = [string]$State.stateDigest
        phase = $Phase
        updatedUtc = [DateTime]::UtcNow.ToString('o')
    }
    $existingJournal = Read-UninstallJournal -Path $resolvedPath
    if ($null -ne $existingJournal -and
        (Get-UninstallPhaseRank -Phase ([string]$existingJournal.phase)) -gt
            (Get-UninstallPhaseRank -Phase $Phase))
    {
        # Recovery progress is monotonic. A retry must never downgrade the
        # journal before re-entering an earlier idempotent cleanup step.
        return
    }
    if ($Phase -eq 'state-removing')
    {
        $backupStatePath = "$resolvedStatePath.bak"
        Assert-NoReparsePath -Path $backupStatePath
        $journal.primaryStateBase64 = [Convert]::ToBase64String(
            [IO.File]::ReadAllBytes($resolvedStatePath))
        $journal.backupStateBase64 = [Convert]::ToBase64String(
            [IO.File]::ReadAllBytes($backupStatePath))
        $journal.primaryStateAcl = (Get-Acl -LiteralPath $resolvedStatePath).Sddl
        $journal.backupStateAcl = (Get-Acl -LiteralPath $backupStatePath).Sddl
    }
    $temporaryPath = "$resolvedPath.$PID.$([Guid]::NewGuid().ToString('N')).tmp"
    try
    {
        Write-FlushedUtf8NoBom `
            -Path $temporaryPath `
            -Content ($journal | ConvertTo-Json -Depth 4)
        Set-FileAclFromTemplate `
            -Path $temporaryPath `
            -TemplatePath $resolvedStatePath
        Assert-ProtectedStateAcl -Path $temporaryPath
        Assert-NoReparsePath -Path $temporaryPath
        if (Test-Path -LiteralPath $resolvedPath -PathType Leaf)
        {
            [IO.File]::Replace($temporaryPath, $resolvedPath, $null, $true)
        }
        else
        {
            [IO.File]::Move($temporaryPath, $resolvedPath)
        }
        Assert-NoReparsePath -Path $resolvedPath
        Assert-ProtectedStateAcl -Path $resolvedPath
        $written = Get-Content -LiteralPath $resolvedPath -Raw | ConvertFrom-Json
        if ([string]$written.transactionId -ne [string]$State.transactionId -or
            [string]$written.stateDigest -ne [string]$State.stateDigest -or
            [string]$written.phase -ne $Phase)
        {
            throw 'The uninstall journal did not verify after replacement.'
        }
    }
    finally
    {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf)
        {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
}

function Test-FileBytesEqual
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$LeftPath,

        [Parameter(Mandatory = $true)]
        [string]$RightPath
    )

    if (-not (Test-Path -LiteralPath $LeftPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $RightPath -PathType Leaf))
    {
        return $false
    }
    $leftBytes = [IO.File]::ReadAllBytes($LeftPath)
    $rightBytes = [IO.File]::ReadAllBytes($RightPath)
    if ($leftBytes.Length -ne $rightBytes.Length)
    {
        return $false
    }
    for ($index = 0; $index -lt $leftBytes.Length; ++$index)
    {
        if ($leftBytes[$index] -ne $rightBytes[$index])
        {
            return $false
        }
    }
    return $true
}

function Read-UninstallCompletionMarker
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$JournalPath
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf))
    {
        return $null
    }
    Assert-ProtectedStateAcl -Path $Path
    $marker = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    foreach ($propertyName in @(
            'schema', 'transactionId', 'stateDigest', 'stateRemoved',
            'completedUtc'))
    {
        if ($null -eq $marker.PSObject.Properties[$propertyName])
        {
            throw "Uninstall completion marker is missing: $propertyName"
        }
    }
    $markerDigest = [string]$marker.stateDigest
    if ([int]$marker.schema -ne 1 -or
        [string]$marker.transactionId -notmatch '^[0-9a-fA-F]{32}$' -or
        ($markerDigest -notmatch '^[0-9A-Fa-f]{64}$' -and
            -not [string]::IsNullOrWhiteSpace($markerDigest)) -or
        $marker.stateRemoved -isnot [bool] -or
        -not [bool]$marker.stateRemoved)
    {
        throw 'Uninstall completion marker has invalid state.'
    }
    try
    {
        [void][DateTime]::Parse(
            [string]$marker.completedUtc,
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::RoundtripKind)
    }
    catch
    {
        throw 'Uninstall completion marker has an invalid timestamp.'
    }
    $journal = Read-UninstallJournal -Path $JournalPath
    if ($null -eq $journal -or [string]$journal.phase -ne 'state-removing' -or
        [string]$journal.transactionId -ne [string]$marker.transactionId -or
        [string]$journal.stateDigest -ne $markerDigest)
    {
        throw 'Uninstall completion marker does not match its journal.'
    }
    return $marker
}

function Read-UninstallJournalSnapshotState
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$Journal
    )

    if ([string]$Journal.phase -ne 'state-removing' -or
        $null -eq $Journal.PSObject.Properties['primaryStateBase64'] -or
        $null -eq $Journal.PSObject.Properties['backupStateBase64'])
    {
        throw 'The uninstall journal has no protected install-state snapshot.'
    }
    $temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) `
        ('bafx-uninstall-snapshot-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temporaryRoot -Force | Out-Null
    $primaryPath = Join-Path $temporaryRoot 'INSTALL-STATE.json'
    $backupPath = Join-Path $temporaryRoot 'INSTALL-STATE.json.bak'
    try
    {
        [IO.File]::WriteAllBytes(
            $primaryPath,
            [Convert]::FromBase64String([string]$Journal.primaryStateBase64))
        [IO.File]::WriteAllBytes(
            $backupPath,
            [Convert]::FromBase64String([string]$Journal.backupStateBase64))
        $primary = Get-Content -LiteralPath $primaryPath -Raw | ConvertFrom-Json
        $backup = Get-Content -LiteralPath $backupPath -Raw | ConvertFrom-Json
        Assert-InstallStatePair `
            -Primary $primary `
            -Backup $backup `
            -PrimaryPath $primaryPath `
            -BackupPath $backupPath
        if ([string]$primary.transactionId -ne [string]$Journal.transactionId -or
            [string]$primary.stateDigest -ne [string]$Journal.stateDigest)
        {
            throw 'The uninstall journal snapshot does not match its transaction.'
        }
        return $primary
    }
    finally
    {
        if (Test-Path -LiteralPath $temporaryRoot -PathType Container)
        {
            Assert-NoReparseTree -Path $temporaryRoot
            Remove-Item -LiteralPath $temporaryRoot -Recurse -Force `
                -ErrorAction SilentlyContinue
        }
    }
}

function Write-UninstallCompletionMarker
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$JournalPath,

        [Parameter(Mandatory = $true)]
        [object]$State
    )

    $resolvedPath = [IO.Path]::GetFullPath($Path)
    $resolvedJournalPath = [IO.Path]::GetFullPath($JournalPath)
    Assert-NoReparsePath -Path $resolvedPath -AllowMissing
    Assert-NoReparsePath -Path $resolvedJournalPath
    $marker = [ordered]@{
        schema = 1
        transactionId = [string]$State.transactionId
        stateDigest = [string]$State.stateDigest
        stateRemoved = $true
        completedUtc = [DateTime]::UtcNow.ToString('o')
    }
    $temporaryPath = "$resolvedPath.$PID.$([Guid]::NewGuid().ToString('N')).tmp"
    try
    {
        Write-FlushedUtf8NoBom `
            -Path $temporaryPath `
            -Content ($marker | ConvertTo-Json -Depth 4)
        Set-FileAclFromTemplate `
            -Path $temporaryPath `
            -TemplatePath $resolvedJournalPath
        Assert-ProtectedStateAcl -Path $temporaryPath
        Assert-NoReparsePath -Path $temporaryPath
        if (Test-Path -LiteralPath $resolvedPath -PathType Leaf)
        {
            [IO.File]::Replace($temporaryPath, $resolvedPath, $null, $true)
        }
        else
        {
            [IO.File]::Move($temporaryPath, $resolvedPath)
        }
        Assert-NoReparsePath -Path $resolvedPath
        Assert-ProtectedStateAcl -Path $resolvedPath
        $written = Read-UninstallCompletionMarker `
            -Path $resolvedPath `
            -JournalPath $resolvedJournalPath
        if ([string]$written.transactionId -ne [string]$State.transactionId -or
            [string]$written.stateDigest -ne [string]$State.stateDigest)
        {
            throw 'The uninstall completion marker did not verify after replacement.'
        }
    }
    finally
    {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf)
        {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
}

function Restore-InstallStateFromUninstallJournal
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [object]$Journal
    )

    if ([string]$Journal.phase -ne 'state-removing' -or
        $null -eq $Journal.PSObject.Properties['primaryStateBase64'] -or
        $null -eq $Journal.PSObject.Properties['backupStateBase64'] -or
        $null -eq $Journal.PSObject.Properties['primaryStateAcl'] -or
        $null -eq $Journal.PSObject.Properties['backupStateAcl'])
    {
        throw 'The uninstall journal has no protected install-state recovery snapshot.'
    }
    $resolvedPath = [IO.Path]::GetFullPath($Path)
    $backupPath = "$resolvedPath.bak"
    Assert-NoReparsePath -Path $resolvedPath -AllowMissing
    Assert-NoReparsePath -Path $backupPath -AllowMissing
    $temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) `
        ('bafx-uninstall-restore-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temporaryRoot -Force | Out-Null
    Assert-NoReparsePath -Path $temporaryRoot
    $primaryTemporaryPath = Join-Path $temporaryRoot 'INSTALL-STATE.json'
    $backupTemporaryPath = Join-Path $temporaryRoot 'INSTALL-STATE.json.bak'
    try
    {
        Assert-NoReparsePath -Path $primaryTemporaryPath -AllowMissing
        Assert-NoReparsePath -Path $backupTemporaryPath -AllowMissing
        [IO.File]::WriteAllBytes(
            $primaryTemporaryPath,
            [Convert]::FromBase64String([string]$Journal.primaryStateBase64))
        [IO.File]::WriteAllBytes(
            $backupTemporaryPath,
            [Convert]::FromBase64String([string]$Journal.backupStateBase64))
        Assert-NoReparsePath -Path $primaryTemporaryPath
        Assert-NoReparsePath -Path $backupTemporaryPath
        $snapshotPrimary = Get-Content -LiteralPath $primaryTemporaryPath -Raw |
            ConvertFrom-Json
        $snapshotBackup = Get-Content -LiteralPath $backupTemporaryPath -Raw |
            ConvertFrom-Json
        Assert-InstallStatePair `
            -Primary $snapshotPrimary `
            -Backup $snapshotBackup `
            -PrimaryPath $primaryTemporaryPath `
            -BackupPath $backupTemporaryPath
        if ([string]$snapshotPrimary.transactionId -ne
                [string]$Journal.transactionId -or
            [string]$snapshotPrimary.stateDigest -ne
                [string]$Journal.stateDigest)
        {
            throw 'The uninstall journal snapshot does not match its transaction.'
        }
        foreach ($entry in @(
                @{ Path = $primaryTemporaryPath; Sddl = [string]$Journal.primaryStateAcl },
                @{ Path = $backupTemporaryPath; Sddl = [string]$Journal.backupStateAcl }))
        {
            Assert-NoReparsePath -Path $entry.Path
            $acl = Get-Acl -LiteralPath $entry.Path
            $acl.SetSecurityDescriptorSddlForm($entry.Sddl)
            Set-Acl -LiteralPath $entry.Path -AclObject $acl
            Assert-NoReparsePath -Path $entry.Path
        }
        foreach ($entry in @(
                @{ Target = $Path; Snapshot = $primaryTemporaryPath; Sddl = [string]$Journal.primaryStateAcl },
                @{ Target = $backupPath; Snapshot = $backupTemporaryPath; Sddl = [string]$Journal.backupStateAcl }))
        {
            $resolvedTarget = [IO.Path]::GetFullPath([string]$entry.Target)
            Assert-NoReparsePath -Path $resolvedTarget -AllowMissing
            if (-not (Test-FileBytesEqual `
                    -LeftPath $resolvedTarget `
                    -RightPath $entry.Snapshot))
            {
                if (Test-Path -LiteralPath $resolvedTarget -PathType Leaf)
                {
                    [IO.File]::Replace($entry.Snapshot, $resolvedTarget, $null, $true)
                }
                else
                {
                    [IO.File]::Move($entry.Snapshot, $resolvedTarget)
                }
                Assert-NoReparsePath -Path $resolvedTarget
            }
            $acl = Get-Acl -LiteralPath $resolvedTarget
            $acl.SetSecurityDescriptorSddlForm($entry.Sddl)
            Set-Acl -LiteralPath $resolvedTarget -AclObject $acl
        }
        Assert-ProtectedStateAcl -Path $Path
        Assert-ProtectedStateAcl -Path $backupPath
        if (-not (Test-FileBytesEqual -LeftPath $Path -RightPath $backupPath))
        {
            throw 'The restored protected install-state pair is not byte-identical.'
        }
    }
    finally
    {
        if (Test-Path -LiteralPath $temporaryRoot -PathType Container)
        {
            Assert-NoReparseTree -Path $temporaryRoot
            Remove-Item -LiteralPath $temporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

function Get-CertificateSha256
{
    param(
        [Parameter(Mandatory = $true)]
        [Security.Cryptography.X509Certificates.X509Certificate2]$Certificate
    )

    $hasher = [Security.Cryptography.SHA256]::Create()
    try
    {
        return ([BitConverter]::ToString($hasher.ComputeHash(
            $Certificate.RawData))).Replace('-', '')
    }
    finally
    {
        $hasher.Dispose()
    }
}

function Assert-InstallStateIntegrity
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [AllowNull()]
        [object]$UninstallJournal = $null
    )

    $uninstallPhaseRank = -1
    if ($null -ne $UninstallJournal)
    {
        $uninstallPhaseRank = Get-UninstallPhaseRank `
            -Phase ([string]$UninstallJournal.phase)
    }

    foreach ($propertyName in @(
        'schema', 'packageName', 'applicationId', 'publisher',
        'productVersion', 'packageVersion', 'packageFullName',
        'packageFamilyName', 'certificateThumbprint',
        'certificateInstalledBySetup', 'externalLocation',
        'installedUserSid', 'hostFile', 'hostSha256', 'packageFile',
        'packageSha256', 'certificateSha256', 'transactionId'))
    {
        if ($null -eq $State.PSObject.Properties[$propertyName])
        {
            throw "Protected install state is missing: $propertyName"
        }
    }
    if ([int]$State.schema -notin @(1, 2) -or
        [string]$State.packageName -ne 'CialloKing.BaClickFxDesktop' -or
        [string]$State.applicationId -ne 'BaClickFxDesktop' -or
        [string]$State.publisher -ne 'CN=BaClickFx.Local' -or
        [string]$State.transactionId -notmatch '^[0-9a-fA-F]{32}$')
    {
        throw 'Protected install state has unsupported identity data.'
    }
    if ([IO.Path]::GetFullPath([string]$State.externalLocation) -ne
        [IO.Path]::GetFullPath($InstallRoot) -or
        [string]$State.installedUserSid -notmatch '^S-1-[0-9-]+$')
    {
        throw 'Protected install state points outside the installed identity.'
    }
    if ([string]$State.hostFile -ne 'ba-click-fx-desktop.exe' -or
        [string]$State.hostSha256 -notmatch '^[0-9A-Fa-f]{64}$' -or
        [string]$State.packageSha256 -notmatch '^[0-9A-Fa-f]{64}$' -or
        [string]$State.certificateSha256 -notmatch '^[0-9A-Fa-f]{64}$' -or
        [string]$State.certificateThumbprint -notmatch '^[0-9A-Fa-f]{40}$' -or
        $State.certificateInstalledBySetup -isnot [bool])
    {
        throw 'Protected install state contains invalid integrity data.'
    }
    $packageFile = [string]$State.packageFile
    if ([IO.Path]::IsPathRooted($packageFile) -or
        $packageFile.Contains('..') -or
        [IO.Path]::GetFileName($packageFile) -ne $packageFile -or
        $packageFile -notmatch '\.msix$')
    {
        throw 'Protected install state has an unsafe package file name.'
    }
    $hostPath = Join-Path $InstallRoot ([string]$State.hostFile)
    $packagePath = Join-Path (Join-Path $InstallRoot 'Identity') $packageFile
    $hostPresent = Test-Path -LiteralPath $hostPath -PathType Leaf
    $packagePresent = Test-Path -LiteralPath $packagePath -PathType Leaf
    if ($hostPresent)
    {
        Assert-NoReparsePath -Path $hostPath
    }
    if ($packagePresent)
    {
        Assert-NoReparsePath -Path $packagePath
    }
    $allowMissingHost = $uninstallPhaseRank -ge
        (Get-UninstallPhaseRank -Phase 'payload-removing')
    $allowMissingPackage = $uninstallPhaseRank -ge
        (Get-UninstallPhaseRank -Phase 'identity-removing')
    if ((-not $hostPresent -and -not $allowMissingHost) -or
        ($hostPresent -and
            (Get-FileHash -LiteralPath $hostPath -Algorithm SHA256).Hash -ne
                [string]$State.hostSha256) -or
        (-not $packagePresent -and -not $allowMissingPackage) -or
        ($packagePresent -and
            (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash -ne
                [string]$State.packageSha256))
    {
        throw 'Installed payload does not match protected install state.'
    }
    $certificate = Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
        Where-Object { $_.Thumbprint -eq [string]$State.certificateThumbprint } |
        Select-Object -First 1
    $allowMissingCertificate = $uninstallPhaseRank -ge
        (Get-UninstallPhaseRank -Phase 'certificate-removing')
    if (($null -eq $certificate -and -not $allowMissingCertificate) -or
        ($null -ne $certificate -and
            (Get-CertificateSha256 -Certificate $certificate) -ne
                [string]$State.certificateSha256))
    {
        throw 'Installed certificate does not match protected install state.'
    }
}

function Read-InstallStateWithBackup
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot
    )

    $resolvedPath = [IO.Path]::GetFullPath($Path)
    $backupPath = "$resolvedPath.bak"
    Assert-NoReparsePath -Path $resolvedPath -AllowMissing
    Assert-NoReparsePath -Path $backupPath -AllowMissing
    $journalPath = Join-Path ([IO.Path]::GetDirectoryName($resolvedPath)) 'UNINSTALL-STATE.json'
    $journal = Read-UninstallJournal -Path $journalPath
    $primaryExists = Test-Path -LiteralPath $Path -PathType Leaf
    $backupExists = Test-Path -LiteralPath $backupPath -PathType Leaf
    if ($null -ne $journal -and [string]$journal.phase -eq 'state-removing')
    {
        $pairMatchesJournal = $false
        if ($primaryExists -and $backupExists)
        {
            try
            {
                Assert-ProtectedStateAcl -Path $resolvedPath
                Assert-ProtectedStateAcl -Path $backupPath
                $currentPrimaryRaw = Get-Content -LiteralPath $resolvedPath -Raw
                $currentBackupRaw = Get-Content -LiteralPath $backupPath -Raw
                $currentPrimary = $currentPrimaryRaw | ConvertFrom-Json
                $currentBackup = $currentBackupRaw | ConvertFrom-Json
                Assert-InstallStatePair `
                    -Primary $currentPrimary `
                    -Backup $currentBackup `
                    -PrimaryPath $resolvedPath `
                    -BackupPath $backupPath
                $pairMatchesJournal =
                    [string]$currentPrimary.transactionId -eq
                        [string]$journal.transactionId -and
                    [string]$currentPrimary.stateDigest -eq
                        [string]$journal.stateDigest
            }
            catch
            {
                $pairMatchesJournal = $false
            }
        }
        if (-not $pairMatchesJournal)
        {
            Restore-InstallStateFromUninstallJournal `
                -Path $resolvedPath `
                -Journal $journal
        }
        $primaryExists = Test-Path -LiteralPath $Path -PathType Leaf
        $backupExists = Test-Path -LiteralPath $backupPath -PathType Leaf
    }
    if (-not $primaryExists -and -not $backupExists)
    {
        throw 'Protected install state is missing; refusing an imprecise uninstall.'
    }
    if (-not $primaryExists -or -not $backupExists)
    {
        throw 'Protected install state primary and backup must be present together.'
    }
    Assert-ProtectedStateAcl -Path $resolvedPath
    Assert-ProtectedStateAcl -Path $backupPath
    $primary = Get-Content -LiteralPath $resolvedPath -Raw | ConvertFrom-Json
    $backup = Get-Content -LiteralPath $backupPath -Raw | ConvertFrom-Json
    Assert-InstallStatePair `
        -Primary $primary `
        -Backup $backup `
            -PrimaryPath $resolvedPath `
        -BackupPath $backupPath
    if ($null -ne $journal)
    {
        Read-UninstallJournal -Path $journalPath -State $primary | Out-Null
        Assert-InstallStateIntegrity `
            -State $primary `
            -InstallRoot $InstallRoot `
            -UninstallJournal $journal
    }
    else
    {
        Assert-InstallStateIntegrity -State $primary -InstallRoot $InstallRoot
    }
    if ($null -eq $primary.PSObject.Properties['ownedCertificateThumbprints'])
    {
        $primary | Add-Member -NotePropertyName ownedCertificateThumbprints `
            -NotePropertyValue ([string]$primary.certificateThumbprint)
    }
    if ($null -eq $primary.PSObject.Properties['ownedPackageFiles'])
    {
        $primary | Add-Member -NotePropertyName ownedPackageFiles `
            -NotePropertyValue ([string]$primary.packageFile)
    }
    if ($null -eq $primary.PSObject.Properties['certificateOwnership'])
    {
        # Legacy states cannot prove ownership; never delete their certificate.
        $primary | Add-Member -NotePropertyName certificateOwnership `
            -NotePropertyValue 'unknown'
    }
    return $primary
}

function Remove-ProtectedInstallStatePair
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $resolvedPath = [IO.Path]::GetFullPath($Path)
    $backupPath = "$resolvedPath.bak"
    Assert-NoReparsePath -Path $resolvedPath
    Assert-NoReparsePath -Path $backupPath
    foreach ($statePath in @($resolvedPath, $backupPath))
    {
        if (-not (Test-Path -LiteralPath $statePath -PathType Leaf))
        {
            throw "Protected install-state file is missing before deletion: $statePath"
        }
    }

    # Keep exact bytes and ACLs outside the install directory. If the second
    # delete fails, restoring the pair preserves the uninstall recovery proof.
    $temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) `
        ('bafx-uninstall-state-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temporaryRoot -Force | Out-Null
    Assert-NoReparsePath -Path $temporaryRoot
    $primaryTemporaryPath = Join-Path $temporaryRoot 'INSTALL-STATE.json'
    $backupTemporaryPath = Join-Path $temporaryRoot 'INSTALL-STATE.json.bak'
    $primaryAcl = (Get-Acl -LiteralPath $resolvedPath).Sddl
    $backupAcl = (Get-Acl -LiteralPath $backupPath).Sddl
    Assert-NoReparsePath -Path $primaryTemporaryPath -AllowMissing
    Assert-NoReparsePath -Path $backupTemporaryPath -AllowMissing
    Copy-Item -LiteralPath $resolvedPath -Destination $primaryTemporaryPath -Force
    Copy-Item -LiteralPath $backupPath -Destination $backupTemporaryPath -Force
    Assert-NoReparsePath -Path $primaryTemporaryPath
    Assert-NoReparsePath -Path $backupTemporaryPath
    $primaryBytes = [Int64](Get-Item -LiteralPath $resolvedPath -Force).Length
    $backupBytes = [Int64](Get-Item -LiteralPath $backupPath -Force).Length
    $primaryHash = (Get-FileHash -LiteralPath $resolvedPath -Algorithm SHA256).Hash
    $backupHash = (Get-FileHash -LiteralPath $backupPath -Algorithm SHA256).Hash

    try
    {
        Remove-Item -LiteralPath $resolvedPath -Force
        if (Test-Path -LiteralPath $resolvedPath -PathType Leaf)
        {
            throw 'The primary protected install-state file remains after deletion.'
        }
        Remove-Item -LiteralPath $backupPath -Force
        if (Test-Path -LiteralPath $backupPath -PathType Leaf)
        {
            throw 'The backup protected install-state file remains after deletion.'
        }
    }
    catch
    {
        $deleteError = $_
        $restoreError = $null
        try
        {
            Assert-NoReparsePath -Path $resolvedPath -AllowMissing
            Copy-Item -LiteralPath $primaryTemporaryPath -Destination $resolvedPath -Force
            Assert-NoReparsePath -Path $resolvedPath
            $primaryRestoredAcl = Get-Acl -LiteralPath $resolvedPath
            $primaryRestoredAcl.SetSecurityDescriptorSddlForm($primaryAcl)
            Set-Acl -LiteralPath $resolvedPath -AclObject $primaryRestoredAcl

            Copy-Item -LiteralPath $backupTemporaryPath -Destination $backupPath -Force
            $backupRestoredAcl = Get-Acl -LiteralPath $backupPath
            $backupRestoredAcl.SetSecurityDescriptorSddlForm($backupAcl)
            Set-Acl -LiteralPath $backupPath -AclObject $backupRestoredAcl

            Assert-ProtectedStateAcl -Path $resolvedPath
            Assert-ProtectedStateAcl -Path $backupPath
            if ([Int64](Get-Item -LiteralPath $resolvedPath -Force).Length -ne $primaryBytes -or
                (Get-FileHash -LiteralPath $resolvedPath -Algorithm SHA256).Hash -ne $primaryHash)
            {
                throw 'The restored primary install-state file does not match its saved bytes.'
            }
            if ([Int64](Get-Item -LiteralPath $backupPath -Force).Length -ne $backupBytes -or
                (Get-FileHash -LiteralPath $backupPath -Algorithm SHA256).Hash -ne $backupHash)
            {
                throw 'The restored backup install-state file does not match its saved bytes.'
            }
            $restoredPrimary = Get-Content -LiteralPath $resolvedPath -Raw | ConvertFrom-Json
            $restoredBackup = Get-Content -LiteralPath $backupPath -Raw | ConvertFrom-Json
            Assert-InstallStatePair `
                -Primary $restoredPrimary `
                -Backup $restoredBackup `
                -PrimaryPath $resolvedPath `
                -BackupPath $backupPath
        }
        catch
        {
            $restoreError = $_
        }
        if ($null -ne $restoreError)
        {
            throw "Protected install-state deletion failed and recovery failed: $($restoreError.Exception.Message)"
        }
        throw $deleteError
    }
    finally
    {
        if (Test-Path -LiteralPath $temporaryRoot -PathType Container)
        {
            Assert-NoReparseTree -Path $temporaryRoot
            Remove-Item -LiteralPath $temporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

function Remove-InstalledPayloadFiles
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [Parameter(Mandatory = $true)]
        [object]$State
    )

    $knownFiles = @(
        'ba-click-fx-desktop.exe',
        'BAFX.ControlCenter.exe',
        'LICENSE.txt',
        'SUPPORT.md',
        'THIRD-PARTY-NOTICES.txt'
    )
    foreach ($relativePath in $knownFiles)
    {
        $path = Join-Path $InstallRoot $relativePath
        if (Test-Path -LiteralPath $path -PathType Leaf)
        {
            # A reparse file may redirect an elevated deletion outside the
            # protected install root, so validate the full component path.
            Assert-NoReparsePath -Path $path
            Remove-Item -LiteralPath $path -Force
        }
        if (Test-Path -LiteralPath $path -PathType Leaf)
        {
            throw "Installed payload file remains after uninstall: $relativePath"
        }
    }
    foreach ($directoryName in @('Identity', '.rollback', '.staging'))
    {
        $path = Join-Path $InstallRoot $directoryName
        if (Test-Path -LiteralPath $path -PathType Container)
        {
            Assert-NoReparseTree -Path $path
            Remove-Item -LiteralPath $path -Recurse -Force
        }
        if (Test-Path -LiteralPath $path)
        {
            throw "Installed payload directory remains after uninstall: $directoryName"
        }
    }
}

function Remove-CertificateFromStores
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Thumbprint,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedSubject
    )

    $normalizedThumbprint = $Thumbprint.ToUpperInvariant()
    if ($normalizedThumbprint -notmatch '^[0-9A-F]{40}$')
    {
        throw 'The certificate cleanup thumbprint is invalid.'
    }

    # Keep the trust entry until the private key is definitely gone. This
    # leaves a recoverable public certificate when key deletion is blocked.
    $privateCertificate = Get-ChildItem -Path 'Cert:\LocalMachine\My' |
        Where-Object { $_.Thumbprint -eq $normalizedThumbprint } |
        Select-Object -First 1
    if ($null -ne $privateCertificate)
    {
        if ($privateCertificate.Subject -ne $ExpectedSubject)
        {
            throw 'Refusing to remove a private certificate with an unexpected subject.'
        }
        Remove-Item -LiteralPath $privateCertificate.PSPath -DeleteKey -Force
        if ($null -ne (Get-ChildItem -Path 'Cert:\LocalMachine\My' |
                Where-Object { $_.Thumbprint -eq $normalizedThumbprint } |
                Select-Object -First 1))
        {
            throw "The private certificate remains after cleanup: $normalizedThumbprint"
        }
    }

    $trustedCertificate = Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
        Where-Object { $_.Thumbprint -eq $normalizedThumbprint } |
        Select-Object -First 1
    if ($null -ne $trustedCertificate)
    {
        if ($trustedCertificate.Subject -ne $ExpectedSubject)
        {
            throw 'Refusing to remove a trusted certificate with an unexpected subject.'
        }
        Remove-Item -LiteralPath $trustedCertificate.PSPath -Force
        if ($null -ne (Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
                Where-Object { $_.Thumbprint -eq $normalizedThumbprint } |
                Select-Object -First 1))
        {
            throw "The trusted certificate remains after cleanup: $normalizedThumbprint"
        }
    }
}

$script:InstallerStep = 'validate-administrator'
trap
{
    Write-BafxInstallerFailure `
        -ErrorRecord $_ `
        -Phase 'UninstallMachine' `
        -Step $script:InstallerStep `
        -ProductVersion $script:InstallerProductVersion `
        -PackageVersion $script:InstallerPackageVersion
    exit 1
}

Assert-Administrator
$script:InstallerStep = 'validate-powershell'
if ($PSVersionTable.PSEdition -ne 'Desktop')
{
    throw 'Uninstall requires Windows PowerShell 5.1.'
}

$script:InstallerStep = 'resolve-install-root'
$installRoot = Resolve-ProtectedProgramFilesPath `
    -Path $InstallDirectory `
    -Description 'uninstall directory'
Assert-NoReparsePath -Path $installRoot
$statePath = Join-Path $installRoot 'Installer\INSTALL-STATE.json'
$uninstallCompleteMarkerPath = Join-Path $installRoot 'Installer\UNINSTALL-COMPLETE.json'
$uninstallJournalPath = Join-Path $installRoot 'Installer\UNINSTALL-STATE.json'
$pendingPath = Join-Path $installRoot 'Installer\PREPARE-STATE.json'
$completionMarkerError = $null
$script:InstallerStep = 'check-pending-installation-transaction'
if (Test-Path -LiteralPath $pendingPath -PathType Leaf)
{
    throw 'A pending installation transaction remains; complete rollback before uninstalling.'
}
$primaryStateExists = Test-Path -LiteralPath $statePath -PathType Leaf
$backupStateExists = Test-Path -LiteralPath "$statePath.bak" -PathType Leaf
if (-not $primaryStateExists -and -not $backupStateExists -and
    (Test-Path -LiteralPath $uninstallCompleteMarkerPath -PathType Leaf))
{
    # The state pair is deleted before this marker is published. A valid marker
    # therefore means only the final Inno cleanup remains after a restart.
    $script:InstallerStep = 'verify-completed-uninstall'
    try
    {
        Read-UninstallCompletionMarker `
            -Path $uninstallCompleteMarkerPath `
            -JournalPath $uninstallJournalPath | Out-Null
        exit 0
    }
    catch
    {
        # A crash can leave a torn marker after the state pair was already
        # deleted. A validated state-removing journal can safely rebuild it.
        $completionMarkerError = $_
    }
}
if (-not $primaryStateExists -and -not $backupStateExists)
{
    $journalForMissingState = Read-UninstallJournal -Path $uninstallJournalPath
    if ($null -ne $journalForMissingState -and
        [string]$journalForMissingState.phase -eq 'state-removing')
    {
        $script:InstallerStep = 'recover-completion-marker'
        $snapshotState = Read-UninstallJournalSnapshotState `
            -Journal $journalForMissingState
        Write-UninstallCompletionMarker `
            -Path $uninstallCompleteMarkerPath `
            -JournalPath $uninstallJournalPath `
            -State $snapshotState
        exit 0
    }
    if ($null -ne $completionMarkerError)
    {
        throw $completionMarkerError
    }
}
$script:InstallerStep = 'read-protected-install-state'
$state = Read-InstallStateWithBackup -Path $statePath -InstallRoot $installRoot
$script:InstallerStep = 'validate-protected-install-state'
foreach ($propertyName in @(
    'schema',
    'packageName',
    'applicationId',
    'publisher',
    'productVersion',
    'packageVersion',
    'packageFullName',
    'packageFamilyName',
    'certificateThumbprint',
    'certificateInstalledBySetup',
    'externalLocation',
    'installedUserSid',
    'packageFile',
    'ownedCertificateThumbprints',
    'ownedPackageFiles',
    'certificateOwnership'))
{
    if ($null -eq $state.PSObject.Properties[$propertyName])
    {
        throw "Protected install state is missing: $propertyName"
    }
}
$script:InstallerProductVersion = [string]$state.productVersion
$script:InstallerPackageVersion = [string]$state.packageVersion
$stateInvalid = `
    ([int]$state.schema -notin @(1, 2)) -or `
    ([string]$state.packageName -ne 'CialloKing.BaClickFxDesktop') -or `
    ([string]$state.applicationId -ne 'BaClickFxDesktop') -or `
    ([string]$state.publisher -ne 'CN=BaClickFx.Local') -or `
    ([string]$state.productVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') -or `
    ([string]$state.packageVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$') -or `
    ([string]$state.packageFullName -notmatch '^CialloKing\.BaClickFxDesktop_[A-Za-z0-9._-]+$') -or `
    ([string]$state.packageFamilyName -notmatch '^CialloKing\.BaClickFxDesktop_[A-Za-z0-9-]+$') -or `
    ([string]$state.certificateThumbprint -notmatch '^[0-9A-Fa-f]{40}$') -or `
    ($state.certificateInstalledBySetup -isnot [bool]) -or `
    ([string]$state.certificateOwnership -notin @(
        'installer-owned', 'preexisting', 'shared', 'unknown')) -or `
    ([string]$state.installedUserSid -notmatch '^S-1-[0-9-]+$') -or `
    ([IO.Path]::GetFullPath([string]$state.externalLocation) -ne $installRoot)
if ($stateInvalid)
{
    throw 'Protected install state failed safety validation.'
}
$packageFile = [string]$state.packageFile
if ([IO.Path]::IsPathRooted($packageFile) -or
    $packageFile.Contains('..') -or
    [IO.Path]::GetFileName($packageFile) -ne $packageFile -or
    $packageFile -notmatch '\.msix$')
{
    throw 'Protected install state has an unsafe package file name.'
}
$uninstallJournal = Read-UninstallJournal `
    -Path $uninstallJournalPath `
    -State $state
$script:InstallerStep = 'write-uninstall-journal'
if ($null -eq $uninstallJournal)
{
    Write-UninstallJournal `
        -Path $uninstallJournalPath `
        -StatePath $statePath `
        -State $state `
        -Phase 'started'
}

$script:InstallerStep = 'ensure-host-process-stopped'
Assert-ExpectedProcessIsStopped -ExecutablePath (Join-Path $installRoot 'ba-click-fx-desktop.exe')
$script:InstallerStep = 'ensure-control-center-process-stopped'
Assert-ExpectedProcessIsStopped -ExecutablePath (Join-Path $installRoot 'BAFX.ControlCenter.exe')

$script:InstallerStep = 'mark-user-package-removal'
Write-UninstallJournal `
    -Path $uninstallJournalPath `
    -StatePath $statePath `
    -State $state `
    -Phase 'user-package-removing'
$script:InstallerStep = 'remove-installed-user-startup-registration'
Remove-InstalledUserStartupRegistration `
    -InstalledUserSid ([string]$state.installedUserSid)

$script:InstallerStep = 'query-installed-user-package'
$registered = @(
    Get-AppxPackage `
        -User ([string]$state.installedUserSid) `
        -Name ([string]$state.packageName) `
        -ErrorAction Stop
)
$target = $registered |
    Where-Object { $_.PackageFullName -eq [string]$state.packageFullName } |
    Select-Object -First 1
if ($null -ne $target)
{
    $script:InstallerStep = 'remove-installed-user-package'
    Remove-AppxPackage `
        -Package $target.PackageFullName `
        -User ([string]$state.installedUserSid) `
        -ErrorAction Stop
}

$script:InstallerStep = 'wait-for-user-package-removal'
$remaining = @()
for ($attempt = 0; $attempt -lt 15; ++$attempt)
{
    $remaining = @(
        Get-AppxPackage `
            -User ([string]$state.installedUserSid) `
            -Name ([string]$state.packageName) `
            -ErrorAction Stop |
            Where-Object { $_.PackageFullName -eq [string]$state.packageFullName }
    )
    if ($remaining.Count -eq 0)
    {
        break
    }
    Start-Sleep -Milliseconds 200
}
if ($remaining.Count -gt 0)
{
    throw 'Sparse package registration remains after uninstall.'
}

$script:InstallerStep = 'query-other-user-packages'
$otherUserPackages = @(
    Get-AppxPackage -AllUsers -Name ([string]$state.packageName) -ErrorAction Stop
)
if ($otherUserPackages.Count -gt 0)
{
    throw 'Another user package registration remains; keeping shared files and certificate.'
}

$script:InstallerStep = 'mark-identity-package-removal'
Write-UninstallJournal `
    -Path $uninstallJournalPath `
    -StatePath $statePath `
    -State $state `
    -Phase 'identity-removing'
$script:InstallerStep = 'remove-owned-identity-packages'
$ownedFiles = Split-Ledger -Value $state.ownedPackageFiles -Separator Pipe
foreach ($ownedFile in $ownedFiles)
{
    if ([IO.Path]::IsPathRooted($ownedFile) -or
        $ownedFile.Contains('..') -or
        [IO.Path]::GetFileName($ownedFile) -ne $ownedFile -or
        $ownedFile -notmatch '\.msix$')
    {
        throw 'Protected install state has an unsafe package ledger entry.'
    }
    $ownedPath = Join-Path (Join-Path $installRoot 'Identity') $ownedFile
    if (Test-Path -LiteralPath $ownedPath -PathType Leaf)
    {
        Remove-Item -LiteralPath $ownedPath -Force
    }
    if (Test-Path -LiteralPath $ownedPath -PathType Leaf)
    {
        throw "The installer-owned identity package remains after uninstall: $ownedFile"
    }
}

$script:InstallerStep = 'mark-certificate-removal'
Write-UninstallJournal `
    -Path $uninstallJournalPath `
    -StatePath $statePath `
    -State $state `
    -Phase 'certificate-removing'
$script:InstallerStep = 'remove-owned-certificates'
if ([string]$state.certificateOwnership -eq 'unknown')
{
    Write-Warning 'Certificate ownership is unknown in the legacy install state; preserving all certificate entries.'
}
else
{
    $currentThumbprint = ([string]$state.certificateThumbprint).ToUpperInvariant()
    foreach ($thumbprint in (Split-Ledger `
            -Value $state.ownedCertificateThumbprints `
            -Separator Comma))
    {
        if ($thumbprint -notmatch '^[0-9A-Fa-f]{40}$')
        {
            throw 'Protected install state has an unsafe certificate ledger entry.'
        }
        if ($thumbprint.ToUpperInvariant() -eq $currentThumbprint -and
            [string]$state.certificateOwnership -in @('preexisting', 'shared'))
        {
            continue
        }
        Remove-CertificateFromStores `
            -Thumbprint $thumbprint `
            -ExpectedSubject ([string]$state.publisher)
    }
}

# Keep the state pair until every destructive payload operation has succeeded.
# If cleanup is interrupted, the journal tells the next attempt which already
# removed resources may be absent while the state pair remains authoritative.
$script:InstallerStep = 'mark-payload-removal'
Write-UninstallJournal `
    -Path $uninstallJournalPath `
    -StatePath $statePath `
    -State $state `
    -Phase 'payload-removing'
$script:InstallerStep = 'remove-installed-payload-files'
Remove-InstalledPayloadFiles -InstallRoot $installRoot -State $state

$script:InstallerStep = 'mark-state-removal'
Write-UninstallJournal `
    -Path $uninstallJournalPath `
    -StatePath $statePath `
    -State $state `
    -Phase 'state-removing'
$script:InstallerStep = 'delete-protected-install-state'
Remove-ProtectedInstallStatePair -Path $statePath

$script:InstallerStep = 'write-uninstall-complete-marker'
Write-UninstallCompletionMarker `
    -Path $uninstallCompleteMarkerPath `
    -JournalPath $uninstallJournalPath `
    -State $state
if (-not (Test-Path -LiteralPath $uninstallCompleteMarkerPath -PathType Leaf))
{
    throw 'The uninstall completion marker was not written.'
}

# Inno deletes Installer after this process exits and verifies the marker. The
# running PowerShell script therefore never has to remove its own directory.
