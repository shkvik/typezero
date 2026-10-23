# Сеть

Решения, принятые 24 октября 2026 года. Пока ничего из этого не реализовано: порядок работ — в
конце, в разделе «План реализации».

## Коротко

- Сервер — `serve` из `"tz:http"`: обработчик получает `Request` и возвращает `Response`, как в
  Deno и Bun. На этом же построен hono ([packages.md](packages.md)).
- Клиент — `fetch`, как в JS.
- Запросы обрабатываются параллельно на всех ядрах ([async.md](async.md)). Цель — не меньше
  Go.
- TLS — OpenSSL, как в node ([crypto.md](crypto.md)). Попадает в exe, только если программа
  работает с https: около 4 МБ (Go: https-клиент целиком — 5,3 МБ). Шифрование трафика — в 3
  раза быстрее, чем в Go. Корневые сертификаты Mozilla — внутри.
- `node:http` (`createServer`), HTTP/2, WebSocket, сокеты TCP/UDP — позже.

## 1. Сервер

```ts
import { serve } from "tz:http"

const server = serve({ port: 3000 }, async req => {
  const url = new URL(req.url)
  if (req.method == "POST" && url.pathname == "/users") {
    const user = await req.json()                      // Json
    return Response.json({ id: 1, user }, { status: 201 })
  }
  return new Response("Not found", { status: 404 })
})
```

С hono:

```ts
const app = new Hono()
app.get("/", c => c.text("Hello"))
serve({ port: 3000 }, app.fetch)
```

- **Параметры:** `port`, `hostname` (по умолчанию `0.0.0.0`), `tls: { cert, key }` — строки PEM
  для https, `maxBodySize` (по умолчанию 128 МБ, как в Bun).
- **`server.stop()`** — перестать принимать соединения и дождаться запросов, которые уже идут.
  `server.port` — порт, если передан `port: 0`.
- **Исключение в обработчике** — ответ `500 Internal Server Error`, ошибка с трассой — в stderr;
  сервер работает дальше.
- **Параллельно.** Каждый запрос — async-задача, обработчики идут на всех ядрах. Общие данные
  (счётчик на уровне модуля, кэш в `Map`) — по очереди, как в [async.md](async.md); код
  остаётся таким же, как для node.
- `node:http` (`createServer((req, res) => …)`) — позже, если понадобится: он держится на
  `EventEmitter` и потоках node, а express на нём — JS, его всё равно не скомпилировать.

## 2. Клиент: `fetch`

```ts
const r = await fetch("https://api.github.com/repos/denoland/deno", {
  signal: AbortSignal.timeout(5000),
})
if (!r.ok) throw new Error(`GitHub: ${r.status}`)
const repo = await r.json()                            // Json
```

- `Request`, `Response`, `Headers`, `URL`, `URLSearchParams`, `FormData`, `AbortController`,
  `AbortSignal.timeout` — как в JS. `json()` возвращает `Json` ([modules.md](modules.md)).
- Распаковка gzip, deflate и brotli, переходы по редиректам (до 20), соединения с keep-alive
  держатся в пуле — как в node.
- **Ошибки — как в node:** `TypeError("fetch failed")`, причина в `cause` — `SystemError` с
  `ECONNREFUSED`, `ENOTFOUND`, `ETIMEDOUT` ([io.md](io.md)) или ошибка сертификата.

## 3. Тела запросов и ответов

- Тело — поток кусков `Uint8Array` ([bytes.md](bytes.md)): `for await (const chunk of req.body)`.
  Целиком — `text()`, `json()`, `bytes()`, `arrayBuffer()`, `formData()`.
- Ответ — строка, `Uint8Array`, `ReadableStream` (отдавать по кускам, например события для
  браузера) или `Response.json(значение)`.
- `ReadableStream` — основное: создание с `start` / `pull` / `cancel`, `getReader()`,
  `for await`. `TransformStream`, `TextDecoderStream`, `CompressionStream` — позже.

## 4. TLS

- **OpenSSL 3.5** — та же библиотека и версия, что в node 24 ([crypto.md](crypto.md)): TLS 1.2
  и 1.3, те же шифры и коды ошибок. Встраивается, только если программа ходит по `https://` или
  запускает `serve` с `tls`.
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
записью в сокет. Замеры — в `bench`, тем же способом.

## 7. Позже

`node:http`, HTTP/2 (через ALPN в TLS), WebSocket (сервер и клиент), `node:net` и `node:dgram`,
прокси для `fetch`, HTTP/3.

## План реализации

На каждом шаге: тесты `tests/run` (сервер и клиент в одной программе, на свободном порту),
ошибки в `tests/errors`, прогон под `TZ_CHECK_LEAKS=1`, обновление README.

1. **Сокеты и DNS** — поверх ожидания ввода-вывода ([async.md](async.md), шаг 2): TCP-соединения
   на IOCP / epoll, свой DNS для Linux.
2. **Сервер** — разбор HTTP/1.1, keep-alive, `Request` / `Response` / `Headers` / `URL`, тела как
   потоки, `serve` с параметрами, 500 на исключение, `stop()`. Нужны классы
   ([objects.md](objects.md), шаг 3), байты ([bytes.md](bytes.md)), `for await`
   ([iterators.md](iterators.md), шаг 4), `Json`. Параллельные обработчики — с параллельным
   рантаймом ([async.md](async.md), шаг 4). Замеры против Go, Bun и node.
3. **`fetch`** — пул соединений, редиректы, gzip / deflate / brotli, `AbortSignal`, ошибки как
   в node.
4. **TLS** — OpenSSL в рантайме (в exe — только при https, [crypto.md](crypto.md)), корневые
   сертификаты, `NODE_EXTRA_CA_CERTS`, `serve` с `tls`; замер размера.
5. **Позже** — раздел 7.
