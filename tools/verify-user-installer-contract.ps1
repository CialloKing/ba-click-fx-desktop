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
        'tools/installer/installer-diagnostics.ps1',
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
        'ChineseSimplified.isl',
        'installer-diagnostics.ps1',
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
            '\x27installer-diagnostics\.ps1\x27\s*,\s*' +
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
        -Pattern 'preexistingFullNames\s*=\s*@\(\$State\.preexistingPackageFullNames\)[\s\S]*sameVersionPackages' `
        -Description 'rollback ignores the preexisting same-version package'
    Assert-TextContains `
        -Text $installMachine `
        -Pattern 'Invoke-PendingRollback[\s\S]*preparedPackagePath[\s\S]*Remove-Item\s+-LiteralPath\s+\$preparedPackagePath' `
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
        -Pattern 'Add-InstallerRelatedFailure[\s\S]*rollback-failed-prepare[\s\S]*rollback-failed-finalize' `
        -Description 'machine rollback failures remain secondary to the root cause'
    Assert-TextExcludes `
        -Text $installMachine `
        -Pattern 'throw\s+\$(prepare|finalize)Error\b' `
        -Description 'lossy machine failure string rethrow'

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
        -Pattern 'ensure-host-process-stopped[\s\S]*remove-installed-user-package[\s\S]*remove-owned-certificates' `
        -Description 'machine uninstall reports stable resource cleanup steps'

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

function Test-InnoPayloadContract
{
    $inno = Read-RepositoryText -RelativePath 'tools/installer/ba-click-fx-desktop.iss'
    $chineseMessages = Read-RepositoryText `
        -RelativePath 'tools/installer/ChineseSimplified.isl'
    $packager = Read-RepositoryText -RelativePath 'tools/package-user-installer.ps1'
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
        -Pattern '(?m)^Name:\s*"\{autodesktop\}\\BAFX Control Center";\s*Filename:\s*"\{app\}\\BAFX\.ControlCenter\.exe";\s*WorkingDir:\s*"\{app\}"\s*$' `
        -Description 'Control Center desktop shortcut follows the installation scope'
    Assert-TextContains `
        -Text $inno `
        -Pattern 'ExecAsOriginalUser' `
        -Description 'registration is executed for the installing user'
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
    Assert-TextExcludes `
        -Text $uninstallCode.Value `
        -Pattern 'ExecAsOriginalUser|register-user-package\.ps1' `
        -Description 'original-user execution during uninstall'
    Assert-TextContains `
        -Text $uninstallCode.Value `
        -Pattern 'install-machine\.ps1[\s\S]*\-Phase Rollback[\s\S]*no committed state remains' `
        -Description 'pending uninstall uses elevated rollback and handles first-install state'
    Assert-TextContains `
        -Text $inno `
        -Pattern '\-Phase Prepare' `
        -Description 'machine preparation phase'
    Assert-TextContains `
        -Text $inno `
        -Pattern '\-Phase Finalize' `
        -Description 'machine finalization phase'

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
    $activation = Read-RepositoryText -RelativePath 'src/control-center/package_activation.cpp'
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
    Assert-TextContains `
        -Text $inno `
        -Pattern 'recovery state[\s\S]*were retained[\s\S]*Exit' `
        -Description 'failed setup retains recovery payload'

    $registration = Read-RepositoryText -RelativePath 'tools/installer/register-user-package.ps1'
    Assert-TextContains `
        -Text $registration `
        -Pattern 'transactionId[\s\S]*Remove-PreviousPackageForReplacement' `
        -Description 'registration transaction binding and same-version replacement'
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
        -Pattern 'foreach\s*\(\$installStatePath\s+in\s+@\(\$statePath,\s*"\$statePath\.bak"\)\)[\s\S]*Test-Path[\s\S]*Remove-Item' `
        -Description 'guarded primary and backup install-state cleanup'
}

function Test-UninstallerBackupFallback
{
    $ast = Get-ParsedScript `
        -RelativePath 'tools/installer/unregister-machine.ps1'
    $reader = Get-FunctionText -Ast $ast -Name 'Read-InstallStateWithBackup'
    $moduleText = @'
Set-StrictMode -Version Latest
function Assert-ProtectedStateAcl
{
    param([string]$Path)
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
        [IO.File]::WriteAllText($statePath, '{broken')
        $backupState = [ordered]@{
            schema = 1
            packageName = 'CialloKing.BaClickFxDesktop'
            applicationId = 'BaClickFxDesktop'
            publisher = 'CN=BaClickFx.Local'
            certificateThumbprint = '1111111111111111111111111111111111111111'
            packageFile = 'identity.msix'
        } | ConvertTo-Json -Compress
        [IO.File]::WriteAllText($backupPath, $backupState)

        $state = & $readerModule {
            param($Path, $InstallRoot)
            Read-InstallStateWithBackup -Path $Path -InstallRoot $InstallRoot
        } $statePath $temporaryRoot
        Assert-True `
            -Condition ([string]$state.packageName -eq 'CialloKing.BaClickFxDesktop') `
            -Message 'Uninstaller did not recover the valid backup state.'
        Assert-True `
            -Condition ((Get-Content -LiteralPath $statePath -Raw) -eq '{broken') `
            -Message 'Uninstaller rewrote the corrupt primary before full validation.'

        Remove-Item -LiteralPath $statePath -Force
        $backupOnlyState = & $readerModule {
            param($Path, $InstallRoot)
            Read-InstallStateWithBackup -Path $Path -InstallRoot $InstallRoot
        } $statePath $temporaryRoot
        Assert-True `
            -Condition ([string]$backupOnlyState.packageFile -eq 'identity.msix') `
            -Message 'Uninstaller did not accept a valid backup without a primary state.'
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
            -ProductVersion '0.1.0-alpha.14' `
            -PackageVersion '0.1.0.14' `
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
Test-VersionMapping
Test-InstallerScriptWhitelist
Test-InnoPayloadContract
Test-SparsePackageContract
Test-UninstallerBackupFallback
Test-PortableZipContract
Test-RegistrationFailureDiagnostics
Test-InstallerFailureDiagnostics

Write-Host "User installer contracts verified (PowerShell $($PSVersionTable.PSVersion))."
