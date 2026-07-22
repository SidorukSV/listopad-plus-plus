[CmdletBinding()]
param(
  [string]$VcpkgRoot = (Join-Path $PSScriptRoot '..\.deps\vcpkg')
)

$ErrorActionPreference = 'Stop'
$baseline = '82b6bc886d7b0f8342e34babc2e0b8943f79b0e1'
$VcpkgRoot = [IO.Path]::GetFullPath($VcpkgRoot)

if (-not (Test-Path (Join-Path $VcpkgRoot '.git'))) {
  New-Item -ItemType Directory -Force -Path (Split-Path $VcpkgRoot) | Out-Null
  & git clone https://github.com/microsoft/vcpkg.git $VcpkgRoot
  if ($LASTEXITCODE) { throw 'Unable to clone vcpkg.' }
}

& git -C $VcpkgRoot fetch origin $baseline --depth 1
if ($LASTEXITCODE) { throw 'Unable to fetch the pinned vcpkg baseline.' }
& git -C $VcpkgRoot checkout --detach $baseline
if ($LASTEXITCODE) { throw 'Unable to check out the pinned vcpkg baseline.' }
& (Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat') -disableMetrics
if ($LASTEXITCODE) { throw 'Unable to bootstrap vcpkg.' }

Write-Host "vcpkg is ready at $VcpkgRoot"
