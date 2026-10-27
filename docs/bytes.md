# Байтовые массивы

Решения, принятые 24 октября 2026 года. Все шаги плана сделаны 24–25 октября. Поправки
26 октября — точные типы элементов (`uint8`, `int32`…) и явные приведения
([numbers.md](numbers.md)), ошибки — `Result` вместо исключений (разделы 1, 3, 5).

## Коротко

- `ArrayBuffer`, `Uint8Array` и остальные типизированные массивы, `DataView`, `TextEncoder` /
  `TextDecoder` — как в JS. `Buffer` — как в node.
- `Buffer.slice` — ошибка: в node он, в отличие от `Uint8Array.slice`, не копирует.
- Число, которое не влезает в элемент (`u8[0] = 300`), — ошибка компиляции, если не доказано,
  что влезает. В JS оно молча обрезается.
- Индексы — как у обычных массивов: без доказательства чтение даёт `undefined`, запись —
  ошибка компиляции.

## 1. Что есть

```ts
const bytes = new Uint8Array([72, 105])     // Uint8Array(2) [ 72, 105 ]
new TextDecoder().decode(bytes)             // "Hi"
new TextEncoder().encode("Привет")          // 12 байт: кириллица — по 2 байта
const buf = Buffer.from("SGk=", "base64")   // <Buffer 48 69>
buf.toString("hex")                         // "4869"
```

| Тип | Элемент | Тип в tz |
|---|---|---|
| `Uint8Array`, `Uint8ClampedArray`, `Buffer` | 0…255 | `uint8` |
| `Int8Array` | −128…127 | `int8` |
| `Uint16Array` / `Int16Array` | 0…65 535 / −32 768…32 767 | `uint16` / `int16` |
| `Int32Array` | −2 147 483 648…2 147 483 647 | `int32` |
| `Uint32Array` | 0…4 294 967 295 | `uint32` |
| `Float16Array`, `Float32Array`, `Float64Array` | дробные | `double` |
| `BigInt64Array`, `BigUint64Array` | — | ошибка: `bigint` нет ([decimal.md](decimal.md)) |

Тип элемента — точный тип из [numbers.md](numbers.md), поэтому счёт с элементами — по правилам
явных приведений: `buf[i] + 1` — `uint8`, сумма байтов в `int` — `sum += buf[i].toInt()`.

- `ArrayBuffer` — сама память; типизированные массивы — «окна» в неё с разным размером
  элемента. `subarray` и окна разных типов над одним `ArrayBuffer` смотрят в одну память,
  `slice` копирует — как в JS.
- `DataView` — все `get…` / `set…` с порядком байтов; `getBigInt64` / `setBigInt64` — ошибка.
- `console.log` печатает как node: `Uint8Array(2) [ 72, 105 ]`, `<Buffer 48 69>`.

## 2. `Buffer`

`Buffer` — как в node: `Uint8Array` с методами node (`from`, `alloc`, `concat`, `toString`,
`equals`, `compare`, `read…` / `write…` и др.). Он нужен, потому что API node отдаёт именно его:
`readFileSync` без кодировки, `crypto`, сеть ([io.md](io.md)).

Одна правка. В node одинаковые на вид записи делают разное:

```ts
const b = Buffer.from([1, 2, 3])
b.slice(0, 2)[0] = 9          // node: b стал <Buffer 09 02 03> — slice смотрит в ту же память
new Uint8Array([1, 2, 3]).slice(0, 2)   // а здесь — копия
```

В tz `Buffer.slice` — ошибка с подсказкой: `subarray()` (та же память) или `Buffer.from(…)`
(копия). В node `Buffer.slice` тоже объявлен устаревшим. `new Buffer(…)` — ошибка с подсказкой
`Buffer.from` / `Buffer.alloc`, как и в node, где он устарел.

## 3. Значение должно влезать

```ts
const u = new Uint8Array(4)
u[0] = 300                    // ошибка: 300 не влезает в байт (в JS молча станет 44)
u[1] = x                      // x: int — ошибка: int в uint8 только явно
u[1] = (x & 0xff).toUint8()   // ок: маска доказывает 0…255
if (x >= 0 && x <= 255) u[1] = x.toUint8()   // ок: проверено
u[2] = other[i]               // ок: other — тоже Uint8Array
u[3] = 1.7                    // ошибка: double в uint8 (как везде в tz)
```

- Проверяется любая запись: по индексу, `fill`, `set`, `new Uint8Array([…])`, `Uint8Array.from`,
  `DataView.set…`, `Buffer.write…`. В node `Buffer.writeUInt8(300)` бросает `RangeError` во
  время работы, а `DataView.setUint8(0, 300)` молча записывает 44; в tz оба — ошибка при
  компиляции.
- Доказательство — то же, что для индексов и переполнения ([numbers.md](numbers.md), раздел 2):
  литералы, маски (`& 0xff`, `>> 24` у беззнакового), проверки выше по коду, значения из
  массива того же типа, `Math.min` / `Math.max` с двух сторон. Сужение пишется явно:
  `.toUint8()`.
- `Uint8ClampedArray` не проверяется: он для того и сделан, чтобы прижимать к 0…255.
- Дробные массивы не проверяются: в `Float32Array` теряется точность, а не значение.

## 4. Индексы

Как у обычных массивов: если доказано, что индекс в пределах, `u[i]` — тип элемента
(`uint8` у `Uint8Array`), иначе `uint8 | undefined`; индекс — любой целый тип; запись — только с доказательством, иначе ошибка компиляции (в JS запись за
пределами молча ничего не делает). Длина типизированного массива не меняется, поэтому
доказательство обычно — цикл по `length`.

## 5. Кодировки

- `utf8`, `latin1` (`binary`), `ascii`, `utf16le` (`ucs2`), `hex`, `base64`, `base64url` — всегда.
- `TextDecoder("windows-1251")` и другие старые кодировки — таблица встраивается в exe, только
  если кодировка названа в коде строкой (как языки в [date.md](date.md)).
- Битый UTF-8 — «�», как в node; с `new TextDecoder("utf-8", { fatal: true })` `decode`
  отдаёт `Result`: `Err` на битом UTF-8 (в node — `TypeError`).
- Разбор чисел из байтов: `readDoubleLE/BE`, `readFloatLE/BE` и `DataView.getFloat…` отдают
  `Result` — `Err`, если в байтах лежит `NaN` или `Infinity` ([numbers.md](numbers.md),
  раздел 9).

## 6. Файлы и `embedBytes`

- `readFileSync(path)` без кодировки возвращает `Buffer`, `writeFileSync` принимает
  `Uint8Array` — как в node ([io.md](io.md)).
- `embedBytes("./logo.png")` вшивает двоичный файл в exe ([modules.md](modules.md)). Если
  программа в эти байты не пишет — они читаются прямо из exe, без копии в памяти.

## 7. Память и async

- Память сплошная: циклы по байтам компилируются в векторные инструкции.
- Окна в одну память — одни и те же данные: параллельные async ([async.md](async.md))
  обращаются к ним по очереди.
- Позже: `ArrayBuffer` с изменяемым размером и `transfer`, `SharedArrayBuffer` и `Atomics`
  (вместе с общей памятью из async.md), `Blob` и `File`.

## План реализации

На каждом шаге: тесты `tests/run` с `// same-as-node`, ошибки в `tests/errors`, прогон под
`TZ_CHECK_LEAKS=1`, обновление README.

1. **Типизированные массивы и `DataView`** (сделано) — типы элементов, индексы с доказательствами,
   проверка значений при записи, `subarray` / `slice` / `set` / `fill` / `from` и остальные
   методы массивов, печать как в node. `RangeError` (`new Uint8Array(-1)`) — после исключений
   ([errors.md](errors.md), шаг 2).
2. **`TextEncoder` / `TextDecoder`, `Buffer`** (сделано) с кодировками из раздела 5, ошибки на
   `Buffer.slice` и `new Buffer`; `readFileSync` без кодировки и `writeFileSync` с байтами;
   `embedBytes` ([modules.md](modules.md), шаг 3).
3. **Старые кодировки** (сделано) — таблицы только для названных в коде.
4. **Замеры** (сделано) в `bench`: циклы по байтам рядом с Go.
5. **Поправки 26 октября** (вместе с [numbers.md](numbers.md), шаг 1, и
   [errors.md](errors.md), шаг 2): типы элементов `uint8` … `uint32`, явные приведения при
   записи; `new Uint8Array(n)` — доказательство `n ≥ 0` вместо `RangeError` (сделано 27
   октября вместе со смещениями чтения и записи, `set`, `with`, видов и методов `Buffer`,
   [errors.md](errors.md), шаг 4); `Result` у
   `TextDecoder` с `fatal` и у чтения дробных из байтов. `Result` у `TextDecoder` с `fatal` сделан 27 октября:
   его тип — `FatalTextDecoder`, `decode` — `Result<string, DecodeError>`.
