# Runs one bench/*.aer script and reports the four axes worth tracking for a language pitched as
# "light, fast, easy to understand" -- wall-clock alone (what every script above already prints
# itself) doesn't show whether the *interpreter* is light, just whether the workload is fast.
#
# Usage: powershell -File bench/run_bench.ps1 <script.aer> [aer.exe path]
param(
    [Parameter(Mandatory=$true)][string]$Script,
    [string]$AerExe = "binary\aer.exe"
)

if (-not (Test-Path $AerExe)) { Write-Error "aer executable not found at $AerExe -- build it first (make all)"; exit 1 }
if (-not (Test-Path $Script)) { Write-Error "script not found: $Script"; exit 1 }

$proc = Start-Process -FilePath $AerExe -ArgumentList $Script -PassThru -NoNewWindow -RedirectStandardOutput "$env:TEMP\bench_stdout.txt"
$peakBytes = 0
while (-not $proc.HasExited) {
    try { $proc.Refresh(); if ($proc.PeakWorkingSet64 -gt $peakBytes) { $peakBytes = $proc.PeakWorkingSet64 } } catch {}
    Start-Sleep -Milliseconds 20
}
$proc.Refresh()
if ($proc.PeakWorkingSet64 -gt $peakBytes) { $peakBytes = $proc.PeakWorkingSet64 }

$stdout = Get-Content "$env:TEMP\bench_stdout.txt" -Raw
Write-Host $stdout.TrimEnd()

$binSize = (Get-Item $AerExe).Length
$peakMB  = [math]::Round($peakBytes / 1MB, 2)
$binKB   = [math]::Round($binSize / 1KB, 1)

Write-Host ("  peak memory: {0} MB" -f $peakMB)
Write-Host ("  binary size: {0} KB ({1})" -f $binKB, $AerExe)
