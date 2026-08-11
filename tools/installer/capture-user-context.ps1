[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

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

if ($PSVersionTable.PSEdition -ne 'Desktop')
{
    throw 'User registration requires Windows PowerShell 5.1.'
}

$outputFullPath = [IO.Path]::GetFullPath($OutputPath)
$outputDirectory = [IO.Path]::GetDirectoryName($outputFullPath)
if ([string]::IsNullOrWhiteSpace($outputDirectory))
{
    throw 'The user context output directory is invalid.'
}

New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
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
Write-Utf8NoBom -Path $outputFullPath -Content ($context | ConvertTo-Json -Depth 3)
