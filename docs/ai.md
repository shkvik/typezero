# tz для ИИ: чем отличается от TypeScript

Памятка для ИИ-агентов, которые пишут код на tz. Синтаксис — TypeScript, но правила строже: то, что
в TS проходит, здесь часто ошибка сборки. Компилятор всегда объясняет ошибку и подсказывает правку —
читай сообщение и делай, что в нём написано. Подробности — README («Чем отличается от TypeScript»)
и docs/*.md. Обновляется вместе с языком (29 октября 2026).

## Сборка и проверка

- Файлы программ — `.tz`. `tz build -o app.exe main.tz`, `tz run main.tz`.
- Компилятор выводит типы сам: аннотации пиши, только если он просит.
- О коде спрашивай компилятор, а не поиск по тексту: `tz map` (обзор с типами),
  `tz context <имя | Класс.член | файл.tz:строка>` (исходник, что использует, кто использует,
  docs), `tz query definition|references|callers|type ...`, `tz impact <имя>` (что затронет
  правка). Через MCP (`tz mcp`) — те же инструменты и `check_edit`: проверка правки до записи.

## Нет совсем

`any`, `as`, `satisfies`, `x!`, `var`, `===` / `!==`, `throw`, `try` / `catch` / `finally`,
`Error` и его виды, `null` (кроме значений `Json`), `NaN`, `Infinity`, `eval`, `new Function`,
`Proxy`, геттеры и сеттеры (`get x()`), `#x` (пиши `private x`), `this` в обычной `function`,
`arguments`, `export default` и импорт по умолчанию (кроме JSON), `import * as`, `AbortSignal`.

## Модули

```ts
import { readFileSync } from "node:fs"            // только по именам
import { Server } from "tz:http"
import config from "./config.json" with { type: "json" }   // JSON — целиком; значения вшиты при сборке
```

## Числа

- `int` — 64 бита; `int8`…`int32`, `uint8`…`uint64`; `double` (`number` — то же); `decimal` —
  точные десятичные (`19.99`, `*`, `+` работают).
- `7 / 2` у `int` — `3`. Тип меняется только явно: `n.toDouble() * 1.5`, `Math.trunc(d).toInt()`.
- Переполнение и деление на 0 должны быть доказуемо невозможны: проверь (`if (b != 0)`,
  `if (x < 1000)`), иначе ошибка сборки с подсказкой.
- `parseInt(s)`, `Number(s)` — `Result<…, NumberError>`.
- Число + строка — ошибка: `` `итого: ${n}` `` или `String(n)`.

## Сравнения и условия

- Только `==` и `!=`, без приведения: типы с двух сторон совпадают.
- В условии — только `boolean` или проверка на `undefined`: не `if (count)`, а `if (count != 0)`;
  не `if (items)`, а `if (items.length > 0)`; не `x || 5`, а `x ?? 5`.
- `let` без значения — с типом: `let total: int`. `let x = undefined` — ошибка.
- `const d = "up"` подходит туда, где ждут `"up" | "down"`; `let` — `string`.

## Ошибки — значения `Result`

```ts
function parseAge(s: string): Result<int, { kind: "INVALID"; code: "ERR_AGE"; message: string }> {
  const n = parseInt(s)
  if (n.isErr()) {
    return err({ kind: "INVALID", code: "ERR_AGE", message: `"${s}" is not a number` })
  }

  return ok(n.value)
}

const age = parseAge(input)
if (age.isErr()) {
  console.log(age.error.message)   // после проверки — error и value
  process.exit(1)
}
console.log(age.value + 1)
```

- `ok`, `err`, `isOk()`, `isErr()`, `.value`, `.error`, `match(onOk, onErr)`, `unwrapOr(x)`, `map`,
  `mapErr`, `Result.combine`.
- Забытый `Result` — ошибка: и вызов, чей результат не взяли, и переменная, которую не прочитали.
  `if (r.isOk())` без `else` разрешён.
- Ошибка — объект с `kind` (что делать), закрытым списком `code` и `message`. `switch (e.kind)`
  обязан разобрать все варианты. Поле есть только у одного варианта — сначала сузь:
  `if (e.code == "ERR_STREAM_FAILED") e.details`.
- Библиотека отдаёт `Result` там, где node бросает: `fetch`, тела ответов, `JSON.parse`, `node:fs`,
  `parseInt`, `URL.parse`, `crypto.subtle`, `execSync`…

## Async

- Как в node; `Promise` не отклоняется — операция, которая может не получиться, —
  `Promise<Result<T, E>>`: сначала `await`, потом методы.
- `void f()` над `Promise<Result>` — ошибка; функцию с `Result` нельзя отдать туда, где ждут
  `() => void` (слушатель, таймер).

## Объекты, классы, функции

- Типы объектов точные: лишнее поле — ошибка. Необязательное поле — `x?: T`.
- `readonly` присваивается только в конструкторе. Значения, которые меняет сам объект, — методы:
  `stream.locked()`, `response.bodyUsed()`.
- Перегрузка — у методов и конструкторов, отдельным телом на вариант; обычные функции — разные имена.
- Параметры объявленных функций аннотируют; тип результата выводится.
- Обобщённые функции и классы — копия на тип; ограничение `T extends { message: string }` —
  «есть хотя бы эти поля».

## Массивы и строки

- `[]` без типа — ошибка: `const xs: int[] = []`.
- `a[i]` — `T | undefined`, если компилятор не доказал, что `i` в пределах: проверь
  `if (i < a.length)` или используй `for...of`.
- `sort()` у чисел — только с функцией: `sort((x, y) => x - y)`. `for...in` по массиву — ошибка.

## JSON

- `JSON.parse(text)` — `Result<Json, JsonError>`. У `Json` ключи читаются сразу, остальное — после
  `typeof` / `Array.isArray`; свой тип собирают литералом из проверенных полей.

## Сеть

```ts
const sent = await fetch(url, { method: "POST", body: "hi" })   // Result<Response, FetchError>
if (sent.isErr()) return
const text = await sent.value.text()                             // Result<string, TextError>

const server = new Server({ port: 3000, hostname: "127.0.0.1" }, async req => new Response("ok"))
const listening = server.listen()                                // Result<Listening, ListenError>
```

- Поток со своей ошибкой — `interface MyError extends StreamError { … }`, `new
  ReadableStream<Uint8Array, MyError>({ … })`; без второго аргумента — любая `StreamError`
  (только `message`). Телом поток отдают только методом: `Response.stream(stream)`,
  `Request.stream(url, stream, options)` или `fetch(url, { body: stream, duplex: "half" })`;
  `new Response(stream)` — ошибка.

## Стиль кода библиотеки (`library/`) — docs/style.md

- Модификаторы `public` / `protected` / `private` у каждого члена; поля — только с типом, все
  значения ставит конструктор.
- Без вложенных вызовов: промежуточное — в `const` с именем; `return` — после пустой строки.
- Ошибка строится на месте: `return err({ kind, code, message })`, `if` — в фигурных скобках.
- Комментариев в коде нет (ни в C++, ни в C, ни в tz); тесты держат `// expect-error:`.
