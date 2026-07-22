[CmdletBinding()]
param(
  [string]$Version = '0.1.1',
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

$sdkBin = Get-ChildItem (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin') -Directory |
  Where-Object { $_.Name -match '^\d+\.\d+\.' } | Sort-Object Name -Descending | Select-Object -First 1
if (-not $sdkBin) { throw 'Windows SDK packaging tools were not found.' }
$makeAppx = Join-Path $sdkBin.FullName 'x64\makeappx.exe'
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
[IO.File]::WriteAllText((Join-Path $identity 'AppxManifest.xml'), $manifest, [Text.UTF8Encoding]::new($false))
$msix = Join-Path $out 'ListopadPP.Identity.msix'
if (Test-Path $msix) { Remove-Item -LiteralPath $msix -Force }
& $makeAppx pack /o /nv /d $identity /p $msix
if ($LASTEXITCODE) { throw 'Sparse identity package creation failed.' }
if ($PfxPath -or $SignCommand) { Invoke-Signer $msix }
Copy-Item $msix (Join-Path $stage 'ListopadPP.Identity.msix') -Force

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
Reset-ChildDirectory $portable
Copy-Item (Join-Path $stage '*') $portable -Recurse -Force
New-Item -ItemType File -Path (Join-Path $portable 'portable.flag') -Force | Out-Null
$zip = Join-Path $out "ListopadPP-$Version-win-x64-portable.zip"
if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $portable '*') -DestinationPath $zip -CompressionLevel Optimal

$wix = Get-Command wix -ErrorAction SilentlyContinue
if ($wix) {
  $msi = Join-Path $out "ListopadPP-$Version-win-x64.msi"
  $registerSparse = if ($PfxPath -or $SignCommand) { '1' } else { '0' }
  & $wix.Source build (Join-Path $repo 'packaging\wix\Package.wxs') -arch x64 `
    -d "StageDir=$stage" -d "ProductVersion=$Version" -d "RegisterSparse=$registerSparse" -o $msi
  if ($LASTEXITCODE) { throw 'WiX MSI build failed.' }
  if ($PfxPath -or $SignCommand) { Invoke-Signer $msi }
} else {
  Write-Warning 'WiX v4 was not found; portable ZIP and sparse identity package were created, MSI was skipped.'
}

Write-Host "Artifacts are available in $out"
