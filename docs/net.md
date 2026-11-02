# Сеть

Решения, принятые 24 октября 2026 года, и поправки 26 октября: ошибки сети — `Result`,
обработчик сервера разбирает ошибки сам ([errors.md](errors.md), раздел 4); 28 октября:
сервер — только `new Server(options, handler)` и `listen()`, без `serve`; `listen()` отдаёт
`Listening`, а соединения, keep-alive и тайм-ауты — забота рантайма. Шаги 1–4 плана
сделаны 24–25 октября, шаг 6 — 26–28 октября; что осталось — в конце, в разделе «План реализации».

## Коротко

- Сервер — `Server` из `"tz:http"`: обработчик получает `Request` (и того, кто его прислал) и
  возвращает `Response`, как в Deno и Bun; создать с настройками, потом `listen()`. Роутера во
  встроенной библиотеке нет (решено 2.11): маршруты пишет программа (условия по пути и
  регулярные выражения с группами) или фреймворк поверх `Server`.
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

Маршруты — без фреймворка: путь из `URL.parse(request.url)` (~0,2 мкс, раздел 6) и регулярные
выражения, которые собираются при сборке, с группами по именам:

```ts
const app = async (request: Request): Promise<Response> => {
  const url = URL.parse(request.url)
  const path = url.isOk() ? url.value.pathname() : "/"
  if (path == "/health") return new Response("ok")
  const user = path.match(/^\/users\/(?<id>[^/]+)$/)
  if (request.method == "GET" && user != undefined) return Response.json({ id: user.groups.id })
  return Response.json({ error: "no such path" }, { status: 404 })
}
const listening = new Server({ port: 3000 }, request => app(request)).listen()
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
- **Отказ доходит до клиента** (1.11). Ответив отказом (413, 408, 400…) и закрыв соединение,
  сервер HTTP/1.1 ещё до 2 с (или до 1 МБ) читает и выбрасывает то, что клиент дошлёт, и
  закрывает сокет после клиента. Без этого остаток тела, пришедший после отказа, заставлял систему
  сбросить соединение, и клиент терял уже пришедший ответ. Пока без TLS. Тест —
  `run/http_refuse_late_body`.
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
  программа для `fetch`. Так же у Deno, и фреймворки в стиле `fetch` это ждут:
  `app.fetch(request, env?)` получает его вторым аргументом (функцию, у которой там `unknown`, tz
  принимает как обработчик).
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
  `http2?: { enabled: false } | { enabled: true; cleartext?: boolean; maxConcurrentStreams?: int;
  webSocket?: boolean }` — без неё включено при `secure: true` и выключено без TLS;
  `webSocket: false` — не объявлять WebSocket поверх HTTP/2 (раздел 9); `cleartext: true` — HTTP/2 без TLS
  (h2c) только «с первого байта», переход `Upgrade: h2c` убран из стандарта и не делается.
  Обработчик тот же, `client.protocol` — `"http/1.1"` или `"h2"`. Каждый поток HTTP/2 — своя
  задача на любом ядре; поток в рантайме — то же соединение для `Request` и `Response`, так что
  тела в обе стороны, потоковые ответы и `maxBodySize` работают как в HTTP/1.1. Движок —
  nghttp2 1.70.0 ([third_party/nghttp2](../third_party/nghttp2), MIT, в `tz licenses`): кадры,
  HPACK, окна, защита от Rapid Reset и CONTINUATION flood — его. Пределы: 100 потоков сразу,
  заголовки — 16 КБ, окно потока — 1 МБ, соединения — 16 МБ. Окно открывается, когда программа
  читает тело: не читает — клиент ждёт, память не растёт (31.10). Тело, которое обработчик не
  дочитал, закрывает только поток (`RST_STREAM` с `NO_ERROR`, как Go). Клиент закрыл свою
  сторону TCP (FIN) — для HTTP/2 это «клиент ушёл»: соединение закрывается, ответы в работе не
  уходят, обработчики доделывают своё (решено 1 ноября; так же Go и `h2` у hyper). «Я всё
  отправил» в HTTP/2 — `END_STREAM` у потока; HTTP/1.1 после FIN клиента ещё отвечает.
  `stop()` шлёт GOAWAY (сначала предупреждение, потом с последним принятым потоком) и ждёт
  начатых. **Сроки у потоков** (1.11) — те же настройки. `headersTimeout` — столько может идти
  блок заголовков потока (и трейлеров): дольше — 408 на этом потоке, `RST_STREAM` с `NO_ERROR`
  и GOAWAY, потому что посреди блока заголовков соединение больше ничего не пропустит; начатые
  потоки доотвечают, потом соединение закрывается. `requestTimeout` — столько может идти весь
  запрос, от первого кадра до `END_STREAM`: дольше — обработчик, который читает тело, получает
  `BODY_ERROR_TIMEOUT`, клиент — 408 и `RST_STREAM` с `NO_ERROR`; ответ уже идёт потоком —
  поток сбрасывается с `CANCEL`; полный ответ уже отдан — только `RST_STREAM` с `NO_ERROR`.
  Соединение и другие потоки живут дальше. Тест — `run/http2_timeouts`. WebSocket поверх
  HTTP/2 — раздел 9. Потоки напрямую —
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
  счёта (не держит программу). Тот же движок nghttp2, что у сервера. Через прокси `fetch` всегда
  идёт туннелем `CONNECT` по HTTP/1.1, как Go и Rust (hyper-util): HTTP/2 к прокси не делаем
  (решено 1 ноября). Тест — `run/fetch_http2`; с Rust hyper (h2c) проверено руками.
- **Шаги запроса — `trace`** (сделано 1 ноября, H15; как `httptrace` в Go): `fetch(url, { trace:
  step => … })` отдаёт функции каждый шаг — объект `FetchStep` с меткой `kind` и `elapsed`
  (миллисекунды от вызова `fetch`). Шаги: `FETCH_STEP_LOOKUP_STARTED` / `_LOOKUP_DONE` (имя →
  адреса; у адреса-числа их нет), `_CONNECTING`, `_CONNECT_FAILED` (адрес и код — и следующий
  адрес), `_CONNECTED` (свой адрес), `_PROXY_TUNNEL_OPENED`, `_TLS_DONE` (протокол по ALPN и
  версия TLS), `_CONNECTION_REUSED` (с `idle` — сколько соединение простояло),
  `_CONNECTION_CLOSED_BY_SERVER` (соединение из пула оказалось закрыто — берётся новое),
  `_CONNECTION_FULL` (у соединения HTTP/2 нет свободного потока — открывается второе),
  `_REQUEST_SENT` (`bytes` — байты тела запроса: голова HTTP/2 сжимается HPACK только при
  отправке кадра), `_RESPONSE_STARTED`, `_REDIRECTED`, `_BODY_RECEIVED` (байты тела, как
  пришли, до распаковки), `_CONNECTION_RETURNED` (в пул), `_CONNECTION_CLOSED` (`SERVER_ASKED`,
  `BODY_NOT_READ` — программа отменила тело, `ERROR`). Без `trace` ничего не создаётся.
  Поиск адресов и попытки подключения теперь идут из `fetch` по одной (`netLookup`, потом
  `netConnect` на каждый адрес): так шаги видны, а поведение то же — IPv4 раньше IPv6, ошибка —
  первой неудачной попытки. Отмена тела ответа (`body().cancel()`) сразу закрывает соединение
  (HTTP/1.1) или сбрасывает поток (HTTP/2). Тест — `run/fetch_trace`, туннель — `cli/proxy`.
- Распаковка gzip, deflate и brotli, переходы по редиректам (до 20), соединения с keep-alive
  держатся в пуле — как в node. **`fetch` никогда не повторяет запрос сам** (H12, 31 октября):
  запрос уходит один раз, и `POST` тоже; повторять и писать в журнал — дело программы. Чтобы
  соединение из пула, которое сервер успел закрыть, не съело запрос, перед отправкой рантайм
  проверяет его (H13: готово ли оно к чтению — пришёл конец, сброс или лишние байты — значит,
  мёртвое) и берёт новое; запрос в мёртвое соединение не уходит. Тест — `run/fetch_pool`.
- **Прокси** (сделано 29 октября, SOCKS и Digest — 1 ноября): из окружения само — `HTTPS_PROXY` для
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
  отвечает — `FETCH_ERROR_NETWORK` с адресом прокси и словами «the proxy» в тексте. Прокси через
  HTTP/2 не будет (1 ноября).
  - **SOCKS** (1 ноября): адрес прокси `socks4://`, `socks4a://`, `socks5://`, `socks5h://`
    (порт по умолчанию 1080) — и в `proxy.url`, и в `HTTPS_PROXY` / `HTTP_PROXY`. Туннель SOCKS —
    и для http://, и для https:// (потом TLS прямо с сервером). `socks4` и `socks5` ищут адрес
    сервера сами (SOCKS4 — только IPv4), `socks4a` и `socks5h` отдают имя прокси. Пользователь
    в адресе — USERID у SOCKS4 (пароль у SOCKS4 — `ERR_INVALID_PROXY_URL`), логин и пароль у
    SOCKS5 (RFC 1929). Отказ прокси — `ERR_PROXY_REFUSED` со словами ответа SOCKS (`status` —
    0), логин не принят или не дан — `ERR_PROXY_AUTH_REQUIRED`.
  - **Digest** (1 ноября): 407 с `Proxy-Authenticate: Digest` (MD5, MD5-sess, SHA-256,
    SHA-256-sess; `qop=auth` или без него) при логине в адресе — тот же запрос ещё раз с
    ответом Digest (для `CONNECT` — новое соединение; для http:// — если тело можно послать
    снова). Вызов запоминается для этого прокси и логина: следующие запросы идут сразу с Digest
    (счётчик `nc` растёт), новый 407 — ещё одна попытка с новым `nonce`, тот же `nonce` — значит,
    пароль не тот: `ERR_PROXY_AUTH_REQUIRED`. Сначала, как и раньше, посылается Basic (как curl
    и Go). MD5 и SHA-256 — на tz (`library/http/fetch_proxy.tz`, без OpenSSL).
  - Тест — `run/fetch_socks` (SOCKS4/4a/5/5h, логин, отказы, Digest MD5 и SHA-256, WebSocket
    через SOCKS5 и Digest); код прокси — часть `tz:fetch` в `library/http/fetch_proxy.tz`.
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
глобальной переменной), идёт в порядке node через главный поток — 92 000. Обработчик, который
трогает только свой `request` (`request.headers.get(…)`, `await request.text()`), с 31.10 идёт
параллельно — 300 000 (до этого 85–92 000; [async.md](async.md), раздел 8, «Свои объекты
вызова»).

HTTP/2 без TLS (h2c), «Hello, World!», `oha --http2 -c 32 -p 8` (32 соединения по 8 запросов
сразу) на той же машине, 31 октября – 1 ноября (`bench/http2.ps1`):

| | запросов в секунду |
|---|---|
| **tz** (nghttp2, Pendulum) | **1 610 000 – 1 750 000** |
| Rust (hyper 1, tokio) | 716 000 – 758 000 |
| Go (`net/http` + `x/net/http2/h2c`) | 221 000 – 228 000 |

До Pendulum у tz было 376–379 тыс.: 40% времени уходило на `WSASend`, по одному–три на ответ.
Теперь потоки одного соединения отправляются одной записью за такт ([async.md](async.md), раздел 8,
«Pendulum»), а ответ рядом с долгим расчётом уходит не позже чем через ~2 мс. С `await
request.text()` и у `tz:http2` с `write` и `end` — 1,54–1,68 млн.

**По ядрам** (2 ноября; «Hello, World!» по HTTP/1.1, сервер привязан к N ядрам, `TZ_THREADS=N` /
`GOMAXPROCS=N`, нагрузка — 128 соединений с 16 других логических ядер; HTTP — нагрузка из
`bench/http_hello/load`, HTTPS — `oha`), тысяч запросов в секунду:

| ядер | tz HTTP | Go HTTP | tz HTTPS | Go HTTPS |
|---|---|---|---|---|
| 1 | 65 | 58 | 54 | 50 |
| 2 | 106 | 97 | 98 | 97 |
| 4 | 176 | 164 | 170 | 166 |
| 8 | 280 | 237 | 263 | 246 |

До 2 ноября соединения вели только потоки пула, а их N − 1 (главный поток считался рабочим, хотя
у сервера он только ждёт): на 2 ядрах tz давал те же 65 тыс., что на одном, на 4 — 149 тыс. А
соединения с TLS в пул не шли вовсе — весь TLS шёл через главный поток, и HTTPS упирался в него:
49 / 85 / 124 / 164 тыс. на 1 / 2 / 4 / 8 ядрах, занято 4 ядра из 8. Теперь сервер добавляет
пулу поток, а соединения с TLS идут в пул, как обычные (`tests/run/tls_serve_load`). На одном ядре
сервер по-прежнему работает в главном потоке. На всей машине — без изменений (319 тыс., Go 240).
Под Linux Go на одном ядре даёт 86–88 тыс. против 57–60 под Windows (WSL, та же машина): часть
разницы с Linux — сама система.

**Разбор адреса для маршрутов** (2 ноября; один поток, 2 млн раз,
`URL.parse("http://127.0.0.1:3000/users/42").pathname()`): было 2,4 мкс — пятая часть запроса
HTTP/2 на одном ядре; стало ~0,21 мкс (`URL.canParse` — 0,15 мкс). Что было: быстрый путь разбора
отказывался от хоста-IP (последняя часть хоста — цифра) и отдавал адрес полному разбору по
стандарту (~2 мкс); теперь IPv4 в канонической записи (четыре числа 0–255 без ведущих нулей —
запись по стандарту та же) идёт быстрым путём. И быстрый путь, когда запись адреса по стандарту
совпадает со входом (порт не по умолчанию и без ведущих нулей, путь не пустой), сразу даёт поля
`URL` срезами входной строки (`href` — сам вход), без склеек: 0,44 → 0,21 мкс. Поля `URL`
по-прежнему готовы при создании, а методы-читатели (`href()`, `pathname()`…) только читают поле:
анализ параллельности считает «только читающим» библиотечный метод без вызовов внутри, и чтение
адреса не упорядочивает обработчики (ленивые поля с кэшем пробовал — стали бы записью, и
обработчики, которые разбирают адрес, шли бы по очереди). Сам выбор маршрута — 23 нс на 4
маршрута и регулярными выражениями, и `split("/")`.

## 7. TCP: `node:net` (сделано 29 октября — 1 ноября)

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
- **Сокеты Unix и именованные каналы** (1 ноября): `new Socket({ path })` и `new Server({ path },
  …)` — настройки адреса — объединение `{ host; port; … } | { path; … }`. Путь вида
  `\\.\pipe\имя` — именованный канал Windows (`CreateNamedPipe` / `ConnectNamedPipe`, чтение и
  запись — `ReadFile` / `WriteFile` на том же IOCP), любой другой — сокет Unix (`AF_UNIX`,
  Windows 10 1803+). Файл сокета Unix сервер удаляет в `stop()`; занятый путь — `EADDRINUSE`
  (у канала — тоже). Ошибки подключения — ещё `ENOENT` (никто не слушает этот путь), `EBUSY`
  (все экземпляры канала заняты), `EACCES`; у ошибок `host` — путь, `port` — 0.
  `Listening.path` — путь, `port` — 0; `remoteAddress()` сокета — путь.
- **Настройки сокета** (T8, 1 ноября): у `Socket` — `localAddress`, `localPort` (привязка перед
  подключением), `allowHalfOpen` (`end()` — только `FIN`, чтение идёт дальше), `noDelay`,
  `keepAlive` с `keepAliveDelay` / `keepAliveInterval` (мс) и `keepAliveCount`,
  `sendBufferSize`, `receiveBufferSize`, `ttl`, `typeOfService`, `userTimeout` (мс,
  `TCP_MAXRTMS` в Windows), `linger` (с, `0` — `RST` при закрытии); у `Server` — те же для
  принятых соединений и ещё `backlog` (511), `ipv6Only`, `reusePort` (в Windows `SO_REUSEPORT`
  нет — `listen()` даёт `Err` `ENOTSUP`, как node). Методы `localAddress()`, `localPort()`.
  Настройку, которую система не приняла, `Socket` пропускает молча, как node. `interface`
  (привязка к сетевому интерфейсу) — нет (в Windows — индекс интерфейса, не имя; потом).
  Тест — `run/net_path`.
- **Ещё не сделано** из решения ([tmp/spec/09-sockets.md](../tmp/spec/09-sockets.md)):
  обработчик-не-`async`, объявления для редактора (`patch.cmake`).

### Сокет любого вида: `tz:socket` (сделано 1 ноября)

```ts
import { Socket } from "tz:socket"

const ping = new Socket({ domain: "ipv4", type: "raw", protocol: 1 })   // ICMP
const sentResult = await ping.sendTo(packet, { address: "8.8.8.8", port: 0 })
if (sentResult.isErr() && sentResult.error.kind == "SOCKET_OPERATION_ERROR_PERMISSION") { … }

const server = new Socket({ domain: "ipv4", type: "stream" })
server.configure({ reuseAddress: true })        // Result; все настройки с типами
server.bind({ address: "127.0.0.1", port: 0 })  // Result
server.listen(128)                              // Result
const acceptedResult = await server.accept()    // Promise<Result<Socket, SocketOperationError>>
```
- Как `socket2` у Rust: `new Socket({ domain: "ipv4" | "ipv6" | "unix", type: "stream" |
  "datagram" | "raw" | "seqpacket", protocol? })` — только настройки, системный сокет создаётся
  при первом действии. Действия — `bind`, `listen`, `accept`, `connect`, `send` / `sendTo`
  (`outOfBand`, `dontRoute`; отдают число байтов), `receive` / `receiveFrom` (`size`, `peek`,
  `outOfBand`, `waitAll`; `receiveFrom` — `Datagram` с адресом отправителя), `shutdown("read" |
  "write" | "both")`, `configure(settings)`, `close()`; адреса — `localAddress()` /
  `remoteAddress()` (`Endpoint`). Адрес — `{ address, port }` (только IP-литерал, имён нет: DNS —
  отдельно) или `{ path }` у `unix`; `""` при `bind` — любой адрес.
- Настройки `configure`: `reuseAddress`, `broadcast`, `keepAlive` (+ `keepAliveDelay`,
  `keepAliveInterval`, `keepAliveCount`), `noDelay`, `linger`, `sendBufferSize`,
  `receiveBufferSize`, `ttl`, `typeOfService`, `ipv6Only`, `headerIncluded` (`IP_HDRINCL`),
  `multicastTtl`, `multicastLoop`, `userTimeout`, `outOfBandInline`, `dontFragment`. Первую,
  которую система не приняла, `configure` возвращает `Err` вида `SETTING` с её именем.
- Ошибки — `SocketOperationError` (имя `SocketError` занято `node:net`):
  `SOCKET_OPERATION_ERROR_PERMISSION` (`EACCES`; у сырого сокета в тексте подсказка «run the
  program as administrator (Linux: setcap cap_net_raw+ep ./app)» — это N6d),
  `…_REQUEST` (`ERR_INVALID_ADDRESS`, `ERR_SOCKET_CLOSED`, `ERR_SOCKET_BUSY`,
  `ERR_NOT_SUPPORTED`, `ERR_INVALID_ARG_VALUE`), `…_SYSTEM` (код системы: `ECONNREFUSED`,
  `ESOCKTNOSUPPORT`…), `…_SETTING` (`setting`).
- Рантайм — `runtime/library/net/socket/socket.c`: свой объект над `SOCKET` на том же IOCP
  (`AcceptEx`, `ConnectEx`, `WSARecvFrom`, `WSASendTo`). `node:net` и `node:dgram` на него **не**
  переведены (решено 1 ноября): у них свой быстрый путь (буферы рантайма, пул, TLS, HTTP), а
  перевод дал бы лишний слой на каждом чтении; `tz:socket` — для того, чего у них нет. Ограничения
  Windows: сырой TCP слать нельзя, `seqpacket` — нет, `AF_UNIX` — только `stream`. Тест —
  `run/socket_any`.

## 8. Пакеты: `tz:packet` (сделано 29 октября)

Сборка и разбор заголовков на tz над `Uint8Array`, без особых прав: `parseEthernet`,
`parseArp`, `parseIpv4` (проверяет контрольную сумму), `parseIpv6`, `parseUdp`, `parseTcp`
(флаги `TCP_SYN`, `TCP_ACK`…), `parseIcmp` (проверяет сумму); `buildIpv4`, `buildUdp` (сумма с
псевдозаголовком), `buildIcmpEcho`; `checksum(bytes, initial?)`, `transportChecksum(source,
destination, protocol, segment)`. Ошибки — `Result` с `PacketError`: `kind: "PACKET_ERROR"`,
`ERR_TRUNCATED`, `ERR_INVALID_HEADER`, `ERR_INVALID_CHECKSUM`, поле `offset`. Отправлять пакеты
нечем, пока нет сырых сокетов `tz:socket` (ping, traceroute — потом).

## 9. WebSocket (сделано 29 октября — 1 ноября)

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
- **WebSocket поверх HTTP/2** (RFC 8441, сделано 1 ноября): сервер с HTTP/2 объявляет его сам
  (`SETTINGS_ENABLE_CONNECT_PROTOCOL`; `http2: { webSocket: false }` — не объявлять), и
  `upgradeWebSocket` принимает оба способа: HTTP/1.1 (ответ 101) и поток HTTP/2 (`CONNECT` с
  `:protocol: websocket`, ответ 200) — обработчик тот же, `client.protocol` скажет, какой.
  Клиент выбирает `httpVersion?: "auto" | "1.1" | "2"` в настройках `WebSocket`: `"auto"` (по
  умолчанию) — потоком по соединению HTTP/2, уже открытому к этому серверу (например, `fetch`),
  если сервер объявил, иначе отдельным HTTP/1.1, как раньше; `"2"` — всегда потоком HTTP/2
  (`wss:` — HTTP/2 по ALPN, `ws:` — h2c), а сервер без HTTP/2 или без объявления —
  `WEB_SOCKET_CONNECT_ERROR_REFUSED` с кодом `ERR_NO_HTTP2` или `ERR_NO_EXTENDED_CONNECT`;
  `"1.1"` — всегда HTTP/1.1. У запроса `CONNECT` поле `request.connectProtocol` — его
  `:protocol`. Тест — `run/websocket_http2`.
- **Сроки, пинги, коды закрытия, прокси** (сделано 1 ноября):
  - `timeout` — срок подключения целиком (адрес, TCP, TLS, прокси, рукопожатие; у потока
    HTTP/2 — сброс потока): `WEB_SOCKET_CONNECT_ERROR_TIMEOUT`, `TIMEOUT_ERR`.
  - `idleTimeout` (у клиента в настройках, у сервера — в `upgradeWebSocket`; 120 000 мс, `0` —
    без срока): тишина половину срока — ping, весь срок — `"error"`
    `WEB_SOCKET_ERROR_TIMEOUT` `ERR_IDLE_TIMEOUT` и `"close"` 1006. Таймер — на tz.
  - `close(code?: CloseCode, reason?)`: `CloseCode = 1000 | 3000 … 4999` — тип в прелюдии, как
    `HttpStatus`; число из переменной подходит после проверки, доказывающей предел
    (`if (code >= 3000 && code <= 4999)`; сообщение компилятора подскажет). Отдельной функции
    `isCloseCode` нет: проверка «1000 или 3000–4999» одной функцией не сужает (факты
    компилятора — только «и»). Причина длиннее 123 байт обрезается по целому символу.
  - `bufferedAmount()` — байты, отданные `send()`, но ещё не ушедшие в сеть.
  - Ошибки подключения — ещё `WEB_SOCKET_CONNECT_ERROR_TLS` (коды `TlsCode`, как у `fetch`) и
    `WEB_SOCKET_CONNECT_ERROR_PROXY` (`ERR_INVALID_PROXY_URL`, `ERR_PROXY_AUTH_REQUIRED`,
    `ERR_PROXY_REFUSED`, `status`); у `"error"` — коды сети `ECONNRESET`, `ECONNABORTED`,
    `ERR_TLS_FAILED`, `UNKNOWN`.
  - Прокси — как у `fetch`: `proxy` в настройках или `HTTP_PROXY` (для `ws:`) / `HTTPS_PROXY`
    (для `wss:`) и `NO_PROXY`; всегда туннелем `CONNECT`, и для `ws:` (прокси не видит
    кадров). Через прокси — только HTTP/1.1, как у `fetch`.
  Тест — `run/websocket_limits`.
- **Ещё не сделано:** объявления для редактора.

## 10. UDP: `node:dgram` (сделано 29 октября, DTLS — 1 ноября)

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
- **DTLS** (1 ноября): `transport: { secure: true, … }` в настройках `Socket`. Сервер —
  `{ secure: true, cert, key }`: после `bind()` у каждого адреса-собеседника своя сессия DTLS
  1.2 (с обменом cookie — HMAC-SHA256 адреса на случайном ключе сокета, против подмены адреса;
  не больше 4096 сессий; незаконченное рукопожатие живёт 10 с), `"message"` несёт расшифрованные
  данные, `send(data, { address, port })` шифрует для сессии этого адреса (нет сессии —
  `UDP_SEND_ERROR_REQUEST` `ERR_NO_SESSION`). Клиент — `{ secure: true, servername? }`:
  `connect({ address, port })` делает рукопожатие и проверяет сертификат и имя (как у `fetch`;
  `servername` по умолчанию — адрес), ошибка — `UDP_CONNECT_ERROR_TLS` (`code` — как у TLS:
  `ERR_TLS_CERT_ALTNAME_INVALID`, `DEPTH_ZERO_SELF_SIGNED_CERT`…, `ETIMEDOUT` за 10 с); до
  `connect()` `send` — `ERR_NOT_CONNECTED`. Неверный сертификат или ключ сервера —
  `UDP_BIND_ERROR_TLS`. `close()` шлёт `close_notify` каждой сессии. Повторы рукопожатия —
  таймер рантайма раз в 100 мс (`DTLSv1_handle_timeout`), MTU — 1500. Рантайм — `tls.c`
  (OpenSSL, `BIO_s_dgram_mem`), `udp.c` отдаёт ему каждое письмо сокета с DTLS. OpenSSL теперь
  собран с DTLS (`build.ps1` без `no-dtls`): размер программ с TLS не изменился (замер 1 ноября:
  https-клиент 6 209 024 → 6 208 512 байт). Тест — `run/dgram_dtls`.
- **Ещё не сделано:** объявления для редактора. Сырые UDP — `tz:socket` (раздел 7). QUIC и
  HTTP/3 не будет — раздел 12.
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
  `maxConcurrentStreams` (100), `headersTimeout` (60 000, работает как у `Server`, раздел 1),
  `keepAliveTimeout`. Только HTTP/2:
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
  165 000 запросов в секунду против 388 000 у `Server` (31.10; до этого 38 000 — обработчик с
  `await` шёл по очереди). Запрос и ответ свои у вызова обработчика ([async.md](async.md),
  раздел 8, «Свои объекты вызова»). Сам слой состояния в tz не держит — флаги потока, трейлеры и
  ожидания в рантайме, функции рантайма очереди не ждут. Остаток разницы — в рантайме: потоки
  одного соединения часто идут через главный поток, общие группы объектов — под одним замком.
- **Тест** — `run/http2_layer`: четыре вида вызовов gRPC (один запрос — один ответ с
  трейлерами, поток ответов до отмены, поток запросов, обе стороны сразу), сброс без `end()`,
  трейлеры раньше тела, `toStream()`, TLS, два чтения сразу, сервер без HTTP/2, отказ в
  соединении.
- **Ещё нет:** `.d.tz` с описаниями для редактора, пинги и настройки соединения (SETTINGS),
  приоритеты (RFC 9218), общий порт с `Server`.

## 12. Позже

`node:http` не будет.

**HTTP/3 и QUIC не будет** (решено 1 ноября). Замер движков (ngtcp2 + nghttp3, OpenSSL QUIC,
quic-go; `tmp/spec/13-http3.md`): HTTP/3 на запрос дороже HTTP/2 по процессору и у сервера, и у
клиента, а выигрывает он только у клиентов на сети с потерями и при смене сети — на сервере за
балансировщиком этого не видно. Кому HTTP/3 нужен — ставит перед сервером tz прокси, который его
умеет (Caddy, nginx, Cloudflare): наружу HTTP/3, к tz — HTTP/1.1 или HTTP/2. Свой HTTP/3 поверх
`node:dgram` программа не соберёт: QUIC нужен TLS 1.3 без записей TLS, а у `node:crypto` такого
нет; это — пакетом поверх ngtcp2, когда будет FFI.

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
