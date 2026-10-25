# The HTTP benchmark (docs/net.md, section 6): the "Hello, World!" server of
# http_hello in tz, Go (net/http) and node (node:http), each loaded in turn
# by load/main.go, which keeps 128 connections alive for 10 s, as bombardier
# does; prints the requests a second and the mean latency of each.
#
#   powershell -NoProfile -File bench\http.ps1 [-Connections 128] [-Seconds 10]
param(
  [int]$Connections = 128,
  [int]$Seconds = 10,
  [string]$Tz = (Join-Path $PSScriptRoot "..\build\dev\bin\tz.exe"),
  [string]$Go = "go",
  [string]$Node = "node"
)
$ErrorActionPreference = "Stop"

$dir = Join-Path $PSScriptRoot "http_hello"
$out = Join-Path $PSScriptRoot "out"
New-Item -ItemType Directory -Force $out | Out-Null
$env:GOAMD64 = if ($env:GOAMD64) { $env:GOAMD64 } else { "v1" }
& $Tz build -o (Join-Path $out "http_hello-tz.exe") (Join-Path $dir "main.ts")
if ($LASTEXITCODE -ne 0) { throw "tz failed" }
& $Go build -o (Join-Path $out "http_hello-go.exe") (Join-Path $dir "main.go")
if ($LASTEXITCODE -ne 0) { throw "go failed" }
Push-Location (Join-Path $dir "load")
& $Go build -o (Join-Path $out "http_load.exe") .
Pop-Location
if ($LASTEXITCODE -ne 0) { throw "go failed on the load" }

$servers = [ordered]@{
  "tz" = @((Join-Path $out "http_hello-tz.exe"))
  "go" = @((Join-Path $out "http_hello-go.exe"))
  "node" = @($Node, (Join-Path $dir "server.mjs"))
}
$port = 18080
foreach ($name in $servers.Keys) {
  $cmd = $servers[$name]
  $args = @($cmd | Select-Object -Skip 1) + @("$port")
  $p = Start-Process -FilePath $cmd[0] -ArgumentList $args -PassThru -WindowStyle Hidden
  Start-Sleep -Milliseconds 700
  $result = & (Join-Path $out "http_load.exe") -url "127.0.0.1:$port" -c $Connections -d "${Seconds}s"
  Stop-Process -Id $p.Id -Force
  "{0,-6} {1}" -f $name, $result
  $port++
}
