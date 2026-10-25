# TypeZero для VS Code

Подсветка и форматирование программ на typezero — файлов `.tz`. В строке состояния VS Code язык называется
TypeZero. Синтаксис у tz тот же, что у TypeScript, поэтому
грамматика — TypeScript, которая есть в VS Code: язык `tz` подключает её
([syntaxes/tz.tmLanguage.json](syntaxes/tz.tmLanguage.json)). Скобки, комментарии и отступы —
как у TypeScript ([language-configuration.json](language-configuration.json) — копия файла
VS Code, MIT).

## Форматирование

Alt+Shift+F (Format Document) и Format Selection форматируют файл `.tz` Prettier'ом
([format.js](format.js)): синтаксис у tz тот же, что у TypeScript, поэтому Prettier читает файл
как TypeScript. Настройки — его по умолчанию: отступ 2 пробела, точки с запятой, двойные
кавычки, строки до 80 знаков; концы строк (`\r\n` или `\n`) остаются, как в файле. Расширение
делает себя форматтером файлов `.tz` по умолчанию и ставит им отступ 2 пробела без угадывания по
файлу (`editor.detectIndentation`); при сохранении файл не форматируется. Если в файле
синтаксическая ошибка, он остаётся как есть, а VS Code показывает ошибку со строкой и столбцом.

Prettier лежит в [third_party/prettier](../../third_party/prettier/README.md); `install.ps1`
копирует его в расширение.

## Чего нет

Проверки типов, подсказок и перехода к определению в редакторе нет, ошибки
показывает `tz build`. Объявления для проверки TypeScript лежат рядом с `tz.exe`, в `types`
([docs/modules.md](../../docs/modules.md), раздел 6).

## Установка

Копией в каталог расширений пользователя:

```
powershell -ExecutionPolicy Bypass -File editors\vscode\install.ps1
```

После этого — `Developer: Reload Window`. После правки расширения команду повторяют.
