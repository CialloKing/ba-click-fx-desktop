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
    $expectedCurrent = $currentMatch.Groups[1].Value -replace '-alpha\.', '.'
    Assert-True `
        -Condition ([string]$current.numericVersion -eq $expectedCurrent) `
        -Message "Current alpha version mapping is incorrect: $($currentMatch.Groups[1].Value)"
    Assert-True `
        -Condition ([string]$current.packageVersion -eq $expectedCurrent) `
        -Message "Current package version mapping is incorrect: $($currentMatch.Groups[1].Value)"

    $sample = Get-NumericVersions -Version '12.34.56-alpha.789'
    Assert-True `
        -Condition ([string]$sample.numericVersion -eq '12.34.56.789') `
        -Message 'alpha.N must map to the fourth Windows package version component.'
    Assert-True `
        -Condition ([string]$sample.packageVersion -eq '12.34.56.789') `
        -Message 'PackageVersion must match the numeric Windows version.'

    Assert-Throws `
        -Action { Get-NumericVersions -Version '12.34.56-beta.1' } `
        -Description 'non-alpha prerelease'
    Assert-Throws `
        -Action { Get-NumericVersions -Version '12.34.56-alpha.65536' } `
        -Description 'out-of-range alpha component'
}

function Test-PowerShellScriptContracts
{
    $scriptPaths = @(
        'tools/package-user-installer.ps1',
        'tools/installer/capture-user-context.ps1',
        'tools/installer/install-machine.ps1',
        'tools/installer/register-user-package.ps1',
        'tools/installer/unregister-machine.ps1',
        'tools/verify-user-installer-contract.ps1'
    )
    foreach ($scriptPath in $scriptPaths)
    {
        Get-ParsedScript -RelativePath $scriptPath | Out-Null
    }
}

function Test-InstallerScriptWhitelist
{
    $installerRoot = Resolve-RepositoryPath -RelativePath 'tools/installer'
    $expectedFiles = @(
        'ba-click-fx-desktop.iss',
        'capture-user-context.ps1',
        'install-machine.ps1',
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
            '\x27install-machine\.ps1\x27\s*,\s*' +
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
        -Pattern 'schema\s*=\s*1[\s\S]*files\s*=\s*\$payloadFiles' `
        -Description 'payload manifest schema and file hashes'
    Assert-TextContains `
        -Text $packager `
        -Pattern 'Assert-ExecutableVersion[\s\S]*-NumericVersion\s+\$numericVersion' `
        -Description 'Host and Control Center version-resource verification'

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

function Test-InnoPayloadContract
{
    $inno = Read-RepositoryText -RelativePath 'tools/installer/ba-click-fx-desktop.iss'
    $packager = Read-RepositoryText -RelativePath 'tools/package-user-installer.ps1'
    $identityBuilder = Read-RepositoryText -RelativePath 'tools/identity-package/build-identity-package.ps1'
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
        '{#StageRoot}\Identity\*',
        '{#StageRoot}\Installer\*'
    )
    Assert-ArrayEquals `
        -Expected $expectedSources `
        -Actual $sources `
        -Description 'Inno [Files] payload source whitelist'

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
        -Pattern 'ExecAsOriginalUser' `
        -Description 'registration is executed for the installing user'
    Assert-TextContains `
        -Text $inno `
        -Pattern '\-Phase Prepare' `
        -Description 'machine preparation phase'
    Assert-TextContains `
        -Text $inno `
        -Pattern '\-Phase Finalize' `
        -Description 'machine finalization phase'

    # SDK tools and the temporary signing key belong to the release machine,
    # never to the installer's target-machine payload.
    Assert-TextContains `
        -Text $identityBuilder `
        -Pattern '\-KeyExportPolicy\s+NonExportable' `
        -Description 'non-exportable release signing key'
    Assert-TextContains `
        -Text $identityBuilder `
        -Pattern 'Cert:\\CurrentUser\\My\\\$\(\$certificate\.Thumbprint\)' `
        -Description 'release signing certificate cleanup'
    Assert-TextExcludes `
        -Text $identityBuilder `
        -Pattern '(?i)Export-PfxCertificate' `
        -Description 'private key export'
    Assert-TextContains `
        -Text $packager `
        -Pattern "Filter\s+'\*\.msix'" `
        -Description 'signed sparse package selection'
    Assert-TextContains `
        -Text $packager `
        -Pattern "Filter\s+'\*\.cer'" `
        -Description 'public certificate selection'
    Assert-TextContains `
        -Text $packager `
        -Pattern "Filter\s+'\*\.identity\.json'" `
        -Description 'public identity metadata selection'
    Assert-TextContains `
        -Text $packager `
        -Pattern "Filter\s+'\*\.pfx'\s+-Recurse" `
        -Description 'private key staging guard'
    Assert-TextContains `
        -Text $packager `
        -Pattern 'Private signing certificate remains after packaging' `
        -Description 'certificate-store cleanup guard'
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

    $controlCenter = Read-RepositoryText -RelativePath 'src/control-center/control_center_window.cpp'
    $activation = Read-RepositoryText -RelativePath 'src/control-center/package_activation.cpp'
    $activationSources = $controlCenter + "`n" + $activation
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
    Assert-TextExcludes `
        -Text $activationSources `
        -Pattern 'ShellExecuteW\s*\(' `
        -Description 'bare ShellExecute Host launch'
    $inno = Read-RepositoryText -RelativePath 'tools/installer/ba-click-fx-desktop.iss'
    Assert-TextExcludes `
        -Text $inno `
        -Pattern 'DisableSystemBorder' `
        -Description 'installer preserves the default visible system border'
}

function Test-PortableZipContract
{
    $portableVerifier = Read-RepositoryText -RelativePath 'tools/verify-alpha-package.ps1'
    Assert-TextContains `
        -Text $portableVerifier `
        -Pattern 'locked three-file package contract' `
        -Description 'portable ZIP three-file contract'
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
        -Pattern 'verify_alpha_package' `
        -Description 'portable package verification target remains available'
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
Test-VersionMapping
Test-InstallerScriptWhitelist
Test-InnoPayloadContract
Test-SparsePackageContract
Test-PortableZipContract

Write-Host "User installer contracts verified (PowerShell $($PSVersionTable.PSVersion))."
