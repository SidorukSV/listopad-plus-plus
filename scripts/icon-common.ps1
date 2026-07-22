# Shared source preparation for every icon and Store asset the project ships.
#
# The authored artwork sits in a 1024x1024 canvas but only fills roughly 75% of
# it, and is off-centre by ~90px horizontally. Rendered straight into a 16x16 or
# 32x32 frame that padding survives proportionally, so the taskbar button looks
# noticeably smaller than neighbouring apps. Trimming to the drawn pixels and
# re-centring at a fixed fill ratio makes the glyph match the platform norm.

Add-Type -AssemblyName System.Drawing

function Get-ContentBounds([Drawing.Bitmap]$Image, [int]$AlphaThreshold = 8) {
  $w = $Image.Width; $h = $Image.Height
  $rect = [Drawing.Rectangle]::new(0, 0, $w, $h)
  $data = $Image.LockBits($rect, [Drawing.Imaging.ImageLockMode]::ReadOnly,
                          [Drawing.Imaging.PixelFormat]::Format32bppArgb)
  try {
    $bytes = New-Object byte[] ($data.Stride * $h)
    [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
  } finally {
    $Image.UnlockBits($data)
  }
  $minX = $w; $minY = $h; $maxX = -1; $maxY = -1
  for ($y = 0; $y -lt $h; $y++) {
    $row = $y * $data.Stride
    for ($x = 0; $x -lt $w; $x++) {
      if ($bytes[$row + $x * 4 + 3] -gt $AlphaThreshold) {
        if ($x -lt $minX) { $minX = $x }
        if ($x -gt $maxX) { $maxX = $x }
        if ($y -lt $minY) { $minY = $y }
        if ($y -gt $maxY) { $maxY = $y }
      }
    }
  }
  if ($maxX -lt 0) { throw "Icon source is fully transparent: no drawn pixels found." }
  return [Drawing.Rectangle]::new($minX, $minY, $maxX - $minX + 1, $maxY - $minY + 1)
}

# Returns a square 32bpp bitmap whose artwork is trimmed, centred and scaled so
# that its longest side covers $Fill of the canvas. Caller owns the result.
# Full bleed matches how neighbouring taskbar icons are drawn; anything less
# reads as visibly undersized next to them.
function Get-NormalizedIconSource([string]$Path, [double]$Fill = 1.0) {
  $original = [Drawing.Bitmap]::new($Path)
  try {
    $bounds = Get-ContentBounds $original
    $longest = [Math]::Max($bounds.Width, $bounds.Height)
    $canvas = [int][Math]::Round($longest / $Fill)
    $scale = ($canvas * $Fill) / $longest
    $drawWidth = [int][Math]::Round($bounds.Width * $scale)
    $drawHeight = [int][Math]::Round($bounds.Height * $scale)
    $offsetX = [int][Math]::Round(($canvas - $drawWidth) / 2.0)
    $offsetY = [int][Math]::Round(($canvas - $drawHeight) / 2.0)

    $result = [Drawing.Bitmap]::new($canvas, $canvas, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [Drawing.Graphics]::FromImage($result)
    try {
      $graphics.Clear([Drawing.Color]::Transparent)
      $graphics.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
      $graphics.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
      $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
      $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
      $graphics.DrawImage($original,
                          [Drawing.Rectangle]::new($offsetX, $offsetY, $drawWidth, $drawHeight),
                          $bounds, [Drawing.GraphicsUnit]::Pixel)
    } finally {
      $graphics.Dispose()
    }
    return $result
  } finally {
    $original.Dispose()
  }
}
