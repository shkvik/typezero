# Measures the check of big programs that gen.py writes: time and peak memory
# of the check the editor runs (tz lsp --check), of tz map, and the size of
# the records of the modules (tz query records). docs/lsp.md, step 3.
#
#   powershell -File bench\scale\measure.ps1 <dir> 100000,1000000
#
# <dir> gets one project per size (p<lines>), made once and kept.

param([string]$Dir, [string]$Lines)
$ErrorActionPreference = "Stop"
$tz = Join-Path $PSScriptRoot "..\..\build\dev\bin\tz.exe"
$gen = Join-Path $PSScriptRoot "gen.py"

function Measure-Tz([string[]]$TzArgs, [string]$Out) {
  $clock = [System.Diagnostics.Stopwatch]::StartNew()
  $p = Start-Process -FilePath $tz -ArgumentList $TzArgs -NoNewWindow -PassThru `
    -RedirectStandardOutput $Out -RedirectStandardError "$Out.err"
  $null = $p.Handle
  $peak = 0
  while (-not $p.HasExited) {
    $q = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
    if ($q) { $peak = [Math]::Max($peak, [int64]$q.PeakWorkingSet64) }
    Start-Sleep -Milliseconds 20
  }
  $p.WaitForExit()
  $clock.Stop()
  try { $peak = [Math]::Max($peak, $p.PeakWorkingSet64) } catch {}
  return @{ Seconds = $clock.Elapsed.TotalSeconds; PeakMB = [Math]::Round($peak / 1MB); Code = $p.ExitCode }
}

foreach ($n in ($Lines -split "," | ForEach-Object { [int]$_ })) {
  $project = Join-Path $Dir "p$n"
  if (-not (Test-Path (Join-Path $project "main.tz"))) { python $gen $project $n | Out-Null }
  $main = (Resolve-Path (Join-Path $project "main.tz")).Path
  $request = Join-Path $Dir "request-$n.json"
  @{ entry = $main; files = @() } | ConvertTo-Json | Set-Content -Encoding ascii $request
  $check = Measure-Tz @("lsp", "--check", $request, (Join-Path $Dir "response-$n.json")) (Join-Path $Dir "check-$n.txt")
  $map = Measure-Tz @("map", "--entry", $main, "--budget", "2000") (Join-Path $Dir "map-$n.txt")
  $records = Measure-Tz @("query", "records", "--entry", $main) (Join-Path $Dir "records-$n.json")
  $bytes = (Get-Item (Join-Path $Dir "records-$n.json")).Length
  "{0,9} lines: check {1,7:N1} s {2,6} MB | map {3,7:N1} s {4,6} MB | records {5,6:N0} MB ({6:N0} bytes/line) | codes {7} {8} {9}" -f `
    $n, $check.Seconds, $check.PeakMB, $map.Seconds, $map.PeakMB, ($bytes / 1MB), ($bytes / $n), $check.Code, $map.Code, $records.Code
}
