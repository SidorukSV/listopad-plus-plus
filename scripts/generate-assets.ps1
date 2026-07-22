[CmdletBinding()]
param([Parameter(Mandatory)][string]$Destination)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$Destination = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
$sourcePath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\assets\listopad-plus-plus.png'))
. (Join-Path $PSScriptRoot 'icon-common.ps1')
$source = Get-NormalizedIconSource $sourcePath

function Write-TransparentPng([int]$Size, [string]$OutputPath) {
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
  }
}

try {
  Write-TransparentPng 50 (Join-Path $Destination 'ListopadPP-StoreLogo.png')
  Write-TransparentPng 150 (Join-Path $Destination 'ListopadPP-Square150x150Logo.png')
  Write-TransparentPng 44 (Join-Path $Destination 'ListopadPP-Square44x44Logo.png')

  $targetSizes = 16, 20, 24, 30, 32, 36, 40, 44, 48, 60, 64, 72, 80, 96, 256
  foreach ($size in $targetSizes) {
    $default = Join-Path $Destination "ListopadPP-Square44x44Logo.targetsize-$size.png"
    Write-TransparentPng $size $default
    Copy-Item $default (Join-Path $Destination "ListopadPP-Square44x44Logo.targetsize-${size}_altform-unplated.png") -Force
    Copy-Item $default (Join-Path $Destination "ListopadPP-Square44x44Logo.targetsize-${size}_altform-lightunplated.png") -Force
  }
} finally {
  $source.Dispose()
}
