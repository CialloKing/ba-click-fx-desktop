Set-StrictMode -Version Latest

function Test-InstallerTrustedPrincipal
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Sid
    )

    # TrustedInstaller is the Windows servicing identity that owns the
    # inherited Program Files write rule. It is not an interactive user and
    # must remain an allowed writer for protected staging and rollback trees.
    return $Sid -in @(
        'S-1-5-18',
        'S-1-5-32-544',
        'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464'
    )
}

function Resolve-InstallerAclIdentity
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$Rule,

        [Parameter(Mandatory = $true)]
        [int]$WriteRights,

        [string]$Path = ''
    )

    $identityReference = $Rule.IdentityReference
    $identityText = ([string]$identityReference).Trim()
    try
    {
        # Prefer the security subsystem's translation so localized account
        # names and well-known aliases are handled by Windows itself.
        return [string]$identityReference.Translate(
            [Security.Principal.SecurityIdentifier]).Value
    }
    catch
    {
        # ACLs can retain a SID after its account/provider is unavailable. A
        # SID is still authoritative evidence, so validate and use it directly.
        if ($identityText -match '^(?i:S-\d-\d+(?:-\d+)+)$')
        {
            try
            {
                return [string]([Security.Principal.SecurityIdentifier]::new(
                        $identityText)).Value
            }
            catch
            {
                # Fall through to the same fail-closed handling as an unknown
                # account name if the textual SID is malformed.
            }
        }

        $hasWriteAccess = (([int]$Rule.FileSystemRights -band $WriteRights) -ne 0)
        if ($hasWriteAccess)
        {
            $location = if ([string]::IsNullOrWhiteSpace($Path))
            {
                '<unknown path>'
            }
            else
            {
                $Path
            }
            throw "Protected ACL contains an unresolvable identity with write access: $location ($identityText)"
        }

        # An unresolvable read-only application-package identity is harmless;
        # rejecting it would make localized Windows installations unusable.
        return $null
    }
}

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

function Assert-NoReparsePath
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [switch]$AllowMissing
    )

    $resolved = [IO.Path]::GetFullPath($Path)
    $pathRoot = [IO.Path]::GetPathRoot($resolved)
    if ([string]::IsNullOrWhiteSpace($pathRoot))
    {
        throw "Installer path has no filesystem root: $Path"
    }

    $current = $pathRoot
    $rootItem = Get-Item -LiteralPath $current -Force -ErrorAction Stop
    if (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
    {
        throw "Installer path contains a reparse point: $current"
    }
    $relative = $resolved.Substring($pathRoot.Length).Trim('\')
    if ([string]::IsNullOrWhiteSpace($relative))
    {
        return
    }

    foreach ($component in @($relative -split '\\'))
    {
        if ([string]::IsNullOrWhiteSpace($component))
        {
            continue
        }
        $current = Join-Path $current $component
        $item = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
        if ($null -eq $item)
        {
            if ($AllowMissing)
            {
                return
            }
            throw "Installer path component is missing: $current"
        }
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
        {
            throw "Installer path contains a reparse point: $current"
        }
    }
}

function Replace-InstallerFileAtomically
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourcePath,

        [Parameter(Mandatory = $true)]
        [string]$DestinationPath
    )

    $source = [IO.Path]::GetFullPath($SourcePath)
    $destination = [IO.Path]::GetFullPath($DestinationPath)
    Assert-NoReparsePath -Path $source
    Assert-NoReparsePath -Path $destination -AllowMissing
    $destinationParent = [IO.Path]::GetDirectoryName($destination)
    if (-not [string]::IsNullOrWhiteSpace($destinationParent))
    {
        Assert-NoReparsePath -Path $destinationParent -AllowMissing
    }

    # .NET Framework rejects a null backup argument for File.Replace. Supply a
    # unique same-directory backup so the replacement remains atomic, then
    # remove that transient copy before returning to the caller.
    $replacementBackup = "$destination.$PID.$([Guid]::NewGuid().ToString('N')).replace.bak"
    try
    {
        if (Test-Path -LiteralPath $destination -PathType Leaf)
        {
            Assert-NoReparsePath -Path $replacementBackup -AllowMissing
            [IO.File]::Replace($source, $destination, $replacementBackup, $true)
        }
        else
        {
            [IO.File]::Move($source, $destination)
        }
        Assert-NoReparsePath -Path $destination
    }
    finally
    {
        if (Test-Path -LiteralPath $replacementBackup -PathType Leaf)
        {
            Remove-Item -LiteralPath $replacementBackup -Force
            if (Test-Path -LiteralPath $replacementBackup -PathType Leaf)
            {
                throw "Atomic replacement backup remains: $replacementBackup"
            }
        }
    }
}

function Assert-NoReparseTree
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [switch]$AllowMissing
    )

    $resolved = [IO.Path]::GetFullPath($Path)
    Assert-NoReparsePath -Path $resolved -AllowMissing:$AllowMissing
    if (-not (Test-Path -LiteralPath $resolved -PathType Container))
    {
        if ($AllowMissing -and -not (Test-Path -LiteralPath $resolved))
        {
            return
        }
        throw "Installer tree is not an existing directory: $resolved"
    }

    $pending = New-Object Collections.Generic.Stack[string]
    $pending.Push($resolved)
    while ($pending.Count -gt 0)
    {
        $current = $pending.Pop()
        $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
        {
            throw "Installer tree contains a reparse point: $current"
        }

        if (-not $item.PSIsContainer)
        {
            continue
        }
        foreach ($child in @(Get-ChildItem -LiteralPath $current -Force -ErrorAction Stop))
        {
            $childPath = [IO.Path]::GetFullPath($child.FullName)
            if (($child.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
            {
                throw "Installer tree contains a reparse point: $childPath"
            }
            if ($child.PSIsContainer)
            {
                $pending.Push($childPath)
            }
        }
    }
}
