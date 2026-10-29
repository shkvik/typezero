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
- **Забытый `Result` — ошибка компиляции.** Вызов, чей `Result` никто не берёт (`save(x)` одной
  строкой), и переменная с `Result`, которую потом ни разу не читают (`const r = save(x)` — и
  всё), — с 29 октября и она: `Err` пропал бы молча. То же для переменной с
  `Promise<Result<...>>`, которую не ждут. Читать — любое чтение, хоть внутри стрелки;
  `if (r.isOk())` без `else` разрешён: ошибку посмотрели и решили не разбирать. Экспортируемые
  переменные не проверяются: их читают другие модули.
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
type ConfigError = ReadError | { code: "ERR_INVALID_JSON"; message: string; line: int }

const r = readConfig("a.json")
if (r.isErr()) {
  switch (r.error.code) {
    case "ENOENT":
      console.log(`нет файла ${r.error.path}`)
      break
    case "ERR_INVALID_JSON":
      console.log(`плохой JSON в строке ${r.error.line}`)
      break
    // ошибка: не разобраны "EACCES" и "EISDIR"
  }
}
```

- **Ошибки библиотеки** — объекты с меткой и текстом: `{ code: "ENOENT", message, path }`.
  Имена `SystemError`, `JsonError`, `FetchError` остаются, но это типы объектов, а не классы.
- **`kind` и коды `ERR_…`** (29 октября, [style.md](style.md), раздел 1): у ошибок библиотеки
  есть `kind` — что делать программе (`"REGEX"`, `"URL"`, `"DATE"`, `"LOCALE"`, `"DECIMAL"`,
  `"NUMBER"`, `"URI"`, `"JSON"`, `"ENCODING"`, `"REFLECT"`, `"SERVER"`, `"SUBTLE"`), а `code` — SCREAM_CASE
  с префиксом `ERR_`: `ERR_INVALID_REGEX`, `ERR_INVALID_URL`, `ERR_INVALID_JSON`,
  `ERR_CIRCULAR_STRUCTURE`, `ERR_NO_VARIANT`, `ERR_INVALID_DATA`… (было `bad_regex`, `bad_url`,
  `circular`, `no_variant`, `DataError`). Системные коды (`ENOENT`), коды OpenSSL, `TIMEOUT_ERR` и `UNKNOWN` — как
  есть. Ещё без `kind`: `SystemError`, `StreamError`, `ExecError`, `CryptoError` (`code:
  string`) — их разбор впереди.
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
- **Отмены нет:** `AbortController` и `AbortSignal` убраны 28 октября
  ([async.md](async.md), раздел 7). `setTimeout(100)` из `"node:timers/promises"` отдаёт
  `Promise<void>`; время запроса `fetch` ограничивает опция `timeout` — `Err` с кодом
  `"TIMEOUT_ERR"`.
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
- **Обработчик сервера возвращает только `Response`.** Ошибку он разбирает сам и сам выбирает
  ответ: плохой JSON — 400, повтор — 409, упавшая база — 500. Особой обработки `Result` у
  сервера нет: автоматический 500 на любой `Err` отвечал бы 500 и на ошибки клиента.

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
| `Math.min(...xs)`, `Math.max(...xs)`, `decimal.min/max(...xs)` | то же; без аргументов — ошибка |
| `new Array(n)`, `a.length = n`, `repeat(n)`, `take(n)`, `drop(n)`, `new Uint8Array(n)`, `Buffer.alloc(n)` | `n ≥ 0` |
| `String.fromCodePoint(x)`, `toFixed(n)`, `toString(radix)`, `buf.readUInt32LE(offset)`, `dv.getUint16(offset)`, `u.set(src, offset)`, `u.with(i, x)`, `copy`, `fill`, `write`, виды на `ArrayBuffer` | диапазон доказан или проверен |
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
- `JSON.parse<T>`, `RegExp.parse(source)` (литералы `/…/` и `new RegExp("…")` проверяются при
  сборке; `new RegExp(s)` с переменной — ошибка с подсказкой `RegExp.parse`),
  `URL.parse(text)` (`new URL(text)` — ошибка с подсказкой `URL.parse`), `decodeURIComponent`,
  `atob`;
- `parseInt`, `parseFloat`, `Number(string)`, унарный `+` от строки — вместо `NaN`;
- `Date.parse(text)` — `Date` всегда правильная, геттеры возвращают `int`; `new Date(text)` со
  строкой из переменной — ошибка с подсказкой `Date.parse` ([date.md](date.md), раздел 4);
- `createHash(name)` с именем из переменной, `Intl.Locale.parse(tag)` для языка из переменной
  (конструкторы `Intl` берут готовый `Intl.Locale`; литералы проверяются при сборке).

**Конструктор не может не получиться** — и в библиотеке, и в своих классах. Это следует само:
исключений нет, `new` всегда даёт объект, а забытый `Result` внутри конструктора — ошибка
компиляции. Всё, что может не получиться, конструктор разбирает сам, или это проверяют до него
типом аргумента (`HttpStatus`). Когда само создание может не получиться (разбор текста,
проверка данных), конструктор делают закрытым, а создают объект статической функцией с
`Result` — как `fn new() -> Result<Self, E>` в Rust и `NewX()` в Go:
```ts
class Port {
  private constructor(readonly value: uint16) {}
  static parse(text: string) {
    const n = parseInt(text)
    if (n.isErr() || n.value < 1 || n.value > 65535) return err({ code: "bad_port", text })
    return ok(new Port(n.value.toUint16()))       // Result<Port, { code: "bad_port"; … }>
  }
}
```
Конструктора, который может не получиться, как `init?` в Swift, нет: тип `new X()` зависел бы
от тела конструктора, а `super(...)` в наследнике пришлось бы разбирать.

В библиотеке так же: `URL.parse`, `Date.parse`, `RegExp.parse`, `Intl.Locale.parse`,
`decimal(s)`. `new` с литералом проверяется при сборке; с переменной — ошибка с подсказкой
взять `parse`.

**Ещё отдаёт `Result`:**
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

1. **Result без neverthrow** (сделано 26 октября). Убраны `safeTry`, `yield*` над `Result`,
   `safeUnwrap`, `_unsafeUnwrap`, `fromThrowable`, `fromPromise`; `import` из `"neverthrow"` —
   ошибка с подсказкой. `ResultAsync` заменён на `Promise<Result>`, убраны методы у `Promise`
   (подсказка `(await p).map(...)`), `okAsync`, `errAsync`, `asyncMap`, `asyncAndThen`.
   `return r` для сужённого `Err`. Место создания в `Err` и печать. Ошибка на лишний
   `return await` (внутри `try` пока разрешён — до шага 5). Тесты переписаны.
2. **Библиотека на `Result`** (сделано 26–27 октября). Всё, что сейчас бросает, отдаёт `Result` (раздел 7): `node:fs`,
   `JSON`, `RegExp`, `URL`, `child_process`, `crypto`, `Date`, разбор чисел. Ошибки библиотеки —
   объекты с меткой. Проверки аргументов — типами параметров (`HttpStatus`, литеральные типы,
   перегрузки). Переписать `library/*.tz`: 227 `throw`.
3. **`Promise` без отказа и события** (сделано 27 октября). Убрать `reject`, `.catch`, `.finally`, `Promise.reject`,
   `allSettled`, `any`. Правило подписки на `'error'`; `for await`, `pipeline`, `finished`,
   `once`, `end` — `Result`; `pipe()` и `uncaughtException` — ошибки. `Result` не приводится
   к `void`: `void` над `Promise<Result>` и колбэк с `Result` там, где ждут `void`, — ошибки.
4. **Доказательства вместо падений** (сделано 27 октября; раздел 7, кроме чисел): `x!` —
   ошибка с подсказкой; `reduce`, `reduceRight` (и у итераторов), `Math.min/max(...xs)`,
   `decimal.min/max(...xs)` — `T | undefined`, пока не доказано, что массив не пуст;
   `new Array(n)`, `a.length = n`, `repeat`, `take`, `drop`, длины байтовых массивов — `n ≥ 0`;
   `fromCodePoint`, `toFixed`, `toString(radix)` — в границах; у байтов — смещения `read…` /
   `write…` / `get…` / `set…`, `set`, `with`, `copy`, `compare`, `write`, `fill`, виды на
   `ArrayBuffer`. Доказательства научились: сумме двух слагаемых (`at + src.length <= u.length`),
   длине `const`-массива байтов в любой функции, индексу колбэка (`0 <= i`), диапазону
   значения при объявлении (`const n = c ? 24 : 16`), идиоме `new Uint8Array(x.buffer,
   x.byteOffset, x.byteLength)`, сравнению констант (ветка, которой не бывает, ничего не
   доказывает). Код `library/*.tz` проверяется при выполнении, выход за границы там —
   внутренняя ошибка (код 2). `swap16/32/64` у некратной длины оставляют хвост,
   `encodeURIComponent` пишет одиночный суррогат как U+FFFD.
5. **Без исключений** (сделано 27 октября). `throw`, `try`, `catch`, `finally`, `Error` и его
   виды, `.throw()` у генераторов — ошибки компиляции с подсказкой; код библиотеки пока
   держит `try { } finally { }` без `catch` — уборку генераторов при раннем выходе, до шага 7.
   Рантайм и библиотека на tz не бросают: ошибки — объекты с меткой, внутренние ошибки —
   паника. Убраны анализ «может бросить» (`compiler/lower/errors`), ячейка `tz_thrown`,
   раскрутка и проверки после вызовов, проверки стека в начале функций, класс `Error` в
   компиляторе и рантайме, отказ `Promise` и в рантайме (поле ошибки, список не обработанных
   отказов). Стек — 1 ГБ на поток: резерв главного потока у компоновщика (`/stack`), потоков
   рантайма — в `runtime/core/parallel/parallel.c`. Переполнение — `panic: stack overflow` и
   код 2, как остальные паники. Потоки параллельного рантайма пока есть только в Windows;
   `mmap` с `MAP_NORESERVE` для Linux и стек главного потока там (`setrlimit` или запуск `main`
   на своём стеке) — вместе с ними.
6. **Числа** (сделано 27 октября) — по плану [numbers.md](numbers.md): `int` = 64 бита,
   явные приведения, ошибки вместо предупреждений о переполнении, биты и `Bit.Wrap32`, без
   `NaN` и `Infinity`.
7. **Ресурсы закрываются сами** (сделано 27 октября; раздел 6). Механизм — объект уборки
   библиотеки (`defer(fn)` из `tz:intrinsics`): его освобождение вызывает `fn`. На нём файл потока
   (`createReadStream`, `createWriteStream`) закрывается, когда поток никому не нужен, а
   генераторы библиотеки (`on()`, `body`, перебор потока) делают уборку вместо `finally`.
   `finally` в библиотеке больше нет; разбор `throw` и `try` только называет их ошибкой, узлов
   `ThrowStmt` и `TryStmt`, проверки `throw`/`try`, механики `finally` в построении IR и
   моделирования `try` в анализе параллельности больше нет. Открытия файла (`open()` с
   `FileHandle`) в tz пока нет — закрываться самому ему придётся так же.
8. **Документы** (сделано 26 октября): под эти решения переписаны [async.md](async.md),
   [streams.md](streams.md), [events.md](events.md), [io.md](io.md), [net.md](net.md),
   [date.md](date.md), [small.md](small.md), [bytes.md](bytes.md), [decimal.md](decimal.md),
   [generics.md](generics.md), [overloads.md](overloads.md), [iterators.md](iterators.md),
   [modules.md](modules.md), [process.md](process.md), [regex.md](regex.md),
   [narrowing.md](narrowing.md), [crypto.md](crypto.md), [intl.md](intl.md),
   [decorators.md](decorators.md), [packages.md](packages.md), [objects.md](objects.md). В
   каждом — пометка о поправках 26 октября и шаг плана с работой по ним.
