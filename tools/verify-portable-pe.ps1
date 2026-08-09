param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [Parameter(Mandatory = $true)]
    [string]$Linker
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Executable -PathType Leaf))
{
    throw "Desktop executable not found: $Executable"
}

if (-not (Test-Path -LiteralPath $Linker -PathType Leaf))
{
    throw "MSVC linker not found: $Linker"
}

$dumpbin = Join-Path (Split-Path -Parent $Linker) 'dumpbin.exe'
if (-not (Test-Path -LiteralPath $dumpbin -PathType Leaf))
{
    throw "MSVC dumpbin not found beside the linker: $dumpbin"
}

$output = & $dumpbin /nologo /dependents $Executable 2>&1
if ($LASTEXITCODE -ne 0)
{
    throw "dumpbin.exe failed to inspect the desktop executable:`n$($output -join "`n")"
}

$dependencies = @(
    $output |
        ForEach-Object { $_.ToString().Trim() } |
        Where-Object { $_ -match '^[A-Za-z0-9_.-]+\.dll$' } |
        Sort-Object -Unique
)

$forbidden = @(
    $dependencies |
        Where-Object {
            $_ -match '^(?i:msvcp\d+|vcruntime\d+(?:_\d+)?|ucrtbased)\.dll$'
        }
)

if ($forbidden.Count -ne 0)
{
    throw "Visual C++ runtime DLL dependencies found: $($forbidden -join ', ')"
}

Write-Host "Portable PE dependency check passed ($($dependencies.Count) imports)."
Write-Host ($dependencies -join ', ')
