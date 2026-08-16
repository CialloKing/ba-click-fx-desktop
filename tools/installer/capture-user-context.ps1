[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installer-diagnostics.ps1')
$script:InstallerStep = 'initialize'
$script:InstallerDiagnosticPath = "$OutputPath.diagnostic.txt"

trap
{
    Write-BafxInstallerFailure `
        -ErrorRecord $_ `
        -Phase 'CaptureUserContext' `
        -Step $script:InstallerStep `
        -DiagnosticPath $script:InstallerDiagnosticPath
    exit 1
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

$script:InstallerStep = 'validate-powershell'
if ($PSVersionTable.PSEdition -ne 'Desktop')
{
    throw 'User registration requires Windows PowerShell 5.1.'
}

$script:InstallerStep = 'resolve-output-path'
$outputFullPath = [IO.Path]::GetFullPath($OutputPath)
$script:InstallerDiagnosticPath = "$outputFullPath.diagnostic.txt"
$outputDirectory = [IO.Path]::GetDirectoryName($outputFullPath)
if ([string]::IsNullOrWhiteSpace($outputDirectory))
{
    throw 'The user context output directory is invalid.'
}

$script:InstallerStep = 'create-output-directory'
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$script:InstallerStep = 'read-original-user-identity'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if ($null -eq $identity.User)
{
    throw 'The original user SID is unavailable.'
}

$context = [ordered]@{
    schema = 1
    userSid = $identity.User.Value
    userName = $identity.Name
    capturedUtc = [DateTime]::UtcNow.ToString('o')
}
$script:InstallerStep = 'write-original-user-context'
Write-Utf8NoBom -Path $outputFullPath -Content ($context | ConvertTo-Json -Depth 3)
