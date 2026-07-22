[CmdletBinding()]
param([Parameter(Mandatory)][string]$Destination)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$Destination = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Force -Path $Destination | Out-Null

$iconDirectory = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\assets\icons'))
$sourceIcons = @{
  16 = Join-Path $iconDirectory 'listopad-plus-plus-16x16.ico'
  24 = Join-Path $iconDirectory 'listopad-plus-plus-24x24.ico'
  32 = Join-Path $iconDirectory 'listopad-plus-plus-32x32.ico'
  64 = Join-Path $iconDirectory 'listopad-plus-plus-64x64.ico'
}

function Get-SourceIcon([int]$Size) {
  if ($Size -le 16) { return $sourceIcons[16] }
  if ($Size -le 24) { return $sourceIcons[24] }
  if ($Size -le 32) { return $sourceIcons[32] }
  return $sourceIcons[64]
}

function Write-TransparentPng([string]$IconPath, [int]$Size, [string]$OutputPath) {
  $icon = [Drawing.Icon]::new($IconPath)
  $source = $icon.ToBitmap()
  $bitmap = [Drawing.Bitmap]::new($Size, $Size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $graphics = [Drawing.Graphics]::FromImage($bitmap)
  try {
    $graphics.Clear([Drawing.Color]::Transparent)
    $graphics.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
    $graphics.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
    $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.DrawImage($source, 0, 0, $Size, $Size)
    $bitmap.Save($OutputPath, [Drawing.Imaging.ImageFormat]::Png)
    if ($bitmap.GetPixel(0, 0).A -ne 0) { throw "Generated asset is not transparent: $OutputPath" }
  } finally {
    $graphics.Dispose()
    $bitmap.Dispose()
    $source.Dispose()
    $icon.Dispose()
  }
}

Write-TransparentPng $sourceIcons[64] 150 (Join-Path $Destination 'ListopadPP.png')

# Windows applies an accent-coloured backplate on Taskbar and Start when the
# target-size unplated variants of Square44x44Logo are absent. The same
# full-colour transparent artwork is suitable for both shell themes.
$targetSizes = 16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 256
foreach ($size in $targetSizes) {
  $default = Join-Path $Destination "ListopadPP.targetsize-$size.png"
  Write-TransparentPng (Get-SourceIcon $size) $size $default
  Copy-Item $default (Join-Path $Destination "ListopadPP.targetsize-${size}_altform-unplated.png") -Force
  Copy-Item $default (Join-Path $Destination "ListopadPP.targetsize-${size}_altform-lightunplated.png") -Force
}
