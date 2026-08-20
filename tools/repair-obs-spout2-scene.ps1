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
$senderName = 'ba-click-fx-desktop'
$spoutSources = @($document.sources) |
    Where-Object {
        if ($_.id -ne 'spout_capture')
        {
            return $false
        }
        $senderProperty = $_.settings.PSObject.Properties['spoutsenders']
        if ($null -eq $senderProperty)
        {
            $senderProperty = $_.settings.PSObject.Properties['spoutname']
        }
        return $null -ne $senderProperty -and
            [string]$senderProperty.Value -eq $senderName
    }
if ($spoutSources.Count -eq 0)
{
    throw "The scene does not contain a Spout2 source bound to '$senderName'."
}

$sceneSources = @($document.sources) |
    Where-Object { $_.id -eq 'scene' }
if ($sceneSources.Count -eq 0)
{
    throw 'The scene does not contain a scene source.'
}

$targetSourceUuids = @{}
foreach ($source in $spoutSources)
{
    $targetSourceUuids[[string]$source.uuid] = $true
}

$targetItems = [Collections.Generic.List[object]]::new()
foreach ($sceneSource in $sceneSources)
{
    # OBS requires a one-item collection to remain a JSON array after the
    # PowerShell object has passed through the pipeline.
    $sceneSource.settings.items = @($sceneSource.settings.items)
    foreach ($item in @($sceneSource.settings.items))
    {
        if ($targetSourceUuids.ContainsKey([string]$item.source_uuid))
        {
            $targetItems.Add($item)
        }
    }
}
if ($targetItems.Count -eq 0)
{
    throw "The '$senderName' Spout2 source is not present in any scene."
}

$needsRepair = $false
foreach ($source in $spoutSources)
{
    $compositeProperty = $source.settings.PSObject.Properties['compositemode']
    if ($null -eq $compositeProperty -or [int]$compositeProperty.Value -ne 4)
    {
        $needsRepair = $true
    }
}
foreach ($item in $targetItems)
{
    if ([int]$item.bounds_type -ne 2 -or
        [double]$item.bounds.x -ne [double]$Width -or
        [double]$item.bounds.y -ne [double]$Height)
    {
        $needsRepair = $true
    }
}

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

if (-not $PSCmdlet.ShouldProcess(
        $sceneFile,
        "repair '$senderName' bounds and premultiplied-alpha composite mode"))
{
    exit 0
}

$backup = "$sceneFile.bak"
Copy-Item -LiteralPath $sceneFile -Destination $backup -Force

# OBS accepts a zero-sized bounds rectangle, but then receives frames without
# rendering them. Restore only the selected sender to a stable canvas rectangle.
foreach ($item in $targetItems)
{
    $item.bounds_type = 2
    $item.bounds = [pscustomobject]@{
        x = [double]$Width
        y = [double]$Height
    }
    $item.bounds_rel = [pscustomobject]@{
        x = [double]$Width / 540.0 * 1.0
        y = [double]$Height / 540.0 * 1.0
    }
}
foreach ($source in $spoutSources)
{
    $compositeProperty = $source.settings.PSObject.Properties['compositemode']
    if ($null -eq $compositeProperty)
    {
        $source.settings | Add-Member -NotePropertyName compositemode -NotePropertyValue 4
    }
    else
    {
        $source.settings.compositemode = 4
    }
}

$document |
    ConvertTo-Json -Depth 100 |
    Set-Content -LiteralPath $sceneFile -Encoding utf8

Write-Output "Repaired OBS Spout2 premultiplied-alpha scene: $sceneFile"
Write-Output "Backup: $backup"
