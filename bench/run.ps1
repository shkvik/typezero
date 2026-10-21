# Builds each benchmark with tz and with Go, checks that both print the same,
# and prints the best wall time of several runs and the peak working set.
#
#   powershell -NoProfile -File bench\run.ps1 [-Names fib,sieve] [-Runs 7]
#
# The peak working set is read with GetProcessMemoryInfo from the handle of
# the exited process, so even short runs are measured exactly.
param(
  [string[]]$Names,
  [int]$Runs = 7,
  [string]$Tz = (Join-Path $PSScriptRoot "..\build\dev\bin\tz.exe"),
  [string]$Go = "go"
)
$ErrorActionPreference = "Stop"

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Mem {
  [StructLayout(LayoutKind.Sequential)]
  public struct Counters {
    public uint cb; public uint PageFaultCount;
    public UIntPtr PeakWorkingSetSize; public UIntPtr WorkingSetSize;
    public UIntPtr QuotaPeakPagedPoolUsage; public UIntPtr QuotaPagedPoolUsage;
    public UIntPtr QuotaPeakNonPagedPoolUsage; public UIntPtr QuotaNonPagedPoolUsage;
    public UIntPtr PagefileUsage; public UIntPtr PeakPagefileUsage;
  }
  [DllImport("psapi.dll", SetLastError = true)]
  static extern bool GetProcessMemoryInfo(IntPtr process, out Counters counters, uint size);
  public static ulong Peak(IntPtr handle) {
    Counters c;
    if (!GetProcessMemoryInfo(handle, out c, (uint)Marshal.SizeOf(typeof(Counters)))) return 0;
    return c.PeakWorkingSetSize.ToUInt64();
  }
}
"@

# Runs `exe` once; returns its output, wall time in ms and peak working set.
function Invoke-Once([string]$exe) {
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $exe
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $p = [System.Diagnostics.Process]::Start($psi)
  $handle = $p.Handle
  $output = $p.StandardOutput.ReadToEnd()
  $p.WaitForExit()
  $sw.Stop()
  $result = @{ Output = $output; Ms = $sw.Elapsed.TotalMilliseconds; Peak = [Mem]::Peak($handle);
               Exit = $p.ExitCode }
  $p.Dispose()
  return $result
}

function Measure-Exe([string]$exe) {
  $best = [double]::MaxValue
  $peak = 0
  for ($i = 0; $i -lt $Runs; $i++) {
    $r = Invoke-Once $exe
    if ($r.Ms -lt $best) { $best = $r.Ms }
    if ($r.Peak -gt $peak) { $peak = $r.Peak }
  }
  return @{ Ms = [math]::Round($best); MB = [math]::Round($peak / 1MB, 1) }
}

$out = Join-Path $PSScriptRoot "out"
New-Item -ItemType Directory -Force $out | Out-Null
# -File passes "a,b" as one string.
$Names = $Names | ForEach-Object { $_ -split ',' } | Where-Object { $_ }
if (-not $Names) {
  $Names = Get-ChildItem -Directory $PSScriptRoot | Where-Object { $_.Name -ne "out" } |
           ForEach-Object { $_.Name }
}

# Build everything first, so that compilers do not run between measurements.
$env:GOAMD64 = if ($env:GOAMD64) { $env:GOAMD64 } else { "v1" }
foreach ($n in $Names) {
  $dir = Join-Path $PSScriptRoot $n
  $ts = Join-Path $dir "main.ts"
  if (Test-Path $ts) {
    & $Tz build -O2 -o (Join-Path $out "$n-tz.exe") $ts
    if ($LASTEXITCODE -ne 0) { throw "tz failed on $n" }
  }
  & $Go build -o (Join-Path $out "$n-go.exe") (Join-Path $dir "main.go")
  if ($LASTEXITCODE -ne 0) { throw "go failed on $n" }
}

"{0,-15} {1,8} {2,8} {3,8} {4,8}" -f "benchmark", "tz ms", "go ms", "tz MB", "go MB"
foreach ($n in $Names) {
  $tzExe = Join-Path $out "$n-tz.exe"
  $goExe = Join-Path $out "$n-go.exe"
  $g = Measure-Exe $goExe
  if (Test-Path $tzExe) {
    $a = Invoke-Once $tzExe
    $b = Invoke-Once $goExe
    if ($a.Output -ne $b.Output) {
      Write-Warning "${n}: tz printed '$($a.Output.Trim())', Go printed '$($b.Output.Trim())'"
    }
    $t = Measure-Exe $tzExe
    "{0,-15} {1,8} {2,8} {3,8} {4,8}" -f $n, $t.Ms, $g.Ms, $t.MB, $g.MB
  } else {
    "{0,-15} {1,8} {2,8} {3,8} {4,8}" -f $n, "-", $g.Ms, "-", $g.MB
  }
}
