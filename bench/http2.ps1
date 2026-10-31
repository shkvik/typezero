# The HTTP/2 benchmark (docs/net.md, section 6): the "Hello, World!" server of
# http2_hello over HTTP/2 without TLS (h2c) in tz, Go (net/http with
# x/net/http2/h2c) and Rust (hyper), each loaded in turn by oha
# (cargo install oha) with 32 connections of 8 requests at once.
#
#   powershell -NoProfile -File bench\http2.ps1 [-Connections 32] [-Streams 8] [-Seconds 8]
param(
  [int]$Connections = 32,
  [int]$Streams = 8,
  [int]$Seconds = 8,
  [string]$Tz = (Join-Path $PSScriptRoot "..\build\dev\bin\tz.exe"),
  [string]$Go = "go",
  [string]$Cargo = "cargo",
  [string]$Oha = "oha"
)
$ErrorActionPreference = "Stop"

$dir = Join-Path $PSScriptRoot "http2_hello"
$out = Join-Path $PSScriptRoot "out"
New-Item -ItemType Directory -Force $out | Out-Null
& $Tz build -o (Join-Path $out "http2_hello-tz.exe") (Join-Path $dir "main.tz")
if ($LASTEXITCODE -ne 0) { throw "tz failed" }
Push-Location (Join-Path $dir "go")
& $Go build -o (Join-Path $out "http2_hello-go.exe") .
Pop-Location
if ($LASTEXITCODE -ne 0) { throw "go failed" }
Push-Location (Join-Path $dir "rust")
& $Cargo build --release -q
Pop-Location
if ($LASTEXITCODE -ne 0) { throw "cargo failed" }
Copy-Item (Join-Path $dir "rust\target\release\http2-hello.exe") (Join-Path $out "http2_hello-rust.exe")

$servers = [ordered]@{
  "tz" = (Join-Path $out "http2_hello-tz.exe")
  "go" = (Join-Path $out "http2_hello-go.exe")
  "rust" = (Join-Path $out "http2_hello-rust.exe")
}
$port = 19080
foreach ($name in $servers.Keys) {
  $p = Start-Process -FilePath $servers[$name] -ArgumentList @("$port") -PassThru -WindowStyle Hidden
  Start-Sleep -Milliseconds 700
  $result = & $Oha -z "${Seconds}s" -c $Connections -p $Streams --http2 --no-tui "http://127.0.0.1:$port/"
  Stop-Process -Id $p.Id -Force
  $rate = ($result | Select-String "Requests/sec").ToString().Trim()
  "{0,-6} {1}" -f $name, $rate
  $port++
}
