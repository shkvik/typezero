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
- HTTP/2, WebSocket, сокеты TCP/UDP — позже (решено 29 октября, `tmp/design`). `node:http` не
  будет: сервер — `tz:http`, клиент — `fetch`.

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
  `hostname` (`0.0.0.0`), `maxBodySize` (128 МБ, как в Bun; больше — 413), `keepAliveTimeout` (5000 мс, как в node:
  столько соединение ждёт следующего запроса), `headersTimeout` (60 000 мс, как в node: столько
  может идти голова запроса, потом 408 и обрыв — защита от клиента, который шлёт по байту),
  `requestTimeout` (300 000 мс, как в node, 0 — без предела: столько может идти весь запрос с
  телом; потом 408 и обрыв, а обработчик, который читает тело, получает `BodyError` вида
  `TIMEOUT`; сделано 29 октября),
  `transport` — `{ secure: false }` (http, по умолчанию) или `{ secure: true, cert, key }`
  (https, строки PEM). `secure: true` без `cert` и `key` не скомпилируется, `secure: false` с
  ними — тоже.
- **Ошибка `listen()` — `ListenError`**: `kind: "SERVER"` (ошибка ядра сервера: сокет,
  сертификат), `code` — один из `ListenErrorCode` (`EADDRINUSE`, `EACCES`, `EADDRNOTAVAIL`,
  `ENOTFOUND`, `ERR_INVALID_ARG_VALUE`, `ERR_TLS_CERT`, `ERR_TLS_KEY`, `EMFILE`, `ENOBUFS`,
  `EAFNOSUPPORT`, `UNKNOWN`), поэтому `switch` по нему проверяется; `message`, `errno`,
  `syscall`, а `address` и `port` — где сервер пытался слушать.
- **`listening.stop()`** — перестать принимать соединения, закрыть те, что ждут запроса, и
  дождаться ответов на запросы, которые уже идут. Это единственный способ остановить сервер
  (опции `signal` нет: `AbortSignal` убран 28 октября, [async.md](async.md), раздел 7). Второй
  вызов ничего не закрывает, только ждёт того же: так ждут конца сервера.
- **`stop({ timeout })`** (сделано 29 октября): ждать не дольше `timeout` мс, потом оборвать
  оставшиеся соединения — у потока ответа (бесконечный SSE) вызывается `cancel` (запись падает,
  `Response` отменяет читателя), зависший обработчик остаётся сам по себе, `stop` возвращается.
  `timeout: 0` обрывает сразу; без `timeout` — ждать сколько нужно. Второй вызов с меньшим
  `timeout` обрывает раньше. Как `Shutdown(ctx)` + `Close()` в Go. Остановка по сигналу:
  ```ts
  process.once("SIGTERM", async () => {
    await listening.stop({ timeout: 10_000 })
    process.exit(0)
  })
  ```
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
- **`url.portNumber(): int | undefined`** (сделано 29 октября) — порт числом; `undefined`, если
  порта в адресе нет (порт схемы по умолчанию разбор убирает). Разбор порт уже проверил, поэтому
  ошибки нет и в типе ([style.md](style.md), раздел 1); `fetch` берёт его вместо
  `parseInt(url.port())`.
- **Параллельно.** Каждый запрос — async-задача, обработчики идут на всех ядрах. Общие данные
  (счётчик на уровне модуля, кэш в `Map`) — по очереди, как в [async.md](async.md); код
  остаётся таким же, как для node.
- **`node:http` нет** (решено 29 октября: «мы вообще его не повторяем»). Сервер — `tz:http`,
  клиент — `fetch`. Пакеты, написанные под `node:http` (express, koa, fastify, axios, got),
  переносятся с переделкой на `tz:http` и `fetch` или заменяются своими в их стиле
  ([decorators.md](decorators.md), раздел 7); express и так не собрался бы: он подменяет
  прототипы `req` и `res`, а их в tz нет.

## 2. Клиент: `fetch`

```ts
async function repo(name: string) {
  const r = await fetch(`https://api.github.com/repos/${name}`, {
    timeout: 5000,
  })                                                   // Result<Response, FetchError>
  if (r.isErr()) return r
  if (!r.value.ok) return err({ code: "http_status", status: r.value.status })
  return r.value.json()                                // Promise<Result<Json, …>>
}
```

- `Request`, `Response`, `Headers`, `URL`, `URLSearchParams`, `FormData` — как в JS. `json()`
  возвращает `Json` ([modules.md](modules.md)).
- **Полей браузера нет** (решено и сделано 29 октября): то, что вне браузера ничего не делает, не
  принимается молча, а убрано. В `RequestInit` — только `method`, `headers`, `body`, `redirect`,
  `timeout`, `duplex`; у `Request` нет `mode`, `credentials`, `cache`, `integrity`, `referrer`,
  `referrerPolicy`, `keepalive`, `destination`, `isReloadNavigation`, `isHistoryNavigation`.
  Такое поле — обычная ошибка «has no field …».
- **`timeout`** вместо `AbortSignal` (решено 28 октября): опция `fetch` и `new Request` в
  миллисекундах, и `request.timeout` — её значение. Время идёт на весь запрос: соединение, TLS,
  отправку тела, голову ответа, редиректы и чтение тела. Не успел — `Err` с кодом
  `"TIMEOUT_ERR"` и текстом node «The operation was aborted due to timeout»: у `fetch`, если
  не пришла голова, у `text()` / `json()` / `bytes()`, если не дочиталось тело. Таймер не
  держит программу. Отменить запрос по своему решению нельзя: `AbortController` убран
  ([async.md](async.md), раздел 7). У запроса сервера `timeout` нет, и сигнала тоже — каждый
  запрос раньше делал свой `AbortSignal`, и его освобождение по очереди стоило серверу втрое
  меньше запросов в секунду (102k вместо 312k).
- Распаковка gzip, deflate и brotli, переходы по редиректам (до 20), соединения с keep-alive
  держатся в пуле — как в node.
- **Прокси** (сделано 29 октября, кроме SOCKS и Digest): из окружения само — `HTTPS_PROXY` для
  https://, `HTTP_PROXY` для http:// (сначала прописные, потом строчные; при `REQUEST_METHOD` —
  CGI — `HTTP_PROXY` не читается, защита от httpoxy), `NO_PROXY` (через запятую: имя с
  поддоменами, `.example.com`, IP, сеть `10.0.0.0/8`, `имя:порт`, `*`); `localhost`,
  `127.0.0.1`, `::1` — мимо прокси всегда. В коде — `proxy: { enabled: true, url } |
  { enabled: false }` у `fetch`. http:// — запрос с полным адресом к прокси; https:// —
  туннель `CONNECT host:port`, потом TLS прямо с сервером (SNI и проверка — сервера); прокси
  `https://` — сначала TLS до прокси. Логин и пароль в адресе прокси — `Proxy-Authorization:
  Basic`. Соединения через прокси — в пуле по паре «прокси + сервер», `timeout` считает и
  прокси. Ошибки — `FetchError` вида `PROXY`: `ERR_INVALID_PROXY_URL`, `ERR_PROXY_AUTH_REQUIRED`
  (407), `ERR_PROXY_REFUSED` (403, 502…); поля `proxy` (адрес без пароля) и `status`. Прокси не
  отвечает — `NETWORK` с адресом прокси и словами «the proxy» в тексте. Ещё нет: SOCKS4/5,
  Digest, прокси через HTTP/2, прокси у WebSocket.
- **Ошибки — `Result`** (переделано 29 октября): `fetch` отдаёт
  `Promise<Result<Response, FetchError>>`. `FetchError` — объект с меткой `kind` (что делать
  программе) и закрытым списком `code`:
  ```ts
  type FetchError =
    | { kind: "REQUEST"; code: "ERR_INVALID_URL" | "ERR_CREDENTIALS_IN_URL" | "ERR_INVALID_METHOD"
          | "ERR_BODY_WITH_GET" | "ERR_DUPLEX_REQUIRED" | "ERR_REQUEST_USED"
          | "ERR_UNSUPPORTED_SCHEME" | "ERR_BLOCKED_PORT" | "ERR_STREAM_FAILED";
        message: string; url: string }                 // чинить код
    | { kind: "NETWORK"; code: "ENOTFOUND" | "EAI_AGAIN" | "ECONNREFUSED" | "ETIMEDOUT"
          | "ENETUNREACH" | "EHOSTUNREACH" | "EADDRNOTAVAIL" | "ECONNRESET" | "ECONNABORTED"
          | "EPIPE" | "EMFILE" | "ENOBUFS" | "UNKNOWN";
        message: string; errno: int; syscall: string; host: string; port: int }  // можно повторить
    | { kind: "RESPONSE"; code: "ERR_CLOSED" | "ERR_INVALID_RESPONSE" | "ERR_HEAD_TOO_LARGE";
        message: string; host: string; port: int }
    | { kind: "TLS"; code: TlsCode; message: string; host: string }
    | { kind: "REDIRECT"; code: "ERR_UNEXPECTED_REDIRECT" | "ERR_TOO_MANY_REDIRECTS"
          | "ERR_INVALID_LOCATION" | "ERR_REDIRECT_WITH_STREAM"; message: string; location: string }
    | { kind: "TIMEOUT"; code: "TIMEOUT_ERR"; message: string; timeout: int }
  ```
  `TlsCode` — коды проверки OpenSSL, как в node, и `ERR_TLS_CERT_ALTNAME_INVALID`,
  `ERR_TLS_HANDSHAKE`. `message` — что не вышло, где и почему: `could not find the address of
  api.github.com: no such name in DNS`, `no full answer from api.github.com within 5000 ms`,
  `the certificate of localhost is self-signed, so it is not trusted; if its root CA is installed
  on this machine, run with NODE_USE_SYSTEM_CA=1`. Поля `cause` нет. Чтение тела (`text()`,
  `json()`, `bytes()`) — тоже `Promise<Result>`, ошибка — `BodyError` (раздел 3).

## 3. Тела запросов и ответов

- Тело — поток кусков `Uint8Array` ([bytes.md](bytes.md)): `for await (const chunk of req.body())`.
  Целиком — `text()`, `json()`, `bytes()`, `arrayBuffer()`, `formData()`.
- Ответ — строка, `Uint8Array`, `ReadableStream` (отдавать по кускам, например события для
  браузера) или `Response.json(значение)`.
- `ReadableStream` — основное: создание с `start` / `pull` / `cancel`, `getReader()`,
  `for await`. `TransformStream`, `TextDecoderStream`, `CompressionStream` — позже.
- **`BodyError`** (29 октября) — ошибка чтения тела:
  ```ts
  type BodyError =
    | { kind: "USED"; code: "ERR_BODY_USED" | "ERR_BODY_LOCKED"; message: string }
    | { kind: "NETWORK"; code: "ERR_CLOSED" | "ECONNRESET" | "ECONNABORTED" | "ETIMEDOUT"
          | "ERR_TLS_FAILED" | "UNKNOWN"; message: string }
    | { kind: "TIMEOUT"; code: "TIMEOUT_ERR"; message: string; timeout: int }
    | { kind: "TOO_LARGE"; code: "ERR_BODY_TOO_LARGE"; message: string; limit: double }
    | { kind: "MALFORMED"; code: "ERR_INVALID_CHUNKED" | "ERR_INVALID_COMPRESSION"; message: string }
    | { kind: "CONTENT"; code: "ERR_INVALID_JSON" | "ERR_NOT_A_FORM" | "ERR_NO_BOUNDARY"
          | "ERR_INVALID_MULTIPART"; message: string; contentType: string }
    | { kind: "STREAM"; code: "ERR_STREAM_FAILED"; message: string }
  ```
  `json()` — `Result<Json, BodyError>` (не JSON — `CONTENT`, `ERR_INVALID_JSON`). Рантайм
  различает «оборвалось» (`ERR_CLOSED`) и «кривые куски chunked» (`ERR_INVALID_CHUNKED`): на
  сервере кривые куски — ещё и ответ 400. Кривая часть multipart не пропускается молча —
  `ERR_INVALID_MULTIPART`. Один вид `CONTENT` для JSON и форм (в решении было два варианта с
  одной меткой; объединены, поле `contentType` есть у обоих). Тип ошибки потока самого
  `ReadableStream` (`ReadableStream<R, E>`) — пока `StreamError`: вывод `E` из
  `controller.error(...)` не сделан.

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

## 7. TCP: `node:net` (первая часть сделана 29 октября)

```ts
import { Server, Socket } from "node:net"

const echo = new Server({ port: 7000, hostname: "127.0.0.1", idleTimeout: 60_000 }, async socket => {
  for await (const chunkResult of socket) {           // поток node: Result<Buffer, StreamError>
    if (chunkResult.isErr()) break
    socket.write(chunkResult.value)
  }
})
const listeningResult = echo.listen()                  // Result<Listening, ListenError>; stop({ timeout })

const socket = new Socket({ host: "localhost", port: 6379, timeout: 5000, noDelay: true })
const connectedResult = await socket.connect()         // Result<void, ConnectError>
socket.write("PING\r\n")
const endedResult = await socket.end()
```
- `Socket` — `Duplex` из `node:stream` над соединением рантайма (то же, что у `fetch` и
  `tz:http`): чтение — новый вход рантайма `tcpRead`, запись — общий `httpWrite` с
  ожиданием `drain`. Настройки: `host`, `port`, `timeout` (на подключение), `noDelay`
  (по умолчанию `true`), `keepAlive`, `transport: { secure: true, servername? }` (TLS сразу);
  `startTls(servername?)` — STARTTLS посреди соединения. `remoteAddress()`, `remotePort()`,
  `bytesRead()`, `bytesWritten()`, `reset()` (RST). `connect()` снова после закрытия —
  переподключение.
- `ConnectError` — `kind`: `REQUEST` (`ERR_INVALID_HOST`, `ERR_ALREADY_CONNECTED`), `NETWORK`
  (`ECONNREFUSED`, `ENOTFOUND`…), `TLS`, `TIMEOUT`; тексты — как у `fetch`.
- `Server` — обработчик `async (socket) => …` на каждое соединение, своей задачей;
  `idleTimeout` — сколько соединение может молчать; `stop({ timeout })` — как у `tz:http`.
- `isIP`, `isIPv4`, `isIPv6`.
- **Ещё не сделано** из решения ([tmp/spec/09-sockets.md](../tmp/spec/09-sockets.md)): слой
  `tz:socket` (сокет любого вида, сырые сокеты, все настройки), `node:dgram` (UDP), сокеты Unix и
  именованные каналы (`{ path }`), `SocketError` с метками у потока (сейчас поток отдаёт
  `StreamError`), обработчик-не-`async`, объявления для редактора (`patch.cmake`).

## 8. Пакеты: `tz:packet` (сделано 29 октября)

Сборка и разбор заголовков на tz над `Uint8Array`, без особых прав: `parseEthernet`,
`parseArp`, `parseIpv4` (проверяет контрольную сумму), `parseIpv6`, `parseUdp`, `parseTcp`
(флаги `TCP_SYN`, `TCP_ACK`…), `parseIcmp` (проверяет сумму); `buildIpv4`, `buildUdp` (сумма с
псевдозаголовком), `buildIcmpEcho`; `checksum(bytes, initial?)`, `transportChecksum(source,
destination, protocol, segment)`. Ошибки — `Result` с `PacketError`: `kind: "PACKET"`,
`ERR_TRUNCATED`, `ERR_INVALID_HEADER`, `ERR_INVALID_CHECKSUM`, поле `offset`. Отправлять пакеты
нечем, пока нет сырых сокетов `tz:socket` (ping, traceroute — потом).

## 9. WebSocket (первая часть сделана 29 октября)

```ts
// Клиент: один объект, конструктор — настройки, connect() — Result.
const socket = new WebSocket({ url: "ws://127.0.0.1:3000/chat", protocols: ["chat"] })
socket.on("message", data => { ... })          // data: string | Uint8Array
socket.on("close", (code, reason) => { ... })
socket.on("error", error => { ... })           // WebSocketError; без слушателя — ошибка сборки
const connectedResult = await socket.connect()  // Result<void, WebSocketConnectError>
const sentResult = socket.send("hello")         // Result<void, SendError>
await socket.close(1000, "bye")

// Сервер — как Deno, upgradeWebSocket из tz:http.
new Server({ port: 3000 }, request => {
  const upgradeResult = upgradeWebSocket(request, { protocol: "chat" })
  if (upgradeResult.isErr()) return new Response(upgradeResult.error.message, { status: 426 })
  const peer = upgradeResult.value.socket       // подключится, когда обработчик вернёт ответ
  peer.on("message", data => { ... })
  return upgradeResult.value.response           // 101
})
```
- Кадры, маска, фрагменты, ping / pong, закрытие с кодом и причиной, проверка UTF-8 текста,
  `maxMessageSize` (16 МБ) — на tz (`library/http/websocket.tz`); рукопожатие — свой SHA-1 на tz
  (без OpenSSL); ввод-вывод — `tcpRead` / `httpWrite` рантайма, случайные байты маски —
  `wsRandom` (`rand_s`). `wss:` — тем же TLS, что у `fetch`.
- События — EventEmitter (`"message"`, `"close"`, `"error"`), `"open"` нет; `connect()` снова
  после `"close"` — переподключение, у серверного сокета — `Err` `ERR_ALREADY_CONNECTED`.
- Ошибки: `WebSocketConnectError` — `REQUEST` (`ERR_INVALID_URL`, `ERR_UNSUPPORTED_SCHEME`,
  `ERR_ALREADY_CONNECTED`), `NETWORK`, `TIMEOUT`, `REFUSED` (`ERR_NOT_UPGRADED`, `status`);
  `WebSocketError` (`"error"`) — `NETWORK` (`ERR_CLOSED` — оборвалось без кадра закрытия),
  `PROTOCOL` (`ERR_INVALID_FRAME`, `ERR_INVALID_UTF8`, `ERR_UNMASKED_FRAME`), `TOO_LARGE`;
  `SendError` — `NOT_CONNECTED` (`ERR_NOT_CONNECTED`, `ERR_CLOSING`); `UpgradeError` —
  `ERR_NOT_WEBSOCKET`, `ERR_UNSUPPORTED_VERSION`.
- **Ещё не сделано** из решения ([tmp/spec/10-websocket.md](../tmp/spec/10-websocket.md)):
  `timeout` и `idleTimeout`, пинги рантайма, тип `CloseCode` (сейчас `close(code: int)`),
  `bufferedAmount()`, прокси и `httpVersion`, WebSocket поверх HTTP/2, `TLS` и `PROXY` в
  ошибках подключения, объявления для редактора.

## 10. Позже

HTTP/2 (через ALPN в TLS), `node:dgram`, SOCKS-прокси, HTTP/3 — решены 29 октября, не сделаны (`tmp/design`, `tmp/spec`). `node:http` не будет.

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
3. **`fetch`** (сделано) — пул соединений, редиректы, gzip / deflate / brotli, ошибки как в node;
   `AbortSignal` заменён опцией `timeout` 28 октября.
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
