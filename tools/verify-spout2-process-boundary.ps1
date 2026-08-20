[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$HostPath,

    [Parameter(Mandatory = $true)]
    [string]$ProbePath,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-True
{
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition)
    {
        throw $Message
    }
}

$resolvedHost = [IO.Path]::GetFullPath($HostPath)
$resolvedProbe = [IO.Path]::GetFullPath($ProbePath)
$resolvedOutput = [IO.Path]::GetFullPath($OutputDirectory)
Assert-True (Test-Path -LiteralPath $resolvedHost -PathType Leaf) `
    "Host executable was not found: $resolvedHost"
Assert-True (Test-Path -LiteralPath $resolvedProbe -PathType Leaf) `
    "Receiver probe was not found: $resolvedProbe"

$existingHosts = @(Get-Process -Name 'ba-click-fx-desktop' -ErrorAction SilentlyContinue)
Assert-True ($existingHosts.Count -eq 0) `
    'A ba-click-fx-desktop process is already running; the sender name would be ambiguous.'

[IO.Directory]::CreateDirectory($resolvedOutput) | Out-Null
$caseRoot = Join-Path $resolvedOutput (
    'spout2-process-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($caseRoot) | Out-Null
$isolatedHost = Join-Path $caseRoot 'ba-click-fx-desktop.exe'
$probeJson = Join-Path $caseRoot 'receiver.json'
$verificationJson = Join-Path $caseRoot 'verification.json'
$hostStdout = Join-Path $caseRoot 'host.stdout.log'
$hostStderr = Join-Path $caseRoot 'host.stderr.log'
Copy-Item -LiteralPath $resolvedHost -Destination $isolatedHost

$hostProcess = $null
try
{
    $hostArguments = @(
        '--spout2',
        '--demo-delay-ms=2500',
        '--disable-raw-input',
        '--quit-after-ms=7500'
    )
    $hostProcess = Start-Process `
        -FilePath $isolatedHost `
        -ArgumentList $hostArguments `
        -WorkingDirectory $caseRoot `
        -RedirectStandardOutput $hostStdout `
        -RedirectStandardError $hostStderr `
        -WindowStyle Hidden `
        -PassThru

    & $resolvedProbe `
        '--sender=ba-click-fx-desktop' `
        '--duration-ms=6500' `
        '--interval-ms=100' `
        "--output=$probeJson"
    Assert-True ($LASTEXITCODE -eq 0) `
        "Receiver probe failed with exit code $LASTEXITCODE."
    Assert-True ($hostProcess.WaitForExit(10000)) `
        'Host did not exit within its bounded lifetime.'
    Assert-True ($hostProcess.ExitCode -eq 0) `
        "Host failed with exit code $($hostProcess.ExitCode)."

    $probe = Get-Content -LiteralPath $probeJson -Raw | ConvertFrom-Json
    Assert-True ([int]$probe.schemaVersion -eq 1) 'Receiver probe schema is unsupported.'
    Assert-True ($probe.sender -eq 'ba-click-fx-desktop') 'Receiver connected to the wrong sender.'
    $samples = @($probe.samples)
    Assert-True ($samples.Count -ge 40) 'Receiver did not collect enough bounded samples.'

    $firstConnected = -1
    for ($index = 0; $index -lt $samples.Count; ++$index)
    {
        if ([bool]$samples[$index].connected)
        {
            $firstConnected = $index
            break
        }
    }
    Assert-True ($firstConnected -ge 0) 'Receiver never connected to the Host sender.'
    for ($index = $firstConnected; $index -lt $samples.Count; ++$index)
    {
        Assert-True ([bool]$samples[$index].connected) `
            'Sender disappeared before the bounded receiver probe completed.'
    }

    $connected = @($samples | Select-Object -Skip $firstConnected)
    $handles = @($connected | ForEach-Object { $_.handle } | Sort-Object -Unique)
    $sizes = @($connected | ForEach-Object { "$($_.width)x$($_.height)" } | Sort-Object -Unique)
    $formats = @($connected | ForEach-Object { [int]$_.format } | Sort-Object -Unique)
    Assert-True ($handles.Count -eq 1 -and $handles[0] -ne '0x0') `
        'Spout2 shared handle changed or was null during the fixed-size run.'
    Assert-True ($sizes.Count -eq 1) 'Spout2 dimensions changed during the fixed-size run.'
    Assert-True ($formats.Count -eq 1 -and $formats[0] -eq 87) `
        'Spout2 output was not DXGI_FORMAT_B8G8R8A8_UNORM.'
    Assert-True (@($connected | Where-Object {
        [UInt64]$_.premultipliedViolations -ne 0
    }).Count -eq 0) 'Spout2 output contains RGB values greater than Alpha.'

    $activeIndex = -1
    for ($index = 0; $index -lt $connected.Count; ++$index)
    {
        if ([UInt64]$connected[$index].nonzeroAlphaPixels -gt 0)
        {
            $activeIndex = $index
            break
        }
    }
    Assert-True ($activeIndex -gt 0) 'Receiver did not observe an effect after an idle frame.'
    Assert-True (@($connected | Select-Object -First $activeIndex | Where-Object {
        [UInt64]$_.nonzeroAlphaPixels -eq 0 -and
        [int]$_.maxRgb -eq 0 -and
        [int]$_.maxAlpha -eq 0
    }).Count -gt 0) 'Receiver did not observe a fully transparent initial frame.'
    Assert-True ([int]$connected[$activeIndex].maxAlpha -gt 0) `
        'Active Spout2 frame has no visible Alpha.'
    Assert-True (@($connected | Select-Object -Skip ($activeIndex + 1) | Where-Object {
        [UInt64]$_.nonzeroAlphaPixels -eq 0 -and
        [int]$_.maxRgb -eq 0 -and
        [int]$_.maxAlpha -eq 0
    }).Count -gt 0) 'Receiver did not return to transparency after the effect decayed.'

    $verification = [ordered]@{
        schemaVersion = 1
        status = 'passed'
        sender = 'ba-click-fx-desktop'
        outputContract = 'bgra8-srgb-premultiplied-fx-only-v1'
        samples = $samples.Count
        connectedSamples = $connected.Count
        size = $sizes[0]
        format = $formats[0]
        handle = $handles[0]
        firstActiveElapsedMs = [UInt64]$connected[$activeIndex].elapsedMs
        maximumActiveAlpha = [int](
            ($connected | Measure-Object -Property maxAlpha -Maximum).Maximum)
        evidenceRoot = $caseRoot
    }
    $verification |
        ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath $verificationJson -Encoding utf8
    Write-Output "Spout2 process boundary verified: $verificationJson"
}
finally
{
    if ($null -ne $hostProcess -and -not $hostProcess.HasExited)
    {
        # Only terminate the process created by this test; never touch an
        # unrelated Host that may belong to the user.
        $hostProcess.Kill($true)
        $hostProcess.WaitForExit()
    }
}
