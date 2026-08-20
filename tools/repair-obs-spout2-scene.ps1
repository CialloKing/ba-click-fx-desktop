[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)]
    [string]$ScenePath,

    [int]$Width = 1920,

    [int]$Height = 1080,

    [switch]$CheckOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($Width -le 0 -or $Height -le 0)
{
    throw 'Width and Height must be positive.'
}

$sceneFile = [IO.Path]::GetFullPath($ScenePath)
if (-not (Test-Path -LiteralPath $sceneFile -PathType Leaf))
{
    throw "OBS scene file was not found: $sceneFile"
}

$document = Get-Content -LiteralPath $sceneFile -Raw | ConvertFrom-Json
$spoutSource = @($document.sources) |
    Where-Object { $_.id -eq 'spout_capture' } |
    Select-Object -First 1
if ($null -eq $spoutSource)
{
    throw 'The scene does not contain a Spout2 capture source.'
}

$sceneSource = @($document.sources) |
    Where-Object { $_.id -eq 'scene' } |
    Select-Object -First 1
if ($null -eq $sceneSource)
{
    throw 'The scene does not contain a scene source.'
}

# ConvertFrom-Json returns a PSCustomObject when a JSON array contains one
# item. OBS requires the scene item collection to remain an array, otherwise
# it silently loads the scene without any visible sources.
$sceneSource.settings.items = @($sceneSource.settings.items)

$item = @($sceneSource.settings.items) |
    Where-Object { $_.source_uuid -eq $spoutSource.uuid } |
    Select-Object -First 1
if ($null -eq $item)
{
    throw 'The Spout2 source is not present in the active scene.'
}

$needsRepair = [int]$item.bounds_type -ne 2 -or
    [double]$item.bounds.x -le 0 -or
    [double]$item.bounds.y -le 0 -or
    [int]$spoutSource.settings.compositemode -ne 1

if (-not $needsRepair)
{
    Write-Output "OBS Spout2 scene is valid: $sceneFile"
    exit 0
}

if ($CheckOnly)
{
    Write-Output "OBS Spout2 scene needs repair: $sceneFile"
    exit 2
}

if (-not $PSCmdlet.ShouldProcess($sceneFile, 'repair Spout2 source bounds and composite mode'))
{
    exit 0
}

$backup = "$sceneFile.bak"
Copy-Item -LiteralPath $sceneFile -Destination $backup -Force

# OBS treats a zero-sized bounds rectangle as a valid saved transform, so the
# source can receive frames while the scene renders nothing. Use a stable
# canvas-sized rectangle and opaque compositing for the Host's BGRA8 payload.
$item.bounds_type = 2
$item.bounds = [pscustomobject]@{
    x = [double]$Width
    y = [double]$Height
}
$item.bounds_rel = [pscustomobject]@{
    x = [double]$Width / 540.0 * 1.0
    y = [double]$Height / 540.0 * 1.0
}
$spoutSource.settings.compositemode = 1

$document |
    ConvertTo-Json -Depth 100 |
    Set-Content -LiteralPath $sceneFile -Encoding utf8

Write-Output "Repaired OBS Spout2 scene: $sceneFile"
Write-Output "Backup: $backup"
