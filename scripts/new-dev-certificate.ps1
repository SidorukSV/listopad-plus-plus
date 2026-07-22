[CmdletBinding(SupportsShouldProcess)]
param(
  [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\.deps\signing'),
  [securestring]$Password = (ConvertTo-SecureString 'listopad-dev-only' -AsPlainText -Force)
)

$ErrorActionPreference = 'Stop'
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
if (-not $PSCmdlet.ShouldProcess('development certificate stores', 'Create and trust Listopad++ development certificate')) { return }
$certificate = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=ListopadPP Development' `
  -CertStoreLocation 'Cert:\CurrentUser\My' -HashAlgorithm SHA256 -KeyExportPolicy Exportable
$pfx = Join-Path $OutputDirectory 'ListopadPP.Development.pfx'
$cer = Join-Path $OutputDirectory 'ListopadPP.Development.cer'
Export-PfxCertificate -Cert $certificate -FilePath $pfx -Password $Password | Out-Null
Export-Certificate -Cert $certificate -FilePath $cer | Out-Null
$certutil = Join-Path $env:SystemRoot 'System32\certutil.exe'
$quotedCertificate = '"' + $cer + '"'
$trust = Start-Process -FilePath $certutil -ArgumentList @('-addstore', 'TrustedPeople', $quotedCertificate) `
  -Verb RunAs -Wait -PassThru
if ($trust.ExitCode -ne 0) { throw "Development certificate trust was cancelled or failed (exit $($trust.ExitCode))." }
Write-Host "Development signing files created in $OutputDirectory"
