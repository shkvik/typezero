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
foreach ($item in "package.json", "language-configuration.json", "syntaxes", "icons",
                  "extension.js", "format.js", "server.js", "README.md") {
  Copy-Item -Recurse (Join-Path $PSScriptRoot $item) $target
}
# The formatter (format.js) needs Prettier next to it.
Copy-Item -Recurse (Join-Path $PSScriptRoot "..\..\third_party\prettier") (Join-Path $target "prettier")
# The language server's client (server.js) needs vscode-languageclient next to it.
$client = Join-Path $PSScriptRoot "..\..\third_party\vscode-languageclient"
Copy-Item (Join-Path $client "client.js") (Join-Path $target "client.js")
Copy-Item -Recurse (Join-Path $client "licenses") (Join-Path $target "client-licenses")
Write-Output "installed $name $($manifest.version) into $target; reload VS Code (Developer: Reload Window)"
