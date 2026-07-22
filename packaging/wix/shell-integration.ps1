# Shell integration for the MSI, kept out of the custom action's command line.
#
# Windows Installer parses Formatted fields itself, and there `{...}` delimits a
# conditional block. An inline `try { } catch { }` therefore loses its bodies
# before PowerShell ever sees it, and the action dies with exit code 1. Keeping
# the logic in a file sidesteps MSI's parser entirely.

# The install directory is deliberately not a parameter. [INSTALLFOLDER] ends
# with a backslash, so "[INSTALLFOLDER]" on a command line becomes ...\" and
# CommandLineToArgvW reads the trailing \" as an escaped quote, splicing the
# arguments together. The script ships inside that directory, so it can just
# look at itself.
[CmdletBinding()]
param(
  [string]$InstallDir,
  [switch]$Remove
)

$ErrorActionPreference = 'Stop'
# Not a param() default: $PSScriptRoot is empty there under Windows PowerShell
# 5.1, which is what the custom action runs.
if (-not $InstallDir) { $InstallDir = Split-Path -Parent $PSCommandPath }
$editor = Join-Path $InstallDir 'ListopadPP.exe'
$package = Join-Path $InstallDir 'ListopadPP.Identity.msix'

function Invoke-Editor([string[]]$Arguments) {
  if (-not (Test-Path -LiteralPath $editor)) { return }
  Start-Process -FilePath $editor -ArgumentList $Arguments -Wait
}

if ($Remove) {
  try { Get-AppxPackage -Name 'ListopadPP' | Remove-AppxPackage -ErrorAction Stop } catch { }
  Invoke-Editor @('--unregister-context-menu')
  return
}

# The modern context menu needs the sparse package, and that only registers when
# the signing certificate is trusted on this machine. A development certificate
# never is, so fall back rather than leaving the user with no menu at all.
$registered = $false
if (Test-Path -LiteralPath $package) {
  try {
    Add-AppxPackage -Path $package -ExternalLocation $InstallDir -ErrorAction Stop
    $registered = $true
  } catch {
    Write-Host "Sparse package registration failed, falling back: $($_.Exception.Message)"
  }
}

if (-not $registered) { Invoke-Editor @('--register-context-menu') }
