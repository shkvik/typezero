# Потоки node

Решения, принятые 24 октября 2026 года. Шаги 1–3 плана сделаны 24–25 октября (из шага 3 — без
асинхронных функций `node:child_process`); что осталось — в конце, в разделе «План реализации».

## Коротко

- `node:stream` — как в node: `Readable`, `Writable`, `Duplex`, `Transform`, `pipe`,
  `pipeline`, `for await`. Код — перенос `lib/internal/streams` из node, как события
  ([events.md](events.md)).
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
for await (const chunk of createReadStream("big.log")) { … }
await pipeline(createReadStream("a.log"), createGzip(), createWriteStream("a.log.gz"))
const out = await text(child.stdout)            // node:stream/consumers
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
- События описаны: `on("data", chunk => …)` — `chunk: T`, `on("error", e => …)` — `e: Error`.

## 3. Как в node

- Всё API: `Readable`, `Writable`, `Duplex`, `Transform`, `PassThrough`, `pipe`, `pipeline` и
  `finished` (и из `node:stream/promises`), `Readable.from`, `compose`, `for await`,
  `node:stream/consumers` (`text`, `json`, `buffer`, `arrayBuffer`), `node:string_decoder`.
- «Не успеваю»: `write()` вернул `false` — ждать `drain`; буфер `highWaterMark` — как в node.
- **`pipe` не передаёт ошибку дальше** — как в node: источник упал, а приёмник остался
  открытым. `pipeline` передаёт ошибку и закрывает всё — его и советуем, но `pipe` не ломаем.
- Web-потоки (`ReadableStream`, `WritableStream`) — для `fetch` и `serve` ([net.md](net.md));
  переходы `Readable.toWeb()`, `Readable.fromWeb()`, `Writable.toWeb()` — как в node.

## 4. Скорость

Перенесённый код node компилируется в машинный код, поэтому быстрее, чем в node. Позже: когда
компилятор видит `pipeline` из файла в сокет, данные идут без копирования через память
программы (`sendfile` в Linux, `TransmitFile` в Windows).

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
