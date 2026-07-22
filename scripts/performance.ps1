[CmdletBinding()]
param(
  [string]$Executable = (Join-Path $PSScriptRoot '..\build\release\ListopadPP.exe'),
  [string]$File,
  [int]$Iterations = 20,
  [string]$OutputCsv = (Join-Path $PSScriptRoot '..\out\performance.csv')
)

$ErrorActionPreference = 'Stop'
$Executable = [IO.Path]::GetFullPath($Executable)
if (-not (Test-Path $Executable)) { throw "Executable not found: $Executable" }
$results = for ($iteration = 1; $iteration -le $Iterations; $iteration++) {
  $watch = [Diagnostics.Stopwatch]::StartNew()
  $arguments = if ($File) { @([IO.Path]::GetFullPath($File)) } else { @() }
  $process = Start-Process -FilePath $Executable -ArgumentList $arguments -PassThru
  while (-not $process.HasExited -and $process.MainWindowHandle -eq 0 -and $watch.ElapsedMilliseconds -lt 5000) {
    Start-Sleep -Milliseconds 5
    $process.Refresh()
  }
  $watch.Stop()
  [pscustomobject]@{ Iteration = $iteration; FirstWindowMs = $watch.ElapsedMilliseconds; File = $File }
  if (-not $process.HasExited) {
    $process.CloseMainWindow() | Out-Null
    if (-not $process.WaitForExit(2000)) { Stop-Process -Id $process.Id -Force }
  }
}
New-Item -ItemType Directory -Force -Path (Split-Path $OutputCsv) | Out-Null
$results | Export-Csv -NoTypeInformation -Encoding UTF8 $OutputCsv
$results | Measure-Object FirstWindowMs -Average -Minimum -Maximum
Write-Host "Raw results: $OutputCsv"
