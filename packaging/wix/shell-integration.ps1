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
$package = Join-Path $InstallDir 'ListopadPP.Identity.msix'

if ($Remove) {
  try { Get-AppxPackage -Name 'ListopadPP' | Remove-AppxPackage -ErrorAction Stop } catch { }
  return
}

# Only the modern menu is handled here. The classic entry is a plain MSI
# component installed unconditionally, so there is nothing to fall back to and
# nothing to undo: a failure here simply leaves the user on the classic menu.
# Registration fails whenever the signing certificate is not trusted, which is
# every machine but the build one while the project signs with a development
# certificate, so it must never abort the install.
if (Test-Path -LiteralPath $package) {
  try {
    Add-AppxPackage -Path $package -ExternalLocation $InstallDir -ErrorAction Stop
  } catch {
    Write-Host "Sparse package registration failed, classic menu remains: $($_.Exception.Message)"
  }
}
