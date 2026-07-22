[CmdletBinding()]
param(
  [ValidateSet('debug', 'release')][string]$Preset = 'release',
  [string]$VcpkgRoot = (Join-Path $PSScriptRoot '..\.deps\vcpkg'),
  [string]$Version = '0.1.1',
  [string]$Publisher = 'CN=ListopadPP Development',
  [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$VcpkgRoot = [IO.Path]::GetFullPath($VcpkgRoot)
if (-not (Test-Path (Join-Path $VcpkgRoot 'vcpkg.exe'))) {
  & (Join-Path $PSScriptRoot 'bootstrap-vcpkg.ps1') -VcpkgRoot $VcpkgRoot
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio Build Tools 2022 were not found.' }
$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'MSVC x64 build tools were not found.' }
$vsdev = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
$environment = & cmd.exe /d /s /c "`"$vsdev`" -no_logo -arch=amd64 -host_arch=amd64 && set"
foreach ($line in $environment) {
  if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($matches[1])" -Value $matches[2] }
}
$env:VCPKG_ROOT = $VcpkgRoot

Push-Location $repo
try {
  & cmake --preset $Preset "-DLISTOPAD_VERSION=$Version" `
    "-DLISTOPAD_PACKAGE_PUBLISHER=$Publisher"
  if ($LASTEXITCODE) { throw 'CMake configure failed.' }
  & cmake --build --preset $Preset --parallel
  if ($LASTEXITCODE) { throw 'CMake build failed.' }
  if (-not $SkipTests) {
    & ctest --preset $Preset --output-on-failure
    if ($LASTEXITCODE) { throw 'Tests failed.' }
  }
} finally {
  Pop-Location
}
