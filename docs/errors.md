# Ошибки: Result без исключений

Решения, принятые 26 октября 2026 года. Они заменяют решения 23 октября: исключения и «паник
нет» уходят. План переделки — в конце, в разделе «План реализации».

## Коротко

- **Исключений нет.** `throw`, `try`, `catch`, `finally` и класс `Error` убраны. Ошибка — это
  значение `Result<T, E>`, а `E` — любой тип, какой выберет программист.
- **Ошибки в программе ловит компилятор.** Каждая опасная операция (деление, переполнение,
  `sqrt` от отрицательного, пустой `reduce`…) либо доказана при сборке, либо проверена кодом
  через `if`. Логической ошибке упасть негде.
- **Фатальны только переполнение стека и нехватка памяти**, как в Go и Rust.
- **Ранний выход** — обычный `if (r.isErr()) return r`.
- **`Promise` не отклоняется.** Асинхронная операция, которая может не получиться, —
  `Promise<Result<T, E>>`.
- **Ресурсы закрываются сами**, когда на них не осталось ссылок.
- **Числа** ([numbers.md](numbers.md)). `int` — ровно 64 бита, есть точные типы `int8` …
  `uint64`, переполнение без доказательства — ошибка компиляции, любое изменение типа —
  явно (`x.toInt32()`). Счёт с заворотом — `Bit.Wrap32` и `Bit.Wrap64`. `NaN` и `Infinity`
  в языке нет.

## 1. Result

```ts
type ParseError = { code: "empty" } | { code: "not_a_number"; text: string }

function parseAge(s: string): Result<int, ParseError> {
  if (s.length == 0) return err({ code: "empty" })
  const n = parseInt(s)
  if (n.isErr()) return err({ code: "not_a_number", text: s })
  return ok(n.value)
}

const text = parseAge("42").match(
  (n) => `возраст ${n}`,
  (e) => `ошибка: ${e.code}`,
)
```

- **Встроен, без импорта.** `Result`, `Ok`, `Err`, `ok`, `err` — встроенные имена. Свои
  переменные с теми же именами (`const ok = ...`) перекрывают их, как обычно. `import` из
  `"neverthrow"` — ошибка с подсказкой «встроено, импорт не нужен»: с neverthrow мы больше не
  совместимы.
- **Значение, а не объект в куче.** `Result` хранится как метка Ok/Err и значение, без выделения
  памяти. Цепочка `.map(...).andThen(...).match(...)` компилируется в обычные `if` / `else`,
  лямбды встраиваются.
- **`==` на `Result`** — ошибка компиляции. Сравнивают через `match`, `isOk()`, `value`.
- **`Result` нельзя выбросить нигде — ошибка компиляции.** Ни строкой (`saveFile("")` без
  использования результата), ни через `void`, ни из колбэка, результат которого никто не
  читает (раздел 4). Если результат правда не нужен — отказ пишут явно, например
  `.then(() => {})`: его видно при чтении и можно найти поиском.
- **API:** `ok`, `err`, `Result`, `Ok`, `Err`; `isOk`, `isErr`, `value`, `error`, `map`,
  `mapErr`, `andThen`, `orElse`, `match`, `unwrapOr`, `andTee`, `orTee`, `andThrough`;
  `Result.combine`, `Result.combineWithAllErrors`.
- **Убраны:** `safeTry` и `yield*` над `Result`, `safeUnwrap`, `_unsafeUnwrap` и
  `_unsafeUnwrapErr`, `fromThrowable`, `fromAsyncThrowable`, `fromPromise`, `fromSafePromise`,
  `ResultAsync` и всё, что с ним связано (раздел 4), `asyncMap` и `asyncAndThen`.

## 2. Значение ошибки

Класса `Error` нет. Ошибка — значение любого типа: строковый литерал, объект с меткой, число.
Вид ошибки различают по метке через `switch`, а не через `instanceof`; компилятор проверяет,
что разобраны все варианты.

```ts
type ReadError = { code: "ENOENT" | "EACCES" | "EISDIR"; message: string; path: string }
type ConfigError = ReadError | { code: "bad_json"; message: string; line: int }

const r = readConfig("a.json")
if (r.isErr()) {
  switch (r.error.code) {
    case "ENOENT":
      console.log(`нет файла ${r.error.path}`)
      break
    case "bad_json":
      console.log(`плохой JSON в строке ${r.error.line}`)
      break
    // ошибка: не разобраны "EACCES" и "EISDIR"
  }
}
```

- **Ошибки библиотеки** — объекты с меткой и текстом: `{ code: "ENOENT", message, path }`.
  Имена `SystemError`, `JsonError`, `FetchError` остаются, но это типы объектов, а не классы.
- **Место ошибки хранит сам `Err`, а не значение.** `err(...)` запоминает файл, строку и
  функцию — это ссылка на данные в exe, ничего не стоит. Печать:
  `Err { error: 'not_found' } at find (users.tz:13)`. Так в Zig: ошибка — простое значение, а
  откуда она — знает язык. Стека у ошибки нет.

## 3. Ранний выход

```ts
function loadConfig(path: string) {
  const text = readFileSync(path, "utf8")
  if (text.isErr()) {
    return text
  }
  const config = JSON.parse<Config>(text.value)
  if (config.isErr()) {
    return config
  }
  return ok(config.value)
}
// Result<Config, ReadError | JsonError> — ошибки собираются из всех return
```

- **`return text`.** После `if` компилятор знает, что `text` — `Err` и значения в нём нет,
  поэтому его можно вернуть из функции, у которой значение другого типа. Так сохраняется место,
  где ошибка родилась. `return err(text.error)` тоже можно, но тогда местом ошибки станет эта
  строка.
- **Короткой записи нет.** Ни `try expr` (как в Zig), ни `?` (как в Rust), ни `safeTry` с
  `yield*` (как в neverthrow): один способ — `if`. Для одного-двух шагов есть цепочки:
  `parseInt(raw).map((n) => n * 2)`.
- **Выход из программы с ошибкой** — тоже `if`:
  ```ts
  const config = readConfig("a.json")
  if (config.isErr()) {
    console.error(config.error)
    process.exit(1)
  }
  ```

## 4. Async: `Promise` без отказа

`Promise` всегда выполняется. Если операция может не получиться, внутри лежит `Result`:
`Promise<Result<T, E>>`. Отдельного имени (`ResultAsync`) нет.

```ts
async function getUser(id: int) {
  const res = await fetch(`/users/${id}`)
  if (res.isErr()) {
    return res
  }
  return res.value.json<User>()
}
// Promise<Result<User, FetchError | JsonError>>
```

- **Методов `Result` у `Promise` нет.** Сначала `await`, потом методы:
  `(await getUser(id)).map((u) => u.name)`. `getUser(id).map(...)` — ошибка с этой подсказкой.
- **Несколько сразу:** `Promise.all` отдаёт каждый `Result` отдельно;
  `Result.combine(await Promise.all(ps))` — все значения или первая ошибка.
- **Уходят:** `reject` в `new Promise`, `.catch`, `.finally`, `Promise.reject`, `allSettled`,
  `any`. Остаются `resolve`, `then`, `Promise.all`, `race`, `withResolvers` (без `reject`).
  API с колбэком `(e, v)` оборачивают так:
  ```ts
  new Promise<Result<string, ReadError>>((resolve) => {
    oldApi((e, v) => resolve(e ? err(e) : ok(v)))
  })
  ```
- **Лишний `return await x` — ошибка**, и `async () => await x` тоже: без `try` и без отказов
  `await` там ничего не меняет, `async`-функция и так дождётся `Promise`. `return (await x).value`
  — нормально.
- **Отмена:** `signal.throwIfAborted()` уходит, вместо него
  `if (signal.aborted) return err(signal.reason)`. `setTimeout(100, undefined, { signal })` из
  `"node:timers/promises"` отдаёт `Result<void, AbortError>`.
- **`process.on("uncaughtException")` и `process.on("unhandledRejection")`** — ошибка:
  ловить нечего.
- **Задача, которую не ждут, разбирает ошибки сама.** `void` над `Promise<Result<...>>` —
  ошибка: `Err` пропал бы молча. То же для колбэка, результат которого никто не читает
  (слушатель события, таймер, `forEach`): функцию с `Result` нельзя передать туда, где ждут
  `() => void` или `() => Promise<void>`. В TS `void` принимает что угодно, у нас нет. Поэтому
  тип `Promise<void>` сам по себе гарантия: внутри все ошибки разобраны.
  ```ts
  void track(req.url)                  // ошибка: void выбрасывает Result

  async function trackQuietly(url: string) {
    const r = await track(url)
    if (r.isErr()) lostTracks++
  }
  void trackQuietly(req.url)           // ок

  setInterval(async () => cleanup(), 60_000)   // ошибка: таймер не читает Result
  ```
  Так строже, чем где-либо, кроме Zig: node роняет процесс на забытом отказе, Go в `go f()` и
  Swift в `Task { }` теряют ошибку молча, Rust только предупреждает.
- **Обработчик `serve` возвращает только `Response`.** Ошибку он разбирает сам и сам выбирает
  ответ: плохой JSON — 400, повтор — 409, упавшая база — 500. Особой обработки `Result` у
  `serve` нет: автоматический 500 на любой `Err` отвечал бы 500 и на ошибки клиента.

## 5. Событие `'error'` и потоки

В node `emit("error", e)` без слушателя `'error'` бросает `e`, и процесс падает. У нас ошибка
должна прийти туда, где компилятор заставит её разобрать.

- **Правило подписки.** Подписался на события объекта, который может испускать `'error'` (это
  видно по его карте событий), — подпишись и на `'error'` этого же объекта, в той же функции, на
  каждом пути. Проверка по путям — как у полей, которые конструктор задаёт на каждом пути.
  ```ts
  const s = createReadStream("a.txt")
  s.on("data", (chunk) => use(chunk))
  // ошибка: 's' может испустить 'error' (нет файла, нет прав), а слушателя 'error' нет;
  // добавь s.on("error", (e) => ...) или читай через for await
  ```
- **Всё, что ждёт объект, отдаёт `Result`**, и слушатель там не нужен: каждый элемент
  `for await` — `Result`; `pipeline`, `finished`, `once`, `end()` у записываемого потока —
  `Promise<Result<...>>`. `write()` копит, первую ошибку отдаёт `end()`.
- **`pipe()` — ошибка** с подсказкой `pipeline`: `pipe` не передаёт ошибки дальше.
- **Свой `EventEmitter`.** Карта событий выводится из `emit`. Если тип испускает `'error'`, а
  слушателя `'error'` у него нет нигде в программе, — ошибка компиляции: иначе ошибка
  пропадает молча. `emit` возвращает, был ли слушатель, — можно разобрать самому.
- **`stream.destroy(e)`** остаётся: ошибка уходит в `Result` у `for await`, `end` и
  `finished`.

## 6. Ресурсы: уборка без `finally`

Файл, сокет и другие ресурсы закрываются сами, когда на них не осталось ни одной ссылки. При
подсчёте ссылок это точный момент, известный при сборке, а не «когда-нибудь», как у сборщика
мусора.

```ts
async function firstLine(path: string) {
  const file = await open(path)
  if (file.isErr()) {
    return file
  }
  for await (const line of file.value.readLines()) {
    if (line.isErr()) {
      return line              // ← здесь компилятор вставит file.close()
    }
    return ok(line.value)      // ← и здесь
  }
  return ok("")                // ← и здесь
}
```

- **Когда:** в конце блока, где объявлена переменная, как в Rust и C++. Закрыть раньше —
  меньший блок `{ }` или отдельная функция. Не по последнему использованию: тогда объект,
  который держат «просто чтобы был» (замок, охрана мьютекса), отпустился бы сразу.
- **Ресурс отдали дальше** (вернули, положили в `Map`, захватили замыканием) — он закроется у
  последнего владельца.
- **Активное держит рантайм**, как в node: сервер, который слушает порт, таймеры, запущенные
  процессы, операция в полёте. Останавливают явно: `server.close()`, `clearTimeout`, `kill`.
- **`close()` руками** отдаёт `Result`. Нужен в двух случаях: узнать, что запись дошла до диска
  (ошибку вроде «диск полон» видно только при закрытии; обычно проще `writeFile`), и закрыть
  ресурс, который держит долгоживущий объект (в Windows открытый файл нельзя переименовать).
  После `close()` любая операция отдаёт `Err`.
- **Если автоматическое закрытие не удалось**, вернуть ошибку некому: она печатается в stderr,
  программа работает дальше. Так делает Python; Rust молча глотает.

## 7. Что проверяет компилятор вместо падений

Опасная операция либо доказана при сборке, либо проверена кодом. Проверка — внутри функции,
где операция, а не у вызывающих.

| Операция | Что нужно |
|---|---|
| `a / b`, `a % b` | `b ≠ 0`; у дробных — `b` не настолько мал, чтобы частное не влезло ([numbers.md](numbers.md), раздел 10) |
| `x!` | убран: сужение через `if (x == undefined) ...` |
| `xs[i]` | как сейчас: доказан индекс — `T`, иначе `T \| undefined` |
| `reduce` без начального значения | `T \| undefined`, пока не доказано, что массив не пуст |
| `Math.min(...xs)`, `Math.max(...xs)` | то же; без аргументов — ошибка |
| `new Array(n)`, `a.length = n`, `repeat(n)` | `n ≥ 0` |
| `String.fromCodePoint(x)`, `toFixed(n)`, `buf.readUInt32LE(offset)` | диапазон доказан или проверен |
| запись в `Buffer` за пределы | как сейчас: ошибка, если не доказано |
| арифметика целых | результат влезает в тип ([numbers.md](numbers.md), раздел 2) |
| арифметика дробных | не получается `NaN` или `Infinity` ([numbers.md](numbers.md), раздел 10) |

**Где компилятор берёт доказательство:** константы; проверки в `if` (после
`if (b == 0) return ...` известно `b ≠ 0`); диапазоны по всем вызовам функции — компилятор
собирает программу целиком, в tz нет `eval` и подгрузки кода; известные диапазоны результатов
(`xs.length ≥ 0`, `Math.abs(x) ≥ 0`, `Math.random()` — 0 … 1).

**Ошибка называет причину и исправление:**
```
average.tz:2:17: error: xs.length may be 0 here
  fix: check it first, e.g. if (xs.length == 0) return err(...)
```

**Проверка аргументов в библиотеке — типом параметра**, как в Rust (`StatusCode::from_u16`
возвращает ошибку, а `Response` принимает только готовый `StatusCode`). Новых механизмов не
нужно: литеральные типы и функции-проверки из [narrowing.md](narrowing.md).
```ts
type HttpStatus = 200 | 201 | … | 599          // список генерируется

new Response("x", { status: 404 })             // ок: литерал подходит к типу сам
new Response("x", { status: 700 })             // ошибка компиляции
new Response("x", { status: code })            // ошибка, если code — просто int
if (!isHttpStatus(code)) return err({ code: "bad_status" })
new Response("x", { status: code })            // ок: после проверки code — HttpStatus
```
- Что выражается перечнем — литеральный тип: имена алгоритмов webcrypto, длина ключа AES
  `128 | 192 | 256`, статус `redirect` `301 | 302 | 303 | 307 | 308`. Обязательное — поле без `?`.
- Связь двух аргументов — перегрузкой ([overloads.md](overloads.md)): у `Response` один
  конструктор принимает тело и статусы с телом, другой — статусы 101, 204, 205, 304 без тела.
- Ошибка данных и среды (тело уже прочитано, поток закрыт, битый ключ) — `Result`, как везде.

**Что отдаёт `Result`** — всё, что приходит снаружи или из данных:
- `node:fs` и остальной ввод-вывод, `fetch`, `execFileSync`;
- `JSON.parse<T>`, `new RegExp(source)` (литералы `/…/` проверяются при сборке),
  `URL.parse(text)` (`new URL(text)` — ошибка с подсказкой `URL.parse`), `decodeURIComponent`,
  `atob`;
- `parseInt`, `parseFloat`, `Number(string)`, унарный `+` от строки — вместо `NaN`;
- `new Date(string)`, `Date.parse` — `Date` всегда правильная, геттеры возвращают `int`;
- `createHash(name)`, `Intl` с языком или опциями из переменной (литералы проверяются при
  сборке);
- `JSON.stringify` — только если тип может содержать цикл; для дерева по типу — просто `string`.

## 8. Числа

Числа вынесены в [numbers.md](numbers.md). Коротко, что там про ошибки:
- переполнение без доказательства — ошибка компиляции, как и другие опасные операции
  (раздел 7);
- любое изменение типа — явно (`x.toInt32()`), разные целые сравниваются по математике;
- счёт с заворотом для хешей — `Bit.Wrap32` и `Bit.Wrap64`;
- `NaN` и `Infinity` в языке нет: деление, `sqrt`, `log` и переполнение `double` доказываются
  так же; разбор дробных отдаёт `Result`.

## 9. Что фатально

- **Переполнение стека и нехватка памяти** — сообщение в stderr и код выхода 2, как в Go и
  Rust. Проверить их при сборке нельзя: глубина рекурсии зависит от данных.
- **Стек — 1 ГБ на поток**, как предел горутины в Go. Это резерв адресов, а не память: страницы
  выделяются по мере роста стека. У async-задач своего стека нет (они — конечные автоматы),
  поэтому 1 ГБ — только на поток рантайма. Тогда стек кончится только при бесконечной
  рекурсии. Цена: бесконечная рекурсия перед падением успеет занять до 1 ГБ.
- **Проверок стека в начале функций нет:** при фатальном переполнении хватает защитной
  страницы.

## 10. Как это компилируется

- `Result` — метка и значение, без памяти в куче; `Err` хранит ещё указатель на место
  создания.
- Ранний выход — обычный `return`. Ячейки брошенной ошибки, проверки после вызовов, таблиц
  раскрутки и раскрутчика нет.
- Доказательства стоят только время сборки: во время работы остаются лишь те `if`, которые
  написал программист.
- Операции с `double` получают флаги LLVM `nnan` и `ninf` ([numbers.md](numbers.md)).

## 11. Что уходит из языка

| Было | Стало |
|---|---|
| `throw new Error("x")` | `return err(...)` |
| `try { } catch { }`, `try { } finally { }` | `Result` и `if`; уборка — раздел 6 |
| `class MyError extends Error` | тип объекта с меткой |
| `safeTry(function* () { yield* r })` | `if (r.isErr()) return r` |
| `ResultAsync<T, E>` | `Promise<Result<T, E>>` |
| `p.catch(f)`, `Promise.reject(e)` | `Result` внутри `Promise` |
| `x!`, `r._unsafeUnwrap()` | сужение через `if`, `match`, `unwrapOr` |
| `gen.throw(e)` у генераторов | `return()` остаётся |
| `long`, смешение типов, `x \| 0`, `NaN`, `Infinity` | [numbers.md](numbers.md), раздел 11 |
| `stream.pipe(dest)` | `pipeline` |
| `process.on("uncaughtException")` | ловить нечего |
| `return await x` | `return x` |

## План реализации

На каждом шаге: тесты `tests/run` (с `// same-as-node`, где поведение совпадает с JS), ошибки в
`tests/errors`, прогон с `-O0` и `-O2` под `TZ_CHECK_LEAKS=1`, обновление README.

Шаги 1–4 прежнего плана (Result, исключения, паники → исключения, ResultAsync) сделаны
24 октября; переделка опирается на них.

1. **Result без neverthrow.** Убрать `safeTry`, `yield*` над `Result`, `safeUnwrap`,
   `_unsafeUnwrap`, `fromThrowable`, `fromPromise`, `import` из `"neverthrow"`; `ResultAsync`
   заменить на `Promise<Result>`, убрать методы у `Promise`, `okAsync`, `errAsync`,
   `asyncMap`, `asyncAndThen`. `return r` для сужённого `Err`. Место создания в `Err` и печать.
   Ошибка на лишний `return await`. Переписать тесты (8 файлов с `safeTry`, 7 с `ResultAsync`).
2. **Библиотека на `Result`.** Всё, что сейчас бросает, отдаёт `Result` (раздел 7): `node:fs`,
   `JSON`, `RegExp`, `URL`, `child_process`, `crypto`, `Date`, разбор чисел. Ошибки библиотеки —
   объекты с меткой. Проверки аргументов — типами параметров (`HttpStatus`, литеральные типы,
   перегрузки). Переписать `library/*.tz`: 227 `throw`.
3. **`Promise` без отказа и события.** Убрать `reject`, `.catch`, `.finally`, `Promise.reject`,
   `allSettled`, `any`. Правило подписки на `'error'`; `for await`, `pipeline`, `finished`,
   `once`, `end` — `Result`; `pipe()` и `uncaughtException` — ошибки. `Result` не приводится
   к `void`: `void` над `Promise<Result>` и колбэк с `Result` там, где ждут `void`, — ошибки.
4. **Доказательства вместо падений** (раздел 7, кроме чисел): `x!`, `reduce`, `new Array(n)`,
   `repeat`, `fromCodePoint`, `toFixed`, чтение `Buffer`.
5. **Без исключений.** Убрать `throw`, `try`, `catch`, `finally`, `Error` и наследников,
   `.throw()` у генераторов, анализ «может бросить» (`compiler/lower/errors/may_throw`) и
   ячейку `tz_thrown`, проверки стека в начале функций. Стек 1 ГБ на поток
   (`runtime/core/parallel/parallel.c`, `tz_stack_limit` в `runtime/core/output/output.c`,
   резерв главного потока у компоновщика; на Linux — `mmap` с `MAP_NORESERVE` и откат на меньший
   размер). Фатальное переполнение и нехватка памяти — сообщение и код 2.
6. **Числа** — по плану [numbers.md](numbers.md): `int` = 64 бита, явные приведения,
   ошибки вместо предупреждений о переполнении, биты и `Bit.Wrap32`, без `NaN` и
   `Infinity`.
7. **Ресурсы закрываются сами** (раздел 6).
8. **Документы:** переписать [async.md](async.md), [streams.md](streams.md),
   [events.md](events.md), [io.md](io.md), [net.md](net.md), [date.md](date.md),
   [small.md](small.md), [bytes.md](bytes.md), [decimal.md](decimal.md) под эти решения.
