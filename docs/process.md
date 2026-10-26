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
- **Ошибки — `Result`:** `execSync` и `execFileSync` отдают `Result<string | Buffer,
  ExecError>`. `Err` — если программа не запустилась (`{ code: "ENOENT" }` и другие, как у
  [io.md](io.md)) или вышла с кодом ≠ 0: тогда в ошибке `code: "exit"`, `status`, `stdout`,
  `stderr`. `spawnSync` код выхода ошибкой не считает и возвращает `status`; `Err` у него —
  только если запустить не вышло.
- **Опции — как в node:** `cwd`, `env`, `input`, `stdio` (`"pipe"`, `"inherit"`, `"ignore"`),
  `timeout`, `killSignal`, `maxBuffer` (1 МБ, сверх — ошибка `ENOBUFS`), `shell`, `windowsHide`.
- Перед запуском с `stdio: "inherit"` tz сбрасывает свой буфер stdout ([io.md](io.md)), чтобы
  вывод программы и дочернего процесса не перепутался.
- Пока синхронная функция ждёт, поток занят; параллельные async-задачи идут на других потоках
  ([async.md](async.md)).

## 2. Асинхронные функции

`spawn`, `exec`, `execFile` — как в node: `ChildProcess` с событиями (`child.on("close", …)`),
потоки `stdin` / `stdout` / `stderr`, колбэки у `exec` и `execFile`, `kill()`, `signal`
(`AbortSignal`).

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
3. **Без исключений** ([errors.md](errors.md), шаг 2): `Result` у `execSync`, `execFileSync`,
   `spawnSync`; `ExecError` — тип объекта с меткой вместо `ExecSyncError`.
