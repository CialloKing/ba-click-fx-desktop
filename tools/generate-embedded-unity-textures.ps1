[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $UnityProjectRoot,

    [string] $OutputPath = (Join-Path $PSScriptRoot '..\src\windows\src\generated_unity_texture_data.inc')
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

function Get-Sha256Hex
{
    param(
        [Parameter(Mandatory = $true)]
        [byte[]] $Bytes
    )

    $algorithm = [Security.Cryptography.SHA256]::Create()
    try
    {
        return (($algorithm.ComputeHash($Bytes) | ForEach-Object {
            $_.ToString('x2')
        }) -join '')
    }
    finally
    {
        $algorithm.Dispose()
    }
}

function New-TrailCoveragePng
{
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    $expectedSourceSha256 =
        '16001511757e7007f43db9613e24144b5e8d726239de0262f55d9e14c0f00feb'
    $expectedCoverageSha256 =
        '172b0c1b69a4fca13fcbde72c5071c8e4e7e1459dba13042818f399e5a697216'
    [byte[]] $sourceBytes = [IO.File]::ReadAllBytes($Path)
    $actualSourceSha256 = Get-Sha256Hex -Bytes $sourceBytes
    if ($actualSourceSha256 -ne $expectedSourceSha256)
    {
        throw "Trail source PNG SHA256 mismatch: $actualSourceSha256"
    }

    $source = [Drawing.Bitmap]::new($Path)
    try
    {
        if ($source.Width -ne 512 -or $source.Height -ne 512)
        {
            throw "Trail source dimensions must be 512x512: $($source.Width)x$($source.Height)"
        }

        $bounds = [Drawing.Rectangle]::new(0, 0, $source.Width, $source.Height)
        [Drawing.Bitmap] $bitmap = $source.Clone(
            $bounds,
            [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try
        {
            $bitmapData = $bitmap.LockBits(
                $bounds,
                [Drawing.Imaging.ImageLockMode]::ReadWrite,
                [Drawing.Imaging.PixelFormat]::Format32bppArgb)
            try
            {
                if ($bitmapData.Stride -le 0)
                {
                    throw 'Trail texture must use a top-down positive bitmap stride.'
                }

                $pixelByteCount = $bitmapData.Stride * $bitmap.Height
                [byte[]] $pixels = [byte[]]::new($pixelByteCount)
                [Runtime.InteropServices.Marshal]::Copy(
                    $bitmapData.Scan0,
                    $pixels,
                    0,
                    $pixelByteCount)
                [byte[]] $coverage = [byte[]]::new($bitmap.Width * $bitmap.Height)

                for ($x = 0; $x -lt $bitmap.Width; $x++)
                {
                    $columnPeak = 0
                    for ($y = 0; $y -lt $bitmap.Height; $y++)
                    {
                        $offset = $y * $bitmapData.Stride + $x * 4
                        $columnPeak = [Math]::Max(
                            $columnPeak,
                            [Math]::Max(
                                $pixels[$offset],
                                [Math]::Max($pixels[$offset + 1], $pixels[$offset + 2])))
                    }

                    for ($y = 0; $y -lt $bitmap.Height; $y++)
                    {
                        $offset = $y * $bitmapData.Stride + $x * 4
                        $support = [Math]::Max(
                            $pixels[$offset],
                            [Math]::Max($pixels[$offset + 1], $pixels[$offset + 2]))
                        $coverageValue = if ($columnPeak -eq 0)
                        {
                            0
                        }
                        else
                        {
                            # Match JavaScript Math.round for the positive byte domain.
                            [Math]::Floor($support / $columnPeak * 255.0 + 0.5)
                        }
                        $coverage[$y * $bitmap.Width + $x] = [byte] $coverageValue
                        $pixels[$offset + 3] = [byte] $coverageValue
                    }
                }

                $actualCoverageSha256 = Get-Sha256Hex -Bytes $coverage
                if ($actualCoverageSha256 -ne $expectedCoverageSha256)
                {
                    throw "Trail Coverage SHA256 mismatch: $actualCoverageSha256"
                }
                [Runtime.InteropServices.Marshal]::Copy(
                    $pixels,
                    0,
                    $bitmapData.Scan0,
                    $pixelByteCount)
            }
            finally
            {
                $bitmap.UnlockBits($bitmapData)
            }

            $stream = [IO.MemoryStream]::new()
            try
            {
                $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
                return $stream.ToArray()
            }
            finally
            {
                $stream.Dispose()
            }
        }
        finally
        {
            $bitmap.Dispose()
        }
    }
    finally
    {
        $source.Dispose()
    }
}

function Add-EmbeddedPng
{
    param(
        [Parameter(Mandatory = $true)]
        [Text.StringBuilder] $Builder,

        [Parameter(Mandatory = $true)]
        [string] $Name,

        [Parameter(Mandatory = $true)]
        [byte[]] $Bytes
    )

    $base64 = [Convert]::ToBase64String($Bytes)
    [void] $Builder.AppendLine("inline constexpr std::string_view ${Name}PngBase64 = R`"bafx(")
    for ($offset = 0; $offset -lt $base64.Length; $offset += 100)
    {
        $length = [Math]::Min(100, $base64.Length - $offset)
        [void] $Builder.AppendLine($base64.Substring($offset, $length))
    }
    [void] $Builder.AppendLine(')bafx";')
    [void] $Builder.AppendLine("inline constexpr std::size_t ${Name}PngByteCount = $($Bytes.Length)U;")
}

$textureDirectory = Join-Path $UnityProjectRoot 'Assets\Imported\FX_Touch\Textures'
$textures = @(
    @{ Name = 'circle01'; File = 'FX_TEX_Circle_01.png' },
    @{ Name = 'gradRing3'; File = 'FX_TEX_Grad_Ring3.png' },
    @{ Name = 'triangle02_1'; File = 'FX_TEX_Triangle_02_1.png' },
    @{ Name = 'trail03'; File = 'FX_TEX_Trail_03.png' }
)

$builder = [System.Text.StringBuilder]::new()
[void] $builder.AppendLine('// Generated by tools/generate-embedded-unity-textures.ps1. Do not edit by hand.')

foreach ($texture in $textures)
{
    $path = Join-Path $textureDirectory $texture.File
    if (-not (Test-Path -LiteralPath $path -PathType Leaf))
    {
        throw "Missing Unity texture: $path"
    }

    [byte[]] $bytes = [System.IO.File]::ReadAllBytes($path)
    Add-EmbeddedPng -Builder $builder -Name $texture.Name -Bytes $bytes
}

$trailPath = Join-Path $textureDirectory 'FX_TEX_Trail_03.png'
[byte[]] $trailCoverageBytes = New-TrailCoveragePng -Path $trailPath
Add-EmbeddedPng `
    -Builder $builder `
    -Name 'trail03Coverage' `
    -Bytes $trailCoverageBytes

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($resolvedOutput)) | Out-Null
[System.IO.File]::WriteAllText(
    $resolvedOutput,
    $builder.ToString(),
    [System.Text.UTF8Encoding]::new($false))

Write-Host "Generated $resolvedOutput from $($textures.Count) Unity textures and Trail Coverage."
