[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Prepare', 'CommitFiles', 'Finalize', 'Rollback', 'RollbackCleanup')]
    [string]$Phase,

    [Parameter(Mandatory = $true)]
    [string]$InstallDirectory,

    [string]$PayloadDirectory = '',

    [Parameter(Mandatory = $true)]
    [string]$UserContextPath,

    [Parameter(Mandatory = $true)]
    [string]$MachineStatePath,

    [Parameter(Mandatory = $true)]
    [string]$RegistrationResultPath,

    [string]$ProductVersion = '',

    [string]$PackageVersion = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installer-diagnostics.ps1')
. (Join-Path $PSScriptRoot 'protected-paths.ps1')
$script:InstallerStep = 'initialize'
$script:InstallerRelatedFailures = New-Object Collections.Generic.List[object]
$script:PayloadRoot = ''
$script:RollbackRoot = ''
$script:PayloadManifest = $null

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

function Stop-InstallerWithFailure
{
    param(
        [Parameter(Mandatory = $true)]
        [Management.Automation.ErrorRecord]$ErrorRecord,

        [Parameter(Mandatory = $true)]
        [string]$Step,

        [int]$ExitCode = 1
    )

    Write-BafxInstallerFailure `
        -ErrorRecord $ErrorRecord `
        -Phase $Phase `
        -Step $Step `
        -ProductVersion $ProductVersion `
        -PackageVersion $PackageVersion `
        -RelatedFailures $script:InstallerRelatedFailures.ToArray()
    exit $ExitCode
}

function Assert-Administrator
{
    $principal = New-Object Security.Principal.WindowsPrincipal(
        [Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
    {
        throw 'The machine installation phase requires administrator privileges.'
    }
}

function Assert-WindowsPowerShell
{
    if ($PSVersionTable.PSEdition -ne 'Desktop')
    {
        throw 'The machine installation phase requires Windows PowerShell 5.1.'
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

    $encoding = New-Object -TypeName System.Text.UTF8Encoding -ArgumentList $false
    [IO.File]::WriteAllText($Path, $Content, $encoding)
}

function Get-StatePropertiesWithoutDigest
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$Value
    )

    $ordered = [ordered]@{}
    if ($Value -is [Collections.IDictionary])
    {
        foreach ($entry in $Value.GetEnumerator())
        {
            if ([string]$entry.Key -ne 'stateDigest')
            {
                $ordered[[string]$entry.Key] = $entry.Value
            }
        }
    }
    else
    {
        foreach ($property in $Value.PSObject.Properties)
        {
            if ($property.Name -ne 'stateDigest')
            {
                $ordered[$property.Name] = $property.Value
            }
        }
    }
    return $ordered
}

function Convert-StateToCanonicalJson
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$Value
    )

    return (Get-StatePropertiesWithoutDigest -Value $Value |
        ConvertTo-Json -Depth 12)
}

function Get-StateDigest
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$Value
    )

    $bytes = [Text.Encoding]::UTF8.GetBytes(
        (Convert-StateToCanonicalJson -Value $Value))
    $hasher = [Security.Cryptography.SHA256]::Create()
    try
    {
        return ([BitConverter]::ToString($hasher.ComputeHash($bytes))).Replace('-', '')
    }
    finally
    {
        $hasher.Dispose()
    }
}

function New-StateWithDigest
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$Value
    )

    $ordered = Get-StatePropertiesWithoutDigest -Value $Value
    $ordered.stateDigest = Get-StateDigest -Value $ordered
    return $ordered
}

function Write-FlushedUtf8NoBom
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Content
    )

    $bytes = [Text.Encoding]::UTF8.GetBytes($Content)
    $stream = [IO.FileStream]::new(
        $Path,
        [IO.FileMode]::Create,
        [IO.FileAccess]::Write,
        [IO.FileShare]::None)
    try
    {
        $stream.Write($bytes, 0, $bytes.Length)
        # Flush(true) closes the crash window between a successful write and
        # the directory entry becoming durable on disk.
        $stream.Flush($true)
    }
    finally
    {
        $stream.Dispose()
    }
}

function Replace-ProtectedFile
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$TemporaryPath,

        [Parameter(Mandatory = $true)]
        [string]$DestinationPath,

        [AllowNull()]
        [string]$ReadSid
    )

    Set-ProtectedStateAcl -Path $TemporaryPath -ReadSid $ReadSid
    if (Test-Path -LiteralPath $DestinationPath -PathType Leaf)
    {
        [IO.File]::Replace($TemporaryPath, $DestinationPath, $null, $true)
    }
    else
    {
        [IO.File]::Move($TemporaryPath, $DestinationPath)
    }
    Set-ProtectedStateAcl -Path $DestinationPath -ReadSid $ReadSid
}

function Set-ProtectedStateAcl
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [AllowNull()]
        [string]$ReadSid
    )

    $acl = New-Object Security.AccessControl.FileSecurity
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($sidValue in @('S-1-5-18', 'S-1-5-32-544'))
    {
        $sid = New-Object Security.Principal.SecurityIdentifier($sidValue)
        $rule = New-Object Security.AccessControl.FileSystemAccessRule(
            $sid,
            'FullControl',
            'Allow')
        $acl.AddAccessRule($rule) | Out-Null
    }
    if (-not [string]::IsNullOrWhiteSpace($ReadSid))
    {
        $sid = New-Object Security.Principal.SecurityIdentifier($ReadSid)
        $rule = New-Object Security.AccessControl.FileSystemAccessRule(
            $sid,
            'ReadAndExecute',
            'Allow')
        $acl.AddAccessRule($rule) | Out-Null
    }
    Set-Acl -LiteralPath $Path -AclObject $acl
}

function Assert-ProtectedStateAcl
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $acl = Get-Acl -LiteralPath $Path
    if (-not $acl.AreAccessRulesProtected)
    {
        throw 'Protected installer state still inherits access rules.'
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
            throw 'Protected installer state grants write access to a non-administrator.'
        }
    }
}

function Write-ProtectedJson
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [object]$Value,

        [AllowNull()]
        [string]$ReadSid
    )

    $temporaryPath = "$Path.$PID.$([Guid]::NewGuid().ToString('N')).tmp"
    try
    {
        $stateValue = New-StateWithDigest -Value $Value
        $serialized = ConvertTo-Json -InputObject $stateValue -Depth 12
        Write-FlushedUtf8NoBom `
            -Path $temporaryPath `
            -Content $serialized
        Replace-ProtectedFile `
            -TemporaryPath $temporaryPath `
            -DestinationPath $Path `
            -ReadSid $ReadSid
    }
    finally
    {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf)
        {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
}

function Write-ProtectedInstallState
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [object]$Value,

        [Parameter(Mandatory = $true)]
        [string]$ReadSid
    )

    $stateValue = New-StateWithDigest -Value $Value
    $backupPath = "$Path.bak"
    $backupTemporaryPath = "$backupPath.$PID.$([Guid]::NewGuid().ToString('N')).tmp"
    $primaryTemporaryPath = "$Path.$PID.$([Guid]::NewGuid().ToString('N')).tmp"
    $serialized = ConvertTo-Json -InputObject $stateValue -Depth 12
    try
    {
        # Commit the backup first. A failure here leaves the previous primary
        # untouched and the pending journal can still direct recovery.
        Write-FlushedUtf8NoBom `
            -Path $backupTemporaryPath `
            -Content $serialized
        Replace-ProtectedFile `
            -TemporaryPath $backupTemporaryPath `
            -DestinationPath $backupPath `
            -ReadSid $ReadSid

        Write-FlushedUtf8NoBom `
            -Path $primaryTemporaryPath `
            -Content $serialized
        Replace-ProtectedFile `
            -TemporaryPath $primaryTemporaryPath `
            -DestinationPath $Path `
            -ReadSid $ReadSid

        $primary = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
        $backup = Get-Content -LiteralPath $backupPath -Raw | ConvertFrom-Json
        Assert-InstallStatePair -Primary $primary -Backup $backup
    }
    finally
    {
        foreach ($temporaryPath in @($backupTemporaryPath, $primaryTemporaryPath))
        {
            if (Test-Path -LiteralPath $temporaryPath -PathType Leaf)
            {
                Remove-Item -LiteralPath $temporaryPath -Force
            }
        }
    }
}

function Assert-InstallStatePair
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$Primary,

        [Parameter(Mandatory = $true)]
        [object]$Backup
    )

    $primaryTransaction = if ($null -eq $Primary.PSObject.Properties['transactionId'])
    {
        ''
    }
    else
    {
        [string]$Primary.transactionId
    }
    $backupTransaction = if ($null -eq $Backup.PSObject.Properties['transactionId'])
    {
        ''
    }
    else
    {
        [string]$Backup.transactionId
    }
    if ([string]::IsNullOrWhiteSpace($primaryTransaction) -or
        $primaryTransaction -ne $backupTransaction)
    {
        throw 'Protected install state primary and backup transactions differ.'
    }
    $primaryDigest = if ($null -eq $Primary.PSObject.Properties['stateDigest'])
    {
        ''
    }
    else
    {
        [string]$Primary.stateDigest
    }
    $backupDigest = if ($null -eq $Backup.PSObject.Properties['stateDigest'])
    {
        ''
    }
    else
    {
        [string]$Backup.stateDigest
    }
    if ($primaryDigest -notmatch '^[0-9A-Fa-f]{64}$' -or
        $backupDigest -notmatch '^[0-9A-Fa-f]{64}$' -or
        $primaryDigest -ne $backupDigest)
    {
        # Legacy schema 2 did not carry a digest. It is accepted only when the
        # canonical payload is byte-for-byte equivalent after parsing; a mixed
        # transaction is never recovered from an arbitrary backup.
        if (-not [string]::IsNullOrWhiteSpace($primaryDigest) -or
            -not [string]::IsNullOrWhiteSpace($backupDigest) -or
            (Convert-StateToCanonicalJson -Value $Primary) -ne
                (Convert-StateToCanonicalJson -Value $Backup))
        {
            throw 'Protected install state primary and backup digests differ.'
        }
    }
    else
    {
        if ((Get-StateDigest -Value $Primary) -ne $primaryDigest -or
            (Get-StateDigest -Value $Backup) -ne $backupDigest)
        {
            throw 'Protected install state digest does not match its content.'
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

function Join-Ledger
{
    param(
        [AllowNull()]
        [object[]]$Values,

        [Parameter(Mandatory = $true)]
        [ValidateSet('Comma', 'Pipe')]
        [string]$Separator
    )

    $delimiter = if ($Separator -eq 'Comma') { ',' } else { '|' }
    return (@(
            $Values |
                Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) } |
                ForEach-Object { ([string]$_).Trim() } |
                Sort-Object -Unique
        ) -join $delimiter)
}

function Get-ExplicitlyOwnedCertificateThumbprints
{
    param(
        [AllowNull()]
        [object]$State
    )

    if ($null -eq $State)
    {
        return @()
    }
    $ownership = if ($null -ne $State.PSObject.Properties['certificateOwnership'])
    {
        [string]$State.certificateOwnership
    }
    else
    {
        'unknown'
    }
    if ($ownership -ne 'installer-owned')
    {
        # A legacy boolean or a pre-existing/shared marker cannot prove that
        # this installer created the certificate. Keep the ledger empty so a
        # later upgrade or uninstall cannot delete a user's certificate.
        return @()
    }
    if ($null -ne $State.PSObject.Properties['ownedCertificateThumbprints'])
    {
        return @(Split-Ledger `
                -Value $State.ownedCertificateThumbprints `
                -Separator Comma)
    }
    if ([string]$State.certificateThumbprint -match '^[0-9A-Fa-f]{40}$')
    {
        return @([string]$State.certificateThumbprint)
    }
    return @()
}

function Assert-TemporaryStatePath
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $resolved = [IO.Path]::GetFullPath($Path)
    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase))
    {
        throw "Installer state must remain below the temporary directory: $resolved"
    }
    return $resolved
}

function Resolve-PayloadDirectory
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [Parameter(Mandatory = $true)]
        [string]$PayloadPath
    )

    $resolvedInstallRoot = [IO.Path]::GetFullPath($InstallRoot).TrimEnd('\')
    $resolvedPayload = [IO.Path]::GetFullPath($PayloadPath).TrimEnd('\')
    $expected = Join-Path $resolvedInstallRoot '.staging\current'
    if (-not $resolvedPayload.Equals(
            [IO.Path]::GetFullPath($expected).TrimEnd('\'),
            [StringComparison]::OrdinalIgnoreCase))
    {
        throw "Installer payload must remain in the protected staging directory: $expected"
    }
    if (-not (Test-Path -LiteralPath $resolvedPayload -PathType Container))
    {
        throw "Installer staging directory is missing: $resolvedPayload"
    }
    return $resolvedPayload
}

function Assert-ProtectedPayloadAcl
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $items = @(
        Get-ChildItem -LiteralPath $Path -Recurse -Force -ErrorAction Stop
    ) + @(Get-Item -LiteralPath $Path -Force -ErrorAction Stop)
    foreach ($item in $items)
    {
        $acl = Get-Acl -LiteralPath $item.FullName
        foreach ($rule in $acl.Access)
        {
            if ($rule.AccessControlType -ne
                [Security.AccessControl.AccessControlType]::Allow)
            {
                continue
            }
            $sid = $rule.IdentityReference.Translate(
                [Security.Principal.SecurityIdentifier]).Value
            if ($sid -in @('S-1-5-18', 'S-1-5-32-544'))
            {
                continue
            }
            $writeRights = [int]([Security.AccessControl.FileSystemRights]::WriteData -bor
                [Security.AccessControl.FileSystemRights]::AppendData -bor
                [Security.AccessControl.FileSystemRights]::Delete -bor
                [Security.AccessControl.FileSystemRights]::ChangePermissions -bor
                [Security.AccessControl.FileSystemRights]::TakeOwnership)
            if (([int]$rule.FileSystemRights -band $writeRights) -ne 0)
            {
                throw "Installer staging path is writable by a non-administrator: $($item.FullName)"
            }
        }
    }
}

function Ensure-ProtectedInstallerDirectory
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [Parameter(Mandatory = $true)]
        [string]$ReadSid
    )

    $installerDirectory = Join-Path $InstallRoot 'Installer'
    $created = $false
    if (-not (Test-Path -LiteralPath $installerDirectory))
    {
        New-Item -ItemType Directory -Path $installerDirectory -Force | Out-Null
        Set-ProtectedStateAcl -Path $installerDirectory -ReadSid $ReadSid
        $created = $true
    }
    elseif (-not (Test-Path -LiteralPath $installerDirectory -PathType Container))
    {
        throw 'The protected Installer path is not a directory.'
    }
    $directoryItem = Get-Item -LiteralPath $installerDirectory -Force
    if (($directoryItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
    {
        throw 'The protected Installer directory cannot be a reparse point.'
    }
    Assert-ProtectedStateAcl -Path $installerDirectory
    return $created
}

function Read-PayloadManifest
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$PayloadRoot
    )

    $manifestPath = Join-Path $PayloadRoot 'Installer\INSTALLER-PAYLOAD.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ([int]$manifest.schema -ne 2 -or
        [string]$manifest.version -ne $ProductVersion -or
        [string]$manifest.identityMode -ne 'target-machine-self-signed')
    {
        throw 'The installer payload manifest has an unexpected version.'
    }
    if ($null -eq $manifest.files -or @($manifest.files).Count -eq 0)
    {
        throw 'The installer payload manifest is empty.'
    }
    return $manifest
}

function Assert-FileHash
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [Int64]$ExpectedBytes,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedSha256
    )

    $file = Get-Item -LiteralPath $Path -ErrorAction Stop
    if ($file.Length -ne $ExpectedBytes)
    {
        throw "Payload size mismatch: $Path"
    }
    $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($hash -ne $ExpectedSha256)
    {
        throw "Payload hash mismatch: $Path"
    }
}

function Get-IdentityTemplateContentHash
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    try
    {
        $blockMapEntry = $archive.Entries |
            Where-Object { $_.FullName -eq 'AppxBlockMap.xml' } |
            Select-Object -First 1
        if ($null -eq $blockMapEntry)
        {
            throw 'Identity package block map is missing.'
        }
        $reader = New-Object IO.StreamReader(
            $blockMapEntry.Open(),
            [Text.Encoding]::UTF8,
            $true)
        try
        {
            $blockMap = [xml]$reader.ReadToEnd()
        }
        finally
        {
            $reader.Dispose()
        }

        # MakeAppx is allowed to enumerate equivalent entries in a different
        # order. Hash the sorted block-map semantics instead of ZIP metadata.
        $canonical = New-Object Text.StringBuilder
        $files = @(
            $blockMap.SelectNodes("/*[local-name()='BlockMap']/*[local-name()='File']") |
                Sort-Object { $_.GetAttribute('Name') }
        )
        foreach ($file in $files)
        {
            foreach ($attributeName in @('Name', 'Size', 'LfhSize'))
            {
                [void]$canonical.Append($file.GetAttribute($attributeName)).Append("`0")
            }
            $blocks = @(
                $file.ChildNodes |
                    Where-Object { $_.LocalName -eq 'Block' }
            )
            foreach ($block in $blocks)
            {
                [void]$canonical.Append($block.GetAttribute('Hash')).Append("`0")
                [void]$canonical.Append($block.GetAttribute('Size')).Append("`0")
            }
            $fileHash = $file.ChildNodes |
                Where-Object { $_.LocalName -eq 'FileHash' } |
                Select-Object -First 1
            if ($null -ne $fileHash)
            {
                [void]$canonical.Append($fileHash.GetAttribute('Hash'))
            }
            [void]$canonical.Append("`n")
        }
        $bytes = [Text.Encoding]::UTF8.GetBytes($canonical.ToString())
        $digest = [Security.Cryptography.SHA256]::Create().ComputeHash($bytes)
        return ([BitConverter]::ToString($digest)).Replace('-', '')
    }
    finally
    {
        $archive.Dispose()
    }
}

function Assert-PayloadManifest
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstallRoot
    )

    $manifest = Read-PayloadManifest -PayloadRoot $InstallRoot
    $hostEntries = @(
        @($manifest.files) |
            Where-Object { [string]$_.path -eq 'ba-click-fx-desktop.exe' }
    )
    if ($hostEntries.Count -ne 1 -or
        [string]$hostEntries[0].sha256 -notmatch '^[0-9A-Fa-f]{64}$')
    {
        throw 'The installer payload manifest must identify exactly one Host.'
    }
    $rootPrefix = $InstallRoot.TrimEnd('\') + '\'
    foreach ($entry in @($manifest.files))
    {
        $relativePath = [string]$entry.path
        if ([IO.Path]::IsPathRooted($relativePath) -or $relativePath.Contains('..'))
        {
            throw "Unsafe payload path: $relativePath"
        }
        $fullPath = [IO.Path]::GetFullPath((Join-Path $InstallRoot $relativePath))
        if (-not $fullPath.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase))
        {
            throw "Payload path escaped the install root: $relativePath"
        }
        Assert-FileHash `
            -Path $fullPath `
            -ExpectedBytes ([Int64]$entry.bytes) `
            -ExpectedSha256 ([string]$entry.sha256)
    }
    $script:PayloadManifest = $manifest
    return ([string]$hostEntries[0].sha256).ToUpperInvariant()
}

function Get-ZipEntrySha256
{
    param(
        [Parameter(Mandatory = $true)]
        [IO.Compression.ZipArchive]$Archive,

        [Parameter(Mandatory = $true)]
        [string]$EntryName
    )

    $entry = $Archive.GetEntry($EntryName)
    if ($null -eq $entry)
    {
        throw "MSIX entry is missing: $EntryName"
    }
    $stream = $entry.Open()
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try
    {
        return ([BitConverter]::ToString($sha256.ComputeHash($stream))).Replace('-', '')
    }
    finally
    {
        $sha256.Dispose()
        $stream.Dispose()
    }
}

function Get-CertificateSha256
{
    param(
        [Parameter(Mandatory = $true)]
        [Security.Cryptography.X509Certificates.X509Certificate2]$Certificate
    )

    $sha256 = [Security.Cryptography.SHA256]::Create()
    try
    {
        return ([BitConverter]::ToString(
            $sha256.ComputeHash($Certificate.RawData))).Replace('-', '')
    }
    finally
    {
        $sha256.Dispose()
    }
}

function Get-CertificateStoreSnapshot
{
    $entries = New-Object Collections.Generic.List[string]
    foreach ($storeName in @('My', 'TrustedPeople'))
    {
        foreach ($certificate in @(Get-ChildItem -Path "Cert:\LocalMachine\$storeName"))
        {
            $thumbprint = ([string]$certificate.Thumbprint).ToUpperInvariant()
            if ($thumbprint -notmatch '^[0-9A-F]{40}$')
            {
                continue
            }
            $certificateSha256 = Get-CertificateSha256 -Certificate $certificate
            $entries.Add("${thumbprint}:$certificateSha256")
        }
    }
    return Join-Ledger -Values @($entries) -Separator Pipe
}

function Test-CertificateStoreSnapshotContains
{
    param(
        [AllowNull()]
        [string]$Snapshot,

        [Parameter(Mandatory = $true)]
        [string]$Thumbprint,

        [Parameter(Mandatory = $true)]
        [string]$CertificateSha256
    )

    $normalizedThumbprint = $Thumbprint.ToUpperInvariant()
    $normalizedSha256 = $CertificateSha256.ToUpperInvariant()
    return (Split-Ledger -Value $Snapshot -Separator Pipe) -contains `
        "${normalizedThumbprint}:$normalizedSha256"
}

function Get-CertificateByThumbprint
{
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('My', 'TrustedPeople')]
        [string]$StoreName,

        [Parameter(Mandatory = $true)]
        [string]$Thumbprint
    )

    $normalizedThumbprint = $Thumbprint.ToUpperInvariant()
    return Get-ChildItem -Path "Cert:\LocalMachine\$StoreName" |
        Where-Object { ([string]$_.Thumbprint).ToUpperInvariant() -eq $normalizedThumbprint } |
        Select-Object -First 1
}

function Assert-ReplacementHostIntegrity
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$CurrentHostSha256,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedReplacementHostSha256,

        [Parameter(Mandatory = $true)]
        [string]$ArchivedHostSha256,

        [Parameter(Mandatory = $true)]
        [string]$CommittedHostSha256
    )

    if ($CurrentHostSha256 -ne $ExpectedReplacementHostSha256)
    {
        throw 'The replacement Host does not match the validated installer payload.'
    }
    if ($ArchivedHostSha256 -ne $CommittedHostSha256)
    {
        throw 'Protected identity state does not match the archived Host.'
    }
}

function Assert-IdentityIntegrityMaterial
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [Parameter(Mandatory = $true)]
        [string]$PackagePath,

        [string]$ExpectedReplacementHostSha256 = '',

        [string]$ReplacementHostPath = ''
    )

    foreach ($propertyName in @(
        'hostFile',
        'hostSha256',
        'packageSha256',
        'certificateSha256'))
    {
        if ($null -eq $State.PSObject.Properties[$propertyName])
        {
            throw "Protected identity state is missing: $propertyName"
        }
    }
    if ([string]$State.hostFile -ne 'ba-click-fx-desktop.exe' -or
        [IO.Path]::GetFileName([string]$State.hostFile) -ne [string]$State.hostFile)
    {
        throw 'Protected identity state has an invalid Host file name.'
    }
    foreach ($hashProperty in @('hostSha256', 'packageSha256', 'certificateSha256'))
    {
        if ([string]$State.$hashProperty -notmatch '^[0-9A-Fa-f]{64}$')
        {
            throw "Protected identity state has an invalid hash: $hashProperty"
        }
    }

    if (-not (Test-Path -LiteralPath $PackagePath -PathType Leaf) -or
        (Get-FileHash -LiteralPath $PackagePath -Algorithm SHA256).Hash -ne
            [string]$State.packageSha256)
    {
        throw 'Protected identity state does not match the signed package.'
    }

    $hostPath = Join-Path $InstallRoot ([string]$State.hostFile)
    if (-not (Test-Path -LiteralPath $hostPath -PathType Leaf))
    {
        if (-not [string]::IsNullOrWhiteSpace($ExpectedReplacementHostSha256))
        {
            throw 'The replacement Host does not match the validated installer payload.'
        }
        throw 'Protected identity state does not match the installed Host.'
    }
    $currentHostSha256 =
        (Get-FileHash -LiteralPath $hostPath -Algorithm SHA256).Hash
    if (-not [string]::IsNullOrWhiteSpace($ExpectedReplacementHostSha256))
    {
        # During Prepare the new Host is still in protected staging. Keep the
        # old live Host bound to the committed state and validate the staged
        # replacement independently against the payload manifest.
        if ($ExpectedReplacementHostSha256 -notmatch '^[0-9A-Fa-f]{64}$')
        {
            throw 'The replacement Host does not match the validated installer payload.'
        }
        $archive = [IO.Compression.ZipFile]::OpenRead($PackagePath)
        try
        {
            $archivedHostHash = Get-ZipEntrySha256 `
                -Archive $archive `
                -EntryName ([string]$State.hostFile)
        }
        finally
        {
            $archive.Dispose()
        }
        if (-not [string]::IsNullOrWhiteSpace($ReplacementHostPath))
        {
            if (-not (Test-Path -LiteralPath $ReplacementHostPath -PathType Leaf))
            {
                throw 'The replacement Host is missing from protected staging.'
            }
            $replacementHostHash =
                (Get-FileHash -LiteralPath $ReplacementHostPath -Algorithm SHA256).Hash
            Assert-ReplacementHostIntegrity `
                -CurrentHostSha256 $replacementHostHash `
                -ExpectedReplacementHostSha256 $ExpectedReplacementHostSha256 `
                -ArchivedHostSha256 $archivedHostHash `
                -CommittedHostSha256 ([string]$State.hostSha256)
            if ($currentHostSha256 -ne [string]$State.hostSha256)
            {
                throw 'Protected identity state does not match the installed Host.'
            }
        }
        else
        {
            Assert-ReplacementHostIntegrity `
                -CurrentHostSha256 $currentHostSha256 `
                -ExpectedReplacementHostSha256 $ExpectedReplacementHostSha256 `
                -ArchivedHostSha256 $archivedHostHash `
                -CommittedHostSha256 ([string]$State.hostSha256)
        }
    }
    else
    {
        if ($currentHostSha256 -ne [string]$State.hostSha256)
        {
            throw 'Protected identity state does not match the installed Host.'
        }
    }

    $certificatePath =
        "Cert:\LocalMachine\TrustedPeople\$([string]$State.certificateThumbprint)"
    if (-not (Test-Path -LiteralPath $certificatePath))
    {
        throw 'Protected identity state certificate is not trusted.'
    }
    $certificate = Get-Item -LiteralPath $certificatePath
    if ((Get-CertificateSha256 -Certificate $certificate) -ne
        [string]$State.certificateSha256)
    {
        throw 'Protected identity state certificate hash mismatch.'
    }
}

function Get-TrustedCertificateByThumbprint
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Thumbprint
    )

    return Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
        Where-Object { $_.Thumbprint -eq $Thumbprint } |
        Select-Object -First 1
}

function Test-ExistingIdentityPackageReusable
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$OldState,

        [Parameter(Mandatory = $true)]
        [object]$Metadata,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [Parameter(Mandatory = $true)]
        [string]$ProductVersion,

        [Parameter(Mandatory = $true)]
        [string]$PackageVersion
    )

    if ($null -eq $OldState -or
        [string]$OldState.productVersion -ne $ProductVersion -or
        [string]$OldState.packageVersion -ne $PackageVersion -or
        [string]$OldState.hostSha256 -ne [string]$Metadata.hostSha256)
    {
        return $null
    }
    $thumbprint = ([string]$OldState.certificateThumbprint).ToUpperInvariant()
    if ($thumbprint -notmatch '^[0-9A-F]{40}$')
    {
        return $null
    }
    $certificate = Get-TrustedCertificateByThumbprint -Thumbprint $thumbprint
    $nowUtc = [DateTime]::UtcNow
    $minimumReusableNotAfterUtc = $nowUtc.AddDays(30)
    if ($null -eq $certificate -or
        $certificate.NotAfter.ToUniversalTime() -le $nowUtc -or
        $certificate.NotAfter.ToUniversalTime() -lt $minimumReusableNotAfterUtc -or
        $certificate.NotBefore.ToUniversalTime() -gt $nowUtc)
    {
        return $null
    }
    if ($null -eq $OldState.PSObject.Properties['certificateSha256'] -or
        (Get-CertificateSha256 -Certificate $certificate) -ne
            [string]$OldState.certificateSha256)
    {
        return $null
    }
    $oldPackageFile = [string]$OldState.packageFile
    if ([IO.Path]::IsPathRooted($oldPackageFile) -or
        $oldPackageFile.Contains('..') -or
        [IO.Path]::GetFileName($oldPackageFile) -ne $oldPackageFile -or
        $oldPackageFile -notmatch '\.msix$')
    {
        return $null
    }
    $oldPackagePath = Join-Path (Join-Path $InstallRoot 'Identity') $oldPackageFile
    if (-not (Test-Path -LiteralPath $oldPackagePath -PathType Leaf) -or
        (Get-FileHash -LiteralPath $oldPackagePath -Algorithm SHA256).Hash -ne
            [string]$OldState.packageSha256)
    {
        return $null
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $oldPackagePath
    if ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid -or
        $null -eq $signature.SignerCertificate -or
        ([string]$signature.SignerCertificate.Thumbprint).ToUpperInvariant() -ne $thumbprint)
    {
        return $null
    }
    return [ordered]@{
        certificate = $certificate
        certificateThumbprint = $thumbprint
        certificateSha256 = [string]$OldState.certificateSha256
        certificateNotAfterUtc = $certificate.NotAfter.ToUniversalTime().ToString('o')
        certificateOwnership = if ($null -ne $OldState.PSObject.Properties['certificateOwnership'])
        {
            [string]$OldState.certificateOwnership
        }
        else
        {
            'unknown'
        }
        packageFile = $oldPackageFile
        packagePath = $oldPackagePath
        packageSha256 = [string]$OldState.packageSha256
    }
}

function Assert-IdentityPayload
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [Parameter(Mandatory = $true)]
        [string]$PendingStatePath,

        [Parameter(Mandatory = $true)]
        [object]$PendingStateSeed
    )

    $identityDirectory = Join-Path $InstallRoot 'Identity'
    $metadataBaseName = "CialloKing.BaClickFxDesktop-$PackageVersion"
    $metadataFile = Get-Item -LiteralPath (
        Join-Path $identityDirectory "$metadataBaseName.identity-template.json") `
        -ErrorAction SilentlyContinue
    if ($null -eq $metadataFile -or $metadataFile.PSIsContainer)
    {
        throw 'Identity package metadata is missing.'
    }
    $metadata = Get-Content -LiteralPath $metadataFile.FullName -Raw | ConvertFrom-Json
    if ([int]$metadata.schema -ne 3 -or
        [string]$metadata.identityMode -ne 'target-machine-self-signed')
    {
        throw 'Identity package metadata has an unsupported schema.'
    }
    $versionMismatch = `
        ([string]$metadata.productVersion -ne $ProductVersion) -or `
        ([string]$metadata.packageVersion -ne $PackageVersion)
    if ($versionMismatch)
    {
        throw 'Identity package metadata version mismatch.'
    }

    $templateFile = [string]$metadata.templateFile
    if ([IO.Path]::IsPathRooted($templateFile) -or
        $templateFile.Contains('..') -or
        [IO.Path]::GetFileName($templateFile) -ne $templateFile -or
        $templateFile -ne "$metadataBaseName.unsigned.msix")
    {
        throw 'Identity package metadata contains an unsafe unsigned template path.'
    }
    if ([string]$metadata.signerFile -ne 'Installer/BAFX.IdentitySigner.exe')
    {
        throw 'Identity package metadata does not name the native signer.'
    }
    $templatePath = Join-Path $identityDirectory $templateFile
    $signerPath = Join-Path $InstallRoot 'Installer\BAFX.IdentitySigner.exe'
    if (-not (Test-Path -LiteralPath $templatePath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $signerPath -PathType Leaf))
    {
        throw 'The unsigned identity template or native signer is missing.'
    }
    if ((Get-FileHash -LiteralPath $templatePath -Algorithm SHA256).Hash -ne
        [string]$metadata.templateSha256)
    {
        throw 'Unsigned identity template hash mismatch.'
    }
    if ((Get-FileHash -LiteralPath $signerPath -Algorithm SHA256).Hash -ne
        [string]$metadata.signerSha256)
    {
        throw 'Native identity signer hash mismatch.'
    }
    $templateSignature = Get-AuthenticodeSignature -LiteralPath $templatePath
    if ($templateSignature.Status -ne [Management.Automation.SignatureStatus]::NotSigned)
    {
        throw 'The identity template must be unsigned before target-machine signing.'
    }

    $reusableIdentity = $null
    if ($null -ne $PendingStateSeed.oldInstallState)
    {
        $reusableIdentity = Test-ExistingIdentityPackageReusable `
            -OldState $PendingStateSeed.oldInstallState `
            -Metadata $metadata `
            -InstallRoot ([string]$PendingStateSeed.oldInstallState.externalLocation) `
            -ProductVersion $ProductVersion `
            -PackageVersion $PackageVersion
    }
    if ($null -ne $reusableIdentity)
    {
        $reusedPackagePath = Join-Path $identityDirectory `
            ([string]$reusableIdentity.packageFile)
        Copy-Item `
            -LiteralPath ([string]$reusableIdentity.packagePath) `
            -Destination $reusedPackagePath `
            -Force
        if ((Get-FileHash -LiteralPath $reusedPackagePath -Algorithm SHA256).Hash -ne
            [string]$reusableIdentity.packageSha256)
        {
            throw 'Reused identity package hash mismatch.'
        }
        $reusedJournal = [ordered]@{}
        $seedEntries = if ($PendingStateSeed -is [Collections.IDictionary])
        {
            $PendingStateSeed.GetEnumerator()
        }
        else
        {
            $PendingStateSeed.PSObject.Properties |
                ForEach-Object { [ordered]@{ Key = $_.Name; Value = $_.Value } }
        }
        foreach ($entry in $seedEntries)
        {
            $reusedJournal[$entry.Key] = $entry.Value
        }
        $reusedJournal.certificatePhase = 'ready'
        $reusedJournal.certificateThumbprint = [string]$reusableIdentity.certificateThumbprint
        $reusedJournal.certificateWasPresent = $true
        $reusedJournal.certificateOwnership = [string]$reusableIdentity.certificateOwnership
        $reusedJournal.certificateSha256 = [string]$reusableIdentity.certificateSha256
        $reusedJournal.certificateNotAfterUtc = [string]$reusableIdentity.certificateNotAfterUtc
        $reusedJournal.packagePath = $reusedPackagePath
        $reusedJournal.packageFile = [string]$reusableIdentity.packageFile
        $reusedJournal.hostFile = 'ba-click-fx-desktop.exe'
        $reusedJournal.hostSha256 = [string]$metadata.hostSha256
        $reusedJournal.packageSha256 = [string]$reusableIdentity.packageSha256
        $oldCertificateOwnership = if (
            $null -ne $PendingStateSeed.oldInstallState.PSObject.Properties['certificateOwnership'])
        {
            [string]$PendingStateSeed.oldInstallState.certificateOwnership
        }
        else
        {
            'unknown'
        }
        $oldOwnedCertificates = Join-Ledger `
            -Values @(Get-ExplicitlyOwnedCertificateThumbprints `
                -State $PendingStateSeed.oldInstallState) `
            -Separator Comma
        $oldOwnedPackages = if (
            $oldCertificateOwnership -eq 'unknown')
        {
            ''
        }
        elseif (
            $null -ne $PendingStateSeed.oldInstallState.PSObject.Properties['ownedPackageFiles'])
        {
            [string]$PendingStateSeed.oldInstallState.ownedPackageFiles
        }
        else
        {
            [string]$PendingStateSeed.oldInstallState.packageFile
        }
        $reusedJournal.ownedCertificateThumbprints = $oldOwnedCertificates
        $reusedJournal.ownedPackageFiles = Join-Ledger `
            -Values @(
                (Split-Ledger `
                    -Value $oldOwnedPackages `
                    -Separator Pipe),
                [string]$reusableIdentity.packageFile) `
            -Separator Pipe
        Write-ProtectedJson `
            -Path $PendingStatePath `
            -Value $reusedJournal `
            -ReadSid ([string]$PendingStateSeed.userSid)
        return [ordered]@{
            metadata = $metadata
            packagePath = $reusedPackagePath
            packageFile = [IO.Path]::GetFileName($reusedPackagePath)
            certificateThumbprint = [string]$reusableIdentity.certificateThumbprint
            certificateWasPresent = $true
            reusedCertificate = $true
        }
    }

    $certificate = $null
    $certificateWasPresent = $false
    $certificatePrivateKeyRemoved = $false
    $trustedCertificateEntryCreated = $false
    $signedPackagePath = $null
    $identityFailureRecord = $null
    $identityFailureStep = ''
    $publicCertificatePath = Join-Path (
        [IO.Path]::GetTempPath()) ('bafx-identity-' + [Guid]::NewGuid().ToString('N') + '.cer')
    $certificateStoreSnapshot = Get-CertificateStoreSnapshot

    try
    {
        $certificateSanUri =
            "urn:bafx:installer:$([string]$PendingStateSeed.transactionId)"
        $creatingJournal = [ordered]@{}
        $seedEntries = if ($PendingStateSeed -is [Collections.IDictionary])
        {
            $PendingStateSeed.GetEnumerator()
        }
        else
        {
            $PendingStateSeed.PSObject.Properties |
                ForEach-Object { [ordered]@{ Key = $_.Name; Value = $_.Value } }
        }
        foreach ($entry in $seedEntries)
        {
            $creatingJournal[$entry.Key] = $entry.Value
        }
        $creatingJournal.certificatePhase = 'creating'
        $creatingJournal.certificateSanUri = $certificateSanUri
        $creatingJournal.certificatePreexisting = $certificateStoreSnapshot
        $creatingJournal.certificateThumbprint = ''
        Write-ProtectedJson `
            -Path $PendingStatePath `
            -Value $creatingJournal `
            -ReadSid ([string]$PendingStateSeed.userSid)
        $script:InstallerStep = 'create-signing-certificate'
        $certificate = New-SelfSignedCertificate `
            -Type CodeSigningCert `
            -Subject ([string]$metadata.publisher) `
            -CertStoreLocation 'Cert:\LocalMachine\My' `
            -KeyAlgorithm RSA `
            -KeyLength 2048 `
            -HashAlgorithm SHA256 `
            -KeyExportPolicy NonExportable `
            -TextExtension @("2.5.29.17={text}URI=$certificateSanUri") `
            -NotAfter (Get-Date).AddYears(2)
        if ($null -eq $certificate -or
            [string]$certificate.Subject -ne [string]$metadata.publisher)
        {
            throw 'Target-machine signing certificate creation returned an unexpected certificate.'
        }
        $certificateThumbprint = ([string]$certificate.Thumbprint).ToUpperInvariant()
        $certificateSha256 = Get-CertificateSha256 -Certificate $certificate
        $certificateNotAfterUtc =
            $certificate.NotAfter.ToUniversalTime().ToString('o')

        # Record the certificate identity while its private key still exists.
        # This marker is what recovery uses if the process dies before the
        # public certificate import or package signing completes.
        $creatingJournal.certificateThumbprint = $certificateThumbprint
        $creatingJournal.certificateSha256 = $certificateSha256
        $creatingJournal.certificateNotAfterUtc = $certificateNotAfterUtc
        $creatingJournal.certificateWasPresent = $false
        $creatingJournal.certificateOwnership = 'unknown'
        $creatingJournal.ownedCertificateThumbprints = ''
        $creatingJournal.ownedPackageFiles = ''
        Write-ProtectedJson `
            -Path $PendingStatePath `
            -Value $creatingJournal `
            -ReadSid ([string]$PendingStateSeed.userSid)

        $journal = $creatingJournal
        $existingPrivateCertificate = Get-CertificateByThumbprint `
            -StoreName 'My' `
            -Thumbprint $certificateThumbprint
        $existingTrustedCertificate = Get-CertificateByThumbprint `
            -StoreName 'TrustedPeople' `
            -Thumbprint $certificateThumbprint
        foreach ($existingCertificate in @(
                $existingPrivateCertificate,
                $existingTrustedCertificate) |
            Where-Object { $null -ne $_ })
        {
            $existingCertificateSha256 = Get-CertificateSha256 `
                -Certificate $existingCertificate
            if ($existingCertificateSha256 -ne $certificateSha256)
            {
                throw 'A different certificate already uses the generated thumbprint.'
            }
        }
        $certificateWasPresent = Test-CertificateStoreSnapshotContains `
            -Snapshot $certificateStoreSnapshot `
            -Thumbprint $certificateThumbprint `
            -CertificateSha256 $certificateSha256
        $certificateOwnership = if ($certificateWasPresent)
        {
            'preexisting'
        }
        else
        {
            'installer-owned'
        }
        $journal.certificatePhase = 'ready'
        $journal.certificateSanUri = $certificateSanUri
        $journal.certificateThumbprint = $certificateThumbprint
        $journal.certificateWasPresent = $certificateWasPresent
        $journal.certificateOwnership = $certificateOwnership
        $journal.certificateNotAfterUtc = $certificateNotAfterUtc
        $journal.packagePath = Join-Path $identityDirectory (
            "$metadataBaseName-$certificateThumbprint.msix")
        $journal.packageFile = [IO.Path]::GetFileName([string]$journal.packagePath)
        $ownedCertificateThumbprints = if ($certificateWasPresent)
        {
            @()
        }
        else
        {
            @($certificateThumbprint)
        }
        $ownedPackageFiles = @([string]$journal.packageFile)
        if ($null -ne $PendingStateSeed.oldInstallState)
        {
            $oldState = $PendingStateSeed.oldInstallState
            $oldCertificateOwnership = if (
                $null -ne $oldState.PSObject.Properties['certificateOwnership'])
            {
                [string]$oldState.certificateOwnership
            }
            else
            {
                'unknown'
            }
            $oldCertificateLedger = Join-Ledger `
                -Values @(Get-ExplicitlyOwnedCertificateThumbprints `
                    -State $oldState) `
                -Separator Comma
            $oldPackageLedger = if (
                $oldCertificateOwnership -eq 'unknown')
            {
                ''
            }
            elseif (
                $null -ne $oldState.PSObject.Properties['ownedPackageFiles'])
            {
                [string]$oldState.ownedPackageFiles
            }
            else
            {
                [string]$oldState.packageFile
            }
            $ownedCertificateThumbprints += Split-Ledger `
                -Value $oldCertificateLedger `
                -Separator Comma
            $ownedPackageFiles += Split-Ledger `
                -Value $oldPackageLedger `
                -Separator Pipe
        }
        $journal.ownedCertificateThumbprints = Join-Ledger `
            -Values @($ownedCertificateThumbprints | Sort-Object -Unique) `
            -Separator Comma
        $journal.ownedPackageFiles = Join-Ledger `
            -Values @($ownedPackageFiles | Sort-Object -Unique) `
            -Separator Pipe
        Write-ProtectedJson `
            -Path $PendingStatePath `
            -Value $journal `
            -ReadSid ([string]$PendingStateSeed.userSid)
        $script:InstallerStep = 'export-signing-certificate'
        Export-Certificate `
            -Cert $certificate `
            -FilePath $publicCertificatePath `
            -Type CERT | Out-Null

        if ($null -eq $existingTrustedCertificate)
        {
            $script:InstallerStep = 'trust-signing-certificate'
            $imported = Import-Certificate `
                -FilePath $publicCertificatePath `
                -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople'
            if ([string]$imported.Thumbprint -ne $certificateThumbprint)
            {
                throw 'Imported target-machine certificate thumbprint mismatch.'
            }
            if ((Get-CertificateSha256 -Certificate $imported) -ne $certificateSha256)
            {
                throw 'Imported target-machine certificate hash mismatch.'
            }
            $trustedCertificateEntryCreated = $true
        }

        $signedPackagePath = [string]$journal.packagePath
        Copy-Item -LiteralPath $templatePath -Destination $signedPackagePath -Force
        $script:InstallerStep = 'sign-identity-package'
        & $signerPath `
            '--package' $signedPackagePath `
            '--thumbprint' $certificateThumbprint `
            '--store-location' 'LocalMachine'
        if ($LASTEXITCODE -ne 0)
        {
            throw "Native identity signer failed with exit code $LASTEXITCODE."
        }

        $script:InstallerStep = 'verify-identity-signature'
        $signature = Get-AuthenticodeSignature -LiteralPath $signedPackagePath
        $signatureInvalid = `
            ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid) -or `
            ($null -eq $signature.SignerCertificate) -or `
            ($signature.SignerCertificate.Thumbprint -ne $certificateThumbprint)
        if ($signatureInvalid)
        {
            throw "Sparse package signature is invalid: $($signature.StatusMessage)"
        }

        $script:InstallerStep = 'verify-signed-identity-payload'
        $archive = [IO.Compression.ZipFile]::OpenRead($signedPackagePath)
        try
        {
            $manifestEntry = $archive.GetEntry('AppxManifest.xml')
            if ($null -eq $manifestEntry)
            {
                throw 'Sparse package manifest is missing.'
            }
            $reader = New-Object IO.StreamReader($manifestEntry.Open())
            try
            {
                [xml]$packageManifest = $reader.ReadToEnd()
            }
            finally
            {
                $reader.Dispose()
            }
            $namespace = New-Object Xml.XmlNamespaceManager($packageManifest.NameTable)
            $namespace.AddNamespace('p', 'http://schemas.microsoft.com/appx/manifest/foundation/windows10')
            $identity = $packageManifest.SelectSingleNode('/p:Package/p:Identity', $namespace)
            $application = $packageManifest.SelectSingleNode('/p:Package/p:Applications/p:Application', $namespace)
            if ($null -eq $identity -or $null -eq $application)
            {
                throw 'Sparse package identity or application is missing.'
            }
            $manifestMismatch = `
                ($identity.Name -ne [string]$metadata.packageName) -or `
                ($identity.Publisher -ne [string]$metadata.publisher) -or `
                ($identity.Version -ne $PackageVersion) -or `
                ($application.Id -ne [string]$metadata.applicationId)
            if ($manifestMismatch)
            {
                throw 'Sparse package manifest does not match installer metadata.'
            }

            $hostPath = Join-Path $InstallRoot 'ba-click-fx-desktop.exe'
            $externalHostHash = (Get-FileHash -LiteralPath $hostPath -Algorithm SHA256).Hash
            $packageHostHash = Get-ZipEntrySha256 `
                -Archive $archive `
                -EntryName 'ba-click-fx-desktop.exe'
            $hostMismatch = `
                ($packageHostHash -ne $externalHostHash) -or `
                ($externalHostHash -ne [string]$metadata.hostSha256)
            if ($hostMismatch)
            {
                throw 'Sparse package Host does not match the external Host.'
            }
        }
        finally
        {
            $archive.Dispose()
        }

        # Persist the values that were validated from the final signed files,
        # not the unsigned release metadata. The protected journal is rewritten
        # only after all three integrity domains agree.
        $journal.hostFile = 'ba-click-fx-desktop.exe'
        $journal.hostSha256 = $externalHostHash
        $journal.packageSha256 =
            (Get-FileHash -LiteralPath $signedPackagePath -Algorithm SHA256).Hash
        $journal.certificateSha256 = Get-CertificateSha256 `
            -Certificate $certificate
        $journal.certificatePhase = 'ready'
        Write-ProtectedJson `
            -Path $PendingStatePath `
            -Value $journal `
            -ReadSid ([string]$PendingStateSeed.userSid)

        # The package is already trusted through its public certificate. The
        # private key must not survive Prepare, because it is not needed after
        # the one package signature has been produced.
        $script:InstallerStep = 'delete-signing-private-key'
        $privateCertificatePath = "Cert:\LocalMachine\My\$certificateThumbprint"
        if (-not $certificateWasPresent)
        {
            Remove-Item `
                -LiteralPath $privateCertificatePath `
                -DeleteKey `
                -Force
            $certificatePrivateKeyRemoved = $true
            if (Test-Path -LiteralPath $privateCertificatePath)
            {
                throw 'Target-machine private signing certificate remains after cleanup.'
            }
        }

        return [ordered]@{
            metadata = $metadata
            packagePath = $signedPackagePath
            packageFile = [IO.Path]::GetFileName($signedPackagePath)
            certificateThumbprint = $certificateThumbprint
            certificateWasPresent = $certificateWasPresent
        }
    }
    catch
    {
        $identityFailureRecord = $_
        $identityFailureStep = $script:InstallerStep
        try
        {
            if ($null -ne $signedPackagePath -and
                (Test-Path -LiteralPath $signedPackagePath -PathType Leaf))
            {
                Remove-Item -LiteralPath $signedPackagePath -Force
            }
        }
        catch
        {
            Add-InstallerRelatedFailure `
                -ErrorRecord $_ `
                -Step 'cleanup-failed-identity-package'
        }
        try
        {
            if ($null -ne $certificate)
            {
                $certificateThumbprint = ([string]$certificate.Thumbprint).ToUpperInvariant()
                $privateCertificatePath = "Cert:\LocalMachine\My\$certificateThumbprint"
                if (-not $certificatePrivateKeyRemoved -and
                    -not $certificateWasPresent -and
                    (Test-Path -LiteralPath $privateCertificatePath))
                {
                    Remove-Item `
                        -LiteralPath $privateCertificatePath `
                        -DeleteKey `
                        -Force
                }
            }
        }
        catch
        {
            Add-InstallerRelatedFailure `
                -ErrorRecord $_ `
                -Step 'cleanup-failed-private-certificate'
        }
        try
        {
            if ($trustedCertificateEntryCreated -and $null -ne $certificate)
            {
                $trusted = Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
                    Where-Object {
                        $_.Thumbprint -eq ([string]$certificate.Thumbprint).ToUpperInvariant()
                    } |
                    Select-Object -First 1
                if ($null -ne $trusted)
                {
                    Remove-Item -LiteralPath $trusted.PSPath -Force
                }
            }
        }
        catch
        {
            Add-InstallerRelatedFailure `
                -ErrorRecord $_ `
                -Step 'cleanup-failed-trusted-certificate'
        }
        $script:InstallerStep = $identityFailureStep
        throw $identityFailureRecord
    }
    finally
    {
        $finalizerFailureRecord = $null
        $finalizerFailureStep = ''
        if ($null -ne $certificate)
        {
            try
            {
                $certificate.Dispose()
            }
            catch
            {
                if ($null -ne $identityFailureRecord)
                {
                    Add-InstallerRelatedFailure `
                        -ErrorRecord $_ `
                        -Step 'dispose-signing-certificate'
                }
                else
                {
                    $finalizerFailureRecord = $_
                    $finalizerFailureStep = 'dispose-signing-certificate'
                }
            }
        }
        try
        {
            if (Test-Path -LiteralPath $publicCertificatePath -PathType Leaf)
            {
                Remove-Item -LiteralPath $publicCertificatePath -Force
            }
        }
        catch
        {
            if ($null -ne $identityFailureRecord -or
                $null -ne $finalizerFailureRecord)
            {
                Add-InstallerRelatedFailure `
                    -ErrorRecord $_ `
                    -Step 'delete-temporary-public-certificate'
            }
            else
            {
                $finalizerFailureRecord = $_
                $finalizerFailureStep = 'delete-temporary-public-certificate'
            }
        }
        if ($null -eq $identityFailureRecord -and
            $null -ne $finalizerFailureRecord)
        {
            $script:InstallerStep = $finalizerFailureStep
            throw $finalizerFailureRecord
        }
    }
}

function Grant-DataDirectoryAccess
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$UserSid
    )

    New-Item -ItemType Directory -Path $Path -Force | Out-Null
    $sid = New-Object Security.Principal.SecurityIdentifier($UserSid)
    $acl = Get-Acl -LiteralPath $Path
    $rule = New-Object Security.AccessControl.FileSystemAccessRule(
        $sid,
        'Modify',
        'ContainerInherit,ObjectInherit',
        'None',
        'Allow')
    $acl.SetAccessRule($rule)
    Set-Acl -LiteralPath $Path -AclObject $acl
}

function Initialize-IdentityConfig
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [Parameter(Mandatory = $true)]
        [string]$DataDirectory
    )

    $configPath = Join-Path $DataDirectory 'BAFX.config.json'
    $hostPath = Join-Path $InstallRoot 'ba-click-fx-desktop.exe'
    $reportName = 'identity-installer-support.txt'
    Push-Location -LiteralPath $InstallRoot
    try
    {
        # GUI-subsystem processes do not reliably update LASTEXITCODE. Wait
        # explicitly so a stale signer exit code cannot mask bootstrap errors.
        $hostProcess = Start-Process `
            -FilePath $hostPath `
            -ArgumentList @("--support-info=$reportName") `
            -WorkingDirectory $InstallRoot `
            -WindowStyle Hidden `
            -Wait `
            -PassThru `
            -ErrorAction Stop
        if ($hostProcess.ExitCode -ne 0)
        {
            throw "Host configuration bootstrap failed with exit code $($hostProcess.ExitCode)."
        }
    }
    finally
    {
        Pop-Location
    }

    foreach ($fileName in @('BAFX.config.json', 'ba-click-fx-desktop-support.log'))
    {
        $rootPath = Join-Path $InstallRoot $fileName
        $destinationPath = Join-Path $DataDirectory $fileName
        if (Test-Path -LiteralPath $rootPath -PathType Leaf)
        {
            if (-not (Test-Path -LiteralPath $destinationPath -PathType Leaf))
            {
                Copy-Item -LiteralPath $rootPath -Destination $destinationPath
            }
            Remove-Item -LiteralPath $rootPath -Force
        }
    }
    if (-not (Test-Path -LiteralPath $configPath -PathType Leaf))
    {
        throw 'Host did not create the identity configuration.'
    }

    foreach ($root in @($InstallRoot, $DataDirectory))
    {
        $reportPath = Join-Path $root $reportName
        if (Test-Path -LiteralPath $reportPath -PathType Leaf)
        {
            Remove-Item -LiteralPath $reportPath -Force
        }
    }
}

function Assert-InstallStateObject
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedUserSid,

        [string]$ExpectedReplacementHostSha256 = '',

        [string]$ReplacementHostPath = '',

        [switch]$SkipPayloadIntegrity
    )

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
        'packageFile'))
    {
        if ($null -eq $State.PSObject.Properties[$propertyName])
        {
            throw "Protected install state is missing: $propertyName"
        }
    }
    $schema = [int]$State.schema
    if ($schema -notin @(1, 2))
    {
        throw 'Protected install state has an unsupported schema.'
    }
    if ([string]$State.packageName -ne 'CialloKing.BaClickFxDesktop')
    {
        throw 'Protected install state belongs to a different package.'
    }
    if ([string]$State.applicationId -ne 'BaClickFxDesktop' -or
        [string]$State.publisher -ne 'CN=BaClickFx.Local')
    {
        throw 'Protected install state has unexpected identity values.'
    }
    if ([string]$State.productVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$')
    {
        throw 'Protected install state has an invalid product version.'
    }
    if ([string]$State.packageVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$')
    {
        throw 'Protected install state has an invalid package version.'
    }
    if ([string]$State.packageFullName -notmatch '^CialloKing\.BaClickFxDesktop_[A-Za-z0-9._-]+$')
    {
        throw 'Protected install state has an invalid package full name.'
    }
    if ([string]$State.packageFamilyName -notmatch '^CialloKing\.BaClickFxDesktop_[A-Za-z0-9-]+$')
    {
        throw 'Protected install state has an invalid package family name.'
    }
    if ([string]$State.certificateThumbprint -notmatch '^[0-9A-Fa-f]{40}$')
    {
        throw 'Protected install state has an invalid certificate thumbprint.'
    }
    if ($State.certificateInstalledBySetup -isnot [bool])
    {
        throw 'Protected install state has an invalid certificate ownership flag.'
    }
    if ($null -eq $State.PSObject.Properties['transactionId'] -or
        [string]$State.transactionId -notmatch '^[0-9a-fA-F]{32}$')
    {
        throw 'Protected install state has an invalid transaction identifier.'
    }
    if ($null -ne $State.PSObject.Properties['stateDigest'] -and
        [string]$State.stateDigest -notmatch '^[0-9A-Fa-f]{64}$')
    {
        throw 'Protected install state has an invalid state digest.'
    }
    if ($null -eq $State.PSObject.Properties['certificateNotAfterUtc'])
    {
        $State | Add-Member -NotePropertyName certificateNotAfterUtc `
            -NotePropertyValue ''
    }
    elseif (-not [string]::IsNullOrWhiteSpace([string]$State.certificateNotAfterUtc))
    {
        $notAfter = [DateTime]::MinValue
        if (-not [DateTime]::TryParse(
                [string]$State.certificateNotAfterUtc,
                [Globalization.CultureInfo]::InvariantCulture,
                [Globalization.DateTimeStyles]::AssumeUniversal,
                [ref]$notAfter))
        {
            throw 'Protected install state has an invalid certificate expiry.'
        }
    }
    if ($null -eq $State.PSObject.Properties['certificateOwnership'])
    {
        # Older states cannot prove whether an existing certificate was created
        # by this installer. Mark them unknown so uninstall never deletes a
        # certificate merely because the legacy boolean was optimistic.
        $State | Add-Member -NotePropertyName certificateOwnership `
            -NotePropertyValue 'unknown'
    }
    if ([string]$State.certificateOwnership -notin @(
            'installer-owned', 'preexisting', 'shared', 'unknown'))
    {
        throw 'Protected install state has an invalid certificate ownership value.'
    }
    if ($null -eq $State.PSObject.Properties['ownedCertificateThumbprints'])
    {
        $ownedCertificateSeed = if (
            [string]$State.certificateOwnership -eq 'installer-owned')
        {
            [string]$State.certificateThumbprint
        }
        else
        {
            # Legacy and pre-existing/shared states cannot prove that the
            # certificate was created by this installer. Keep the ledger empty
            # until a later transaction has explicit ownership evidence.
            ''
        }
        $State | Add-Member -NotePropertyName ownedCertificateThumbprints `
            -NotePropertyValue $ownedCertificateSeed
    }
    if ($null -eq $State.PSObject.Properties['ownedPackageFiles'])
    {
        $State | Add-Member -NotePropertyName ownedPackageFiles `
            -NotePropertyValue ([string]$State.packageFile)
    }
    foreach ($thumbprint in (Split-Ledger `
            -Value $State.ownedCertificateThumbprints `
            -Separator Comma))
    {
        if ($thumbprint -notmatch '^[0-9A-Fa-f]{40}$')
        {
            throw 'Protected install state contains an invalid certificate ledger entry.'
        }
    }
    foreach ($ownedFile in (Split-Ledger `
            -Value $State.ownedPackageFiles `
            -Separator Pipe))
    {
        if ([IO.Path]::IsPathRooted($ownedFile) -or
            $ownedFile.Contains('..') -or
            [IO.Path]::GetFileName($ownedFile) -ne $ownedFile -or
            $ownedFile -notmatch '\.msix$')
        {
            throw 'Protected install state contains an unsafe package ledger entry.'
        }
    }
    if ([string]$State.installedUserSid -notmatch '^S-1-[0-9-]+$')
    {
        throw 'Protected install state has an invalid user SID.'
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedUserSid) -and
        [string]$State.installedUserSid -ne $ExpectedUserSid)
    {
        throw 'Protected install state belongs to a different user.'
    }
    $expectedLocation = [IO.Path]::GetFullPath($InstallRoot)
    if ([IO.Path]::GetFullPath([string]$State.externalLocation) -ne $expectedLocation)
    {
        throw 'Protected install state points to a different external location.'
    }
    $packageFile = [string]$State.packageFile
    if ([IO.Path]::IsPathRooted($packageFile) -or
        $packageFile.Contains('..') -or
        [IO.Path]::GetFileName($packageFile) -ne $packageFile -or
        $packageFile -notmatch '\.msix$')
    {
        throw 'Protected install state has an unsafe package file name.'
    }
    if ($schema -eq 2 -and -not $SkipPayloadIntegrity)
    {
        $replacementHostSha256 = ''
        if (-not [string]::IsNullOrWhiteSpace($ExpectedReplacementHostSha256) -and
            ([string]$State.productVersion -ne $ProductVersion -or
                [string]$State.packageVersion -ne $PackageVersion))
        {
            # Only a real version transition can make the committed Host hash
            # differ from the Inno payload that is already on disk.
            $replacementHostSha256 = $ExpectedReplacementHostSha256
        }
        Assert-IdentityIntegrityMaterial `
            -State $State `
            -InstallRoot $InstallRoot `
            -PackagePath (Join-Path (Join-Path $InstallRoot 'Identity') $packageFile) `
            -ExpectedReplacementHostSha256 $replacementHostSha256 `
            -ReplacementHostPath $ReplacementHostPath
    }
    return $State
}

function Read-OldInstallState
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [Parameter(Mandatory = $true)]
        [string]$UserSid,

        [string]$ExpectedReplacementHostSha256 = '',

        [string]$ReplacementHostPath = '',

        [switch]$SkipPayloadIntegrity
    )

    $path = Join-Path $InstallRoot 'Installer\INSTALL-STATE.json'
    $backupPath = "$path.bak"
    $primaryExists = Test-Path -LiteralPath $path -PathType Leaf
    $backupExists = Test-Path -LiteralPath $backupPath -PathType Leaf
    if (-not $primaryExists -and -not $backupExists)
    {
        return $null
    }
    if (-not $primaryExists -or -not $backupExists)
    {
        throw 'Protected install state primary and backup must be present together.'
    }
    Assert-ProtectedStateAcl -Path $path
    Assert-ProtectedStateAcl -Path $backupPath
    $primary = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    $backup = Get-Content -LiteralPath $backupPath -Raw | ConvertFrom-Json
    Assert-InstallStatePair -Primary $primary -Backup $backup
    return Assert-InstallStateObject `
        -State $primary `
        -InstallRoot $InstallRoot `
        -ExpectedUserSid $UserSid `
        -ExpectedReplacementHostSha256 $ExpectedReplacementHostSha256 `
        -ReplacementHostPath $ReplacementHostPath `
        -SkipPayloadIntegrity:$SkipPayloadIntegrity
}

function Remove-OldCertificate
{
    param(
        [AllowNull()]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$CurrentCertificateThumbprint
    )

    if ($null -eq $State -or
        [string]$State.certificateOwnership -eq 'unknown' -or
        [string]$State.certificateThumbprint -eq $CurrentCertificateThumbprint)
    {
        return
    }
    $oldThumbprint = ([string]$State.certificateThumbprint).ToUpperInvariant()
    if ((Split-Ledger -Value $State.ownedCertificateThumbprints -Separator Comma |
            ForEach-Object { ([string]$_).ToUpperInvariant() }) -notcontains $oldThumbprint)
    {
        return
    }
    if (Test-OtherUserPackageRegistration -State $State)
    {
        Write-Warning "Keeping certificate $($State.certificateThumbprint) while the previous package registration remains."
        return
    }
    Remove-CertificateFromStores `
        -Thumbprint $oldThumbprint `
        -ExpectedSubject ([string]$State.publisher)
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

    # Delete the private key first. If key deletion fails, the public trust
    # entry remains available for recovery instead of being orphaned.
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

function Remove-ObsoleteIdentityArtifacts
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State
    )

    if (Test-OtherUserPackageRegistration -State $State)
    {
        return $State
    }

    $installRoot = [IO.Path]::GetFullPath([string]$State.externalLocation)
    $currentFile = [string]$State.packageFile
    $remainingFiles = New-Object Collections.Generic.List[string]
    foreach ($ownedFile in (Split-Ledger `
            -Value $State.ownedPackageFiles `
            -Separator Pipe))
    {
        if ($ownedFile -eq $currentFile)
        {
            $remainingFiles.Add($ownedFile)
            continue
        }
        $path = Join-Path (Join-Path $installRoot 'Identity') $ownedFile
        if (Test-Path -LiteralPath $path -PathType Leaf)
        {
            Remove-Item -LiteralPath $path -Force
        }
        if (Test-Path -LiteralPath $path -PathType Leaf)
        {
            $remainingFiles.Add($ownedFile)
        }
    }

    $remainingCertificates = New-Object Collections.Generic.List[string]
    $currentThumbprint = ([string]$State.certificateThumbprint).ToUpperInvariant()
    $preserveCertificateLedger = [string]$State.certificateOwnership -eq 'unknown'
    foreach ($thumbprint in (Split-Ledger `
            -Value $State.ownedCertificateThumbprints `
            -Separator Comma))
    {
        $normalizedThumbprint = $thumbprint.ToUpperInvariant()
        if ($preserveCertificateLedger)
        {
            $remainingCertificates.Add($normalizedThumbprint)
            continue
        }
        if ($normalizedThumbprint -eq $currentThumbprint)
        {
            $remainingCertificates.Add($normalizedThumbprint)
            continue
        }
        try
        {
            Remove-CertificateFromStores `
                -Thumbprint $normalizedThumbprint `
                -ExpectedSubject ([string]$State.publisher)
        }
        catch
        {
            # Preserve the ledger when either store could not be cleaned so a
            # later repair can retry without losing ownership evidence.
            $remainingCertificates.Add($normalizedThumbprint)
            throw
        }
    }

    $State.ownedPackageFiles = Join-Ledger `
        -Values @($remainingFiles) `
        -Separator Pipe
    $State.ownedCertificateThumbprints = Join-Ledger `
        -Values @($remainingCertificates) `
        -Separator Comma
    return $State
}

function Test-OtherUserPackageRegistration
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State
    )

    $stateUserSid = if ($null -ne $State.PSObject.Properties['userSid'])
    {
        [string]$State.userSid
    }
    elseif ($null -ne $State.PSObject.Properties['installedUserSid'])
    {
        [string]$State.installedUserSid
    }
    else
    {
        throw 'Protected install state has no user SID for package ownership checks.'
    }
    if ($stateUserSid -notmatch '^S-1-[0-9-]+$')
    {
        throw 'Protected install state has an invalid user SID for package ownership checks.'
    }

    $packages = @(
        Get-AppxPackage -AllUsers -Name ([string]$State.packageName) -ErrorAction Stop
    )
    foreach ($package in $packages)
    {
        if ([string]$package.PackageFullName -ne [string]$State.packageFullName)
        {
            return $true
        }
        $usersProperty = $package.PSObject.Properties['PackageUserInformation']
        if ($null -eq $usersProperty)
        {
            # The package object cannot prove that the current user is the only
            # owner, so retain shared artifacts rather than deleting a signer
            # still needed by another profile.
            return $true
        }
        $userInformation = @($usersProperty.Value)
        if ($userInformation.Count -eq 0)
        {
            return $true
        }
        foreach ($user in $userInformation)
        {
            $installState = if ($null -ne $user.PSObject.Properties['InstallState'])
            {
                [string]$user.InstallState
            }
            else
            {
                ''
            }
            # PowerShell can expose InstallState as either the enum name or its
            # numeric value. Match the complete value so "NotInstalled" is not
            # accidentally treated as an installed registration.
            if ([string]$installState -notmatch '^(Installed|1)$')
            {
                continue
            }
            $userSid = if ($null -ne $user.PSObject.Properties['UserSecurityId'])
            {
                $value = $user.UserSecurityId
                if ($null -ne $value.PSObject.Properties['Value'])
                {
                    [string]$value.Value
                }
                else
                {
                    [string]$value
                }
            }
            else
            {
                ''
            }
            if ([string]$userSid -ne $stateUserSid)
            {
                return $true
            }
        }
    }
    return $false
}

function Complete-CommittedPendingTransaction
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [Parameter(Mandatory = $true)]
        [string]$PendingPath
    )

    $committedState = Read-OldInstallState `
        -InstallRoot $InstallRoot `
        -UserSid ([string]$State.userSid)
    if ($null -eq $committedState -or
        [string]$committedState.transactionId -ne [string]$State.transactionId)
    {
        throw 'The committed install state does not match the pending transaction.'
    }
    $cleanedState = Remove-ObsoleteIdentityArtifacts -State $committedState
    Write-ProtectedInstallState `
        -Path (Join-Path $InstallRoot 'Installer\INSTALL-STATE.json') `
        -Value $cleanedState `
        -ReadSid ([string]$State.userSid)
    Remove-Item -LiteralPath $PendingPath -Force
}

function Assert-PendingStateObject
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [switch]$RequireIntegrity,

        [string]$PayloadDirectory = ''
    )

    foreach ($propertyName in @(
        'schema',
        'stateKind',
        'userSid',
        'packageName',
        'applicationId',
        'publisher',
        'productVersion',
        'packageVersion',
        'transactionId',
        'preexistingPackageFullNames',
        'oldInstallState'))
    {
        if ($null -eq $State.PSObject.Properties[$propertyName])
        {
            throw "Protected pending state is missing: $propertyName"
        }
    }
    $schema = [int]$State.schema
    if ($schema -notin @(1, 2) -or
        [string]$State.stateKind -ne 'prepare')
    {
        throw 'Protected pending state has an unsupported schema.'
    }
    if ([string]$State.userSid -notmatch '^S-1-[0-9-]+$')
    {
        throw 'Protected pending state has an invalid user SID.'
    }
    if ([string]$State.packageName -ne 'CialloKing.BaClickFxDesktop' -or
        [string]$State.applicationId -ne 'BaClickFxDesktop' -or
        [string]$State.publisher -ne 'CN=BaClickFx.Local' -or
        [string]$State.productVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$' -or
        [string]$State.packageVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$')
    {
        throw 'Protected pending state contains invalid identity data.'
    }
    if ([string]$State.transactionId -notmatch '^[0-9a-fA-F]{32}$')
    {
        throw 'Protected pending state has an invalid transaction identifier.'
    }
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
    if ($null -ne $State.PSObject.Properties['stateDigest'] -and
        [string]$State.stateDigest -notmatch '^[0-9A-Fa-f]{64}$')
    {
        throw 'Protected pending state has an invalid state digest.'
    }
    if ($null -ne $State.PSObject.Properties['commitState'] -and
        [string]$State.commitState -notin @('prepared', 'files-committing', 'files-committed', 'committed'))
    {
        throw 'Protected pending state has an invalid commit state.'
    }
    if ($null -ne $State.PSObject.Properties['certificatePhase'] -and
        [string]$State.certificatePhase -notin @('creating', 'ready', 'cleaned'))
    {
        throw 'Protected pending state has an invalid certificate phase.'
    }
    if ($null -ne $State.PSObject.Properties['stateDigest'] -and
        (Get-StateDigest -Value $State) -ne [string]$State.stateDigest)
    {
        throw 'Protected pending state digest does not match its content.'
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
        if ($null -eq $State.PSObject.Properties['certificateSanUri'] -or
            [string]$State.certificateSanUri -notmatch
                '^urn:bafx:installer:[0-9a-fA-F]{32}$')
        {
            throw 'Protected pending state has an invalid certificate creation marker.'
        }
        if ($RequireIntegrity)
        {
            throw 'A certificate-creating transaction cannot be used as committed payload.'
        }
        return $State
    }
    foreach ($propertyName in @(
        'packagePath',
        'packageFile',
        'certificateThumbprint',
        'certificateWasPresent',
        'ownedCertificateThumbprints',
        'ownedPackageFiles'))
    {
        if ($null -eq $State.PSObject.Properties[$propertyName])
        {
            throw "Protected pending state is missing: $propertyName"
        }
    }
    if ([string]$State.certificateThumbprint -notmatch '^[0-9A-Fa-f]{40}$' -or
        $State.certificateWasPresent -isnot [bool])
    {
        throw 'Protected pending state has invalid certificate data.'
    }
    foreach ($thumbprint in (Split-Ledger `
            -Value $State.ownedCertificateThumbprints `
            -Separator Comma))
    {
        if ($thumbprint -notmatch '^[0-9A-Fa-f]{40}$')
        {
            throw 'Protected pending state contains an invalid certificate ledger entry.'
        }
    }
    foreach ($ownedFile in (Split-Ledger `
            -Value $State.ownedPackageFiles `
            -Separator Pipe))
    {
        if ([IO.Path]::IsPathRooted($ownedFile) -or
            $ownedFile.Contains('..') -or
            [IO.Path]::GetFileName($ownedFile) -ne $ownedFile -or
            $ownedFile -notmatch '\.msix$')
        {
            throw 'Protected pending state contains an unsafe package ledger entry.'
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
    $packagePath = [IO.Path]::GetFullPath([string]$State.packagePath)
    $expectedPackagePath = [IO.Path]::GetFullPath(
        (Join-Path (Join-Path $InstallRoot 'Identity') $packageFile))
    $packagePathMatches = $packagePath -eq $expectedPackagePath
    if (-not [string]::IsNullOrWhiteSpace($PayloadDirectory))
    {
        $payloadPackagePath = [IO.Path]::GetFullPath(
            (Join-Path (Join-Path $PayloadDirectory 'Identity') $packageFile))
        $packagePathMatches = $packagePathMatches -or
            $packagePath -eq $payloadPackagePath
    }
    if (-not $packagePathMatches)
    {
        throw 'Protected pending state points to a different package file.'
    }
    if ($RequireIntegrity)
    {
        $integrityRoot = $InstallRoot
        if (-not $packagePath.StartsWith(
                ([IO.Path]::GetFullPath($InstallRoot).TrimEnd('\') + '\'),
                [StringComparison]::OrdinalIgnoreCase) -and
            -not [string]::IsNullOrWhiteSpace($PayloadDirectory))
        {
            $integrityRoot = $PayloadDirectory
        }
        Assert-IdentityIntegrityMaterial `
            -State $State `
            -InstallRoot $integrityRoot `
            -PackagePath $packagePath
    }
    foreach ($fullName in @($State.preexistingPackageFullNames))
    {
        if ([string]$fullName -notmatch '^CialloKing\.BaClickFxDesktop_[A-Za-z0-9._-]+$')
        {
            throw 'Protected pending state contains an invalid previous package name.'
        }
    }
    return $State
}

function Remove-NewPackageRegistrations
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State
    )

    $preexisting = @($State.preexistingPackageFullNames)
    $candidates = @(
        Get-AppxPackage `
            -User ([string]$State.userSid) `
            -Name ([string]$State.packageName) `
            -ErrorAction Stop |
            Where-Object { $preexisting -notcontains [string]$_.PackageFullName }
    )
    foreach ($candidate in $candidates)
    {
        Remove-AppxPackage `
            -Package $candidate.PackageFullName `
            -User ([string]$State.userSid) `
            -ErrorAction Stop
    }
    for ($attempt = 0; $attempt -lt 10; ++$attempt)
    {
        $remaining = @(
            Get-AppxPackage `
                -User ([string]$State.userSid) `
                -Name ([string]$State.packageName) `
                -ErrorAction Stop |
                Where-Object { $preexisting -notcontains [string]$_.PackageFullName }
        )
        if ($remaining.Count -eq 0)
        {
            return
        }
        Start-Sleep -Milliseconds 200
    }
    throw 'A newly registered package remains after rollback.'
}

function Remove-PreparedCertificateIfUnused
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State
    )

    if ($null -eq $State.PSObject.Properties['certificateWasPresent'] -or
        [bool]$State.certificateWasPresent -or
        $null -eq $State.PSObject.Properties['certificateOwnership'] -or
        [string]$State.certificateOwnership -ne 'installer-owned' -or
        $null -eq $State.PSObject.Properties['certificateThumbprint'] -or
        [string]$State.certificateThumbprint -notmatch '^[0-9A-Fa-f]{40}$')
    {
        return
    }
    $preexistingFullNames = @($State.preexistingPackageFullNames)
    $sameVersionPackages = @(
        Get-AppxPackage -AllUsers -Name ([string]$State.packageName) -ErrorAction Stop |
            Where-Object {
                [string]$_.Version -eq [string]$State.packageVersion -and
                $preexistingFullNames -notcontains [string]$_.PackageFullName
            }
    )
    if ($sameVersionPackages.Count -gt 0)
    {
        throw 'Refusing to remove the prepared certificate while its package version remains registered.'
    }
    Remove-CertificateFromStores `
        -Thumbprint ([string]$State.certificateThumbprint) `
        -ExpectedSubject ([string]$State.publisher)
}

function Test-CertificateSanUri
{
    param(
        [Parameter(Mandatory = $true)]
        [Security.Cryptography.X509Certificates.X509Certificate2]$Certificate,

        [Parameter(Mandatory = $true)]
        [string]$SanUri
    )

    foreach ($extension in $Certificate.Extensions |
        Where-Object { $_.Oid.Value -eq '2.5.29.17' })
    {
        if ($extension.Format($true).Contains($SanUri) -or
            $extension.Format($false).Contains($SanUri))
        {
            return $true
        }
    }
    return $false
}

function Recover-CreatingCertificate
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State
    )

    if ($null -eq $State.PSObject.Properties['certificatePhase'] -or
        [string]$State.certificatePhase -ne 'creating' -or
        [string]$State.transactionId -notmatch '^[0-9a-fA-F]{32}$')
    {
        return
    }
    $sanUri = if ($null -ne $State.PSObject.Properties['certificateSanUri'])
    {
        [string]$State.certificateSanUri
    }
    else
    {
        "urn:bafx:installer:$([string]$State.transactionId)"
    }
    if ([string]::IsNullOrWhiteSpace($sanUri))
    {
        return
    }
    $preexistingSnapshot = if ($null -ne $State.PSObject.Properties['certificatePreexisting'])
    {
        [string]$State.certificatePreexisting
    }
    else
    {
        # A legacy creating journal cannot prove that a matching certificate
        # was created by this transaction. Preserve it rather than deleting a
        # user's pre-existing key during recovery.
        return
    }
    $recordedThumbprint = if ($null -ne $State.PSObject.Properties['certificateThumbprint'])
    {
        [string]$State.certificateThumbprint
    }
    else
    {
        ''
    }
    foreach ($storeName in @('My', 'TrustedPeople'))
    {
        $certificates = @(Get-ChildItem -Path "Cert:\LocalMachine\$storeName")
        foreach ($candidate in $certificates)
        {
            if (-not (Test-CertificateSanUri -Certificate $candidate -SanUri $sanUri))
            {
                continue
            }
            if ($candidate.Subject -ne [string]$State.publisher)
            {
                continue
            }
            $candidateSha256 = Get-CertificateSha256 -Certificate $candidate
            if (Test-CertificateStoreSnapshotContains `
                    -Snapshot $preexistingSnapshot `
                    -Thumbprint ([string]$candidate.Thumbprint) `
                    -CertificateSha256 $candidateSha256)
            {
                continue
            }
            if (-not [string]::IsNullOrWhiteSpace($recordedThumbprint) -and
                ([string]$candidate.Thumbprint).ToUpperInvariant() -ne
                    $recordedThumbprint.ToUpperInvariant())
            {
                continue
            }
            $candidatePath = $candidate.PSPath
            if ($storeName -eq 'My')
            {
                Remove-Item -LiteralPath $candidatePath -DeleteKey -Force
                if ($null -ne (Get-ChildItem -Path 'Cert:\LocalMachine\My' |
                        Where-Object { $_.Thumbprint -eq $candidate.Thumbprint } |
                        Select-Object -First 1))
                {
                    throw "The recovery private certificate remains: $($candidate.Thumbprint)"
                }
            }
            else
            {
                Remove-Item -LiteralPath $candidatePath -Force
                if ($null -ne (Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
                        Where-Object { $_.Thumbprint -eq $candidate.Thumbprint } |
                        Select-Object -First 1))
                {
                    throw "The recovery trusted certificate remains: $($candidate.Thumbprint)"
                }
            }
        }
    }
}

function Resolve-InstallerRelativePath
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [string]$RelativePath
    )

    $normalized = $RelativePath.Replace('/', '\')
    if ([IO.Path]::IsPathRooted($normalized) -or
        @($normalized -split '\\' | Where-Object { $_ -eq '..' }).Count -gt 0)
    {
        throw "Installer payload path is unsafe: $RelativePath"
    }
    $resolvedRoot = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $resolved = [IO.Path]::GetFullPath((Join-Path $Root $normalized))
    if (-not $resolved.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase))
    {
        throw "Installer payload path escaped its root: $RelativePath"
    }
    return $resolved
}

function Copy-VerifiedInstallerFile
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourcePath,

        [Parameter(Mandatory = $true)]
        [string]$DestinationPath,

        [Parameter(Mandatory = $true)]
        [Int64]$ExpectedBytes,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedSha256
    )

    if (-not (Test-Path -LiteralPath $SourcePath -PathType Leaf))
    {
        throw "Installer payload file is missing: $SourcePath"
    }
    $destinationDirectory = [IO.Path]::GetDirectoryName($DestinationPath)
    if (-not [string]::IsNullOrWhiteSpace($destinationDirectory))
    {
        New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
    }
    Copy-Item -LiteralPath $SourcePath -Destination $DestinationPath -Force
    Assert-FileHash `
        -Path $DestinationPath `
        -ExpectedBytes $ExpectedBytes `
        -ExpectedSha256 $ExpectedSha256
}

function Save-DataDirectoryRollback
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [Parameter(Mandatory = $true)]
        [string]$RollbackRoot
    )

    $dataDirectory = Join-Path $InstallRoot 'data'
    $dataDirectoryExisted = Test-Path -LiteralPath $dataDirectory -PathType Container
    $dataDirectoryAcl = ''
    if (Test-Path -LiteralPath $dataDirectory)
    {
        if (-not $dataDirectoryExisted)
        {
            throw 'The installed data path is not a directory.'
        }
        $dataItem = Get-Item -LiteralPath $dataDirectory -Force
        if (($dataItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
        {
            throw 'The installed data directory cannot be a reparse point.'
        }
        $dataDirectoryAcl = [string](Get-Acl -LiteralPath $dataDirectory).Sddl
    }

    $dataBackupRoot = Join-Path $RollbackRoot 'data'
    $fileEntries = New-Object Collections.Generic.List[object]
    foreach ($fileName in @('BAFX.config.json', 'ba-click-fx-desktop-support.log'))
    {
        $sourcePath = Join-Path $dataDirectory $fileName
        $existed = Test-Path -LiteralPath $sourcePath -PathType Leaf
        $bytes = [Int64]0
        $sha256 = ''
        $backupRelativePath = ''
        if ($existed)
        {
            $sourceItem = Get-Item -LiteralPath $sourcePath -Force
            if (($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
            {
                throw "The installed data file cannot be a reparse point: $fileName"
            }
            $bytes = [Int64]$sourceItem.Length
            $sha256 = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash
            $backupPath = Join-Path $dataBackupRoot $fileName
            New-Item -ItemType Directory -Path $dataBackupRoot -Force | Out-Null
            Copy-VerifiedInstallerFile `
                -SourcePath $sourcePath `
                -DestinationPath $backupPath `
                -ExpectedBytes $bytes `
                -ExpectedSha256 $sha256
            $backupRelativePath = Join-Path 'data' $fileName
            $backupRelativePath = $backupRelativePath.Replace('\', '/')
        }
        $fileEntries.Add([ordered]@{
                name = $fileName
                existed = $existed
                bytes = $bytes
                sha256 = $sha256
                backupPath = $backupRelativePath
            })
    }

    return [ordered]@{
        dataDirectoryExisted = $dataDirectoryExisted
        dataDirectoryAcl = $dataDirectoryAcl
        dataFiles = @($fileEntries)
    }
}

function Save-PreviousInstallStatePair
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [Parameter(Mandatory = $true)]
        [string]$RollbackRoot
    )

    $primaryPath = Join-Path $InstallRoot 'Installer\INSTALL-STATE.json'
    $backupPath = "$primaryPath.bak"
    $primaryExists = Test-Path -LiteralPath $primaryPath -PathType Leaf
    $backupExists = Test-Path -LiteralPath $backupPath -PathType Leaf
    if ($primaryExists -ne $backupExists)
    {
        throw 'Protected install state primary and backup must be present together.'
    }
    if (-not $primaryExists)
    {
        return [ordered]@{
            previousStatePresent = $false
            previousStatePrimaryBackupPath = ''
            previousStateBackupBackupPath = ''
        }
    }

    $primary = Get-Content -LiteralPath $primaryPath -Raw | ConvertFrom-Json
    $backup = Get-Content -LiteralPath $backupPath -Raw | ConvertFrom-Json
    Assert-InstallStatePair -Primary $primary -Backup $backup

    $stateBackupRoot = Join-Path $RollbackRoot 'state-before'
    New-Item -ItemType Directory -Path $stateBackupRoot -Force | Out-Null
    $primaryBackupPath = Join-Path $stateBackupRoot 'INSTALL-STATE.json'
    $backupBackupPath = Join-Path $stateBackupRoot 'INSTALL-STATE.json.bak'
    Copy-Item -LiteralPath $primaryPath -Destination $primaryBackupPath -Force
    Copy-Item -LiteralPath $backupPath -Destination $backupBackupPath -Force
    Assert-FileHash `
        -Path $primaryBackupPath `
        -ExpectedBytes ([Int64](Get-Item -LiteralPath $primaryPath).Length) `
        -ExpectedSha256 ((Get-FileHash -LiteralPath $primaryPath -Algorithm SHA256).Hash)
    Assert-FileHash `
        -Path $backupBackupPath `
        -ExpectedBytes ([Int64](Get-Item -LiteralPath $backupPath).Length) `
        -ExpectedSha256 ((Get-FileHash -LiteralPath $backupPath -Algorithm SHA256).Hash)
    Set-ProtectedStateAcl -Path $primaryBackupPath -ReadSid ''
    Set-ProtectedStateAcl -Path $backupBackupPath -ReadSid ''
    return [ordered]@{
        previousStatePresent = $true
        previousStatePrimaryBackupPath = 'state-before/INSTALL-STATE.json'
        previousStateBackupBackupPath = 'state-before/INSTALL-STATE.json.bak'
    }
}

function New-PayloadRollbackManifest
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [Parameter(Mandatory = $true)]
        [string]$PayloadRoot
    )

    $rollbackRoot = Join-Path $InstallRoot ('.rollback\' + [string]$State.transactionId)
    $rollbackManifestPath = Join-Path $rollbackRoot 'ROLLBACK-MANIFEST.json'
    if (Test-Path -LiteralPath $rollbackManifestPath -PathType Leaf)
    {
        return Get-Content -LiteralPath $rollbackManifestPath -Raw | ConvertFrom-Json
    }
    New-Item -ItemType Directory -Path $rollbackRoot -Force | Out-Null
    $previousState = Save-PreviousInstallStatePair `
        -State $State `
        -InstallRoot $InstallRoot `
        -RollbackRoot $rollbackRoot
    $dataRollback = Save-DataDirectoryRollback `
        -InstallRoot $InstallRoot `
        -RollbackRoot $rollbackRoot
    $entries = New-Object Collections.Generic.List[object]
    foreach ($entry in @($script:PayloadManifest.files))
    {
        $relativePath = [string]$entry.path
        $sourcePath = Resolve-InstallerRelativePath `
            -Root $PayloadRoot `
            -RelativePath $relativePath
        $livePath = Resolve-InstallerRelativePath `
            -Root $InstallRoot `
            -RelativePath $relativePath
        $backupRelativePath = Join-Path 'files' $relativePath
        $backupPath = Resolve-InstallerRelativePath `
            -Root $rollbackRoot `
            -RelativePath $backupRelativePath
        $existed = Test-Path -LiteralPath $livePath -PathType Leaf
        if ($existed)
        {
            New-Item -ItemType Directory `
                -Path ([IO.Path]::GetDirectoryName($backupPath)) `
                -Force | Out-Null
            Copy-Item -LiteralPath $livePath -Destination $backupPath -Force
            Assert-FileHash `
                -Path $backupPath `
                -ExpectedBytes ([Int64](Get-Item -LiteralPath $livePath).Length) `
                -ExpectedSha256 ((Get-FileHash -LiteralPath $livePath -Algorithm SHA256).Hash)
        }
        $entries.Add([ordered]@{
                path = $relativePath
                existed = $existed
                bytes = if ($existed) { [Int64](Get-Item -LiteralPath $livePath).Length } else { 0 }
                sha256 = if ($existed) { (Get-FileHash -LiteralPath $livePath -Algorithm SHA256).Hash } else { '' }
                backupPath = if ($existed) { $backupRelativePath.Replace('\', '/') } else { '' }
            })
    }

    $oldPackageBackupPath = ''
    $oldPackageBytes = [Int64]0
    $oldPackageSha256 = ''
    if ($null -ne $State.oldInstallState)
    {
        $oldPackageFile = [string]$State.oldInstallState.packageFile
        if (-not [string]::IsNullOrWhiteSpace($oldPackageFile))
        {
            $oldPackagePath = Resolve-InstallerRelativePath `
                -Root (Join-Path $InstallRoot 'Identity') `
                -RelativePath $oldPackageFile
            if (-not (Test-Path -LiteralPath $oldPackagePath -PathType Leaf))
            {
                throw 'The previous package file is unavailable for rollback.'
            }
            $oldPackageBytes = [Int64](Get-Item -LiteralPath $oldPackagePath).Length
            $oldPackageSha256 =
                (Get-FileHash -LiteralPath $oldPackagePath -Algorithm SHA256).Hash
            if ($null -ne $State.oldInstallState.PSObject.Properties['packageSha256'] -and
                [string]$State.oldInstallState.packageSha256 -notmatch '^[0-9A-Fa-f]{64}$')
            {
                throw 'The previous install state has an invalid package hash.'
            }
            if ($null -ne $State.oldInstallState.PSObject.Properties['packageSha256'] -and
                $oldPackageSha256 -ne [string]$State.oldInstallState.packageSha256)
            {
                throw 'The previous package does not match its protected install state.'
            }
            $oldPackageBackupPath = Join-Path $rollbackRoot ('old\' + $oldPackageFile)
            New-Item -ItemType Directory `
                -Path ([IO.Path]::GetDirectoryName($oldPackageBackupPath)) `
                -Force | Out-Null
            Copy-VerifiedInstallerFile `
                -SourcePath $oldPackagePath `
                -DestinationPath $oldPackageBackupPath `
                -ExpectedBytes $oldPackageBytes `
                -ExpectedSha256 $oldPackageSha256
            $oldPackageBackupPath = Join-Path 'old' $oldPackageFile
        }
    }
    $manifest = [ordered]@{
        schema = 1
        transactionId = [string]$State.transactionId
        files = @($entries)
        oldPackageFile = if ($null -eq $State.oldInstallState) { '' } else { [string]$State.oldInstallState.packageFile }
        oldPackageBackupPath = $oldPackageBackupPath.Replace('\', '/')
        oldPackageBytes = $oldPackageBytes
        oldPackageSha256 = $oldPackageSha256
        dataDirectoryExisted = [bool]$dataRollback.dataDirectoryExisted
        dataDirectoryAcl = [string]$dataRollback.dataDirectoryAcl
        dataFiles = @($dataRollback.dataFiles)
        previousStatePresent = [bool]$previousState.previousStatePresent
        previousStatePrimaryBackupPath = [string]$previousState.previousStatePrimaryBackupPath
        previousStateBackupBackupPath = [string]$previousState.previousStateBackupBackupPath
    }
    Write-ProtectedJson `
        -Path $rollbackManifestPath `
        -Value $manifest `
        -ReadSid ''
    return Get-Content -LiteralPath $rollbackManifestPath -Raw | ConvertFrom-Json
}

function Commit-PayloadFiles
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [Parameter(Mandatory = $true)]
        [string]$PayloadRoot,

        [Parameter(Mandatory = $true)]
        [string]$PendingPath
    )

    $script:InstallerStep = 'validate-commit-payload'
    Assert-PayloadManifest -InstallRoot $PayloadRoot | Out-Null
    $rollbackManifest = New-PayloadRollbackManifest `
        -State $State `
        -InstallRoot $InstallRoot `
        -PayloadRoot $PayloadRoot
    $State.rollbackDirectory = Join-Path $InstallRoot ('.rollback\' + [string]$State.transactionId)
    $State.rollbackManifest = Join-Path ([string]$State.rollbackDirectory) 'ROLLBACK-MANIFEST.json'
    $State.filesCommitted = $true
    $State.commitState = 'files-committing'
    Write-ProtectedJson `
        -Path $PendingPath `
        -Value $State `
        -ReadSid ([string]$State.userSid)
    foreach ($entry in @($script:PayloadManifest.files))
    {
        $relativePath = [string]$entry.path
        $script:InstallerStep = "commit-file-$relativePath"
        Copy-VerifiedInstallerFile `
            -SourcePath (Resolve-InstallerRelativePath -Root $PayloadRoot -RelativePath $relativePath) `
            -DestinationPath (Resolve-InstallerRelativePath -Root $InstallRoot -RelativePath $relativePath) `
            -ExpectedBytes ([Int64]$entry.bytes) `
            -ExpectedSha256 ([string]$entry.sha256)
    }

    $packageFile = [string]$State.packageFile
    $stagedPackagePath = [IO.Path]::GetFullPath([string]$State.packagePath)
    $expectedStagedPackagePath = [IO.Path]::GetFullPath(
        (Join-Path (Join-Path $PayloadRoot 'Identity') $packageFile))
    if ($stagedPackagePath -ne $expectedStagedPackagePath)
    {
        throw 'Prepared package is outside the protected staging directory.'
    }
    $livePackagePath = Join-Path (Join-Path $InstallRoot 'Identity') $packageFile
    $script:InstallerStep = 'commit-signed-identity-package'
    Copy-VerifiedInstallerFile `
        -SourcePath $stagedPackagePath `
        -DestinationPath $livePackagePath `
        -ExpectedBytes ([Int64](Get-Item -LiteralPath $stagedPackagePath).Length) `
        -ExpectedSha256 ([string]$State.packageSha256)

    $State.packagePath = $livePackagePath
    if ($null -eq $State.PSObject.Properties['stagedPackagePath'])
    {
        $State | Add-Member -NotePropertyName stagedPackagePath `
            -NotePropertyValue $stagedPackagePath
    }
    else
    {
        $State.stagedPackagePath = $stagedPackagePath
    }
    $dataDirectory = Join-Path $InstallRoot 'data'
    $script:InstallerStep = 'grant-data-directory-access'
    Grant-DataDirectoryAccess `
        -Path $dataDirectory `
        -UserSid ([string]$State.userSid)
    $script:InstallerStep = 'bootstrap-host-configuration'
    Initialize-IdentityConfig `
        -InstallRoot $InstallRoot `
        -DataDirectory $dataDirectory
    $State.commitState = 'files-committed'
    Write-ProtectedJson `
        -Path $PendingPath `
        -Value $State `
        -ReadSid ([string]$State.userSid)
    $validated = Get-Content -LiteralPath $PendingPath -Raw | ConvertFrom-Json
    Assert-PendingStateObject `
        -State $validated `
        -InstallRoot $InstallRoot `
        -RequireIntegrity
    return $validated
}

function Restore-CommittedPayloadFiles
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot
    )

    $rollbackRoot = Join-Path $InstallRoot ('.rollback\' + [string]$State.transactionId)
    $rollbackManifestPath = Join-Path $rollbackRoot 'ROLLBACK-MANIFEST.json'
    if (-not (Test-Path -LiteralPath $rollbackManifestPath -PathType Leaf))
    {
        throw 'The payload rollback manifest is missing.'
    }
    $rollbackManifest = Get-Content -LiteralPath $rollbackManifestPath -Raw | ConvertFrom-Json
    if ([string]$rollbackManifest.transactionId -ne [string]$State.transactionId)
    {
        throw 'The payload rollback manifest belongs to a different transaction.'
    }
    foreach ($entry in @($rollbackManifest.files | Sort-Object { [string]$_.path } -Descending))
    {
        $livePath = Resolve-InstallerRelativePath `
            -Root $InstallRoot `
            -RelativePath ([string]$entry.path)
        if ([bool]$entry.existed)
        {
            $backupPath = Resolve-InstallerRelativePath `
                -Root $rollbackRoot `
                -RelativePath ([string]$entry.backupPath)
            Copy-VerifiedInstallerFile `
                -SourcePath $backupPath `
                -DestinationPath $livePath `
                -ExpectedBytes ([Int64]$entry.bytes) `
                -ExpectedSha256 ([string]$entry.sha256)
        }
        else
        {
            if (Test-Path -LiteralPath $livePath -PathType Leaf)
            {
                Remove-Item -LiteralPath $livePath -Force
            }
            if (Test-Path -LiteralPath $livePath -PathType Leaf)
            {
                throw "A newly committed file remains after rollback: $($entry.path)"
            }
        }
    }

    if (-not [string]::IsNullOrWhiteSpace([string]$rollbackManifest.oldPackageFile) -and
        -not [string]::IsNullOrWhiteSpace([string]$rollbackManifest.oldPackageBackupPath))
    {
        $oldPackageFile = [string]$rollbackManifest.oldPackageFile
        if ([IO.Path]::IsPathRooted($oldPackageFile) -or
            $oldPackageFile.Contains('..') -or
            [IO.Path]::GetFileName($oldPackageFile) -ne $oldPackageFile -or
            $oldPackageFile -notmatch '\.msix$')
        {
            throw 'The rollback manifest has an unsafe previous package file name.'
        }
        $oldPackagePath = Join-Path (Join-Path $InstallRoot 'Identity') `
            $oldPackageFile
        $oldPackageBackupPath = Resolve-InstallerRelativePath `
            -Root $rollbackRoot `
            -RelativePath ([string]$rollbackManifest.oldPackageBackupPath)
        $oldPackageBytes = 0L
        $oldPackageSha256 = ''
        if ($null -ne $rollbackManifest.PSObject.Properties['oldPackageBytes'])
        {
            $oldPackageBytes = [Int64]$rollbackManifest.oldPackageBytes
        }
        if ($null -ne $rollbackManifest.PSObject.Properties['oldPackageSha256'])
        {
            $oldPackageSha256 = [string]$rollbackManifest.oldPackageSha256
        }
        if ($oldPackageBytes -le 0 -or
            $oldPackageSha256 -notmatch '^[0-9A-Fa-f]{64}$')
        {
            # Older manifests did not carry these fields. The pending state is
            # still authoritative when it contains the protected old hash.
            if ($null -ne $State.oldInstallState -and
                $null -ne $State.oldInstallState.PSObject.Properties['packageSha256'] -and
                [string]$State.oldInstallState.packageSha256 -match '^[0-9A-Fa-f]{64}$')
            {
                $oldPackageSha256 = [string]$State.oldInstallState.packageSha256
                $oldPackageBytes = [Int64](Get-Item -LiteralPath $oldPackageBackupPath).Length
            }
            else
            {
                throw 'The rollback manifest has no verifiable previous package hash.'
            }
        }
        Copy-VerifiedInstallerFile `
            -SourcePath $oldPackageBackupPath `
            -DestinationPath $oldPackagePath `
            -ExpectedBytes $oldPackageBytes `
            -ExpectedSha256 $oldPackageSha256
    }
}

function Restore-DataDirectory
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot
    )

    $rollbackRoot = Join-Path $InstallRoot ('.rollback\' + [string]$State.transactionId)
    $rollbackManifestPath = Join-Path $rollbackRoot 'ROLLBACK-MANIFEST.json'
    if (-not (Test-Path -LiteralPath $rollbackManifestPath -PathType Leaf))
    {
        throw 'The payload rollback manifest is missing.'
    }
    $rollbackManifest = Get-Content -LiteralPath $rollbackManifestPath -Raw | ConvertFrom-Json
    if ($null -eq $rollbackManifest.PSObject.Properties['dataDirectoryExisted'] -or
        $null -eq $rollbackManifest.PSObject.Properties['dataFiles'])
    {
        # Transactions created before data rollback was introduced did not
        # modify the data directory in CommitFiles, so there is nothing safe to
        # restore from their manifest.
        return
    }

    $dataDirectory = Join-Path $InstallRoot 'data'
    $dataDirectoryExisted = [bool]$rollbackManifest.dataDirectoryExisted
    if (-not (Test-Path -LiteralPath $dataDirectory))
    {
        if ($dataDirectoryExisted)
        {
            New-Item -ItemType Directory -Path $dataDirectory -Force | Out-Null
        }
        else
        {
            return
        }
    }
    if (-not (Test-Path -LiteralPath $dataDirectory -PathType Container))
    {
        throw 'The installed data path is not a directory during rollback.'
    }
    $dataItem = Get-Item -LiteralPath $dataDirectory -Force
    if (($dataItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
    {
        throw 'The installed data directory cannot be a reparse point during rollback.'
    }

    foreach ($entry in @($rollbackManifest.dataFiles))
    {
        $fileName = [string]$entry.name
        if ($fileName -notin @('BAFX.config.json', 'ba-click-fx-desktop-support.log'))
        {
            throw 'The data rollback manifest contains an unexpected file.'
        }
        $destinationPath = Join-Path $dataDirectory $fileName
        if ([bool]$entry.existed)
        {
            $backupPath = Resolve-InstallerRelativePath `
                -Root $rollbackRoot `
                -RelativePath ([string]$entry.backupPath)
            Copy-VerifiedInstallerFile `
                -SourcePath $backupPath `
                -DestinationPath $destinationPath `
                -ExpectedBytes ([Int64]$entry.bytes) `
                -ExpectedSha256 ([string]$entry.sha256)
        }
        else
        {
            if (Test-Path -LiteralPath $destinationPath -PathType Leaf)
            {
                Remove-Item -LiteralPath $destinationPath -Force
            }
            if (Test-Path -LiteralPath $destinationPath)
            {
                throw "A newly generated data file remains after rollback: $fileName"
            }
        }
    }

    $savedSddl = [string]$rollbackManifest.dataDirectoryAcl
    if ($dataDirectoryExisted -and -not [string]::IsNullOrWhiteSpace($savedSddl))
    {
        $acl = Get-Acl -LiteralPath $dataDirectory
        $acl.SetSecurityDescriptorSddlForm($savedSddl)
        Set-Acl -LiteralPath $dataDirectory -AclObject $acl
    }
    elseif (-not $dataDirectoryExisted)
    {
        $remaining = @(Get-ChildItem -LiteralPath $dataDirectory -Force)
        if ($remaining.Count -eq 0)
        {
            Remove-Item -LiteralPath $dataDirectory -Force
        }
    }
}

function Restore-PreviousInstallStatePair
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot
    )

    $rollbackRoot = Join-Path $InstallRoot ('.rollback\' + [string]$State.transactionId)
    $rollbackManifestPath = Join-Path $rollbackRoot 'ROLLBACK-MANIFEST.json'
    if (-not (Test-Path -LiteralPath $rollbackManifestPath -PathType Leaf))
    {
        throw 'The payload rollback manifest is missing.'
    }
    $rollbackManifest = Get-Content -LiteralPath $rollbackManifestPath -Raw | ConvertFrom-Json
    if ([string]$rollbackManifest.transactionId -ne [string]$State.transactionId)
    {
        throw 'The payload rollback manifest belongs to a different transaction.'
    }

    $previousStatePresent = if (
        $null -ne $rollbackManifest.PSObject.Properties['previousStatePresent'])
    {
        [bool]$rollbackManifest.previousStatePresent
    }
    else
    {
        $null -ne $State.oldInstallState
    }
    $primaryPath = Join-Path $InstallRoot 'Installer\INSTALL-STATE.json'
    $backupPath = "$primaryPath.bak"
    $hasManifestStateBackups =
        $null -ne $rollbackManifest.PSObject.Properties['previousStatePrimaryBackupPath'] -and
        $null -ne $rollbackManifest.PSObject.Properties['previousStateBackupBackupPath']
    if (-not $hasManifestStateBackups -and $null -ne $State.oldInstallState)
    {
        # Transactions created before the state-backup fields were introduced
        # still carry the complete previous state in their journal. Rebuild a
        # matching pair from that protected object instead of trusting a mixed
        # live pair.
        $legacyState = New-StateWithDigest -Value $State.oldInstallState
        $legacySerialized = ConvertTo-Json -InputObject $legacyState -Depth 12
        $legacyReadSid = if (
            $null -ne $State.oldInstallState.PSObject.Properties['installedUserSid'])
        {
            [string]$State.oldInstallState.installedUserSid
        }
        else
        {
            ''
        }
        $legacyPrimaryTemporaryPath =
            "$primaryPath.$PID.$([Guid]::NewGuid().ToString('N')).restore.tmp"
        $legacyBackupTemporaryPath =
            "$backupPath.$PID.$([Guid]::NewGuid().ToString('N')).restore.tmp"
        try
        {
            Write-FlushedUtf8NoBom `
                -Path $legacyBackupTemporaryPath `
                -Content $legacySerialized
            Replace-ProtectedFile `
                -TemporaryPath $legacyBackupTemporaryPath `
                -DestinationPath $backupPath `
                -ReadSid $legacyReadSid
            Write-FlushedUtf8NoBom `
                -Path $legacyPrimaryTemporaryPath `
                -Content $legacySerialized
            Replace-ProtectedFile `
                -TemporaryPath $legacyPrimaryTemporaryPath `
                -DestinationPath $primaryPath `
                -ReadSid $legacyReadSid
        }
        finally
        {
            foreach ($temporaryPath in @(
                    $legacyPrimaryTemporaryPath,
                    $legacyBackupTemporaryPath))
            {
                if (Test-Path -LiteralPath $temporaryPath -PathType Leaf)
                {
                    Remove-Item -LiteralPath $temporaryPath -Force
                }
            }
        }
        $restoredPrimary = Get-Content -LiteralPath $primaryPath -Raw | ConvertFrom-Json
        $restoredBackup = Get-Content -LiteralPath $backupPath -Raw | ConvertFrom-Json
        Assert-InstallStatePair -Primary $restoredPrimary -Backup $restoredBackup
        return
    }
    if (-not $previousStatePresent)
    {
        foreach ($path in @($primaryPath, $backupPath))
        {
            if (Test-Path -LiteralPath $path -PathType Leaf)
            {
                Remove-Item -LiteralPath $path -Force
            }
            if (Test-Path -LiteralPath $path -PathType Leaf)
            {
                throw "A new install-state file remains after rollback: $path"
            }
        }
        return
    }

    $primaryRelativePath = [string]$rollbackManifest.previousStatePrimaryBackupPath
    $backupRelativePath = [string]$rollbackManifest.previousStateBackupBackupPath
    if ([string]::IsNullOrWhiteSpace($primaryRelativePath) -or
        [string]::IsNullOrWhiteSpace($backupRelativePath))
    {
        throw 'The previous install-state backups are missing.'
    }
    $primaryBackupPath = Resolve-InstallerRelativePath `
        -Root $rollbackRoot `
        -RelativePath $primaryRelativePath
    $backupBackupPath = Resolve-InstallerRelativePath `
        -Root $rollbackRoot `
        -RelativePath $backupRelativePath
    foreach ($path in @($primaryBackupPath, $backupBackupPath))
    {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf))
        {
            throw "The previous install-state backup is missing: $path"
        }
    }

    $readSid = if ($null -ne $State.oldInstallState -and
        $null -ne $State.oldInstallState.PSObject.Properties['installedUserSid'])
    {
        [string]$State.oldInstallState.installedUserSid
    }
    else
    {
        ''
    }
    $temporaryPrimaryPath = "$primaryPath.$PID.$([Guid]::NewGuid().ToString('N')).restore.tmp"
    $temporaryBackupPath = "$backupPath.$PID.$([Guid]::NewGuid().ToString('N')).restore.tmp"
    try
    {
        Copy-Item -LiteralPath $backupBackupPath -Destination $temporaryBackupPath -Force
        Set-ProtectedStateAcl -Path $temporaryBackupPath -ReadSid $readSid
        Replace-ProtectedFile `
            -TemporaryPath $temporaryBackupPath `
            -DestinationPath $backupPath `
            -ReadSid $readSid
        Copy-Item -LiteralPath $primaryBackupPath -Destination $temporaryPrimaryPath -Force
        Set-ProtectedStateAcl -Path $temporaryPrimaryPath -ReadSid $readSid
        Replace-ProtectedFile `
            -TemporaryPath $temporaryPrimaryPath `
            -DestinationPath $primaryPath `
            -ReadSid $readSid
    }
    finally
    {
        foreach ($temporaryPath in @($temporaryPrimaryPath, $temporaryBackupPath))
        {
            if (Test-Path -LiteralPath $temporaryPath -PathType Leaf)
            {
                Remove-Item -LiteralPath $temporaryPath -Force
            }
        }
    }
    $restoredPrimary = Get-Content -LiteralPath $primaryPath -Raw | ConvertFrom-Json
    $restoredBackup = Get-Content -LiteralPath $backupPath -Raw | ConvertFrom-Json
    Assert-InstallStatePair -Primary $restoredPrimary -Backup $restoredBackup
}

function Invoke-PendingRollback
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State
    )

    # AppX registrations belong to the original user token. This elevated
    # phase only restores machine-owned files and the protected state pair;
    # package/certificate cleanup is deliberately deferred until the original
    # user's previous registration has been restored.
    if ($null -ne $State.PSObject.Properties['filesCommitted'] -and
        [bool]$State.filesCommitted)
    {
        $script:InstallerStep = 'restore-committed-machine-files'
        Restore-CommittedPayloadFiles `
            -State $State `
            -InstallRoot ([IO.Path]::GetFullPath($InstallDirectory))
        $script:InstallerStep = 'restore-previous-install-state'
        Restore-PreviousInstallStatePair `
            -State $State `
            -InstallRoot ([IO.Path]::GetFullPath($InstallDirectory))
        $script:InstallerStep = 'restore-data-directory'
        Restore-DataDirectory `
            -State $State `
            -InstallRoot ([IO.Path]::GetFullPath($InstallDirectory))
    }
}

function Remove-PendingPackageFiles
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [Parameter(Mandatory = $true)]
        [string]$PayloadRoot
    )

    $packageFile = [string]$State.packageFile
    if ([IO.Path]::IsPathRooted($packageFile) -or
        $packageFile.Contains('..') -or
        [IO.Path]::GetFileName($packageFile) -ne $packageFile -or
        $packageFile -notmatch '\.msix$')
    {
        throw 'Protected pending state has an unsafe package file name.'
    }

    $livePath = [IO.Path]::GetFullPath(
        (Join-Path (Join-Path $InstallRoot 'Identity') $packageFile))
    $stagedPath = [IO.Path]::GetFullPath(
        (Join-Path (Join-Path $PayloadRoot 'Identity') $packageFile))
    $declaredPath = [IO.Path]::GetFullPath([string]$State.packagePath)
    if ($declaredPath -ne $livePath -and $declaredPath -ne $stagedPath)
    {
        throw 'Protected pending state points to an unsafe package cleanup path.'
    }

    # A same-version repair can use the same filename as the old package. The
    # machine rollback restores that file before this function runs, so never
    # delete the live path when it is also the protected previous package.
    $oldPackageIsSameFile = $false
    if ($null -ne $State.oldInstallState)
    {
        $oldPackageIsSameFile =
            [string]$State.oldInstallState.packageFile -eq $packageFile
    }
    foreach ($candidatePath in @($declaredPath, $livePath, $stagedPath))
    {
        $candidate = [IO.Path]::GetFullPath($candidatePath)
        if ($candidate -eq $livePath -and $oldPackageIsSameFile)
        {
            continue
        }
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf))
        {
            continue
        }
        Remove-Item -LiteralPath $candidate -Force
        if (Test-Path -LiteralPath $candidate -PathType Leaf)
        {
            throw "The transaction-owned identity package remains: $candidate"
        }
    }
}

function Invoke-PendingRollbackCleanup
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [Parameter(Mandatory = $true)]
        [string]$PayloadRoot,

        [Parameter(Mandatory = $true)]
        [string]$PendingPath
    )

    # This is intentionally a separate phase. If it fails, the restored state
    # and journal remain available for a later repair and the caller returns
    # 1001 instead of pretending that rollback was complete.
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
        Recover-CreatingCertificate -State $State
        if (Test-Path -LiteralPath $PendingPath -PathType Leaf)
        {
            Remove-Item -LiteralPath $PendingPath -Force
        }
        return
    }
    Remove-PendingPackageFiles `
        -State $State `
        -InstallRoot $InstallRoot `
        -PayloadRoot $PayloadRoot
    Remove-PreparedCertificateIfUnused -State $State
    if (Test-Path -LiteralPath $PendingPath -PathType Leaf)
    {
        Remove-Item -LiteralPath $PendingPath -Force
    }
}

function Read-RegistrationResult
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [object]$State
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf))
    {
        throw 'Package registration did not produce a result.'
    }
    $result = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    foreach ($propertyName in @(
        'schema',
        'transactionId',
        'succeeded',
        'installedUserSid',
        'packageName',
        'packageVersion',
        'packageFullName',
        'packageFamilyName',
        'error'))
    {
        if ($null -eq $result.PSObject.Properties[$propertyName])
        {
            throw "Package registration result is missing: $propertyName"
        }
    }
    if ([int]$result.schema -ne 1 -or $result.succeeded -isnot [bool])
    {
        throw 'Package registration result has an unsupported schema.'
    }
    if ([string]$result.transactionId -ne [string]$State.transactionId)
    {
        throw 'Package registration result belongs to a different installation transaction.'
    }
    if (-not [bool]$result.succeeded)
    {
        throw "Package registration failed: $([string]$result.error)"
    }
    $resultMismatch = `
        ([string]$result.installedUserSid -ne [string]$State.userSid) -or `
        ([string]$result.packageName -ne [string]$State.packageName) -or `
        ([string]$result.packageVersion -ne [string]$State.packageVersion) -or `
        ([string]$result.packageFullName -notmatch '^CialloKing\.BaClickFxDesktop_[A-Za-z0-9._-]+$') -or `
        ([string]$result.packageFamilyName -notmatch '^CialloKing\.BaClickFxDesktop_[A-Za-z0-9-]+$')
    if ($resultMismatch)
    {
        throw 'Package registration result does not match protected pending state.'
    }
    $registered = @(
        Get-AppxPackage `
            -User ([string]$State.userSid) `
            -Name ([string]$State.packageName) `
            -ErrorAction Stop |
            Where-Object { [string]$_.PackageFullName -eq [string]$result.packageFullName }
    )
    if ($registered.Count -ne 1 -or
        [string]$registered[0].Version -ne [string]$State.packageVersion -or
        [string]$registered[0].PackageFamilyName -ne [string]$result.packageFamilyName)
    {
        throw 'The Appx registration does not match the reported package.'
    }
    return $result
}

trap
{
    Stop-InstallerWithFailure `
        -ErrorRecord $_ `
        -Step $script:InstallerStep
}

$script:InstallerStep = 'validate-environment'
Assert-Administrator
Assert-WindowsPowerShell
$script:InstallerStep = 'load-compression-runtime'
# Upgrade validation reads the previous MSIX before preparing the new package,
# so both ZIP assemblies must be available independently of that later path.
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$script:InstallerStep = 'validate-installer-version-arguments'
if ($Phase -ne 'Rollback' -and
    ([string]::IsNullOrWhiteSpace($ProductVersion) -or
        [string]::IsNullOrWhiteSpace($PackageVersion)))
{
    throw 'ProductVersion and PackageVersion are required outside rollback.'
}
$script:InstallerStep = 'resolve-installer-paths'
$installRoot = Resolve-ProtectedProgramFilesPath `
    -Path $InstallDirectory `
    -Description 'install directory'
$script:PayloadRoot = ''
if (-not [string]::IsNullOrWhiteSpace($PayloadDirectory))
{
    $script:PayloadRoot = Resolve-PayloadDirectory `
        -InstallRoot $installRoot `
        -PayloadPath $PayloadDirectory
}
elseif ($Phase -notin @('Rollback', 'RollbackCleanup'))
{
    throw 'Installer payload directory is required for this phase.'
}
$script:RollbackRoot = Join-Path $installRoot '.rollback'
$userContextFullPath = Assert-TemporaryStatePath -Path $UserContextPath
$registrationResultFullPath = Assert-TemporaryStatePath -Path $RegistrationResultPath
$machineStateFullPath = [IO.Path]::GetFullPath($MachineStatePath)
$expectedMachineStatePath = [IO.Path]::GetFullPath(
    (Join-Path $installRoot 'Installer\PREPARE-STATE.json'))
if ($machineStateFullPath -ne $expectedMachineStatePath)
{
    throw 'Protected pending state must remain in the Installer directory.'
}
$installStatePath = Join-Path $installRoot 'Installer\INSTALL-STATE.json'

if ($Phase -eq 'Rollback')
{
    if (-not (Test-Path -LiteralPath $machineStateFullPath -PathType Leaf))
    {
        exit 0
    }
    $script:InstallerStep = 'validate-pending-rollback'
    Assert-ProtectedStateAcl -Path $machineStateFullPath
    $pendingState = Get-Content -LiteralPath $machineStateFullPath -Raw | ConvertFrom-Json
    $pendingState = Assert-PendingStateObject -State $pendingState -InstallRoot $installRoot

    # Finalize writes the install-state pair before it marks the journal as
    # committed. A crash in either direction must be classified explicitly:
    # a valid matching pair is already committed, while a present but damaged
    # pair is ambiguous and must remain available for repair.
    $commitState = if ($null -ne $pendingState.PSObject.Properties['commitState'])
    {
        [string]$pendingState.commitState
    }
    else
    {
        'prepared'
    }
    $primaryStatePath = Join-Path $installRoot 'Installer\INSTALL-STATE.json'
    $backupStatePath = "$primaryStatePath.bak"
    $statePairPresent =
        (Test-Path -LiteralPath $primaryStatePath -PathType Leaf) -or
        (Test-Path -LiteralPath $backupStatePath -PathType Leaf)
    $committedState = $null
    $statePairError = $null
    $filesCommitted = $null -ne $pendingState.PSObject.Properties['filesCommitted'] -and
        [bool]$pendingState.filesCommitted
    if ($statePairPresent -or $commitState -eq 'committed')
    {
        try
        {
            $committedState = Read-OldInstallState `
                -InstallRoot $installRoot `
                -UserSid ([string]$pendingState.userSid) `
                -SkipPayloadIntegrity
        }
        catch
        {
            # A torn pair is recoverable only when the pending journal still
            # proves which transaction owns the live files. Keep the error so
            # the rollback path can restore the protected pair from its
            # transaction manifest.
            $statePairError = $_
        }
        if ($null -ne $committedState -and
            [string]$committedState.transactionId -eq
                [string]$pendingState.transactionId)
        {
            try
            {
                $script:InstallerStep = 'retry-committed-cleanup'
                Complete-CommittedPendingTransaction `
                    -State $pendingState `
                    -InstallRoot $installRoot `
                    -PendingPath $machineStateFullPath
            }
            catch
            {
                Stop-InstallerWithFailure `
                    -ErrorRecord $_ `
                    -Step 'retry-committed-cleanup' `
                    -ExitCode 1001
            }
            exit 0
        }
        if ($null -ne $committedState)
        {
            $previousState = $pendingState.oldInstallState
            $previousMatches = $false
            if ($null -ne $previousState)
            {
                if ($null -ne $previousState.PSObject.Properties['transactionId'] -and
                    -not [string]::IsNullOrWhiteSpace([string]$previousState.transactionId))
                {
                    $previousMatches =
                        [string]$committedState.transactionId -eq
                            [string]$previousState.transactionId
                }
                else
                {
                    # Schema 1 did not always carry a transaction marker. Its
                    # fixed package identity is still sufficient to distinguish
                    # the previous install from an unrelated state file.
                    $previousMatches =
                        [string]$committedState.packageFullName -eq
                            [string]$previousState.packageFullName -and
                        [string]$committedState.packageFile -eq
                            [string]$previousState.packageFile -and
                        [string]$committedState.packageSha256 -eq
                            [string]$previousState.packageSha256
                }
            }
            if (-not $previousMatches)
            {
                $unrelatedState = [System.InvalidOperationException]::new(
                    'The protected install state belongs to an unrelated transaction.')
                Stop-InstallerWithFailure `
                    -ErrorRecord ([Management.Automation.ErrorRecord]::new($unrelatedState)) `
                    -Step 'classify-committed-state-pair' `
                    -ExitCode 1001
            }
        }
        if ($commitState -eq 'committed')
        {
            $ambiguousCommit = [System.InvalidOperationException]::new(
                'The pending transaction is marked committed, but the matching install-state pair is unavailable.')
            Stop-InstallerWithFailure `
                -ErrorRecord ([Management.Automation.ErrorRecord]::new($ambiguousCommit)) `
                -Step 'classify-committed-state-pair' `
                -ExitCode 1001
        }
        if ($null -ne $statePairError -and
            -not $filesCommitted)
        {
            Stop-InstallerWithFailure `
                -ErrorRecord $statePairError `
                -Step 'classify-committed-state-pair' `
                -ExitCode 1001
        }
        if ($null -ne $statePairError -and
            $filesCommitted)
        {
            $rollbackManifestPath = Join-Path $installRoot `
                ('.rollback\' + [string]$pendingState.transactionId +
                    '\ROLLBACK-MANIFEST.json')
            if (-not (Test-Path -LiteralPath $rollbackManifestPath -PathType Leaf))
            {
                Stop-InstallerWithFailure `
                    -ErrorRecord $statePairError `
                    -Step 'classify-committed-state-pair' `
                    -ExitCode 1001
            }
        }
    }

    $script:InstallerStep = 'rollback-pending-transaction'
    Invoke-PendingRollback -State $pendingState
    # The original-user package must be restored only after these machine files
    # are back in place. The Inno coordinator performs that user-context step,
    # then invokes RollbackCleanup to remove the new package and certificate.
    exit 0
}

if ($Phase -eq 'RollbackCleanup')
{
    if (-not (Test-Path -LiteralPath $machineStateFullPath -PathType Leaf))
    {
        exit 0
    }
    try
    {
        $script:InstallerStep = 'validate-pending-cleanup'
        Assert-ProtectedStateAcl -Path $machineStateFullPath
        $pendingState = Get-Content `
            -LiteralPath $machineStateFullPath `
            -Raw | ConvertFrom-Json
        Recover-CreatingCertificate -State $pendingState
        if ($null -ne $pendingState.PSObject.Properties['certificatePhase'] -and
            [string]$pendingState.certificatePhase -eq 'creating')
        {
            # No signed package exists yet; the SAN marker recovery above is
            # the only cleanup needed for this early-crash state.
            Remove-Item -LiteralPath $machineStateFullPath -Force
            exit 0
        }
        $pendingState = Assert-PendingStateObject `
            -State $pendingState `
            -InstallRoot $installRoot `
            -PayloadDirectory $script:PayloadRoot
        $script:InstallerStep = 'cleanup-rolled-back-package-and-certificate'
        Invoke-PendingRollbackCleanup `
            -State $pendingState `
            -InstallRoot $installRoot `
            -PayloadRoot $script:PayloadRoot `
            -PendingPath $machineStateFullPath
        exit 0
    }
    catch
    {
        Stop-InstallerWithFailure `
            -ErrorRecord $_ `
            -Step $script:InstallerStep `
            -ExitCode 1001
    }
}

if ($Phase -eq 'CommitFiles')
{
    $script:InstallerStep = 'validate-commit-state'
    if (-not (Test-Path -LiteralPath $machineStateFullPath -PathType Leaf))
    {
        throw 'The prepared installation state is missing.'
    }
    Assert-ProtectedStateAcl -Path $machineStateFullPath
    $pendingState = Get-Content -LiteralPath $machineStateFullPath -Raw | ConvertFrom-Json
    $pendingState = Assert-PendingStateObject `
        -State $pendingState `
        -InstallRoot $installRoot `
        -PayloadDirectory $script:PayloadRoot `
        -RequireIntegrity
    $script:InstallerStep = 'commit-staged-files'
    $pendingState = Commit-PayloadFiles `
        -State $pendingState `
        -InstallRoot $installRoot `
        -PayloadRoot $script:PayloadRoot `
        -PendingPath $machineStateFullPath
    exit 0
}

if ($Phase -eq 'Prepare')
{
    $identity = $null
    $pendingState = $null
    $prepareFailureStep = ''
    $installerDirectoryCreated = $false
    $stalePendingRequiresCoordinator = $false
    try
    {
        $script:InstallerStep = 'validate-staging-acl'
        Assert-ProtectedPayloadAcl -Path $script:PayloadRoot
        $script:InstallerStep = 'validate-installer-payload'
        $replacementHostSha256 = Assert-PayloadManifest -InstallRoot $script:PayloadRoot
        if (Test-Path -LiteralPath $machineStateFullPath -PathType Leaf)
        {
            $script:InstallerStep = 'recover-stale-transaction'
            Assert-ProtectedStateAcl -Path $machineStateFullPath
            $stalePending = Get-Content -LiteralPath $machineStateFullPath -Raw | ConvertFrom-Json
            Recover-CreatingCertificate -State $stalePending
            $stalePending = Assert-PendingStateObject `
                -State $stalePending `
                -InstallRoot $installRoot
            # Recovery has to remove/restore the user's AppX registration in a
            # separate original-user process. Leave this journal untouched and
            # let the Inno coordinator execute the fixed four-step sequence.
            $stalePendingRequiresCoordinator = $true
            throw 'A previous pending transaction requires coordinator recovery.'
        }
        $script:InstallerStep = 'read-original-user-context'
        $context = Get-Content -LiteralPath $userContextFullPath -Raw | ConvertFrom-Json
        if ([int]$context.schema -ne 1 -or [string]$context.userSid -notmatch '^S-1-[0-9-]+$')
        {
            throw 'The original user context is invalid.'
        }

        $script:InstallerStep = 'prepare-protected-installer-directory'
        $installerDirectoryCreated = Ensure-ProtectedInstallerDirectory `
            -InstallRoot $installRoot `
            -ReadSid ([string]$context.userSid)
        $script:InstallerStep = 'read-existing-install-state'
        # Bind the old state to the exact replacement hash validated from
        # staging while retaining the old live Host check until commit.
        $oldInstallState = Read-OldInstallState `
            -InstallRoot $installRoot `
            -UserSid ([string]$context.userSid) `
            -ExpectedReplacementHostSha256 $replacementHostSha256 `
            -ReplacementHostPath (Join-Path $script:PayloadRoot 'ba-click-fx-desktop.exe')
        $metadataPath = Join-Path (Join-Path $script:PayloadRoot 'Identity') `
            "CialloKing.BaClickFxDesktop-$PackageVersion.identity-template.json"
        $metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
        if ([int]$metadata.schema -ne 3 -or
            [string]$metadata.identityMode -ne 'target-machine-self-signed')
        {
            throw 'Identity package metadata has an unsupported schema.'
        }
        $templatePath = Join-Path (Join-Path $script:PayloadRoot 'Identity') `
            ([string]$metadata.templateFile)
        $script:InstallerStep = 'inspect-existing-package-registrations'
        $preexistingFullNames = @(
            Get-AppxPackage `
                -User ([string]$context.userSid) `
                -Name ([string]$metadata.packageName) `
                -ErrorAction Stop |
                ForEach-Object { [string]$_.PackageFullName }
        )
        if ($null -eq $oldInstallState -and $preexistingFullNames.Count -gt 0)
        {
            throw 'An untracked package registration already exists; uninstall it before continuing.'
        }
        $untrackedFullNames = @()
        if ($null -ne $oldInstallState)
        {
            $untrackedFullNames = @(
                $preexistingFullNames | Where-Object {
                    [string]$_ -ne [string]$oldInstallState.packageFullName
                }
            )
        }
        if ($null -ne $oldInstallState -and $untrackedFullNames.Count -gt 0)
        {
            throw 'An untracked package registration exists beside the protected install state.'
        }
        $script:InstallerStep = 'validate-same-version-repair'
        if ($null -ne $oldInstallState -and
            [string]$oldInstallState.productVersion -eq $ProductVersion -and
            $null -ne $oldInstallState.PSObject.Properties['templateSha256'] -and
            [string]$oldInstallState.templateSha256 -ne [string]$metadata.templateSha256)
        {
            $oldPackagePath = Join-Path (Join-Path $installRoot 'Identity') `
                ([string]$oldInstallState.packageFile)
            if (-not (Test-Path -LiteralPath $oldPackagePath -PathType Leaf) -or
                (Get-IdentityTemplateContentHash -Path $oldPackagePath) -ne
                    (Get-IdentityTemplateContentHash -Path $templatePath))
            {
                throw 'Refusing a same-version repair whose identity package content changed.'
            }
        }

        $pendingSeed = [ordered]@{
            schema = 2
            stateKind = 'prepare'
            commitState = 'prepared'
            userSid = [string]$context.userSid
            packageName = [string]$metadata.packageName
            applicationId = [string]$metadata.applicationId
            publisher = [string]$metadata.publisher
            productVersion = $ProductVersion
            packageVersion = $PackageVersion
            transactionId = [Guid]::NewGuid().ToString('N')
            templateSha256 = [string]$metadata.templateSha256
            preexistingPackageFullNames = $preexistingFullNames
            oldInstallState = $oldInstallState
            installerDirectoryCreated = $installerDirectoryCreated
            preparedUtc = [DateTime]::UtcNow.ToString('o')
        }
        $script:InstallerStep = 'prepare-identity-package'
        $identity = Assert-IdentityPayload `
            -InstallRoot $script:PayloadRoot `
            -PendingStatePath $machineStateFullPath `
            -PendingStateSeed $pendingSeed
        $script:InstallerStep = 'validate-prepared-identity'
        Assert-ProtectedStateAcl -Path $machineStateFullPath
        $pendingState = Get-Content -LiteralPath $machineStateFullPath -Raw | ConvertFrom-Json
        $pendingState = Assert-PendingStateObject `
            -State $pendingState `
            -InstallRoot $installRoot `
            -RequireIntegrity `
            -PayloadDirectory $script:PayloadRoot
        exit 0
    }
    catch
    {
        $prepareErrorRecord = $_
        $prepareFailureStep = $script:InstallerStep
        $prepareRollbackSucceeded = $true
        if (-not $stalePendingRequiresCoordinator -and
            ($null -ne $pendingState -or
            (Test-Path -LiteralPath $machineStateFullPath -PathType Leaf))
        )
        {
            try
            {
                $script:InstallerStep = 'rollback-failed-prepare'
                if ($null -eq $pendingState)
                {
                    Assert-ProtectedStateAcl -Path $machineStateFullPath
                    $pendingState = Get-Content `
                        -LiteralPath $machineStateFullPath `
                        -Raw | ConvertFrom-Json
                    $pendingState = Assert-PendingStateObject `
                        -State $pendingState `
                        -InstallRoot $installRoot
                }
                Invoke-PendingRollback -State $pendingState
                Invoke-PendingRollbackCleanup `
                    -State $pendingState `
                    -InstallRoot $installRoot `
                    -PayloadRoot $script:PayloadRoot `
                    -PendingPath $machineStateFullPath
                if ($installerDirectoryCreated -and
                    -not (Test-Path -LiteralPath $installStatePath -PathType Leaf))
                {
                    $installerDirectory = Join-Path $installRoot 'Installer'
                    if ((Test-Path -LiteralPath $installerDirectory -PathType Container) -and
                        @(Get-ChildItem -LiteralPath $installerDirectory -Force).Count -eq 0)
                    {
                        Remove-Item -LiteralPath $installerDirectory -Force
                    }
                }
            }
            catch
            {
                $prepareRollbackSucceeded = $false
                Add-InstallerRelatedFailure `
                    -ErrorRecord $_ `
                    -Step 'rollback-failed-prepare'
            }
        }
        $script:InstallerStep = $prepareFailureStep
        Stop-InstallerWithFailure `
            -ErrorRecord $prepareErrorRecord `
            -Step $prepareFailureStep `
            -ExitCode $(if ($prepareRollbackSucceeded) { 1 } else { 1002 })
    }
}

$script:InstallerStep = 'validate-finalize-state'
Assert-ProtectedStateAcl -Path $machineStateFullPath
$pendingState = Get-Content -LiteralPath $machineStateFullPath -Raw | ConvertFrom-Json
$pendingState = Assert-PendingStateObject `
    -State $pendingState `
    -InstallRoot $installRoot `
    -RequireIntegrity
$stateCommitted = $false
try
{
    $script:InstallerStep = 'read-package-registration-result'
    $registrationResult = Read-RegistrationResult `
        -Path $registrationResultFullPath `
        -State $pendingState
    $certificateInstalledBySetup = -not [bool]$pendingState.certificateWasPresent
    $certificateOwnership = if ($null -ne $pendingState.PSObject.Properties['certificateOwnership'])
    {
        [string]$pendingState.certificateOwnership
    }
    elseif ($certificateInstalledBySetup)
    {
        'installer-owned'
    }
    else
    {
        'unknown'
    }
    if ($null -ne $pendingState.oldInstallState -and
        [string]$pendingState.oldInstallState.certificateThumbprint -eq
            [string]$pendingState.certificateThumbprint)
    {
        $certificateInstalledBySetup =
            [bool]$pendingState.oldInstallState.certificateInstalledBySetup
    }
    $ownedCertificateThumbprints = @(
        Split-Ledger `
            -Value $pendingState.ownedCertificateThumbprints `
            -Separator Comma
    )
    if ($certificateOwnership -eq 'installer-owned')
    {
        # Only an explicit installer-owned marker proves that this transaction
        # created the certificate. Pre-existing, shared, and legacy-unknown
        # certificates must never enter the deletion ledger.
        $ownedCertificateThumbprints += [string]$pendingState.certificateThumbprint
    }
    $ownedPackageFiles = @(
        Split-Ledger `
            -Value $pendingState.ownedPackageFiles `
            -Separator Pipe
        [string]$pendingState.packageFile
    )
    $installState = [ordered]@{
        schema = 2
        transactionId = [string]$pendingState.transactionId
        packageName = [string]$pendingState.packageName
        applicationId = [string]$pendingState.applicationId
        publisher = [string]$pendingState.publisher
        productVersion = $ProductVersion
        packageVersion = $PackageVersion
        templateSha256 = if ($null -eq $pendingState.PSObject.Properties['templateSha256'])
        {
            ''
        }
        else
        {
            [string]$pendingState.templateSha256
        }
        packageFullName = [string]$registrationResult.packageFullName
        packageFamilyName = [string]$registrationResult.packageFamilyName
        certificateThumbprint = [string]$pendingState.certificateThumbprint
        certificateSha256 = [string]$pendingState.certificateSha256
        certificateInstalledBySetup = [bool]$certificateInstalledBySetup
        certificateNotAfterUtc = [string]$pendingState.certificateNotAfterUtc
        certificateOwnership = $certificateOwnership
        externalLocation = $installRoot
        installedUserSid = [string]$pendingState.userSid
        hostFile = [string]$pendingState.hostFile
        hostSha256 = [string]$pendingState.hostSha256
        packageFile = [string]$pendingState.packageFile
        packageSha256 = [string]$pendingState.packageSha256
        ownedCertificateThumbprints = Join-Ledger `
            -Values $ownedCertificateThumbprints `
            -Separator Comma
        ownedPackageFiles = Join-Ledger `
            -Values $ownedPackageFiles `
            -Separator Pipe
        installedUtc = [DateTime]::UtcNow.ToString('o')
    }
    $script:InstallerStep = 'commit-install-state'
    Write-ProtectedInstallState `
        -Path $installStatePath `
        -Value $installState `
        -ReadSid ([string]$pendingState.userSid)
    $stateCommitted = $true

    # Publish the committed marker before deleting any old package or
    # certificate. If cleanup or its follow-up state write is interrupted,
    # recovery must finish cleanup instead of restoring a transaction whose
    # state pair is already valid.
    $script:InstallerStep = 'mark-pending-committed'
    $pendingState.commitState = 'committed'
    Write-ProtectedJson `
        -Path $machineStateFullPath `
        -Value $pendingState `
        -ReadSid ([string]$pendingState.userSid)

    $script:InstallerStep = 'clean-obsolete-identity-artifacts'
    $committedState = Read-OldInstallState `
        -InstallRoot $installRoot `
        -UserSid ([string]$pendingState.userSid)
    $oldOwnedPackageFiles = [string]$committedState.ownedPackageFiles
    $oldOwnedCertificateThumbprints = [string]$committedState.ownedCertificateThumbprints
    $cleanedState = Remove-ObsoleteIdentityArtifacts -State $committedState
    if ([string]$cleanedState.ownedPackageFiles -ne $oldOwnedPackageFiles -or
        [string]$cleanedState.ownedCertificateThumbprints -ne
            $oldOwnedCertificateThumbprints)
    {
        # This is an optional ledger compaction. The pair is already committed,
        # so a failed rewrite is recoverable and must retain the journal.
        $script:InstallerStep = 'record-cleaned-identity-ledger'
        Write-ProtectedInstallState `
            -Path $installStatePath `
            -Value $cleanedState `
            -ReadSid ([string]$pendingState.userSid)
    }

    # Keep the journal until every cleanup operation and the optional ledger
    # update has completed. A restart can then distinguish a committed state
    # from a transaction that still needs machine-file rollback.
    $script:InstallerStep = 'delete-pending-state'
    Remove-Item -LiteralPath $machineStateFullPath -Force
}
catch
{
    $finalizeErrorRecord = $_
    $finalizeFailureStep = $script:InstallerStep
    if ($stateCommitted)
    {
        # The protected state pair is already committed. Cleanup failures must
        # retain the journal for repair and must never undo a valid install.
        Stop-InstallerWithFailure `
            -ErrorRecord $finalizeErrorRecord `
            -Step $finalizeFailureStep `
            -ExitCode 1001
    }
    # Leave the pending transaction untouched. The elevated phase cannot
    # remove the original user's package, so the Inno coordinator must perform
    # RemoveNew -> machine restore -> RestorePrevious -> final cleanup in that
    # order. A restart follows the same coordinator path.
    $script:InstallerStep = $finalizeFailureStep
    Stop-InstallerWithFailure `
        -ErrorRecord $finalizeErrorRecord `
        -Step $finalizeFailureStep `
        -ExitCode 1
}
