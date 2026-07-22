[CmdletBinding()]
param(
  [string]$Source = (Join-Path $PSScriptRoot '..\assets\listopad-plus-plus.png'),
  [string]$Destination = (Join-Path $PSScriptRoot '..\assets\icons')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$Source = [IO.Path]::GetFullPath($Source)
$Destination = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
$master = [Drawing.Bitmap]::new($Source)

function Get-ScaledPngBytes([int]$Size) {
  $bitmap = [Drawing.Bitmap]::new($Size, $Size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $graphics = [Drawing.Graphics]::FromImage($bitmap)
  $stream = [IO.MemoryStream]::new()
  try {
    $graphics.Clear([Drawing.Color]::Transparent)
    $graphics.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
    $graphics.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
    $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.DrawImage($master, 0, 0, $Size, $Size)
    if ($bitmap.GetPixel(0, 0).A -ne 0) { throw "Scaled icon is not transparent at $Size px." }
    $bitmap.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
    return ,$stream.ToArray()
  } finally {
    $stream.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
  }
}

function Write-IcoFile([string]$OutputPath, [int[]]$Sizes) {
  $images = foreach ($size in $Sizes) {
    [pscustomobject]@{ Size = $size; Data = [byte[]](Get-ScaledPngBytes $size) }
  }
  $stream = [IO.File]::Create($OutputPath)
  $writer = [IO.BinaryWriter]::new($stream)
  try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$images.Count)
    $offset = 6 + 16 * $images.Count
    foreach ($image in $images) {
      $dimension = if ($image.Size -ge 256) { [byte]0 } else { [byte]$image.Size }
      $writer.Write($dimension)
      $writer.Write($dimension)
      $writer.Write([byte]0)
      $writer.Write([byte]0)
      $writer.Write([uint16]1)
      $writer.Write([uint16]32)
      $writer.Write([uint32]$image.Data.Length)
      $writer.Write([uint32]$offset)
      $offset += $image.Data.Length
    }
    foreach ($image in $images) { $writer.Write([byte[]]$image.Data) }
  } finally {
    $writer.Dispose()
    $stream.Dispose()
  }
}

try {
  foreach ($size in 16, 24, 32, 64) {
    Write-IcoFile (Join-Path $Destination "listopad-plus-plus-${size}x${size}.ico") @($size)
  }
  Write-IcoFile (Join-Path $Destination 'listopad-plus-plus-multisize.ico') @(16, 24, 32, 64, 256)
} finally {
  $master.Dispose()
}
