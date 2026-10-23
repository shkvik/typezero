# Байтовые массивы

Решения, принятые 24 октября 2026 года. Пока ничего из этого не реализовано: порядок работ — в
конце, в разделе «План реализации».

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
| `Uint8Array`, `Uint8ClampedArray`, `Buffer` | 0…255 | `int` |
| `Int8Array` | −128…127 | `int` |
| `Uint16Array` / `Int16Array` | 0…65 535 / −32 768…32 767 | `int` |
| `Int32Array` | как `int` | `int` |
| `Uint32Array` | 0…4 294 967 295 — в `int` не влезает | `long` |
| `Float16Array`, `Float32Array`, `Float64Array` | дробные | `double` |
| `BigInt64Array`, `BigUint64Array` | — | ошибка: `bigint` нет ([decimal.md](decimal.md)) |

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
u[1] = x                      // ошибка, если не доказано, что x от 0 до 255
u[1] = x & 0xff               // ок: заворот явно
if (x >= 0 && x <= 255) u[1] = x       // ок: проверено
u[2] = other[i]               // ок: other — тоже Uint8Array
u[3] = 1.7                    // ошибка: double в int без явного перевода (как везде в tz)
```

- Проверяется любая запись: по индексу, `fill`, `set`, `new Uint8Array([…])`, `Uint8Array.from`,
  `DataView.set…`, `Buffer.write…`. В node `Buffer.writeUInt8(300)` бросает `RangeError` во
  время работы, а `DataView.setUint8(0, 300)` молча записывает 44; в tz оба — ошибка при
  компиляции.
- Доказательство — то же, что для индексов и переполнения ([small.md](small.md), раздел 6):
  литералы, маски (`& 0xff`, `>>> 24`), проверки выше по коду, значения из массива того же
  или более узкого типа, `Math.min` / `Math.max` с двух сторон.
- `Uint8ClampedArray` не проверяется: он для того и сделан, чтобы прижимать к 0…255.
- Дробные массивы не проверяются: в `Float32Array` теряется точность, а не значение.

## 4. Индексы

Как у обычных массивов: если доказано, что индекс в пределах, `u[i]` — `int`, иначе
`int | undefined`; запись — только с доказательством, иначе ошибка компиляции (в JS запись за
пределами молча ничего не делает). Длина типизированного массива не меняется, поэтому
доказательство обычно — цикл по `length`.

## 5. Кодировки

- `utf8`, `latin1` (`binary`), `ascii`, `utf16le` (`ucs2`), `hex`, `base64`, `base64url` — всегда.
- `TextDecoder("windows-1251")` и другие старые кодировки — таблица встраивается в exe, только
  если кодировка названа в коде строкой (как языки в [date.md](date.md)).
- Битый UTF-8 — «�», как в node; `new TextDecoder("utf-8", { fatal: true })` — `TypeError`.

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

1. **Типизированные массивы и `DataView`** — типы элементов, индексы с доказательствами,
   проверка значений при записи, `subarray` / `slice` / `set` / `fill` / `from` и остальные
   методы массивов, печать как в node. `RangeError` (`new Uint8Array(-1)`) — после исключений
   ([errors.md](errors.md), шаг 2).
2. **`TextEncoder` / `TextDecoder`, `Buffer`** с кодировками из раздела 5, ошибки на
   `Buffer.slice` и `new Buffer`; `readFileSync` без кодировки и `writeFileSync` с байтами;
   `embedBytes` ([modules.md](modules.md), шаг 3).
3. **Старые кодировки** — таблицы только для названных в коде.
4. **Замеры** в `bench`: циклы по байтам рядом с Go.
