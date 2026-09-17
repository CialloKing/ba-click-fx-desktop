# Shared serialization and byte-pair primitives for install, registration and
# uninstall. Keep transaction/schema policy in each caller: recovery accepts
# legacy state that normal uninstall must still reject.
Set-StrictMode -Version Latest

function Get-StatePropertiesWithoutDigest
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$Value
    )

    $ordered = [ordered]@{}
    if ($Value -is [Collections.IDictionary])
    {
        foreach ($entry in $Value.GetEnumerator())
        {
            if ([string]$entry.Key -ne 'stateDigest')
            {
                $ordered[[string]$entry.Key] = $entry.Value
            }
        }
    }
    else
    {
        foreach ($property in $Value.PSObject.Properties)
        {
            if ($property.Name -ne 'stateDigest')
            {
                $ordered[$property.Name] = $property.Value
            }
        }
    }
    return $ordered
}

function Convert-StateToCanonicalJson
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$Value
    )

    return (Get-StatePropertiesWithoutDigest -Value $Value |
        ConvertTo-Json -Depth 12)
}

function Get-StateDigest
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$Value
    )

    $bytes = [Text.Encoding]::UTF8.GetBytes(
        (Convert-StateToCanonicalJson -Value $Value))
    $hasher = [Security.Cryptography.SHA256]::Create()
    try
    {
        return ([BitConverter]::ToString($hasher.ComputeHash($bytes))).Replace('-', '')
    }
    finally
    {
        $hasher.Dispose()
    }
}

function Assert-InstallStateRawPair
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$PrimaryPath,

        [Parameter(Mandatory = $true)]
        [string]$BackupPath
    )

    $primaryBytes = [IO.File]::ReadAllBytes($PrimaryPath)
    $backupBytes = [IO.File]::ReadAllBytes($BackupPath)
    if ($primaryBytes.Length -ne $backupBytes.Length)
    {
        throw 'Protected install state primary and backup bytes differ.'
    }
    for ($index = 0; $index -lt $primaryBytes.Length; ++$index)
    {
        if ($primaryBytes[$index] -ne $backupBytes[$index])
        {
            throw 'Protected install state primary and backup bytes differ.'
        }
    }
}

function Split-Ledger
{
    param(
        [AllowNull()]
        [object]$Value,

        [Parameter(Mandatory = $true)]
        [ValidateSet('Comma', 'Pipe')]
        [string]$Separator
    )

    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace([string]$Value))
    {
        return @()
    }
    # One released PowerShell 5.1 path joined package names with a space.
    # Package artifacts never contain whitespace, so accept that legacy form
    # while preserving the normal pipe-delimited representation on rewrite.
    $pattern = if ($Separator -eq 'Comma') { ',' } else { '[|\s]+' }
    return @(
        ([string]$Value -split $pattern) |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            ForEach-Object { $_.Trim() }
    )
}
