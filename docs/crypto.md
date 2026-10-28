# Криптография

Решения, принятые 24 октября 2026 года. Все шаги плана сделаны: 1–5 — 24–25 октября,
шаг 6 — 26–27 октября. Поправка 26 октября: ошибки — `Result`, а не исключения (раздел 5, [errors.md](errors.md)); свои хеши и
шифры на 32- и 64-битном счёте — `Bit.Wrap32`, `Bit.Wrap64` и объект `Bit`
([numbers.md](numbers.md)).

## Коротко

- `node:crypto` и Web Crypto (`crypto.subtle`, `crypto.getRandomValues`, `crypto.randomUUID`) —
  как в JS.
- Внутри — OpenSSL 3.5, та же библиотека и версия, что в node 24. Готовые библиотеки — в
  поставке tz, собирать ничего не нужно.
- OpenSSL собран как в node: без того, что node и сам выключил.
- Платишь за то, чем пользуешься: хеш с именем в коде — десятки КБ; TLS, ключи, имя алгоритма
  из переменной — OpenSSL целиком, 3,4–4,1 МБ; без криптографии — ничего.
- Быстрее Go: шифрование AES-GCM — в 3 раза, подпись RSA — в 4 раза.

## 1. Что есть

```ts
import { createHash, createHmac, randomUUID, randomBytes, scryptSync, timingSafeEqual } from "node:crypto"

createHash("sha256").update("abc").digest("hex")              // отпечаток данных
createHmac("sha256", secret).update(body).digest("base64")     // подпись вебхука, JWT
randomUUID()                                                   // id
randomBytes(32).toString("hex")                                // токен
scryptSync(password, salt, 64)                                 // хранение паролей; ещё argon2, pbkdf2
timingSafeEqual(a, b)                                          // сравнить секреты, не выдавая время
await crypto.subtle.sign("HMAC", key, data)                    // Web Crypto — так пишут hono и jose
```

И остальное из `node:crypto`: шифрование (`createCipheriv`: AES-GCM / CBC / CTR,
ChaCha20-Poly1305), ключи (`KeyObject`, PEM / DER / JWK), подписи (RSA, ECDSA, Ed25519), обмен
ключами (ECDH, X25519), `X509Certificate`, `hkdfSync`, `argon2Sync`. Web Crypto — `digest`,
`importKey` / `exportKey`, `sign` / `verify`, `encrypt` / `decrypt`, `deriveBits` / `deriveKey`,
`generateKey`.

## 2. OpenSSL

- **Версия** — 3.5, как в node 24; поддержка до апреля 2030 года.
- **Готовые библиотеки.** tz собирает OpenSSL сам — под Windows x64, Linux x64 и arm64
  (статически, с musl) и macOS — и кладёт в поставку, как LLVM. Perl и NASM программисту не
  нужны. Сборка — 11 секунд.
- **Что убрано** — то, что node 24 по умолчанию выключил сам (RC4, RC2, RC5, Blowfish, CAST,
  IDEA, SEED, MD4, MDC2, Whirlpool, одинарный DES — в node они дают `ERR_OSSL_EVP_UNSUPPORTED`),
  и то, чем API node не пользуется (SSLv3, DTLS, SRP, CMS, CMP, движки, устаревший API). Всё,
  что работает в node по умолчанию, работает и в tz: 3DES, DH, DSA, SM2 / SM3 / SM4, RIPEMD-160,
  Camellia, ARIA остаются.
- **Почему не mbedTLS:** он меньше, но в 7–17 раз медленнее. **Почему не убрать больше:**
  ещё 0,4 МБ не стоят расхождения с node.

## 3. Размер: два слоя

Статические сборки под Linux (musl, `-Os`, без отладочной информации); пустая программа на C —
14 КБ.

| | Размер exe |
|---|---|
| SHA-256 — функция OpenSSL напрямую | 55 КБ |
| SHA-256 через общий API OpenSSL | 3,4 МБ |
| https-клиент | 4,1 МБ |
| для сравнения, OpenSSL без урезания: https-клиент | 5,0 МБ |
| для сравнения, Go: https-клиент | 5,3 МБ |

OpenSSL 3 ищет алгоритм по имени (`"sha256"`) в общей таблице во время работы. Таблица ссылается
на все алгоритмы, поэтому даже один хеш через общий API тянет их все. Если вызвать функцию
алгоритма напрямую — таблицы нет, а код и ассемблер те же. Поэтому компилятор выбирает слой сам:

- **Простое — напрямую, десятки КБ на алгоритм:** хеши, HMAC, PBKDF2, scrypt, Argon2, HKDF,
  `createCipheriv` для AES-GCM / CBC / CTR и ChaCha20-Poly1305 — когда имя алгоритма написано в
  коде строкой; случайные числа (`randomBytes`, `randomUUID`, `getRandomValues`) — от системы,
  `timingSafeEqual`.
- **Сложное — OpenSSL целиком:** TLS (https в `fetch` и `serve`, [net.md](net.md)), ключи,
  подписи, сертификаты, обмен ключами, имя алгоритма из переменной.

## 4. Скорость

Один Linux-контейнер, одно ядро, AMD Ryzen 9 9950X3D; OpenSSL 3.5.9 (`openssl speed`), Go 1.23.

| | OpenSSL | Go | Быстрее |
|---|---|---|---|
| SHA-256 | 2 507 МБ/с | 2 571 МБ/с | одинаково |
| AES-256-GCM | 21 417 МБ/с | 7 015 МБ/с | OpenSSL в 3 раза |
| ChaCha20-Poly1305 | 6 401 МБ/с | 3 395 МБ/с | OpenSSL в 1,9 раза |
| подпись ECDSA P-256 | 82 866 в с | 71 321 в с | OpenSSL в 1,2 раза |
| обмен ключами X25519 | 52 600 в с | 33 888 в с | OpenSSL в 1,6 раза |
| подпись RSA-2048 | 5 805 в с | 1 429 в с | OpenSSL в 4 раза |
| подпись Ed25519 | 48 628 в с | 70 164 в с | Go в 1,4 раза |

Для https главное — шифрование трафика (AES-GCM) и рукопожатие (подпись и обмен ключами): там
OpenSSL быстрее Go. В node те же функции идут через JS: AES-GCM там — 3 752 МБ/с, время уходит
на новые буферы и переходы JS → C. tz вызывает OpenSSL прямо из машинного кода.

## 5. Как в node, и где строже

- Имена алгоритмов, `getHashes()`, `getCiphers()`, коды ошибок — как в node: библиотека та же.
- Имя алгоритма в коде, которого нет (`createHash("sha265")`, `createHash("md4")`), — ошибка
  компиляции. В node — ошибка при работе. Имя из переменной — `createHash(name)` отдаёт
  `Result`: `Err`, если такого алгоритма нет.
- **Ошибки — `Result`, а не исключения** ([errors.md](errors.md)): неверный ключ, плохой PEM,
  не совпал тег AES-GCM при расшифровке, неверная подпись в разборе — `Err` с меткой `code`, как
  коды ошибок node. `crypto.subtle` отдаёт `Promise<Result<...>>` вместо отказа `Promise`.
  Проверка аргументов, которая выражается типом (имена алгоритмов, длина ключа AES
  `128 | 192 | 256`), — при сборке ([errors.md](errors.md), раздел 7).
- Функции с колбэком (`scrypt(…, cb)`, `pbkdf2(…, cb)`, `randomBytes(n, cb)`) — ошибка с
  подсказкой: `…Sync` или `crypto.subtle`, как с `node:fs` ([io.md](io.md)).
- md5 и sha1 — есть, как в node: нужны для старых форматов и проверки файлов.

## 6. Обновления и лицензия

- Нашли дыру в OpenSSL — выходит новая версия tz, программы нужно пересобрать (у Go так же: всё
  вшито в exe).
- Лицензия OpenSSL — Apache 2.0: вшивать в свой exe можно, но вместе с exe нужно отдавать текст
  лицензии. `tz licenses app.exe` печатает лицензии всего чужого кода, что попал в exe.

## План реализации

На каждом шаге: тесты `tests/run` с `// same-as-node`, ошибки в `tests/errors`, прогон под
`TZ_CHECK_LEAKS=1`, обновление README.

1. **OpenSSL в поставке** (сделано) — сборка под каждую цель с настройками из раздела 2, подключение к
   exe только при использовании, `tz licenses`.
2. **Простой слой** (сделано) — хеши, HMAC, `hash()`, `randomBytes` / `randomUUID` / `randomInt` /
   `getRandomValues`, `timingSafeEqual`, `pbkdf2Sync` / `scryptSync` / `argon2Sync` /
   `hkdfSync`, `createCipheriv` / `createDecipheriv` для AES и ChaCha20 с именем в коде; ошибки
   компиляции на неизвестные имена и колбэки. Нужен `Buffer` ([bytes.md](bytes.md), шаг 2).
3. **Полный слой** (сделано) — `KeyObject`, ключи PEM / DER / JWK, подписи, `generateKeyPairSync`, ECDH,
   `X509Certificate`, имена из переменных. Он же — основа TLS ([net.md](net.md), шаг 4).
4. **Web Crypto** (`crypto.subtle`) (сделано) — после async ([async.md](async.md), шаг 1).
5. **Замеры** (сделано) в `bench` против Go и node.
6. **Без исключений** (сделано 26–27 октября; [errors.md](errors.md), шаг 2): `Result` у `node:crypto` и
   `crypto.subtle` (134 `throw` в `library/crypto/webcrypto.tz`), литеральные типы для имён
   алгоритмов и длин ключей. `node:crypto` сделан 26 октября: `Result<T, CryptoError>` с кодами
   node у того, что зависит от данных и у имён из переменных; `X509Certificate.parse`; хеш не
   закрывается после `digest()`; ошибки порядка у шифра отдаёт `final()`. Числа вне диапазона —
   с шагом 4 errors.md (доказательства). `crypto.subtle` отдаёт `Promise<Result<T, SubtleError>>`
   (`code` — имя `DOMException` node); внутри `webcrypto.tz` исключений нет с шагом 5 (27 октября).
