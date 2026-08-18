[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [string]$OutputDirectory,

    [string]$Revision,

    [string]$Python = 'python',

    [ValidateRange(1000, 60000)]
    [int]$CaptureTimeoutMilliseconds = 15000,

    [ValidateRange(1000, 120000)]
    [int]$ProcessTimeoutMilliseconds = 30000,

    [ValidateRange(1000, 60000)]
    [int]$VerifierTimeoutMilliseconds = 10000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

function Resolve-RepositoryPath
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if ([IO.Path]::IsPathRooted($Path))
    {
        return [IO.Path]::GetFullPath($Path)
    }
    return [IO.Path]::GetFullPath((Join-Path $repositoryRoot $Path))
}

function Get-Revision
{
    if (-not [string]::IsNullOrWhiteSpace($Revision))
    {
        return $Revision.Trim()
    }

    $resolvedRevision = & git -C $repositoryRoot rev-parse --short HEAD
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($resolvedRevision))
    {
        throw 'Unable to resolve the current git revision; pass -Revision explicitly.'
    }
    return $resolvedRevision.Trim()
}

function Invoke-BoundedProcess
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(Mandatory = $true)]
        [string[]]$ArgumentList,

        [Parameter(Mandatory = $true)]
        [string]$WorkingDirectory,

        [Parameter(Mandatory = $true)]
        [int]$TimeoutMilliseconds,

        [Parameter(Mandatory = $true)]
        [string]$StandardOutputPath,

        [Parameter(Mandatory = $true)]
        [string]$StandardErrorPath
    )

    $process = $null
    $stdoutTask = $null
    $stderrTask = $null
    $stdoutWritten = $false
    $stderrWritten = $false
    try
    {
        $startInfo = New-Object System.Diagnostics.ProcessStartInfo
        $startInfo.FileName = $FilePath
        $startInfo.WorkingDirectory = $WorkingDirectory
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $quotedArguments = @(
            $ArgumentList | ForEach-Object { '"' + $_ + '"' }
        )
        $startInfo.Arguments = $quotedArguments -join ' '

        $process = New-Object System.Diagnostics.Process
        $process.StartInfo = $startInfo
        if (-not $process.Start())
        {
            throw "Unable to start process: $FilePath"
        }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()

        if (-not $process.WaitForExit($TimeoutMilliseconds))
        {
            try
            {
                $process.Kill()
            }
            catch
            {
                # Preserve the timeout evidence even if the process exits during Kill.
            }
            $null = $process.WaitForExit(5000)
            throw "$FilePath exceeded the $TimeoutMilliseconds ms process timeout."
        }

        $null = $process.WaitForExit()
        $stdoutText = $stdoutTask.GetAwaiter().GetResult()
        $stderrText = $stderrTask.GetAwaiter().GetResult()
        [IO.File]::WriteAllText(
            $StandardOutputPath,
            $stdoutText,
            (New-Object System.Text.UTF8Encoding($false)))
        $stdoutWritten = $true
        [IO.File]::WriteAllText(
            $StandardErrorPath,
            $stderrText,
            (New-Object System.Text.UTF8Encoding($false)))
        $stderrWritten = $true
        return [int]$process.ExitCode
    }
    finally
    {
        if ($null -ne $process -and -not $process.HasExited)
        {
            try
            {
                $process.Kill()
                $null = $process.WaitForExit(5000)
            }
            catch
            {
                # The caller still receives the original process failure or timeout.
            }
        }
        if ($null -ne $stdoutTask -and -not $stdoutWritten -and $stdoutTask.IsCompleted)
        {
            try
            {
                [IO.File]::WriteAllText(
                    $StandardOutputPath,
                    $stdoutTask.GetAwaiter().GetResult(),
                    (New-Object System.Text.UTF8Encoding($false)))
            }
            catch
            {
                # Keep the original process error as the useful failure reason.
            }
        }
        if ($null -ne $stderrTask -and -not $stderrWritten -and $stderrTask.IsCompleted)
        {
            try
            {
                [IO.File]::WriteAllText(
                    $StandardErrorPath,
                    $stderrTask.GetAwaiter().GetResult(),
                    (New-Object System.Text.UTF8Encoding($false)))
            }
            catch
            {
                # Keep the original process error as the useful failure reason.
            }
        }
        if ($null -ne $process)
        {
            $process.Dispose()
        }
    }
}

$revisionValue = Get-Revision
$resolvedExecutable = Resolve-RepositoryPath -Path $Executable
$verifierPath = Join-Path $repositoryRoot 'tools\verify-wgc-session-exclusion-spike.py'
$minimumProcessTimeoutMilliseconds = $CaptureTimeoutMilliseconds + 5000

if ($ProcessTimeoutMilliseconds -lt $minimumProcessTimeoutMilliseconds)
{
    throw "Process timeout must be at least capture timeout plus 5000 ms so the collector watchdog can write failure evidence."
}

if (-not (Test-Path -LiteralPath $resolvedExecutable -PathType Leaf))
{
    throw "Spike executable does not exist: $resolvedExecutable"
}
if (-not (Test-Path -LiteralPath $verifierPath -PathType Leaf))
{
    throw "Session exclusion verifier does not exist: $verifierPath"
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory))
{
    $machineName = [Environment]::MachineName
    $OutputDirectory = Join-Path `
        $repositoryRoot `
        "artifacts\local\spikes\spk-002-session-exclusion\$machineName-$revisionValue"
}
else
{
    $OutputDirectory = Resolve-RepositoryPath -Path $OutputDirectory
}

$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory -PathType Leaf)
{
    throw "Session exclusion output path is a file: $OutputDirectory"
}
if (Test-Path -LiteralPath $OutputDirectory -PathType Container)
{
    $existingEntry = Get-ChildItem -LiteralPath $OutputDirectory -Force |
        Select-Object -First 1
    if ($null -ne $existingEntry)
    {
        throw "Session exclusion output directory is not empty; choose a new directory to preserve prior evidence: $OutputDirectory"
    }
}
$null = New-Item -ItemType Directory -Force -Path $OutputDirectory

$captureJsonPath = Join-Path $OutputDirectory 'session-exclusion.json'
$verificationJsonPath = Join-Path $OutputDirectory 'verification.json'
$collectorStdoutPath = Join-Path $OutputDirectory 'collector.stdout.log'
$collectorStderrPath = Join-Path $OutputDirectory 'collector.stderr.log'
$verifierStdoutPath = Join-Path $OutputDirectory 'verifier.stdout.log'
$verifierStderrPath = Join-Path $OutputDirectory 'verifier.stderr.log'

Write-Host "Session exclusion output: $OutputDirectory"
Write-Host "Revision: $revisionValue"

$collectorExitCode = Invoke-BoundedProcess `
    -FilePath $resolvedExecutable `
    -ArgumentList @(
        "--output=$OutputDirectory",
        "--revision=$revisionValue",
        "--timeout-ms=$CaptureTimeoutMilliseconds") `
    -WorkingDirectory $repositoryRoot `
    -TimeoutMilliseconds $ProcessTimeoutMilliseconds `
    -StandardOutputPath $collectorStdoutPath `
    -StandardErrorPath $collectorStderrPath

if ($collectorExitCode -ne 0)
{
    throw "Session exclusion collector exited with code $collectorExitCode. Raw logs remain in $OutputDirectory"
}
if (-not (Test-Path -LiteralPath $captureJsonPath -PathType Leaf))
{
    throw "Collector exited successfully without writing $captureJsonPath"
}

$verifierExitCode = Invoke-BoundedProcess `
    -FilePath $Python `
    -ArgumentList @(
        '-B',
        $verifierPath,
        $captureJsonPath,
        "--report=$verificationJsonPath") `
    -WorkingDirectory $repositoryRoot `
    -TimeoutMilliseconds $VerifierTimeoutMilliseconds `
    -StandardOutputPath $verifierStdoutPath `
    -StandardErrorPath $verifierStderrPath

if ($verifierExitCode -ne 0)
{
    throw "Session exclusion verifier exited with code $verifierExitCode. Raw logs remain in $OutputDirectory"
}
if (-not (Test-Path -LiteralPath $verificationJsonPath -PathType Leaf))
{
    throw "Verifier exited successfully without writing $verificationJsonPath"
}

Write-Host 'Session exclusion collector and verifier completed.'
Write-Host "Verification: $verificationJsonPath"
