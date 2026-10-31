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
- HTTP/2 — у сервера (раздел 1) и у `fetch` (раздел 2), сделан 31 октября; потоки HTTP/2
  напрямую — с трейлерами и полным дуплексом, основа для gRPC — `tz:http2` (раздел 11).
  WebSocket и сокеты TCP/UDP — разделы 7–10. `node:http` не будет: сервер — `tz:http`, клиент —
  `fetch`.

## 1. Сервер

```ts
import { Server } from "tz:http"

const server = new Server({ port: 3000 }, async (req, client) => {
  const url = new URL(req.url)
  if (req.method == "POST" && url.pathname() == "/users") {
    const user = await req.json()                      // Result<Json, JsonBodyError>
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
  `BODY_ERROR_TIMEOUT`; сделано 29 октября),
  `transport` — `{ secure: false }` (http, по умолчанию) или `{ secure: true, cert, key }`
  (https, строки PEM). `secure: true` без `cert` и `key` не скомпилируется, `secure: false` с
  ними — тоже.
- **Ошибка `listen()` — `ListenError`**: `kind: "LISTEN_ERROR"` (ошибка ядра сервера: сокет,
  сертификат), `code` — один из `ListenErrorCode` (`EADDRINUSE`, `EACCES`, `EADDRNOTAVAIL`,
  `ENOTFOUND`, `ERR_INVALID_ARG_VALUE`, `ERR_TLS_CERT`, `ERR_TLS_KEY`, `EMFILE`, `ENOBUFS`,
  `EAFNOSUPPORT`, `UNKNOWN`), поэтому `switch` по нему проверяется; `message`, `syscall`, а
  `address` и `port` — где сервер пытался слушать.
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
- **HTTP/2** (сделано 31 октября, [tmp/spec/12-http2.md](../tmp/spec/12-http2.md)): с TLS сервер
  договаривается о нём сам (ALPN `h2`), старые клиенты получают HTTP/1.1; настройка
  `http2?: { enabled: false } | { enabled: true; cleartext?: boolean; maxConcurrentStreams?: int }`
  — без неё включено при `secure: true` и выключено без TLS; `cleartext: true` — HTTP/2 без TLS
  (h2c) только «с первого байта», переход `Upgrade: h2c` убран из стандарта и не делается.
  Обработчик тот же, `client.protocol` — `"http/1.1"` или `"h2"`. Каждый поток HTTP/2 — своя
  задача на любом ядре; поток в рантайме — то же соединение для `Request` и `Response`, так что
  тела в обе стороны, потоковые ответы и `maxBodySize` работают как в HTTP/1.1. Движок —
  nghttp2 1.70.0 ([third_party/nghttp2](../third_party/nghttp2), MIT, в `tz licenses`): кадры,
  HPACK, окна, защита от Rapid Reset и CONTINUATION flood — его. Пределы: 100 потоков сразу,
  заголовки — 16 КБ, окно потока — 1 МБ, соединения — 16 МБ. Окно открывается, когда программа
  читает тело: не читает — клиент ждёт, память не растёт (31.10). Тело, которое обработчик не
  дочитал, закрывает только поток (`RST_STREAM` с `NO_ERROR`, как Go). `stop()` шлёт GOAWAY
  (сначала предупреждение, потом с последним принятым потоком) и ждёт начатых. Ещё нет:
  `headersTimeout` и `requestTimeout` у потоков, WebSocket поверх HTTP/2. Потоки напрямую —
  `tz:http2` (раздел 11). Тест — `cli/http2_server` (клиент — `node:http2`).
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
- **HTTP/2** (сделано 31 октября, H3, H9, H10, Q9б): по `https://` `fetch` предлагает HTTP/2 в
  ALPN и берёт его, если сервер согласен; все запросы к серверу идут сразу по одному
  соединению, второе открывается, только когда первое заполнено (`MAX_CONCURRENT_STREAMS`
  сервера). `http://` — HTTP/1.1, как у браузеров. Явный выбор — `httpVersion?: "auto" | "1.1" |
  "2"` в `RequestInit`: `"1.1"` — только HTTP/1.1, `"2"` — HTTP/2 и без TLS (h2c «с первого
  байта»), а сервер без HTTP/2 — ошибка `FETCH_ERROR_RESPONSE` `ERR_NO_HTTP2`; если по `http://`
  ответил сервер только с h2c — `ERR_INVALID_RESPONSE` с подсказкой `httpVersion: "2"`.
  `response.protocol` — `"http/1.1"` или `"h2"`; `statusText` по HTTP/2 — стандартный текст
  кода (в HTTP/2 его нет). Запрос в поток, который сервер не тронул (`REFUSED_STREAM`, GOAWAY
  ниже номера), — `FETCH_ERROR_NETWORK` `ERR_NOT_PROCESSED`: его можно смело отправить снова;
  сброс потока посреди ответа — `ERR_STREAM_RESET`. Простаивающее соединение HTTP/2 читает без
  счёта (не держит программу). Тот же движок nghttp2, что у сервера. Ещё нет: HTTP/2 через
  прокси, `trace` (H15). Тест — `run/fetch_http2`; с Rust hyper (h2c) проверено руками.
- Распаковка gzip, deflate и brotli, переходы по редиректам (до 20), соединения с keep-alive
  держатся в пуле — как в node. **`fetch` никогда не повторяет запрос сам** (H12, 31 октября):
  запрос уходит один раз, и `POST` тоже; повторять и писать в журнал — дело программы. Чтобы
  соединение из пула, которое сервер успел закрыть, не съело запрос, перед отправкой рантайм
  проверяет его (H13: готово ли оно к чтению — пришёл конец, сброс или лишние байты — значит,
  мёртвое) и берёт новое; запрос в мёртвое соединение не уходит. Тест — `run/fetch_pool`.
- **Прокси** (сделано 29 октября, кроме SOCKS и Digest): из окружения само — `HTTPS_PROXY` для
  https://, `HTTP_PROXY` для http:// (сначала прописные, потом строчные; при `REQUEST_METHOD` —
  CGI — `HTTP_PROXY` не читается, защита от httpoxy), `NO_PROXY` (через запятую: имя с
  поддоменами, `.example.com`, IP, сеть `10.0.0.0/8`, `имя:порт`, `*`); `localhost`,
  `127.0.0.1`, `::1` — мимо прокси всегда. В коде — `proxy: { enabled: true, url } |
  { enabled: false }` у `fetch`. http:// — запрос с полным адресом к прокси; https:// —
  туннель `CONNECT host:port`, потом TLS прямо с сервером (SNI и проверка — сервера); прокси
  `https://` — сначала TLS до прокси. Логин и пароль в адресе прокси — `Proxy-Authorization:
  Basic`. Соединения через прокси — в пуле по паре «прокси + сервер», `timeout` считает и
  прокси. Ошибки — `FetchError` вида `FETCH_ERROR_PROXY`: `ERR_INVALID_PROXY_URL`, `ERR_PROXY_AUTH_REQUIRED`
  (407), `ERR_PROXY_REFUSED` (403, 502…); поля `proxy` (адрес без пароля) и `status`. Прокси не
  отвечает — `FETCH_ERROR_NETWORK` с адресом прокси и словами «the proxy» в тексте. Ещё нет: SOCKS4/5,
  Digest, прокси через HTTP/2, прокси у WebSocket.
- **Ошибки — `Result`** (переделано 29 октября): `fetch` отдаёт
  `Promise<Result<Response, FetchError>>`. `FetchError` — объект с меткой `kind` (что делать
  программе) и закрытым списком `code`. С 31 октября (Ф7, Ф9) `kind` уникален во всей
  библиотеке — имя типа и причина (`FETCH_ERROR_NETWORK`, `BODY_ERROR_TOO_LARGE`, у типов с одной
  причиной — имя типа: `LISTEN_ERROR`, `URL_ERROR`), поля `errno` у ошибок сети нет (`syscall`
  есть):
  ```ts
  type FetchError<E = never> =
    | { kind: "FETCH_ERROR_REQUEST"; code: "ERR_INVALID_URL" | "ERR_CREDENTIALS_IN_URL" | "ERR_INVALID_METHOD"
          | "ERR_BODY_WITH_GET" | "ERR_DUPLEX_REQUIRED" | "ERR_REQUEST_USED"
          | "ERR_UNSUPPORTED_SCHEME" | "ERR_BLOCKED_PORT" | "ERR_BODY_FAILED";
        message: string; url: string }                 // чинить код
    | { kind: "FETCH_ERROR_REQUEST"; code: "ERR_STREAM_FAILED"; message: string; url: string;
        details: E }                                   // свой поток тела упал (раздел 3)
    | { kind: "FETCH_ERROR_NETWORK"; code: "ENOTFOUND" | "EAI_AGAIN" | "ECONNREFUSED" | "ETIMEDOUT"
          | "ENETUNREACH" | "EHOSTUNREACH" | "EADDRNOTAVAIL" | "ECONNRESET" | "ECONNABORTED"
          | "EPIPE" | "EMFILE" | "ENOBUFS" | "UNKNOWN";
        message: string; syscall: string; host: string; port: int }  // можно повторить
    | { kind: "FETCH_ERROR_RESPONSE"; code: "ERR_CLOSED" | "ERR_INVALID_RESPONSE" | "ERR_HEAD_TOO_LARGE";
        message: string; host: string; port: int }
    | { kind: "FETCH_ERROR_TLS"; code: TlsCode; message: string; host: string }
    | { kind: "FETCH_ERROR_REDIRECT"; code: "ERR_UNEXPECTED_REDIRECT" | "ERR_TOO_MANY_REDIRECTS"
          | "ERR_INVALID_LOCATION" | "ERR_REDIRECT_WITH_STREAM"; message: string; location: string }
    | { kind: "FETCH_ERROR_TIMEOUT"; code: "TIMEOUT_ERR"; message: string; timeout: int }
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
- `ReadableStream<R, E>` (В9, 29 октября, ночь; В2 и Т3 — 31 октября) — основное: создание с
  `start` / `pull` / `cancel`, `getReader()`, `for await`. `E extends StreamError` — тип ошибки
  потока, то, что отдают в `controller.error(...)`; по умолчанию `StreamError` — любая ошибка, у
  которой есть только `message` ([streams.md](streams.md), раздел 3). Ошибка со своими полями —
  свой тип: `interface DiskError extends StreamError { kind; … }`, `new ReadableStream<string,
  DiskError>({ ... })`; поток без ошибок — `ReadableStream<string, never>`. `read()` и `for await`
  отдают `Result<…, E | ReaderError | ControllerClosedError>`; `ReaderError` (`kind: "READER_ERROR"`,
  `code: "ERR_STREAM_LOCKED" | "ERR_READER_RELEASED"`) — поток читают неправильно (второй
  читатель, чтение после `releaseLock()`); `getReader()` — без `Result`, второй читатель
  «пустой», ошибка — при первом `read()`. `ControllerClosedError` (`kind:
  "CONTROLLER_CLOSED_ERROR"`, `code: "ERR_INVALID_STATE"`) — тот, кто делает поток, позвал
  `enqueue()` или `close()` после `close()`: поток ломается, остаток не отдаётся; после конца
  потока сказать некому. `stream.failure()` — своя ошибка потока из `controller.error`
  (`E | undefined`). `cancel(reason?: string)` — причина текстом, ошибкой она не становится.
  `TransformStream`, `TextDecoderStream`, `CompressionStream` — позже.
- **Поток телом** (В9; Г2 — 31 октября): конструкторы `new Response(тело)` и `new Request(url,
  { body })` потоков не берут совсем, даже без ошибок (`docs/style.md`, раздел 6): тело
  конструктора — `BodyData` (строка, байты, `ArrayBuffer`, `URLSearchParams`, `FormData`),
  настройки `new Request` — `RequestOptions`. Поток отдают методом: `Response.stream(поток,
  init?)`, `Request.stream(url, поток, options?)` или `fetch(url, { body: поток, duplex: "half"
  })`; поток в конструкторе — ошибка сборки с подсказкой. `RequestInit<E>` — настройки `fetch`,
  у них `body` может быть потоком; `RequestInit` без аргумента — с любым потоком. У `fetch`
  своя ошибка потока приходит целиком: `{ kind: "FETCH_ERROR_REQUEST", code: "ERR_STREAM_FAILED", message,
  url, details: E }`; у `fetch(url)` без потока `E = never`, такой ошибки нет (ветку не пишут,
  сравнение с ней — ошибка сборки). Тело `Request`, которое упало при отправке, а своего типа
  ошибки у `fetch` нет (`fetch(request)`), — `ERR_BODY_FAILED`. Внутри тело хранит поток с
  `BodyError`: поток другого вида оборачивается (`bodyStream`), его ошибка становится `{ kind:
  "STREAM", code: "ERR_STREAM_FAILED", message }`. Упал поток ответа сервера — ошибка в stderr,
  соединение обрывается (как в node). Пример — `tmp/design/stream-as-body.tz`, тест
  `run/stream_bodies`.
- **`BodyError`** (29 октября) — ошибка чтения тела:
  ```ts
  // каждый вариант — interface … extends StreamError (с 31 октября): тело — поток
  type BodyError =
    | BodyErrorUsed       // { kind: "BODY_ERROR_USED"; code: "ERR_BODY_USED" | "ERR_BODY_LOCKED"; message }
    | BodyErrorNetwork    // { kind: "BODY_ERROR_NETWORK"; code: "ERR_CLOSED" | "ECONNRESET" | "ECONNABORTED"
                          //   | "ETIMEDOUT" | "ERR_TLS_FAILED" | "UNKNOWN"; message }
    | BodyErrorTimeout    // { kind: "BODY_ERROR_TIMEOUT"; code: "TIMEOUT_ERR"; message; timeout: int }
    | BodyErrorTooLarge   // { kind: "BODY_ERROR_TOO_LARGE"; code: "ERR_BODY_TOO_LARGE"; message; limit: double }
    | BodyErrorMalformed  // { kind: "BODY_ERROR_MALFORMED"; code: "ERR_INVALID_CHUNKED" | "ERR_INVALID_COMPRESSION"; message }
    | BodyErrorStream     // { kind: "BODY_ERROR_STREAM"; code: "ERR_STREAM_FAILED"; message }
  type TextError = BodyError                     // text()
  type BytesError = BodyError                    // bytes(), arrayBuffer()
  type JsonBodyError = BodyError                 // json()
    | { kind: "JSON_BODY_ERROR_CONTENT"; code: "ERR_INVALID_JSON"; message: string; contentType?: string;
        start: string }                          // start — первые 200 символов тела
  type FormDataError = BodyError                 // formData()
    | { kind: "FORM_DATA_ERROR_CONTENT"; code: "ERR_NOT_A_FORM" | "ERR_NO_BOUNDARY" | "ERR_INVALID_MULTIPART";
        message: string; contentType?: string }
  ```
  С 29 октября (ночь) у каждого метода — только его ошибки (`switch` не требует ветки, которой
  не бывает); `contentType` — заголовок ответа, нет заголовка — нет поля; `start` у «не JSON»
  показывает, что пришло вместо него (чаще всего — HTML-страница ошибки). Рантайм
  различает «оборвалось» (`ERR_CLOSED`) и «кривые куски chunked» (`ERR_INVALID_CHUNKED`): на
  сервере кривые куски — ещё и ответ 400. Кривая часть multipart не пропускается молча —
  `ERR_INVALID_MULTIPART`. Поток тела — `body()` — это `ReadableStream<Uint8Array, BodyError>`:
  `for await` по нему даёт `Result<Uint8Array, BodyError | ReaderError | ControllerClosedError>`
  (`ReaderError` — `READER_ERROR`).

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

HTTP/2 без TLS (h2c), «Hello, World!», `oha --http2 -c 32 -p 8` (32 соединения по 8 запросов
сразу) на той же машине, 31 октября (`bench/http2.ps1`):

| | запросов в секунду |
|---|---|
| Rust (hyper 1, tokio) | 757 000 |
| **tz** (nghttp2) | **379 000** |
| Go (`net/http` + `x/net/http2/h2c`) | 232 000 |

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
- `ConnectError` — `kind`: `CONNECT_ERROR_REQUEST` (`ERR_INVALID_HOST`,
  `ERR_ALREADY_CONNECTED`), `CONNECT_ERROR_NETWORK` (`ECONNREFUSED`, `ENOTFOUND`…),
  `CONNECT_ERROR_TLS`, `CONNECT_ERROR_TIMEOUT`; тексты — как у `fetch`. Ошибка потока `Socket`
  (31 октября, Т2) — `SocketError`: `SOCKET_ERROR_NETWORK` (`ECONNRESET`, `EPIPE`,
  `ECONNABORTED`, `ETIMEDOUT`, `ERR_CLOSED`, `UNKNOWN`; `syscall`, `host`, `port`) и
  `SOCKET_ERROR_TLS` (у сокета с TLS); `Socket extends Duplex<Buffer, Buffer, SocketError>`.
  Ошибка `listen()` у `Server` из `node:net` — `NetListenError` (`kind: "NET_LISTEN_ERROR"`):
  имя `ListenError` занято сервером `tz:http`.
- `Server` — обработчик `async (socket) => …` на каждое соединение, своей задачей;
  `idleTimeout` — сколько соединение может молчать; `stop({ timeout })` — как у `tz:http`.
- `isIP`, `isIPv4`, `isIPv6`.
- **Ещё не сделано** из решения ([tmp/spec/09-sockets.md](../tmp/spec/09-sockets.md)): слой
  `tz:socket` (сокет любого вида, сырые сокеты, все настройки), сокеты Unix и
  именованные каналы (`{ path }`), `SocketError` с метками у потока (сейчас поток отдаёт
  `StreamError`), обработчик-не-`async`, объявления для редактора (`patch.cmake`).

## 8. Пакеты: `tz:packet` (сделано 29 октября)

Сборка и разбор заголовков на tz над `Uint8Array`, без особых прав: `parseEthernet`,
`parseArp`, `parseIpv4` (проверяет контрольную сумму), `parseIpv6`, `parseUdp`, `parseTcp`
(флаги `TCP_SYN`, `TCP_ACK`…), `parseIcmp` (проверяет сумму); `buildIpv4`, `buildUdp` (сумма с
псевдозаголовком), `buildIcmpEcho`; `checksum(bytes, initial?)`, `transportChecksum(source,
destination, protocol, segment)`. Ошибки — `Result` с `PacketError`: `kind: "PACKET_ERROR"`,
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
const sentResult = socket.send("hello")         // Result<void, WebSocketSendError>
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
- Ошибки (`kind` — имя типа и причина, 31 октября): `WebSocketConnectError` —
  `WEB_SOCKET_CONNECT_ERROR_REQUEST` (`ERR_INVALID_URL`, `ERR_UNSUPPORTED_SCHEME`,
  `ERR_ALREADY_CONNECTED`), `…_NETWORK`, `…_TIMEOUT`, `…_REFUSED` (`ERR_NOT_UPGRADED`,
  `status`); `WebSocketError` (`"error"`) — `WEB_SOCKET_ERROR_NETWORK` (`ERR_CLOSED` —
  оборвалось без кадра закрытия), `…_PROTOCOL` (`ERR_INVALID_FRAME`, `ERR_INVALID_UTF8`,
  `ERR_UNMASKED_FRAME`), `…_TOO_LARGE`; `WebSocketSendError` (было `SendError`) —
  `WEB_SOCKET_SEND_ERROR` (`ERR_NOT_CONNECTED`, `ERR_CLOSING`); `UpgradeError` —
  `UPGRADE_ERROR` (`ERR_NOT_WEBSOCKET`, `ERR_UNSUPPORTED_VERSION`). С 29 октября `emit` в коде библиотеки
  сверяется с картой событий, как в программе.
- **Ещё не сделано** из решения ([tmp/spec/10-websocket.md](../tmp/spec/10-websocket.md)):
  `timeout` и `idleTimeout`, пинги рантайма, тип `CloseCode` (сейчас `close(code: int)`),
  `bufferedAmount()`, прокси и `httpVersion`, WebSocket поверх HTTP/2, `TLS` и `PROXY` в
  ошибках подключения, объявления для редактора.

## 10. UDP: `node:dgram` (сделано 29 октября)

```ts
import { Socket } from "node:dgram"

const socket = new Socket({ type: "udp4", port: 41234, address: "0.0.0.0" })  // настройки — тут
socket.on("message", (data, from) => { ... })   // data: Buffer; from: { address, family, port, size }
socket.on("error", error => { ... })
const boundResult = await socket.bind()          // Result<void, UdpBindError>
const sentResult = await socket.send("hi", { address: "127.0.0.1", port: 53 })  // Result<void, UdpSendError>
await socket.close()                             // ждёт конца приёма, потом "close"
```
- Решение — [tmp/design/udp.tz](../tmp/design/udp.tz) (U1–U9). `type` необязателен: без него
  один сокет на IPv4 и IPv6 (двойной стек), адрес отправителя IPv4 — в обычном виде
  (`127.0.0.1`, а не `::ffff:127.0.0.1`); `"udp4"` / `"udp6"` — ровно одна версия.
- Настройки — в конструкторе: `port`, `address` (IP, `localhost` или имя — ищется в DNS),
  `reuseAddr` / `reusePort` (в Windows оба — `SO_REUSEADDR`), `ipv6Only`, `broadcast`, `ttl`,
  `multicast: { ttl, loopback, interface }`, `recvBufferSize`, `sendBufferSize`.
- Приём — событие `"message"` (или цикл `on(socket, "message")`), с `bind()` до `close()`;
  программа живёт, пока сокет открыт. Клиенту `bind()` не нужен: первый `send()` или
  `connect()` сам займёт свободный порт.
- `send(data, { address, port })` — `string` (UTF-8) или `Uint8Array`, имя ищется в DNS.
  `connect({ address, port })` — «подключённый» сокет: `send(data)` без адреса, письма только
  оттуда; если там никто не слушает, следующий `send` — `Err` `ECONNREFUSED`. `disconnect()`.
- Мультикаст: `addMembership(group, interface?)`, `dropMembership`,
  `addSourceMembership(source, group, interface?)` (только IPv4), `dropSourceMembership` — после
  `bind()`, `Result`. `address()`, `remoteAddress()`.
- Ошибки (с 31 октября имена с `Udp`: `ConnectError` и `SocketError` есть и у `node:net`, а
  `kind` уникален во всей библиотеке): `UdpBindError` — `UDP_BIND_ERROR_REQUEST`
  (`ERR_INVALID_ADDRESS`, `ERR_SOCKET_BOUND`, `ERR_SOCKET_CLOSED`), `UDP_BIND_ERROR_NETWORK`
  (`EADDRINUSE`, `EACCES`, `EADDRNOTAVAIL`, `ENOTFOUND`…); `UdpSendError` —
  `UDP_SEND_ERROR_REQUEST` (`ERR_NO_ADDRESS`, `ERR_SOCKET_CONNECTED`, `ENOTFOUND`…), `…_NETWORK`
  (`ECONNREFUSED`, `ENETUNREACH`…), `…_TOO_LARGE` (`EMSGSIZE`: больше 65 507 байт у IPv4, 65 527
  у IPv6), `…_PERMISSION` (`EACCES`: широковещание без `broadcast: true`), `…_CLOSED`;
  `UdpConnectError`, `UdpMembershipError`; `"error"` — `UdpSocketError` (`UDP_SOCKET_ERROR`: то,
  что пришло не в ответ на `send`). Поля `errno` нет. Тексты — понятным языком.
- Рантайм — `runtime/library/net/udp/udp.c`: `WSARecvFrom` / `WSASendTo` на порту завершения
  цикла, поиск имён — `GetAddrInfoExW`; в Linux пока нет (как и TCP).
- **Ещё не сделано:** DTLS, QUIC (с HTTP/3), сырые UDP (`tz:socket`), объявления для
  редактора.
- **`ref()` / `unref()` нет** (решено 30 октября): открытый сокет держит программу, отпускает её
  `close()`; добавим, если понадобится.

## 11. Потоки HTTP/2 напрямую: `tz:http2` (сделано 31 октября)

`Server` и `fetch` прячут HTTP/2 за теми же `Request` и `Response`. `tz:http2` — для того, чего
в `Response` нет: трейлеры (заголовки после тела, на них стоит gRPC), полный дуплекс (запрос и
ответ идут одновременно) и отмена одного запроса с кодом из стандарта. Решения —
[tmp/spec/12-http2.md](../tmp/spec/12-http2.md) (30 и 31 октября), примеры —
[tmp/design/http2-layer.tz](../tmp/design/http2-layer.tz).

```ts
import { Http2Client, Http2Server } from "tz:http2"

const server = new Http2Server({ port: 50051 }, async (request, client) => {
  let asked = ""
  for await (const chunkResult of request.body()) {
    if (chunkResult.isErr()) return
    asked += new TextDecoder().decode(chunkResult.value)
  }
  const responded = request.respond({ status: 200, headers: { "content-type": "application/grpc" } })
  if (responded.isErr()) return
  const response = responded.value
  const wrote = await response.write(new TextEncoder().encode(`${asked}: 7 EUR`))
  if (wrote.isErr()) return
  const ended = await response.end({ "grpc-status": "0" })
  if (ended.isErr()) console.log(ended.error.message)
})

const connected = await new Http2Client({ url: "http://localhost:50051" }).connect()
if (connected.isOk()) {
  const started = await connected.value.request({ method: "POST", path: "/shop.Prices/Get" })
  // call.write(bytes), call.end(trailers?), call.cancel(code?), await call.response()
  // → response.status, response.headers, response.body(), response.trailers()
}
```

- **Сервер** — `new Http2Server(options, handler).listen()` → `Result<Http2Listening,
  ListenError>`; настройки — как у `Server`: `port`, `hostname`, `transport`,
  `maxConcurrentStreams` (100), `headersTimeout` (60 000), `keepAliveTimeout`. Только HTTP/2:
  без TLS — h2c «с первого байта», с TLS — ALPN только `h2`; клиент HTTP/1.1 получает `505 HTTP
  Version Not Supported`, по TLS без `h2` — отказ в рукопожатии. Общего срока на запрос и срока
  простоя соединения по умолчанию нет (как в node и Go): поток вроде «следить за ценой» живёт
  часами, срок ставит программа. Общий порт с `Server` — вместе с библиотекой gRPC (31.10).
- **Обработчик** — `(request: Http2IncomingRequest, client: Client) => Promise<void>`: у запроса
  `method`, `scheme`, `authority`, `path` (как пришли, с `?…`), `headers`, `body()`,
  `trailers()`, `respond({ status, headers? })` → `Http2OutgoingResponse` с `write(bytes)` и
  `end(trailers?)`. Обработчик закончился, а `end()` не было (или не было `respond()`) — поток
  сбрасывается с `INTERNAL_ERROR`: клиент видит ошибку, а не обрезанный ответ, похожий на
  успешный (31.10). Долгий ответ обработчик ждёт сам. Не дочитанное тело запроса после конца
  ответа закрывает только поток (`RST_STREAM` с `NO_ERROR`, как Go).
- **Клиент** — `new Http2Client({ url, timeout? }).connect()` → `Http2Connection`; `url` —
  строка или `URL`; https — ALPN, http — h2c «с первого байта». `connection.request({ method,
  path, headers?, authority? })` → `Http2OutgoingRequest`: `write`, `end(trailers?)`,
  `cancel(code?)`, `response()` → `Http2IncomingResponse` (`status`, `headers`, `body()`,
  `trailers()`). Заполненное соединение (`maxConcurrentStreams` сервера) — запрос ждёт места,
  как в gRPC у Go. Оборвалось соединение или пришёл GOAWAY — программа подключается заново сама
  (ничего скрытого: `fetch` тоже сам не повторяет, раздел 2); запрос, который сервер не трогал, — код
  `REFUSED_STREAM`, его можно повторить. `connection.close()` шлёт GOAWAY.
- **Тело — свой класс `Http2Body`** (31.10): `for await` и `read()` дают `Result<Uint8Array,
  Http2BodyError>` только с ошибками HTTP/2; `cancel(code?)` сбрасывает поток с кодом RFC 9113
  (`CANCEL` без кода) — в HTTP/2 закрыть одну сторону нельзя, сброс закрывает весь поток;
  `toStream()` — `ReadableStream`, чтобы отдать тело в `fetch` или `Response.stream`. Тело
  берётся прямо из буфера рантайма. Скорость в обе стороны держит окно HTTP/2: программа не
  читает — другая сторона ждёт (окно потока 1 МБ, соединения 16 МБ); `write()` ждёт, пока
  другая сторона готова принять. Два чтения одного тела сразу — ошибка `HTTP2_BODY_ERROR_BUSY`.
- **Трейлеры** — `trailers()` после того, как тело прочитано до конца; раньше — ошибка
  `HTTP2_TRAILERS_ERROR_BODY_NOT_READ`. В `end(trailers)` псевдозаголовки и запрещённые в
  трейлерах поля (`content-length`, `host`, `te`, поля соединения) выкидываются. `te: trailers`
  в запросе проходит (его требует gRPC).
- **Ошибки** — тип на функцию, `kind` уникален ([errors.md](errors.md)): `Http2ConnectError`
  (`HTTP2_CONNECT_ERROR_URL`, `_NETWORK`, `_TLS`, `_TIMEOUT`, `_NOT_HTTP2`),
  `Http2RequestError` (`_INVALID`, `_CLOSED` с `ERR_HTTP2_GOAWAY`, `ERR_HTTP2_CLOSED`,
  `ERR_HTTP2_CONNECTION_LOST`, `ERR_HTTP2_NOT_SUPPORTED`), `Http2RequestWriteError`,
  `Http2RequestEndError`, `Http2ResponseWaitError`, `Http2RespondError` (`…_ALREADY_RESPONDED`,
  `…_INVALID_STATUS` для статуса не из 200–599), `Http2ResponseWriteError`,
  `Http2ResponseEndError`, `Http2TrailersError`, `Http2BodyError` (интерфейсы от `StreamError`).
  Сброс потока — вид `…_RESET` с кодом `Http2ErrorCode` (имена RFC 9113: `NO_ERROR` …
  `HTTP_1_1_REQUIRED`); обрыв соединения — `…_CONNECTION`. h2c-клиент к серверу HTTP/1.1 —
  `ERR_HTTP2_NOT_SUPPORTED` («ответил по HTTP/1.1»).
- **Скорость.** «Hello, World!» через `respond` + `write` + `end` (`bench/http2_hello/layer`) —
  38 000 запросов в секунду против 376 000 у `Server`: обработчик с `await` идёт по очереди
  ([async.md](async.md), раздел 8; у `Server` с `await request.text()` — те же 85 000). Сам слой
  состояния в tz не держит — флаги потока, трейлеры и ожидания в рантайме, функции рантайма
  очереди не ждут; ускорится вместе с обработчиками `Server`.
- **Тест** — `run/http2_layer`: четыре вида вызовов gRPC (один запрос — один ответ с
  трейлерами, поток ответов до отмены, поток запросов, обе стороны сразу), сброс без `end()`,
  трейлеры раньше тела, `toStream()`, TLS, два чтения сразу, сервер без HTTP/2, отказ в
  соединении.
- **Ещё нет:** `.d.tz` с описаниями для редактора, пинги и настройки соединения (SETTINGS),
  приоритеты (RFC 9218), общий порт с `Server`.

## 12. Позже

HTTP/2 (через ALPN в TLS), SOCKS-прокси, HTTP/3 — решены 29 октября, не сделаны (`tmp/design`, `tmp/spec`). `node:http` не будет.

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
