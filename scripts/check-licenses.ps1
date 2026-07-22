[CmdletBinding()]
param(
  [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build\release')
)

$ErrorActionPreference = 'Stop'
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$required = @(
  'vcpkg_installed\x64-windows-static\share\pcre2\copyright',
  'vcpkg_installed\x64-windows-static\share\pugixml\copyright',
  'vcpkg_installed\x64-windows-static\share\quickjs-ng\copyright',
  'vcpkg_installed\x64-windows-static\share\tidy-html5\copyright',
  'vcpkg_installed\x64-windows-static\share\uchardet\copyright',
  'vcpkg_installed\x64-windows-static\share\yyjson\copyright',
  '_deps\scintilla_upstream-src\License.txt',
  '_deps\lexilla_upstream-src\License.txt'
)
$missing = $required | Where-Object { -not (Test-Path (Join-Path $BuildDirectory $_)) }
if ($missing) { throw "Missing dependency license files:`n$($missing -join "`n")" }
Write-Host 'All dependency license files are present.'
