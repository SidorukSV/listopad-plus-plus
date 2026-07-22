[CmdletBinding()]
param(
  [string]$Source = (Join-Path $PSScriptRoot '..\assets\listopad-plus-plus.png'),
  [string]$Destination = (Join-Path $PSScriptRoot '..\assets\icons')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
. (Join-Path $PSScriptRoot 'icon-common.ps1')

$Source = [IO.Path]::GetFullPath($Source)
$Destination = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Force -Path $Destination | Out-Null

function New-PngFrame([Drawing.Bitmap]$Image, [int]$Size) {
  $bitmap = [Drawing.Bitmap]::new($Size, $Size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $graphics = [Drawing.Graphics]::FromImage($bitmap)
  try {
    $graphics.Clear([Drawing.Color]::Transparent)
    $graphics.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
    $graphics.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
    $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.DrawImage($Image, 0, 0, $Size, $Size)

    $stream = [IO.MemoryStream]::new()
    $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
    return $stream.ToArray()
  } finally {
    $graphics.Dispose()
    $bitmap.Dispose()
  }
}

function Write-Ico([Drawing.Bitmap]$Image, [int[]]$Sizes, [string]$OutputPath) {
  $frames = [Collections.Generic.List[byte[]]]::new()
  foreach ($size in $Sizes) {
    [byte[]]$frame = New-PngFrame $Image $size
    $frames.Add($frame)
  }
  $stream = [IO.File]::Open($OutputPath, [IO.FileMode]::Create, [IO.FileAccess]::Write)
  $writer = [IO.BinaryWriter]::new($stream)
  try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$Sizes.Count)

    $offset = 6 + (16 * $Sizes.Count)
    for ($index = 0; $index -lt $Sizes.Count; $index++) {
      $size = $Sizes[$index]
      $writer.Write([byte]$(if ($size -eq 256) { 0 } else { $size }))
      $writer.Write([byte]$(if ($size -eq 256) { 0 } else { $size }))
      $writer.Write([byte]0)
      $writer.Write([byte]0)
      $writer.Write([uint16]1)
      $writer.Write([uint16]32)
      $writer.Write([uint32]$frames[$index].Length)
      $writer.Write([uint32]$offset)
      $offset += $frames[$index].Length
    }

    foreach ($frame in $frames) { $writer.Write($frame) }
  } finally {
    $writer.Dispose()
    $stream.Dispose()
  }
}

$sourceImage = Get-NormalizedIconSource $Source
try {
  foreach ($size in 16, 24, 32, 64) {
    Write-Ico $sourceImage @($size) (Join-Path $Destination "listopad-plus-plus-${size}x${size}.ico")
  }
  Write-Ico $sourceImage @(16, 20, 24, 32, 40, 48, 64, 128, 256) `
    (Join-Path $Destination 'listopad-plus-plus-multisize.ico')
} finally {
  $sourceImage.Dispose()
}
