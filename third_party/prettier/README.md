# Prettier для расширения VS Code

Форматтер файлов `.tz` в расширении [editors/vscode](../../editors/vscode/README.md): `install.ps1`
копирует этот каталог в расширение как `prettier`. В программы и в `tz` он не попадает.

| Пакет | Версия | Лицензия |
|---|---|---|
| `prettier` | 3.9.10 | MIT ([LICENSE](LICENSE); лицензии того, что собрано внутрь, — [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)) |

Из пакета npm (`npm pack prettier@3.9.10`) взяты без изменений только нужные файлы:
`standalone.js` (Prettier без доступа к диску), `plugins/typescript.js` (разбор TypeScript),
`plugins/estree.js` (печать), `LICENSE` и `THIRD-PARTY-NOTICES.md`.

Чтобы обновить: распаковать новую версию, заменить эти файлы, поправить таблицу и прогнать
тест `cli/format`: он проверяет, что Prettier разбирает все файлы `.tz` репозитория.
