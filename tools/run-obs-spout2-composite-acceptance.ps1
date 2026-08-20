[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$HostPath,

    [Parameter(Mandatory = $true)]
    [string]$ProbePath,

    [Parameter(Mandatory = $true)]
    [string]$ObsPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$senderName = 'ba-click-fx-desktop'
$outputContract = 'bgra8-srgb-extended-premultiplied-fx-only-v2'
$frameFormat = 87
$requestTimeoutMilliseconds = 10000
$utf8NoBom = [Text.UTF8Encoding]::new($false)

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

function Write-JsonFile
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [object]$Value
    )

    $json = $Value | ConvertTo-Json -Depth 64
    [IO.File]::WriteAllText($Path, $json + [Environment]::NewLine, $utf8NoBom)
}

function Read-JsonFile
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    # Windows PowerShell 5 treats BOM-less UTF-8 as the active ANSI code page.
    # OBS writes BOM-less UTF-8 and may use localized profile or scene names.
    return [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8) | ConvertFrom-Json
}

function Get-RelativeChildPath
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $fullPath = [IO.Path]::GetFullPath($Path)
    $prefix = $rootPath + [IO.Path]::DirectorySeparatorChar
    Assert-True (
        $fullPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) `
        "Path is outside the expected root: $fullPath"
    return $fullPath.Substring($prefix.Length).Replace('\', '/')
}

function Get-PathFromRelative
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,

        [Parameter(Mandatory = $true)]
        [string]$RelativePath
    )

    return Join-Path $Root $RelativePath.Replace('/', [IO.Path]::DirectorySeparatorChar)
}

function Resolve-Executable
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    $resolved = [IO.Path]::GetFullPath($Path)
    Assert-True (Test-Path -LiteralPath $resolved -PathType Leaf) `
        "$Label was not found: $resolved"
    return $resolved
}

function Wait-ForStableFile
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [int]$TimeoutMilliseconds = 15000
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
    $lastLength = [UInt64]0
    $lastWriteTicks = [Int64]0
    $stableSamples = 0
    while ([DateTime]::UtcNow -lt $deadline)
    {
        if (Test-Path -LiteralPath $Path -PathType Leaf)
        {
            $item = Get-Item -LiteralPath $Path
            $length = [UInt64]$item.Length
            $writeTicks = [Int64]$item.LastWriteTimeUtc.Ticks
            if ($length -gt 0 -and
                $length -eq $lastLength -and
                $writeTicks -eq $lastWriteTicks)
            {
                ++$stableSamples
                if ($stableSamples -ge 5)
                {
                    return
                }
            }
            else
            {
                $stableSamples = 0
                $lastLength = $length
                $lastWriteTicks = $writeTicks
            }
        }
        Start-Sleep -Milliseconds 200
    }
    throw "File did not become non-empty and stable: $Path"
}

function Resolve-ObsInstallRoot
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExecutablePath
    )

    $candidate = Split-Path -Parent $ExecutablePath
    for ($depth = 0; $depth -lt 5; ++$depth)
    {
        if ((Test-Path -LiteralPath (Join-Path $candidate 'obs-plugins') -PathType Container) -and
            (Test-Path -LiteralPath (Join-Path $candidate 'data') -PathType Container))
        {
            return $candidate
        }
        $parent = Split-Path -Parent $candidate
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $candidate)
        {
            break
        }
        $candidate = $parent
    }
    throw "Could not locate the OBS installation root from: $ExecutablePath"
}

function Resolve-ObsConfigRoot
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstallRoot
    )

    $portable =
        (Test-Path -LiteralPath (Join-Path $InstallRoot 'portable_mode') -PathType Leaf) -or
        (Test-Path -LiteralPath (Join-Path $InstallRoot 'portable_mode.txt') -PathType Leaf)
    if ($portable)
    {
        return [pscustomobject]@{
            Root = Join-Path $InstallRoot 'config\obs-studio'
            Portable = $true
        }
    }
    Assert-True (-not [string]::IsNullOrWhiteSpace($env:APPDATA)) `
        'APPDATA is unavailable, so the OBS configuration root cannot be located.'
    return [pscustomobject]@{
        Root = Join-Path $env:APPDATA 'obs-studio'
        Portable = $false
    }
}

function Get-IniValue
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Section,

        [Parameter(Mandatory = $true)]
        [string]$Key
    )

    $currentSection = ''
    foreach ($line in @(Get-Content -LiteralPath $Path))
    {
        $trimmed = $line.Trim()
        if ($trimmed -match '^\[(?<section>[^\]]+)\]$')
        {
            $currentSection = [string]$Matches.section
            continue
        }
        if (-not $currentSection.Equals($Section, [StringComparison]::OrdinalIgnoreCase) -or
            $trimmed.StartsWith(';') -or $trimmed.StartsWith('#'))
        {
            continue
        }
        $separator = $trimmed.IndexOf('=')
        if ($separator -le 0)
        {
            continue
        }
        $candidateKey = $trimmed.Substring(0, $separator).Trim()
        if ($candidateKey.Equals($Key, [StringComparison]::OrdinalIgnoreCase))
        {
            return $trimmed.Substring($separator + 1).Trim()
        }
    }
    throw "OBS INI value was not found: [$Section] $Key"
}

function Get-ManagedState
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$ConfigRoot,

        [Parameter(Mandatory = $true)]
        [string[]]$ManagedDirectories,

        [Parameter(Mandatory = $true)]
        [string[]]$ManagedFiles
    )

    $files = [Collections.Generic.List[object]]::new()
    $directories = [Collections.Generic.List[string]]::new()
    foreach ($directory in $ManagedDirectories)
    {
        if (-not (Test-Path -LiteralPath $directory -PathType Container))
        {
            continue
        }
        $directories.Add((Get-RelativeChildPath -Root $ConfigRoot -Path $directory))
        foreach ($child in @(Get-ChildItem -LiteralPath $directory -Directory -Force -Recurse))
        {
            $directories.Add((Get-RelativeChildPath -Root $ConfigRoot -Path $child.FullName))
        }
        foreach ($child in @(Get-ChildItem -LiteralPath $directory -File -Force -Recurse))
        {
            $files.Add([pscustomobject]@{
                RelativePath = Get-RelativeChildPath -Root $ConfigRoot -Path $child.FullName
                Length = [UInt64]$child.Length
                Sha256 = (Get-FileHash -LiteralPath $child.FullName -Algorithm SHA256).Hash
            })
        }
    }
    foreach ($file in $ManagedFiles)
    {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf))
        {
            continue
        }
        $item = Get-Item -LiteralPath $file
        $files.Add([pscustomobject]@{
            RelativePath = Get-RelativeChildPath -Root $ConfigRoot -Path $item.FullName
            Length = [UInt64]$item.Length
            Sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash
        })
    }
    return [pscustomobject]@{
        Files = @($files | Sort-Object -Property RelativePath -Unique)
        Directories = @($directories | Sort-Object -Unique)
    }
}

function ConvertTo-EvidenceState
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$ConfigRoot,

        [Parameter(Mandatory = $true)]
        [object]$State
    )

    return [ordered]@{
        schemaVersion = 1
        configRoot = $ConfigRoot
        scope = 'OBS profiles, scene collections, global/user INI, and obs-websocket config'
        files = @($State.Files | ForEach-Object {
            [ordered]@{
                path = $_.RelativePath
                length = $_.Length
                sha256 = $_.Sha256
            }
        })
        directories = @($State.Directories)
    }
}

function Backup-ManagedState
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$ConfigRoot,

        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$BackupRoot
    )

    [IO.Directory]::CreateDirectory($BackupRoot) | Out-Null
    foreach ($file in @($State.Files))
    {
        $source = Get-PathFromRelative -Root $ConfigRoot -RelativePath $file.RelativePath
        $destination = Get-PathFromRelative -Root $BackupRoot -RelativePath $file.RelativePath
        [IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Force
        Assert-True (
            (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -eq $file.Sha256) `
            "Configuration backup hash mismatch: $($file.RelativePath)"
    }
}

function Test-IsManagedPath
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string[]]$ManagedDirectories,

        [Parameter(Mandatory = $true)]
        [string[]]$ManagedFiles
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    foreach ($file in $ManagedFiles)
    {
        if ($fullPath.Equals([IO.Path]::GetFullPath($file), [StringComparison]::OrdinalIgnoreCase))
        {
            return $true
        }
    }
    foreach ($directory in $ManagedDirectories)
    {
        $root = [IO.Path]::GetFullPath($directory).TrimEnd('\', '/')
        if ($fullPath.Equals($root, [StringComparison]::OrdinalIgnoreCase) -or
            $fullPath.StartsWith(
                $root + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase))
        {
            return $true
        }
    }
    return $false
}

function Assert-StatesEqual
{
    param(
        [Parameter(Mandatory = $true)]
        [object]$Expected,

        [Parameter(Mandatory = $true)]
        [object]$Actual
    )

    $expectedFiles = @{}
    foreach ($file in @($Expected.Files))
    {
        $expectedFiles[[string]$file.RelativePath] = $file
    }
    $actualFiles = @{}
    foreach ($file in @($Actual.Files))
    {
        $actualFiles[[string]$file.RelativePath] = $file
    }
    Assert-True ($expectedFiles.Count -eq $actualFiles.Count) `
        'OBS configuration file set was not restored exactly.'
    foreach ($path in $expectedFiles.Keys)
    {
        Assert-True ($actualFiles.ContainsKey($path)) `
            "OBS configuration file was not restored: $path"
        Assert-True (
            [UInt64]$actualFiles[$path].Length -eq [UInt64]$expectedFiles[$path].Length -and
            [string]$actualFiles[$path].Sha256 -eq [string]$expectedFiles[$path].Sha256) `
            "OBS configuration bytes differ after restoration: $path"
    }
    $expectedDirectories = @($Expected.Directories | Sort-Object)
    $actualDirectories = @($Actual.Directories | Sort-Object)
    Assert-True ($expectedDirectories.Count -eq $actualDirectories.Count) `
        'OBS configuration directory set was not restored exactly.'
    for ($index = 0; $index -lt $expectedDirectories.Count; ++$index)
    {
        Assert-True ($expectedDirectories[$index] -eq $actualDirectories[$index]) `
            'OBS configuration directory set was not restored exactly.'
    }
}

function Restore-ManagedState
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$ConfigRoot,

        [Parameter(Mandatory = $true)]
        [object]$Expected,

        [Parameter(Mandatory = $true)]
        [string]$BackupRoot,

        [Parameter(Mandatory = $true)]
        [string[]]$ManagedDirectories,

        [Parameter(Mandatory = $true)]
        [string[]]$ManagedFiles
    )

    $expectedFiles = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($file in @($Expected.Files))
    {
        $expectedFiles.Add([string]$file.RelativePath) | Out-Null
    }
    $expectedDirectories = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($directory in @($Expected.Directories))
    {
        $expectedDirectories.Add([string]$directory) | Out-Null
    }

    $current = Get-ManagedState `
        -ConfigRoot $ConfigRoot `
        -ManagedDirectories $ManagedDirectories `
        -ManagedFiles $ManagedFiles
    foreach ($file in @($current.Files))
    {
        if ($expectedFiles.Contains([string]$file.RelativePath))
        {
            continue
        }
        $path = Get-PathFromRelative -Root $ConfigRoot -RelativePath $file.RelativePath
        Assert-True (Test-IsManagedPath `
            -Path $path `
            -ManagedDirectories $ManagedDirectories `
            -ManagedFiles $ManagedFiles) "Refusing to remove an unmanaged path: $path"
        Remove-Item -LiteralPath $path -Force
    }

    foreach ($directory in @($Expected.Directories))
    {
        $path = Get-PathFromRelative -Root $ConfigRoot -RelativePath $directory
        [IO.Directory]::CreateDirectory($path) | Out-Null
    }
    foreach ($file in @($Expected.Files))
    {
        $source = Get-PathFromRelative -Root $BackupRoot -RelativePath $file.RelativePath
        $destination = Get-PathFromRelative -Root $ConfigRoot -RelativePath $file.RelativePath
        [IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }

    $afterFiles = Get-ManagedState `
        -ConfigRoot $ConfigRoot `
        -ManagedDirectories $ManagedDirectories `
        -ManagedFiles $ManagedFiles
    foreach ($directory in @($afterFiles.Directories | Sort-Object -Property Length -Descending))
    {
        if ($expectedDirectories.Contains([string]$directory))
        {
            continue
        }
        $path = Get-PathFromRelative -Root $ConfigRoot -RelativePath $directory
        Assert-True (Test-IsManagedPath `
            -Path $path `
            -ManagedDirectories $ManagedDirectories `
            -ManagedFiles $ManagedFiles) "Refusing to remove an unmanaged directory: $path"
        Assert-True (@(Get-ChildItem -LiteralPath $path -Force).Count -eq 0) `
            "Temporary OBS configuration directory is not empty: $path"
        Remove-Item -LiteralPath $path -Force
    }

    $actual = Get-ManagedState `
        -ConfigRoot $ConfigRoot `
        -ManagedDirectories $ManagedDirectories `
        -ManagedFiles $ManagedFiles
    Assert-StatesEqual -Expected $Expected -Actual $actual
    return $actual
}

function Enable-ObsWebSocketTemporarily
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$ConfigPath
    )

    $document = Read-JsonFile -Path $ConfigPath
    $property = $document.PSObject.Properties['server_enabled']
    if ($null -eq $property)
    {
        $document | Add-Member -NotePropertyName server_enabled -NotePropertyValue $true
    }
    else
    {
        $document.server_enabled = $true
    }
    [IO.File]::WriteAllText(
        $ConfigPath,
        ($document | ConvertTo-Json -Depth 32) + [Environment]::NewLine,
        $utf8NoBom)
}

function Resolve-ApplicationCommand
{
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Names,

        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    foreach ($name in $Names)
    {
        $command = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($null -ne $command)
        {
            return $command.Source
        }
    }
    throw "$Label was not found on PATH."
}

function Get-VideoProbe
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$EvidencePath
    )

    $output = @(& $script:ffprobePath `
        '-v' 'error' `
        '-print_format' 'json' `
        '-show_format' `
        '-show_streams' `
        $Path 2>&1)
    $exitCode = $LASTEXITCODE
    $text = ($output | ForEach-Object { [string]$_ }) -join [Environment]::NewLine
    Assert-True ($exitCode -eq 0) "FFprobe could not inspect $Path`: $text"
    [IO.File]::WriteAllText(
        $EvidencePath,
        $text + [Environment]::NewLine,
        $utf8NoBom)
    return Read-JsonFile -Path $EvidencePath
}

function Invoke-ObsRequest
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Request,

        [object]$Data = @{}
    )

    $requestData = $Data | ConvertTo-Json -Depth 32 -Compress
    # Windows PowerShell 5 removes embedded quotes when it launches native
    # programs. Base64 keeps request JSON byte-exact across that boundary.
    $requestDataBase64 = [Convert]::ToBase64String(
        [Text.Encoding]::UTF8.GetBytes($requestData))
    $startedAt = [DateTime]::UtcNow.ToString('o')
    $output = @(& $script:nodePath `
        $script:obsRequestHelper `
        '--config' $script:webSocketConfig `
        '--request' $Request `
        '--data-base64' $requestDataBase64 `
        '--timeout-ms' ([string]$requestTimeoutMilliseconds) 2>&1)
    $exitCode = $LASTEXITCODE
    $outputText = ($output | ForEach-Object { [string]$_ }) -join [Environment]::NewLine
    if ($exitCode -ne 0)
    {
        $script:requestTranscript.Add([ordered]@{
            timestampUtc = $startedAt
            request = $Request
            status = 'failed'
            error = $outputText
        })
        throw "OBS request '$Request' failed: $outputText"
    }
    $response = if ([string]::IsNullOrWhiteSpace($outputText))
    {
        [pscustomobject]@{}
    }
    else
    {
        $outputText | ConvertFrom-Json
    }
    $script:requestTranscript.Add([ordered]@{
        timestampUtc = $startedAt
        request = $Request
        status = 'passed'
    })
    return $response
}

function Wait-ForObsWebSocket
{
    for ($attempt = 0; $attempt -lt 80; ++$attempt)
    {
        try
        {
            return Invoke-ObsRequest -Request 'GetVersion'
        }
        catch
        {
            Start-Sleep -Milliseconds 250
        }
    }
    throw 'OBS WebSocket did not become ready within 20 seconds.'
}

function Invoke-CleanupRequest
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Request,

        [object]$Data = @{}
    )

    try
    {
        Invoke-ObsRequest -Request $Request -Data $Data | Out-Null
    }
    catch
    {
        $script:cleanupErrors.Add($_.Exception.Message)
    }
}

function Get-TemporarySceneCollectionContract
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$ScenesRoot,

        [Parameter(Mandatory = $true)]
        [string]$CollectionName,

        [Parameter(Mandatory = $true)]
        [string]$SceneName,

        [Parameter(Mandatory = $true)]
        [string]$SpoutInputName,

        [Parameter(Mandatory = $true)]
        [string]$EvidencePath
    )

    $matching = [Collections.Generic.List[object]]::new()
    foreach ($file in @(Get-ChildItem -LiteralPath $ScenesRoot -Filter '*.json' -File -Force))
    {
        try
        {
            $document = Read-JsonFile -Path $file.FullName
            if ([string]$document.name -eq $CollectionName)
            {
                $matching.Add([pscustomobject]@{
                    File = $file
                    Document = $document
                })
            }
        }
        catch
        {
            continue
        }
    }
    Assert-True ($matching.Count -eq 1) `
        "Expected one serialized temporary scene collection, found $($matching.Count)."
    $entry = $matching[0]
    $sources = @($entry.Document.sources)
    $spoutSources = @($sources | Where-Object {
        [string]$_.name -eq $SpoutInputName -and [string]$_.id -eq 'spout_capture'
    })
    Assert-True ($spoutSources.Count -eq 1) `
        'Serialized temporary collection does not contain the isolated Spout2 input.'
    $sceneSources = @($sources | Where-Object {
        [string]$_.name -eq $SceneName -and [string]$_.id -eq 'scene'
    })
    Assert-True ($sceneSources.Count -eq 1) `
        'Serialized temporary collection does not contain the isolated test scene.'
    $items = @($sceneSources[0].settings.items)
    $spoutItems = @($items | Where-Object { [string]$_.name -eq $SpoutInputName })
    Assert-True ($spoutItems.Count -eq 1) `
        'Serialized temporary scene does not contain exactly one Spout2 item.'
    $spoutItem = $spoutItems[0]
    Assert-True ([string]$spoutItem.blend_type -eq 'normal') `
        'Serialized Spout2 item blend_type is not normal.'
    Assert-True ([string]$spoutItem.blend_method -eq 'default') `
        'Serialized Spout2 item blend_method is not default.'
    Assert-True ([int]$spoutSources[0].settings.compositemode -eq 4) `
        'Serialized Spout2 input compositemode is not Premultiplied Alpha.'
    Assert-True ([string]$spoutSources[0].settings.spoutsenders -eq $senderName) `
        'Serialized Spout2 input is bound to the wrong sender.'

    Copy-Item -LiteralPath $entry.File.FullName -Destination $EvidencePath -Force
    return [ordered]@{
        schemaVersion = 1
        collection = $CollectionName
        scene = $SceneName
        input = $SpoutInputName
        sourceKind = 'spout_capture'
        compositeMode = 4
        blendType = 'normal'
        blendMethod = 'default'
        sourceSrgbAware = $false
        expectedBlendDomain = 'srgb-byte'
        serializedCollection = Split-Path -Leaf $EvidencePath
        sha256 = (Get-FileHash -LiteralPath $EvidencePath -Algorithm SHA256).Hash
    }
}

function Copy-ObsSessionLog
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$ConfigRoot,

        [Parameter(Mandatory = $true)]
        [DateTime]$StartedAtUtc,

        [Parameter(Mandatory = $true)]
        [string]$Destination
    )

    $logsRoot = Join-Path $ConfigRoot 'logs'
    Assert-True (Test-Path -LiteralPath $logsRoot -PathType Container) `
        "OBS log directory was not found: $logsRoot"
    $log = @(Get-ChildItem -LiteralPath $logsRoot -File -Force |
        Where-Object { $_.LastWriteTimeUtc -ge $StartedAtUtc.AddSeconds(-2) } |
        Sort-Object -Property LastWriteTimeUtc -Descending |
        Select-Object -First 1)
    Assert-True ($log.Count -eq 1) 'OBS did not write a log for the acceptance session.'
    Copy-Item -LiteralPath $log[0].FullName -Destination $Destination -Force
}

function Get-ObsSessionLogContract
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$SpoutInputName,

        [Parameter(Mandatory = $true)]
        [int]$Width,

        [Parameter(Mandatory = $true)]
        [int]$Height
    )

    $content = [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8)
    $pluginLoadCount = [regex]::Matches(
        $content,
        'win-spout loaded!',
        [Text.RegularExpressions.RegexOptions]::IgnoreCase).Count
    Assert-True ($pluginLoadCount -eq 1 -and $content -match '(?i)win-spout\.dll') `
        'OBS log does not prove that win-spout.dll loaded exactly once.'

    $escapedInput = [regex]::Escape($SpoutInputName)
    $escapedSender = [regex]::Escape($senderName)
    $dimensionPattern = '\[' + $escapedInput + '\].*Sender ' + $escapedSender +
        ' is of dimensions ' + $Width + ' x ' + $Height
    $senderAcquisitions = [regex]::Matches(
        $content,
        $dimensionPattern,
        [Text.RegularExpressions.RegexOptions]::IgnoreCase).Count
    Assert-True ($senderAcquisitions -eq 1) `
        "OBS log recorded $senderAcquisitions acquisitions for the isolated Spout2 source."

    $senderUnavailablePattern = '\[' + $escapedInput + '\].*(?:' +
        'Sender\s+' + $escapedSender + '\s+has changed\s*/\s*gone away|' +
        'Sorry,\s*Sender Name.*not found|' +
        'Named.*sender not found)'
    $senderUnavailableCount = [regex]::Matches(
        $content,
        $senderUnavailablePattern,
        [Text.RegularExpressions.RegexOptions]::IgnoreCase).Count
    $sourceFailureCount = [regex]::Matches(
        $content,
        "Source ID\s*'spout_capture'\s*not found|Failed to create source.*spout_capture",
        [Text.RegularExpressions.RegexOptions]::IgnoreCase).Count
    Assert-True ($senderUnavailableCount -eq 0 -and $sourceFailureCount -eq 0) `
        'OBS log contains sender loss or Spout2 source creation failure.'

    return [ordered]@{
        schemaVersion = 1
        status = 'passed'
        plugin = 'win-spout.dll'
        pluginLoadCount = $pluginLoadCount
        isolatedInput = $SpoutInputName
        sender = $senderName
        senderAcquisitionCount = $senderAcquisitions
        senderDimensions = @($Width, $Height)
        senderUnavailableCount = $senderUnavailableCount
        sourceFailureCount = $sourceFailureCount
        log = Split-Path -Leaf $Path
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    }
}

function Remove-BackupDirectory
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
    $resolved = [IO.Path]::GetFullPath($Path)
    $prefix = $temporaryRoot + [IO.Path]::DirectorySeparatorChar
    Assert-True ($resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) `
        "Refusing to remove a backup outside the temporary directory: $resolved"
    Assert-True ((Split-Path -Leaf $resolved).StartsWith('bafx-obs-acceptance-',
        [StringComparison]::OrdinalIgnoreCase)) `
        "Refusing to remove an unexpected temporary directory: $resolved"
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

function Remove-GeneratedRecordingDirectory
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
    $resolved = [IO.Path]::GetFullPath($Path)
    $prefix = $temporaryRoot + [IO.Path]::DirectorySeparatorChar
    Assert-True ($resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) `
        "Refusing to remove a recording directory outside temporary storage: $resolved"
    Assert-True ((Split-Path -Leaf $resolved) -match '^bafx-obs-recording-[0-9a-f]{32}$') `
        "Refusing to remove an unexpected recording directory: $resolved"
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

function Write-EvidenceHashes
{
    param(
        [Parameter(Mandatory = $true)]
        [string]$EvidenceRoot
    )

    $hashPath = Join-Path $EvidenceRoot 'SHA256SUMS.txt'
    $lines = @(Get-ChildItem -LiteralPath $EvidenceRoot -File -Force -Recurse |
        Where-Object { $_.FullName -ne $hashPath } |
        Sort-Object -Property FullName |
        ForEach-Object {
            $relative = Get-RelativeChildPath -Root $EvidenceRoot -Path $_.FullName
            '{0} *{1}' -f (
                (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()),
                $relative
        })
    [IO.File]::WriteAllLines($hashPath, $lines, $utf8NoBom)
}

$resolvedHost = Resolve-Executable -Path $HostPath -Label 'Host executable'
$resolvedProbe = Resolve-Executable -Path $ProbePath -Label 'Receiver probe'
$resolvedObs = Resolve-Executable -Path $ObsPath -Label 'OBS executable'
$resolvedOutput = [IO.Path]::GetFullPath($OutputDirectory)
Assert-True (-not (Test-Path -LiteralPath $resolvedOutput)) `
    "Output directory already exists; refusing to overwrite evidence: $resolvedOutput"

$existingHosts = @(Get-Process -Name 'ba-click-fx-desktop' -ErrorAction SilentlyContinue)
Assert-True ($existingHosts.Count -eq 0) `
    'A ba-click-fx-desktop process is already running; the sender name would be ambiguous.'
$obsProcessName = [IO.Path]::GetFileNameWithoutExtension($resolvedObs)
$existingObs = @(Get-Process -Name $obsProcessName -ErrorAction SilentlyContinue)
Assert-True ($existingObs.Count -eq 0) `
    'OBS is already running. Close it so its configuration can be backed up and restored exactly.'

$obsInstallRoot = Resolve-ObsInstallRoot -ExecutablePath $resolvedObs
$obsConfig = Resolve-ObsConfigRoot -InstallRoot $obsInstallRoot
$configRoot = [IO.Path]::GetFullPath($obsConfig.Root)
$profilesRoot = Join-Path $configRoot 'basic\profiles'
$scenesRoot = Join-Path $configRoot 'basic\scenes'
$globalConfig = Join-Path $configRoot 'global.ini'
$userConfig = Join-Path $configRoot 'user.ini'
$webSocketConfig = Join-Path $configRoot 'plugin_config\obs-websocket\config.json'
$sentinelRoot = Join-Path $configRoot '.sentinel'
$configPrefix = $configRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
Assert-True (-not $resolvedOutput.Equals(
        $configRoot,
        [StringComparison]::OrdinalIgnoreCase) -and
    -not $resolvedOutput.StartsWith(
        $configPrefix,
        [StringComparison]::OrdinalIgnoreCase)) `
    'OutputDirectory must be outside the OBS configuration root.'
foreach ($requiredPath in @($profilesRoot, $scenesRoot))
{
    Assert-True (Test-Path -LiteralPath $requiredPath -PathType Container) `
        "OBS configuration directory was not found: $requiredPath"
}
foreach ($requiredPath in @($globalConfig, $userConfig, $webSocketConfig))
{
    Assert-True (Test-Path -LiteralPath $requiredPath -PathType Leaf) `
        "OBS configuration file was not found: $requiredPath"
}

$obsRequestHelper = Join-Path $PSScriptRoot 'obs-websocket-request.mjs'
$verifier = Join-Path $PSScriptRoot 'verify-obs-spout2-composite.py'
$recordingVerifier = Join-Path $PSScriptRoot 'verify-obs-spout2-recording.py'
Assert-True (Test-Path -LiteralPath $obsRequestHelper -PathType Leaf) `
    "OBS WebSocket helper was not found: $obsRequestHelper"
Assert-True (Test-Path -LiteralPath $verifier -PathType Leaf) `
    "OBS composite verifier was not found: $verifier"
Assert-True (Test-Path -LiteralPath $recordingVerifier -PathType Leaf) `
    "OBS recording verifier was not found: $recordingVerifier"
$nodePath = Resolve-ApplicationCommand -Names @('node.exe', 'node') -Label 'Node.js'
$pythonPath = $null
$pythonPrefix = @()
try
{
    $pythonPath = Resolve-ApplicationCommand -Names @('python.exe', 'python') -Label 'Python'
}
catch
{
    $pythonPath = Resolve-ApplicationCommand -Names @('py.exe', 'py') -Label 'Python launcher'
    $pythonPrefix = @('-3')
}
$ffmpegPath = Resolve-ApplicationCommand -Names @('ffmpeg.exe', 'ffmpeg') -Label 'FFmpeg'
$ffprobePath = Resolve-ApplicationCommand -Names @('ffprobe.exe', 'ffprobe') -Label 'FFprobe'

[IO.Directory]::CreateDirectory($resolvedOutput) | Out-Null
$backupRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'bafx-obs-acceptance-' + [Guid]::NewGuid().ToString('N'))
$runId = [Guid]::NewGuid().ToString('N')
$testProfile = "BAFX-OBS-Acceptance-$runId"
$testCollection = "BAFX-OBS-Acceptance-$runId"
$testScene = "BAFX-OBS-Composite-$runId"
$backgroundInput = "BAFX-Background-$runId"
$spoutInput = "BAFX-Spout2-$runId"
$recordingWorkRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'bafx-obs-recording-' + $runId)
$activeProfileName = Get-IniValue -Path $userConfig -Section 'Basic' -Key 'ProfileDir'
$activeCollectionName = Get-IniValue `
    -Path $userConfig `
    -Section 'Basic' `
    -Key 'SceneCollectionFile'
Assert-True (-not [string]::IsNullOrWhiteSpace($activeProfileName) -and
    -not [string]::IsNullOrWhiteSpace($activeCollectionName)) `
    'OBS active profile or scene collection identifier is empty.'
$activeProfileRoot = [IO.Path]::GetFullPath((Join-Path $profilesRoot $activeProfileName))
$activeCollectionFileName = if ($activeCollectionName.EndsWith(
        '.json',
        [StringComparison]::OrdinalIgnoreCase))
{
    $activeCollectionName
}
else
{
    "$activeCollectionName.json"
}
$activeCollectionPath = [IO.Path]::GetFullPath((
    Join-Path $scenesRoot $activeCollectionFileName))
Get-RelativeChildPath -Root $profilesRoot -Path $activeProfileRoot | Out-Null
Get-RelativeChildPath -Root $scenesRoot -Path $activeCollectionPath | Out-Null
Assert-True (Test-Path -LiteralPath $activeProfileRoot -PathType Container) `
    "Active OBS profile directory was not found: $activeProfileRoot"
Assert-True (Test-Path -LiteralPath $activeCollectionPath -PathType Leaf) `
    "Active OBS scene collection was not found: $activeCollectionPath"
$temporaryProfileRoot = $null
$temporaryCollectionPath = $null
$managedDirectories = @($profilesRoot, $scenesRoot, $sentinelRoot)
$managedFiles = [Collections.Generic.List[string]]::new()
foreach ($file in @(
    $globalConfig,
    $userConfig,
    $webSocketConfig))
{
    $managedFiles.Add($file)
}
$auditDirectories = $managedDirectories
$auditFiles = @($managedFiles)
$beforeState = $null
$afterState = $null
$auditBeforeState = $null
$auditAfterState = $null
$backupReady = $false
$externalRestoreExact = $false
$hostProcess = $null
$obsProcess = $null
$obsStartedAtUtc = [DateTime]::MinValue
$obsForcedTermination = $false
$mainError = $null
$mainPassed = $false
$sceneContract = $null
$obsLogContract = $null
$recordingContract = $null
$requestTranscript = [Collections.Generic.List[object]]::new()
$cleanupErrors = [Collections.Generic.List[string]]::new()
$postShutdownErrors = [Collections.Generic.List[string]]::new()
$testProfileCreated = $false
$testCollectionCreated = $false
$testSceneCreated = $false
$spoutInputCreated = $false
$recordingStarted = $false
$recordingWorkReady = $false
$recordingWorkRemoved = $true
$originalProfile = $null
$originalCollection = $null
$originalScene = $null
$activeFrame = Join-Path $resolvedOutput 'active-frame.bgra'
$activeFrameAfter = Join-Path $resolvedOutput 'active-frame-after.bgra'
$probeJson = Join-Path $resolvedOutput 'receiver.json'
$probeAfterJson = Join-Path $resolvedOutput 'receiver-after.json'
$manifestPath = Join-Path $resolvedOutput 'composite-manifest.json'
$verificationReport = Join-Path $resolvedOutput 'composite-verification.json'
$recordingManifestPath = Join-Path $resolvedOutput 'recording-manifest.json'
$recordingVerificationReport = Join-Path $resolvedOutput 'recording-verification.json'
$recordingPath = $null
$recordingRawPath = $null
$frameWidth = 0
$frameHeight = 0

try
{
    $auditBeforeState = Get-ManagedState `
        -ConfigRoot $configRoot `
        -ManagedDirectories $auditDirectories `
        -ManagedFiles $auditFiles
    $beforeState = Get-ManagedState `
        -ConfigRoot $configRoot `
        -ManagedDirectories $managedDirectories `
        -ManagedFiles $managedFiles
    Write-JsonFile `
        -Path (Join-Path $resolvedOutput 'external-state-before.json') `
        -Value (ConvertTo-EvidenceState -ConfigRoot $configRoot -State $auditBeforeState)
    Backup-ManagedState `
        -ConfigRoot $configRoot `
        -State $beforeState `
        -BackupRoot $backupRoot
    $backupReady = $true
    Enable-ObsWebSocketTemporarily -ConfigPath $webSocketConfig
    if (Test-Path -LiteralPath $sentinelRoot -PathType Container)
    {
        # OBS 32 ignores --disable-shutdown-check. Temporarily removing the
        # run sentinels avoids a blocking Safe Mode dialog; the exact original
        # directory is restored after OBS exits.
        foreach ($sentinel in @(Get-ChildItem `
            -LiteralPath $sentinelRoot `
            -File `
            -Force | Where-Object { $_.Name -match '^run_[0-9a-f-]+$' }))
        {
            Get-RelativeChildPath -Root $sentinelRoot -Path $sentinel.FullName | Out-Null
            Remove-Item -LiteralPath $sentinel.FullName -Force
        }
    }

    $hostProcess = Start-Process `
        -FilePath $resolvedHost `
        -ArgumentList @(
            '--spout2',
            '--demo-delay-ms=2000',
            '--demo-age-ms=130',
            '--disable-raw-input',
            '--quit-after-ms=120000'
        ) `
        -WorkingDirectory $resolvedOutput `
        -RedirectStandardOutput (Join-Path $resolvedOutput 'host.stdout.log') `
        -RedirectStandardError (Join-Path $resolvedOutput 'host.stderr.log') `
        -WindowStyle Hidden `
        -PassThru

    & $resolvedProbe `
        "--sender=$senderName" `
        '--duration-ms=5000' `
        '--interval-ms=100' `
        "--capture-output=$activeFrame" `
        "--output=$probeJson" `
        1> (Join-Path $resolvedOutput 'receiver.stdout.log') `
        2> (Join-Path $resolvedOutput 'receiver.stderr.log')
    Assert-True ($LASTEXITCODE -eq 0) `
        "Receiver probe failed with exit code $LASTEXITCODE."
    Assert-True (-not $hostProcess.HasExited) `
        'Host exited before OBS could consume the fixed-age sender.'

    $probe = Read-JsonFile -Path $probeJson
    Assert-True ([int]$probe.schemaVersion -eq 2) 'Receiver probe schema is unsupported.'
    Assert-True ([string]$probe.sender -eq $senderName) `
        'Receiver probe connected to the wrong sender.'
    Assert-True ($null -ne $probe.capturedFrame) `
        'Receiver probe did not preserve an extended-premultiplied frame.'
    $connectedSamples = @($probe.samples | Where-Object { [bool]$_.connected })
    Assert-True ($connectedSamples.Count -gt 0) `
        'Receiver probe never connected to the Host sender.'
    $senderHandles = @($connectedSamples | ForEach-Object { [string]$_.handle } |
        Sort-Object -Unique)
    Assert-True ($senderHandles.Count -eq 1 -and $senderHandles[0] -ne '0x0') `
        'Spout2 shared handle was null or changed before OBS capture.'
    Assert-True (@($connectedSamples | Where-Object {
        [UInt64]$_.extendedPremultipliedPixels -gt 0
    }).Count -gt 0) 'Receiver observed no RGB-above-Alpha pixels.'
    Assert-True (@($connectedSamples | Where-Object {
        [UInt64]$_.zeroAlphaEmissionPixels -gt 0
    }).Count -gt 0) 'Receiver observed no zero-Alpha additive emission.'
    $frameWidth = [int]$probe.capturedFrame.width
    $frameHeight = [int]$probe.capturedFrame.height
    Assert-True ($frameWidth -gt 0 -and $frameHeight -gt 0) `
        'Receiver probe reported invalid frame dimensions.'
    Assert-True ([int]$probe.capturedFrame.format -eq $frameFormat) `
        'Receiver probe did not capture DXGI_FORMAT_B8G8R8A8_UNORM.'
    Assert-True (Test-Path -LiteralPath $activeFrame -PathType Leaf) `
        'Receiver raw BGRA evidence is missing.'
    $expectedBytes = [UInt64]$frameWidth * [UInt64]$frameHeight * 4
    Assert-True ([UInt64](Get-Item -LiteralPath $activeFrame).Length -eq $expectedBytes) `
        'Receiver raw BGRA evidence has the wrong byte count.'

    $obsArguments = @('--disable-shutdown-check')
    if ($obsConfig.Portable)
    {
        $obsArguments += '--portable'
    }
    $obsStartedAtUtc = [DateTime]::UtcNow
    $obsProcess = Start-Process `
        -FilePath $resolvedObs `
        -ArgumentList $obsArguments `
        -WorkingDirectory (Split-Path -Parent $resolvedObs) `
        -PassThru
    $version = Wait-ForObsWebSocket

    $profileList = Invoke-ObsRequest -Request 'GetProfileList'
    $collectionList = Invoke-ObsRequest -Request 'GetSceneCollectionList'
    $programScene = Invoke-ObsRequest -Request 'GetCurrentProgramScene'
    $originalProfile = [string]$profileList.currentProfileName
    $originalCollection = [string]$collectionList.currentSceneCollectionName
    $originalScene = [string]$programScene.currentProgramSceneName
    if ([string]::IsNullOrWhiteSpace($originalScene))
    {
        $originalScene = [string]$programScene.sceneName
    }
    Assert-True (-not [string]::IsNullOrWhiteSpace($originalProfile)) `
        'OBS did not report its original profile.'
    Assert-True (-not [string]::IsNullOrWhiteSpace($originalCollection)) `
        'OBS did not report its original scene collection.'
    Assert-True (-not [string]::IsNullOrWhiteSpace($originalScene)) `
        'OBS did not report its original program scene.'
    Write-JsonFile -Path (Join-Path $resolvedOutput 'runtime-state-before.json') -Value ([ordered]@{
        schemaVersion = 1
        obsVersion = $version.obsVersion
        obsWebSocketVersion = $version.obsWebSocketVersion
        profile = $originalProfile
        sceneCollection = $originalCollection
        programScene = $originalScene
    })

    $profileDirectoriesBeforeCreate = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($directory in @(Get-ChildItem -LiteralPath $profilesRoot -Directory -Force))
    {
        $profileDirectoriesBeforeCreate.Add(
            [IO.Path]::GetFullPath($directory.FullName)) | Out-Null
    }
    Invoke-ObsRequest -Request 'CreateProfile' -Data ([ordered]@{
        profileName = $testProfile
    }) | Out-Null
    $testProfileCreated = $true
    $newProfileDirectories = @()
    for ($attempt = 0; $attempt -lt 20; ++$attempt)
    {
        $newProfileDirectories = @(Get-ChildItem `
            -LiteralPath $profilesRoot `
            -Directory `
            -Force | Where-Object {
                -not $profileDirectoriesBeforeCreate.Contains(
                    [IO.Path]::GetFullPath($_.FullName))
            })
        if ($newProfileDirectories.Count -eq 1)
        {
            break
        }
        Start-Sleep -Milliseconds 100
    }
    Assert-True ($newProfileDirectories.Count -eq 1) `
        'OBS did not create exactly one isolated temporary profile directory.'
    $temporaryProfileRoot = [IO.Path]::GetFullPath(
        $newProfileDirectories[0].FullName)
    Get-RelativeChildPath -Root $profilesRoot -Path $temporaryProfileRoot | Out-Null
    $temporaryProfileConfig = Join-Path $temporaryProfileRoot 'basic.ini'
    Assert-True (Test-Path -LiteralPath $temporaryProfileConfig -PathType Leaf) `
        'OBS temporary profile does not contain basic.ini.'
    Assert-True ((Get-IniValue `
        -Path $temporaryProfileConfig `
        -Section 'General' `
        -Key 'Name') -eq $testProfile) `
        'OBS temporary profile directory belongs to a different profile.'
    Invoke-ObsRequest -Request 'SetVideoSettings' -Data ([ordered]@{
        baseWidth = $frameWidth
        baseHeight = $frameHeight
        outputWidth = $frameWidth
        outputHeight = $frameHeight
        fpsNumerator = 30
        fpsDenominator = 1
    }) | Out-Null
    $profileParameters = @(
        [ordered]@{ category = 'Output'; name = 'Mode'; value = 'Simple' },
        [ordered]@{ category = 'SimpleOutput'; name = 'RecQuality'; value = 'Stream' },
        [ordered]@{ category = 'SimpleOutput'; name = 'RecEncoder'; value = 'nvenc' },
        [ordered]@{ category = 'SimpleOutput'; name = 'RecFormat2'; value = 'hybrid_mp4' },
        [ordered]@{ category = 'SimpleOutput'; name = 'VBitrate'; value = '20000' },
        [ordered]@{ category = 'SimpleOutput'; name = 'NVENCPreset2'; value = 'p5' },
        [ordered]@{ category = 'Video'; name = 'ColorFormat'; value = 'NV12' },
        [ordered]@{ category = 'Video'; name = 'ColorSpace'; value = '709' },
        [ordered]@{ category = 'Video'; name = 'ColorRange'; value = 'Partial' }
    )
    foreach ($parameter in $profileParameters)
    {
        Invoke-ObsRequest -Request 'SetProfileParameter' -Data ([ordered]@{
            parameterCategory = $parameter.category
            parameterName = $parameter.name
            parameterValue = $parameter.value
        }) | Out-Null
        $observedParameter = Invoke-ObsRequest -Request 'GetProfileParameter' -Data ([ordered]@{
            parameterCategory = $parameter.category
            parameterName = $parameter.name
        })
        Assert-True ([string]$observedParameter.parameterValue -eq $parameter.value) `
            "OBS profile parameter did not persist: $($parameter.category)/$($parameter.name)"
    }
    Copy-Item `
        -LiteralPath $temporaryProfileConfig `
        -Destination (Join-Path $resolvedOutput 'temporary-profile-basic.ini') `
        -Force

    # A separate collection keeps every test scene and input out of the user's
    # scene graph. The byte snapshot below removes the collection after OBS exits.
    Invoke-ObsRequest -Request 'CreateSceneCollection' -Data ([ordered]@{
        sceneCollectionName = $testCollection
    }) | Out-Null
    $testCollectionCreated = $true
    $matchingCollections = @()
    for ($attempt = 0; $attempt -lt 20; ++$attempt)
    {
        $matchingCollections = @(Get-ChildItem `
            -LiteralPath $scenesRoot `
            -Filter '*.json' `
            -File `
            -Force | Where-Object {
                try
                {
                    $candidate = Read-JsonFile -Path $_.FullName
                    [string]$candidate.name -eq $testCollection
                }
                catch
                {
                    $false
                }
            })
        if ($matchingCollections.Count -eq 1)
        {
            break
        }
        Start-Sleep -Milliseconds 100
    }
    Assert-True ($matchingCollections.Count -eq 1) `
        'OBS did not serialize exactly one isolated temporary scene collection.'
    $temporaryCollectionPath = [IO.Path]::GetFullPath(
        $matchingCollections[0].FullName)
    Get-RelativeChildPath -Root $scenesRoot -Path $temporaryCollectionPath | Out-Null
    Invoke-ObsRequest -Request 'SetCurrentSceneCollection' -Data ([ordered]@{
        sceneCollectionName = $testCollection
    }) | Out-Null
    Invoke-ObsRequest -Request 'CreateScene' -Data ([ordered]@{
        sceneName = $testScene
    }) | Out-Null
    $testSceneCreated = $true
    Invoke-ObsRequest -Request 'SetCurrentProgramScene' -Data ([ordered]@{
        sceneName = $testScene
    }) | Out-Null

    $backgroundResponse = Invoke-ObsRequest -Request 'CreateInput' -Data ([ordered]@{
        sceneName = $testScene
        inputName = $backgroundInput
        inputKind = 'color_source_v3'
        inputSettings = [ordered]@{
            color = [Int64]4278190080
            width = $frameWidth
            height = $frameHeight
        }
        sceneItemEnabled = $true
    })
    $backgroundItemId = [int]$backgroundResponse.sceneItemId
    $spoutResponse = Invoke-ObsRequest -Request 'CreateInput' -Data ([ordered]@{
        sceneName = $testScene
        inputName = $spoutInput
        inputKind = 'spout_capture'
        inputSettings = [ordered]@{
            spoutsenders = $senderName
            compositemode = 4
        }
        sceneItemEnabled = $true
    })
    $spoutItemId = [int]$spoutResponse.sceneItemId
    $spoutInputCreated = $true

    Invoke-ObsRequest -Request 'SetSceneItemIndex' -Data ([ordered]@{
        sceneName = $testScene
        sceneItemId = $backgroundItemId
        sceneItemIndex = 0
    }) | Out-Null
    Invoke-ObsRequest -Request 'SetSceneItemIndex' -Data ([ordered]@{
        sceneName = $testScene
        sceneItemId = $spoutItemId
        sceneItemIndex = 1
    }) | Out-Null
    Invoke-ObsRequest -Request 'SetSceneItemBlendMode' -Data ([ordered]@{
        sceneName = $testScene
        sceneItemId = $backgroundItemId
        sceneItemBlendMode = 'OBS_BLEND_NORMAL'
    }) | Out-Null
    Invoke-ObsRequest -Request 'SetSceneItemBlendMode' -Data ([ordered]@{
        sceneName = $testScene
        sceneItemId = $spoutItemId
        sceneItemBlendMode = 'OBS_BLEND_NORMAL'
    }) | Out-Null

    $spoutTransform = $null
    for ($attempt = 0; $attempt -lt 80; ++$attempt)
    {
        $spoutTransform = Invoke-ObsRequest -Request 'GetSceneItemTransform' -Data ([ordered]@{
            sceneName = $testScene
            sceneItemId = $spoutItemId
        })
        if ([int]$spoutTransform.sceneItemTransform.sourceWidth -eq $frameWidth -and
            [int]$spoutTransform.sceneItemTransform.sourceHeight -eq $frameHeight)
        {
            break
        }
        Start-Sleep -Milliseconds 100
    }
    Assert-True ([int]$spoutTransform.sceneItemTransform.sourceWidth -eq $frameWidth -and
        [int]$spoutTransform.sceneItemTransform.sourceHeight -eq $frameHeight) `
        'OBS Spout2 source did not acquire the fixed-size sender.'

    foreach ($itemId in @($backgroundItemId, $spoutItemId))
    {
        Invoke-ObsRequest -Request 'SetSceneItemTransform' -Data ([ordered]@{
            sceneName = $testScene
            sceneItemId = $itemId
            sceneItemTransform = [ordered]@{
                positionX = 0.0
                positionY = 0.0
                rotation = 0.0
                scaleX = 1.0
                scaleY = 1.0
                alignment = 5
                boundsType = 'OBS_BOUNDS_NONE'
                cropTop = 0
                cropBottom = 0
                cropLeft = 0
                cropRight = 0
            }
        }) | Out-Null
    }

    $sceneItems = Invoke-ObsRequest -Request 'GetSceneItemList' -Data ([ordered]@{
        sceneName = $testScene
    })
    $items = @($sceneItems.sceneItems)
    Assert-True ($items.Count -eq 2) `
        'Isolated OBS test scene must contain exactly two items.'
    $backgroundItems = @($items | Where-Object { [string]$_.sourceName -eq $backgroundInput })
    $spoutItems = @($items | Where-Object { [string]$_.sourceName -eq $spoutInput })
    Assert-True ($backgroundItems.Count -eq 1 -and $spoutItems.Count -eq 1) `
        'Isolated OBS test scene contains unexpected sources.'
    Assert-True ([string]$backgroundItems[0].inputKind -eq 'color_source_v3' -and
        [int]$backgroundItems[0].sceneItemIndex -eq 0) `
        'Isolated OBS background is not the bottom color source.'
    Assert-True ([string]$spoutItems[0].inputKind -eq 'spout_capture' -and
        [int]$spoutItems[0].sceneItemIndex -eq 1) `
        'Isolated OBS Spout2 input is not the top source.'

    $spoutSettings = Invoke-ObsRequest -Request 'GetInputSettings' -Data ([ordered]@{
        inputName = $spoutInput
    })
    Assert-True ([string]$spoutSettings.inputKind -eq 'spout_capture') `
        'OBS did not create a Spout2 capture input.'
    Assert-True ([string]$spoutSettings.inputSettings.spoutsenders -eq $senderName -and
        [int]$spoutSettings.inputSettings.compositemode -eq 4) `
        'OBS Spout2 input does not use the required sender and Premultiplied Alpha mode.'
    $spoutBlend = Invoke-ObsRequest -Request 'GetSceneItemBlendMode' -Data ([ordered]@{
        sceneName = $testScene
        sceneItemId = $spoutItemId
    })
    Assert-True ([string]$spoutBlend.sceneItemBlendMode -eq 'OBS_BLEND_NORMAL') `
        'OBS Spout2 scene item blend mode is not Normal.'
    $backgroundFilters = Invoke-ObsRequest -Request 'GetSourceFilterList' -Data ([ordered]@{
        sourceName = $backgroundInput
    })
    $spoutFilters = Invoke-ObsRequest -Request 'GetSourceFilterList' -Data ([ordered]@{
        sourceName = $spoutInput
    })
    Assert-True (@($backgroundFilters.filters).Count -eq 0 -and
        @($spoutFilters.filters).Count -eq 0) `
        'Isolated OBS sources must not contain filters.'
    $specialInputs = Invoke-ObsRequest -Request 'GetSpecialInputs'
    $audioInputNames = [Collections.Generic.List[string]]::new()
    foreach ($property in @($specialInputs.PSObject.Properties))
    {
        if ($property.Name -in @('desktop1', 'desktop2', 'mic1', 'mic2', 'mic3', 'mic4') -and
            -not [string]::IsNullOrWhiteSpace([string]$property.Value))
        {
            $audioInputNames.Add([string]$property.Value)
        }
    }
    foreach ($audioInputName in @($audioInputNames))
    {
        Invoke-ObsRequest -Request 'SetInputMute' -Data ([ordered]@{
            inputName = $audioInputName
            inputMuted = $true
        }) | Out-Null
        $muteState = Invoke-ObsRequest -Request 'GetInputMute' -Data ([ordered]@{
            inputName = $audioInputName
        })
        Assert-True ([bool]$muteState.inputMuted) `
            "OBS special audio input was not muted: $audioInputName"
    }
    Write-JsonFile `
        -Path (Join-Path $resolvedOutput 'obs-audio-isolation.json') `
        -Value ([ordered]@{
            schemaVersion = 1
            status = 'passed'
            specialInputs = @($audioInputNames)
            allSpecialInputsMuted = $true
            finalEvidenceAudioStreams = 0
        })
    $videoSettings = Invoke-ObsRequest -Request 'GetVideoSettings'
    Assert-True ([int]$videoSettings.baseWidth -eq $frameWidth -and
        [int]$videoSettings.baseHeight -eq $frameHeight -and
        [int]$videoSettings.outputWidth -eq $frameWidth -and
        [int]$videoSettings.outputHeight -eq $frameHeight) `
        'Temporary OBS profile video dimensions do not match the Spout2 sender.'
    Assert-True (-not $hostProcess.HasExited) `
        'Host exited during the isolated OBS preflight.'

    Write-JsonFile -Path (Join-Path $resolvedOutput 'obs-isolation-preflight.json') -Value ([ordered]@{
        schemaVersion = 1
        status = 'passed'
        profile = $testProfile
        sceneCollection = $testCollection
        scene = $testScene
        itemCount = $items.Count
        background = [ordered]@{
            inputName = $backgroundInput
            inputKind = 'color_source_v3'
            sceneItemId = $backgroundItemId
            sceneItemIndex = 0
            filters = 0
        }
        spout2 = [ordered]@{
            inputName = $spoutInput
            inputKind = 'spout_capture'
            sender = $senderName
            compositeMode = 4
            sceneItemId = $spoutItemId
            sceneItemIndex = 1
            blendMode = 'OBS_BLEND_NORMAL'
            blendMethod = 'default (verified from serialized temporary collection)'
            filters = 0
        }
        dimensions = @($frameWidth, $frameHeight)
        audio = [ordered]@{
            specialInputCount = $audioInputNames.Count
            allSpecialInputsMuted = $true
            finalEvidenceAudioStreams = 0
        }
    })

    $captureCases = @(
        [ordered]@{ name = 'black'; rgb = @(0, 0, 0); packed = [Int64]4278190080 },
        [ordered]@{ name = 'gray'; rgb = @(96, 96, 96); packed = [Int64]4284506208 },
        [ordered]@{ name = 'white'; rgb = @(255, 255, 255); packed = [Int64]4294967295 },
        [ordered]@{ name = 'color'; rgb = @(32, 80, 144); packed = [Int64]4287647776 }
    )
    $manifestCases = [Collections.Generic.List[object]]::new()
    foreach ($captureCase in $captureCases)
    {
        Invoke-ObsRequest -Request 'SetInputSettings' -Data ([ordered]@{
            inputName = $backgroundInput
            inputSettings = [ordered]@{
                color = $captureCase.packed
                width = $frameWidth
                height = $frameHeight
            }
            overlay = $true
        }) | Out-Null
        Start-Sleep -Milliseconds 500
        $imageName = "$($captureCase.name).png"
        $imagePath = Join-Path $resolvedOutput $imageName
        Invoke-ObsRequest -Request 'SaveSourceScreenshot' -Data ([ordered]@{
            sourceName = $testScene
            imageFormat = 'png'
            imageFilePath = $imagePath
            imageWidth = $frameWidth
            imageHeight = $frameHeight
            imageCompressionQuality = 100
        }) | Out-Null
        Assert-True (Test-Path -LiteralPath $imagePath -PathType Leaf) `
            "OBS did not write the $($captureCase.name) screenshot."
        $manifestCases.Add([ordered]@{
            name = $captureCase.name
            backgroundRgb = @($captureCase.rgb)
            image = $imageName
        })
    }

    Invoke-ObsRequest -Request 'SetInputSettings' -Data ([ordered]@{
        inputName = $backgroundInput
        inputSettings = [ordered]@{
            color = [Int64]4284506208
            width = $frameWidth
            height = $frameHeight
        }
        overlay = $true
    }) | Out-Null
    Start-Sleep -Milliseconds 500
    # gray.png already passed the raw ONE/INV_SRC_ALPHA oracle above, so the
    # recording comparison cannot accidentally bless a darkened reference.
    $recordingReferenceName = 'gray.png'
    $recordingReferencePath = Join-Path $resolvedOutput $recordingReferenceName
    Assert-True (Test-Path -LiteralPath $recordingReferencePath -PathType Leaf) `
        'OBS did not write the recording reference screenshot.'

    $recordStatusBefore = Invoke-ObsRequest -Request 'GetRecordStatus'
    Assert-True (-not [bool]$recordStatusBefore.outputActive) `
        'OBS recording was already active in the isolated temporary profile.'
    Assert-True (-not (Test-Path -LiteralPath $recordingWorkRoot)) `
        "Temporary recording directory already exists: $recordingWorkRoot"
    [IO.Directory]::CreateDirectory($recordingWorkRoot) | Out-Null
    $recordingWorkReady = $true
    $recordingWorkRemoved = $false
    Invoke-ObsRequest -Request 'SetRecordDirectory' -Data ([ordered]@{
        recordDirectory = $recordingWorkRoot
    }) | Out-Null
    $recordDirectory = Invoke-ObsRequest -Request 'GetRecordDirectory'
    Assert-True ([IO.Path]::GetFullPath([string]$recordDirectory.recordDirectory).Equals(
            $recordingWorkRoot,
            [StringComparison]::OrdinalIgnoreCase)) `
        'OBS did not apply the isolated recording directory.'
    # Mark the side effect before the request so a WebSocket timeout after OBS
    # starts recording still triggers the status-based cleanup in finally.
    $recordingStarted = $true
    $recordStart = Invoke-ObsRequest -Request 'StartRecord'
    Write-JsonFile `
        -Path (Join-Path $resolvedOutput 'obs-record-start.json') `
        -Value $recordStart
    $recordStatus = $null
    $recordDeadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $recordDeadline)
    {
        $recordStatus = Invoke-ObsRequest -Request 'GetRecordStatus'
        if ([bool]$recordStatus.outputActive -and
            [int64]$recordStatus.outputDuration -ge 2500 -and
            [int64]$recordStatus.outputBytes -gt 0)
        {
            break
        }
        Start-Sleep -Milliseconds 250
    }
    Write-JsonFile `
        -Path (Join-Path $resolvedOutput 'obs-record-status.json') `
        -Value $recordStatus
    Assert-True ([bool]$recordStatus.outputActive -and
        [int64]$recordStatus.outputDuration -ge 2500 -and
        [int64]$recordStatus.outputBytes -gt 0) `
        'OBS did not sustain the bounded recording for at least 2.5 seconds.'
    $recordStop = Invoke-ObsRequest -Request 'StopRecord'
    Write-JsonFile `
        -Path (Join-Path $resolvedOutput 'obs-record-stop.json') `
        -Value $recordStop
    $recordStopped = $false
    for ($attempt = 0; $attempt -lt 40; ++$attempt)
    {
        $recordStatusAfterStop = Invoke-ObsRequest -Request 'GetRecordStatus'
        if (-not [bool]$recordStatusAfterStop.outputActive)
        {
            $recordStopped = $true
            break
        }
        Start-Sleep -Milliseconds 250
    }
    Assert-True $recordStopped 'OBS recording remained active after StopRecord.'
    $recordingStarted = $false
    Assert-True (-not [string]::IsNullOrWhiteSpace([string]$recordStop.outputPath)) `
        'OBS StopRecord did not return the recording output path.'
    $recordingRawPath = [IO.Path]::GetFullPath([string]$recordStop.outputPath)
    Assert-True ((Split-Path -Parent $recordingRawPath).Equals(
            $recordingWorkRoot,
            [StringComparison]::OrdinalIgnoreCase)) `
        "OBS recording was written outside temporary isolation: $recordingRawPath"
    # StopRecord can return before the muxer has renamed and flushed the final
    # container. Waiting for stable length avoids inspecting a zero-byte shell.
    Wait-ForStableFile -Path $recordingRawPath

    $originalProbePath = Join-Path $resolvedOutput 'recording-original-ffprobe.json'
    $originalProbe = Get-VideoProbe `
        -Path $recordingRawPath `
        -EvidencePath $originalProbePath
    $originalVideoStreams = @($originalProbe.streams | Where-Object {
        [string]$_.codec_type -eq 'video'
    })
    Assert-True ($originalVideoStreams.Count -eq 1) `
        'OBS recording does not contain exactly one video stream.'

    $recordingPath = Join-Path $resolvedOutput 'obs-recording-video-only.mp4'
    Assert-True (-not (Test-Path -LiteralPath $recordingPath)) `
        "Sanitized recording already exists: $recordingPath"
    $sanitizeOutput = @(& $ffmpegPath `
        '-hide_banner' `
        '-loglevel' 'error' `
        '-nostdin' `
        '-i' $recordingRawPath `
        '-map' '0:v:0' `
        '-c:v' 'copy' `
        '-an' `
        '-movflags' '+faststart' `
        '-y' `
        $recordingPath 2>&1)
    $sanitizeExitCode = $LASTEXITCODE
    $sanitizeText = ($sanitizeOutput | ForEach-Object { [string]$_ }) -join [Environment]::NewLine
    [IO.File]::WriteAllText(
        (Join-Path $resolvedOutput 'recording-sanitize.log'),
        $sanitizeText + [Environment]::NewLine,
        $utf8NoBom)
    Assert-True ($sanitizeExitCode -eq 0) `
        "FFmpeg could not remove OBS recording audio: $sanitizeText"
    Wait-ForStableFile -Path $recordingPath

    $ffprobeJsonPath = Join-Path $resolvedOutput 'recording-ffprobe.json'
    $ffprobe = Get-VideoProbe -Path $recordingPath -EvidencePath $ffprobeJsonPath
    $videoStreams = @($ffprobe.streams | Where-Object { [string]$_.codec_type -eq 'video' })
    $audioStreams = @($ffprobe.streams | Where-Object { [string]$_.codec_type -eq 'audio' })
    Assert-True ($videoStreams.Count -eq 1 -and
        [int]$videoStreams[0].width -eq $frameWidth -and
        [int]$videoStreams[0].height -eq $frameHeight -and
        $audioStreams.Count -eq 0) `
        'Sanitized recording must contain one full-size video stream and no audio.'
    [double]$recordingDurationSeconds = 0.0
    $durationParsed = [double]::TryParse(
        [string]$ffprobe.format.duration,
        [Globalization.NumberStyles]::Float,
        [Globalization.CultureInfo]::InvariantCulture,
        [ref]$recordingDurationSeconds)
    Assert-True ($durationParsed -and $recordingDurationSeconds -ge 2.0) `
        'OBS recording duration is shorter than two seconds.'
    Write-JsonFile `
        -Path (Join-Path $resolvedOutput 'recording-container.json') `
        -Value ([ordered]@{
            schemaVersion = 1
            original = [ordered]@{
                fileName = Split-Path -Leaf $recordingRawPath
                bytes = [UInt64](Get-Item -LiteralPath $recordingRawPath).Length
                sha256 = (Get-FileHash -LiteralPath $recordingRawPath -Algorithm SHA256).Hash
                audioStreamCount = @($originalProbe.streams | Where-Object {
                    [string]$_.codec_type -eq 'audio'
                }).Count
                retained = $false
            }
            evidence = [ordered]@{
                fileName = Split-Path -Leaf $recordingPath
                bytes = [UInt64](Get-Item -LiteralPath $recordingPath).Length
                sha256 = (Get-FileHash -LiteralPath $recordingPath -Algorithm SHA256).Hash
                videoStreamCount = $videoStreams.Count
                audioStreamCount = $audioStreams.Count
                videoPacketsCopiedWithoutReencoding = $true
            }
        })

    $recordingFrameName = 'recording-frame.png'
    $recordingFramePath = Join-Path $resolvedOutput $recordingFrameName
    $ffmpegOutput = @(& $ffmpegPath `
        '-hide_banner' `
        '-loglevel' 'error' `
        '-nostdin' `
        '-i' $recordingPath `
        '-ss' '1.000' `
        '-frames:v' '1' `
        '-an' `
        '-y' `
        $recordingFramePath 2>&1)
    $ffmpegExitCode = $LASTEXITCODE
    $ffmpegText = ($ffmpegOutput | ForEach-Object { [string]$_ }) -join [Environment]::NewLine
    [IO.File]::WriteAllText(
        (Join-Path $resolvedOutput 'recording-ffmpeg.log'),
        $ffmpegText + [Environment]::NewLine,
        $utf8NoBom)
    Assert-True ($ffmpegExitCode -eq 0 -and
        (Test-Path -LiteralPath $recordingFramePath -PathType Leaf)) `
        "FFmpeg could not decode the OBS recording frame: $ffmpegText"

    Write-JsonFile -Path $recordingManifestPath -Value ([ordered]@{
        schemaVersion = 1
        contract = $outputContract
        backgroundRgb = @(96, 96, 96)
        rawFrame = [ordered]@{
            path = Split-Path -Leaf $activeFrame
            width = $frameWidth
            height = $frameHeight
            format = $frameFormat
        }
        referenceImage = $recordingReferenceName
        decodedFrame = $recordingFrameName
        video = Split-Path -Leaf $recordingPath
        recording = [ordered]@{
            durationMilliseconds = [int64]$recordStatus.outputDuration
            codec = [string]$videoStreams[0].codec_name
            pixelFormat = [string]$videoStreams[0].pix_fmt
        }
        tools = [ordered]@{
            ffmpegPath = $ffmpegPath
            ffmpegSha256 = (Get-FileHash -LiteralPath $ffmpegPath -Algorithm SHA256).Hash
            ffprobePath = $ffprobePath
            ffprobeSha256 = (Get-FileHash -LiteralPath $ffprobePath -Algorithm SHA256).Hash
        }
    })
    & $pythonPath `
        @pythonPrefix `
        '-B' `
        $recordingVerifier `
        $recordingManifestPath `
        '--report' $recordingVerificationReport `
        1> (Join-Path $resolvedOutput 'recording-verifier.stdout.log') `
        2> (Join-Path $resolvedOutput 'recording-verifier.stderr.log')
    Assert-True ($LASTEXITCODE -eq 0) `
        "OBS recording verifier failed with exit code $LASTEXITCODE."
    Assert-True (Test-Path -LiteralPath $recordingVerificationReport -PathType Leaf) `
        'OBS recording verifier did not write its report.'
    $recordingContract = Read-JsonFile -Path $recordingVerificationReport
    Assert-True ([string]$recordingContract.status -eq 'passed') `
        'OBS recording verification report is not passed.'

    & $resolvedProbe `
        "--sender=$senderName" `
        '--duration-ms=2000' `
        '--interval-ms=100' `
        "--capture-output=$activeFrameAfter" `
        "--output=$probeAfterJson" `
        1> (Join-Path $resolvedOutput 'receiver-after.stdout.log') `
        2> (Join-Path $resolvedOutput 'receiver-after.stderr.log')
    Assert-True ($LASTEXITCODE -eq 0) `
        "Post-capture receiver probe failed with exit code $LASTEXITCODE."
    $probeAfter = Read-JsonFile -Path $probeAfterJson
    Assert-True ([int]$probeAfter.schemaVersion -eq 2 -and
        $null -ne $probeAfter.capturedFrame) `
        'Post-capture receiver probe did not preserve an active frame.'
    Assert-True ([int]$probeAfter.capturedFrame.width -eq $frameWidth -and
        [int]$probeAfter.capturedFrame.height -eq $frameHeight -and
        [int]$probeAfter.capturedFrame.format -eq $frameFormat) `
        'Post-capture receiver frame contract changed.'
    $connectedAfter = @($probeAfter.samples | Where-Object { [bool]$_.connected })
    $handlesAfter = @($connectedAfter | ForEach-Object { [string]$_.handle } |
        Sort-Object -Unique)
    Assert-True ($handlesAfter.Count -eq 1 -and
        $handlesAfter[0] -eq $senderHandles[0]) `
        'Spout2 shared handle changed during OBS capture.'
    Assert-True (Test-Path -LiteralPath $activeFrameAfter -PathType Leaf) `
        'Post-capture receiver raw BGRA evidence is missing.'
    $rawFrameHash = (Get-FileHash -LiteralPath $activeFrame -Algorithm SHA256).Hash
    $rawFrameAfterHash = (Get-FileHash -LiteralPath $activeFrameAfter -Algorithm SHA256).Hash
    Assert-True ($rawFrameHash -eq $rawFrameAfterHash) `
        'Fixed-age Spout2 frame changed while OBS captured the four backgrounds.'

    Write-JsonFile -Path $manifestPath -Value ([ordered]@{
        schemaVersion = 1
        contract = $outputContract
        obsBlendMethod = 'default'
        obsBlendMode = 'normal'
        rawFrame = [ordered]@{
            path = Split-Path -Leaf $activeFrame
            width = $frameWidth
            height = $frameHeight
            format = $frameFormat
        }
        frameStability = [ordered]@{
            beforePath = Split-Path -Leaf $activeFrame
            afterPath = Split-Path -Leaf $activeFrameAfter
            sha256 = $rawFrameHash
        }
        cases = @($manifestCases)
    })

    & $pythonPath `
        @pythonPrefix `
        '-B' `
        $verifier `
        $manifestPath `
        '--report' $verificationReport `
        '--tolerance=3' `
        1> (Join-Path $resolvedOutput 'verifier.stdout.log') `
        2> (Join-Path $resolvedOutput 'verifier.stderr.log')
    Assert-True ($LASTEXITCODE -eq 0) `
        "OBS composite verifier failed with exit code $LASTEXITCODE."
    Assert-True (Test-Path -LiteralPath $verificationReport -PathType Leaf) `
        'OBS composite verifier did not write its report.'
    $verification = Read-JsonFile -Path $verificationReport
    Assert-True ([string]$verification.status -eq 'passed') `
        'OBS composite verification report is not passed.'
    $mainPassed = $true
}
catch
{
    $mainError = $_.Exception.Message
}
finally
{
    if ($null -ne $obsProcess -and -not $obsProcess.HasExited)
    {
        try
        {
            $cleanupRecordStatus = Invoke-ObsRequest -Request 'GetRecordStatus'
            if ([bool]$cleanupRecordStatus.outputActive)
            {
                Invoke-ObsRequest -Request 'StopRecord' | Out-Null
            }
            $cleanupRecordingStopped = $false
            for ($attempt = 0; $attempt -lt 40; ++$attempt)
            {
                $cleanupRecordStatus = Invoke-ObsRequest -Request 'GetRecordStatus'
                if (-not [bool]$cleanupRecordStatus.outputActive)
                {
                    $cleanupRecordingStopped = $true
                    break
                }
                Start-Sleep -Milliseconds 250
            }
            Assert-True $cleanupRecordingStopped `
                'OBS recording remained active during cleanup.'
            $recordingStarted = $false
        }
        catch
        {
            $cleanupErrors.Add("Could not stop OBS recording before profile restore: $($_.Exception.Message)")
        }
        if ($testCollectionCreated -and -not [string]::IsNullOrWhiteSpace($originalCollection))
        {
            Invoke-CleanupRequest -Request 'SetCurrentSceneCollection' -Data ([ordered]@{
                sceneCollectionName = $originalCollection
            })
        }
        if ($testCollectionCreated -and -not [string]::IsNullOrWhiteSpace($originalScene))
        {
            Invoke-CleanupRequest -Request 'SetCurrentProgramScene' -Data ([ordered]@{
                sceneName = $originalScene
            })
        }
        if ($testProfileCreated -and -not [string]::IsNullOrWhiteSpace($originalProfile))
        {
            Invoke-CleanupRequest -Request 'SetCurrentProfile' -Data ([ordered]@{
                profileName = $originalProfile
            })
        }

        try
        {
            $obsProcess.Refresh()
            $closeRequested = $obsProcess.CloseMainWindow()
            if (-not $closeRequested -or -not $obsProcess.WaitForExit(20000))
            {
                # The exact spawned PID is the only process force-terminated.
                $obsForcedTermination = $true
                $obsProcess.Kill()
                $obsProcess.WaitForExit()
            }
        }
        catch
        {
            $cleanupErrors.Add("Could not stop the spawned OBS process: $($_.Exception.Message)")
        }
    }
    if ($obsForcedTermination)
    {
        $cleanupErrors.Add('OBS required forced termination; graceful scene serialization was not proven.')
    }

    if ($obsStartedAtUtc -ne [DateTime]::MinValue)
    {
        try
        {
            $obsLogPath = Join-Path $resolvedOutput 'obs-session.log'
            Copy-ObsSessionLog `
                -ConfigRoot $configRoot `
                -StartedAtUtc $obsStartedAtUtc `
                -Destination $obsLogPath
            if ($spoutInputCreated -and $frameWidth -gt 0 -and $frameHeight -gt 0)
            {
                $obsLogContract = Get-ObsSessionLogContract `
                    -Path $obsLogPath `
                    -SpoutInputName $spoutInput `
                    -Width $frameWidth `
                    -Height $frameHeight
                Write-JsonFile `
                    -Path (Join-Path $resolvedOutput 'obs-log-verification.json') `
                    -Value $obsLogContract
            }
        }
        catch
        {
            $postShutdownErrors.Add($_.Exception.Message)
        }
    }
    if ($spoutInputCreated)
    {
        try
        {
            $sceneContract = Get-TemporarySceneCollectionContract `
                -ScenesRoot $scenesRoot `
                -CollectionName $testCollection `
                -SceneName $testScene `
                -SpoutInputName $spoutInput `
                -EvidencePath (Join-Path $resolvedOutput 'temporary-scene-collection.json')
            Write-JsonFile `
                -Path (Join-Path $resolvedOutput 'obs-saved-scene-contract.json') `
                -Value $sceneContract
        }
        catch
        {
            $postShutdownErrors.Add($_.Exception.Message)
        }
    }

    if ($null -ne $hostProcess -and -not $hostProcess.HasExited)
    {
        try
        {
            # Only terminate the Host PID created by this bounded acceptance run.
            $hostProcess.Kill()
            $hostProcess.WaitForExit()
        }
        catch
        {
            $cleanupErrors.Add("Could not stop the spawned Host process: $($_.Exception.Message)")
        }
    }

    $obsStoppedForRestore = $true
    try
    {
        if ($null -ne $obsProcess)
        {
            $obsProcess.Refresh()
            $obsStoppedForRestore = $obsProcess.HasExited
        }
        $remainingObs = @(Get-Process -Name $obsProcessName -ErrorAction SilentlyContinue)
        if ($remainingObs.Count -ne 0)
        {
            $obsStoppedForRestore = $false
        }
    }
    catch
    {
        $obsStoppedForRestore = $false
        $cleanupErrors.Add("Could not prove OBS stopped before restoration: $($_.Exception.Message)")
    }
    if (-not $obsStoppedForRestore)
    {
        $cleanupErrors.Add(
            'OBS is still running; configuration was not restored to avoid a write race.')
    }

    if ($recordingWorkReady -and $obsStoppedForRestore)
    {
        try
        {
            if (Test-Path -LiteralPath $recordingWorkRoot -PathType Container)
            {
                Remove-GeneratedRecordingDirectory -Path $recordingWorkRoot
            }
            $recordingWorkRemoved = -not (Test-Path -LiteralPath $recordingWorkRoot)
            Assert-True $recordingWorkRemoved `
                'Temporary raw OBS recording directory was not removed.'
        }
        catch
        {
            $cleanupErrors.Add("Could not remove temporary raw OBS recording: $($_.Exception.Message)")
        }
    }
    elseif ($recordingWorkReady)
    {
        $cleanupErrors.Add(
            "Raw OBS recording remains in temporary storage: $recordingWorkRoot")
    }

    if ($backupReady -and $obsStoppedForRestore)
    {
        try
        {
            $afterState = Restore-ManagedState `
                -ConfigRoot $configRoot `
                -Expected $beforeState `
                -BackupRoot $backupRoot `
                -ManagedDirectories $managedDirectories `
                -ManagedFiles $managedFiles
            $auditAfterState = Get-ManagedState `
                -ConfigRoot $configRoot `
                -ManagedDirectories $auditDirectories `
                -ManagedFiles $auditFiles
            Write-JsonFile `
                -Path (Join-Path $resolvedOutput 'external-state-after.json') `
                -Value (ConvertTo-EvidenceState -ConfigRoot $configRoot -State $auditAfterState)
            Assert-StatesEqual -Expected $auditBeforeState -Actual $auditAfterState
            $externalRestoreExact = $true
        }
        catch
        {
            $cleanupErrors.Add("OBS configuration restoration failed: $($_.Exception.Message)")
        }
    }
    if ($externalRestoreExact -and (Test-Path -LiteralPath $backupRoot -PathType Container))
    {
        try
        {
            Remove-BackupDirectory -Path $backupRoot
        }
        catch
        {
            $cleanupErrors.Add("Could not remove the verified temporary backup: $($_.Exception.Message)")
        }
    }

    try
    {
        Write-JsonFile `
            -Path (Join-Path $resolvedOutput 'obs-websocket-transcript.json') `
            -Value ([ordered]@{
                schemaVersion = 1
                requests = @($requestTranscript)
            })
    }
    catch
    {
        $cleanupErrors.Add("Could not write the OBS request transcript: $($_.Exception.Message)")
    }
}

$passed = $mainPassed -and
    $null -eq $mainError -and
    $cleanupErrors.Count -eq 0 -and
    $postShutdownErrors.Count -eq 0 -and
    $externalRestoreExact -and
    $recordingWorkRemoved -and
    $null -ne $sceneContract -and
    $null -ne $obsLogContract -and
    $null -ne $recordingContract
$summary = [ordered]@{
    schemaVersion = 1
    status = if ($passed) { 'passed' } else { 'failed' }
    contract = $outputContract
    sender = $senderName
    fixedAgeMilliseconds = 130
    host = [ordered]@{
        path = $resolvedHost
        sha256 = (Get-FileHash -LiteralPath $resolvedHost -Algorithm SHA256).Hash
    }
    receiverProbe = [ordered]@{
        path = $resolvedProbe
        sha256 = (Get-FileHash -LiteralPath $resolvedProbe -Algorithm SHA256).Hash
    }
    obs = [ordered]@{
        path = $resolvedObs
        sha256 = (Get-FileHash -LiteralPath $resolvedObs -Algorithm SHA256).Hash
        portable = [bool]$obsConfig.Portable
        forcedTermination = $obsForcedTermination
    }
    isolation = [ordered]@{
        temporaryProfile = $testProfile
        temporarySceneCollection = $testCollection
        temporaryScene = $testScene
        userSceneModifiedByTest = $false
        displayOrWindowCapturePresent = $false
    }
    composition = [ordered]@{
        sourceCompositeMode = 4
        sceneItemBlendMode = 'OBS_BLEND_NORMAL'
        sceneItemBlendMethod = if ($null -ne $sceneContract) { 'default' } else { $null }
        sourceSrgbAware = $false
        expectedBlendDomain = 'srgb-byte'
        backgrounds = @('black', 'gray', 'white', 'color')
    }
    recording = [ordered]@{
        verified = $null -ne $recordingContract
        video = if ([string]::IsNullOrWhiteSpace([string]$recordingPath)) {
            $null
        } else {
            Split-Path -Leaf $recordingPath
        }
        comparison = 'decoded-recording-frame-vs-obs-scene-png'
        finalEvidenceAudioStreams = 0
        temporaryRawContainerRemoved = $recordingWorkRemoved
        effectBrightnessEnergyRatio = if ($null -ne $recordingContract) {
            $recordingContract.metrics.effectBrightnessEnergyRatio
        } else {
            $null
        }
        darkenedBrightPixelFraction = if ($null -ne $recordingContract) {
            $recordingContract.metrics.darkenedBrightPixelFraction
        } else {
            $null
        }
    }
    obsLogVerified = $null -ne $obsLogContract
    externalConfigurationRestoreExact = $externalRestoreExact
    mainError = $mainError
    postShutdownErrors = @($postShutdownErrors)
    cleanupErrors = @($cleanupErrors)
    retainedRecoveryBackup = if ($externalRestoreExact) { $null } else { $backupRoot }
    retainedRawRecordingDirectory = if ($recordingWorkRemoved) {
        $null
    } else {
        $recordingWorkRoot
    }
    completedAtUtc = [DateTime]::UtcNow.ToString('o')
}
Write-JsonFile -Path (Join-Path $resolvedOutput 'acceptance-summary.json') -Value $summary
Write-EvidenceHashes -EvidenceRoot $resolvedOutput

if (-not $passed)
{
    $messages = [Collections.Generic.List[string]]::new()
    if ($null -ne $mainError)
    {
        $messages.Add($mainError)
    }
    foreach ($errorMessage in @($postShutdownErrors))
    {
        $messages.Add($errorMessage)
    }
    foreach ($errorMessage in @($cleanupErrors))
    {
        $messages.Add($errorMessage)
    }
    throw "OBS Spout2 composite acceptance failed: $($messages -join ' | ')"
}

Write-Output "OBS Spout2 composite acceptance passed: $resolvedOutput"
