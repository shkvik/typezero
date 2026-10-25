# typezero для VS Code

Подсветка программ на typezero — файлов `.tz`. Синтаксис у tz тот же, что у TypeScript, поэтому
грамматика — TypeScript, которая есть в VS Code: язык `tz` подключает её
([syntaxes/tz.tmLanguage.json](syntaxes/tz.tmLanguage.json)). Скобки, комментарии и отступы —
как у TypeScript ([language-configuration.json](language-configuration.json) — копия файла
VS Code, MIT).

Только подсветка: проверки типов, подсказок и перехода к определению в редакторе нет, ошибки
показывает `tz build`. Объявления для проверки TypeScript лежат рядом с `tz.exe`, в `types`
([docs/modules.md](../../docs/modules.md), раздел 6).

Установка — копией в каталог расширений пользователя:

```
powershell -ExecutionPolicy Bypass -File editors\vscode\install.ps1
```

После этого — `Developer: Reload Window`. После правки расширения команду повторяют.
