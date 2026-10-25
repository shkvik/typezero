# Installs the extension of this directory into VS Code for the current user:
# a copy in ~/.vscode/extensions, which VS Code picks up when it starts or
# reloads its window. Run it again after a change; it replaces the copy.
#
#   powershell -ExecutionPolicy Bypass -File editors\vscode\install.ps1

$ErrorActionPreference = "Stop"
$manifest = Get-Content (Join-Path $PSScriptRoot "package.json") -Raw | ConvertFrom-Json
$name = "$($manifest.publisher).$($manifest.name)"
$extensions = Join-Path $env:USERPROFILE ".vscode\extensions"
New-Item -ItemType Directory -Force $extensions | Out-Null

# Earlier versions go away.
Get-ChildItem $extensions -Directory -Filter "$name-*" | Remove-Item -Recurse -Force

$target = Join-Path $extensions "$name-$($manifest.version)"
New-Item -ItemType Directory -Force $target | Out-Null
foreach ($item in "package.json", "language-configuration.json", "syntaxes", "README.md") {
  Copy-Item -Recurse (Join-Path $PSScriptRoot $item) $target
}
Write-Output "installed $name $($manifest.version) into $target; reload VS Code (Developer: Reload Window)"
