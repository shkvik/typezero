# Builds OpenSSL for tz (docs/crypto.md, section 2): the version line of node 24, static, with the
# static CRT, trimmed as node is trimmed. The result, in -Out:
#
#   include/                  the headers of OpenSSL
#   private/                  the internal headers runtime/crypto_*.c needs for the direct layer
#   lib/libcrypto.lib         all of libcrypto: the full layer (docs/crypto.md, section 3)
#   lib/libcrypto_direct.lib  the objects of it that the direct layer needs (see direct.txt)
#   lib/libssl.lib            TLS, for later (docs/net.md)
#   LICENSE.txt               the license of OpenSSL, which `tz licenses` prints
#
# Everything it needs comes from pinned downloads checked by their hashes: the OpenSSL sources,
# Strawberry Perl (Configure), NASM (the assembly) and jom (a parallel nmake). The C compiler is
# clang-cl of the LLVM tz is built with (see tz.conf): unlike cl it has __int128, which the fast
# 64-bit code of P-256 and Ed25519 needs. A finished build is kept in -Cache under the hash of this
# recipe, so a fresh build tree of tz copies it instead of building again.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Out <dir> -LlvmBin <LLVM bin>
#              [-Cache <dir>]

param(
  [Parameter(Mandatory = $true)][string]$Out,
  [Parameter(Mandatory = $true)][string]$LlvmBin,
  [string]$Cache = (Join-Path $env:LOCALAPPDATA 'typezero\deps')
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue' # Invoke-WebRequest is slow with the progress bar

$Version = '3.5.9'
$Sources = @{
  Url    = "https://github.com/openssl/openssl/releases/download/openssl-$Version/openssl-$Version.tar.gz"
  File   = "openssl-$Version.tar.gz"
  Sha256 = '603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a'
}
# The tools, with the hashes vcpkg pins for them (scripts/cmake/vcpkg_find_acquire_program).
$Perl = @{
  Url    = 'https://github.com/StrawberryPerl/Perl-Dist-Strawberry/releases/download/SP_54221_64bit/strawberry-perl-5.42.2.1-64bit-portable.zip'
  File   = 'strawberry-perl-5.42.2.1-64bit-portable.zip'
  Sha512 = 'e37c541bb6c4f1c0187bf8ba22b19ce9ead87f6bd1e68c05e7fc4eb2c10a4183c3ee732bf1b26071939f525861fae62b6bef04100b31504ab50fffe7526f84e3'
}
$Nasm = @{
  Url    = 'https://www.nasm.us/pub/nasm/releasebuilds/3.01/win64/nasm-3.01-win64.zip'
  File   = 'nasm-3.01-win64.zip'
  Sha512 = '771c238ddb17c98d5736ccaba4ade1d1601d896f09e588489cb43a4f6381bc0ae14d1869f5316fe94f847f54867e65cf12665529b1e7ad88e5e7d3e162719a4f'
}
$Jom = @{
  Url    = 'https://download.qt.io/official_releases/jom/jom_1_1_7.zip'
  File   = 'jom_1_1_7.zip'
  Sha512 = 'f48ffee06d10012100adf7bf89059a5fee17d97a7f7cb25d0e216be939c366f26014688107d9ed4ce43845a2bea2601d22813df462358c87449723e297f14677'
}

# What node 24 turns off by default (the algorithms of the legacy provider: in node they fail with
# ERR_OSSL_EVP_UNSUPPORTED) and what the API of node does not use. What works in node by default
# stays: 3DES, DH, DSA, SM2/SM3/SM4, RIPEMD-160, Camellia, ARIA.
$Options = @(
  'no-shared', 'no-module', 'no-apps', 'no-tests', 'no-docs', 'no-makedepend',
  'no-legacy', 'no-rc2', 'no-rc4', 'no-rc5', 'no-bf', 'no-cast', 'no-idea', 'no-seed', 'no-md4',
  'no-mdc2', 'no-whirlpool',
  'no-ssl3', 'no-ssl3-method', 'no-dtls', 'no-srp', 'no-cms', 'no-cmp', 'no-engine',
  'no-deprecated', 'no-comp', 'no-ui-console',
  # A program reads no openssl.cnf it did not ask for.
  'no-autoload-config',
  'enable-ec_nistp_64_gcc_128'
)

function Get-Download($item, $dir) {
  $path = Join-Path $dir $item.File
  $algorithm = if ($item.Sha256) { 'SHA256' } else { 'SHA512' }
  $expected = if ($item.Sha256) { $item.Sha256 } else { $item.Sha512 }
  $ok = (Test-Path $path) -and ((Get-FileHash $path -Algorithm $algorithm).Hash -eq $expected)
  if (-not $ok) {
    Write-Host "openssl: downloading $($item.Url)"
    New-Item -ItemType Directory -Force $dir | Out-Null
    Invoke-WebRequest -Uri $item.Url -OutFile "$path.part" -UseBasicParsing
    $actual = (Get-FileHash "$path.part" -Algorithm $algorithm).Hash
    if ($actual -ne $expected) {
      Remove-Item "$path.part"
      throw "openssl: $($item.File) has the $algorithm hash $actual, expected $expected"
    }
    Move-Item -Force "$path.part" $path
  }
  return $path
}

# Unpacks an archive into a directory of its own, once.
function Expand-Once($archive, $dir) {
  if (-not (Test-Path (Join-Path $dir '.unpacked'))) {
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory -Force $dir | Out-Null
    & "$env:SystemRoot\System32\tar.exe" -xf $archive -C $dir
    if ($LASTEXITCODE -ne 0) { throw "openssl: cannot unpack $archive" }
    New-Item -ItemType File (Join-Path $dir '.unpacked') | Out-Null
  }
}

function Invoke-Checked($exe) {
  & $exe @args
  if ($LASTEXITCODE -ne 0) { throw "openssl: '$exe $args' failed with exit code $LASTEXITCODE" }
}

# The MSVC environment (the headers and libraries of the CRT and the SDK, nmake), unless the caller
# already runs in one.
function Enter-Msvc {
  if ($env:INCLUDE -and $env:LIB) { return }
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $vs) { throw 'openssl: no Visual Studio with the C++ tools found' }
  $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
  foreach ($line in (cmd /c "`"$vcvars`" >nul && set")) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] }
  }
}

$config = Join-Path $PSScriptRoot 'tz.conf'
$directList = Join-Path $PSScriptRoot 'direct.txt'
$hashes = ($PSCommandPath, $config, $directList | ForEach-Object { (Get-FileHash $_ -Algorithm SHA256).Hash }) -join ''
$recipe = (Get-FileHash -InputStream ([IO.MemoryStream]::new([Text.Encoding]::UTF8.GetBytes($hashes))) `
  -Algorithm SHA256).Hash.Substring(0, 16)
$stamp = Join-Path $Out 'recipe.txt'
if ((Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $recipe)) { exit 0 }

# The global names each object of a library defines and needs, from llvm-nm.
function Get-Symbols($path) {
  $objects = @{}
  $current = $null
  foreach ($line in (& (Join-Path $LlvmBin 'llvm-nm.exe') $path 2>$null)) {
    if ($line -match '^(.+\.obj):$') {
      $current = @{ Defined = @{}; Needed = @{} }
      $objects[($Matches[1] -split '\\')[-1]] = $current
    } elseif ($current -and $line -match '^\s+U (\S+)$') {
      $current.Needed[$Matches[1]] = $true
    } elseif ($current -and $line -match '^[0-9a-f]+ [TDRBCV] (\S+)$') {
      $current.Defined[$Matches[1]] = $true
    }
  }
  return $objects
}

# libcrypto_direct.lib: the objects direct.txt names, and a stub for each name of the rest of
# libcrypto that they need.
function New-DirectLibrary($build, $work, $built) {
  $library = Join-Path $build 'libcrypto.lib'
  $wanted = Get-Content $directList | Where-Object { $_ -and -not $_.StartsWith('#') }
  $dir = Join-Path $work 'direct'
  New-Item -ItemType Directory -Force $dir | Out-Null
  $members = & (Join-Path $LlvmBin 'llvm-ar.exe') t $library
  Push-Location $dir
  try {
    foreach ($name in $wanted) {
      $member = $members | Where-Object { ($_ -split '\\')[-1] -eq $name }
      if (-not $member) { throw "openssl: libcrypto.lib has no $name (direct.txt)" }
      Invoke-Checked (Join-Path $LlvmBin 'llvm-ar.exe') x $library $member
    }
    $all = Get-Symbols $library
    $everywhere = @{}
    foreach ($object in $all.Values) { foreach ($s in $object.Defined.Keys) { $everywhere[$s] = $true } }
    $defined = @{}
    foreach ($name in $wanted) { foreach ($s in $all[$name].Defined.Keys) { $defined[$s] = $true } }
    $missing = @()
    foreach ($name in $wanted) {
      foreach ($s in $all[$name].Needed.Keys) {
        if ($everywhere[$s] -and -not $defined[$s] -and $missing -notcontains $s) { $missing += $s }
      }
    }
    $stubs = @(
      '/* Generated by third_party/openssl/build.ps1: the names the objects of the direct layer',
      '   need from the rest of libcrypto, which the code of tz never reaches. */',
      '#include <stdio.h>',
      '#include <stdlib.h>',
      'static void missing(const char *name) {',
      '  fputs("tz: internal error: OpenSSL called ", stderr);',
      '  fputs(name, stderr);',
      '  fputs(", which is not in its direct layer\n", stderr);',
      '  abort();',
      '}'
    )
    foreach ($s in ($missing | Sort-Object)) { $stubs += "void $s(void) { missing(`"$s`"); }" }
    Set-Content -Path 'stubs.c' -Value $stubs
    Invoke-Checked (Join-Path $LlvmBin 'clang-cl.exe') /nologo /c /O2 /MT /Zl /Gy /Gw stubs.c
    $objects = (Get-ChildItem -Filter '*.obj').Name
    Invoke-Checked (Join-Path $LlvmBin 'llvm-lib.exe') /nologo (
      '/out:' + (Join-Path $built 'lib\libcrypto_direct.lib')) @objects
  } finally {
    Pop-Location
  }
}

$downloads = Join-Path $Cache 'downloads'
$built = Join-Path $Cache "openssl-$Version-$recipe"
if (-not (Test-Path (Join-Path $built 'recipe.txt'))) {
  $tools = Join-Path $Cache 'tools'
  Expand-Once (Get-Download $Perl $downloads) (Join-Path $tools 'strawberry-perl-5.42.2.1')
  Expand-Once (Get-Download $Nasm $downloads) (Join-Path $tools 'nasm-3.01')
  Expand-Once (Get-Download $Jom $downloads) (Join-Path $tools 'jom-1.1.7')
  $work = Join-Path $Cache "work-$recipe"
  foreach ($dir in $work, $built) { if (Test-Path $dir) { Remove-Item -Recurse -Force $dir } }
  Expand-Once (Get-Download $Sources $downloads) (Join-Path $work 'src')
  $src = Join-Path $work "src\openssl-$Version"

  Enter-Msvc
  $env:PATH = @(
    (Join-Path $tools 'strawberry-perl-5.42.2.1\perl\bin'),
    (Join-Path $tools 'nasm-3.01\nasm-3.01'),
    (Join-Path $tools 'jom-1.1.7'),
    $LlvmBin,
    $env:PATH
  ) -join ';'
  $build = Join-Path $work 'build'
  New-Item -ItemType Directory -Force $build | Out-Null
  Push-Location $build
  try {
    Write-Host "openssl: building OpenSSL $Version (once; kept in $Cache)"
    Invoke-Checked perl (Join-Path $src 'Configure') "--config=$config" tz-win64 @Options `
      "--prefix=$built" "--openssldir=$built\ssl"
    Invoke-Checked jom /NOLOGO /J ([Environment]::ProcessorCount) build_libs
  } finally {
    Pop-Location
  }
  # What install_dev installs, without the debug information it expects.
  $include = Join-Path $built 'include\openssl'
  New-Item -ItemType Directory -Force $include, (Join-Path $built 'lib') | Out-Null
  Get-ChildItem (Join-Path $src 'include\openssl\*.h') | Where-Object { $_.Name -notmatch '__DECC_' } |
    Copy-Item -Destination $include
  Copy-Item (Join-Path $build 'include\openssl\*.h') $include
  Copy-Item (Join-Path $build 'libcrypto.lib'), (Join-Path $build 'libssl.lib') (Join-Path $built 'lib')
  Copy-Item (Join-Path $src 'LICENSE.txt') $built
  foreach ($header in 'crypto\modes.h', 'crypto\poly1305.h', 'crypto\sha.h', 'internal\sha3.h',
                      'internal\sm3.h', 'internal\deprecated.h') {
    $to = Join-Path $built "private\$header"
    New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null
    Copy-Item (Join-Path $src "include\$header") $to
  }
  New-DirectLibrary $build $work $built
  Set-Content -Path (Join-Path $built 'recipe.txt') -Value $recipe
  Remove-Item -Recurse -Force $work
}

if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($part in 'include', 'private', 'lib', 'LICENSE.txt') {
  Copy-Item -Recurse (Join-Path $built $part) $Out
}
Set-Content -Path $stamp -Value $recipe
