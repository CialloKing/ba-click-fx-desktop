[CmdletBinding()]
param(
    [string]$RepositoryRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-RepositoryPath
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$RelativePath
    )

    return [IO.Path]::GetFullPath((Join-Path $repositoryRoot $RelativePath))
}

function Read-RepositoryText
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$RelativePath
    )

    $path = Resolve-RepositoryPath -RelativePath $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf))
    {
        throw "Required repository file is missing: $RelativePath"
    }
    return Get-Content -LiteralPath $path -Raw
}

function Assert-True
{
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,

        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition)
    {
        throw $Message
    }
}

function Assert-TextContains
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text,

        [Parameter(Mandatory = $true)]
        [string]$Pattern,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    Assert-True `
        -Condition ($Text -match $Pattern) `
        -Message "Installer contract is missing: $Description"
}

function Assert-TextExcludes
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text,

        [Parameter(Mandatory = $true)]
        [string]$Pattern,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    Assert-True `
        -Condition ($Text -notmatch $Pattern) `
        -Message "Installer contract contains forbidden content: $Description"
}

function Get-ParsedScript
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$RelativePath
    )

    $path = Resolve-RepositoryPath -RelativePath $RelativePath
    $tokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile(
        $path,
        [ref]$tokens,
        [ref]$parseErrors)
    if ($parseErrors.Count -ne 0)
    {
        $messages = @($parseErrors | ForEach-Object { $_.Message }) -join '; '
        throw "PowerShell parse failed for ${RelativePath}: $messages"
    }
    return $ast
}

function Get-FunctionText
{
    param(
        [Parameter(Mandatory = $true)]
        [Management.Automation.Language.Ast]$Ast,

        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    $function = $Ast.Find(
        {
            param($node)
            return $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
                $node.Name -eq $Name
        },
        $true) | Select-Object -First 1
    if ($null -eq $function)
    {
        throw "Required PowerShell function is missing: $Name"
    }
    return $function.Extent.Text
}

function Assert-ArrayEquals
{
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Expected,

        [Parameter(Mandatory = $true)]
        [string[]]$Actual,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $expectedSorted = @($Expected | Sort-Object)
    $actualSorted = @($Actual | Sort-Object)
    $difference = @(
        Compare-Object `
            -ReferenceObject $expectedSorted `
            -DifferenceObject $actualSorted
    )
    if ($difference.Count -ne 0)
    {
        $details = @($difference | ForEach-Object { "$($_.SideIndicator):$($_.InputObject)" }) -join ', '
        throw "$Description differs: $details"
    }
}

function Assert-Throws
{
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock]$Action,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $threw = $false
    try
    {
        $null = & $Action
    }
    catch
    {
        $threw = $true
    }
    Assert-True -Condition $threw -Message "Expected failure did not occur: $Description"
}

function Get-CompressionRuntimeLoadStatements
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstallMachine
    )

    $matches = @(
        [regex]::Matches(
            $InstallMachine,
            '(?m)^[ \t]*Add-Type\s+-AssemblyName\s+System\.IO\.Compression(?:\.FileSystem)?[ \t]*$')
    )
    Assert-True `
        -Condition ($matches.Count -eq 2) `
        -Message 'Installer compression assemblies must load once at startup.'
    return @($matches | ForEach-Object { $_.Value.Trim() })
}

function Test-VersionMapping
{
    $scriptPath = 'tools/package-user-installer.ps1'
    $ast = Get-ParsedScript -RelativePath $scriptPath
    $functionText = Get-FunctionText -Ast $ast -Name 'Get-NumericVersions'
    . ([scriptblock]::Create($functionText))

    $currentVersionText = Read-RepositoryText -RelativePath 'cmake/Version.cmake'
    $currentMatch = [regex]::Match($currentVersionText, 'BAFX_VERSION\s+"([^"]+)"')
    Assert-True -Condition $currentMatch.Success -Message 'cmake/Version.cmake has no BAFX_VERSION.'

    $current = Get-NumericVersions -Version $currentMatch.Groups[1].Value
    $expectedCurrent = $currentMatch.Groups[1].Value + '.0'
    Assert-True `
        -Condition ([string]$current.numericVersion -eq $expectedCurrent) `
        -Message "Current version mapping is incorrect: $($currentMatch.Groups[1].Value)"
    Assert-True `
        -Condition ([string]$current.packageVersion -eq $expectedCurrent) `
        -Message "Current package version mapping is incorrect: $($currentMatch.Groups[1].Value)"

    $sample = Get-NumericVersions -Version '12.34.56'
    Assert-True `
        -Condition ([string]$sample.numericVersion -eq '12.34.56.0') `
        -Message 'Three-component versions must map to a zero Windows revision.'
    Assert-True `
        -Condition ([string]$sample.packageVersion -eq '12.34.56.0') `
        -Message 'PackageVersion must match the numeric Windows version.'

    Assert-Throws `
        -Action { Get-NumericVersions -Version '12.34.56-alpha.1' } `
        -Description 'alpha prerelease'
    Assert-Throws `
        -Action { Get-NumericVersions -Version '12.34.56-beta.1' } `
        -Description 'beta prerelease'
    Assert-Throws `
        -Action { Get-NumericVersions -Version '12.34.56.0' } `
        -Description 'four-component version'
}

function Test-PowerShellScriptContracts
{
    $scriptPaths = @(
        'tools/package-user-installer.ps1',
        'tools/installer/capture-user-context.ps1',
        'tools/installer/installer-diagnostics.ps1',
        'tools/installer/install-machine.ps1',
        'tools/installer/protected-paths.ps1',
        'tools/installer/register-user-package.ps1',
        'tools/installer/unregister-machine.ps1',
        'tools/verify-user-installer-contract.ps1'
    )
    foreach ($scriptPath in $scriptPaths)
    {
        Get-ParsedScript -RelativePath $scriptPath | Out-Null
    }

    $protectedPaths = Read-RepositoryText `
        -RelativePath 'tools/installer/protected-paths.ps1'
    Assert-TextContains `
        -Text $protectedPaths `
        -Pattern 'function\s+Test-InstallerTrustedPrincipal[\s\S]*S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464' `
        -Description 'protected tree ACLs allow only the Windows TrustedInstaller service identity'
    $protectedPathsAst = Get-ParsedScript `
        -RelativePath 'tools/installer/protected-paths.ps1'
    . ([scriptblock]::Create((Get-FunctionText `
            -Ast $protectedPathsAst `
            -Name 'Test-InstallerTrustedPrincipal')))
    Assert-True `
        -Condition (Test-InstallerTrustedPrincipal `
            -Sid 'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464') `
        -Message 'TrustedInstaller must be accepted as a protected-tree writer.'
    Assert-True `
        -Condition (-not (Test-InstallerTrustedPrincipal -Sid 'S-1-5-32-545')) `
        -Message 'Interactive Users must not be accepted as a protected-tree writer.'
}

function Test-CertificateCreationRecoveryContract
{
    $machineAst = Get-ParsedScript `
        -RelativePath 'tools/installer/install-machine.ps1'
    $snapshotText = Get-FunctionText `
        -Ast $machineAst `
        -Name 'Assert-CertificateStoreSnapshot'
    $pendingText = Get-FunctionText `
        -Ast $machineAst `
        -Name 'Assert-PendingStateObject'
    $recoveryText = Get-FunctionText `
        -Ast $machineAst `
        -Name 'Recover-CreatingCertificate'
    $cleanupText = Get-FunctionText `
        -Ast $machineAst `
        -Name 'Invoke-PendingRollbackCleanup'

    Assert-TextContains `
        -Text $recoveryText `
        -Pattern 'no ownership snapshot[sS]*throw[sS]*Assert-CertificateStoreSnapshot[sS]*matchingCertificateKeys[sS]*-gts+1' `
        -Description 'certificate recovery retains ambiguous ownership evidence'
    Assert-TextContains `
        -Text $recoveryText `
        -Pattern 'recorded certificate does not carry the transaction SAN marker[sS]*recorded certificate hash does not match' `
        -Description 'certificate recovery validates the recorded SAN and DER hash'
    Assert-TextContains `
        -Text $cleanupText `
        -Pattern 'Recover-CreatingCertificates+-States+$State[sS]*Test-Paths+-LiteralPaths+$PendingPath' `
        -Description 'pending cleanup deletes its journal only after certificate recovery'
    Assert-TextContains `
        -Text (Get-FunctionText -Ast $machineAst -Name 'Assert-PendingStateObject') `
        -Pattern 'certificateSanUris+-cne[sS]*urn:bafx:installer' `
        -Description 'creating journals bind the SAN marker to the transaction id'
    Assert-TextContains `
        -Text (Read-RepositoryText -RelativePath 'tools/installer/install-machine.ps1') `
        -Pattern 'TextExtension\s+@\(\x222\.5\.29\.17=\{text\}URL=\$certificateSanUri\x22\)' `
        -Description 'certificate SAN uses the CertEnroll URI GeneralName token'

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

    . ([scriptblock]::Create($snapshotText))
    . ([scriptblock]::Create($pendingText))
    $validSnapshot = ('1' * 40) + ':' + ('2' * 64)
    Assert-CertificateStoreSnapshot -Snapshot $validSnapshot
    Assert-Throws `
        -Action {
            Assert-CertificateStoreSnapshot `
                -Snapshot (('1' * 40) + ':bad')
        } `
        -Description 'malformed certificate snapshot'
    Assert-Throws `
        -Action {
            Assert-CertificateStoreSnapshot `
                -Snapshot ($validSnapshot + '|' + $validSnapshot)
        } `
        -Description 'duplicate certificate snapshot evidence'

    $transactionId = 'a' * 32
    $creatingState = [pscustomobject]@{
        schema = 1
        stateKind = 'prepare'
        transactionId = $transactionId
        userSid = 'S-1-5-21-1-2-3-1001'
        packageName = 'CialloKing.BaClickFxDesktop'
        applicationId = 'BaClickFxDesktop'
        publisher = 'CN=BaClickFx.Local'
        productVersion = '1.2.3'
        packageVersion = '1.2.3.0'
        preexistingPackageFullNames = @()
        oldInstallState = $null
        certificatePhase = 'creating'
        certificateSanUri = "urn:bafx:installer:$transactionId"
    }
    Assert-PendingStateObject `
        -State $creatingState `
        -InstallRoot 'C:\Program Files\ba-click-fx-desktop' | Out-Null
    $creatingState.certificateSanUri = 'urn:bafx:installer:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'
    Assert-Throws `
        -Action {
            Assert-PendingStateObject `
                -State $creatingState `
                -InstallRoot 'C:\Program Files\ba-click-fx-desktop' | Out-Null
        } `
        -Description 'certificate SAN marker bound to another transaction'
}

function Test-CertificateLifecycleBoundaryContract
{
    $ast = Get-ParsedScript `
        -RelativePath 'tools/installer/install-machine.ps1'
    $moduleText = @(
        'Set-StrictMode -Version Latest'
        "`$ErrorActionPreference = 'Stop'"
        @'
$script:TrustedCertificate = $null
$script:PrivateCertificates = @()
$script:TrustedCertificates = @()
$script:RemovedCertificatePaths = New-Object Collections.Generic.List[string]
$script:FailPrivateCertificateDeletion = $false

function Get-TrustedCertificateByThumbprint
{
    param([string]$Thumbprint)
    return $script:TrustedCertificate
}

function Get-CertificateSha256
{
    param([object]$Certificate)
    return [string]$Certificate.sha256
}

function Test-Path
{
    param([string]$LiteralPath, [string]$PathType)
    return $true
}

function Get-FileHash
{
    param([string]$LiteralPath, [string]$Algorithm)
    return [pscustomobject]@{ Hash = 'C' * 64 }
}

function Get-AuthenticodeSignature
{
    param([string]$LiteralPath)
    return [pscustomobject]@{
        Status = [Management.Automation.SignatureStatus]::Valid
        SignerCertificate = [pscustomobject]@{ Thumbprint = 'A' * 40 }
    }
}

function Get-ChildItem
{
    param([string]$Path)
    if ($Path -eq 'Cert:\LocalMachine\My')
    {
        return @($script:PrivateCertificates)
    }
    if ($Path -eq 'Cert:\LocalMachine\TrustedPeople')
    {
        return @($script:TrustedCertificates)
    }
    return @()
}

function Remove-Item
{
    param(
        [string]$LiteralPath,
        [switch]$DeleteKey,
        [switch]$Force
    )
    [void]$script:RemovedCertificatePaths.Add($LiteralPath)
    if ($DeleteKey -and $script:FailPrivateCertificateDeletion)
    {
        throw 'injected private-key deletion failure'
    }
    $script:PrivateCertificates = @(
        $script:PrivateCertificates | Where-Object { $_.PSPath -ne $LiteralPath })
    $script:TrustedCertificates = @(
        $script:TrustedCertificates | Where-Object { $_.PSPath -ne $LiteralPath })
}

function Test-CertificateSanUri
{
    param([object]$Certificate, [string]$SanUri)
    return [string]$Certificate.sanUri -ceq $SanUri
}

function Invoke-CertificateLifecycleProbe
{
}
'@
        (Get-FunctionText -Ast $ast -Name 'Split-Ledger')
        (Get-FunctionText -Ast $ast -Name 'Assert-CertificateStoreSnapshot')
        (Get-FunctionText -Ast $ast -Name 'Test-CertificateStoreSnapshotContains')
        (Get-FunctionText -Ast $ast -Name 'Test-ExistingIdentityPackageReusable')
        (Get-FunctionText -Ast $ast -Name 'Remove-CertificateFromStores')
        (Get-FunctionText -Ast $ast -Name 'Recover-CreatingCertificate')
        'Export-ModuleMember -Function Invoke-CertificateLifecycleProbe'
    ) -join "`n"
    $probeModule = New-Module -ScriptBlock ([scriptblock]::Create($moduleText))
    try
    {
        $nowUtc = [DateTime]::SpecifyKind(
            [DateTime]::new(2030, 1, 1, 0, 0, 0),
            [DateTimeKind]::Utc)
        $oldState = [pscustomobject]@{
            productVersion = '1.2.3'
            packageVersion = '1.2.3.0'
            hostSha256 = 'B' * 64
            certificateThumbprint = 'A' * 40
            certificateSha256 = 'D' * 64
            packageFile = 'identity.msix'
            packageSha256 = 'C' * 64
            certificateOwnership = 'preexisting'
        }
        $metadata = [pscustomobject]@{ hostSha256 = 'B' * 64 }

        function Invoke-ReuseProbe
        {
            param([AllowNull()][object]$Certificate)

            return & $probeModule {
                param($State, $Metadata, $CertificateValue, $Now)
                $script:TrustedCertificate = $CertificateValue
                Test-ExistingIdentityPackageReusable `
                    -OldState $State `
                    -Metadata $Metadata `
                    -InstallRoot 'C:\Program Files\ba-click-fx-desktop' `
                    -ProductVersion '1.2.3' `
                    -PackageVersion '1.2.3.0' `
                    -NowUtc $Now
            } $oldState $metadata $Certificate $nowUtc
        }

        $certificate = [pscustomobject]@{
            Thumbprint = 'A' * 40
            sha256 = 'D' * 64
            NotBefore = $nowUtc.AddDays(-1)
            NotAfter = $nowUtc.AddDays(31)
        }
        $reusable = Invoke-ReuseProbe -Certificate $certificate
        Assert-True `
            -Condition ($null -ne $reusable -and
                [string]$reusable.certificateOwnership -eq 'preexisting') `
            -Message 'A valid pre-existing 31-day certificate was not reused.'

        $certificate.NotAfter = $nowUtc.AddDays(30)
        Assert-True `
            -Condition ($null -ne (Invoke-ReuseProbe -Certificate $certificate)) `
            -Message 'A certificate with exactly 30 days remaining was rotated.'

        foreach ($notAfter in @($nowUtc.AddDays(30).AddTicks(-1), $nowUtc.AddDays(-1)))
        {
            $certificate.NotAfter = $notAfter
            Assert-True `
                -Condition ($null -eq (Invoke-ReuseProbe -Certificate $certificate)) `
                -Message 'A near-expiry or expired certificate was incorrectly reused.'
        }
        Assert-True `
            -Condition ($null -eq (Invoke-ReuseProbe -Certificate $null)) `
            -Message 'A missing trusted certificate was incorrectly reused.'

        $transactionId = 'e' * 32
        $sanUri = "urn:bafx:installer:$transactionId"
        $newCertificate = [pscustomobject]@{
            Thumbprint = 'F' * 40
            sha256 = '1' * 64
            Subject = 'CN=BaClickFx.Local'
            sanUri = $sanUri
            PSPath = 'Cert:\LocalMachine\My\new'
        }
        $pendingCreation = [pscustomobject]@{
            transactionId = $transactionId
            publisher = 'CN=BaClickFx.Local'
            certificatePhase = 'creating'
            certificateSanUri = $sanUri
            certificatePreexisting = ''
            certificateThumbprint = ''
            certificateSha256 = ''
        }
        & $probeModule {
            param($Certificate, $State)
            $script:PrivateCertificates = @($Certificate)
            $script:TrustedCertificates = @()
            $script:RemovedCertificatePaths.Clear()
            $script:FailPrivateCertificateDeletion = $false
            Recover-CreatingCertificate -State $State
        } $newCertificate $pendingCreation
        $afterCrashRecovery = & $probeModule {
            return [pscustomobject]@{
                privateCount = $script:PrivateCertificates.Count
                removedCount = $script:RemovedCertificatePaths.Count
            }
        }
        Assert-True `
            -Condition ($afterCrashRecovery.privateCount -eq 0 -and
                $afterCrashRecovery.removedCount -eq 1) `
            -Message 'A post-creation certificate was not removed during crash recovery.'

        $preexistingTrusted = [pscustomobject]@{
            Thumbprint = '9' * 40
            sha256 = '2' * 64
            Subject = 'CN=BaClickFx.Local'
            sanUri = $sanUri
            PSPath = 'Cert:\LocalMachine\TrustedPeople\preexisting'
        }
        $preexistingState = [pscustomobject]@{
            transactionId = $transactionId
            publisher = 'CN=BaClickFx.Local'
            certificatePhase = 'creating'
            certificateSanUri = $sanUri
            certificatePreexisting = (('9' * 40) + ':' + ('2' * 64))
            certificateThumbprint = ''
            certificateSha256 = ''
        }
        & $probeModule {
            param($Certificate, $State)
            $script:PrivateCertificates = @()
            $script:TrustedCertificates = @($Certificate)
            $script:RemovedCertificatePaths.Clear()
            Recover-CreatingCertificate -State $State
        } $preexistingTrusted $preexistingState
        $preexistingPreserved = & $probeModule {
            return $script:TrustedCertificates.Count -eq 1 -and
                $script:RemovedCertificatePaths.Count -eq 0
        }
        Assert-True `
            -Condition $preexistingPreserved `
            -Message 'Crash recovery removed a pre-existing TrustedPeople certificate.'

        $ambiguousCertificate = [pscustomobject]@{
            Thumbprint = '8' * 40
            sha256 = '3' * 64
            Subject = 'CN=BaClickFx.Local'
            sanUri = $sanUri
            PSPath = 'Cert:\LocalMachine\TrustedPeople\ambiguous'
        }
        Assert-Throws `
            -Action {
                & $probeModule {
                    param($First, $Second, $State)
                    $script:PrivateCertificates = @($First)
                    $script:TrustedCertificates = @($Second)
                    $script:RemovedCertificatePaths.Clear()
                    Recover-CreatingCertificate -State $State
                } $newCertificate $ambiguousCertificate $pendingCreation
            } `
            -Description 'multiple certificate SAN transaction markers'
        $ambiguousPreserved = & $probeModule {
            return $script:RemovedCertificatePaths.Count -eq 0
        }
        Assert-True `
            -Condition $ambiguousPreserved `
            -Message 'Ambiguous SAN recovery deleted certificate evidence.'

        $trustedCopy = [pscustomobject]@{
            Thumbprint = 'F' * 40
            sha256 = '1' * 64
            Subject = 'CN=BaClickFx.Local'
            sanUri = $sanUri
            PSPath = 'Cert:\LocalMachine\TrustedPeople\new'
        }
        Assert-Throws `
            -Action {
                & $probeModule {
                    param($Private, $Trusted)
                    $script:PrivateCertificates = @($Private)
                    $script:TrustedCertificates = @($Trusted)
                    $script:RemovedCertificatePaths.Clear()
                    $script:FailPrivateCertificateDeletion = $true
                    Remove-CertificateFromStores `
                        -Thumbprint ('F' * 40) `
                        -ExpectedSubject 'CN=BaClickFx.Local'
                } $newCertificate $trustedCopy
            } `
            -Description 'private-key deletion failure'
        $privateFailureSafe = & $probeModule {
            return $script:RemovedCertificatePaths.Count -eq 1 -and
                $script:RemovedCertificatePaths[0] -eq 'Cert:\LocalMachine\My\new' -and
                $script:TrustedCertificates.Count -eq 1
        }
        Assert-True `
            -Condition $privateFailureSafe `
            -Message 'Private-key deletion failure removed the trusted certificate.'
    }
    finally
    {
        if ($null -ne $probeModule)
        {
            Remove-Module -ModuleInfo $probeModule -Force -ErrorAction SilentlyContinue
        }
    }
}

function Test-UninstallerProcessPathFilter
{
    $scriptPath = 'tools/installer/unregister-machine.ps1'
    $ast = Get-ParsedScript -RelativePath $scriptPath
    $functionText = Get-FunctionText -Ast $ast -Name 'Assert-ExpectedProcessIsStopped'

    # PowerShell accepts a line-leading -and as a command during parsing, so this
    # function must run to catch the failure mode seen by the shipped uninstaller.
    $mockExecutablePath = 'D:\Portable\ba-click-fx-desktop.exe'
    function Get-CimInstance
    {
        param(
            [Parameter(Position = 0)]
            [string]$ClassName,

            [string]$Filter
        )

        return [pscustomobject]@{
            ExecutablePath = $mockExecutablePath
        }
    }
    . ([scriptblock]::Create($functionText))

    $installedExecutablePath =
        'C:\Program Files\ba-click-fx-desktop\ba-click-fx-desktop.exe'
    Assert-ExpectedProcessIsStopped -ExecutablePath $installedExecutablePath

    $mockExecutablePath = $installedExecutablePath
    Assert-Throws `
        -Action {
            Assert-ExpectedProcessIsStopped -ExecutablePath $installedExecutablePath
        } `
        -Description 'installed Host process still running'
}

function Test-InstallerScriptWhitelist
{
    $installerRoot = Resolve-RepositoryPath -RelativePath 'tools/installer'
    $expectedFiles = @(
        'ba-click-fx-desktop.iss',
        'capture-user-context.ps1',
        'ChineseSimplified.isl',
        'installer-diagnostics.ps1',
        'install-machine.ps1',
        'protected-paths.ps1',
        'register-user-package.ps1',
        'unregister-machine.ps1'
    )
    $actualFiles = @(Get-ChildItem -LiteralPath $installerRoot -File | ForEach-Object { $_.Name })
    Assert-ArrayEquals `
        -Expected $expectedFiles `
        -Actual $actualFiles `
        -Description 'tools/installer file whitelist'

    $packager = Read-RepositoryText -RelativePath 'tools/package-user-installer.ps1'
    Assert-TextContains `
        -Text $packager `
        -Pattern (('\$scriptName\s+in\s+@\(\s*' +
            '\x27capture-user-context\.ps1\x27\s*,\s*' +
            '\x27installer-diagnostics\.ps1\x27\s*,\s*' +
            '\x27install-machine\.ps1\x27\s*,\s*' +
            '\x27protected-paths\.ps1\x27\s*,\s*' +
            '\x27register-user-package\.ps1\x27\s*,\s*' +
            '\x27unregister-machine\.ps1\x27\s*\)')) `
        -Description 'explicit runtime installer script whitelist'
    Assert-TextContains `
        -Text $packager `
        -Pattern 'Join-Path\s+\$PSScriptRoot\s+"installer\\\$scriptName"' `
        -Description 'runtime scripts are copied from the installer directory'
    Assert-TextContains `
        -Text $packager `
        -Pattern 'INSTALLER-PAYLOAD\.json' `
        -Description 'payload hash manifest generation'
    Assert-TextContains `
        -Text $packager `
        -Pattern 'FullName\s+-ne\s+\(Join-Path\s+\$installerDirectory\s+\x27INSTALLER-PAYLOAD\.json\x27\)' `
        -Description 'payload manifest does not hash itself'
    Assert-TextContains `
        -Text $packager `
        -Pattern 'schema\s*=\s*2[\s\S]*identityMode\s*=\s*\x27target-machine-self-signed\x27[\s\S]*files\s*=\s*\$payloadFiles' `
        -Description 'payload manifest schema and file hashes'
    Assert-TextContains `
        -Text $packager `
        -Pattern 'Assert-ExecutableVersion[\s\S]*-NumericVersion\s+\$numericVersion' `
        -Description 'Host and Control Center version-resource verification'
    Assert-TextContains `
        -Text $packager `
        -Pattern 'Assert-IsccDiagnosticsSupport[\s\S]*minimumVersion\s*=\s*\[Version\]\x276\.3\.0\x27' `
        -Description 'Inno compiler supports output capture and uninstall logging'

    $installMachine = Read-RepositoryText -RelativePath 'tools/installer/install-machine.ps1'
    $installMachineAst = Get-ParsedScript `
        -RelativePath 'tools/installer/install-machine.ps1'
    $splitLedgerText = Get-FunctionText `
        -Ast $installMachineAst `
        -Name 'Split-Ledger'
    $splitLedgerModule = New-Module -ScriptBlock ([scriptblock]::Create(
        "Set-StrictMode -Version Latest`n$splitLedgerText"))
    try
    {
        $legacyPackageLedger = 'old.msix current.msix|current.msix'
        $legacyPackageFiles = @(& $splitLedgerModule {
                param($Value)
                Split-Ledger -Value $Value -Separator Pipe
            } $legacyPackageLedger)
        Assert-ArrayEquals `
            -Expected @('old.msix', 'current.msix', 'current.msix') `
            -Actual $legacyPackageFiles `
            -Description 'legacy PowerShell 5.1 package ledger migration'
    }
    finally
    {
        if ($null -ne $splitLedgerModule)
        {
            Remove-Module -ModuleInfo $splitLedgerModule -Force `
                -ErrorAction SilentlyContinue
        }
    }
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'function\s+Get-IdentityTemplateContentHash' `
        -Description 'canonical identity package content hashing'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'Get-IdentityTemplateContentHash\s+-Path\s+\$oldPackagePath[\s\S]*identity package content changed' `
        -Description 'same-version repair compares package semantics'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'function\s+Remove-PreparedCertificateIfUnused[\s\S]*Test-RegisteredPackageUsesCertificate' `
        -Description 'rollback certificate cleanup checks all shared package users'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'function\s+Remove-PendingPackageFiles[\s\S]*Remove-Item\s+-LiteralPath\s+\$candidate[\s\S]*function\s+Invoke-PendingRollbackCleanup' `
        -Description 'rollback removes the prepared signed package file'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'Start-Process[\s\S]*-Wait[\s\S]*\$hostProcess\.ExitCode' `
        -Description 'identity bootstrap uses an explicit GUI process exit code'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'Write-BafxInstallerFailure[\s\S]*-Phase\s+\$Phase[\s\S]*-Step\s+\$script:InstallerStep' `
        -Description 'machine phases emit structured failure diagnostics'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'Add-InstallerRelatedFailure[\s\S]*rollback-failed-prepare' `
        -Description 'prepare rollback failures remain secondary to the root cause'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern '\$prepareRollbackSucceeded\)\s*\{\s*1002\s*\}\s*else\s*\{\s*1001' `
        -Description 'prepare failures return rollback-success and recovery-retained exit codes'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'stateCommitted[\s\S]*ExitCode\s+1001' `
        -Description 'committed finalize cleanup failures retain recovery state'
    Assert-TextExcludes `
        -Text $installMachine `
        -Pattern 'throw\s+\$(prepare|finalize)Error\b' `
        -Description 'lossy machine failure string rethrow'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'hostEntries[\s\S]*Count\s+-ne\s+1[\s\S]*return\s+\(\[string\]\$hostEntries\[0\]\.sha256\)' `
        -Description 'validated payload identifies exactly one replacement Host hash'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern '\$replacementHostSha256\s*=\s*Assert-PayloadManifest[\s\S]*read-existing-install-state[\s\S]*\-ExpectedReplacementHostSha256\s+\$replacementHostSha256' `
        -Description 'Prepare binds old-state validation to the replacement Host hash'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'State\.productVersion\s+-ne\s+\$ProductVersion\s+-or[\s\S]*State\.packageVersion\s+-ne\s+\$PackageVersion' `
        -Description 'replacement Host allowance covers any version transition'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'Assert-ReplacementHostIntegrity[\s\S]*\-CurrentHostSha256\s+\$currentHostSha256[\s\S]*\-ArchivedHostSha256\s+\$archivedHostHash' `
        -Description 'live and archived Host hashes use the replacement integrity contract'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern "InstallerStep\s*=\s*'load-compression-runtime'[\s\S]*Add-Type\s+-AssemblyName\s+System\.IO\.Compression[\s\S]*Add-Type\s+-AssemblyName\s+System\.IO\.Compression\.FileSystem[\s\S]*InstallerStep\s*=\s*'resolve-installer-paths'" `
        -Description 'ZIP runtime loads before any existing install state is read'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'function\s+Get-CertificateStoreSnapshot[\s\S]*Cert:\\LocalMachine\\\$storeName[\s\S]*My[\s\S]*TrustedPeople' `
        -Description 'certificate ownership snapshots both machine stores'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'certificatePreexisting\s*=\s*\$certificateStoreSnapshot[\s\S]*Test-CertificateStoreSnapshotContains[\s\S]*certificateWasPresent' `
        -Description 'certificate presence is decided from thumbprint and DER evidence before import'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'certificateOwnership\s*=\s*''unknown''[\s\S]*ownedCertificateThumbprints\s*=\s*''''[\s\S]*certificateOwnership\s*=\s*if' `
        -Description 'certificate ownership ledger is not optimistic during the creation window'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'minimumReusableNotAfterUtc\s*=\s*\$nowUtc\.AddDays\(30\)[\s\S]*NotAfter\.ToUniversalTime\(\)\s+-le\s+\$nowUtc[\s\S]*-lt\s+\$minimumReusableNotAfterUtc' `
        -Description 'certificate reuse rejects expired and under-thirty-day certificates'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'Recover-CreatingCertificate[\s\S]*certificatePreexisting[\s\S]*Test-CertificateStoreSnapshotContains' `
        -Description 'certificate creation crash recovery preserves pre-existing SAN certificates'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'function\s+Test-CertificateSanUri[\s\S]*RawData[\s\S]*tag\s+-eq\s+0x86[\s\S]*expectedBytes\.Length' `
        -Description 'certificate recovery matches an exact SAN URI DER value'
    Assert-TextExcludes `
        -Text (Get-FunctionText `
            -Ast (Get-ParsedScript `
                -RelativePath 'tools/installer/install-machine.ps1') `
            -Name 'Test-CertificateSanUri') `
        -Pattern '\.Contains\s*\(' `
        -Description 'substring SAN URI matching'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'function\s+Test-RegisteredPackageUsesCertificate[\s\S]*Get-AppxPackage\s+-AllUsers[\s\S]*PackageUserInformation[\s\S]*Get-AuthenticodeSignature' `
        -Description 'certificate cleanup scans every registered user and package version'
    Assert-TextExcludes `
        -Text $installMachine `
        -Pattern '\$sameVersionPackages' `
        -Description 'certificate cleanup does not filter shared packages by one version'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'function\s+Assert-PayloadRollbackManifest[\s\S]*stateDigest[\s\S]*duplicate file path[\s\S]*Assert-RollbackManifestBackup' `
        -Description 'rollback manifest validates its digest, paths, and backup evidence'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'function\s+Read-PayloadRollbackManifest[\s\S]*Assert-PayloadRollbackManifest' `
        -Description 'all rollback restore paths revalidate the manifest'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'function\s+Ensure-RollbackEvidenceAcl[\s\S]*Assert-RollbackEvidenceAclCanBeHardened[\s\S]*Set-ProtectedStateAcl' `
        -Description 'legacy rollback evidence ACLs are hardened only after write-access checks'
    $protectedPaths = Read-RepositoryText `
        -RelativePath 'tools/installer/protected-paths.ps1'
    Assert-TextContains `
        -Text $protectedPaths `
        -Pattern 'function\s+Assert-NoReparsePath[\s\S]*Get-Item\s+-LiteralPath\s+\$current[\s\S]*FileAttributes\]::ReparsePoint' `
        -Description 'installer paths reject reparse points component by component'
    Assert-TextContains `
        -Text $protectedPaths `
        -Pattern 'function\s+Assert-NoReparseTree[\s\S]*Get-ChildItem\s+-LiteralPath\s+\$current[\s\S]*FileAttributes\]::ReparsePoint' `
        -Description 'recursive deletion trees reject reparse points before traversal'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'function\s+Write-ProtectedJson[\s\S]*Assert-NoReparsePath\s+-Path\s+\$resolvedPath[\s\S]*function\s+Write-ProtectedInstallState[\s\S]*Assert-NoReparsePath\s+-Path\s+\$resolvedPath' `
        -Description 'machine state writers revalidate their destination paths'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'function\s+Assert-ProtectedPayloadAcl[\s\S]*Stack\[string\][\s\S]*cannot contain a reparse point' `
        -Description 'staging traversal stops before entering a reparse directory'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'function\s+Copy-VerifiedInstallerFile[\s\S]*Assert-NoReparsePath\s+-Path\s+\$SourcePath[\s\S]*DestinationPath\s+-AllowMissing[\s\S]*Copy-Item[\s\S]*Assert-NoReparsePath\s+-Path\s+\$DestinationPath' `
        -Description 'verified copies reject source and destination reparse points'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'function\s+New-PayloadRollbackManifest[\s\S]*rollbackParent[\s\S]*Assert-NoReparsePath[\s\S]*rollbackRoot[\s\S]*Assert-NoReparsePath' `
        -Description 'rollback roots are validated before manifest evidence is written'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'function\s+Assert-PayloadFileSetMatchesState' `
        -Description 'payload file-set ledger validation is implemented'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'Assert-PayloadManifest\s+-InstallRoot\s+\$PayloadRoot[\s\S]*Assert-PayloadFileSetMatchesState' `
        -Description 'commit validates the payload file-set ledger before writing live files'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'payloadFileSet\s*=\s*\[string\]\$script:PayloadFileSet' `
        -Description 'rollback manifests carry a scalar payload file-set ledger'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'function\s+Assert-PayloadFileSetAgreement[\s\S]*stateFileSet[\s\S]*manifestFileSet[\s\S]*entryFileSet' `
        -Description 'rollback manifests carry a scalar payload file-set ledger'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'previousStatePrimaryBytes[\s\S]*previousStatePrimarySha256[\s\S]*previousStateBackupBytes[\s\S]*previousStateBackupSha256' `
        -Description 'rollback manifest records previous state backup evidence'
    $null = Get-CompressionRuntimeLoadStatements -InstallMachine $installMachine

    $captureUserContext = Read-RepositoryText `
        -RelativePath 'tools/installer/capture-user-context.ps1'
    Assert-TextContains `
        -Text $captureUserContext `
        -Pattern 'Write-BafxInstallerFailure[\s\S]*CaptureUserContext[\s\S]*\.diagnostic\.txt' `
        -Description 'original-user context failures create a diagnostic sidecar'

    $registerUserPackage = Read-RepositoryText `
        -RelativePath 'tools/installer/register-user-package.ps1'
    Assert-TextContains `
        -Text $registerUserPackage `
        -Pattern 'Write-BafxInstallerFailure[\s\S]*InstallerDiagnosticPath[\s\S]*RelatedFailures' `
        -Description 'user package failures retain structured and related diagnostics'
    Assert-TextContains `
        -Text $registerUserPackage `
        -Pattern 'function\s+Assert-PayloadFileSetLedger[\s\S]*-isnot\s+\[string\][\s\S]*duplicate payload file path[\s\S]*Assert-PayloadFileSetLedger\s+-State\s+\$State' `
        -Description 'user package validation checks the optional payload file-set ledger'
    Assert-TextExcludes `
        -Text $registerUserPackage `
        -Pattern 'throw\s+\$registrationError\b' `
        -Description 'lossy package registration failure string rethrow'

    $unregisterMachine = Read-RepositoryText `
        -RelativePath 'tools/installer/unregister-machine.ps1'
    Assert-TextContains `
        -Text $unregisterMachine `
        -Pattern 'Write-BafxInstallerFailure[\s\S]*UninstallMachine[\s\S]*InstallerStep' `
        -Description 'machine uninstall emits structured failure diagnostics'
    Assert-TextContains `
        -Text $unregisterMachine `
        -Pattern 'function\s+Write-FlushedUtf8NoBom[\s\S]*Assert-NoReparsePath[\s\S]*function\s+Write-UninstallJournal[\s\S]*Assert-NoReparsePath[\s\S]*function\s+Write-UninstallCompletionMarker[\s\S]*Assert-NoReparsePath' `
        -Description 'uninstall journal writers revalidate protected paths'
    Assert-TextContains `
        -Text $unregisterMachine `
        -Pattern 'ensure-host-process-stopped[\s\S]*remove-installed-user-startup-registration[\s\S]*remove-installed-user-package[\s\S]*remove-owned-certificates' `
        -Description 'machine uninstall reports stable resource cleanup steps'
    Assert-TextContains `
        -Text $unregisterMachine `
        -Pattern 'function\s+Read-UninstallCompletionMarker[\s\S]*stateRemoved[\s\S]*Read-UninstallJournal' `
        -Description 'uninstall completion markers are bound to the journal'
    Assert-TextContains `
        -Text $unregisterMachine `
        -Pattern 'Remove-ProtectedInstallStatePair\s+-Path\s+\$statePath[\s\S]*Write-UninstallCompletionMarker' `
        -Description 'uninstall publishes completion only after state deletion'
    Assert-TextContains `
        -Text $unregisterMachine `
        -Pattern 'verify-completed-uninstall[\s\S]*Read-UninstallCompletionMarker[\s\S]*exit\s+0' `
        -Description 'uninstall retries finish an already deleted state pair'
    Assert-TextContains `
        -Text $unregisterMachine `
        -Pattern 'function\s+Read-UninstallJournalSnapshotState[\s\S]*Assert-InstallStatePair[\s\S]*primaryStateBase64' `
        -Description 'uninstall validates journal snapshots before rebuilding a marker'
    Assert-TextContains `
        -Text $unregisterMachine `
        -Pattern 'recover-completion-marker[\s\S]*Read-UninstallJournalSnapshotState[\s\S]*Write-UninstallCompletionMarker' `
        -Description 'uninstall recovers a marker after state deletion'

    $protectedPaths = Read-RepositoryText `
        -RelativePath 'tools/installer/protected-paths.ps1'
    Assert-TextContains `
        -Text $protectedPaths `
        -Pattern 'function\s+Get-ProtectedProgramFilesRoots[\s\S]*ProgramW6432[\s\S]*ProgramFilesDir' `
        -Description 'shared installer validation recognizes relocated Program Files roots'
    Assert-TextContains `
        -Text $protectedPaths `
        -Pattern 'function\s+Resolve-InstallerAclIdentity[\s\S]*Translate[\s\S]*SecurityIdentifier[\s\S]*unresolvable identity with write access' `
        -Description 'ACL identity resolution fails closed only for unknown writers'
    Assert-TextContains `
        -Text $protectedPaths `
        -Pattern 'function\s+Replace-InstallerFileAtomically[\s\S]*File\]::Replace[\s\S]*replacementBackup' `
        -Description 'atomic installer replacement supplies a valid framework backup path'
    Assert-TextContains `
        -Text ($installMachine + $registerUserPackage + $unregisterMachine) `
        -Pattern 'Resolve-InstallerAclIdentity\s+`' `
        -Description 'all installer ACL checks use shared identity resolution'
    Assert-TextExcludes `
        -Text ($installMachine + $unregisterMachine) `
        -Pattern 'File\]::Replace[\s\S]*\$null' `
        -Description 'installer replacement never passes a null framework backup path'
    Assert-TextContains `
        -Text ($installMachine + $unregisterMachine) `
        -Pattern 'installer-diagnostics\.ps1[\s\S]*protected-paths\.ps1[\s\S]*Resolve-ProtectedProgramFilesPath' `
        -Description 'machine install and uninstall share protected path validation'

    $diagnostics = Read-RepositoryText `
        -RelativePath 'tools/installer/installer-diagnostics.ps1'
    Assert-TextContains `
        -Text $diagnostics `
        -Pattern 'BAFX_INSTALL_FAILURE:' `
        -Description 'installer failure summary marker'
    Assert-TextContains `
        -Text $diagnostics `
        -Pattern 'BAFX_INSTALL_DIAGNOSTIC_JSON:' `
        -Description 'installer diagnostic JSON marker'

    $controlCenterResource = Read-RepositoryText `
        -RelativePath 'src/control-center/BAFX.ControlCenter.rc.in'
    Assert-TextContains `
        -Text $controlCenterResource `
        -Pattern 'FILEVERSION\s+@BAFX_VERSION_MAJOR@,@BAFX_VERSION_MINOR@,@BAFX_VERSION_PATCH@,@BAFX_VERSION_REVISION@' `
        -Description 'Control Center fixed file version resource'
    Assert-TextContains `
        -Text $controlCenterResource `
        -Pattern 'VALUE\s+"ProductVersion",\s+"@BAFX_VERSION@\\0"' `
        -Description 'Control Center product version resource'
}

function Test-ProtectedAclIdentityResolutionContract
{
    $probeModule = $null
    $protectedPathsAst = Get-ParsedScript `
        -RelativePath 'tools/installer/protected-paths.ps1'
    $helperText = Get-FunctionText `
        -Ast $protectedPathsAst `
        -Name 'Resolve-InstallerAclIdentity'
    $helperScript = @(
        'Set-StrictMode -Version Latest'
        $helperText
    ) -join "`n"
    $probeModule = New-Module -ScriptBlock ([scriptblock]::Create($helperScript))
    try
    {
        $writeData = [int][Security.AccessControl.FileSystemRights]::WriteData
        $readData = [int][Security.AccessControl.FileSystemRights]::ReadData

        $knownRule = [pscustomobject]@{
            IdentityReference = [Security.Principal.SecurityIdentifier]::new('S-1-5-18')
            FileSystemRights = $writeData
        }
        $knownResult = & $probeModule {
            param($Rule, $Mask)
            Resolve-InstallerAclIdentity -Rule $Rule -WriteRights $Mask
        } $knownRule $writeData
        Assert-True `
            -Condition ([string]$knownResult -eq 'S-1-5-18') `
            -Message 'Resolvable ACL identities must translate to their SID.'

        $sidRule = [pscustomobject]@{
            IdentityReference = 'S-1-5-21-461670580-1850758422-1265360451-1001'
            FileSystemRights = $writeData
        }
        $sidResult = & $probeModule {
            param($Rule, $Mask)
            Resolve-InstallerAclIdentity -Rule $Rule -WriteRights $Mask
        } $sidRule $writeData
        Assert-True `
            -Condition ([string]$sidResult -eq [string]$sidRule.IdentityReference) `
            -Message 'A textual SID must remain usable when account translation is unavailable.'

        $unknownReadRule = [pscustomobject]@{
            IdentityReference = [Security.Principal.NTAccount]::new(
                'BAFX-Unknown-Application-Package\ReadOnly')
            FileSystemRights = $readData
        }
        $unknownReadResult = & $probeModule {
            param($Rule, $Mask)
            Resolve-InstallerAclIdentity -Rule $Rule -WriteRights $Mask
        } $unknownReadRule $writeData
        Assert-True `
            -Condition ($null -eq $unknownReadResult) `
            -Message 'An unresolvable read-only ACL identity must be ignored.'

        $unknownWriteRule = [pscustomobject]@{
            IdentityReference = [Security.Principal.NTAccount]::new(
                'BAFX-Unknown-Application-Package\Writer')
            FileSystemRights = $writeData
        }
        Assert-Throws `
            -Action {
                & $probeModule {
                    param($Rule, $Mask)
                    Resolve-InstallerAclIdentity `
                        -Rule $Rule `
                        -WriteRights $Mask `
                        -Path 'C:\Program Files\ba-click-fx-desktop\.staging\current'
                } $unknownWriteRule $writeData
            } `
            -Description 'an unresolvable ACL identity with write access'
    }
    finally
    {
        if ($null -ne $probeModule)
        {
            Remove-Module -ModuleInfo $probeModule -Force -ErrorAction SilentlyContinue
        }
    }
}

function Test-AtomicFileReplacementContract
{
    $protectedPathsAst = Get-ParsedScript `
        -RelativePath 'tools/installer/protected-paths.ps1'
    $pathHelperText = Get-FunctionText `
        -Ast $protectedPathsAst `
        -Name 'Assert-NoReparsePath'
    $replaceHelperText = Get-FunctionText `
        -Ast $protectedPathsAst `
        -Name 'Replace-InstallerFileAtomically'
    $probeModule = $null
    $temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    $temporaryRoot = Join-Path `
        $temporaryParent `
        ('bafx-atomic-replace-' + [Guid]::NewGuid().ToString('N'))
    try
    {
        $helperScript = @(
            'Set-StrictMode -Version Latest'
            $pathHelperText
            $replaceHelperText
        ) -join "`n"
        $probeModule = New-Module -ScriptBlock ([scriptblock]::Create($helperScript))
        New-Item -ItemType Directory -Path $temporaryRoot -Force | Out-Null
        $sourcePath = Join-Path $temporaryRoot 'candidate.tmp'
        $destinationPath = Join-Path $temporaryRoot 'state.json'
        [IO.File]::WriteAllText($sourcePath, 'candidate')
        [IO.File]::WriteAllText($destinationPath, 'previous')
        & $probeModule {
            param($Source, $Destination)
            Replace-InstallerFileAtomically `
                -SourcePath $Source `
                -DestinationPath $Destination
        } $sourcePath $destinationPath
        Assert-True `
            -Condition ([IO.File]::ReadAllText($destinationPath) -eq 'candidate') `
            -Message 'Atomic replacement did not publish the candidate file.'
        Assert-True `
            -Condition (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) `
            -Message 'Atomic replacement left the source file behind.'
        Assert-True `
            -Condition (@(Get-ChildItem -LiteralPath $temporaryRoot -Filter '*.replace.bak').Count -eq 0) `
            -Message 'Atomic replacement leaked its transient backup.'
    }
    finally
    {
        if ($null -ne $probeModule)
        {
            Remove-Module -ModuleInfo $probeModule -Force -ErrorAction SilentlyContinue
        }
        $resolvedTemporaryRoot = [IO.Path]::GetFullPath($temporaryRoot)
        if ($resolvedTemporaryRoot.StartsWith(
                $temporaryParent,
                [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($resolvedTemporaryRoot).StartsWith(
                'bafx-atomic-replace-',
                [StringComparison]::Ordinal))
        {
            Remove-Item -LiteralPath $resolvedTemporaryRoot -Recurse -Force `
                -ErrorAction SilentlyContinue
        }
    }
}

function Test-CompressionRuntimeColdStart
{
    $installMachine = Read-RepositoryText `
        -RelativePath 'tools/installer/install-machine.ps1'
    $loadStatements = @(
        Get-CompressionRuntimeLoadStatements -InstallMachine $installMachine
    )
    $childScriptLines = @(
        'Set-StrictMode -Version Latest',
        '$ErrorActionPreference = ''Stop''',
        'if ($PSVersionTable.PSEdition -ne ''Desktop'' -or $PSVersionTable.PSVersion.Major -ne 5)',
        '{',
        '    throw "Expected Windows PowerShell 5.1, found $($PSVersionTable.PSVersion)."',
        '}',
        '$zipTypeBefore = ''IO.Compression.ZipFile'' -as [type]',
        'if ($null -ne $zipTypeBefore)',
        '{',
        '    throw ''ZipFile resolved before the production startup statements.''',
        '}'
    )
    $childScriptLines += $loadStatements
    $childScriptLines += @(
        '$zipTypeAfter = ''IO.Compression.ZipFile'' -as [type]',
        'if ($null -eq $zipTypeAfter)',
        '{',
        '    throw ''ZipFile did not resolve after the production startup statements.''',
        '}'
    )
    $childScript = $childScriptLines -join [Environment]::NewLine
    $encodedCommand = [Convert]::ToBase64String(
        [Text.Encoding]::Unicode.GetBytes($childScript))
    $windowsPowerShell =
        Get-Command powershell.exe -CommandType Application -ErrorAction Stop |
            Select-Object -First 1
    $process = $null
    $timeoutMilliseconds = 5000
    try
    {
        $process = Start-Process `
            -FilePath $windowsPowerShell.Source `
            -ArgumentList @(
                '-NoLogo',
                '-NoProfile',
                '-NonInteractive',
                '-ExecutionPolicy',
                'Bypass',
                '-EncodedCommand',
                $encodedCommand) `
            -WindowStyle Hidden `
            -PassThru

        # A broken child probe must not stall packaging or release jobs.
        if (-not $process.WaitForExit($timeoutMilliseconds))
        {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $null = $process.WaitForExit(1000)
            throw "Compression cold-start probe exceeded the $timeoutMilliseconds ms timeout."
        }
        if ($process.ExitCode -ne 0)
        {
            throw "Compression cold-start probe failed with exit code $($process.ExitCode)."
        }
    }
    finally
    {
        if ($null -ne $process)
        {
            if (-not $process.HasExited)
            {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
                $null = $process.WaitForExit(1000)
            }
            $process.Dispose()
        }
    }
}

function Test-InnoPayloadContract
{
    $inno = Read-RepositoryText -RelativePath 'tools/installer/ba-click-fx-desktop.iss'
    $chineseMessages = Read-RepositoryText `
        -RelativePath 'tools/installer/ChineseSimplified.isl'
    $packager = Read-RepositoryText -RelativePath 'tools/package-user-installer.ps1'
    $machineInstaller = Read-RepositoryText -RelativePath 'tools/installer/install-machine.ps1'
    $identityBuilder = Read-RepositoryText -RelativePath 'tools/identity-package/build-identity-package.ps1'
    Assert-TextContains `
        -Text $inno `
        -Pattern '(?m)^ShowLanguageDialog=no$' `
        -Description 'automatic installer language selection without a prompt'
    Assert-TextContains `
        -Text $inno `
        -Pattern '(?m)^LanguageDetectionMethod=uilanguage$' `
        -Description 'Windows UI language based installer selection'
    Assert-TextContains `
        -Text $inno `
        -Pattern '(?m)^UsePreviousLanguage=no$' `
        -Description 'installer language is re-detected instead of inherited from an older release'
    Assert-TextContains `
        -Text $inno `
        -Pattern '(?m)^DisableDirPage=yes$' `
        -Description 'protected machine installer does not offer an unsafe custom directory'
    Assert-TextContains `
        -Text $inno `
        -Pattern '(?m)^UsePreviousAppDir=no$' `
        -Description 'installer does not inherit an unsupported release install directory'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'function\s+PrepareToInstall[\s\S]*\{autopf\}\\ba-click-fx-desktop[\s\S]*CompareText[\s\S]*ProtectedInstallDirectoryRequired' `
        -Description 'install directory is rejected before privileged payload files are copied'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'Name:\s*"english"[\s\S]*Name:\s*"chinesesimplified"[\s\S]*ChineseSimplified\.isl' `
        -Description 'English and Simplified Chinese installer languages'
    Assert-TextContains `
        -Text $chineseMessages `
        -Pattern '(?m)^LanguageID=\$0804$' `
        -Description 'Simplified Chinese Windows language identifier'
    Assert-TextContains `
        -Text $inno `
        -Pattern '\{cm:LaunchProgram,BAFX Control Center\}' `
        -Description 'localized post-install Control Center action'
    Assert-TextContains `
        -Text $inno `
        -Pattern '(?m)^Filename:\s*"\{app\}\\BAFX\.ControlCenter\.exe";[^\r\n]*Check:\s*MachineInstallationCompleted$' `
        -Description 'Control Center launches only after machine installation succeeds'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'english\.PowerShellFailedWithExitCode=[^\r\n]+[\s\S]*chinesesimplified\.PowerShellFailedWithExitCode=[^\r\n]+' `
        -Description 'localized installer failure summary'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'english\.RollbackRecovery=[^\r\n]+[\s\S]*chinesesimplified\.RollbackRecovery=[^\r\n]+' `
        -Description 'localized installer recovery guidance'
    Assert-TextContains `
        -Text $inno `
        -Pattern "CustomMessage\('PrepareMachineInstallation'\)[\s\S]*CustomMessage\('RegisterPackage'\)[\s\S]*CustomMessage\('FinalizeMachineInstallation'\)" `
        -Description 'machine install phases use localized descriptions'
    Assert-TextExcludes `
        -Text $inno `
        -Pattern "FormatPowerShellFailure\(\s*'[^']+'" `
        -Description 'hard-coded user-facing PowerShell phase descriptions'
    $filesSectionMatch = [regex]::Match(
        $inno,
        '(?ms)^\[Files\]\s*(?<body>.*?)(?=^\[|\z)')
    Assert-True -Condition $filesSectionMatch.Success -Message 'Inno [Files] section is missing.'
    $sources = @(
        [regex]::Matches($filesSectionMatch.Groups['body'].Value, '(?m)^\s*Source:\s*"([^"]+)"') |
            ForEach-Object { $_.Groups[1].Value }
    )
    $expectedSources = @(
        '{#StageRoot}\ba-click-fx-desktop.exe',
        '{#StageRoot}\BAFX.ControlCenter.exe',
        '{#StageRoot}\LICENSE.txt',
        '{#StageRoot}\SUPPORT.md',
        '{#StageRoot}\THIRD-PARTY-NOTICES.txt',
        '{#StageRoot}\Identity\*',
        '{#StageRoot}\Installer\*'
    )
    Assert-ArrayEquals `
        -Expected $expectedSources `
        -Actual $sources `
        -Description 'Inno [Files] payload source whitelist'
    $destinations = @(
        [regex]::Matches(
            $filesSectionMatch.Groups['body'].Value,
            '(?m)^\s*[^\r\n]*?DestDir:\s*"([^"]+)"') |
            ForEach-Object { $_.Groups[1].Value }
    )
    Assert-True `
        -Condition ($destinations.Count -eq $expectedSources.Count) `
        -Message 'Every Inno payload entry must have one protected staging destination.'
    foreach ($destination in $destinations)
    {
        Assert-True `
            -Condition $destination.StartsWith(
                '{app}\.staging\current',
                [StringComparison]::OrdinalIgnoreCase) `
            -Message "Inno payload destination escaped protected staging: $destination"
    }
    Assert-TextContains `
        -Text $inno `
        -Pattern '(?m)^AppMutex=Global\\BAFX\.UserInstaller\.v1$' `
        -Description 'installer-wide mutex prevents concurrent transactions'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'function\s+IsReparsePointPath[\s\S]*function\s+AssertNoReparsePointTree[\s\S]*function\s+SafeDeleteTree[\s\S]*AssertNoReparsePointTree[\s\S]*DelTree' `
        -Description 'protected tree deletion rejects reparse points before DelTree'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'GetFileAttributesW@kernel32\.dll[\s\S]*function\s+IsReparsePointPath[\s\S]*GetFileAttributesW\(Path\)[\s\S]*INVALID_FILE_ATTRIBUTES' `
        -Description 'reparse validation handles filesystem-root directories'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'function\s+SafeDeleteFile[\s\S]*AssertNoReparsePointPath[\s\S]*DeleteFile\(' `
        -Description 'protected file deletion rejects reparse points before DeleteFile'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'PrepareToInstall[\s\S]*SafeDeleteTree\(PayloadRoot\)' `
        -Description 'stale staging cleanup uses the protected deletion wrapper'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'CleanupUncommittedInstallArtifacts[\s\S]*SafeDeleteTree\(StagingRoot\)' `
        -Description 'early setup failure cleanup uses the protected deletion wrapper'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'CurUninstallStepChanged[\s\S]*SafeDeleteTree\(InstallerRoot\)' `
        -Description 'uninstall recovery evidence cleanup uses the protected deletion wrapper'
    Assert-TextExcludes `
        -Text $inno `
        -Pattern '(?ms)^\[UninstallDelete\]' `
        -Description 'uninstall does not erase recovery evidence declaratively'
    $rawDelTreeCalls = @([regex]::Matches($inno, '(?m)^\s*[^\r\n]*\bDelTree\('))
    Assert-True `
        -Condition ($rawDelTreeCalls.Count -eq 1) `
        -Message 'All Inno tree deletion must be centralized in SafeDeleteTree.'
    $rawDeleteFileCalls = @([regex]::Matches($inno, '(?m)^\s*[^\r\n]*\bDeleteFile\('))
    Assert-True `
        -Condition ($rawDeleteFileCalls.Count -eq 7) `
        -Message 'Unexpected raw Inno file deletion path; use SafeDeleteFile for protected files.'
    Assert-TextContains `
        -Text $inno `
        -Pattern '#ifdef\s+IncludeSpout2Notice[\s\S]*THIRD-PARTY-NOTICES\.txt[\s\S]*#endif' `
        -Description 'Spout2 notice is conditional on the Full installer define'
    Assert-TextContains `
        -Text $packager `
        -Pattern "if\s*\(\s*-not\s+\`$Slim\s*\)[\s\S]*THIRD-PARTY-NOTICES\.txt[\s\S]*/DIncludeSpout2Notice=1" `
        -Description 'Full installer stages and enables the Spout2 notice'

    Assert-TextExcludes `
        -Text $inno `
        -Pattern '(?i)(signtool|makeappx|Windows Kits|\\bin\\|\.(pfx|pvk|snk|key|pem)\b)' `
        -Description 'SDK tools or private key material'
    Assert-TextContains `
        -Text $inno `
        -Pattern '(?m)^PrivilegesRequired=admin$' `
        -Description 'machine installation requires administrator privileges'
    Assert-TextContains `
        -Text $inno `
        -Pattern '(?m)^Filename:\s*"\{app\}\\BAFX\.ControlCenter\.exe".*runasoriginaluser' `
        -Description 'post-install Control Center launch uses the original user'
    Assert-TextContains `
        -Text $inno `
        -Pattern '(?m)^Name:\s*"\{autodesktop\}\\BAFX Control Center";\s*Filename:\s*"\{app\}\\BAFX\.ControlCenter\.exe";\s*WorkingDir:\s*"\{app\}"\s*$' `
        -Description 'Control Center desktop shortcut follows the installation scope'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'ExecAsOriginalUser' `
        -Description 'registration is executed for the installing user'
    Assert-TextContains `
        -Text $inno `
        -Pattern "register-user-package\.ps1'[\s\S]*-PayloadDirectory '\s*\+\s*QuoteArgument\(PayloadRoot\)" `
        -Description 'normal user-package registration is bound to the protected staging payload'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'ExecAndLogOutput[\s\S]*@HandlePowerShellOutput' `
        -Description 'elevated PowerShell output is copied into the installer log'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'BAFX_INSTALL_FAILURE:[\s\S]*BAFX_INSTALL_DIAGNOSTIC_JSON:' `
        -Description 'installer recognizes structured PowerShell diagnostics'
    Assert-TextContains `
        -Text $inno `
        -Pattern "LastPowerShellRawOutput[\s\S]*CustomMessage\('PowerShellOutput'\)" `
        -Description 'unstructured early PowerShell errors remain visible'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'LoadPowerShellDiagnostic[\s\S]*\.diagnostic\.txt' `
        -Description 'original-user diagnostic sidecars are copied into the installer log'
    Assert-TextContains `
        -Text $inno `
        -Pattern '(?m)^SetupLogging=yes\s*$[\s\S]*^UninstallLogging=yes\s*$' `
        -Description 'setup and uninstall log files are enabled'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'ExpandConstant\(\x27\{log\}\x27\)' `
        -Description 'failure messages expose the detailed installer log path'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'RollbackResultPath[\s\S]*\x27 -ResultPath \x27\s*\+\s*QuoteArgument\(RollbackResultPath\)' `
        -Description 'rollback diagnostics cannot overwrite registration diagnostics'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'PrimaryFailure\s*:=\s*FormatPowerShellFailure[\s\S]{0,500}RunBestEffortRollback' `
        -Description 'the first failure is saved before rollback starts'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'PowerShell \[\x27 \+ ContextName \+ \x27\] exited with code' `
        -Description 'PowerShell execution context and exit code logging'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'Package registration result follows:' `
        -Description 'package registration result logging'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'SuppressibleMsgBox' `
        -Description 'recovery prompts support unattended installation'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'RecoveryRequired\s*:=\s*True[\s\S]*function\s+GetCustomSetupExitCode[\s\S]*Result\s*:=\s*1001' `
        -Description 'retained recovery state returns a nonzero setup exit code'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'procedure\s+RaiseInstallerFailure[\s\S]*SetupFailureExitCode\s*:=\s*ExitCode[\s\S]*RaiseException' `
        -Description 'RaiseException failures retain an explicit nonzero setup exit code'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'if\s+ExitCode\s*=\s*1001[\s\S]*ShowRetainedRecovery[\s\S]*RunBestEffortRollback[\s\S]*RaiseInstallerFailure\([^\)]*1002' `
        -Description 'installer distinguishes retained recovery from completed rollback'
    Assert-TextExcludes `
        -Text $inno `
        -Pattern '(?m)^\s*MsgBox\s*\(' `
        -Description 'unsuppressible custom installer prompts'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'GenerateUniqueName\(TempRoot,\s*\x27\.json\x27\)' `
        -Description 'original-user state uses unique user TEMP files'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'DeleteFile\([^\r\n]+\.diagnostic\.txt[\s\S]*procedure\s+DeinitializeSetup[\s\S]*DeleteTransientState[\s\S]*procedure\s+DeinitializeUninstall[\s\S]*DeleteTransientState' `
        -Description 'transient original-user state is removed after setup and uninstall'
    Assert-TextExcludes `
        -Text $inno `
        -Pattern 'procedure\s+CurUninstallStepChanged[\s\S]*?RegistrationResultPath\s*:\s*String;' `
        -Description 'uninstall result path does not shadow transient cleanup state'
    Assert-TextExcludes `
        -Text $inno `
        -Pattern '\{tmp\}\\bafx-[A-Za-z-]*(user-context|registration-result)\.json' `
        -Description 'original-user state avoids the protected Inno temp directory'

    $uninstallCode = [regex]::Match(
        $inno,
        '(?ms)procedure\s+CurUninstallStepChanged\b[\s\S]*\z')
    Assert-True `
        -Condition $uninstallCode.Success `
        -Message 'Inno uninstall code is missing.'
    Assert-TextContains `
        -Text $uninstallCode.Value `
        -Pattern 'RollbackAction RemoveNew[\s\S]*-Phase Rollback[\s\S]*RollbackAction RestorePrevious[\s\S]*-Phase RollbackCleanup' `
        -Description 'pending uninstall uses the fixed original-user and machine rollback order'
    Assert-TextContains `
        -Text $uninstallCode.Value `
        -Pattern 'ResolveRollbackScript[\s\S]*-Phase Rollback[\s\S]*ResolveRestoredRollbackScript[\s\S]*RollbackAction RestorePrevious' `
        -Description 'uninstall re-resolves the restored recovery script after machine rollback'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'function\s+ResolveRollbackScript[\s\S]*StagedPath\s*:=.*ScriptName[\s\S]*if\s+FileExists\(StagedPath\)[\s\S]*LivePath\s*:=.*Installer' `
        -Description 'pending recovery prefers the transaction staged scripts across releases'
    Assert-TextExcludes `
        -Text $uninstallCode.Value `
        -Pattern 'DeleteFile\(\s*MachineStatePath\s*\)' `
        -Description 'uninstall does not delete the pending journal outside rollback cleanup'
    Assert-TextContains `
        -Text $uninstallCode.Value `
        -Pattern 'install-machine\.ps1[\s\S]*\-Phase Rollback[\s\S]*no committed state remains' `
        -Description 'pending uninstall uses elevated rollback and handles first-install state'
    Assert-TextContains `
        -Text $uninstallCode.Value `
        -Pattern 'not FileExists\(InstallStatePath\)[\s\S]*not FileExists\(MachineStatePath\)[\s\S]*not DirExists\(InstallerRoot\)[\s\S]*CleanupFirstInstallPayload' `
        -Description 'uninstall cleans an early first-install failure without recovery scripts'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'CommittedStatePresent[\s\S]*ResolveRestoredRollbackScript[\s\S]*ResolveRollbackScript[\s\S]*RollbackCleanup' `
        -Description 'first-install rollback cleanup falls back to the staged recovery script'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'CommittedStatePresent[\s\S]*if\s+FileExists\(ExistingPendingPath\)\s+and\s+CommittedStatePresent[\s\S]*RollbackAction RestorePrevious' `
        -Description 'first-install pending recovery skips restoring a nonexistent previous package'
    Assert-TextContains `
        -Text $inno `
        -Pattern '\-Phase Prepare' `
        -Description 'machine preparation phase'
    Assert-TextContains `
        -Text $inno `
        -Pattern '\-Phase Finalize' `
        -Description 'machine finalization phase'
    Assert-TextContains `
        -Text $machineInstaller `
        -Pattern 'filesCommitted\s*=\s*\$false[\s\S]*rollbackDirectory\s*=\s*''''[\s\S]*rollbackManifest\s*=\s*''''[\s\S]*stagedPackagePath\s*=\s*''''[\s\S]*committedInstallState\s*=\s*\$null' `
        -Description 'pending journal predeclares mutable commit fields for PowerShell 5.1'

    # The release machine creates only the unsigned package template. The
    # target machine owns the short-lived signing key.
    Assert-TextContains `
        -Text $identityBuilder `
        -Pattern '\[switch\]\$UnsignedTemplate' `
        -Description 'unsigned identity-template build mode'
    Assert-TextContains `
        -Text $packager `
        -Pattern '\-UnsignedTemplate' `
        -Description 'release packaging requests an unsigned identity template'
    Assert-TextContains `
        -Text $packager `
        -Pattern "Filter\s+'\*\.unsigned\.msix'" `
        -Description 'unsigned sparse-package template selection'
    Assert-TextContains `
        -Text $packager `
        -Pattern "Filter\s+'\*\.identity-template\.json'" `
        -Description 'identity-template metadata selection'
    Assert-TextContains `
        -Text $packager `
        -Pattern 'BAFX\.IdentitySigner\.exe' `
        -Description 'native target-machine signer payload'
    Assert-TextContains `
        -Text $packager `
        -Pattern 'Get-AuthenticodeSignature[\s\S]*SignatureStatus\]::NotSigned' `
        -Description 'unsigned template signature guard'
    Assert-TextContains `
        -Text $packager `
        -Pattern "Extension\s+-in\s+@\('\.cer',\s*'\.pfx',\s*'\.pvk',\s*'\.snk',\s*'\.key',\s*'\.pem'\)" `
        -Description 'certificate and private-key staging guard'
    Assert-TextExcludes `
        -Text $packager `
        -Pattern '(?i)(New-SelfSignedCertificate|Import-Certificate|Export-Certificate)' `
        -Description 'release-machine certificate handling'

    $packagingCmake = Read-RepositoryText -RelativePath 'cmake/Packaging.cmake'
    Assert-TextContains `
        -Text $packagingCmake `
        -Pattern 'add_custom_target\(\s*package_user_installer' `
        -Description 'ordinary-user installer CMake target'
    Assert-TextContains `
        -Text $packagingCmake `
        -Pattern 'DEPENDS[\s\S]*ba_click_fx_desktop[\s\S]*bafx_control_center[\s\S]*bafx_identity_signer' `
        -Description 'ordinary-user installer Release payload dependencies'
}

function Test-CrossVersionPendingRecoveryContract
{
    $installMachinePath = 'tools/installer/install-machine.ps1'
    $installMachine = Read-RepositoryText -RelativePath $installMachinePath
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'function\s+Resolve-PayloadDirectory[\s\S]*\[switch\]\$AllowMissing' `
        -Description 'rollback can resolve a missing legacy staging directory'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern '\-AllowMissing:\(\$Phase\s+\-in\s+@\(\x27Rollback\x27,\s*\x27RollbackCleanup\x27\)\)' `
        -Description 'only rollback phases may use a missing staging directory'

    $rollbackMatch = [regex]::Match(
        $installMachine,
        '(?ms)if\s*\(\$Phase\s*-eq\s*\x27Rollback\x27\)(?<body>[\s\S]*?)(?=^if\s*\(\$Phase\s*-eq\s*\x27RollbackCleanup\x27\))')
    Assert-True `
        -Condition $rollbackMatch.Success `
        -Message 'Install-machine rollback phase is missing.'
    Assert-TextExcludes `
        -Text $rollbackMatch.Groups['body'].Value `
        -Pattern 'Read-PayloadManifest|Assert-PayloadManifest' `
        -Description 'legacy rollback does not require the current payload manifest'
    Assert-TextContains `
        -Text $rollbackMatch.Groups['body'].Value `
        -Pattern 'Assert-PendingStateObject[\s\S]*-PayloadDirectory\s+\$script:PayloadRoot' `
        -Description 'rollback accepts a pending package path in protected staging'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'stalePending\s*=\s*Assert-PendingStateObject[\s\S]*-PayloadDirectory\s+\$script:PayloadRoot' `
        -Description 'stale pending validation accepts its staged package path'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'rollback-failed-prepare[\s\S]*pendingState\s*=\s*Assert-PendingStateObject[\s\S]*-PayloadDirectory\s+\$script:PayloadRoot' `
        -Description 'prepare failure cleanup accepts its staged package path'

    $ast = Get-ParsedScript -RelativePath $installMachinePath
    $functionText = Get-FunctionText -Ast $ast -Name 'Resolve-PayloadDirectory'
    $probeModule = New-Module -ScriptBlock ([scriptblock]::Create(
        "Set-StrictMode -Version Latest`n$functionText"))
    $temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    $temporaryRoot = Join-Path `
        $temporaryParent `
        ('bafx-cross-version-recovery-' + [Guid]::NewGuid().ToString('N'))
    try
    {
        $installRoot = Join-Path $temporaryRoot 'install'
        $payloadRoot = Join-Path $installRoot '.staging\current'
        New-Item -ItemType Directory -Path $installRoot -Force | Out-Null
        $resolved = & $probeModule {
            param($Root, $Payload)
            Resolve-PayloadDirectory `
                -InstallRoot $Root `
                -PayloadPath $Payload `
                -AllowMissing
        } $installRoot $payloadRoot
        Assert-True `
            -Condition ([IO.Path]::GetFullPath($resolved) -eq [IO.Path]::GetFullPath($payloadRoot)) `
            -Message 'Rollback did not accept a missing legacy staging directory.'

        New-Item -ItemType Directory -Path $payloadRoot -Force | Out-Null
        $resolvedExisting = & $probeModule {
            param($Root, $Payload)
            Resolve-PayloadDirectory `
                -InstallRoot $Root `
                -PayloadPath $Payload `
                -AllowMissing
        } $installRoot $payloadRoot
        Assert-True `
            -Condition ([IO.Path]::GetFullPath($resolvedExisting) -eq [IO.Path]::GetFullPath($payloadRoot)) `
            -Message 'Rollback could not resolve an existing staging directory.'
    }
    finally
    {
        if ($null -ne $probeModule)
        {
            Remove-Module -ModuleInfo $probeModule -Force -ErrorAction SilentlyContinue
        }
        $resolvedTemporaryRoot = [IO.Path]::GetFullPath($temporaryRoot)
        if ($resolvedTemporaryRoot.StartsWith(
                $temporaryParent,
                [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($resolvedTemporaryRoot).StartsWith(
                'bafx-cross-version-recovery-',
                [StringComparison]::Ordinal))
        {
            Remove-Item -LiteralPath $resolvedTemporaryRoot -Recurse -Force `
                -ErrorAction SilentlyContinue
        }
    }
}

function Test-PendingRecoveryFailureContract
{
    $installMachinePath = 'tools/installer/install-machine.ps1'
    $installMachine = Read-RepositoryText -RelativePath $installMachinePath
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'commitState\s*-eq\s*''committed''[\s\S]*Complete-CommittedPendingTransaction' `
        -Description 'committed pending snapshots repair the install-state pair'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'statePairError[\s\S]*filesCommitted[\s\S]*ROLLBACK-MANIFEST\.json' `
        -Description 'torn state pairs require rollback evidence before recovery'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'RollbackCleanup[\s\S]*Recover-CreatingCertificate[\s\S]*certificatePhase' `
        -Description 'restart recovery handles a certificate-creation journal'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern '\$packagePathIsStaged\s*=\s*\$packagePath\s*-eq\s*\$payloadPackagePath' `
        -Description 'staged integrity validation uses an exact package path match'

    $ast = Get-ParsedScript -RelativePath $installMachinePath
    $pendingText = Get-FunctionText -Ast $ast -Name 'Assert-PendingStateObject'
    $splitText = Get-FunctionText -Ast $ast -Name 'Split-Ledger'
    $normalizeText = Get-FunctionText -Ast $ast -Name 'Normalize-PayloadFileSetLedger'
    $readOldStateText = Get-FunctionText -Ast $ast -Name 'Read-OldInstallState'
    $readModuleText = @(
        'Set-StrictMode -Version Latest'
        "`$ErrorActionPreference = 'Stop'"
        (Get-FunctionText -Ast $ast -Name 'Get-StatePropertiesWithoutDigest')
        (Get-FunctionText -Ast $ast -Name 'Convert-StateToCanonicalJson')
        (Get-FunctionText -Ast $ast -Name 'Get-StateDigest')
        $splitText
        (Get-FunctionText -Ast $ast -Name 'Normalize-PayloadRelativePath')
        $normalizeText
        $pendingText
        @'
function Assert-IdentityIntegrityMaterial
{
    param(
        [object]$State,
        [string]$InstallRoot,
        [string]$PackagePath
    )
    $script:ObservedIntegrityRoot = [IO.Path]::GetFullPath($InstallRoot)
    $script:ObservedIntegrityPackagePath = [IO.Path]::GetFullPath($PackagePath)
}
'@
    ) -join "`n"
    $readModuleText += @'
function Assert-ProtectedStateAcl
{
    param([string]$Path)
}

function Assert-InstallStatePair
{
    param(
        [object]$Primary,
        [object]$Backup,
        [string]$PrimaryPath,
        [string]$BackupPath
    )
}

function Assert-InstallStateObject
{
    param(
        [object]$State,
        [string]$InstallRoot,
        [string]$ExpectedUserSid,
        [string]$ExpectedReplacementHostSha256,
        [string]$ReplacementHostPath,
        [switch]$SkipPayloadIntegrity
    )
    return $State
}
'@
    $readModuleText += $readOldStateText
    $probeModule = New-Module -ScriptBlock ([scriptblock]::Create($readModuleText))

    $temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    $temporaryRoot = Join-Path `
        $temporaryParent `
        ('bafx-pending-recovery-' + [Guid]::NewGuid().ToString('N'))
    try
    {
        $installRoot = Join-Path $temporaryRoot 'install'
        $installerRoot = Join-Path $installRoot 'Installer'
        New-Item -ItemType Directory -Path $installerRoot -Force | Out-Null

        $basePending = [ordered]@{
            schema = 1
            stateKind = 'prepare'
            userSid = 'S-1-5-21-1-2-3-1001'
            packageName = 'CialloKing.BaClickFxDesktop'
            applicationId = 'BaClickFxDesktop'
            publisher = 'CN=BaClickFx.Local'
            productVersion = '99.98.97'
            packageVersion = '99.98.97.0'
            transactionId = 'a' * 32
            preexistingPackageFullNames = @()
            oldInstallState = $null
            certificatePhase = 'creating'
            certificateSanUri = 'urn:bafx:installer:' + ('a' * 32)
        }
        $schemaOne = & $probeModule {
            param($State, $Root)
            Assert-PendingStateObject -State $State -InstallRoot $Root
        } ([pscustomobject]$basePending) $installRoot
        Assert-True `
            -Condition ([string]$schemaOne.productVersion -eq '99.98.97') `
            -Message 'Schema 1 pending recovery rejected a cross-version product.'

        $schemaTwo = [ordered]@{}
        foreach ($entry in $basePending.GetEnumerator())
        {
            $schemaTwo[$entry.Key] = $entry.Value
        }
        $schemaTwo.schema = 2
        $schemaTwo.templateSha256 = 'B' * 64
        $schemaTwoResult = & $probeModule {
            param($State, $Root)
            Assert-PendingStateObject -State $State -InstallRoot $Root
        } ([pscustomobject]$schemaTwo) $installRoot
        Assert-True `
            -Condition ([int]$schemaTwoResult.schema -eq 2) `
            -Message 'Schema 2 pending recovery did not validate.'

        $stagingRoot = Join-Path $installRoot '.staging\current'
        $stagedPackageFile = 'CialloKing.BaClickFxDesktop-test.msix'
        $stagedPackagePath = Join-Path (
            Join-Path $stagingRoot 'Identity') $stagedPackageFile
        New-Item -ItemType Directory -Path (Split-Path $stagedPackagePath -Parent) -Force |
            Out-Null
        $stagedPending = [ordered]@{}
        foreach ($entry in $schemaTwo.GetEnumerator())
        {
            $stagedPending[$entry.Key] = $entry.Value
        }
        $stagedPending.certificatePhase = 'ready'
        $stagedPending.packagePath = $stagedPackagePath
        $stagedPending.packageFile = $stagedPackageFile
        $stagedPending.certificateThumbprint = 'A' * 40
        $stagedPending.certificateWasPresent = $false
        $stagedPending.ownedCertificateThumbprints = ''
        $stagedPending.ownedPackageFiles = $stagedPackageFile
        $stagedPending.hostFile = 'ba-click-fx-desktop.exe'
        $stagedPending.hostSha256 = 'B' * 64
        $stagedPending.packageSha256 = 'C' * 64
        $stagedPending.certificateSha256 = 'D' * 64
        $stagedObservedRoot = & $probeModule {
            param($State, $Root, $Payload)
            Assert-PendingStateObject `
                -State $State `
                -InstallRoot $Root `
                -PayloadDirectory $Payload `
                -RequireIntegrity
            return $script:ObservedIntegrityRoot
        } ([pscustomobject]$stagedPending) $installRoot $stagingRoot
        Assert-True `
            -Condition ([IO.Path]::GetFullPath($stagedObservedRoot) -eq
                [IO.Path]::GetFullPath($stagingRoot)) `
            -Message 'Staged pending integrity validation used the live install root.'

        $stagedPending.packagePath = Join-Path (
            Join-Path $installRoot 'Identity') $stagedPackageFile
        $liveObservedRoot = & $probeModule {
            param($State, $Root, $Payload)
            Assert-PendingStateObject `
                -State $State `
                -InstallRoot $Root `
                -PayloadDirectory $Payload `
                -RequireIntegrity
            return $script:ObservedIntegrityRoot
        } ([pscustomobject]$stagedPending) $installRoot $stagingRoot
        Assert-True `
            -Condition ([IO.Path]::GetFullPath($liveObservedRoot) -eq
                [IO.Path]::GetFullPath($installRoot)) `
            -Message 'Live pending integrity validation used the staging root.'

        $unknownSchema = [ordered]@{}
        foreach ($entry in $schemaTwo.GetEnumerator())
        {
            $unknownSchema[$entry.Key] = $entry.Value
        }
        $unknownSchema.schema = 99
        Assert-Throws `
            -Action {
                & $probeModule {
                    param($State, $Root)
                    Assert-PendingStateObject -State $State -InstallRoot $Root
                } ([pscustomobject]$unknownSchema) $installRoot
            } `
            -Description 'unknown pending-state schema'

        $statePath = Join-Path $installerRoot 'INSTALL-STATE.json'
        $backupPath = "$statePath.bak"
        $stateJson = ([ordered]@{
                schema = 2
                transactionId = 'c' * 32
                marker = 'old'
            } | ConvertTo-Json)
        [IO.File]::WriteAllText($statePath, $stateJson)
        Assert-Throws `
            -Action {
                & $probeModule {
                    param($Root, $UserSid)
                    Read-OldInstallState -InstallRoot $Root -UserSid $UserSid
                } $installRoot $basePending.userSid
            } `
            -Description 'pending recovery with a missing state backup'
        [IO.File]::WriteAllText($backupPath, '{invalid-json')
        Assert-Throws `
            -Action {
                & $probeModule {
                    param($Root, $UserSid)
                    Read-OldInstallState -InstallRoot $Root -UserSid $UserSid
                } $installRoot $basePending.userSid
            } `
            -Description 'pending recovery with a damaged state backup'

        Remove-Item -LiteralPath $statePath -Force
        Remove-Item -LiteralPath $backupPath -Force
        $payloadRoot = Join-Path $installRoot '.staging\current'
        $resolveText = Get-FunctionText -Ast $ast -Name 'Resolve-PayloadDirectory'
        $resolveModule = New-Module -ScriptBlock ([scriptblock]::Create(
            "Set-StrictMode -Version Latest`n$resolveText"))
        try
        {
            Remove-Item -LiteralPath $stagingRoot -Recurse -Force
            $missingPayload = & $resolveModule {
                param($Root, $Payload)
                Resolve-PayloadDirectory `
                    -InstallRoot $Root `
                    -PayloadPath $Payload `
                    -AllowMissing
            } $installRoot $payloadRoot
            Assert-True `
                -Condition ([IO.Path]::GetFullPath($missingPayload) -eq
                    [IO.Path]::GetFullPath($payloadRoot)) `
                -Message 'Restart recovery did not accept a missing staging directory.'
            Assert-Throws `
                -Action {
                    & $resolveModule {
                        param($Root, $Payload)
                        Resolve-PayloadDirectory `
                            -InstallRoot $Root `
                            -PayloadPath $Payload
                    } $installRoot $payloadRoot
                } `
                -Description 'prepare cannot continue with missing staging payload'
        }
        finally
        {
            Remove-Module -ModuleInfo $resolveModule -Force -ErrorAction SilentlyContinue
        }
    }
    finally
    {
        if ($null -ne $probeModule)
        {
            Remove-Module -ModuleInfo $probeModule -Force -ErrorAction SilentlyContinue
        }
        $resolvedTemporaryRoot = [IO.Path]::GetFullPath($temporaryRoot)
        if ($resolvedTemporaryRoot.StartsWith(
                $temporaryParent,
                [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($resolvedTemporaryRoot).StartsWith(
                'bafx-pending-recovery-',
                [StringComparison]::Ordinal))
        {
            Remove-Item -LiteralPath $resolvedTemporaryRoot -Recurse -Force `
                -ErrorAction SilentlyContinue
        }
    }
}

function Test-SparsePackageContract
{
    $manifest = Read-RepositoryText -RelativePath 'tools/identity-package/Package.appxmanifest.in'
    Assert-TextContains `
        -Text $manifest `
        -Pattern '<uap10:AllowExternalContent>\s*true\s*</uap10:AllowExternalContent>' `
        -Description 'sparse package external content'
    Assert-TextContains `
        -Text $manifest `
        -Pattern 'rescap:Capability\s+Name="runFullTrust"' `
        -Description 'sparse package full trust capability'
    Assert-TextContains `
        -Text $manifest `
        -Pattern 'uap11:Capability\s+Name="graphicsCaptureWithoutBorder"' `
        -Description 'WGC borderless capability'
    Assert-TextContains `
        -Text $manifest `
        -Pattern 'Executable="ba-click-fx-desktop\.exe"' `
        -Description 'registered Host entry point executable'
    Assert-TextContains `
        -Text $manifest `
        -Pattern 'EntryPoint="Windows\.FullTrustApplication"' `
        -Description 'registered full trust application entry point'

    $registration = Read-RepositoryText -RelativePath 'tools/installer/register-user-package.ps1'
    Assert-TextContains `
        -Text (Get-FunctionText `
            -Ast (Get-ParsedScript `
                -RelativePath 'tools/installer/register-user-package.ps1') `
            -Name 'Assert-PendingState') `
        -Pattern 'certificateSanUri\s+-cne[\s\S]*State\.transactionId' `
        -Description 'user-package recovery binds the certificate SAN marker to its transaction id'
    Assert-TextContains `
        -Text $registration `
        -Pattern 'FileInfo\(\$Path\)[\s\S]*GetAccessControl\(\)' `
        -Description 'original-user ACL validation avoids the PowerShell Security module'
    Assert-TextExcludes `
        -Text $registration `
        -Pattern '(?m)^\s*\$acl\s*=\s*Get-Acl\b' `
        -Description 'original-user registration does not require Get-Acl'
    Assert-TextContains `
        -Text $registration `
        -Pattern 'Add-AppxPackage\s+`[\s\S]*-ExternalLocation\s+\$installRoot' `
        -Description 'current-user external-location package registration'
    Assert-TextContains `
        -Text $registration `
        -Pattern 'Get-AppxPackage\s+-Name\s+\$packageName' `
        -Description 'registration result verification'
    Assert-TextContains `
        -Text $registration `
        -Pattern 'packageFullName' `
        -Description 'installation records package full name'
    Assert-TextContains `
        -Text $registration `
        -Pattern 'userSid' `
        -Description 'installation records the registering user'
    $machineInstaller = Read-RepositoryText -RelativePath 'tools/installer/install-machine.ps1'
    Assert-TextContains `
        -Text $machineInstaller `
        -Pattern 'installedUserSid' `
        -Description 'protected install state records the registering user'
    Assert-TextContains `
        -Text $machineInstaller `
        -Pattern 'Write-ProtectedJson' `
        -Description 'protected install state is written with an ACL'
    Assert-TextContains `
        -Text $machineInstaller `
        -Pattern 'New-SelfSignedCertificate[\s\S]*Cert:\\LocalMachine\\My' `
        -Description 'target-machine certificate generation'
    Assert-TextContains `
        -Text $machineInstaller `
        -Pattern 'BAFX\.IdentitySigner\.exe[\s\S]*\-store-location' `
        -Description 'native target-machine package signing'
    Assert-TextContains `
        -Text $machineInstaller `
        -Pattern 'OpenRead\(\$signedPackagePath\)' `
        -Description 'generated package path is used for manifest validation'
    Assert-TextContains `
        -Text $machineInstaller `
        -Pattern '\-DeleteKey' `
        -Description 'private signing-key cleanup'
    Assert-TextContains `
        -Text $machineInstaller `
        -Pattern 'Write-ProtectedInstallState[\s\S]*\.bak' `
        -Description 'protected install-state backup'
    Assert-TextContains `
        -Text $machineInstaller `
        -Pattern 'transactionId[\s\S]*ownedCertificateThumbprints[\s\S]*ownedPackageFiles' `
        -Description 'transaction and cleanup ledgers'
    Assert-TextContains `
        -Text $machineInstaller `
        -Pattern '\$ownedCertificateThumbprints\s*=\s*@\([\s\S]*Split-Ledger[\s\S]*\$ownedPackageFiles\s*=\s*@\([\s\S]*Split-Ledger' `
        -Description 'final install-state ledgers are flattened before serialization'

    $controlCenter = Read-RepositoryText -RelativePath 'src/control-center/control_center_window.cpp'
    $controlCenterMain = Read-RepositoryText -RelativePath 'src/control-center/main.cpp'
    $activation = Read-RepositoryText -RelativePath 'src/control-center/package_activation.cpp'
    $updateCheck = Read-RepositoryText -RelativePath 'src/release_update/update_check.cpp'
    $writtenInstallStateSchema = [regex]::Match(
        $machineInstaller,
        '(?s)\$installState\s*=\s*\[ordered\]@\{.*?schema\s*=\s*([0-9]+)')
    $acceptedInstallStateSchema = [regex]::Match(
        $activation,
        'expectedInstallStateSchema\s*=\s*([0-9]+)U')
    Assert-True `
        -Condition $writtenInstallStateSchema.Success `
        -Message 'Installer contract could not find the written install-state schema.'
    Assert-True `
        -Condition $acceptedInstallStateSchema.Success `
        -Message 'Installer contract could not find the Control Center install-state schema.'
    Assert-True `
        -Condition (
            $writtenInstallStateSchema.Groups[1].Value -eq
                $acceptedInstallStateSchema.Groups[1].Value) `
        -Message 'Installer and Control Center install-state schemas differ.'
    $activationSources = $controlCenter + "`n" + $activation
    Assert-TextContains `
        -Text $activation `
        -Pattern 'rawContents[\s\S]*sameModernBytes[\s\S]*rawContents' `
        -Description 'Control Center compares install-state raw bytes before BOM normalization'
    $externalTrust = Read-RepositoryText `
        -RelativePath 'src/windows/src/external_host_trust.cpp'
    Assert-TextContains `
        -Text $externalTrust `
        -Pattern 'std::string primaryJson\s*=\s*primaryContents[\s\S]*stripUtf8Bom\(primaryJson\)[\s\S]*sameInstallStatePair' `
        -Description 'Host trust parses BOM-free copies while retaining raw pair bytes'
    Assert-TextContains `
        -Text $activationSources `
        -Pattern '(?i)(ApplicationActivationManager|IApplicationActivationManager|ActivateApplication)' `
        -Description 'Control Center Package Activation API'
    Assert-TextContains `
        -Text $controlCenter `
        -Pattern 'readPackageActivationState' `
        -Description 'Control Center reads protected install state'
    Assert-TextContains `
        -Text $controlCenter `
        -Pattern 'CreateProcessW\s*\(' `
        -Description 'portable fallback uses direct process creation'
    Assert-TextContains `
        -Text $controlCenter `
        -Pattern 'constexpr\s+wchar_t\s+obsSpoutPluginPage\[\]\s*=\s*L"https://github\.com/Off-World-Live/obs-spout2-plugin/releases"' `
        -Description 'fixed official OBS Spout2 plugin page'
    Assert-TextContains `
        -Text $controlCenter `
        -Pattern 'openFixedOfficialPage\s*\(\s*window_,\s*obsSpoutPluginPage\s*\)' `
        -Description 'official OBS Spout2 plugin page action'
    Assert-TextContains `
        -Text $updateCheck `
        -Pattern 'std::wstring_view\s+officialLatestReleasePageUrl\s*\(\s*\)\s*noexcept\s*\{\s*return\s+L"https://github\.com/CialloKing/ba-click-fx-desktop/releases/latest";\s*\}' `
        -Description 'fixed official latest Release page'
    Assert-TextContains `
        -Text $controlCenter `
        -Pattern 'openFixedOfficialPage\s*\(\s*window_,\s*bafx::release_update::officialLatestReleasePageUrl\s*\(\s*\)\.data\s*\(\s*\)\s*\)' `
        -Description 'fixed official latest Release page action'
    Assert-TextContains `
        -Text $updateCheck `
        -Pattern 'std::wstring_view\s+officialProjectRepositoryUrl\s*\(\s*\)\s*noexcept\s*\{\s*return\s+L"https://github\.com/CialloKing/ba-click-fx-desktop";\s*\}' `
        -Description 'fixed official project repository page'
    Assert-TextContains `
        -Text $controlCenter `
        -Pattern 'openFixedOfficialPage\s*\(\s*window_,\s*bafx::release_update::officialProjectRepositoryUrl\s*\(\s*\)\.data\s*\(\s*\)\s*\)' `
        -Description 'fixed official project repository page action'
    Assert-TextContains `
        -Text $controlCenter `
        -Pattern 'createChild\s*\(\s*L"BUTTON",\s*L"打开项目仓库",\s*BS_PUSHBUTTON\s*\|\s*WS_TABSTOP,\s*ControlId::OpenRepository\s*\)' `
        -Description 'always-enabled project repository button'
    Assert-TextContains `
        -Text $controlCenter `
        -Pattern 'case\s+ControlId::OpenRepository\s*:\s*if\s*\(\s*notificationCode\s*==\s*BN_CLICKED\s*\)\s*\{\s*openOfficialProjectRepository\s*\(\s*\)\s*;\s*\}\s*break\s*;' `
        -Description 'project repository button entry'

    # Each approved call site must use a fixed official target. The exact helper
    # reference total prevents another caller from silently adding navigation.
    $navigationSourceFiles = @(
        Get-ChildItem `
            -LiteralPath (Resolve-RepositoryPath -RelativePath 'src/control-center') `
            -Recurse `
            -File
        Get-ChildItem `
            -LiteralPath (Resolve-RepositoryPath -RelativePath 'src/release_update') `
            -Recurse `
            -File
    ) | Where-Object { $_.Extension -in @('.cpp', '.hpp') }
    $trustedNavigationSources = @(
        $navigationSourceFiles |
            Sort-Object -Property FullName |
            ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }
    ) -join "`n"
    $shellNavigationCalls = [regex]::Matches(
        $trustedNavigationSources,
        '\bShellExecute(?:Ex)?(?:A|W)?\s*\(')
    Assert-True `
        -Condition ($shellNavigationCalls.Count -eq 1) `
        -Message "Installer contract requires one centralized shell-navigation call; found $($shellNavigationCalls.Count)."
    $officialPageHelperReferences = [regex]::Matches(
        $controlCenter,
        '\bopenFixedOfficialPage\s*\(')
    Assert-True `
        -Condition ($officialPageHelperReferences.Count -eq 4) `
        -Message "Installer contract requires one official-page helper and three approved callers; found $($officialPageHelperReferences.Count) references."

    # Only the explicit button command may reach ReleaseUpdateChecker::start.
    # Keeping this as a unique source-level path proves that ordinary launch and
    # --startup construct the checker without starting WinHTTP work.
    Assert-TextContains `
        -Text $controlCenter `
        -Pattern 'case\s+ControlId::CheckForUpdates\s*:\s*if\s*\(\s*notificationCode\s*==\s*BN_CLICKED\s*\)\s*\{\s*beginManualUpdateCheck\s*\(\s*\)\s*;\s*\}\s*break\s*;' `
        -Description 'manual update-check button entry'
    $manualUpdateEntryReferences = [regex]::Matches(
        $controlCenter,
        '\bbeginManualUpdateCheck\s*\(\s*\)')
    Assert-True `
        -Condition ($manualUpdateEntryReferences.Count -eq 2) `
        -Message "Installer contract requires one manual update entry definition and one button call; found $($manualUpdateEntryReferences.Count) references."
    $manualUpdateEntry = [regex]::Match(
        $controlCenter,
        '(?m)^void\s+ControlCenterWindow::beginManualUpdateCheck\s*\(\s*\)[\s\S]*?(?=^void\s+ControlCenterWindow::pollManualUpdateCheck\s*\()')
    Assert-True `
        -Condition $manualUpdateEntry.Success `
        -Message 'Installer contract could not isolate the manual update-check entry.'
    $updateStartCalls = [regex]::Matches(
        $controlCenter,
        '\bupdateChecker_\s*->\s*start\s*\(\s*\)')
    Assert-True `
        -Condition ($updateStartCalls.Count -eq 1) `
        -Message "Installer contract requires exactly one update-check start call; found $($updateStartCalls.Count)."
    Assert-TextContains `
        -Text $manualUpdateEntry.Value `
        -Pattern '\bupdateChecker_\s*->\s*start\s*\(\s*\)' `
        -Description 'update checker starts only from the manual entry'

    $controlCenterCreate = [regex]::Match(
        $controlCenter,
        '(?m)^bool\s+ControlCenterWindow::create\s*\([\s\S]*?(?=^int\s+ControlCenterWindow::runMessageLoop\s*\()')
    Assert-True `
        -Condition $controlCenterCreate.Success `
        -Message 'Installer contract could not isolate Control Center creation.'
    Assert-TextExcludes `
        -Text $controlCenterCreate.Value `
        -Pattern 'beginManualUpdateCheck\s*\(|updateChecker_\s*->\s*start\s*\(' `
        -Description 'automatic update check during Control Center creation'
    Assert-TextContains `
        -Text $controlCenterMain `
        -Pattern 'if\s*\(\s*argument\s*==\s*L"--startup"\s*\)\s*\{\s*options\.startup\s*=\s*true;\s*\}' `
        -Description '--startup option parsing remains local'
    Assert-TextContains `
        -Text $controlCenterMain `
        -Pattern 'window\.create\s*\(\s*effectiveShowCommand\s*,\s*options\.startup\s*\)' `
        -Description '--startup only flows into Control Center creation'
    Assert-TextExcludes `
        -Text $controlCenterMain `
        -Pattern 'ReleaseUpdateChecker|beginManualUpdateCheck|fetchLatestRelease|officialLatestReleasePageUrl' `
        -Description 'update-check work from the process startup path'

    $latestReleaseFetchCalls = [regex]::Matches(
        $updateCheck,
        '\bfetchLatestRelease\s*\(')
    Assert-True `
        -Condition ($latestReleaseFetchCalls.Count -eq 1) `
        -Message "Installer contract requires exactly one latest-Release fetch call; found $($latestReleaseFetchCalls.Count)."
    Assert-TextContains `
        -Text $updateCheck `
        -Pattern 'void\s+ReleaseUpdateChecker::run\s*\([^)]*\)\s*noexcept\s*\{[\s\S]*?transport_\s*->\s*fetchLatestRelease\s*\(' `
        -Description 'latest-Release fetch remains inside the checker worker'
    $inno = Read-RepositoryText -RelativePath 'tools/installer/ba-click-fx-desktop.iss'
    Assert-TextExcludes `
        -Text $inno `
        -Pattern 'DisableSystemBorder' `
        -Description 'installer preserves the default visible system border'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'recovery state[\s\S]*were retained[\s\S]*Exit' `
        -Description 'failed setup retains recovery payload'

    $registration = Read-RepositoryText -RelativePath 'tools/installer/register-user-package.ps1'
    Assert-TextContains `
        -Text $registration `
        -Pattern 'transactionId[\s\S]*Remove-PreviousPackageForReplacement' `
        -Description 'registration transaction binding and same-version replacement'
    $machineInstaller = Read-RepositoryText -RelativePath 'tools/installer/install-machine.ps1'
    $uninstaller = Read-RepositoryText -RelativePath 'tools/installer/unregister-machine.ps1'
    Assert-TextContains `
        -Text $uninstaller `
        -Pattern 'writeRights\s*=\s*\[int\]\(\[Security\.AccessControl\.FileSystemRights\]::WriteData[\s\S]*TakeOwnership\)' `
        -Description 'uninstall ACL validation uses atomic write rights'
    Assert-TextExcludes `
        -Text $uninstaller `
        -Pattern 'writeRights[\s\S]*?FileSystemRights\]::(Modify|FullControl)' `
        -Description 'uninstall write mask does not include read bits from composite rights'
    Assert-TextContains `
        -Text $uninstaller `
        -Pattern 'Read-InstallStateWithBackup[\s\S]*ownedCertificateThumbprints[\s\S]*\-DeleteKey' `
        -Description 'uninstall backup and complete certificate ledger cleanup'
    Assert-TextExcludes `
        -Text $uninstaller `
        -Pattern '\bSet-ProtectedStateAcl\b' `
        -Description 'undefined uninstall state ACL repair helper'
    Assert-TextContains `
        -Text $uninstaller `
        -Pattern 'function\s+Remove-ProtectedInstallStatePair[\s\S]*primaryAcl[\s\S]*backupAcl[\s\S]*Assert-InstallStatePair' `
        -Description 'guarded primary and backup install-state cleanup'
    Assert-TextContains `
        -Text $uninstaller `
        -Pattern 'Remove-ProtectedInstallStatePair\s+-Path\s+\$statePath' `
        -Description 'uninstall uses the transactional state-pair cleanup helper'
    Assert-TextContains `
        -Text $uninstaller `
        -Pattern 'function\s+Write-UninstallJournal[\s\S]*primaryStateBase64[\s\S]*state-removing' `
        -Description 'uninstall journal snapshots state before destructive deletion'
    Assert-TextContains `
        -Text $uninstaller `
        -Pattern 'function\s+Read-InstallStateWithBackup[\s\S]*Read-UninstallJournal[\s\S]*Restore-InstallStateFromUninstallJournal' `
        -Description 'uninstall retries recover a torn state pair from its journal'
    Assert-TextContains `
        -Text $machineInstaller `
        -Pattern 'function\s+Assert-InstallStateRawPair[\s\S]*Assert-InstallStatePair[\s\S]*PrimaryPath' `
        -Description 'machine recovery requires byte-identical install-state files'
    Assert-TextContains `
        -Text $registration `
        -Pattern 'function\s+Assert-InstallStateRawPair[\s\S]*Get-InstallStatePairStatus[\s\S]*Assert-InstallStateRawPair' `
        -Description 'user registration rejects byte-divergent install-state files'
    Assert-TextContains `
        -Text $uninstaller `
        -Pattern 'function\s+Assert-InstallStateRawPair[\s\S]*Read-InstallStateWithBackup[\s\S]*PrimaryPath' `
        -Description 'uninstall rejects byte-divergent install-state files'
    Assert-TextContains `
        -Text $uninstaller `
        -Pattern 'Get-UninstallPhaseRank[\s\S]*monotonic[\s\S]*return' `
        -Description 'uninstall journal phases never regress on retry'

    Assert-TextContains `
        -Text $machineInstaller `
        -Pattern 'Read-OldInstallState[\s\S]*SkipPayloadIntegrity[\s\S]*filesCommitted' `
        -Description 'pending recovery classifies a torn state pair from its journal'
    Assert-TextContains `
        -Text $machineInstaller `
        -Pattern '\$schema\s+-eq\s+2[\s\S]*templateSha256' `
        -Description 'schema 1 pending recovery does not require the schema 2 template hash'
    Assert-TextContains `
        -Text $registration `
        -Pattern '\$schema\s+-eq\s+2[\s\S]*templateSha256' `
        -Description 'user-context registration accepts legacy schema 1 pending state'
    $uninstallerAst = Get-ParsedScript `
        -RelativePath 'tools/installer/unregister-machine.ps1'
    $profileHiveResolver = Get-FunctionText `
        -Ast $uninstallerAst `
        -Name 'Get-InstalledUserProfileHivePath'
    $registryHiveCommand = Get-FunctionText `
        -Ast $uninstallerAst `
        -Name 'Invoke-BoundedRegistryHiveCommand'
    $startupValueCleanup = Get-FunctionText `
        -Ast $uninstallerAst `
        -Name 'Remove-StartupValueFromLoadedHive'
    $startupCleanup = Get-FunctionText `
        -Ast $uninstallerAst `
        -Name 'Remove-InstalledUserStartupRegistration'
    Assert-TextContains `
        -Text $startupCleanup `
        -Pattern 'SecurityIdentifier\]::new\(\$InstalledUserSid\)[\s\S]*\$sid\.Value\s+-ne\s+\$InstalledUserSid' `
        -Description 'startup cleanup validates the protected installed-user SID'
    Assert-TextContains `
        -Text $profileHiveResolver `
        -Pattern 'RegistryHive\]::LocalMachine[\s\S]*RegistryView\]::Registry64[\s\S]*ProfileList\\\$InstalledUserSid' `
        -Description 'offline startup cleanup trusts the machine ProfileList SID mapping'
    Assert-TextContains `
        -Text $profileHiveResolver `
        -Pattern 'ProfileImagePath[\s\S]*DoNotExpandEnvironmentNames[\s\S]*Join-Path\s+\$profilePath\s+''NTUSER\.DAT''[\s\S]*Test-Path\s+-LiteralPath\s+\$hivePath\s+-PathType\s+Leaf' `
        -Description 'offline startup cleanup resolves an existing NTUSER.DAT'
    Assert-TextContains `
        -Text $profileHiveResolver `
        -Pattern 'SpecialFolder\]::Windows[\s\S]*GetPathRoot\(\$windowsDirectory\)[\s\S]*unsupported environment token' `
        -Description 'ProfileList expansion does not trust the invoking environment'
    Assert-TextContains `
        -Text $registryHiveCommand `
        -Pattern 'ValidateSet\(''Load'',\s*''Unload''\)[\s\S]*TimeoutMilliseconds\s*=\s*10000[\s\S]*WaitForExit\(\$TimeoutMilliseconds\)[\s\S]*\.Kill\(\)' `
        -Description 'offline registry hive commands have a hard timeout'
    Assert-TextContains `
        -Text $registryHiveCommand `
        -Pattern 'SpecialFolder\]::System[\s\S]*reg\.exe[\s\S]*ExitCode\s+-ne\s+0[\s\S]*throw' `
        -Description 'offline registry hive command failures propagate'
    Assert-TextContains `
        -Text $startupValueCleanup `
        -Pattern '\.DeleteValue\(\s*''BAFX Control Center''\s*,\s*\$false\s*\)' `
        -Description 'startup cleanup removes only the BAFX Control Center value idempotently'
    Assert-TextContains `
        -Text $startupValueCleanup `
        -Pattern 'Software\\Microsoft\\Windows\\CurrentVersion\\Run[\s\S]*GetValueNames\(\)' `
        -Description 'startup cleanup verifies the exact Run value was removed'
    Assert-TextExcludes `
        -Text $startupValueCleanup `
        -Pattern '(DeleteSubKey|DeleteSubKeyTree|Remove-Item)' `
        -Description 'startup cleanup deleting a registry key'
    Assert-TextContains `
        -Text $startupCleanup `
        -Pattern 'Test-RegistryHiveMounted\s+-HiveName\s+\$sid\.Value[\s\S]*Get-InstalledUserProfileHivePath[\s\S]*BAFX_Uninstall_\$\{PID\}_\$\(\[Guid\]::NewGuid\(\)\.ToString\(''N''\)\)[\s\S]*Test-RegistryHiveMounted\s+-HiveName\s+\$temporaryHiveName[\s\S]*already mounted' `
        -Description 'offline startup cleanup uses a unique temporary HKU mount'
    Assert-TextContains `
        -Text $startupCleanup `
        -Pattern 'try[\s\S]*\$loadAttempted\s*=\s*\$true[\s\S]*-Operation\s+Load[\s\S]*Remove-StartupValueFromLoadedHive[\s\S]*finally[\s\S]*\$loadAttempted[\s\S]*-Operation\s+Unload[\s\S]*remains after unload' `
        -Description 'offline startup cleanup always verifies temporary hive unload'
    Assert-TextContains `
        -Text $uninstaller `
        -Pattern 'InstallerStep\s*=\s*''remove-installed-user-startup-registration''[\s\S]*Remove-InstalledUserStartupRegistration\s+`?\s*-InstalledUserSid\s+\(\[string\]\$state\.installedUserSid\)[\s\S]*InstallerStep\s*=\s*''query-installed-user-package''' `
        -Description 'startup cleanup uses protected state before uninstalling files'
    Assert-TextContains `
        -Text $uninstaller `
        -Pattern 'foreach\s*\(\$directoryName\s+in\s+@\(''Identity'',\s*''\.rollback'',\s*''\.staging''\)[\s\S]*Assert-NoReparseTree\s+-Path\s+\$path[\s\S]*Remove-Item\s+-LiteralPath\s+\$path\s+-Recurse' `
        -Description 'uninstall validates every protected tree before recursive deletion'
    Assert-TextContains `
        -Text $uninstaller `
        -Pattern 'foreach\s*\(\$relativePath\s+in\s+\$knownFiles\)[\s\S]*Assert-NoReparsePath\s+-Path\s+\$path[\s\S]*Remove-Item\s+-LiteralPath\s+\$path\s+-Force' `
        -Description 'uninstall validates protected files before elevated deletion'
    $recursiveDeleteCalls = @(
        [regex]::Matches(
            $uninstaller,
            'Remove-Item\s+-LiteralPath\s+\$temporaryRoot\s+-Recurse\s+-Force')
    )
    Assert-True `
        -Condition ($recursiveDeleteCalls.Count -eq 3) `
        -Message 'Uninstaller temporary recovery trees changed unexpectedly.'
    Assert-TextContains `
        -Text $uninstaller `
        -Pattern 'Assert-NoReparseTree\s+-Path\s+\$temporaryRoot[\s\S]*Remove-Item\s+-LiteralPath\s+\$temporaryRoot\s+-Recurse' `
        -Description 'uninstall validates temporary recovery trees before recursive deletion'
}

function Test-UninstallerStatePairContract
{
    $ast = Get-ParsedScript `
        -RelativePath 'tools/installer/unregister-machine.ps1'
    $reader = Get-FunctionText -Ast $ast -Name 'Read-InstallStateWithBackup'
    $moduleText = @'
Set-StrictMode -Version Latest
function Assert-NoReparsePath
{
    param(
        [string]$Path,
        [switch]$AllowMissing
    )
}
function Assert-ProtectedStateAcl
{
    param([string]$Path)
}

function Assert-InstallStateRawPair
{
    param(
        [string]$PrimaryPath,
        [string]$BackupPath
    )
    $primaryBytes = [IO.File]::ReadAllBytes($PrimaryPath)
    $backupBytes = [IO.File]::ReadAllBytes($BackupPath)
    if ($primaryBytes.Length -ne $backupBytes.Length)
    {
        throw 'raw state pair mismatch'
    }
    for ($index = 0; $index -lt $primaryBytes.Length; ++$index)
    {
        if ($primaryBytes[$index] -ne $backupBytes[$index])
        {
            throw 'raw state pair mismatch'
        }
    }
}

function Assert-InstallStatePair
{
    param(
        [object]$Primary,
        [object]$Backup,
        [string]$PrimaryPath = '',
        [string]$BackupPath = ''
    )

    if (-not [string]::IsNullOrWhiteSpace($PrimaryPath))
    {
        Assert-InstallStateRawPair -PrimaryPath $PrimaryPath -BackupPath $BackupPath
    }
    if ([string]$Primary.transactionId -ne [string]$Backup.transactionId -or
        [string]$Primary.stateDigest -ne [string]$Backup.stateDigest)
    {
        throw 'state pair mismatch'
    }
}

function Assert-InstallStateIntegrity
{
    param(
        [object]$State,
        [string]$InstallRoot,
        [object]$UninstallJournal
    )
}

function Read-UninstallJournal
{
    param(
        [string]$Path,
        [object]$State
    )
    return $null
}

function Restore-InstallStateFromUninstallJournal
{
    param(
        [string]$Path,
        [object]$Journal
    )
}

'@ + "`n" + $reader
    $readerModule = New-Module -ScriptBlock ([scriptblock]::Create($moduleText))

    $temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    $temporaryRoot = Join-Path `
        $temporaryParent `
        ('bafx-uninstall-backup-' + [Guid]::NewGuid().ToString('N'))
    try
    {
        New-Item -ItemType Directory -Path $temporaryRoot -Force | Out-Null
        $statePath = Join-Path $temporaryRoot 'INSTALL-STATE.json'
        $backupPath = "$statePath.bak"
        $state = [ordered]@{
            schema = 2
            transactionId = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
            stateDigest = ('b' * 64)
            certificateThumbprint = ('1' * 40)
            packageFile = 'identity.msix'
        } | ConvertTo-Json -Compress
        [IO.File]::WriteAllText($statePath, $state)
        [IO.File]::WriteAllText($backupPath, $state)

        $readState = {
            param($Path, $InstallRoot)
            Read-InstallStateWithBackup -Path $Path -InstallRoot $InstallRoot
        }
        $pairState = & $readerModule $readState $statePath $temporaryRoot
        Assert-True `
            -Condition ([string]$pairState.transactionId -eq
                'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa') `
            -Message 'Uninstaller did not accept a matching state pair.'

        [IO.File]::WriteAllText($backupPath, $state + "`n")
        Assert-Throws `
            -Action {
                & $readerModule $readState $statePath $temporaryRoot
            } `
            -Description 'semantically equal but byte-different state pair'
        [IO.File]::WriteAllText($backupPath, $state)

        [IO.File]::WriteAllText($statePath, '{broken')
        Assert-Throws `
            -Action {
                & $readerModule $readState $statePath $temporaryRoot
            } `
            -Description 'corrupt primary with a valid backup'
        Assert-True `
            -Condition ((Get-Content -LiteralPath $statePath -Raw) -eq '{broken') `
            -Message 'Uninstaller rewrote the corrupt primary while rejecting it.'

        [IO.File]::WriteAllText($statePath, $state)
        [IO.File]::WriteAllText($backupPath, ($state | ConvertFrom-Json |
            ForEach-Object {
                $_.stateDigest = ('c' * 64)
                $_ | ConvertTo-Json -Compress
            }))
        Assert-Throws `
            -Action {
                & $readerModule $readState $statePath $temporaryRoot
            } `
            -Description 'mismatched state digests'

        Remove-Item -LiteralPath $backupPath -Force
        Assert-Throws `
            -Action {
                & $readerModule $readState $statePath $temporaryRoot
            } `
            -Description 'missing backup state'

        Remove-Item -LiteralPath $statePath -Force
        Assert-Throws `
            -Action {
                & $readerModule $readState $statePath $temporaryRoot
            } `
            -Description 'missing primary and backup states'
    }
    finally
    {
        if ($null -ne $readerModule)
        {
            Remove-Module -ModuleInfo $readerModule -Force -ErrorAction SilentlyContinue
        }
        $resolvedTemporaryRoot = [IO.Path]::GetFullPath($temporaryRoot)
        if ($resolvedTemporaryRoot.StartsWith(
                $temporaryParent,
                [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($resolvedTemporaryRoot).StartsWith(
                'bafx-uninstall-backup-',
                [StringComparison]::Ordinal))
        {
            Remove-Item `
                -LiteralPath $resolvedTemporaryRoot `
                -Recurse `
                -Force `
                -ErrorAction SilentlyContinue
        }
    }
}

function Test-UninstallerCompletionMarkerContract
{
    $ast = Get-ParsedScript `
        -RelativePath 'tools/installer/unregister-machine.ps1'
    $reader = Get-FunctionText `
        -Ast $ast `
        -Name 'Read-UninstallCompletionMarker'
    $moduleText = @'
Set-StrictMode -Version Latest
$script:Journal = [pscustomobject]@{
    schema = 1
    transactionId = ('a' * 32)
    stateDigest = ('b' * 64)
    phase = 'state-removing'
}
function Assert-ProtectedStateAcl
{
    param([string]$Path)
}
function Read-UninstallJournal
{
    param([string]$Path)
    return $script:Journal
}
'@ + "`n" + $reader
    $readerModule = New-Module -ScriptBlock ([scriptblock]::Create($moduleText))
    $temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    $temporaryRoot = Join-Path `
        $temporaryParent `
        ('bafx-uninstall-marker-' + [Guid]::NewGuid().ToString('N'))
    try
    {
        New-Item -ItemType Directory -Path $temporaryRoot -Force | Out-Null
        $markerPath = Join-Path $temporaryRoot 'UNINSTALL-COMPLETE.json'
        $journalPath = Join-Path $temporaryRoot 'UNINSTALL-STATE.json'
        $marker = [ordered]@{
            schema = 1
            transactionId = ('a' * 32)
            stateDigest = ('b' * 64)
            stateRemoved = $true
            completedUtc = [DateTime]::UtcNow.ToString('o')
        }
        $marker | ConvertTo-Json -Depth 4 |
            Set-Content -LiteralPath $markerPath -Encoding UTF8
        $read = & $readerModule {
            param($MarkerPath, $JournalPath)
            Read-UninstallCompletionMarker `
                -Path $MarkerPath `
                -JournalPath $JournalPath
        } $markerPath $journalPath
        Assert-True `
            -Condition ([bool]$read.stateRemoved) `
            -Message 'Valid uninstall completion marker was rejected.'

        $marker.stateRemoved = $false
        $marker | ConvertTo-Json -Depth 4 |
            Set-Content -LiteralPath $markerPath -Encoding UTF8
        Assert-Throws `
            -Action {
                & $readerModule {
                    param($MarkerPath, $JournalPath)
                    Read-UninstallCompletionMarker `
                        -Path $MarkerPath `
                        -JournalPath $JournalPath
                } $markerPath $journalPath
            } `
            -Description 'unfinished uninstall completion marker'

        $marker.stateRemoved = $true
        $marker.transactionId = ('c' * 32)
        $marker | ConvertTo-Json -Depth 4 |
            Set-Content -LiteralPath $markerPath -Encoding UTF8
        Assert-Throws `
            -Action {
                & $readerModule {
                    param($MarkerPath, $JournalPath)
                    Read-UninstallCompletionMarker `
                        -Path $MarkerPath `
                        -JournalPath $JournalPath
                } $markerPath $journalPath
            } `
            -Description 'completion marker from another transaction'
    }
    finally
    {
        if ($null -ne $readerModule)
        {
            Remove-Module -ModuleInfo $readerModule -Force -ErrorAction SilentlyContinue
        }
        $resolvedTemporaryRoot = [IO.Path]::GetFullPath($temporaryRoot)
        if ($resolvedTemporaryRoot.StartsWith(
                $temporaryParent,
                [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($resolvedTemporaryRoot).StartsWith(
                'bafx-uninstall-marker-',
                [StringComparison]::Ordinal))
        {
            Remove-Item -LiteralPath $resolvedTemporaryRoot -Recurse -Force `
                -ErrorAction SilentlyContinue
        }
    }
}

function Test-InstallStateWriteFaultInjectionContract
{
    $ast = Get-ParsedScript `
        -RelativePath 'tools/installer/install-machine.ps1'
    $pairText = Get-FunctionText `
        -Ast $ast `
        -Name 'Assert-InstallStatePair'
    $pairCoreText = $pairText -replace `
        'function\s+Assert-InstallStatePair\b',
        'function Assert-InstallStatePairCore'
    $functionText = @(
        'Set-StrictMode -Version Latest'
        "`$ErrorActionPreference = 'Stop'"
        "`$global:BafxStateFaultPoint = ''"
        "`$global:BafxStatePrimaryPath = ''"
        @'
function Assert-NoReparsePath
{
    param(
        [string]$Path,
        [switch]$AllowMissing
    )
}

function Set-ProtectedStateAcl
{
    param(
        [string]$Path,
        [string]$ReadSid
    )
    if ($global:BafxStateFaultPoint -eq 'acl')
    {
        throw 'injected ACL failure'
    }
}

function Write-FlushedUtf8NoBom
{
    param(
        [string]$Path,
        [string]$Content
    )
    if ($global:BafxStateFaultPoint -eq 'temporary-write')
    {
        throw 'injected temporary write failure'
    }
    $encoding = New-Object -TypeName System.Text.UTF8Encoding -ArgumentList $false
    $bytes = $encoding.GetBytes($Content)
    $stream = [IO.FileStream]::new(
        $Path,
        [IO.FileMode]::Create,
        [IO.FileAccess]::Write,
        [IO.FileShare]::None)
    try
    {
        $stream.Write($bytes, 0, $bytes.Length)
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
        [string]$TemporaryPath,
        [string]$DestinationPath,
        [string]$ReadSid
    )
    $destination = [IO.Path]::GetFullPath($DestinationPath)
    if ($global:BafxStateFaultPoint -eq 'backup-replace' -and
        $destination.EndsWith('.bak', [StringComparison]::OrdinalIgnoreCase))
    {
        throw 'injected backup replacement failure'
    }
    if ($global:BafxStateFaultPoint -eq 'primary-replace' -and
        $destination -eq $global:BafxStatePrimaryPath)
    {
        throw 'injected primary replacement failure'
    }
    Set-ProtectedStateAcl -Path $TemporaryPath -ReadSid $ReadSid
    if (Test-Path -LiteralPath $destination -PathType Leaf)
    {
        Remove-Item -LiteralPath $destination -Force
        [IO.File]::Move(
            [IO.Path]::GetFullPath($TemporaryPath),
            $destination)
    }
    else
    {
        [IO.File]::Move([IO.Path]::GetFullPath($TemporaryPath), $destination)
    }
    Set-ProtectedStateAcl -Path $destination -ReadSid $ReadSid
}

function Get-SerializedState
{
    param([object]$Value)
    $state = New-StateWithDigest -Value $Value
    return ($state | ConvertTo-Json -Depth 12)
}
'@
        (Get-FunctionText -Ast $ast -Name 'Get-StatePropertiesWithoutDigest')
        (Get-FunctionText -Ast $ast -Name 'Convert-StateToCanonicalJson')
        (Get-FunctionText -Ast $ast -Name 'Get-StateDigest')
        (Get-FunctionText -Ast $ast -Name 'New-StateWithDigest')
        (Get-FunctionText -Ast $ast -Name 'Assert-InstallStateRawPair')
        $pairCoreText
        @'
function Assert-InstallStatePair
{
    param(
        [object]$Primary,
        [object]$Backup,
        [string]$PrimaryPath = '',
        [string]$BackupPath = ''
    )
    if ($global:BafxStateFaultPoint -eq 'readback')
    {
        throw 'injected readback validation failure'
    }
    Assert-InstallStatePairCore @PSBoundParameters
}
'@
        (Get-FunctionText -Ast $ast -Name 'Write-ProtectedInstallState')
    ) -join "`n"
    $probeModule = New-Module -ScriptBlock ([scriptblock]::Create($functionText))

    $temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    $temporaryRoot = Join-Path `
        $temporaryParent `
        ('bafx-install-state-faults-' + [Guid]::NewGuid().ToString('N'))
    try
    {
        New-Item -ItemType Directory -Path $temporaryRoot -Force | Out-Null
        $statePath = Join-Path $temporaryRoot 'INSTALL-STATE.json'
        $backupPath = "$statePath.bak"
        $oldValue = [ordered]@{
            schema = 2
            transactionId = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
            marker = 'old'
        }
        $newValue = [ordered]@{
            schema = 2
            transactionId = 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'
            marker = 'new'
        }
        $oldSerialized = & $probeModule {
            param($Value)
            Get-SerializedState -Value $Value
        } $oldValue
        $newSerialized = & $probeModule {
            param($Value)
            Get-SerializedState -Value $Value
        } $newValue
        $encoding = New-Object -TypeName System.Text.UTF8Encoding -ArgumentList $false
        $oldBytes = $encoding.GetBytes([string]$oldSerialized)
        $newBytes = $encoding.GetBytes([string]$newSerialized)

        $cases = @(
            @{ name = 'temporary-write'; expected = 'old' }
            @{ name = 'acl'; expected = 'old' }
            @{ name = 'backup-replace'; expected = 'old' }
            @{ name = 'primary-replace'; expected = 'mixed' }
            @{ name = 'readback'; expected = 'new' }
        )
        foreach ($case in $cases)
        {
            [IO.File]::WriteAllBytes($statePath, $oldBytes)
            [IO.File]::WriteAllBytes($backupPath, $oldBytes)
            $threw = $false
            try
            {
                & $probeModule {
                    param($Point, $PrimaryPathValue, $Path, $Value)
                    $global:BafxStateFaultPoint = $Point
                    $global:BafxStatePrimaryPath = [IO.Path]::GetFullPath($PrimaryPathValue)
                    Write-ProtectedInstallState `
                        -Path $Path `
                        -Value $Value `
                        -ReadSid 'S-1-5-21-1'
                } $case.name $statePath $statePath $newValue
            }
            catch
            {
                $threw = $true
            }
            Assert-True `
                -Condition $threw `
                -Message "Injected state writer failure did not surface: $($case.name)"
            $primaryBytes = [IO.File]::ReadAllBytes($statePath)
            $backupBytes = [IO.File]::ReadAllBytes($backupPath)
            $temporaryFiles = @(Get-ChildItem -LiteralPath $temporaryRoot -Filter '*.tmp' -File)
            Assert-True `
                -Condition ($temporaryFiles.Count -eq 0) `
                -Message "State writer leaked a temporary file: $($case.name)"

            if ($case.expected -eq 'old')
            {
                Assert-True `
                    -Condition ([Convert]::ToBase64String($primaryBytes) -eq
                        [Convert]::ToBase64String($oldBytes)) `
                    -Message "State writer changed the primary on an early failure: $($case.name)"
                Assert-True `
                    -Condition ([Convert]::ToBase64String($backupBytes) -eq
                        [Convert]::ToBase64String($oldBytes)) `
                    -Message "State writer changed the backup on an early failure: $($case.name)"
            }
            elseif ($case.expected -eq 'mixed')
            {
                Assert-True `
                    -Condition ([Convert]::ToBase64String($primaryBytes) -ne
                        [Convert]::ToBase64String($backupBytes)) `
                    -Message 'Primary replacement injection did not create the expected torn pair.'
                $pairRejected = $false
                try
                {
                    & $probeModule {
                        param($Path, $BackupPathValue)
                        $primary = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
                        $backup = Get-Content -LiteralPath $BackupPathValue -Raw | ConvertFrom-Json
                        Assert-InstallStatePairCore `
                            -Primary $primary `
                            -Backup $backup `
                            -PrimaryPath $Path `
                            -BackupPath $BackupPathValue
                    } $statePath $backupPath
                }
                catch
                {
                    $pairRejected = $true
                }
                Assert-True `
                    -Condition $pairRejected `
                    -Message 'A mixed install-state transaction was accepted after primary replacement failure.'
            }
            else
            {
                Assert-True `
                    -Condition ([Convert]::ToBase64String($primaryBytes) -eq
                        [Convert]::ToBase64String($newBytes)) `
                    -Message 'Readback failure did not leave the candidate primary state intact.'
                Assert-True `
                    -Condition ([Convert]::ToBase64String($backupBytes) -eq
                        [Convert]::ToBase64String($newBytes)) `
                    -Message 'Readback failure left a mixed state pair.'
            }
        }

        & $probeModule {
            param($Path, $PrimaryPathValue, $Value)
            $global:BafxStateFaultPoint = ''
            $global:BafxStatePrimaryPath = [IO.Path]::GetFullPath($PrimaryPathValue)
            Write-ProtectedInstallState `
                -Path $Path `
                -Value $Value `
                -ReadSid 'S-1-5-21-1'
        } $statePath $statePath $newValue
        $successfulPrimary = [IO.File]::ReadAllBytes($statePath)
        $successfulBackup = [IO.File]::ReadAllBytes($backupPath)
        Assert-True `
            -Condition ([Convert]::ToBase64String($successfulPrimary) -eq
                [Convert]::ToBase64String($successfulBackup)) `
            -Message 'Successful state commit did not produce identical primary and backup bytes.'
    }
    finally
    {
        Remove-Variable -Name BafxStateFaultPoint, BafxStatePrimaryPath `
            -Scope Global -ErrorAction SilentlyContinue
        if ($null -ne $probeModule)
        {
            Remove-Module -ModuleInfo $probeModule -Force -ErrorAction SilentlyContinue
        }
        $resolvedTemporaryRoot = [IO.Path]::GetFullPath($temporaryRoot)
        if ($resolvedTemporaryRoot.StartsWith(
                $temporaryParent,
                [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($resolvedTemporaryRoot).StartsWith(
                'bafx-install-state-faults-',
                [StringComparison]::Ordinal))
        {
            Remove-Item `
                -LiteralPath $resolvedTemporaryRoot `
                -Recurse `
                -Force `
                -ErrorAction SilentlyContinue
        }
    }
}

function Test-UninstallerOfflineHiveFailureContract
{
    $ast = Get-ParsedScript `
        -RelativePath 'tools/installer/unregister-machine.ps1'
    $cleanup = Get-FunctionText `
        -Ast $ast `
        -Name 'Remove-InstalledUserStartupRegistration'
    $moduleText = @'
Set-StrictMode -Version Latest
$script:InstallerStep = ''
$script:ProbeFailure = ''
$script:ProbeMounted = $false
$script:ProbeEvents = New-Object Collections.Generic.List[string]

function Test-RegistryHiveMounted
{
    param([string]$HiveName)
    return $script:ProbeMounted
}

function Get-InstalledUserProfileHivePath
{
    param([string]$InstalledUserSid)
    $script:ProbeEvents.Add('Resolve')
    return 'C:\TrustedProfile\NTUSER.DAT'
}

function Invoke-BoundedRegistryHiveCommand
{
    param(
        [string]$Operation,
        [string]$HiveName,
        [string]$HiveFilePath = '')
    $script:ProbeEvents.Add($Operation)
    if ($Operation -eq 'Load')
    {
        # Model reg.exe reporting failure after the hive became visible.
        $script:ProbeMounted = $true
    }
    if ($script:ProbeFailure -eq $Operation)
    {
        throw "Probe $Operation failure."
    }
    if ($Operation -eq 'Unload')
    {
        $script:ProbeMounted = $false
    }
}

function Remove-StartupValueFromLoadedHive
{
    param([string]$HiveName)
    $script:ProbeEvents.Add('Remove')
    if ($script:ProbeFailure -eq 'Remove')
    {
        throw 'Probe Remove failure.'
    }
}
'@ + "`n" + $cleanup
    $probeModule = New-Module -ScriptBlock ([scriptblock]::Create($moduleText))
    try
    {
        $expectedEvents = @{
            Load = @('Resolve', 'Load', 'Unload')
            Remove = @('Resolve', 'Load', 'Remove', 'Unload')
            Unload = @('Resolve', 'Load', 'Remove', 'Unload')
        }
        $expectedSteps = @{
            Load = 'load-installed-user-registry-hive'
            Remove = 'remove-installed-user-startup-registration'
            Unload = 'unload-installed-user-registry-hive'
        }
        foreach ($failure in @('Load', 'Remove', 'Unload'))
        {
            $result = & $probeModule {
                param($Failure)
                $script:InstallerStep = 'remove-installed-user-startup-registration'
                $script:ProbeFailure = $Failure
                $script:ProbeMounted = $false
                $script:ProbeEvents.Clear()
                $failureMessage = ''
                try
                {
                    Remove-InstalledUserStartupRegistration `
                        -InstalledUserSid 'S-1-5-21-1000-1000-1000-1000'
                }
                catch
                {
                    $failureMessage = $_.Exception.Message
                }
                return [PSCustomObject]@{
                    failureMessage = $failureMessage
                    events = @($script:ProbeEvents)
                    mounted = $script:ProbeMounted
                    installerStep = $script:InstallerStep
                }
            } $failure

            Assert-True `
                -Condition (-not [string]::IsNullOrWhiteSpace($result.failureMessage)) `
                -Message "Offline hive $failure failure did not propagate."
            Assert-ArrayEquals `
                -Expected $expectedEvents[$failure] `
                -Actual @($result.events) `
                -Description "offline hive $failure failure cleanup order"
            Assert-True `
                -Condition ([string]$result.installerStep -eq $expectedSteps[$failure]) `
                -Message "Offline hive $failure failure reported the wrong uninstall step."
            if ($failure -ne 'Unload')
            {
                Assert-True `
                    -Condition (-not [bool]$result.mounted) `
                    -Message "Offline hive $failure failure left the temporary hive mounted."
            }
        }
    }
    finally
    {
        if ($null -ne $probeModule)
        {
            Remove-Module -ModuleInfo $probeModule -Force -ErrorAction SilentlyContinue
        }
    }
}

function Test-UpgradeHostIntegrityContract
{
    $ast = Get-ParsedScript `
        -RelativePath 'tools/installer/install-machine.ps1'
    $assertionText = Get-FunctionText `
        -Ast $ast `
        -Name 'Assert-ReplacementHostIntegrity'
    . ([scriptblock]::Create($assertionText))

    $committedHash = (('A' * 64) -join '')
    $replacementHash = (('B' * 64) -join '')
    Assert-ReplacementHostIntegrity `
        -CurrentHostSha256 $replacementHash `
        -ExpectedReplacementHostSha256 $replacementHash `
        -ArchivedHostSha256 $committedHash `
        -CommittedHostSha256 $committedHash

    Assert-Throws `
        -Action {
            Assert-ReplacementHostIntegrity `
                -CurrentHostSha256 (('C' * 64) -join '') `
                -ExpectedReplacementHostSha256 $replacementHash `
                -ArchivedHostSha256 $committedHash `
                -CommittedHostSha256 $committedHash
        } `
        -Description 'replacement Host outside the validated payload'
    Assert-Throws `
        -Action {
            Assert-ReplacementHostIntegrity `
                -CurrentHostSha256 $replacementHash `
                -ExpectedReplacementHostSha256 $replacementHash `
                -ArchivedHostSha256 (('D' * 64) -join '') `
                -CommittedHostSha256 $committedHash
        } `
        -Description 'old state whose archived Host does not match'
}

function Test-PortableZipContract
{
    $portablePackaging = Read-RepositoryText -RelativePath 'tools/package-test-bundle.ps1'
    Assert-TextContains `
        -Text $portablePackaging `
        -Pattern 'ba-click-fx-desktop-\$version\$variantSuffix-Portable-windows-x64' `
        -Description 'portable release archive name identifies the Portable package'
    Assert-TextExcludes `
        -Text $portablePackaging `
        -Pattern 'ba-click-fx-desktop-\$version\$variantSuffix-test-windows-x64' `
        -Description 'legacy test bundle release archive name'

    $portableVerifier = Read-RepositoryText -RelativePath 'tools/verify-release-package.ps1'
    Assert-TextContains `
        -Text $portableVerifier `
        -Pattern 'BAFX\.ControlCenter\.exe' `
        -Description 'portable ZIP Control Center executable'
    Assert-TextContains `
        -Text $portableVerifier `
        -Pattern 'LICENSE\.txt' `
        -Description 'portable ZIP license'
    Assert-TextContains `
        -Text $portableVerifier `
        -Pattern 'SUPPORT\.md' `
        -Description 'portable ZIP support document'
    Assert-TextContains `
        -Text $portableVerifier `
        -Pattern 'ba-click-fx-desktop\.exe' `
        -Description 'portable ZIP Host executable'
    Assert-TextContains `
        -Text $portableVerifier `
        -Pattern 'Runtime data escaped the executable directory' `
        -Description 'portable runtime data stays beside the executable'

    $packaging = Read-RepositoryText -RelativePath 'cmake/Packaging.cmake'
    Assert-TextContains `
        -Text $packaging `
        -Pattern 'CPACK_GENERATOR\s+"ZIP"' `
        -Description 'portable packaging remains ZIP'
    Assert-TextContains `
        -Text $packaging `
        -Pattern 'verify_release_package' `
        -Description 'portable package verification target remains available'
}

function Test-RegistrationFailureDiagnostics
{
    $temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    $temporaryRoot = Join-Path `
        $temporaryParent `
        ('bafx-registration-diagnostic-' + [Guid]::NewGuid().ToString('N'))
    try
    {
        $installRoot = Join-Path $temporaryRoot 'install'
        $installerRoot = Join-Path $installRoot 'Installer'
        New-Item -ItemType Directory -Path $installerRoot -Force | Out-Null
        $missingStatePath = Join-Path $installerRoot 'PREPARE-STATE.json'
        $resultPath = Join-Path $temporaryRoot 'registration-result.json'
        $diagnosticPath = "$resultPath.diagnostic.txt"
        $registrationScript = Resolve-RepositoryPath `
            -RelativePath 'tools/installer/register-user-package.ps1'
        $windowsPowerShell = Get-Command powershell.exe -ErrorAction Stop | Select-Object -First 1

        $previousErrorActionPreference = $ErrorActionPreference
        try
        {
            $ErrorActionPreference = 'Continue'
            & $windowsPowerShell.Source `
                -NoLogo `
                -NoProfile `
                -ExecutionPolicy Bypass `
                -File $registrationScript `
                -InstallDirectory $installRoot `
                -MachineStatePath $missingStatePath `
                -ResultPath $resultPath 2> $null | Out-Null
            $exitCode = $LASTEXITCODE
        }
        finally
        {
            $ErrorActionPreference = $previousErrorActionPreference
        }

        Assert-True `
            -Condition ($exitCode -ne 0) `
            -Message 'Missing protected state unexpectedly succeeded.'
        Assert-True `
            -Condition (Test-Path -LiteralPath $resultPath -PathType Leaf) `
            -Message 'Early registration failure did not create a diagnostic result.'
        $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
        Assert-True `
            -Condition (-not [bool]$result.succeeded) `
            -Message 'Early registration failure diagnostic reported success.'
        Assert-True `
            -Condition (-not [string]::IsNullOrWhiteSpace([string]$result.error)) `
            -Message 'Early registration failure diagnostic omitted the error.'
        Assert-True `
            -Condition (Test-Path -LiteralPath $diagnosticPath -PathType Leaf) `
            -Message 'Early registration failure did not create a structured sidecar.'
        $diagnosticLines = @(Get-Content -LiteralPath $diagnosticPath)
        Assert-True `
            -Condition (
                $diagnosticLines.Count -eq 2 -and
                $diagnosticLines[0].StartsWith('BAFX_INSTALL_FAILURE:')) `
            -Message 'Registration diagnostic sidecar has an invalid summary contract.'
        $diagnosticPrefix = 'BAFX_INSTALL_DIAGNOSTIC_JSON: '
        Assert-True `
            -Condition $diagnosticLines[1].StartsWith($diagnosticPrefix) `
            -Message 'Registration diagnostic sidecar omitted structured JSON.'
        $diagnostic =
            $diagnosticLines[1].Substring($diagnosticPrefix.Length) | ConvertFrom-Json
        Assert-True `
            -Condition (
                [string]$diagnostic.phase -eq 'RegisterUserPackage' -and
                [string]$diagnostic.step -eq 'validate-protected-pending-state' -and
                [int]$diagnostic.scriptLine -gt 0) `
            -Message 'Registration diagnostic sidecar omitted the failing step.'
    }
    finally
    {
        $resolvedTemporaryRoot = [IO.Path]::GetFullPath($temporaryRoot)
        if ($resolvedTemporaryRoot.StartsWith(
                $temporaryParent,
                [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($resolvedTemporaryRoot).StartsWith(
                'bafx-registration-diagnostic-',
                [StringComparison]::Ordinal))
        {
            Remove-Item -LiteralPath $resolvedTemporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

function Test-PayloadRollbackManifestContract
{
    $ast = Get-ParsedScript `
        -RelativePath 'tools/installer/install-machine.ps1'
    $installMachine = Read-RepositoryText `
        -RelativePath 'tools/installer/install-machine.ps1'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'Save-DataDirectoryRollback[\s\S]*dataFiles\s*=\s*\$fileEntries\.ToArray\(\)' `
        -Description 'data rollback entries are materialized for PowerShell 5.1'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'New-PayloadRollbackManifest[\s\S]*files\s*=\s*\$entries\.ToArray\(\)' `
        -Description 'payload rollback entries are materialized for PowerShell 5.1'
    $protectedPathsAst = Get-ParsedScript `
        -RelativePath 'tools/installer/protected-paths.ps1'
    $functionNames = @(
        'Get-StatePropertiesWithoutDigest',
        'Convert-StateToCanonicalJson',
        'Get-StateDigest',
        'Assert-FileHash',
        'Normalize-PayloadRelativePath',
        'Get-PayloadFileSetLedger',
        'Normalize-PayloadFileSetLedger',
        'Assert-PayloadFileSetAgreement',
        'Assert-InstallStateRawPair',
        'Assert-InstallStatePair',
        'Resolve-InstallerRelativePath',
        'Resolve-RollbackManifestRelativePath',
        'Assert-RollbackManifestBackup',
        'Assert-PayloadRollbackManifest'
    )
    $functionText = @(
        "Set-StrictMode -Version Latest"
        "`$ErrorActionPreference = 'Stop'"
        'function Assert-ProtectedStateAcl { param([string]$Path) }'
        'function Ensure-RollbackEvidenceAcl { param([string]$RollbackRoot) }'
        (Get-FunctionText -Ast $protectedPathsAst -Name 'Assert-NoReparsePath')
        foreach ($name in $functionNames)
        {
            Get-FunctionText -Ast $ast -Name $name
        }
    ) -join "`n"
    $probeModule = New-Module -ScriptBlock ([scriptblock]::Create($functionText))
    $temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    $temporaryRoot = Join-Path `
        $temporaryParent `
        ('bafx-rollback-manifest-' + [Guid]::NewGuid().ToString('N'))
    try
    {
        $installRoot = Join-Path $temporaryRoot 'install'
        $transactionId = 'a' * 32
        $rollbackRoot = Join-Path $installRoot ('.rollback\' + $transactionId)
        $backupRoot = Join-Path $rollbackRoot 'files'
        New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
        $backupPath = Join-Path $backupRoot 'ba-click-fx-desktop.exe'
        [IO.File]::WriteAllText($backupPath, 'old host')
        $backupBytes = [Int64](Get-Item -LiteralPath $backupPath).Length
        $backupSha256 = (Get-FileHash -LiteralPath $backupPath -Algorithm SHA256).Hash
        $payloadManifestBackupRoot = Join-Path $rollbackRoot 'payload-manifest'
        New-Item -ItemType Directory -Path $payloadManifestBackupRoot -Force | Out-Null
        $payloadManifestBackupPath = Join-Path `
            $payloadManifestBackupRoot `
            'INSTALLER-PAYLOAD.json'
        [IO.File]::WriteAllText($payloadManifestBackupPath, 'old payload manifest')
        $payloadManifestBytes = [Int64](Get-Item -LiteralPath $payloadManifestBackupPath).Length
        $payloadManifestSha256 =
            (Get-FileHash -LiteralPath $payloadManifestBackupPath -Algorithm SHA256).Hash
        $entry = [ordered]@{
            path = 'ba-click-fx-desktop.exe'
            existed = $true
            bytes = $backupBytes
            sha256 = $backupSha256
            backupPath = 'files/ba-click-fx-desktop.exe'
        }
        $baseManifest = [ordered]@{
            schema = 1
            transactionId = $transactionId
            payloadFileSet = 'ba-click-fx-desktop.exe'
            files = @($entry)
            oldPackageFile = ''
            oldPackageBackupPath = ''
            oldPackageBytes = 0
            oldPackageSha256 = ''
            dataDirectoryExisted = $false
            dataDirectoryAcl = ''
            dataFiles = @(
                [ordered]@{
                    name = 'BAFX.config.json'
                    existed = $false
                    bytes = 0
                    sha256 = ''
                    backupPath = ''
                },
                [ordered]@{
                    name = 'ba-click-fx-desktop-support.log'
                    existed = $false
                    bytes = 0
                    sha256 = ''
                    backupPath = ''
                }
            )
            payloadManifestPath = 'Installer/INSTALLER-PAYLOAD.json'
            payloadManifestExisted = $true
            payloadManifestBytes = $payloadManifestBytes
            payloadManifestSha256 = $payloadManifestSha256
            payloadManifestBackupPath = 'payload-manifest/INSTALLER-PAYLOAD.json'
            previousStatePresent = $false
            previousStatePrimaryBackupPath = ''
            previousStateBackupBackupPath = ''
            previousStatePrimaryBytes = 0
            previousStatePrimarySha256 = ''
            previousStateBackupBytes = 0
            previousStateBackupSha256 = ''
        }
        $manifest = & $probeModule {
            param($Base)
            $value = [ordered]@{}
            foreach ($property in $Base.GetEnumerator())
            {
                $value[$property.Key] = $property.Value
            }
            $value.stateDigest = Get-StateDigest -Value $value
            return ($value | ConvertTo-Json -Depth 12 | ConvertFrom-Json)
        } $baseManifest
        $state = [pscustomobject]@{
            transactionId = $transactionId
            payloadFileSet = 'ba-click-fx-desktop.exe'
            oldInstallState = $null
        }
        $manifestPath = Join-Path $rollbackRoot 'ROLLBACK-MANIFEST.json'
        [IO.File]::WriteAllText(
            $manifestPath,
            ($manifest | ConvertTo-Json -Depth 12))
        $null = & $probeModule {
            param($State, $Root, $Manifest, $Path)
            Assert-PayloadRollbackManifest `
                -State $State `
                -InstallRoot $Root `
                -Manifest $Manifest `
                -ManifestPath $Path
        } $state $installRoot $manifest $manifestPath

        # Schema 1 transactions written before payload-manifest evidence was
        # introduced remain valid and must still be recoverable.
        $legacyManifest = $baseManifest | ConvertTo-Json -Depth 12 | ConvertFrom-Json
        foreach ($propertyName in @(
                'payloadManifestPath',
                'payloadManifestExisted',
                'payloadManifestBytes',
                'payloadManifestSha256',
                'payloadManifestBackupPath'))
        {
            $legacyManifest.PSObject.Properties.Remove($propertyName)
        }
        $null = & $probeModule {
            param($State, $Root, $Manifest, $Path)
            Assert-PayloadRollbackManifest `
                -State $State `
                -InstallRoot $Root `
                -Manifest $Manifest `
                -ManifestPath $Path
        } $state $installRoot $legacyManifest $manifestPath

        $missingEvidenceManifest = $baseManifest | ConvertTo-Json -Depth 12 |
            ConvertFrom-Json
        $missingEvidenceManifest.PSObject.Properties.Remove('payloadManifestSha256')
        Assert-Throws `
            -Action {
                & $probeModule {
                    param($State, $Root, $Manifest, $Path)
                    Assert-PayloadRollbackManifest `
                        -State $State `
                        -InstallRoot $Root `
                        -Manifest $Manifest `
                        -ManifestPath $Path
                } $state $installRoot $missingEvidenceManifest $manifestPath
            } `
            -Description 'rollback manifest with incomplete payload manifest evidence'

        $invalidPayloadManifestPath = $baseManifest | ConvertTo-Json -Depth 12 |
            ConvertFrom-Json
        $invalidPayloadManifestPath.payloadManifestPath = 'Installer/OTHER.json'
        Assert-Throws `
            -Action {
                & $probeModule {
                    param($State, $Root, $Manifest, $Path)
                    Assert-PayloadRollbackManifest `
                        -State $State `
                        -InstallRoot $Root `
                        -Manifest $Manifest `
                        -ManifestPath $Path
                } $state $installRoot $invalidPayloadManifestPath $manifestPath
            } `
            -Description 'payload manifest rollback path substitution'

        $invalidPayloadManifestEvidence = $baseManifest | ConvertTo-Json -Depth 12 |
            ConvertFrom-Json
        $invalidPayloadManifestEvidence.payloadManifestBytes++
        Assert-Throws `
            -Action {
                & $probeModule {
                    param($State, $Root, $Manifest, $Path)
                    Assert-PayloadRollbackManifest `
                        -State $State `
                        -InstallRoot $Root `
                        -Manifest $Manifest `
                        -ManifestPath $Path
                } $state $installRoot $invalidPayloadManifestEvidence $manifestPath
            } `
            -Description 'payload manifest rollback byte evidence tampering'

        $missingPayloadManifestEvidence = $baseManifest | ConvertTo-Json -Depth 12 |
            ConvertFrom-Json
        $missingPayloadManifestEvidence.payloadManifestExisted = $false
        $missingPayloadManifestEvidence.payloadManifestBytes = 1
        $missingPayloadManifestEvidence.payloadManifestSha256 = ''
        $missingPayloadManifestEvidence.payloadManifestBackupPath = ''
        Assert-Throws `
            -Action {
                & $probeModule {
                    param($State, $Root, $Manifest, $Path)
                    Assert-PayloadRollbackManifest `
                        -State $State `
                        -InstallRoot $Root `
                        -Manifest $Manifest `
                        -ManifestPath $Path
                } $state $installRoot $missingPayloadManifestEvidence $manifestPath
            } `
            -Description 'payload manifest evidence for a missing file'

        # Every path below is syntactically valid and the existing backup is
        # intact; only the protected file-set ledger exposes the missing entry.
        $missingEntryManifest = $baseManifest | ConvertTo-Json -Depth 12 |
            ConvertFrom-Json
        $missingEntryManifest.payloadFileSet =
            'ba-click-fx-desktop.exe|BAFX.ControlCenter.exe'
        $missingEntryState = [pscustomobject]@{
            transactionId = $transactionId
            payloadFileSet = 'ba-click-fx-desktop.exe|BAFX.ControlCenter.exe'
            oldInstallState = $null
        }
        Assert-Throws `
            -Action {
                & $probeModule {
                    param($State, $Root, $Manifest, $Path)
                    Assert-PayloadRollbackManifest `
                        -State $State `
                        -InstallRoot $Root `
                        -Manifest $Manifest `
                        -ManifestPath $Path
                } $missingEntryState $installRoot $missingEntryManifest $manifestPath
            } `
            -Description 'rollback manifest omits a valid payload file entry'

        [IO.File]::WriteAllText($backupPath, 'tampered')
        Assert-Throws `
            -Action {
                & $probeModule {
                    param($State, $Root, $Manifest, $Path)
                    Assert-PayloadRollbackManifest `
                        -State $State `
                        -InstallRoot $Root `
                        -Manifest $Manifest `
                        -ManifestPath $Path
                } $state $installRoot $manifest $manifestPath
            } `
            -Description 'rollback backup hash tampering'

        $duplicateManifest = $baseManifest | ConvertTo-Json -Depth 12 | ConvertFrom-Json
        $duplicateManifest.files = @($entry, $entry)
        Assert-Throws `
            -Action {
                & $probeModule {
                    param($State, $Root, $Manifest, $Path)
                    Assert-PayloadRollbackManifest `
                        -State $State `
                        -InstallRoot $Root `
                        -Manifest $Manifest `
                        -ManifestPath $Path
                } $state $installRoot $duplicateManifest $manifestPath
            } `
            -Description 'duplicate rollback manifest path'

        $unsafeManifest = $baseManifest | ConvertTo-Json -Depth 12 | ConvertFrom-Json
        $unsafeManifest.files[0].path = '..\ba-click-fx-desktop.exe'
        Assert-Throws `
            -Action {
                & $probeModule {
                    param($State, $Root, $Manifest, $Path)
                    Assert-PayloadRollbackManifest `
                        -State $State `
                        -InstallRoot $Root `
                        -Manifest $Manifest `
                        -ManifestPath $Path
                } $state $installRoot $unsafeManifest $manifestPath
            } `
            -Description 'unsafe rollback manifest path'
    }
    finally
    {
        if ($null -ne $probeModule)
        {
            Remove-Module -ModuleInfo $probeModule -Force -ErrorAction SilentlyContinue
        }
        $resolvedTemporaryRoot = [IO.Path]::GetFullPath($temporaryRoot)
        if ($resolvedTemporaryRoot.StartsWith(
                $temporaryParent,
                [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($resolvedTemporaryRoot).StartsWith(
                'bafx-rollback-manifest-',
                [StringComparison]::Ordinal))
        {
            Remove-Item -LiteralPath $resolvedTemporaryRoot -Recurse -Force `
                -ErrorAction SilentlyContinue
        }
    }
}

function Test-InstallerFailureDiagnostics
{
    $temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    $temporaryRoot = Join-Path `
        $temporaryParent `
        ('bafx-installer-diagnostic-' + [Guid]::NewGuid().ToString('N'))
    try
    {
        New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
        $diagnosticPath = Join-Path $temporaryRoot 'failure.txt'
        . (Resolve-RepositoryPath `
            -RelativePath 'tools/installer/installer-diagnostics.ps1')

        $failure = $null
        try
        {
            throw [IO.IOException]::new(
                "diagnostic`r`nprobe failure; step=forged")
        }
        catch
        {
            $failure = $_
        }
        $rollbackFailure = $null
        try
        {
            throw [UnauthorizedAccessException]::new('rollback probe failure')
        }
        catch
        {
            $rollbackFailure = $_
        }
        $relatedFailure = New-BafxInstallerRelatedFailure `
            -ErrorRecord $rollbackFailure `
            -Step 'rollback-diagnostic-probe'
        Write-BafxInstallerFailure `
            -ErrorRecord $failure `
            -Phase 'Prepare' `
            -Step 'diagnostic-probe' `
            -ProductVersion '0.2.4' `
            -PackageVersion '0.2.4.0' `
            -DiagnosticPath $diagnosticPath `
            -RelatedFailures @($relatedFailure) `
            -SuppressConsole

        $lines = @(Get-Content -LiteralPath $diagnosticPath)
        Assert-True `
            -Condition ($lines.Count -eq 2) `
            -Message 'Installer failure diagnostic must contain summary and JSON lines.'
        Assert-True `
            -Condition (
                $lines[0].StartsWith(
                    'BAFX_INSTALL_FAILURE: phase=Prepare; step=diagnostic-probe;') -and
                $lines[0].Contains('message=diagnostic probe failure%3B step%3Dforged;')) `
            -Message 'Installer failure summary is not safely single-line encoded.'
        $jsonPrefix = 'BAFX_INSTALL_DIAGNOSTIC_JSON: '
        Assert-True `
            -Condition $lines[1].StartsWith($jsonPrefix) `
            -Message 'Installer failure diagnostic omitted structured JSON.'
        $diagnostic = $lines[1].Substring($jsonPrefix.Length) | ConvertFrom-Json
        Assert-True `
            -Condition (
                [int]$diagnostic.schema -eq 1 -and
                [string]$diagnostic.event -eq 'BAFX.InstallerFailure' -and
                [string]$diagnostic.powerShellVersion -match '^\d+\.' -and
                [string]$diagnostic.processArchitecture -in @('x86', 'x64') -and
                [string]$diagnostic.phase -eq 'Prepare' -and
                [string]$diagnostic.step -eq 'diagnostic-probe' -and
                [string]$diagnostic.message -eq 'diagnostic probe failure; step=forged' -and
                [string]$diagnostic.exceptionType -eq 'System.IO.IOException' -and
                [string]$diagnostic.hresult -match '^0x[0-9A-F]{8}$' -and
                [int]$diagnostic.scriptLine -gt 0 -and
                @($diagnostic.relatedErrors).Count -eq 1 -and
                [string]$diagnostic.relatedErrors[0].step -eq
                    'rollback-diagnostic-probe' -and
                [string]$diagnostic.relatedErrors[0].exceptionType -eq
                    'System.UnauthorizedAccessException') `
            -Message 'Installer failure diagnostic fields are incomplete.'
    }
    finally
    {
        $resolvedTemporaryRoot = [IO.Path]::GetFullPath($temporaryRoot)
        if ($resolvedTemporaryRoot.StartsWith(
                $temporaryParent,
                [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($resolvedTemporaryRoot).StartsWith(
                'bafx-installer-diagnostic-',
                [StringComparison]::Ordinal))
        {
            Remove-Item `
                -LiteralPath $resolvedTemporaryRoot `
                -Recurse `
                -Force `
                -ErrorAction SilentlyContinue
        }
    }
}

$repositoryRootValue = $RepositoryRoot
if ([string]::IsNullOrWhiteSpace($repositoryRootValue))
{
    $repositoryRootValue = Join-Path $PSScriptRoot '..'
}
$repositoryRoot = [IO.Path]::GetFullPath($repositoryRootValue)
if (-not (Test-Path -LiteralPath $repositoryRoot -PathType Container))
{
    throw "Repository root does not exist: $repositoryRoot"
}

Test-PowerShellScriptContracts
Test-ProtectedAclIdentityResolutionContract
Test-AtomicFileReplacementContract
Test-UninstallerProcessPathFilter
Test-CertificateLifecycleBoundaryContract
Test-VersionMapping
Test-InstallerScriptWhitelist
Test-CompressionRuntimeColdStart
Test-InnoPayloadContract
Test-CrossVersionPendingRecoveryContract
Test-PendingRecoveryFailureContract
Test-SparsePackageContract
Test-UninstallerStatePairContract
Test-InstallStateWriteFaultInjectionContract
Test-UninstallerCompletionMarkerContract
Test-UninstallerOfflineHiveFailureContract
Test-UpgradeHostIntegrityContract
Test-PortableZipContract
Test-RegistrationFailureDiagnostics
Test-PayloadRollbackManifestContract
Test-InstallerFailureDiagnostics

Write-Host "User installer contracts verified (PowerShell $($PSVersionTable.PSVersion))."
