Set-StrictMode -Version Latest

function Get-ProtectedProgramFilesRoots
{
    $environmentCandidates = @(
        $env:ProgramW6432,
        $env:ProgramFiles,
        ${env:ProgramFiles(x86)}
    )
    $registryCandidates = New-Object Collections.Generic.List[string]

    # An elevated process can inherit stale environment variables after
    # Program Files was relocated. The registry is Windows' durable source for
    # both native and 32-bit protected roots.
    foreach ($registryPath in @(
            'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion',
            'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion'))
    {
        try
        {
            $registryValues = Get-ItemProperty `
                -LiteralPath $registryPath `
                -ErrorAction Stop
            foreach ($propertyName in @('ProgramFilesDir', 'ProgramFilesDir (x86)'))
            {
                $property = $registryValues.PSObject.Properties[$propertyName]
                if ($null -ne $property -and
                    -not [string]::IsNullOrWhiteSpace([string]$property.Value))
                {
                    $registryCandidates.Add([string]$property.Value)
                }
            }
        }
        catch
        {
            # A missing registry view is expected on some Windows editions.
        }
    }

    # Environment variables are inherited by the elevated process and are not
    # an authority boundary. Use them only when Windows exposes no registry
    # root at all, which preserves a bounded fallback for unusual editions.
    $candidates = if ($registryCandidates.Count -gt 0)
    {
        @($registryCandidates)
    }
    else
    {
        $environmentCandidates
    }

    $roots = New-Object Collections.Generic.List[string]
    foreach ($candidate in $candidates)
    {
        if ([string]::IsNullOrWhiteSpace([string]$candidate))
        {
            continue
        }
        try
        {
            $normalized = [IO.Path]::GetFullPath([string]$candidate).TrimEnd('\') + '\'
            if (-not ($roots | Where-Object { $_ -ieq $normalized }))
            {
                $roots.Add($normalized)
            }
        }
        catch
        {
            # Ignore malformed optional environment or registry values.
        }
    }
    return @($roots)
}

function Resolve-ProtectedProgramFilesPath
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    $resolved = [IO.Path]::GetFullPath($Path)
    $roots = @(Get-ProtectedProgramFilesRoots)
    foreach ($root in $roots)
    {
        if ($resolved.StartsWith($root, [StringComparison]::OrdinalIgnoreCase))
        {
            return $resolved
        }
    }

    $recognizedRoots = if ($roots.Count -gt 0) { $roots -join '; ' } else { '<none>' }
    throw "The $Description is outside Program Files: $resolved. Recognized protected roots: $recognizedRoots"
}
