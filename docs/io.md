# Ввод-вывод

Решения, принятые 23 октября 2026 года, и поправки 26 октября: ошибки ОС — `Result`, а не
исключение (раздел 5, [errors.md](errors.md)). Шаги 1–3 плана сделаны 24 октября, шаг 5 — 26–27 октября; что осталось —
в конце, в разделе «План реализации».

## Коротко

- Библиотека — API node: `process`, `console`, `node:fs`, `node:path`, `node:os`. Делаем ту
  часть, что нужна первой; на остальное — ошибка «пока не поддерживается».
- Своё — только то, чего в node нет совсем, в модулях `"tz:..."`: `readLine` из `"tz:io"`,
  HTTP-сервер `Server` из `"tz:http"` ([net.md](net.md)).
- Ошибки ОС (нет файла, нет прав и т.п.) — `Result`: у каждой функции `node:fs` свой тип
  ошибки — `readFileSync` отдаёт `Result<string, ReadFileError>`, `renameSync` —
  `Result<void, RenameError>` (раздел 5): по варианту на причину, `kind` говорит и что
  делали, и почему не вышло.
- `process.argv` — как в node: аргументы начинаются с индекса 2.
- Сейчас — только синхронные функции и только текст.

## 1. Библиотека

```ts
import { readFileSync, writeFileSync } from "node:fs"
import { join } from "node:path"
import { readLine } from "tz:io"

const [input, output] = process.argv.slice(2)
const text = readFileSync(input, "utf8")         // Result<string, ReadFileError>
if (text.isErr()) {
  console.error(text.error.message)
  process.exit(1)
}
const w = writeFileSync(join("out", output), text.value.toUpperCase())
if (w.isErr()) {
  console.error(w.error.message)
  process.exit(1)
}
```

- **Почему API node.** Скрипт для node компилируется, когда его файлы переименованы в `.tz`
  ([modules.md](modules.md), раздел 2), примеры из документации node и из интернета подходят,
  типы уже есть (`@types/node`). Ту же программу можно запустить в node (копию с расширением
  `.ts`) и сравнить вывод — так работают наши тесты.
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
- **`process.exit(code)`** выходит сразу. Всё, что уже напечатано, дописывается.
  `process.exitCode = 1` задаёт код выхода при обычном завершении. Выход с ошибкой пишут так:
  `if (r.isErr()) { console.error(r.error); process.exit(1) }` — отдельной короткой записи нет
  ([errors.md](errors.md), раздел 3).
- **Коды выхода:** 0 — обычное завершение; 1 и другие — то, что программа передала в
  `process.exit`; 2 — переполнение стека, нехватка памяти и другие сбои рантайма
  ([errors.md](errors.md), раздел 9). Не пойманных исключений нет.
- Ещё `process.cwd()`, `process.platform`, `process.execPath`.
- **Сигналы** (решено и сделано 29 октября): `process.on(signal, listener)`, `process.once`,
  `process.off` — только для `"SIGINT" | "SIGTERM" | "SIGHUP" | "SIGBREAK"`; слушатель —
  `() => void` или `async () => …`. Есть слушатель сигнала — процесс по нему сам не выходит
  (решает программа, как в node); живой программу слушатель не держит (как `unref` в node).
  Несколько слушателей вызываются по порядку подписки. Windows: `SIGINT` — Ctrl+C, `SIGBREAK` —
  Ctrl+Break, `SIGHUP` — закрытие окна консоли (после него у программы около 5 секунд);
  `SIGTERM` не приходит никогда. Linux — с переносом рантайма. Остальные события `process`
  (`"exit"`, `"uncaughtException"`…) — ошибки сборки.

## 3. Вывод

- `console.log` / `info` / `debug` пишут в stdout, `console.error` / `warn` — в stderr. Формат
  как в node.
- `process.stdout.write(s)` и `process.stderr.write(s)` пишут без перевода строки,
  `process.stdout.isTTY` говорит, терминал ли это.
- **Буфер.** Вывод в файл или в другую программу копится в буфере и уходит крупными кусками; в
  терминал — по строкам. Так быстрее: в Go, например, каждый `fmt.Println` — отдельный вызов
  ОС. Буфер сбрасывается при любом выходе (обычном, `process.exit`, сбое рантайма), перед
  чтением ввода и перед записью в stderr. Последнее нужно, чтобы при
  `prog > out.txt 2>&1` строки из stdout и stderr шли в том же порядке, что в node.
- **Логи не теряются** (29 октября): буфер сбрасывается и тогда, когда программе нечего делать —
  перед ожиданием сети, таймера, ввода. Сервер, который пишет в файл, трубу, Docker или systemd,
  виден сразу, и убитый процесс (`kill -9`, `TerminateProcess`) теряет только напечатанное в
  последний миг работы. Ctrl+C, Ctrl+Break и закрытие окна дописывают буфер до смерти процесса.
- **Windows:** русские буквы работают в консоли (вывод и ввод), в аргументах, переменных
  окружения и путях. Разделитель путей — `\`, как у node в Windows.

## 4. Ввод

```ts
import { readFileSync } from "node:fs"
import { readLine } from "tz:io"

const all = readFileSync(0, "utf8")      // весь ввод разом; 0 — stdin; Result<string, ReadFileError>

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

Решено 30 октября (4.6, шаг 1; все решения — `tmp/design/fs-errors.tz`, Ф1–Ф10).

```ts
import { readFileSync, ReadFileError } from "node:fs"

function readConfig() {
  const text = readFileSync("config.txt", "utf8")   // Result<string, ReadFileError>
  if (text.isErr()) {
    if (text.error.kind == "READ_FILE_ERROR_NOT_FOUND") {
      return ok(defaults)           // нет файла — беру настройки по умолчанию
    }
    return text                     // остальное — наверх, место ошибки сохраняется
  }
  return parseConfig(text.value)
}

function advice(e: ReadFileError): string {
  switch (e.kind) {                 // полный switch: шесть причин чтения
    case "READ_FILE_ERROR_NOT_FOUND": return "use the defaults"
    case "READ_FILE_ERROR_WRONG_TYPE": return "ask for a file, not a folder"
    case "READ_FILE_ERROR_PERMISSION": return "tell the user"
    case "READ_FILE_ERROR_BUSY": return "try again later"
    case "READ_FILE_ERROR_PATH": return "fix the input"
    case "READ_FILE_ERROR_SYSTEM": return "log it and stop"
  }
}
```

- **Тип на функцию.** У каждой функции свой тип ошибки — объединение вариантов, по варианту на
  причину; у `readFileSync` и `readFile` из `node:fs/promises` — один (`ReadFileError`).
  Типы: `ReadFileError`, `WriteFileError`, `AppendFileError`, `TruncateError`, `AccessError`,
  `ChangeModeError` (chmod), `StatusError` (stat), `LinkStatusError` (lstat),
  `ReadDirectoryError`, `RealPathError`, `MakeDirectoryError`, `MakeTemporaryDirectoryError`,
  `RemoveDirectoryError`, `UnlinkError`, `RemoveError` (rm), `RenameError`, `CopyFileError`.
  Их импортируют из `node:fs` или `node:fs/promises` (`import { ReadFileError } from
  "node:fs"`), глобальных нет; писать их руками почти не нужно — тип результата tz выводит сам.
  Общего объединения «любая ошибка файлов» нет: кому нужно, пишет `ReadFileError | RenameError`.
- **Опции-литералы меняют тип**: `flag: "wx"` — `WriteNewFileError` (есть причина `EXISTS`),
  `"ax"` — `AppendNewFileError`, `{ throwIfNoEntry: false }` — `StatusIfExistsError` /
  `LinkStatusIfExistsError` (нет `NOT_FOUND`: нет файла — `undefined`), `{ force: true }` —
  `ForceRemoveError` (нет `NOT_FOUND`).
- **Поля**: `kind`, `code`, `message`, `syscall`, `path`; у `renameSync`, `copyFileSync` и
  `replaceSync` ещё `dest`.
- **Потоки файлов** (30 октября, Т2): `createReadStream` — `ReadStream` типа
  `Readable<…, ReadStreamError>`, `createWriteStream` — `Writable<Buffer, WriteStreamError>`;
  причины — как у `ReadFileError` / `WriteFileError` и ещё `INVALID_RANGE` (`start` / `end` из
  переменных не целые или `start > end`); варианты — интерфейсы `extends StreamError`
  (`ReadStreamErrorNotFound`…), поэтому поток файла идёт и туда, где ждут любой поток
  (`Readable<string>`). Тексты те же: `could not read a.log: there is no such file`.
  `flags` с `x` литералом (`{ flags: "wx" }`) — `WriteNewStream`, `Writable<Buffer,
  WriteNewStreamError>`, с причиной `EXISTS` (31 октября).
  - `kind` уникален во всей библиотеке — имя типа и причина: `READ_FILE_ERROR_NOT_FOUND`,
    `RENAME_ERROR_EXISTS`. Одна проверка `kind` говорит, какая функция сломалась и почему, и
    сужает тип: после `kind == "RENAME_ERROR_UNSUPPORTED"` есть `dest`, а `code` — только
    `"EXDEV"`. Причины: `NOT_FOUND`, `EXISTS`, `NOT_EMPTY`, `WRONG_TYPE` (папка вместо файла и
    наоборот), `PERMISSION` (в Windows «доступ запрещён» — `EPERM`, в Linux — `EACCES`; `kind`
    собирает оба), `BUSY`, `FULL`, `PATH` (плохой путь), `UNSUPPORTED` (только перенос на другой
    диск), `SYSTEM`. У функции — только её причины и только её коды: у чтения нет `EROFS`.
  - `code` — системное имя, как в node (`"ENOENT"`, `"EACCES"`…): `e.code == "ENOENT"` из кода
    node работает. Неожиданный код (система дала то, чего у функции не ждали) — причина
    `SYSTEM` с кодом `"UNKNOWN"`, настоящий код — в `message`.
  - `message` — что не вышло, где, почему и что сделать: `could not read C:\app\config.json:
    there is no such file`; `could not write C:\app\out\report.txt: the folder C:\app\out
    does not exist; create it with mkdirSync(path, { recursive: true })` (какой папки нет,
    рантайм узнаёт сам, только при ошибке).
  - `syscall` — на какой операции упало: `"open"`, `"read"`, `"mkdir"`…
  - `path` — всегда и всегда полный; у числа вместо пути — `"<stdin>"`, `"<stdout>"`,
    `"<stderr>"`, `"<fd 5>"`.
  - `errno` нет: число node на этой ОС сверх `code` ничего не даёт; `e.errno` — ошибка сборки с
    подсказкой.
- **Перенос и копирование никогда не стирают файл молча**: `renameSync` и `copyFileSync`, если
  под новым именем что-то есть, дают `RENAME_ERROR_EXISTS` / `COPY_FILE_ERROR_EXISTS` (в node —
  заменяют). Проверка «занято ли имя» и перенос — одно действие системы (Windows —
  `MoveFileExW` без замены, Linux — `renameat2(RENAME_NOREPLACE)`). У `copyFileSync` нет
  третьего аргумента (`COPYFILE_EXCL` не нужен). Заменить нарочно — `replaceSync` / `replace` из
  `"tz:fs"` (`ReplaceError`): перенос поверх за один шаг, так файл сохраняют безопасно:
  ```ts
  import { replaceSync } from "tz:fs"
  writeFileSync("config.json.tmp", text)
  replaceSync("config.json.tmp", "config.json")
  ```
- **Ноль в пути**: литерал (`readFileSync("a\0b")`) — ошибка сборки; строка, собранная при
  работе, — `Err` с причиной `PATH` и кодом `ERR_INVALID_PATH`. Имя с запрещёнными в Windows
  знаками (`"a?b.txt"`) — `EINVAL` (`PATH`), а не `ENOENT`, как в node: «нет такого файла» тут
  неправда. `rmSync` папки без `recursive` — `EISDIR` (`WRONG_TYPE`), не `ERR_FS_EISDIR`.
- **Печать** — как объект, плюс место, где `Err` родился:
  ```
  Err {
    error: {
      kind: 'READ_FILE_ERROR_NOT_FOUND',
      code: 'ENOENT',
      message: 'could not read C:\\app\\config.txt: there is no such file',
      syscall: 'open',
      path: 'C:\\app\\config.txt'
    }
  } at readConfig (config.tz:4)
  ```
- **Тесты, которые сравнивают вывод с node,** печатают из ошибки только то, что у node то же
  (`r.error.code`, `r.error.path`): тексты и `rename` поверх файла у tz свои.

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
| глобальные | `console.error` / `warn` / `info` / `debug`, `Date.now`, `performance.now` |

Запуск других программ, сеть и потоки (streams) — вместе с async.

**Редактор.** Объявления tz для tsc ([modules.md](modules.md), раздел 6) дают типы ошибок
функций (`ReadFileError`…), модуль `"tz:io"` и сигнатуры `node:fs` с `Result`; `@types/node` для них не
подходит — там функции бросают.

## План реализации

На каждом шаге: тесты `tests/run` (с аргументами, вводом, переменными окружения и ожидаемым
кодом выхода; `// same-as-node`, где поведение совпадает с node), ошибки в `tests/errors`,
обновление README.

1. **Процесс и вывод** (сделано) — глобальные имена, импорты не нужны:
   - `process.argv`, чтение `process.env`, `exit`, `exitCode`, `cwd()`, `platform`, `execPath`;
   - `console.error` / `warn` / `info` / `debug`, `process.stdout.write` / `stderr.write`,
     `isTTY`;
   - буфер вывода и его сброс;
   - Windows: UTF-8 в консоли, аргументы и переменные окружения в Unicode;
   - `Date.now()`, `performance.now()`.

   `process.env` как полноценный словарь (`Object.keys`, перебор) — после словарей
   ([objects.md](objects.md), шаг 7).
2. **Встроенные модули** (сделано) — после модулей ([modules.md](modules.md), шаг 1):
   - `"node:..."` и те же имена без `node:`;
   - `node:path`, `node:os`, `import { argv, env } from "node:process"`;
   - `readLine` из `"tz:io"` (в Windows — ввод русских букв из консоли);
   - ошибки с подсказками на импорт по умолчанию, `import *`, функции с колбэком и то, чего
     пока нет.
3. **Файлы** (сделано) — после исключений ([errors.md](errors.md), шаг 2):
   - функции `node:fs` из таблицы и `SystemError`;
   - `statSync`, для которого нужны классы;
   - заглушка `SystemError` для запуска тестов в node.
4. **С async** (`node:fs/promises`, `node:readline` сделаны; процессы, сеть и потоки — в своих документах) — `node:fs/promises`, `node:readline`, запуск других программ, сеть, потоки.
5. **Ошибки ОС — `Result`** (сделано 26–27 октября; [errors.md](errors.md), шаг 2): `node:fs`, `node:fs/promises`
   (`Promise<Result>`), `node:readline` отдают `Result<T, SystemError>`; `SystemError` — тип
   объекта вместо класса; убрать заглушку `SystemError` для node; коды выхода без «не пойманного
   исключения»; ресурсы (`FileHandle`) закрываются сами ([errors.md](errors.md), раздел 6).
   Сделано 26 октября: `node:fs` и `node:fs/promises` (путь с нулевым символом — тоже `Err`, с
   кодом node `ERR_INVALID_ARG_VALUE`; неверный `flag` — ошибка компиляции), объявления для
   редактора. 27 октября, с шагом 3 errors.md: `node:readline` (`question` отдаёт `Result`: `Err`,
   если `Interface` закрыт); `SystemError` — тип объекта, а не класс. Отмена через `signal` была и
   ушла 28 октября вместе с `AbortSignal` ([async.md](async.md), раздел 7).
