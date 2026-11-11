<#
One-command benchmark run for Windows (MSVC Build Tools + the conda env described in docs/BENCHMARKING.md).

  scripts\bench_windows.ps1 -Quick                       # a few minutes: checks everything works
  scripts\bench_windows.ps1                              # full run
  scripts\bench_windows.ps1 -Itch C:\Data\07302019.NASDAQ_ITCH50.gz   # also replay real Nasdaq data

Builds Release, runs the tests and the differential-fuzz gate, runs the single-thread, queue and
(optionally) real-data benchmarks, and writes bench/results/platforms/<id>/ (JSON per run,
machine.txt, SUMMARY.md). Threads are pinned: -Cpu is the logical CPU for the single-thread runs
(pick a performance core; see docs/BENCHMARKING.md), the queue benchmark chooses two cores itself.
#>
param(
  [switch]$Quick,
  [string]$Itch = "",
  [string]$Id = "",
  [int]$Cpu = 8
)
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")

$cpuName = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim()
if (-not $Id) { $Id = "windows-" + (($cpuName -replace '[^A-Za-z0-9]+', '-').Trim('-').ToLower()) }
$out = "bench/results/platforms/$Id"
New-Item -ItemType Directory -Force "$out/single", "$out/mt", "$out/realdata" | Out-Null
Start-Transcript -Path "$out/run.log" -Force | Out-Null

function Run([string]$what, [scriptblock]$cmd) {
  & $cmd
  if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" }
}

Write-Host "== $cpuName, $((Get-CimInstance Win32_OperatingSystem).Caption), results in $out"
Write-Host "Close heavy applications first; the machine should be idle and on AC power."

Write-Host "== configure, build, test (Release)"
$build = "build-rel"
Run "build" { cmd /c "scripts\win-build.cmd $build" }
$env:PATH = "$env:USERPROFILE\miniforge3\envs\lob\Library\bin;$env:PATH"
Write-Host "== differential fuzz vs the reference model"
$fuzzOps = if ($Quick) { 200000 } else { 1000000 }
Run "fuzz" { & "$build\tests\lob_fuzz.exe" --seed 1 --ops $fuzzOps --runs 2 }

@(
  "date: $((Get-Date).ToUniversalTime().ToString('s'))Z",
  "git: $(git rev-parse --short HEAD)",
  "cpu: $cpuName ($((Get-CimInstance Win32_Processor | Select-Object -First 1).NumberOfLogicalProcessors) logical)",
  "memory: $([math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)) GB",
  "os: $((Get-CimInstance Win32_OperatingSystem).Caption) $((Get-CimInstance Win32_OperatingSystem).Version)",
  "power plan: $((powercfg /getactivescheme) -join ' ')"
) | Set-Content -Encoding utf8 "$out/machine.txt"

if ($Quick) { $ops = 500000; $reps = 3; $mtOps = 200000 } else { $ops = 2000000; $reps = 7; $mtOps = 1000000 }
$note = "$cpuName, Windows, not isolated: normal background load, boost not controlled; threads pinned."
if ($Quick) { $note = "SMOKE TEST ONLY. $note" }

Write-Host "== single-thread benchmark"
Run "run_bench" { python scripts/run_bench.py --label final --build $build --cpu $Cpu --ops $ops --reps $reps --outdir "$out/single" --note $note }
Write-Host "== queue benchmark (producer -> queue -> matching thread)"
Run "run_mt_bench" { python scripts/run_mt_bench.py --build $build --ops $mtOps --reps $reps --outdir "$out/mt" --note $note }

if ($Itch) {
  if (-not (Test-Path $Itch)) { throw "ITCH file not found: $Itch" }
  Write-Host "== real Nasdaq ITCH replay"
  Run "run_realdata_bench" { python scripts/run_realdata_bench.py --exe "$build\itch\lob_itch.exe" --input $Itch --out "$out/realdata/bench.json" --cpu $Cpu --reps $reps --build $build --note-extra $note }
} else {
  Write-Host "== real-data replay skipped (pass -Itch FILE to include it)"
  Remove-Item "$out/realdata" -ErrorAction SilentlyContinue
}

Run "summarize" { python scripts/summarize_platform.py $out }
Stop-Transcript | Out-Null
Write-Host "== done. Results: $out (summary: $out/SUMMARY.md)"
if ($Quick) { Write-Host "That was a -Quick smoke test. Run again without -Quick for publishable numbers." }
