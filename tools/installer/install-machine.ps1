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
    [string]$PackageVersion,

    [switch]$DisableSystemBorder
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

function Assert-PayloadManifest
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstallRoot
    )

    $manifestPath = Join-Path $InstallRoot 'Installer\INSTALLER-PAYLOAD.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ([int]$manifest.schema -ne 1 -or [string]$manifest.version -ne $ProductVersion)
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

function Assert-IdentityPayload
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstallRoot
    )

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    $identityDirectory = Join-Path $InstallRoot 'Identity'
    $metadataBaseName = "CialloKing.BaClickFxDesktop-$PackageVersion"
    $metadataFile = Get-Item -LiteralPath (
        Join-Path $identityDirectory "$metadataBaseName.identity.json") `
        -ErrorAction SilentlyContinue
    if ($null -eq $metadataFile -or $metadataFile.PSIsContainer)
    {
        throw 'Identity package metadata is missing.'
    }
    $metadata = Get-Content -LiteralPath $metadataFile.FullName -Raw | ConvertFrom-Json
    if ([int]$metadata.schema -ne 2)
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

    $packagePath = Join-Path $identityDirectory ([string]$metadata.packageFile)
    $certificatePath = Join-Path $identityDirectory ([string]$metadata.certificateFile)
    foreach ($payloadName in @(
        [string]$metadata.packageFile,
        [string]$metadata.certificateFile))
    {
        if ([IO.Path]::IsPathRooted($payloadName) -or
            $payloadName.Contains('..') -or
            [IO.Path]::GetFileName($payloadName) -ne $payloadName)
        {
            throw 'Identity package metadata contains an unsafe payload path.'
        }
    }
    if ([string]$metadata.packageFile -ne "$metadataBaseName.msix" -or
        [string]$metadata.certificateFile -ne "$metadataBaseName.cer")
    {
        throw 'Identity package metadata names do not match the requested version.'
    }
    $certificate = New-Object Security.Cryptography.X509Certificates.X509Certificate2($certificatePath)
    if ($certificate.Thumbprint -ne [string]$metadata.certificateThumbprint)
    {
        throw 'Identity certificate thumbprint mismatch.'
    }
    if ($certificate.Subject -ne [string]$metadata.publisher)
    {
        throw 'Identity certificate subject mismatch.'
    }

    $existingCertificate = Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
        Where-Object { $_.Thumbprint -eq $certificate.Thumbprint } |
        Select-Object -First 1
    $certificateWasPresent = $null -ne $existingCertificate
    if (-not $certificateWasPresent)
    {
        $imported = Import-Certificate `
            -FilePath $certificatePath `
            -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople'
        if ($imported.Thumbprint -ne $certificate.Thumbprint)
        {
            throw 'Imported certificate thumbprint mismatch.'
        }
    }

    try
    {
        $signature = Get-AuthenticodeSignature -LiteralPath $packagePath
        $signatureInvalid = `
            ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid) -or `
            ($null -eq $signature.SignerCertificate) -or `
            ($signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint)
        if ($signatureInvalid)
        {
            throw "Sparse package signature is invalid: $($signature.StatusMessage)"
        }

        $archive = [IO.Compression.ZipFile]::OpenRead($packagePath)
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
    }
    catch
    {
        if (-not $certificateWasPresent)
        {
            $trusted = Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
                Where-Object { $_.Thumbprint -eq $certificate.Thumbprint } |
                Select-Object -First 1
            if ($null -ne $trusted)
            {
                Remove-Item -LiteralPath $trusted.PSPath -Force
            }
        }
        throw
    }

    return [ordered]@{
        metadata = $metadata
        packagePath = $packagePath
        certificateThumbprint = $certificate.Thumbprint
        certificateWasPresent = $certificateWasPresent
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
    $configAlreadyExisted = Test-Path -LiteralPath $configPath -PathType Leaf
    $hostPath = Join-Path $InstallRoot 'ba-click-fx-desktop.exe'
    $reportName = 'identity-installer-support.txt'
    Push-Location -LiteralPath $InstallRoot
    try
    {
        & $hostPath "--support-info=$reportName" | Out-Null
        if ($LASTEXITCODE -ne 0)
        {
            throw "Host configuration bootstrap failed with exit code $LASTEXITCODE."
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

    if ($DisableSystemBorder -and -not $configAlreadyExisted)
    {
        $config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
        if ($null -eq $config.background)
        {
            throw 'Generated configuration has no background object.'
        }
        $config.background.allowSystemBorder = $false
        Write-Utf8NoBom -Path $configPath -Content ($config | ConvertTo-Json -Depth 12)
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
    if ([int]$State.schema -ne 1)
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
    if (-not (Test-Path -LiteralPath $path -PathType Leaf))
    {
        return $null
    }
    Assert-ProtectedStateAcl -Path $path
    $state = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    return Assert-InstallStateObject `
        -State $state `
        -InstallRoot $InstallRoot `
        -ExpectedUserSid $UserSid
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

function Assert-PendingStateObject
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
        'userSid',
        'packageName',
        'applicationId',
        'publisher',
        'productVersion',
        'packageVersion',
        'packagePath',
        'packageFile',
        'certificateThumbprint',
        'certificateWasPresent',
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
    if ([string]$State.certificateThumbprint -notmatch '^[0-9A-Fa-f]{40}$' -or
        $State.certificateWasPresent -isnot [bool])
    {
        throw 'Protected pending state has invalid certificate data.'
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
    $sameVersionPackages = @(
        Get-AppxPackage -AllUsers -Name ([string]$State.packageName) -ErrorAction Stop |
            Where-Object { [string]$_.Version -eq [string]$State.packageVersion }
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
        return
    }
    if ($certificate.Subject -ne [string]$State.publisher)
    {
        throw 'Refusing to remove the prepared certificate with an unexpected subject.'
    }
    Remove-Item -LiteralPath $certificate.PSPath -Force
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
            throw 'A previous protected pending state still exists; complete rollback before retrying.'
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
        $identity = Assert-IdentityPayload -InstallRoot $installRoot
        $metadata = $identity.metadata
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
        if ($null -ne $oldInstallState -and
            $preexistingFullNames -notcontains [string]$oldInstallState.packageFullName)
        {
            throw 'Protected install state does not match the existing package registration.'
        }

        $packageFile = [IO.Path]::GetFileName([string]$identity.packagePath)
        $pendingState = [ordered]@{
            schema = 1
            stateKind = 'prepare'
            userSid = [string]$context.userSid
            packageName = [string]$metadata.packageName
            applicationId = [string]$metadata.applicationId
            publisher = [string]$metadata.publisher
            productVersion = $ProductVersion
            packageVersion = $PackageVersion
            packagePath = [string]$identity.packagePath
            packageFile = $packageFile
            certificateThumbprint = [string]$identity.certificateThumbprint
            certificateWasPresent = [bool]$identity.certificateWasPresent
            preexistingPackageFullNames = $preexistingFullNames
            oldInstallState = $oldInstallState
            preparedUtc = [DateTime]::UtcNow.ToString('o')
        }
        Write-ProtectedJson `
            -Path $machineStateFullPath `
            -Value $pendingState `
            -ReadSid ([string]$context.userSid)
        Initialize-IdentityConfig -InstallRoot $installRoot -DataDirectory $dataDirectory
        exit 0
    }
    catch
    {
        $prepareError = $_.Exception.Message
        if ($null -ne $pendingState)
        {
            try
            {
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
        elseif ($null -ne $identity -and -not [bool]$identity.certificateWasPresent)
        {
            $certificate = Get-ChildItem -Path 'Cert:\LocalMachine\TrustedPeople' |
                Where-Object { $_.Thumbprint -eq [string]$identity.certificateThumbprint } |
                Select-Object -First 1
            if ($null -ne $certificate)
            {
                Remove-Item -LiteralPath $certificate.PSPath -Force
            }
        }
        throw $prepareError
    }
}

Assert-ProtectedStateAcl -Path $machineStateFullPath
$pendingState = Get-Content -LiteralPath $machineStateFullPath -Raw | ConvertFrom-Json
$pendingState = Assert-PendingStateObject -State $pendingState -InstallRoot $installRoot
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
    $installState = [ordered]@{
        schema = 1
        packageName = [string]$pendingState.packageName
        applicationId = [string]$pendingState.applicationId
        publisher = [string]$pendingState.publisher
        productVersion = $ProductVersion
        packageVersion = $PackageVersion
        packageFullName = [string]$registrationResult.packageFullName
        packageFamilyName = [string]$registrationResult.packageFamilyName
        certificateThumbprint = [string]$pendingState.certificateThumbprint
        certificateInstalledBySetup = [bool]$certificateInstalledBySetup
        externalLocation = $installRoot
        installedUserSid = [string]$pendingState.userSid
        packageFile = [string]$pendingState.packageFile
        installedUtc = [DateTime]::UtcNow.ToString('o')
    }
    Write-ProtectedJson -Path $installStatePath -Value $installState -ReadSid $null
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

# Cleanup after the protected commit is best effort. A stale old certificate
# or payload is safer than rolling back an already committed registration.
try
{
    Remove-OldCertificate `
        -State $pendingState.oldInstallState `
        -CurrentCertificateThumbprint ([string]$pendingState.certificateThumbprint)
}
catch
{
    Write-Warning "Could not remove the previous installer certificate: $($_.Exception.Message)"
}
try
{
    $identityDirectory = Join-Path $installRoot 'Identity'
    $obsoleteRegistrations = @(
        Get-AppxPackage -AllUsers -Name ([string]$pendingState.packageName) -ErrorAction Stop |
            Where-Object {
                [string]$_.PackageFullName -ne [string]$registrationResult.packageFullName
            }
    )
    if ($obsoleteRegistrations.Count -gt 0)
    {
        Write-Warning 'Keeping previous identity payloads while another registration still uses them.'
    }
    else
    {
        $currentNames = @(
            [string]$pendingState.packageFile,
            "CialloKing.BaClickFxDesktop-$PackageVersion.cer",
            "CialloKing.BaClickFxDesktop-$PackageVersion.identity.json"
        )
        Get-ChildItem -LiteralPath $identityDirectory -File |
            Where-Object {
                $isIdentityFile = $_.Name -like 'CialloKing.BaClickFxDesktop-*'
                $isObsolete = $currentNames -notcontains $_.Name
                $hasExpectedExtension = $_.Extension -in @('.msix', '.cer', '.json')
                $isIdentityFile -and $isObsolete -and $hasExpectedExtension
            } |
            Remove-Item -Force
    }
}
catch
{
    Write-Warning "Could not remove obsolete identity payloads: $($_.Exception.Message)"
}
