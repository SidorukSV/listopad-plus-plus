[CmdletBinding()]
param([Parameter(Mandatory)][string]$Destination)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$Destination = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
$iconPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\assets\icons\listopad-plus-plus-64x64.ico'))
$icon = [Drawing.Icon]::new($iconPath)
$source = $icon.ToBitmap()
$bitmap = [Drawing.Bitmap]::new(150, 150)
$graphics = [Drawing.Graphics]::FromImage($bitmap)
try {
  $graphics.Clear([Drawing.Color]::Transparent)
  $graphics.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
  $graphics.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
  $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
  $graphics.DrawImage($source, 0, 0, 150, 150)
  $bitmap.Save((Join-Path $Destination 'ListopadPP.png'), [Drawing.Imaging.ImageFormat]::Png)
} finally {
  $graphics.Dispose()
  $bitmap.Dispose()
  $source.Dispose()
  $icon.Dispose()
}
