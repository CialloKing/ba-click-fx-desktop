[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Prepare', 'Finalize', 'Rollback')]
    [string]$Phase,

    [Parameter(Mandatory = $true)]
    [string]$InstallDirectory,

    [Parameter(Mandatory = $true)]
    [string]$UserContextPath,

    [Parameter(Mandatory = $true)]
    [string]$MachineStatePath,

    [Parameter(Mandatory = $true)]
    [string]$RegistrationResultPath,

    [Parameter(Mandatory = $true)]
    [string]$ProductVersion,

    [Parameter(Mandatory = $true)]
    [string]$PackageVersion
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

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

    $temporaryPath = "$Path.$PID.tmp"
    try
    {
        Write-Utf8NoBom `
            -Path $temporaryPath `
            -Content ($Value | ConvertTo-Json -Depth 8)
        Set-ProtectedStateAcl -Path $temporaryPath -ReadSid $ReadSid
        Move-Item -LiteralPath $temporaryPath -Destination $Path -Force
        Set-ProtectedStateAcl -Path $Path -ReadSid $ReadSid
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

    Write-ProtectedJson -Path $Path -Value $Value -ReadSid $ReadSid
    $backupPath = "$Path.bak"
    Copy-Item -LiteralPath $Path -Destination $backupPath -Force
    Set-ProtectedStateAcl -Path $backupPath -ReadSid $ReadSid
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

function Resolve-ProtectedInstallRoot
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $resolved = [IO.Path]::GetFullPath($Path)
    $roots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}) |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        ForEach-Object { [IO.Path]::GetFullPath($_).TrimEnd('\') + '\' }
    foreach ($root in $roots)
    {
        if ($resolved.StartsWith($root, [StringComparison]::OrdinalIgnoreCase))
        {
            return $resolved
        }
    }
    throw "The install directory is outside Program Files: $resolved"
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

    Add-Type -AssemblyName System.IO.Compression.FileSystem
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

    $manifestPath = Join-Path $InstallRoot 'Installer\INSTALLER-PAYLOAD.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ([int]$manifest.schema -ne 2 -or
        [string]$manifest.version -ne $ProductVersion -or
        [string]$manifest.identityMode -ne 'target-machine-self-signed')
    {
        throw 'The installer payload manifest has an unexpected version.'
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

function Assert-IdentityIntegrityMaterial
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [Parameter(Mandatory = $true)]
        [string]$PackagePath
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

    $hostPath = Join-Path $InstallRoot ([string]$State.hostFile)
    if (-not (Test-Path -LiteralPath $hostPath -PathType Leaf) -or
        (Get-FileHash -LiteralPath $hostPath -Algorithm SHA256).Hash -ne
            [string]$State.hostSha256)
    {
        throw 'Protected identity state does not match the installed Host.'
    }
    if (-not (Test-Path -LiteralPath $PackagePath -PathType Leaf) -or
        (Get-FileHash -LiteralPath $PackagePath -Algorithm SHA256).Hash -ne
            [string]$State.packageSha256)
    {
        throw 'Protected identity state does not match the signed package.'
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

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem

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

    $certificate = $null
    $certificateWasPresent = $false
    $certificatePrivateKeyRemoved = $false
    $signedPackagePath = $null
    $publicCertificatePath = Join-Path (
        [IO.Path]::GetTempPath()) ('bafx-identity-' + [Guid]::NewGuid().ToString('N') + '.cer')

    try
    {
        $certificate = New-SelfSignedCertificate `
            -Type CodeSigningCert `
            -Subject ([string]$metadata.publisher) `
            -CertStoreLocation 'Cert:\LocalMachine\My' `
            -KeyAlgorithm RSA `
            -KeyLength 2048 `
            -HashAlgorithm SHA256 `
            -KeyExportPolicy NonExportable `
            -NotAfter (Get-Date).AddYears(2)
        if ($null -eq $certificate -or
            [string]$certificate.Subject -ne [string]$metadata.publisher)
        {
            throw 'Target-machine signing certificate creation returned an unexpected certificate.'
        }
        $certificateThumbprint = ([string]$certificate.Thumbprint).ToUpperInvariant()
        $journal = [ordered]@{}
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
            if ($PendingStateSeed -is [Collections.IDictionary])
            {
                $journal[$entry.Key] = $entry.Value
            }
            else
            {
                $journal[$entry.Key] = $entry.Value
            }
        }
        $journal.certificateThumbprint = $certificateThumbprint
        $journal.certificateWasPresent = $false
        $journal.packagePath = Join-Path $identityDirectory (
            "$metadataBaseName-$certificateThumbprint.msix")
        $journal.packageFile = [IO.Path]::GetFileName([string]$journal.packagePath)
        $ownedCertificateThumbprints = @($certificateThumbprint)
        $ownedPackageFiles = @([string]$journal.packageFile)
        if ($null -ne $PendingStateSeed.oldInstallState)
        {
            $oldState = $PendingStateSeed.oldInstallState
            $oldCertificateLedger = if (
                $null -ne $oldState.PSObject.Properties['ownedCertificateThumbprints'])
            {
                [string]$oldState.ownedCertificateThumbprints
            }
            else
            {
                [string]$oldState.certificateThumbprint
            }
            $oldPackageLedger = if (
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
        Export-Certificate `
            -Cert $certificate `
            -FilePath $publicCertificatePath `
            -Type CERT | Out-Null

        $existingCertificate = Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
            Where-Object { $_.Thumbprint -eq $certificateThumbprint } |
            Select-Object -First 1
        $certificateWasPresent = $null -ne $existingCertificate
        if (-not $certificateWasPresent)
        {
            $imported = Import-Certificate `
                -FilePath $publicCertificatePath `
                -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople'
            if ([string]$imported.Thumbprint -ne $certificateThumbprint)
            {
                throw 'Imported target-machine certificate thumbprint mismatch.'
            }
        }

        $signedPackagePath = [string]$journal.packagePath
        Copy-Item -LiteralPath $templatePath -Destination $signedPackagePath -Force
        & $signerPath `
            '--package' $signedPackagePath `
            '--thumbprint' $certificateThumbprint `
            '--store-location' 'LocalMachine'
        if ($LASTEXITCODE -ne 0)
        {
            throw "Native identity signer failed with exit code $LASTEXITCODE."
        }

        $signature = Get-AuthenticodeSignature -LiteralPath $signedPackagePath
        $signatureInvalid = `
            ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid) -or `
            ($null -eq $signature.SignerCertificate) -or `
            ($signature.SignerCertificate.Thumbprint -ne $certificateThumbprint)
        if ($signatureInvalid)
        {
            throw "Sparse package signature is invalid: $($signature.StatusMessage)"
        }

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
        Write-ProtectedJson `
            -Path $PendingStatePath `
            -Value $journal `
            -ReadSid ([string]$PendingStateSeed.userSid)

        # The package is already trusted through its public certificate. The
        # private key must not survive Prepare, because it is not needed after
        # the one package signature has been produced.
        $privateCertificatePath = "Cert:\LocalMachine\My\$certificateThumbprint"
        Remove-Item `
            -LiteralPath $privateCertificatePath `
            -DeleteKey `
            -Force
        $certificatePrivateKeyRemoved = $true
        if (Test-Path -LiteralPath $privateCertificatePath)
        {
            throw 'Target-machine private signing certificate remains after cleanup.'
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
        if ($null -ne $signedPackagePath -and
            (Test-Path -LiteralPath $signedPackagePath -PathType Leaf))
        {
            Remove-Item -LiteralPath $signedPackagePath -Force
        }
        if ($null -ne $certificate)
        {
            $certificateThumbprint = ([string]$certificate.Thumbprint).ToUpperInvariant()
            $privateCertificatePath = "Cert:\LocalMachine\My\$certificateThumbprint"
            if (-not $certificatePrivateKeyRemoved -and
                (Test-Path -LiteralPath $privateCertificatePath))
            {
                Remove-Item `
                    -LiteralPath $privateCertificatePath `
                    -DeleteKey `
                    -Force
            }
        }
        if ($null -ne $certificate -and -not $certificateWasPresent)
        {
            $trusted = Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
                Where-Object { $_.Thumbprint -eq ([string]$certificate.Thumbprint).ToUpperInvariant() } |
                Select-Object -First 1
            if ($null -ne $trusted)
            {
                Remove-Item -LiteralPath $trusted.PSPath -Force
            }
        }
        throw
    }
    finally
    {
        if ($null -ne $certificate)
        {
            $certificate.Dispose()
        }
        if (Test-Path -LiteralPath $publicCertificatePath -PathType Leaf)
        {
            Remove-Item -LiteralPath $publicCertificatePath -Force
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
        [string]$ExpectedUserSid
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
    if ([string]$State.productVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+-alpha\.[0-9]+$')
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
    if ($null -eq $State.PSObject.Properties['ownedCertificateThumbprints'])
    {
        $State | Add-Member -NotePropertyName ownedCertificateThumbprints `
            -NotePropertyValue ([string]$State.certificateThumbprint)
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
    if ($schema -eq 2)
    {
        Assert-IdentityIntegrityMaterial `
            -State $State `
            -InstallRoot $InstallRoot `
            -PackagePath (Join-Path (Join-Path $InstallRoot 'Identity') $packageFile)
    }
    return $State
}

function Read-OldInstallState
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [Parameter(Mandatory = $true)]
        [string]$UserSid
    )

    $path = Join-Path $InstallRoot 'Installer\INSTALL-STATE.json'
    $backupPath = "$path.bak"
    $candidates = @(
        @($path, $backupPath) |
            Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }
    )
    if ($candidates.Count -eq 0)
    {
        return $null
    }
    $errors = New-Object Collections.Generic.List[string]
    foreach ($candidate in $candidates)
    {
        try
        {
            Assert-ProtectedStateAcl -Path $candidate
            $state = Get-Content -LiteralPath $candidate -Raw | ConvertFrom-Json
            $validated = Assert-InstallStateObject `
                -State $state `
                -InstallRoot $InstallRoot `
                -ExpectedUserSid $UserSid
            if ($candidate -ne $path)
            {
                Copy-Item -LiteralPath $candidate -Destination $path -Force
                Set-ProtectedStateAcl -Path $path -ReadSid $UserSid
            }
            return $validated
        }
        catch
        {
            $errors.Add("$candidate`: $($_.Exception.Message)")
        }
    }
    throw "Protected install state and its backup are invalid: $($errors -join ' | ')"
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
        -not [bool]$State.certificateInstalledBySetup -or
        [string]$State.certificateThumbprint -eq $CurrentCertificateThumbprint)
    {
        return
    }
    $remainingPackages = @(
        Get-AppxPackage -AllUsers -Name ([string]$State.packageName) -ErrorAction Stop |
            Where-Object { [string]$_.PackageFullName -eq [string]$State.packageFullName }
    )
    if ($remainingPackages.Count -gt 0)
    {
        Write-Warning "Keeping certificate $($State.certificateThumbprint) while the previous package registration remains."
        return
    }
    $certificate = Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
        Where-Object { $_.Thumbprint -eq [string]$State.certificateThumbprint } |
        Select-Object -First 1
    if ($null -eq $certificate)
    {
        return
    }
    if ($certificate.Subject -ne [string]$State.publisher)
    {
        throw 'Refusing to remove an old certificate with an unexpected subject.'
    }
    Remove-Item -LiteralPath $certificate.PSPath -Force
}

function Remove-ObsoleteIdentityArtifacts
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State
    )

    $otherPackages = @(
        Get-AppxPackage -AllUsers -Name ([string]$State.packageName) -ErrorAction Stop |
            Where-Object { [string]$_.PackageFullName -ne [string]$State.packageFullName }
    )
    if ($otherPackages.Count -gt 0)
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
    foreach ($thumbprint in (Split-Ledger `
            -Value $State.ownedCertificateThumbprints `
            -Separator Comma))
    {
        $normalizedThumbprint = $thumbprint.ToUpperInvariant()
        if ($normalizedThumbprint -eq $currentThumbprint)
        {
            $remainingCertificates.Add($normalizedThumbprint)
            continue
        }
        $certificate = Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
            Where-Object { $_.Thumbprint -eq $normalizedThumbprint } |
            Select-Object -First 1
        if ($null -ne $certificate)
        {
            if ($certificate.Subject -ne [string]$State.publisher)
            {
                throw 'Refusing to remove an owned certificate with an unexpected subject.'
            }
            Remove-Item -LiteralPath $certificate.PSPath -Force
        }
        $privateCertificate = Get-ChildItem -Path 'Cert:\LocalMachine\My' |
            Where-Object { $_.Thumbprint -eq $normalizedThumbprint } |
            Select-Object -First 1
        if ($null -ne $privateCertificate)
        {
            if ($privateCertificate.Subject -ne [string]$State.publisher)
            {
                throw 'Refusing to remove an owned private certificate with an unexpected subject.'
            }
            Remove-Item -LiteralPath $privateCertificate.PSPath -DeleteKey -Force
        }
        if ($null -ne (Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
                Where-Object { $_.Thumbprint -eq $normalizedThumbprint } |
                Select-Object -First 1))
        {
            $remainingCertificates.Add($normalizedThumbprint)
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

function Assert-PendingStateObject
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot,

        [switch]$RequireIntegrity
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
        'templateSha256',
        'packagePath',
        'packageFile',
        'certificateThumbprint',
        'certificateWasPresent',
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
    if ([string]$State.userSid -notmatch '^S-1-[0-9-]+$')
    {
        throw 'Protected pending state has an invalid user SID.'
    }
    if ([string]$State.packageName -ne 'CialloKing.BaClickFxDesktop' -or
        [string]$State.applicationId -ne 'BaClickFxDesktop' -or
        [string]$State.publisher -ne 'CN=BaClickFx.Local' -or
        [string]$State.productVersion -ne $ProductVersion -or
        [string]$State.packageVersion -ne $PackageVersion)
    {
        throw 'Protected pending state does not match this installer.'
    }
    if ([string]$State.transactionId -notmatch '^[0-9a-fA-F]{32}$')
    {
        throw 'Protected pending state has an invalid transaction identifier.'
    }
    if ([string]$State.templateSha256 -notmatch '^[0-9A-Fa-f]{64}$')
    {
        throw 'Protected pending state has an invalid template hash.'
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
    $expectedPackagePath = [IO.Path]::GetFullPath(
        (Join-Path (Join-Path $InstallRoot 'Identity') $packageFile))
    if ([IO.Path]::GetFullPath([string]$State.packagePath) -ne $expectedPackagePath)
    {
        throw 'Protected pending state points to a different package file.'
    }
    if ($RequireIntegrity)
    {
        Assert-IdentityIntegrityMaterial `
            -State $State `
            -InstallRoot $InstallRoot `
            -PackagePath $expectedPackagePath
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

    if ([bool]$State.certificateWasPresent)
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
    $certificate = Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
        Where-Object { $_.Thumbprint -eq [string]$State.certificateThumbprint } |
        Select-Object -First 1
    if ($null -eq $certificate)
    {
        $certificate = $null
    }
    if ($null -ne $certificate -and $certificate.Subject -ne [string]$State.publisher)
    {
        throw 'Refusing to remove the prepared certificate with an unexpected subject.'
    }
    if ($null -ne $certificate)
    {
        Remove-Item -LiteralPath $certificate.PSPath -Force
    }
    $privateCertificate = Get-ChildItem -Path 'Cert:\LocalMachine\My' |
        Where-Object { $_.Thumbprint -eq [string]$State.certificateThumbprint } |
        Select-Object -First 1
    if ($null -ne $privateCertificate)
    {
        if ($privateCertificate.Subject -ne [string]$State.publisher)
        {
            throw 'Refusing to remove the prepared private certificate with an unexpected subject.'
        }
        Remove-Item -LiteralPath $privateCertificate.PSPath -DeleteKey -Force
    }
}

function Invoke-PendingRollback
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$State
    )

    $packageError = $null
    try
    {
        Remove-NewPackageRegistrations -State $State
    }
    catch
    {
        $packageError = $_.Exception.Message
    }
    if ($null -ne $packageError)
    {
        throw "Package rollback failed: $packageError"
    }
    $preparedPackagePath = [string]$State.packagePath
    if (Test-Path -LiteralPath $preparedPackagePath -PathType Leaf)
    {
        # Assert-PendingStateObject already bound this path to packageFile under
        # the Identity directory; remove the transaction-owned artifact only.
        Remove-Item -LiteralPath $preparedPackagePath -Force
    }
    Remove-PreparedCertificateIfUnused -State $State
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

Assert-Administrator
Assert-WindowsPowerShell
$installRoot = Resolve-ProtectedInstallRoot -Path $InstallDirectory
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
    Assert-ProtectedStateAcl -Path $machineStateFullPath
    $pendingState = Get-Content -LiteralPath $machineStateFullPath -Raw | ConvertFrom-Json
    $pendingState = Assert-PendingStateObject -State $pendingState -InstallRoot $installRoot
    Invoke-PendingRollback -State $pendingState
    Remove-Item -LiteralPath $machineStateFullPath -Force
    exit 0
}

if ($Phase -eq 'Prepare')
{
    $identity = $null
    $pendingState = $null
    try
    {
        if (Test-Path -LiteralPath $machineStateFullPath -PathType Leaf)
        {
            Assert-ProtectedStateAcl -Path $machineStateFullPath
            $stalePending = Get-Content -LiteralPath $machineStateFullPath -Raw | ConvertFrom-Json
            $stalePending = Assert-PendingStateObject `
                -State $stalePending `
                -InstallRoot $installRoot
            $committedState = $null
            try
            {
                $committedState = Read-OldInstallState `
                    -InstallRoot $installRoot `
                    -UserSid ([string]$stalePending.userSid)
            }
            catch
            {
                $committedState = $null
            }
            if ($null -ne $committedState -and
                [string]$committedState.transactionId -eq [string]$stalePending.transactionId)
            {
                # Finalize committed the state before the process stopped. The
                # pending journal is no longer needed and must not trigger a
                # second package rollback on the next repair.
                Remove-Item -LiteralPath $machineStateFullPath -Force
            }
            else
            {
                Invoke-PendingRollback -State $stalePending
                Remove-Item -LiteralPath $machineStateFullPath -Force
            }
        }
        $context = Get-Content -LiteralPath $userContextFullPath -Raw | ConvertFrom-Json
        if ([int]$context.schema -ne 1 -or [string]$context.userSid -notmatch '^S-1-[0-9-]+$')
        {
            throw 'The original user context is invalid.'
        }

        Assert-PayloadManifest -InstallRoot $installRoot
        $dataDirectory = Join-Path $installRoot 'data'
        $oldInstallState = Read-OldInstallState `
            -InstallRoot $installRoot `
            -UserSid ([string]$context.userSid)
        Grant-DataDirectoryAccess -Path $dataDirectory -UserSid ([string]$context.userSid)
        $metadataPath = Join-Path (Join-Path $installRoot 'Identity') `
            "CialloKing.BaClickFxDesktop-$PackageVersion.identity-template.json"
        $metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
        if ([int]$metadata.schema -ne 3 -or
            [string]$metadata.identityMode -ne 'target-machine-self-signed')
        {
            throw 'Identity package metadata has an unsupported schema.'
        }
        $templatePath = Join-Path (Join-Path $installRoot 'Identity') `
            ([string]$metadata.templateFile)
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
            schema = 1
            stateKind = 'prepare'
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
            preparedUtc = [DateTime]::UtcNow.ToString('o')
        }
        $identity = Assert-IdentityPayload `
            -InstallRoot $installRoot `
            -PendingStatePath $machineStateFullPath `
            -PendingStateSeed $pendingSeed
        Assert-ProtectedStateAcl -Path $machineStateFullPath
        $pendingState = Get-Content -LiteralPath $machineStateFullPath -Raw | ConvertFrom-Json
        $pendingState = Assert-PendingStateObject `
            -State $pendingState `
            -InstallRoot $installRoot `
            -RequireIntegrity
        Initialize-IdentityConfig -InstallRoot $installRoot -DataDirectory $dataDirectory
        exit 0
    }
    catch
    {
        $prepareError = $_.Exception.Message
        if ($null -ne $pendingState -or
            (Test-Path -LiteralPath $machineStateFullPath -PathType Leaf))
        {
            try
            {
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
                if (Test-Path -LiteralPath $machineStateFullPath -PathType Leaf)
                {
                    Remove-Item -LiteralPath $machineStateFullPath -Force
                }
            }
            catch
            {
                throw "Prepare failed: $prepareError Rollback failed: $($_.Exception.Message)"
            }
        }
        throw $prepareError
    }
}

Assert-ProtectedStateAcl -Path $machineStateFullPath
$pendingState = Get-Content -LiteralPath $machineStateFullPath -Raw | ConvertFrom-Json
$pendingState = Assert-PendingStateObject `
    -State $pendingState `
    -InstallRoot $installRoot `
    -RequireIntegrity
try
{
    $registrationResult = Read-RegistrationResult `
        -Path $registrationResultFullPath `
        -State $pendingState
    $certificateInstalledBySetup = -not [bool]$pendingState.certificateWasPresent
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
        [string]$pendingState.certificateThumbprint
    )
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
        templateSha256 = [string]$pendingState.templateSha256
        packageFullName = [string]$registrationResult.packageFullName
        packageFamilyName = [string]$registrationResult.packageFamilyName
        certificateThumbprint = [string]$pendingState.certificateThumbprint
        certificateSha256 = [string]$pendingState.certificateSha256
        certificateInstalledBySetup = [bool]$certificateInstalledBySetup
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
    Write-ProtectedInstallState `
        -Path $installStatePath `
        -Value $installState `
        -ReadSid ([string]$pendingState.userSid)
    Remove-Item -LiteralPath $machineStateFullPath -Force
}
catch
{
    $finalizeError = $_.Exception.Message
    try
    {
        Invoke-PendingRollback -State $pendingState
    }
    catch
    {
        throw "Finalize failed: $finalizeError Rollback failed: $($_.Exception.Message)"
    }
    throw $finalizeError
}

# Cleanup after the protected commit is best effort. The state ledger is
# deliberately written before cleanup so a locked file or shared package can
# be retried on the next repair or uninstall.
try
{
    $committedState = Read-OldInstallState `
        -InstallRoot $installRoot `
        -UserSid ([string]$pendingState.userSid)
    $cleanedState = Remove-ObsoleteIdentityArtifacts -State $committedState
    Write-ProtectedInstallState `
        -Path $installStatePath `
        -Value $cleanedState `
        -ReadSid ([string]$pendingState.userSid)
}
catch
{
    Write-Warning "Could not clean obsolete identity artifacts: $($_.Exception.Message)"
}
