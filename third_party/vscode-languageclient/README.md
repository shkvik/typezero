# vscode-languageclient для расширения VS Code

Клиент языкового сервера `tz lsp` в расширении [editors/vscode](../../editors/vscode/README.md):
`install.ps1` копирует `client.js` в расширение, рядом с `server.js`, а `licenses` — как
`client-licenses`. В программы и в `tz` он не попадает.

`client.js` — пакет `vscode-languageclient/node` со всеми зависимостями, собранный в один файл
(esbuild, без изменений в коде пакетов):

| Пакет | Версия | Лицензия |
|---|---|---|
| `vscode-languageclient` | 9.0.1 | MIT ([licenses/vscode-languageclient.txt](licenses/vscode-languageclient.txt)) |
| `vscode-languageserver-protocol` | 3.17.5 | MIT ([licenses/vscode-languageserver-protocol.txt](licenses/vscode-languageserver-protocol.txt)) |
| `vscode-jsonrpc` | 8.2.0 | MIT ([licenses/vscode-jsonrpc.txt](licenses/vscode-jsonrpc.txt)) |
| `vscode-languageserver-types` | 3.17.5 | MIT ([licenses/vscode-languageserver-types.txt](licenses/vscode-languageserver-types.txt)) |
| `minimatch` | 5.1.9 | ISC ([licenses/minimatch.txt](licenses/minimatch.txt)) |
| `semver` | 7.8.5 | ISC ([licenses/semver.txt](licenses/semver.txt)) |
| `brace-expansion` | 2.1.7 | MIT ([licenses/brace-expansion.txt](licenses/brace-expansion.txt)) |
| `balanced-match` | 1.0.2 | MIT ([licenses/balanced-match.txt](licenses/balanced-match.txt)) |

Версия 9.0.1, а не новейшая 11.0.0 (вышла 22 октября 2026): 9.0.1 проверена годами, для
ошибок, наведения, перехода, дополнения и переименования её хватает; ей нужен VS Code 1.82+.

Чтобы обновить (в пустом каталоге):

```
npm install vscode-languageclient@<версия> esbuild
echo module.exports = require("vscode-languageclient/node") > entry.js
npx esbuild entry.js --bundle --platform=node --format=cjs --target=node18 --external:vscode --minify --legal-comments=none --outfile=client.js
```

Затем заменить `client.js`, лицензии в `licenses` (файлы `LICENSE` пакетов из `node_modules`) и
таблицу и прогнать тест `cli/lsp`.
