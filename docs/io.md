# Ввод-вывод

Решения, принятые 23 октября 2026 года. Пока ничего из этого не реализовано: порядок работ — в
конце, в разделе «План реализации».

## Коротко

- Библиотека — API node: `process`, `console`, `node:fs`, `node:path`, `node:os`. Делаем ту
  часть, что нужна первой; на остальное — ошибка «пока не поддерживается».
- Своё — только то, чего в node нет совсем, в модулях `"tz:..."`: `readLine` из `"tz:io"`,
  HTTP-сервер `serve` из `"tz:http"` ([net.md](net.md)).
- Ошибки ОС (нет файла, нет прав и т.п.) — исключение `SystemError`, наследник `Error`, с
  полями `code`, `errno`, `syscall`, `path`.
- `process.argv` — как в node: аргументы начинаются с индекса 2.
- Сейчас — только синхронные функции и только текст.

## 1. Библиотека

```ts
import { readFileSync, writeFileSync } from "node:fs"
import { join } from "node:path"
import { readLine } from "tz:io"

const [input, output] = process.argv.slice(2)
const text = readFileSync(input, "utf8")
writeFileSync(join("out", output), text.toUpperCase())
```

- **Почему API node.** Скрипты для node компилируются без правок, примеры из документации node
  и из интернета подходят, типы для редактора уже есть (`@types/node`). Ту же программу можно
  запустить в node и сравнить вывод — так работают наши тесты.
- **Своё** — только там, где в node нужного нет вообще. Живёт в модулях `"tz:..."`, как
  `"tz:string"` для строк.
- **Импорт:** `"node:fs"` и `"fs"` — одно и то же. Только по имени, как все импорты
  ([modules.md](modules.md)): на `import fs from "fs"` и `import * as fs from "node:fs"` —
  ошибка с подсказкой перечислить нужные имена.
- **`process` и `console`** — глобальные, как в node. `import { argv, env } from
  "node:process"` тоже работает.
- **Только синхронные функции.** `node:fs/promises` и `node:readline` — вместе с async. Функции
  с колбэком (`readFile(path, cb)`) — ошибка с подсказкой взять `readFileSync`.
- **Сначала только текст:** `readFileSync(path, "utf8")` или `{ encoding: "utf8" }`. Без
  кодировки node возвращает `Buffer` — это придёт вместе с байтовыми массивами
  ([bytes.md](bytes.md)), до тех пор ошибка с подсказкой. Битый UTF-8 при чтении превращается в
  «�», как в node.

## 2. Процесс

- **`process.argv`** — как в node: `[путь к exe, путь к exe, ...аргументы]`. В node на первых
  двух местах стоят `node.exe` и путь к скрипту; у exe скрипта нет, поэтому оба места занимает
  сам exe. Благодаря этому `process.argv.slice(2)` работает, как в любом скрипте для node.
  Если бы аргументы начинались с индекса 1, как в C и Go, `slice(2)` молча терял бы первый.
- **`process.env.HOME`** — `string | undefined`. Присваивание меняет переменную окружения
  процесса.
- **`process.exit(code)`** выходит сразу: блоки `finally` не выполняются, как в node. Всё, что
  уже напечатано, дописывается. `process.exitCode = 1` задаёт код выхода при обычном завершении.
- **Коды выхода:** 0 — обычное завершение; 1 — не пойманное исключение
  ([errors.md](errors.md)); 2 — сбой самого рантайма.
- Ещё `process.cwd()`, `process.platform`, `process.execPath`.

## 3. Вывод

- `console.log` / `info` / `debug` пишут в stdout, `console.error` / `warn` — в stderr. Формат
  как в node.
- `process.stdout.write(s)` и `process.stderr.write(s)` пишут без перевода строки,
  `process.stdout.isTTY` говорит, терминал ли это.
- **Буфер.** Вывод в файл или в другую программу копится в буфере и уходит крупными кусками; в
  терминал — по строкам. Так быстрее: в Go, например, каждый `fmt.Println` — отдельный вызов
  ОС. Буфер сбрасывается при любом выходе (обычном, `process.exit`, не пойманном исключении,
  сбое рантайма), перед чтением ввода и перед записью в stderr. Последнее нужно, чтобы при
  `prog > out.txt 2>&1` строки из stdout и stderr шли в том же порядке, что в node.
- **Windows:** русские буквы работают в консоли (вывод и ввод), в аргументах, переменных
  окружения и путях. Разделитель путей — `\`, как у node в Windows.

## 4. Ввод

```ts
import { readFileSync } from "node:fs"
import { readLine } from "tz:io"

const all = readFileSync(0, "utf8")      // весь ввод разом; 0 — stdin

const name = readLine("Имя? ")           // печатает вопрос как есть и ждёт строку
console.log("привет,", name)

let line: string | undefined
while ((line = readLine()) != undefined) {    // без вопроса ничего не печатает
  console.log(line.toUpperCase())
}
```

- **`readLine(question?)`** — как `input()` в Python. Печатает вопрос как есть, без
  добавленного пробела, читает строку до перевода строки (`\n` и `\r\n` отрезаются). В конце
  ввода возвращает `undefined`.
- **Почему своя функция.** В node прочитать одну строку без async нельзя. `prompt()` из
  браузеров, Deno и Bun не подходит: в конце ввода он возвращает `null`, а без текста печатает
  `"Prompt "`. Тот же код в Bun вёл бы себя иначе: `while (line != undefined)` не кончался бы.

## 5. Ошибки ОС

```ts
try {
  const text = readFileSync("config.txt", "utf8")
} catch (e) {
  if (e instanceof SystemError && e.code == "ENOENT") console.log("нет файла — беру настройки по умолчанию")
  else throw e
}
```

- **`SystemError`** — встроенный наследник `Error`, глобальный, как `TypeError`. Его поля:
  - `code: string` — `"ENOENT"`, `"EACCES"`, `"EEXIST"`, `"EISDIR"`, `"ENOTDIR"`,
    `"ENOTEMPTY"` и т.д.;
  - `errno: int` — число, которое node даёт на этой ОС;
  - `syscall: string` — например, `"open"`;
  - `path?: string`;
  - `dest?: string` — второй путь у `renameSync` и `copyFileSync`.

  У самого `Error` этих полей нет.
- **Текст и печать — как в node:**
  ```
  Error: ENOENT: no such file or directory, open 'C:\...\config.txt'
      at ... {
    errno: -4058,
    code: 'ENOENT',
    syscall: 'open',
    path: 'C:\\...\\config.txt'
  }
  ```
  `e.name` — `"Error"`, как в node.
- **В node глобального `SystemError` нет:** это только название таких ошибок в документации
  node. Поэтому программа с `instanceof SystemError` в node упадёт с `ReferenceError`. Для
  тестов, которые сравнивают вывод с node, нужна заглушка: перед запуском в node она объявляет
  `SystemError`, а `instanceof SystemError` срабатывает на `Error` со строковыми `code` и
  `syscall` (через `Symbol.hasInstance`).
- **`Result` вместо исключения** — завернуть функцию:
  `Result.fromThrowable((p: string) => readFileSync(p, "utf8"))`.

## 6. Время

`Date.now()` и `performance.now()` — сразу. Полный `Date` (разбор дат, часовые пояса, языки) —
в [date.md](date.md).

## 7. Первый набор

| Откуда | Что |
|---|---|
| `process` | `argv`, `env`, `exit`, `exitCode`, `cwd()`, `platform`, `execPath`, `stdout.write`, `stderr.write`, `stdout.isTTY` |
| `node:fs` | `readFileSync`, `writeFileSync`, `appendFileSync`, `existsSync`, `readdirSync`, `mkdirSync`, `rmSync`, `renameSync`, `copyFileSync`, `statSync` (`isFile()`, `isDirectory()`, `size`, `mtimeMs`) |
| `node:path` | `join`, `resolve`, `dirname`, `basename`, `extname`, `relative`, `isAbsolute`, `sep` |
| `node:os` | `EOL`, `platform`, `homedir`, `tmpdir` |
| `tz:io` | `readLine` |
| глобальные | `console.error` / `warn` / `info` / `debug`, `SystemError`, `Date.now`, `performance.now` |

Запуск других программ, сеть и потоки (streams) — вместе с async.

**Редактор.** Файл объявлений tz ([modules.md](modules.md), раздел 6) добавит `SystemError` и
модуль `"tz:io"`. В настройках tsc для tz выключен `useUnknownInCatchVariables`: в tz `e` в
`catch` всегда `Error`, а не `unknown`.

## План реализации

На каждом шаге: тесты `tests/run` (с аргументами, вводом, переменными окружения и ожидаемым
кодом выхода; `// same-as-node`, где поведение совпадает с node), ошибки в `tests/errors`,
обновление README.

1. **Процесс и вывод** — глобальные имена, импорты не нужны:
   - `process.argv`, чтение `process.env`, `exit`, `exitCode`, `cwd()`, `platform`, `execPath`;
   - `console.error` / `warn` / `info` / `debug`, `process.stdout.write` / `stderr.write`,
     `isTTY`;
   - буфер вывода и его сброс;
   - Windows: UTF-8 в консоли, аргументы и переменные окружения в Unicode;
   - `Date.now()`, `performance.now()`.

   `process.env` как полноценный словарь (`Object.keys`, перебор) — после словарей
   ([objects.md](objects.md), шаг 7).
2. **Встроенные модули** — после модулей ([modules.md](modules.md), шаг 1):
   - `"node:..."` и те же имена без `node:`;
   - `node:path`, `node:os`, `import { argv, env } from "node:process"`;
   - `readLine` из `"tz:io"` (в Windows — ввод русских букв из консоли);
   - ошибки с подсказками на импорт по умолчанию, `import *`, функции с колбэком и то, чего
     пока нет.
3. **Файлы** — после исключений ([errors.md](errors.md), шаг 2):
   - функции `node:fs` из таблицы и `SystemError`;
   - `statSync`, для которого нужны классы;
   - заглушка `SystemError` для запуска тестов в node.
4. **С async** — `node:fs/promises`, `node:readline`, запуск других программ, сеть, потоки.
