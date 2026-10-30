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
- **Сложное — OpenSSL целиком:** TLS (https в `fetch` и `Server`, [net.md](net.md)), ключи,
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
  компиляции. В node — ошибка при работе. Имя из переменной — `Err` с причиной
  `UNKNOWN_ALGORITHM`.
- **Ошибки — `Result`, а не исключения** ([errors.md](errors.md)); `crypto.subtle` отдаёт
  `Promise<Result<...>>` вместо отказа `Promise`. Как устроены — ниже, «Ошибки».
- Что видно по коду, — ошибка сборки, а не `Err` (решено 30 октября, К4): длина ключа и IV
  у `createCipheriv` с именем в коде (`aes-256-gcm needs a 32-byte key, but this one has 5
  bytes`), `iterations` и `keylen` у `pbkdf2Sync` / `hkdfSync` меньше 1, `N` у `scryptSync` —
  не степень двойки, `modulusLength` RSA меньше 512, длина AES-ключа у `generateKeySync`.
  `Result` при этом остаётся: OpenSSL может отказать и по другим причинам.
- Функции с колбэком (`scrypt(…, cb)`, `pbkdf2(…, cb)`, `randomBytes(n, cb)`) — ошибка с
  подсказкой: `…Sync` или `crypto.subtle`, как с `node:fs` ([io.md](io.md)).
- md5 и sha1 — есть, как в node: нужны для старых форматов и проверки файлов.

### Ошибки

Решено 30 октября (`tmp/design/crypto-errors.tz`, К1–К8), по правилам `node:fs` и
`node:child_process`: у каждой функции свой тип ошибки, общего `CryptoError` нет.

```ts
import { createDecipheriv, DecipherFinalError } from "node:crypto"

const decipher = createDecipheriv("aes-256-gcm", key, iv)
if (decipher.isErr()) return
decipher.value.setAuthTag(tag)
const head = decipher.value.update(sealed)
const tail = decipher.value.final()
if (tail.isErr()) {
  switch (tail.error.kind) {
    case "DECIPHER_FINAL_ERROR_AUTHENTICATION_FAILED": console.log("the data was changed"); break
    case "DECIPHER_FINAL_ERROR_WRONG_ORDER": console.log(tail.error.message); break
    default: console.log(tail.error.code, tail.error.message)
  }
}
```

- **Что отдаёт `Result`** — всё, где OpenSSL может отказать: `createHash`, `hash`,
  `hash.copy({ outputLength })`, `createHmac`, `pbkdf2Sync`, `hkdfSync`, `scryptSync`,
  `argon2Sync`, `createCipheriv`, `createDecipheriv`, `cipher.final()`, `decipher.final()`,
  `cipher.getAuthTag()`, `createPrivateKey`, `createPublicKey`, `generateKeyPairSync`,
  `key.export()`, `sign`, `verify`, `createSign`, `createVerify`, `signer.sign()`,
  `verifier.verify()`, `createECDH`, `ecdh.getPublicKey()`, `ecdh.getPrivateKey()`,
  `ecdh.setPrivateKey()`, `ecdh.computeSecret()`, `diffieHellman`, `X509Certificate.parse`,
  `cert.checkPrivateKey()`, `cert.verify()`. С именем алгоритма в коде — тоже (К4а): имя
  проверено при сборке, а `Result` — на всё остальное. Без `Result`: `randomBytes`, `randomInt`,
  `randomUUID`, `getRandomValues`, `timingSafeEqual`, `createSecretKey`, `generateKeySync`,
  `update`, `setAAD`, `setAuthTag` — что пошло не так в `update`, `setAAD` и `setAuthTag`,
  отдаёт следующий `final()`.
- **Типы** — по функции: `CreateHashError`, `HashError`, `HashCopyError`, `CreateHmacError`,
  `Pbkdf2Error`, `HkdfError`, `ScryptError`, `Argon2Error`, `CreateCipherError`,
  `CreateDecipherError`, `CipherFinalError`, `DecipherFinalError`, `GetAuthTagError`,
  `CreatePrivateKeyError`, `CreatePublicKeyError`, `GenerateKeyPairError`, `KeyExportError`,
  `SignError`, `VerifyError`, `CreateSignError`, `CreateVerifyError`, `SignerSignError`,
  `VerifierVerifyError`, `CreateEcdhError`, `EcdhGetPublicKeyError`, `EcdhGetPrivateKeyError`,
  `EcdhSetPrivateKeyError`, `EcdhComputeSecretError`, `DiffieHellmanError`,
  `ParseCertificateError`, `CertificateCheckPrivateKeyError`, `CertificateVerifyError`;
  импорт — `import { CreateCipherError } from "node:crypto"`.
- **`kind`** — имя типа и причина (`CREATE_CIPHER_ERROR_WRONG_KEY_LENGTH`). Причины:
  ошибки программы — `UNKNOWN_ALGORITHM`, `UNSUPPORTED_ALGORITHM` (md4, des: в tz их нет),
  `WRONG_KEY_LENGTH`, `WRONG_IV_LENGTH`, `WRONG_TAG_LENGTH`, `WRONG_KEY_TYPE`,
  `INCOMPATIBLE_KEYS`, `WRONG_PARAMETERS`, `INCOMPATIBLE_OPTIONS`, `WRONG_ORDER` (второй
  `final()`, `getAuthTag()` до `final()`, ECDH без ключей); данные — `AUTHENTICATION_FAILED`
  (GCM / CCM / ChaCha: тег не сошёлся), `BAD_DECRYPT` (CBC: ключ не тот или данные испорчены),
  `WRONG_DATA_LENGTH`, `NOT_A_KEY`, `PASSPHRASE_MISSING`, `WRONG_PASSPHRASE`, `INVALID_JWK`,
  `NOT_A_CERTIFICATE`, `INVALID_PUBLIC_KEY`, `CANNOT_EXPORT`; сломалось ниже — `SYSTEM`
  (`ERR_CRYPTO_OPERATION_FAILED` или `UNKNOWN`: код, которого нет в списке).
- **`code`** — код node; у ошибок OpenSSL — его причина (`ERR_OSSL_PEM_NO_START_LINE`,
  `ERR_OSSL_ASN1_TOO_LONG`, `ERR_OSSL_BAD_DECRYPT`…, К3). Ключ, который OpenSSL не прочитал,
  tz разбирает сам (рамка PEM, метка, строение DER), чтобы сказать почему: «нет
  `-----BEGIN`», «битый base64», «это открытый ключ», «зашифрован, а пароля нет»;
  `ERR_OSSL_UNSUPPORTED` — только когда строение верное, а такой ключ OpenSSL не знает.
- **Поля — у варианта только свои:** `algorithm` (`"aes-256-gcm"`), `length` и `expected`
  (ключ 5 из 32 байт), `got` и `expected` (`"public"` вместо `"private"`), `parameter` и
  `value` (`"iterations"`, `0`), `field` (поле JWK), `format` (`"pem"` / `"der"` / `"jwk"`),
  `openssl` (что сказал OpenSSL, как есть: `PEM routines::no start line`).
- **Тексты** — что не вышло и почему: `aes-256-gcm needs a 32-byte key, but the key has 5
  bytes`, `could not decrypt with aes-256-gcm: the data was changed or the key is wrong (the
  authentication tag does not match)`, `final() was already called on this decipher; make a new
  one with createDecipheriv`, `could not read the private key: the text is not PEM (no
  "-----BEGIN" line)`, `scrypt: N must be a power of two greater than 1, not 3`.
- **`crypto.subtle`** (К8) — по тому же правилу: свой тип у каждого метода
  (`SubtleDigestError`, `SubtleGenerateKeyError`, `SubtleImportKeyError`, `SubtleExportKeyError`,
  `SubtleSignError`, `SubtleVerifyError`, `SubtleEncryptError`, `SubtleDecryptError`,
  `SubtleDeriveBitsError`, `SubtleDeriveKeyError`, `SubtleWrapKeyError`,
  `SubtleUnwrapKeyError`; без импорта, как сам `crypto.subtle`), те же типы у методов
  `CryptoKey` (`key.sign` — `SubtleSignError`). Причины: `UNSUPPORTED_ALGORITHM`,
  `WRONG_PARAMETERS`, `WRONG_KEY`, `WRONG_USAGE`, `NOT_A_KEY`, `DECRYPTION_FAILED` (`decrypt`,
  `unwrapKey`), `SYSTEM`; у метода — только те, что у него бывают; что пришло сверх списка, —
  `SYSTEM`. `code` и тексты — как в node (имя `DOMException` в нашем виде), их сверяет тест.
- Отличия от node: свои тексты; `X509Certificate.parse` байтов DER говорит про DER (раньше tz
  отдавал ошибку PEM); зашифрованный ключ без пароля — `ERR_MISSING_PASSPHRASE`, как в node
  (было `ERR_OSSL_CRYPTO_PASSED_INVALID_ARGUMENT`); `pbkdf2Sync` и `hkdfSync` с `keylen: 0` —
  `WRONG_PARAMETERS`; `decipher.final()` GCM без `setAuthTag()` — `WRONG_ORDER` (в node
  `ERR_CRYPTO_AUTH_FAILED`).

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
   (с 30 октября — свой тип у каждого метода, раздел 5)
   (`code` — имя `DOMException` node; с 29 октября — наш вид: `kind: "SUBTLE"`,
   `ERR_INVALID_DATA`, `ERR_NOT_SUPPORTED`, `ERR_OPERATION_FAILED`… — README, «Web Crypto»); внутри `webcrypto.tz` исключений нет с шагом 5 (27 октября).
7. **Ошибки по функциям** (сделано 30 октября, 4.6 шаг 3, `tmp/design/crypto-errors.tz` К1–К8):
   32 типа `node:crypto` и 12 типов `crypto.subtle` вместо `CryptoError` и `SubtleError`, причины
   в `kind`, поля по месту, коды OpenSSL, разбор нечитаемых ключей самим tz, тексты К6, ошибки
   сборки К4, `Result` и у имён в коде (К4а), `WRONG_ORDER` (К5). Раздел 5, «Ошибки».
