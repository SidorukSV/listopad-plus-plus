[CmdletBinding()]
param(
  [string]$Executable = (Join-Path $PSScriptRoot '..\build\release\ListopadPP.exe'),
  [string]$File,
  [int]$Iterations = 20,
  [string]$OutputCsv = (Join-Path $PSScriptRoot '..\out\performance.csv'),
  [int]$MaxAverageMs = 2500,
  [int]$MaxSingleMs = 4500
)

$ErrorActionPreference = 'Stop'
$Executable = [IO.Path]::GetFullPath($Executable)
if (-not (Test-Path $Executable)) { throw "Executable not found: $Executable" }
$profile = Join-Path ([IO.Path]::GetTempPath()) "listopad-performance-$([guid]::NewGuid().ToString('N'))"
$previousInstance = [Environment]::GetEnvironmentVariable('LISTOPAD_INSTANCE_ID', 'Process')
$previousProfile = [Environment]::GetEnvironmentVariable('LISTOPAD_PROFILE_DIR', 'Process')
$env:LISTOPAD_INSTANCE_ID = "performance-$([guid]::NewGuid().ToString('N'))"
$env:LISTOPAD_PROFILE_DIR = $profile
New-Item -ItemType Directory -Force -Path $profile | Out-Null

try {
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
  $summary = $results | Measure-Object FirstWindowMs -Average -Minimum -Maximum
  $summary
  Write-Host "Budgets: average <= $MaxAverageMs ms; single run <= $MaxSingleMs ms"
  Write-Host "Raw results: $OutputCsv"

  if ($summary.Average -gt $MaxAverageMs) {
    throw "Average first-window time $([math]::Round($summary.Average, 1)) ms exceeded the $MaxAverageMs ms budget."
  }
  if ($summary.Maximum -gt $MaxSingleMs) {
    throw "First-window time $($summary.Maximum) ms exceeded the $MaxSingleMs ms budget."
  }
} finally {
  if ($null -eq $previousInstance) {
    Remove-Item Env:LISTOPAD_INSTANCE_ID -ErrorAction SilentlyContinue
  } else {
    $env:LISTOPAD_INSTANCE_ID = $previousInstance
  }
  if ($null -eq $previousProfile) {
    Remove-Item Env:LISTOPAD_PROFILE_DIR -ErrorAction SilentlyContinue
  } else {
    $env:LISTOPAD_PROFILE_DIR = $previousProfile
  }
  if (Test-Path -LiteralPath $profile) {
    Remove-Item -LiteralPath $profile -Recurse -Force
  }
}
