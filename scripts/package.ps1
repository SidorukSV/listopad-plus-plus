[CmdletBinding()]
param(
  [string]$Version = '0.2.1',
  [string]$PfxPath,
  [securestring]$PfxPassword,
  [string]$SignCommand,
  [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$out = Join-Path $repo 'out'
$stage = Join-Path $out 'stage'
$build = Join-Path $repo 'build\release'

if ($Version -notmatch '^\d+\.\d+\.\d+$') {
  throw "Version must have the form MAJOR.MINOR.PATCH; got '$Version'."
}

# Remove outputs produced by the retired sparse-package pipeline so a local
# packaging run cannot look as if it still emits MSIX artifacts.
foreach ($obsolete in (Join-Path $out 'identity'), (Join-Path $out 'shellext'),
                      (Join-Path $out 'ListopadPP.Identity.msix')) {
  if (Test-Path -LiteralPath $obsolete) {
    Remove-Item -LiteralPath $obsolete -Recurse -Force
  }
}

function Reset-ChildDirectory([string]$Path) {
  $resolvedParent = [IO.Path]::GetFullPath((Split-Path $Path))
  if ($resolvedParent -ne [IO.Path]::GetFullPath($out)) { throw "Refusing to reset path outside $out" }
  if (Test-Path $Path) { Remove-Item -LiteralPath $Path -Recurse -Force }
  New-Item -ItemType Directory -Force -Path $Path | Out-Null
}

function Find-LockedFile([string]$Path) {
  if (-not (Test-Path $Path)) { return $null }
  foreach ($file in Get-ChildItem -LiteralPath $Path -Recurse -File) {
    $stream = $null
    try {
      $stream = [IO.File]::Open($file.FullName, [IO.FileMode]::Open,
                                [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    } catch {
      return $file.FullName
    } finally {
      if ($null -ne $stream) { $stream.Dispose() }
    }
  }
  return $null
}

# A major upgrade needs a new ProductCode for every released version, while a
# rebuild of that same version must keep the same code. Derive an RFC 4122 v5
# UUID from the permanent UpgradeCode namespace and the version instead of
# letting WiX generate a fresh ProductCode on every invocation.
function Get-ProductCode([string]$ProductVersion) {
  $namespace = [guid]'3D765D12-AE8D-4C77-9F39-C5CBF4884FD9'
  $namespaceBytes = $namespace.ToByteArray()
  [Array]::Reverse($namespaceBytes, 0, 4)
  [Array]::Reverse($namespaceBytes, 4, 2)
  [Array]::Reverse($namespaceBytes, 6, 2)

  $nameBytes = [Text.Encoding]::UTF8.GetBytes("ListopadPP/windows-x64/$ProductVersion")
  $inputBytes = [byte[]]::new($namespaceBytes.Length + $nameBytes.Length)
  [Buffer]::BlockCopy($namespaceBytes, 0, $inputBytes, 0, $namespaceBytes.Length)
  [Buffer]::BlockCopy($nameBytes, 0, $inputBytes, $namespaceBytes.Length, $nameBytes.Length)

  $sha1 = [Security.Cryptography.SHA1]::Create()
  try {
    $hash = $sha1.ComputeHash($inputBytes)
  } finally {
    $sha1.Dispose()
  }
  $hash[6] = [byte](($hash[6] -band 0x0f) -bor 0x50)
  $hash[8] = [byte](($hash[8] -band 0x3f) -bor 0x80)
  $hex = -join ($hash[0..15] | ForEach-Object { $_.ToString('x2') })
  return ('{0}-{1}-{2}-{3}-{4}' -f $hex.Substring(0, 8), $hex.Substring(8, 4),
          $hex.Substring(12, 4), $hex.Substring(16, 4), $hex.Substring(20, 12)).ToUpperInvariant()
}

if (-not $SkipBuild) {
  & (Join-Path $PSScriptRoot 'build.ps1') -Preset release -Version $Version
}
& (Join-Path $PSScriptRoot 'check-licenses.ps1') -BuildDirectory $build

Reset-ChildDirectory $stage
& cmake --install $build --prefix $stage
if ($LASTEXITCODE) { throw 'CMake install failed.' }
Copy-Item (Join-Path $repo 'README.md'), (Join-Path $repo 'LICENSE'), `
          (Join-Path $repo 'THIRD_PARTY_NOTICES.md') -Destination $stage
& (Join-Path $PSScriptRoot 'generate-assets.ps1') -Destination (Join-Path $stage 'Assets')

function Invoke-Signer([string]$Path) {
  if ($SignCommand) {
    & $SignCommand $Path
    if ($LASTEXITCODE) { throw "External signing failed for $Path" }
  } elseif ($PfxPath) {
    $sdkBin = Get-ChildItem (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin') -Directory |
      Where-Object { $_.Name -match '^\d+\.\d+\.' } |
      Sort-Object Name -Descending | Select-Object -First 1
    if (-not $sdkBin) { throw 'Windows SDK signing tool was not found.' }
    $signTool = Join-Path $sdkBin.FullName 'x64\signtool.exe'
    $password = if ($PfxPassword) {
      $credential = [pscredential]::new('pfx', $PfxPassword)
      $credential.GetNetworkCredential().Password
    } else { '' }
    & $signTool sign /fd SHA256 /f $PfxPath /p $password /tr http://timestamp.digicert.com /td SHA256 $Path
    if ($LASTEXITCODE) { throw "Signing failed for $Path" }
  }
}

if ($PfxPath -or $SignCommand) {
  Get-ChildItem $stage -File | Where-Object Extension -In '.exe', '.dll' |
    ForEach-Object { Invoke-Signer $_.FullName }
} else {
  Write-Warning 'Application binaries are unsigned. Release UAC saving will remain disabled.'
}

$licenseDirectory = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Force -Path $licenseDirectory | Out-Null
$licenseSources = @{
  'PCRE2.txt' = 'pcre2'; 'pugixml.txt' = 'pugixml'; 'QuickJS-NG.txt' = 'quickjs-ng'
  'tidy-html5.txt' = 'tidy-html5'; 'uchardet.txt' = 'uchardet'; 'yyjson.txt' = 'yyjson'
}
foreach ($entry in $licenseSources.GetEnumerator()) {
  Copy-Item (Join-Path $build "vcpkg_installed\x64-windows-static\share\$($entry.Value)\copyright") `
            (Join-Path $licenseDirectory $entry.Key)
}
Copy-Item (Join-Path $repo 'licenses\Scintilla-Lexilla.txt'), (Join-Path $repo 'licenses\Emmet.txt') -Destination $licenseDirectory

$portable = Join-Path $out 'portable'
$portableImage = Join-Path $out 'portable-image'
Reset-ChildDirectory $portableImage
Copy-Item (Join-Path $stage '*') $portableImage -Recurse -Force
New-Item -ItemType File -Path (Join-Path $portableImage 'portable.flag') -Force | Out-Null
$zip = Join-Path $out "ListopadPP-$Version-win-x64-portable.zip"
if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $portableImage '*') -DestinationPath $zip -CompressionLevel Optimal

$lockedFile = Find-LockedFile $portable
if ($lockedFile) {
  Write-Warning "The development portable directory is in use by '$lockedFile' and was left unchanged. The portable ZIP was still created from the new image."
  Remove-Item -LiteralPath $portableImage -Recurse -Force
} else {
  $portableBackup = Join-Path $out "portable.backup-$([guid]::NewGuid().ToString('N'))"
  $hadPortable = Test-Path $portable
  if ($hadPortable) { Move-Item -LiteralPath $portable -Destination $portableBackup }
  try {
    Move-Item -LiteralPath $portableImage -Destination $portable
  } catch {
    if ($hadPortable -and (Test-Path $portableBackup) -and -not (Test-Path $portable)) {
      Move-Item -LiteralPath $portableBackup -Destination $portable
    }
    throw
  }
  if ($hadPortable -and (Test-Path $portableBackup)) {
    Remove-Item -LiteralPath $portableBackup -Recurse -Force
  }
}

$wix = Get-Command wix -ErrorAction SilentlyContinue
if ($wix) {
  # The licence dialog needs RTF. Deriving it from LICENSE keeps the installer
  # from quoting a stale copy of the terms.
  $licenseRtf = Join-Path $out 'License.rtf'
  $licenseText = Get-Content (Join-Path $repo 'LICENSE') -Raw
  foreach ($pair in @(@('\', '\\'), @('{', '\{'), @('}', '\}'))) {
    $licenseText = $licenseText.Replace($pair[0], $pair[1])
  }
  $licenseBody = ($licenseText -split '\r?\n') -join '\par' + "`r`n"
  [IO.File]::WriteAllText($licenseRtf,
    "{\rtf1\ansi\deff0{\fonttbl{\f0\fswiss Segoe UI;}}\fs18`r`n$licenseBody`r`n}",
    [Text.ASCIIEncoding]::new())

  $msi = Join-Path $out "ListopadPP-$Version-win-x64.msi"
  if (Test-Path -LiteralPath $msi) {
    try {
      Remove-Item -LiteralPath $msi -Force
    } catch {
      throw "Existing MSI is in use and cannot be replaced: $msi"
    }
  }
  $wixPdb = [IO.Path]::ChangeExtension($msi, '.wixpdb')
  if (Test-Path -LiteralPath $wixPdb) { Remove-Item -LiteralPath $wixPdb -Force }
  $productCode = Get-ProductCode $Version
  $iconFile = Join-Path $repo 'assets\icons\listopad-plus-plus-multisize.ico'
  & $wix.Source build (Join-Path $repo 'packaging\wix\Package.wxs') -arch x64 `
    -ext WixToolset.UI.wixext -culture ru-ru `
    -d "StageDir=$stage" -d "ProductVersion=$Version" -d "ProductCode=$productCode" `
    -d "LicenseRtf=$licenseRtf" -d "IconFile=$iconFile" -o $msi
  if ($LASTEXITCODE) { throw 'WiX MSI build failed.' }
  Write-Host "MSI ProductCode: $productCode"
} else {
  Write-Warning 'WiX v5 was not found; the portable ZIP was created, MSI was skipped.'
}

Write-Host "Artifacts are available in $out"
