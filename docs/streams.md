# Потоки node

Решения, принятые 24 октября 2026 года, и поправки 26 октября: ошибки потоков — `Result`, без
исключений, `pipe()` убран (раздел 3, [errors.md](errors.md), раздел 5). 28 октября: состояние
потока закрыто, читается методами (`r.readableLength()`), методы, которые пишет свой класс, —
`onRead`, `onWrite`… (раздел 6). Шаги 1–3 плана сделаны
24–25 октября (из шага 3 — без асинхронных функций `node:child_process`), шаг 6 — 27 октября; что осталось — в конце,
в разделе «План реализации».

## Коротко

- `node:stream` — как в node: `Readable`, `Writable`, `Duplex`, `Transform`, `pipeline`,
  `for await`. Код — перенос `lib/internal/streams` из node, как события
  ([events.md](events.md)).
- Ошибка потока приходит туда, где компилятор заставит её разобрать: каждый элемент `for await`
  — `Result`, `pipeline`, `finished` и `end()` отдают `Promise<Result<...>>`, а подписка на
  `data` требует подписки на `error`. `pipe()` нет.
- Тип кусков известен при компиляции: `Readable<T>`. По умолчанию — `Buffer`, с кодировкой —
  `string`, в `objectMode` — то, что кладут в поток.
- `setEncoding` меняет тип кусков у той же переменной дальше по коду. Если поток уже передан
  куда-то ещё — ошибка компиляции.
- Web-потоки (`ReadableStream` для `fetch`) — тоже есть, с переходами между ними, как в node.

## 1. Что это

Данные, которые приходят или уходят кусками: файл в 10 ГБ не читают целиком, вывод процесса и
тело запроса идут, пока те работают.

| Вид | Что | Примеры |
|---|---|---|
| `Readable` | откуда читать | файл, `child.stdout`, тело запроса, `process.stdin` |
| `Writable` | куда писать | файл, `child.stdin`, ответ сервера, `process.stdout` |
| `Duplex` | и то, и другое | сокет |
| `Transform` | посередине меняет | сжатие, шифрование, разбор строк |

```ts
for await (const item of createReadStream("big.log")) {
  if (item.isErr()) return item                 // нет файла, нет прав, диск отвалился
  use(item.value)
}
const r = await pipeline(createReadStream("a.log"), createGzip(), createWriteStream("a.log.gz"))
// r: Result<void, SystemError | ZlibError> — первая ошибка любого из потоков
const out = await text(child.stdout)            // node:stream/consumers, Result<string, …>
```

## 2. Тип кусков: `Readable<T>`

В `@types/node` кусок — `any`, а в node его вид меняется прямо во время работы: без кодировки —
`Buffer`, после `setEncoding("utf8")` — строки, в `objectMode` — любые значения. Собирать строку
из кусков `Buffer` руками нельзя: буква «п» — 2 байта, и если кусок оборвался посередине,
`chunk.toString()` даёт «�». `setEncoding` придерживает половинку буквы до следующего куска.

```ts
createReadStream("a.log")                       // Readable<Buffer>
createReadStream("a.log", "utf8")               // Readable<string>
const users = Readable.from([ann, bob])         // Readable<User>
child.stdout.setEncoding("utf8")
child.stdout.on("data", s => s.length)          // s: string
```

- **По умолчанию** — `Buffer` ([bytes.md](bytes.md)); кодировка при создании (`"utf8"`,
  `{ encoding: "utf8" }`) — `string`.
- **`objectMode`** — тип выводится из `push()` и `Readable.from(…)`, как типы событий из
  `emit` ([events.md](events.md)); можно и явно: `new Readable<User>({ objectMode: true, … })`.
- **`setEncoding`** меняет тип кусков у той же переменной (или того же пути, `child.stdout`)
  дальше по коду.
- **Если поток уже ушёл в другое место** — передан в функцию, сохранён в поле или массиве, на
  нём уже висят слушатели — а потом на нём `setEncoding`, это ошибка компиляции: там тип
  остался бы `Buffer`, а пришли бы строки. Подсказка: задать кодировку раньше или при
  создании.
- `Writable<T>` — так же: что можно писать. `write("текст")` в поток байтов — можно, как в
  node (строка кодируется в UTF-8).
- События описаны: `on("data", chunk => …)` — `chunk: T`, `on("error", e => …)` — `e` —
  объект ошибки с меткой (`SystemError`, `ZlibError` — типы объектов, не классы).

## 3. Ошибки

В node ошибка потока — событие `error`, а без слушателя `error` процесс падает. У нас
исключений и падений нет ([errors.md](errors.md), раздел 5), поэтому:

- **Всё, что ждёт поток, отдаёт `Result`:** каждый элемент `for await` — `Result<T, E>` (как
  `lines()` в Rust: ошибку не пропустить); `pipeline`, `finished`, `once`, `text` / `json` /
  `buffer` из `node:stream/consumers` — `Promise<Result<...>>`. Слушатель `error` там не нужен.
- **Запись:** `write()` копит и запоминает первую ошибку, `end()` отдаёт её —
  `await w.end()` даёт `Result<void, SystemError>`; забытый `Result` — ошибка компиляции.
- **Подписка на события** (`on("data")`, `on("end")`) у потока, который может испустить
  `error`, требует подписки на `error` в той же функции на каждом пути — иначе ошибка
  компиляции с подсказкой: `on("error", …)` или `for await`.
- **`pipe()` нет** — ошибка с подсказкой `pipeline`: `pipe` не передаёт ошибку дальше, источник
  падает, а приёмник остаётся открытым.
- **`stream.destroy(e)`** остаётся: ошибка уходит в `Result` у `for await`, `end` и `finished`.

## 4. Как в node

- Всё API: `Readable`, `Writable`, `Duplex`, `Transform`, `PassThrough`, `pipeline` и
  `finished` (и из `node:stream/promises`), `Readable.from`, `compose`, `for await`,
  `node:stream/consumers` (`text`, `json`, `buffer`, `arrayBuffer`), `node:string_decoder`.
  Кроме `pipe` и того, что про ошибки (раздел 3).
- «Не успеваю»: `write()` вернул `false` — ждать `drain`; буфер `highWaterMark` — как в node.
- Web-потоки (`ReadableStream`, `WritableStream`) — для `fetch` и `Server` ([net.md](net.md));
  переходы `Readable.toWeb()`, `Readable.fromWeb()`, `Writable.toWeb()` — как в node.

## 5. Скорость

Перенесённый код node компилируется в машинный код, поэтому быстрее, чем в node. Позже: когда
компилятор видит `pipeline` из файла в сокет, данные идут без копирования через память
программы (`sendfile` в Linux, `TransmitFile` в Windows).

## 6. Состояние потока и свои классы

Решено 28 октября. В node состояние потока открыто под именами на `_` (`_readableState`,
`_writableState`, `_writev`…): программа может написать `r._readableState.flowing = false`, и
поток зависнет. В tz его снаружи нет, как и других членов «только для библиотеки»
([objects.md](objects.md), раздел 6):

- Состояние и вся работа потока — закрытые (`private`) члены класса `Stream`: чтение, запись,
  `pipeline`, `finished`, `compose` живут в его теле, потому что только там видно состояние любого
  потока. `r.readableState` — ошибка. То, что поток меняет сам, программа читает методами
  (поправка 28 октября, [objects.md](objects.md), раздел 6): `r.readableLength()`,
  `r.readableFlowing()`, `r.destroyed()`, `w.writableEnded()`, `w.writableNeedDrain()`…, у
  файлового потока — `fd()`, `pending()`, `bytesRead()`, `bytesWritten()`. Методы читают
  состояние прямо, копии в полях нет.
- `isReadable`, `isWritable`, `isDestroyed`, `isErrored`, `isDisturbed`, `addAbortSignal` —
  функции `node:stream` и статические методы `Stream`, как в node (`Stream.isErrored(s)`).
- Свой класс заменяет методы, которые зовёт поток: `onRead`, `onWrite`, `onWritev`,
  `onTransform`, `onFlush`, `onFinal`, `onDestroy`, `onConstruct` (в node — `_read`, `_write`…;
  `tz port` переименовывает их сам). Это `protected`-методы `Stream` и его наследников: их зовёт
  поток, а снаружи — ошибка, если класс не открыл свой метод:
  ```ts
  class Counter extends Readable<string> {
    private n = 0
    protected override onRead() {
      this.push(this.n < 3 ? `${this.n++}` : undefined)
    }
  }
  ```
  Запись объектом, `new Readable({ read() {} })`, не меняется: в node ключи и так без `_`.
- Наследнику открыто ещё несколько операций: `pushChunk(chunk)` — положить кусок, тип которого
  компилятор не знает (поток файла, где тип кусков — параметр класса; кусок не того типа кончает
  поток ошибкой `ERR_INVALID_ARG_TYPE`); `readChunk(size)` и `iterate<C>()` — взять кусок; `setUp`
  — то, чем `Readable`, `Writable` и другие задают поток при создании (второй раз ничего не
  делает); и мосты `emitData`, `writeChunk`, `writevChunks`, `transformChunk`, через которые
  поток отдаёт куски `onWrite`, `onTransform` и слушателям `data` с их типом. Мосты поток зовёт
  сам с проверенными кусками; если класс позовёт их сам с куском не того типа, программа
  остановится с ошибкой: проверка в них стоила около 6% скорости потоков.

## План реализации

На каждом шаге: тесты `tests/run` с `// same-as-node`, ошибки в `tests/errors`, прогон под
`TZ_CHECK_LEAKS=1`, обновление README.

1. **Потоки** (сделано) — перенос `lib/internal/streams` на tz поверх `EventEmitter`
   ([events.md](events.md), шаг 1), `Readable<T>` / `Writable<T>`, `pipe`, `pipeline`,
   `for await` ([iterators.md](iterators.md), шаг 4), `node:stream/consumers`,
   `node:string_decoder`. Нужен `Buffer` ([bytes.md](bytes.md), шаг 2).
2. **Типы кусков** (сделано) — по кодировке при создании, вывод для `objectMode`, `setEncoding` с
   ошибкой на поток, который уже ушёл в другое место.
3. **Потоки у файлов и процессов** (сделано, кроме асинхронных функций `node:child_process`) — `fs.createReadStream` / `createWriteStream`,
   `process.stdin`, асинхронные функции `node:child_process` ([process.md](process.md)).
4. **Web-потоки и переходы** между ними; `node:http` ([net.md](net.md), раздел 7).
5. **Позже** — `sendfile` / `TransmitFile` для `pipeline` из файла в сокет.
6. **Ошибки без исключений** (сделано 27 октября; раздел 3; [errors.md](errors.md), шаг 3): `Result` у элементов
   `for await`, у `pipeline`, `finished`, `end()` и `node:stream/consumers`; правило подписки на
   `error`; убрать `pipe()`; переписать перенесённый код node в `library/stream/*.tz`. Сделано
   27 октября снаружи: что видит программа, — `Result`, `pipe()` и колбэк `end()` — ошибки;
   с шагом 5 errors.md (27 октября) и внутри перенесённый код node не бросает и не ловит, тип
   ошибки — `StreamError`, объект с меткой.
