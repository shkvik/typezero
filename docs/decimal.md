# Точные десятичные числа: `decimal`

Решения, принятые 23 октября 2026 года. Шаги 1 и 2 плана сделаны 24 октября, `JSON.stringify` —
вместе с JSON; ошибки при выполнении — исключения.

## Коротко

- `decimal` — встроенный тип точных десятичных чисел любой длины. Умеет то же, что
  BigNumber.js, но с обычными операторами: `price * 3 + 0.01`. Для денег и всего, где
  `0.1 + 0.2` должно быть `0.3`.
- Литерал, который стоит там, где ждут `decimal`, читается из текста программы — точно.
- `NaN` и `Infinity` нет; общей настройки точности на всю программу нет.
- `bigint` нет: `10n` и `BigInt(x)` — ошибка с подсказкой `long` или `decimal`.

## 1. Как выглядит

```ts
const price: decimal = 19.99          // точно: литерал читается из текста программы
const total = price * 3 + 0.01        // decimal 59.98
if (total > 50) console.log(total)    // 59.98
```

То же на BigNumber.js — методами, потому что в JS нельзя задать операторы для объекта:

```ts
const price = new BigNumber("19.99")
const total = price.times(3).plus("0.01")
if (total.gt(50)) console.log(total.toString())
```

Операторы: `+ - * / % **`, унарный `-`, `< <= > >= == !=`, `+= -= *= /= %=`, `++ --`.
Методов `plus`, `minus`, `times`, `gt`, `eq` нет — вместо них операторы.

## 2. Откуда берётся и куда уходит

```ts
const big: decimal = 123456789012345678901234567890   // точно (number в node: 1.2345678901234568e+29)
const sum = price * qty                // qty: int — int и long становятся decimal сами, без потерь
const parsed = decimal("0.1")          // из строки; "abc" → SyntaxError, как BigInt("abc")
const bad = price * rate               // rate: double — ошибка → price * decimal(rate)
decimal(0.1)                           // 0.1 — кратчайшая запись, как в BigNumber.js
decimal(0.1 + 0.2)                     // 0.30000000000000004: ошибка уже в double
Number(total)                          // decimal → double
```

- **`double` → `decimal` только явно**: `double` уже неточный, и молча переносить его
  неточность в точный тип нельзя. Значение — кратчайшая запись числа, как у `String(x)` в JS.
- **Печать** — `console.log`, `String(x)`, шаблон — без степени: `decimal(2) ** 100` печатается
  как `1267650600228229401496703205376` (BigNumber.js: `1.267650600228229401496703205376e+30`).
  `console.log(price)` печатает `19.99`, а не внутренности объекта, как node с BigNumber.js.
- **`JSON.stringify`** пишет строку: `{"price":"19.99"}`, как BigNumber.js. Числом JS прочитал
  бы его с потерями.

## 3. Деление

```ts
decimal(1) / 3                         // 0.33333333333333333333
decimal(1).div(3, 2)                   // 0.33
decimal(1).div(3, 2, "half-even")
```

- `/` даёт 20 знаков после точки с округлением `"half-up"` — как BigNumber.js по умолчанию.
- Сколько знаков и как округлять — аргументами `div`. Общей настройки, как
  `BigNumber.config({ DECIMAL_PLACES: 2 })`, нет: настройка одной библиотеки ломала бы
  другую, а при параллельном async ([async.md](async.md)) это ещё и общие данные на запись.
- `%` — как в JS: знак результата — как у делимого.
- `**` — с целым показателем; отрицательный показатель — деление по тем же правилам. Дробный
  показатель — ошибка, для корня есть `sqrt()`.

## 4. Округление и вывод

```ts
const x: decimal = 2.5
x.round()                              // 3 — до целого, "half-up"
x.round(0, "half-even")                // 2
total.toFixed(2)                       // "59.98"
decimal("1234567.891").toFormat(2)     // "1,234,567.89"
decimal(255).toString(16)              // "ff"
```

- Режимы округления — строками: `"up"`, `"down"`, `"ceil"`, `"floor"`, `"half-up"`,
  `"half-down"`, `"half-even"`, `"half-ceil"`, `"half-floor"` (в BigNumber.js — числа
  `ROUND_UP = 0` … `ROUND_HALF_FLOOR = 8`).
- `round` заменяет `decimalPlaces(n, mode)` и `integerValue(mode)` из BigNumber.js.
- Остальное — как в BigNumber.js: `sqrt`, `abs`, `isInteger`, `decimalPlaces()`, `shiftedBy`,
  `toExponential`, `toPrecision`, `toFraction`, `decimal.max` / `min` / `sum`.

## 5. Сравнение — по значению

```ts
decimal("0.1") + decimal("0.2") == decimal("0.3")   // true (BigNumber.js: false — два объекта)
decimal("0.30") == decimal("0.3")                   // true
```

Нули в конце не хранятся, как в BigNumber.js: `decimal("0.30")` печатается как `0.3`. Для
показа — `toFixed(2)`.

## 6. Нет `NaN` и `Infinity`

| Что | tz | BigNumber.js |
|---|---|---|
| `x / n` при `n = 0` | `RangeError`, как `1n / 0n` в JS | `Infinity` или `NaN` |
| `x / 0` — константа | ошибка компиляции, как у `int` | `Infinity` |
| `decimal(NaN)`, `decimal(Infinity)` | `RangeError`, как `BigInt(NaN)` | `NaN`, `Infinity` |
| `decimal("abc")` | `SyntaxError`, как `BigInt("abc")` | исключение |
| `sqrt()` отрицательного | `RangeError` | `NaN` |

## 7. `bigint`

`10n`, `BigInt(5)`, `typeof x == "bigint"` — ошибка с подсказкой: целые до 9.2·10¹⁸ — `long`,
длиннее — `decimal` (`decimal(2) ** 100` считается точно).

## 8. Как устроен

Значение, как строка: неизменяемое, с подсчётом ссылок. Число до 18 цифр хранится как целое
`long` плюс «сколько знаков после точки», без кучи: сложение и умножение — обычные целые
операции, при переполнении — переход к длинной форме. Длиннее — цифры в куче. BigNumber.js на
каждую операцию создаёт объект и массив.

## План реализации

На каждом шаге: тесты `tests/run`, ошибки в `tests/errors`, прогон под `TZ_CHECK_LEAKS=1`,
обновление README. Сравнить с node напрямую нельзя: ожидаемый вывод сверяется с тем же расчётом
на BigNumber.js.

1. **Тип и арифметика** (сделано). `decimal` в типах; литералы из текста программы; операторы и
   сравнения; `int` / `long` → `decimal`; `decimal(x)` из строки и `double`; `Number(x)`;
   печать и `String(x)`; деление на 20 знаков; ошибки: `double` без `decimal()`, деление на
   константу 0, `10n` и `BigInt`. Рантайм: короткая форма и длинная. Исключения (`RangeError`,
   `SyntaxError`) — после [errors.md](errors.md), шаг 2.
2. **Методы** (сделано). `div` с режимами, `round`, `toFixed`, `toFormat`, `toString(base)`,
   `toExponential`, `toPrecision`, `sqrt`, `abs`, `isInteger`, `decimalPlaces()`, `shiftedBy`,
   `toFraction`, `decimal.max` / `min` / `sum`; `JSON.stringify` строкой.
