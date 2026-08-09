[CmdletBinding()]
param(
    [string]$RuntimeProjectRoot,
    [string]$ExtractionProjectRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$manifestPath = Join-Path $repositoryRoot 'reference\unity-reference.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json

function Resolve-ReferenceRoot
{
    param(
        [string]$ExplicitValue,
        [string]$EnvironmentName,
        [string]$DefaultValue
    )

    $candidate = $ExplicitValue
    if ([string]::IsNullOrWhiteSpace($candidate))
    {
        $environmentValue = Get-Item -LiteralPath "Env:$EnvironmentName" -ErrorAction SilentlyContinue
        if ($null -ne $environmentValue)
        {
            $candidate = $environmentValue.Value
        }
    }

    if ([string]::IsNullOrWhiteSpace($candidate))
    {
        $candidate = $DefaultValue
    }

    if (-not (Test-Path -LiteralPath $candidate -PathType Container))
    {
        throw "Reference root does not exist: $candidate"
    }

    return (Resolve-Path -LiteralPath $candidate).Path
}

$runtimeRoot = Resolve-ReferenceRoot `
    -ExplicitValue $RuntimeProjectRoot `
    -EnvironmentName 'BAFX_UNITY_RUNTIME_ROOT' `
    -DefaultValue $manifest.defaults.runtimeRoot
$extractionRoot = Resolve-ReferenceRoot `
    -ExplicitValue $ExtractionProjectRoot `
    -EnvironmentName 'BAFX_UNITY_EXTRACTION_ROOT' `
    -DefaultValue $manifest.defaults.extractionRoot

$roots = @{
    runtime = $runtimeRoot
    extraction = $extractionRoot
}
$failures = [System.Collections.Generic.List[string]]::new()
$verifiedFiles = 0

foreach ($entry in $manifest.files)
{
    $root = $roots[$entry.root]
    $relativePath = $entry.path.Replace('/', [IO.Path]::DirectorySeparatorChar)
    $fullPath = Join-Path $root $relativePath
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf))
    {
        $failures.Add("Missing [$($entry.class)] $fullPath")
        continue
    }

    $item = Get-Item -LiteralPath $fullPath
    if ($item.Length -ne [long]$entry.bytes)
    {
        $failures.Add(
            "Length mismatch $fullPath expected=$($entry.bytes) actual=$($item.Length)")
        continue
    }

    $actualHash = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash
    if ($actualHash -ne $entry.sha256)
    {
        $failures.Add(
            "SHA-256 mismatch $fullPath expected=$($entry.sha256) actual=$actualHash")
        continue
    }

    $verifiedFiles++
}

foreach ($tree in $manifest.trees)
{
    $root = $roots[$tree.root]
    $relativePath = $tree.path.Replace('/', [IO.Path]::DirectorySeparatorChar)
    $fullPath = Join-Path $root $relativePath
    if (-not (Test-Path -LiteralPath $fullPath -PathType Container))
    {
        $failures.Add("Missing tree $fullPath")
        continue
    }

    $files = @(Get-ChildItem -LiteralPath $fullPath -File -Recurse)
    $totalBytes = ($files | Measure-Object -Property Length -Sum).Sum
    if ($files.Count -ne [int]$tree.fileCount -or $totalBytes -ne [long]$tree.totalBytes)
    {
        $failures.Add(
            "Tree mismatch $fullPath expected=$($tree.fileCount)/$($tree.totalBytes) " +
            "actual=$($files.Count)/$totalBytes")
    }
}

if ($failures.Count -gt 0)
{
    foreach ($failure in $failures)
    {
        Write-Error $failure
    }
    exit 1
}

Write-Host "Unity reference verified: $verifiedFiles files, $($manifest.trees.Count) trees"
Write-Host "Runtime root: $runtimeRoot"
Write-Host "Extraction root: $extractionRoot"

