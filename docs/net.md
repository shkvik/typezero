# Сеть

Решения, принятые 24 октября 2026 года, и поправки 26 октября: ошибки сети — `Result`,
обработчик сервера разбирает ошибки сам ([errors.md](errors.md), раздел 4); 28 октября:
сервер — только `new Server(options, handler)` и `listen()`, без `serve`; `listen()` отдаёт
`Listening`, а соединения, keep-alive и тайм-ауты — забота рантайма. Шаги 1–4 плана
сделаны 24–25 октября, шаг 6 — 26–28 октября; что осталось — в конце, в разделе «План реализации».

## Коротко

- Сервер — `Server` из `"tz:http"`: обработчик получает `Request` (и того, кто его прислал) и
  возвращает `Response`, как в Deno и Bun; создать с настройками, потом `listen()`. На этом же
  построен hono ([packages.md](packages.md)).
- Клиент — `fetch`, как в JS.
- Запросы обрабатываются параллельно на всех ядрах ([async.md](async.md), раздел 8): «Hello,
  World!» — 314 тыс. запросов в секунду, у Go — 239 тыс.
- TLS — OpenSSL, как в node ([crypto.md](crypto.md)). Попадает в exe, только если программа
  работает с https: около 4 МБ (Go: https-клиент целиком — 5,3 МБ). Шифрование трафика — в 3
  раза быстрее, чем в Go. Корневые сертификаты Mozilla — внутри.
- `node:http` (`createServer`), HTTP/2, WebSocket, сокеты TCP/UDP — позже.

## 1. Сервер

```ts
import { Server } from "tz:http"

const server = new Server({ port: 3000 }, async (req, client) => {
  const url = new URL(req.url)
  if (req.method == "POST" && url.pathname() == "/users") {
    const user = await req.json()                      // Result<Json, BodyError | JsonError>
    if (user.isErr()) return new Response("плохой JSON", { status: 400 })
    return Response.json({ id: 1, user: user.value, from: client.address }, { status: 201 })
  }
  return new Response("Not found", { status: 404 })
})
const listening = server.listen()                      // Result<Listening, ListenError>
if (listening.isErr()) return listening
console.log(listening.value.url.href())                // http://localhost:3000/
await listening.value.stop()
```

С hono:

```ts
const app = new Hono()
app.get("/", c => c.text("Hello"))
const listening = new Server({ port: 3000 }, app.fetch).listen()
```

- **Два объекта, у каждого одно состояние** (решено 28 октября). `Server` — только настройки и
  обработчик, после создания не меняется; `listen()` открывает сокет и отдаёт `Listening` —
  запущенный сервер: `url`, `hostname`, `port` (настоящий, и при `port: 0`) и `stop()`. Без
  сокета `Listening` не бывает, поэтому полей «пока не слушает» (`port: 0`, `url?`) нет.
  `listen()` можно звать снова: это ещё один сокет с теми же настройками (на том же порту —
  `EADDRINUSE` от системы, на порту 0 — второй сервер). Короткого `serve(...)` нет: путь один.
- **Настройки** (`ServeOptions`, все необязательные): `port` (8000; 0 — любой свободный),
  `hostname` (`0.0.0.0`), `signal` (его отмена останавливает сервер, как `stop()`),
  `maxBodySize` (128 МБ, как в Bun; больше — 413), `keepAliveTimeout` (5000 мс, как в node:
  столько соединение ждёт следующего запроса), `headersTimeout` (60 000 мс, как в node: столько
  может идти голова запроса, потом 408 и обрыв — защита от клиента, который шлёт по байту),
  `transport` — `{ secure: false }` (http, по умолчанию) или `{ secure: true, cert, key }`
  (https, строки PEM). `secure: true` без `cert` и `key` не скомпилируется, `secure: false` с
  ними — тоже.
- **Ошибка `listen()` — `ListenError`**: `kind: "SERVER"` (ошибка ядра сервера: сокет,
  сертификат), `code` — один из `ListenErrorCode` (`EADDRINUSE`, `EACCES`, `EADDRNOTAVAIL`,
  `ENOTFOUND`, `ERR_INVALID_ARG_VALUE`, `ERR_TLS_CERT`, `ERR_TLS_KEY`, `EMFILE`, `ENOBUFS`,
  `EAFNOSUPPORT`, `UNKNOWN`), поэтому `switch` по нему проверяется; `message`, `errno`,
  `syscall`, а `address` и `port` — где сервер пытался слушать.
- **`listening.stop()`** — перестать принимать соединения, закрыть те, что ждут запроса, и
  дождаться ответов на запросы, которые уже идут. Второй вызов и вызов после `signal` ничего не
  закрывают, только ждут того же: так ждут конца сервера.
- **Кто прислал запрос** — второй аргумент обработчика: `client: { address, port }` (за
  прокси — адрес прокси). Не поле `Request`: `Request` — класс web, его делает и сама
  программа для `fetch`. Так же у Deno, и hono это ждёт: `app.fetch(request, env?)` получает
  его вторым аргументом (функцию, у которой там `unknown`, tz принимает как обработчик).
- **Соединения — дело рантайма**: он принимает их, читает запросы, держит keep-alive, отдаёт
  запросы одного соединения по очереди, закрывает молчащие и медленные, собирает полный URL
  запроса (схема, `Host`, путь) и URL самого сервера (`localhost` вместо `0.0.0.0`, IPv6 в
  скобках). Программа влияет на это только пределами в настройках.
- **Обработчик возвращает только `Response`** и ошибки разбирает сам: плохой JSON — 400,
  повтор — 409, упавшая база — 500. Особой обработки `Result` у сервера нет: автоматический 500
  на любой `Err` отвечал бы 500 и на ошибки клиента ([errors.md](errors.md), раздел 4). Падать
  обработчику нечему: исключений нет, а переполнение стека и нехватка памяти фатальны для всего
  процесса.
- **Аргументы `Response` проверяются типами:** `status` — `HttpStatus` (200 … 599), литерал
  подходит сам, переменную проверяют `isHttpStatus(code)`; статусы 101, 204, 205, 304 — без тела
  (перегрузка конструктора). `Response.redirect` берёт уже разобранный `URL` и статус
  `301 | 302 | 303 | 307 | 308` ([errors.md](errors.md), раздел 7).
- **Поля вместо геттеров:** `res.status`, `res.statusText` — `readonly`-поля; то, что объект
  меняет сам, — методы: `res.body()`, `res.bodyUsed()`, части `URL` (`url.pathname()`), которые
  меняют методы `url.setPathname(...)` и другие ([objects.md](objects.md), раздел 6).
- **Параллельно.** Каждый запрос — async-задача, обработчики идут на всех ядрах. Общие данные
  (счётчик на уровне модуля, кэш в `Map`) — по очереди, как в [async.md](async.md); код
  остаётся таким же, как для node.
- `node:http` (`createServer((req, res) => …)`) — позже, если понадобится: он держится на
  `EventEmitter` и потоках node, а express на нём — JS, его всё равно не скомпилировать.

## 2. Клиент: `fetch`

```ts
async function repo(name: string) {
  const r = await fetch(`https://api.github.com/repos/${name}`, {
    signal: AbortSignal.timeout(5000),
  })                                                   // Result<Response, FetchError>
  if (r.isErr()) return r
  if (!r.value.ok) return err({ code: "http_status", status: r.value.status })
  return r.value.json()                                // Promise<Result<Json, …>>
}
```

- `Request`, `Response`, `Headers`, `URL`, `URLSearchParams`, `FormData`, `AbortController`,
  `AbortSignal.timeout` — как в JS. `json()` возвращает `Json` ([modules.md](modules.md)).
- Распаковка gzip, deflate и brotli, переходы по редиректам (до 20), соединения с keep-alive
  держатся в пуле — как в node.
- **Ошибки — `Result`:** `fetch` отдаёт `Promise<Result<Response, FetchError>>`. `FetchError` —
  объект с меткой `code`: `ECONNREFUSED`, `ENOTFOUND`, `ETIMEDOUT` ([io.md](io.md)), ошибка
  сертификата, `ABORT_ERR` при отмене; `message` — как в node. Чтение тела (`text()`, `json()`,
  `bytes()`) — тоже `Promise<Result>`: обрыв связи, тело уже прочитано, плохой JSON.

## 3. Тела запросов и ответов

- Тело — поток кусков `Uint8Array` ([bytes.md](bytes.md)): `for await (const chunk of req.body())`.
  Целиком — `text()`, `json()`, `bytes()`, `arrayBuffer()`, `formData()`.
- Ответ — строка, `Uint8Array`, `ReadableStream` (отдавать по кускам, например события для
  браузера) или `Response.json(значение)`.
- `ReadableStream` — основное: создание с `start` / `pull` / `cancel`, `getReader()`,
  `for await`. `TransformStream`, `TextDecoderStream`, `CompressionStream` — позже.

## 4. TLS

- **OpenSSL 3.5** — та же библиотека и версия, что в node 24 ([crypto.md](crypto.md)): TLS 1.2
  и 1.3, те же шифры и коды ошибок. Встраивается, только если программа ходит по `https://` или
  запускает сервер с `tls`.
- **Размер.** Статический https-клиент на C с проверкой сертификатов — 4,1 МБ (пустая
  программа — 14 КБ). Для сравнения, Go: hello — 1,5 МБ, https-клиент — 5,3 МБ, HTTP-сервер —
  5,2 МБ.
- **Скорость** (одно ядро): AES-256-GCM — 21 417 МБ/с против 7 015 у Go, подпись RSA-2048 —
  5 805 в секунду против 1 429, ECDSA P-256 и X25519 — тоже быстрее Go. Таблица — в
  [crypto.md](crypto.md).
- **Почему не своё:** свой TLS — годы работы и риск дыр (у Go на это отдельная команда).
  **Почему не mbedTLS:** он меньше (около 630 КБ), но в 7–17 раз медленнее — писался для
  маленьких устройств. **Почему не TLS системы:** в Windows он есть, в Linux — нет, и поведение
  на разных системах было бы разным.
- **Корневые сертификаты** — список Mozilla (118 штук, 127 КБ, 94 КБ сжатыми) встраивается
  вместе с TLS, как в node: в Docker `FROM scratch` своих нет (в Go их копируют в образ
  руками). Дополнительные — переменной `NODE_EXTRA_CA_CERTS`, как в node.

## 5. DNS

В Windows — системный. В Linux — свой (`/etc/resolv.conf`, `/etc/hosts`): статическому exe не на
что опереться, Go делает так же.

## 6. Скорость

Сервер «Hello, World!», 128 соединений, 10 с, bombardier на той же машине (32 потока, Windows):

| | запросов в секунду | задержка | ядра |
|---|---|---|---|
| Go (`net/http`) | 249 000 | 0,5 мс | все |
| Bun (`Bun.serve`) | 86 000 | 1,5 мс | одно |
| node (`http`) | 60 000 | 2,1 мс | одно |

tz должен быть не ниже Go: параллельные обработчики, разбор HTTP без копирования, ответ одной
записью в сокет. Замеры — в `bench`, тем же способом (`bench/http.ps1`, нагрузка — своя на Go,
как bombardier). 25 октября: tz — 314 000 запросов в секунду, 0,41 мс, все ядра; Go в том же
прогоне — 239 000, 0,54 мс; node — 61 000. Обработчик, который трогает общие данные (счётчик в
глобальной переменной), идёт в порядке node через главный поток — 92 000; async-обработчик с
`await` пока тоже — 85 000 ([async.md](async.md), раздел 8).

## 7. Позже

`node:http`, HTTP/2 (через ALPN в TLS), WebSocket (сервер и клиент), `node:net` и `node:dgram`,
прокси для `fetch`, HTTP/3.

## План реализации

На каждом шаге: тесты `tests/run` (сервер и клиент в одной программе, на свободном порту),
ошибки в `tests/errors`, прогон под `TZ_CHECK_LEAKS=1`, обновление README.

1. **Сокеты и DNS** (сделано) — поверх ожидания ввода-вывода ([async.md](async.md), шаг 2): TCP-соединения
   на IOCP / epoll, свой DNS для Linux.
2. **Сервер** (сделано) — разбор HTTP/1.1, keep-alive, `Request` / `Response` / `Headers` / `URL`, тела как
   потоки, сервер с параметрами, 500 на исключение, `stop()`. Нужны классы
   ([objects.md](objects.md), шаг 3), байты ([bytes.md](bytes.md)), `for await`
   ([iterators.md](iterators.md), шаг 4), `Json`. Параллельные обработчики — с параллельным
   рантаймом ([async.md](async.md), шаг 4; сделано 25 октября). Замеры против Go, Bun и node.
3. **`fetch`** (сделано) — пул соединений, редиректы, gzip / deflate / brotli, `AbortSignal`, ошибки как
   в node.
4. **TLS** (сделано) — OpenSSL в рантайме (в exe — только при https, [crypto.md](crypto.md)), корневые
   сертификаты, `NODE_EXTRA_CA_CERTS`, сервер с `tls`; замер размера.
5. **Позже** — раздел 7.
6. **Без исключений** (сделано 26–28 октября; [errors.md](errors.md), шаг 2): `fetch` и чтение тел — `Result`;
   обработчик сервера возвращает только `Response`, убрать «500 на исключение»; `HttpStatus`,
   перегрузки конструктора `Response`, `redirect(url: URL, …)`; геттеры `Request` / `Response`
   / `Headers` — в `readonly`-поля и методы. Переписать `library/http/*.tz`.
   Сделано 26 октября: `fetch` — `Promise<Result<Response, FetchError>>`, чтение тел —
   `Promise<Result<T, BodyError>>`; `HttpStatus` (200 … 599) — объединение литералов, значение
   программы подходит после проверки, которая доказывает предел (`isHttpStatus(code)`);
   `RedirectStatus` и `redirect(url: URL, …)`. Вместо перегрузки для 204, 205 и 304: литерал с
   телом — ошибка компиляции, статус из переменной оставляет ответ без тела (перегрузка
   потребовала бы доказать `code != 204` для любого статуса из переменной). `throw` в
   конструкторах `Request` и `Headers` и «500 на исключение» ушли с шагом 5 errors.md
   (27 октября). Геттеры ушли с шагом 11 [objects.md](objects.md) (28 октября).
7. **Сервер: настройки в конструкторе, `Listening`** (сделано 28 октября). `new Server(options,
   handler)` и `listen()` без аргументов, `Listening` с `url`, `hostname`, `port`, `stop()`;
   `ListenError` вместо `SystemError`; `transport` с меткой `secure` вместо `tls`; второй
   аргумент обработчика — `client`; `keepAliveTimeout` и `headersTimeout` (408) — в рантайме,
   таймер обхода соединений тоже там (`setInterval` и `netSweep` из библиотеки ушли); полный URL
   запроса и URL сервера собирает рантайм; `Response.writeTo` отдаёт `Written` с меткой
   (`"done"` или `"streaming"`). Нет `onListen`, `finished`, `server.port` и
   `ERR_SERVER_ALREADY_LISTEN`. Цикл по запросам одного соединения пока остаётся закрытым
   методом `Listening` (перенос в рантайм — отдельный шаг, с замером). «Hello, World!» и сервер
   с разбором адреса — не медленнее, чем до шага.
