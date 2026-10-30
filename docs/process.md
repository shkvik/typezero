# Запуск процессов

Решения, принятые 24 октября 2026 года, и поправка 26 октября: ошибки запуска и ненулевой код
выхода — `Result`, а не исключение ([errors.md](errors.md)). Шаг 1 плана сделан 24 октября; что
осталось — в конце, в разделе «План реализации».

## Коротко

- `node:child_process` — как в node.
- Сначала синхронные функции: `execFileSync`, `execSync`, `spawnSync`.
- Асинхронные — `spawn`, `exec`, `execFile` — как в node, вместе с `EventEmitter`
  ([events.md](events.md)) и потоками node ([streams.md](streams.md)): на них они держатся.
- `exec` и `execSync` идут через оболочку, как в node, — со всеми её возможностями и
  опасностями.
- `fork` — ошибка: второго node в tz нет.

## 1. Синхронные функции

```ts
import { execFileSync, execSync, spawnSync } from "node:child_process"

execFileSync("git", ["rev-parse", "HEAD"], { encoding: "utf8" })  // Result: Ok("a1b2c3…\n")
execSync("git rev-parse HEAD", { encoding: "utf8" })               // то же, через оболочку
const r = spawnSync("git", ["status"], { encoding: "utf8" })        // { status, stdout, stderr, … }
```

- **Тип результата — по `encoding`:** с `encoding: "utf8"` — `string`, без него — `Buffer`
  ([bytes.md](bytes.md)), как в node.
- **Ошибки — `Result`, у каждой функции свой тип** (раздел 3 ниже; решено 30 октября,
  `tmp/design/process-errors.tz`, П0–П7): `execSync` — `ExecTextError` / `ExecBytesError`,
  `execFileSync` — `ExecFileTextError` / `ExecFileBytesError`, `spawnSync` — `SpawnTextError` /
  `SpawnBytesError` (Text — с `encoding`, вывод строкой; Bytes — без него, `Buffer`); с `shell`
  у `execFileSync` и `spawnSync` — `ExecFileShell…` / `SpawnShell…`. `spawnSync` код выхода
  ошибкой не считает: Ok — `SpawnTextResult` / `SpawnBytesResult` (`status`, `signal`,
  `output`, `pid`, `stdout?`, `stderr?`).
- **Опции — как в node:** `cwd`, `env`, `input`, `stdio` (`"pipe"`, `"inherit"`, `"ignore"`),
  `timeout`, `killSignal`, `maxBuffer` (1 МБ, сверх — ошибка `ENOBUFS`), `shell`, `windowsHide`.
- Перед запуском с `stdio: "inherit"` tz сбрасывает свой буфер stdout ([io.md](io.md)), чтобы
  вывод программы и дочернего процесса не перепутался.
- Пока синхронная функция ждёт, поток занят; параллельные async-задачи идут на других потоках
  ([async.md](async.md)).

### Ошибки

```ts
import { execFileSync, ExecFileTextError } from "node:child_process"

const head = execFileSync("git", ["rev-parse", "HEAD"], { encoding: "utf8" })
if (head.isErr()) {
  switch (head.error.kind) {
    case "EXEC_FILE_TEXT_ERROR_PROGRAM_NOT_FOUND_IN_PATH": console.log("install git first"); break
    case "EXEC_FILE_TEXT_ERROR_EXIT": console.log(head.error.status, head.error.stderr); break
    case "EXEC_FILE_TEXT_ERROR_TIMEOUT": console.log(head.error.timeout); break
    default: console.log(head.error.message)
  }
}
```

- **`kind`** — имя типа и причина (`EXEC_FILE_TEXT_ERROR_EXIT`); у варианта только его поля:
  после `EXIT` есть `status: int`, после `TIMEOUT` — `timeout` и `signal`, после
  `OUTPUT_TOO_LARGE` — `stream` (`"stdout"` / `"stderr"`) и `maxBuffer`. Вместо `path` и
  `spawnargs` (имена node) — имена параметров: `command` у `execSync` и `spawnSync`, `file` и
  `args` у `execFileSync` (у `spawnSync` — `command` и `args`). `errno` нет; `syscall` — только
  там, где упал вызов системы: `"spawn"`, `"write"`.
- **Причины** (вариант выбирает рантайм — у разных причин бывает один код):
  - не запустилась: `PROGRAM_NOT_FOUND_IN_PATH` (имя без пути, в `PATH` нет),
    `PROGRAM_FILE_NOT_FOUND` (путь дан, файла нет), `SHELL_NOT_FOUND` (поле `shell`),
    `CWD_NOT_FOUND` / `CWD_NOT_A_FOLDER` (рантайм смотрит папку `cwd` только при ошибке),
    `NO_PERMISSION`, `NEEDS_ADMINISTRATOR`, `BATCH_FILE_WITHOUT_SHELL` (`.bat` без
    `shell: true`), `NOT_A_PROGRAM`, `COMMAND_LINE_TOO_LONG`, `PATH_TOO_LONG`,
    `TOO_MANY_FILES`, `TOO_MANY_PROCESSES`, `OUT_OF_MEMORY`, `SYSTEM`;
  - остановлена: `INPUT_CLOSED`, `TIMEOUT`, `OUTPUT_TOO_LARGE`;
  - кончилась плохо (только `execSync` и `execFileSync`): `EXIT` (`ERR_NONZERO_EXIT`),
    `SHELL_COMMAND_NOT_FOUND` / `SHELL_COMMAND_NOT_RUNNABLE` (`sh` вышла с 127 / 126),
    `CRASHED` (поле `crash`: `SIGSEGV`… или `ACCESS_VIOLATION`, `STACK_OVERFLOW`… в Windows по
    коду выхода), `STOPPED` (`SIGTERM`, `SIGINT`…), `KILLED`;
  - ошибка в значениях: `ZERO_CHARACTER` (поле `argument`: `"args[1]"`, `"cwd"`, `"env.A"`),
    `EMPTY_NAME`, `OUT_OF_RANGE`, `UNKNOWN_SIGNAL`. Те же ошибки в литералах — ошибка сборки
    (`execFileSync("git", [], { timeout: -5 })`, `killSignal: "SIGFOO"`, ноль в аргументе,
    пустое имя, `uid` в Windows); `shell` пишут литералом (`true` / `false`) или строкой: от
    него зависит тип ошибки.
- **Тексты** — что не вышло и почему: `could not run git: there is no such program in PATH;
  install it or give the full path`, `git rev-parse HEAD exited with code 128: fatal: not a git
  repository` (в конце — первая строка stderr), `cmd did not finish in 100 ms and was stopped`.

## 2. Асинхронные функции

`spawn`, `exec`, `execFile` — как в node: `ChildProcess` с событиями (`child.on("close", …)`),
потоки `stdin` / `stdout` / `stderr`, колбэки у `exec` и `execFile`, `kill()`. Опции `signal`
нет: `AbortSignal` убран 28 октября ([async.md](async.md), раздел 7).

```ts
const child = spawn("ffmpeg", ["-i", "in.mp4", "out.webm"], { stdio: "inherit" })
child.on("close", code => console.log("готово", code))
```

Они держатся на `EventEmitter` и потоках node — это отдельная тема, она же нужна для `node:http`
([net.md](net.md)). До тех пор вызов — ошибка с подсказкой взять синхронную функцию.

## 3. Оболочка

- `exec` и `execSync` отдают строку оболочке — `cmd.exe` в Windows, `/bin/sh` в Linux, — как
  в node. Подстановка данных в строку — на совести программиста, как в node:
  `execSync("echo " + userInput)` с `userInput = "hi & echo ВНЕДРЕНО"` выполнит вторую
  команду. Данные безопасно передавать отдельными аргументами: `execFileSync("echo",
  [userInput])` — аргумент уходит как есть.
- В Docker `FROM scratch` оболочки нет: `exec` и `execSync` там дают ошибку `ENOENT`, как node
  без `/bin/sh`. `execFileSync` и `spawnSync` работают.
- Windows: `.bat` и `.cmd` без `shell: true` — `EINVAL`, как в node (так node закрыл дыру в
  2024 году).

## 4. `fork`

`fork("worker.js")` в node запускает ещё один node с каналом сообщений. В tz второго node нет —
ошибка с подсказкой: параллельная работа и так есть в async ([async.md](async.md)); отдельный
процесс — `spawn(process.execPath, […])`.

## План реализации

На каждом шаге: тесты `tests/run` с `// same-as-node`, ошибки в `tests/errors`, обновление
README.

1. **Синхронные функции** (сделано) — `execFileSync`, `execSync`, `spawnSync` со всеми опциями из
   раздела 1, тип результата по `encoding`, исключения по коду выхода; запуск в Windows (правила
   кавычек — как в node) и в Linux (статический exe). Нужны `Buffer` ([bytes.md](bytes.md),
   шаг 2) и исключения ([errors.md](errors.md), шаг 2).
2. **Асинхронные функции** — после `EventEmitter` ([events.md](events.md)) и потоков node
   ([streams.md](streams.md), шаг 3). Событие `error` у `ChildProcess` — по правилу
   [events.md](events.md), раздел 4.
3. **Без исключений** (сделано 26 октября, [errors.md](errors.md), шаг 2): `Result` у
   `execSync`, `execFileSync`, `spawnSync`; `ExecError` — тип объекта с меткой вместо
   `ExecSyncError`. Таймаут и `maxBuffer` у `spawnSync` — тоже `Err` (программа запустилась, но
   её убили); неверные значения опций — `Err` с кодами node (`ERR_INVALID_ARG_VALUE`,
   `ERR_OUT_OF_RANGE`, `ERR_UNKNOWN_SIGNAL`).
