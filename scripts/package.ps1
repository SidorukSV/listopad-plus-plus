[CmdletBinding()]
param(
  [string]$Version = '0.1.2',
  [string]$Publisher = 'CN=ListopadPP Development',
  [string]$PackageName = 'ListopadPP',
  [string]$PfxPath,
  [securestring]$PfxPassword,
  [string]$SignCommand,
  [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$out = Join-Path $repo 'out'
$stage = Join-Path $out 'stage'
$identity = Join-Path $out 'identity'
$build = Join-Path $repo 'build\release'

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

if (-not $SkipBuild) {
  & (Join-Path $PSScriptRoot 'build.ps1') -Preset release -Version $Version -Publisher $Publisher
}
& (Join-Path $PSScriptRoot 'check-licenses.ps1') -BuildDirectory $build

Reset-ChildDirectory $stage
Reset-ChildDirectory $identity
& cmake --install $build --prefix $stage
if ($LASTEXITCODE) { throw 'CMake install failed.' }
Copy-Item (Join-Path $repo 'README.md'), (Join-Path $repo 'LICENSE'), `
          (Join-Path $repo 'THIRD_PARTY_NOTICES.md') -Destination $stage
& (Join-Path $PSScriptRoot 'generate-assets.ps1') -Destination (Join-Path $stage 'Assets')
# Installed alongside the binaries: the MSI custom actions run it instead of an
# inline command, which Windows Installer would mangle.
Copy-Item (Join-Path $repo 'packaging\wix\shell-integration.ps1') $stage -Force

$sdkBin = Get-ChildItem (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin') -Directory |
  Where-Object { $_.Name -match '^\d+\.\d+\.' } | Sort-Object Name -Descending | Select-Object -First 1
if (-not $sdkBin) { throw 'Windows SDK packaging tools were not found.' }
$makeAppx = Join-Path $sdkBin.FullName 'x64\makeappx.exe'
$makePri = Join-Path $sdkBin.FullName 'x64\makepri.exe'
$signTool = Join-Path $sdkBin.FullName 'x64\signtool.exe'

function Invoke-Signer([string]$Path) {
  if ($SignCommand) {
    & $SignCommand $Path
    if ($LASTEXITCODE) { throw "External signing failed for $Path" }
  } elseif ($PfxPath) {
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
  Write-Warning 'Artifacts are unsigned. Release UAC saving and modern Windows 11 context-menu registration will remain disabled.'
}

$manifest = Get-Content -Raw -Encoding UTF8 (Join-Path $repo 'packaging\msix\AppxManifest.xml.in')
$manifest = $manifest.Replace('@PACKAGE_NAME@', $PackageName).Replace('@PUBLISHER@', $Publisher).Replace('@PACKAGE_VERSION@', "$Version.0")
$manifestPath = Join-Path $identity 'AppxManifest.xml'
[IO.File]::WriteAllText($manifestPath, $manifest, [Text.UTF8Encoding]::new($false))

# The shell reads the taskbar and Start logos from the package payload, not from
# the external content location, so the visual assets must live inside the MSIX
# and the resource index must be built over the very directory that is packed.
$identityAssets = Join-Path $identity 'Assets'
Copy-Item (Join-Path $stage 'Assets') $identity -Recurse -Force
$logoCount = (Get-ChildItem $identityAssets -Filter '*.png' -File).Count
if ($logoCount -lt 1) { throw 'No visual assets were staged into the identity package.' }
foreach ($required in 'ListopadPP-StoreLogo.png', 'ListopadPP-Square150x150Logo.png',
                      'ListopadPP-Square44x44Logo.png') {
  if (-not (Test-Path (Join-Path $identityAssets $required))) {
    throw "Manifest references a missing visual asset: Assets\$required"
  }
}

$priConfig = Join-Path $identity 'priconfig.xml'
$priPath = Join-Path $identity 'resources.pri'
& $makePri createconfig /cf $priConfig /dq en-US /o
if ($LASTEXITCODE) { throw 'PRI configuration creation failed.' }
& $makePri new /pr $identity /cf $priConfig /mn $manifestPath /of $priPath /o
if ($LASTEXITCODE) { throw 'Package resource index creation failed.' }
Remove-Item -LiteralPath $priConfig -Force
$msix = Join-Path $out 'ListopadPP.Identity.msix'
if (Test-Path $msix) { Remove-Item -LiteralPath $msix -Force }
& $makeAppx pack /o /nv /d $identity /p $msix
if ($LASTEXITCODE) { throw 'Sparse identity package creation failed.' }
if ($PfxPath -or $SignCommand) { Invoke-Signer $msix }
Copy-Item $msix (Join-Path $stage 'ListopadPP.Identity.msix') -Force

# External content location for the sparse package. Keeping it to just the
# context-menu server and the declared host is hygiene, not a hard requirement:
# a package does not block undeclared binaries placed beside it (verified). The
# editor is excluded because nothing in the package needs it, and a narrow
# external location makes the boundary obvious.
$shellExt = Join-Path $out 'shellext'
Reset-ChildDirectory $shellExt
Copy-Item (Join-Path $stage 'ListopadShell.dll') $shellExt -Force
# Never executed; it only satisfies the manifest's Executable attribute.
Copy-Item (Join-Path $stage 'ListopadShellHost.exe') $shellExt -Force
foreach ($required in 'ListopadShell.dll', 'ListopadShellHost.exe') {
  if (-not (Test-Path (Join-Path $shellExt $required))) {
    throw "External content directory is missing $required"
  }
}
if (Test-Path (Join-Path $shellExt 'ListopadPP.exe')) {
  throw 'The editor does not belong in the package external content directory.'
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
  $registerSparse = if ($PfxPath -or $SignCommand) { '1' } else { '0' }
  & $wix.Source build (Join-Path $repo 'packaging\wix\Package.wxs') -arch x64 `
    -ext WixToolset.UI.wixext -culture ru-ru `
    -d "StageDir=$stage" -d "ProductVersion=$Version" -d "RegisterSparse=$registerSparse" `
    -d "LicenseRtf=$licenseRtf" -o $msi
  if ($LASTEXITCODE) { throw 'WiX MSI build failed.' }
  if ($PfxPath -or $SignCommand) { Invoke-Signer $msi }
} else {
  Write-Warning 'WiX v5 was not found; portable ZIP and sparse identity package were created, MSI was skipped.'
}

Write-Host "Artifacts are available in $out"
