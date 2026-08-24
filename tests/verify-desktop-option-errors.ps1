param(
    [Parameter(Mandatory = $true)]
    [string]$Executable
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-InvalidOptionCase
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string[]]$CaseArguments,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedDiagnostic,

        [Parameter(Mandatory = $true)]
        [string]$TemporaryRoot
    )

    $caseDirectory = Join-Path $TemporaryRoot $Name
    $caseExecutable = Join-Path $caseDirectory 'ba-click-fx-desktop.exe'
    $logPath = Join-Path $caseDirectory 'ba-click-fx-desktop-support.log'
    [void](New-Item -ItemType Directory -Path $caseDirectory)
    Copy-Item -LiteralPath $Executable -Destination $caseExecutable

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $caseExecutable
    $startInfo.WorkingDirectory = $caseDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.Arguments = $CaseArguments -join ' '

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try
    {
        if (-not $process.Start())
        {
            throw "Host did not start for case '$Name'"
        }
        if (-not $process.WaitForExit(5000))
        {
            $process.Kill()
            throw "Host did not reject case '$Name' within five seconds"
        }
        if ($process.ExitCode -ne 2)
        {
            throw "Case '$Name' returned $($process.ExitCode), expected 2"
        }
    }
    finally
    {
        $process.Dispose()
    }

    if (-not (Test-Path -LiteralPath $logPath -PathType Leaf))
    {
        throw "Case '$Name' did not create a diagnostic log"
    }
    $log = [System.IO.File]::ReadAllText($logPath)
    foreach ($requiredText in @(
        'Event.Name=Process.Startup.Failed',
        'Event.Level=Error',
        'Startup.Phase=command-line',
        "Error.Message=$ExpectedDiagnostic"))
    {
        if (-not $log.Contains($requiredText))
        {
            throw "Case '$Name' diagnostic log is missing '$requiredText'"
        }
    }
}

$temporaryRoot = Join-Path `
    ([System.IO.Path]::GetTempPath()) `
    ("bafx-host-option-errors-" + [guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $temporaryRoot)
try
{
    Invoke-InvalidOptionCase `
        -Name 'invalid-scenario' `
        -CaseArguments @('--demo-scenario=unknown') `
        -ExpectedDiagnostic '--demo-scenario requires center-click, interior-trail, or boundary-top-left' `
        -TemporaryRoot $temporaryRoot
    Invoke-InvalidOptionCase `
        -Name 'duplicate-scenario' `
        -CaseArguments @(
            '--demo-scenario=center-click',
            '--demo-scenario=boundary-top-left') `
        -ExpectedDiagnostic '--demo-scenario may be specified only once' `
        -TemporaryRoot $temporaryRoot
    Invoke-InvalidOptionCase `
        -Name 'short-interior-trail' `
        -CaseArguments @(
            '--demo-scenario=interior-trail',
            '--demo-age-ms=59') `
        -ExpectedDiagnostic '--demo-age-ms must be at least 60 for interior-trail' `
        -TemporaryRoot $temporaryRoot
}
finally
{
    Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
}

Write-Host 'Desktop option error process tests passed.'
