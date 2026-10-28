# Итераторы и генераторы

Решения, принятые 23 октября 2026 года, и поправки 26 октября: у генераторов нет `throw()` и
`finally`, `safeTry` убран ([errors.md](errors.md)). Все шаги плана сделаны: 1–4 — 24 октября (и
генераторы-методы классов), поправки (шаг 5) — 26–27 октября.

## Коротко

- Генераторы (`function*`, `yield`, `yield*`) — как в JS. Генератор, который тут же перебирают в
  `for...of`, компилируется в обычный цикл: без объекта в куче и без вызовов `next()`.
- Свои коллекции перебираются через `[Symbol.iterator]`, как в JS: `for...of`, `[...x]`,
  `new Set(x)` и т.д. вызывают этот метод напрямую.
- Ленивые цепочки из JS 2025 — `values().filter(...).map(...).take(3).toArray()` — есть сразу и
  компилируются в один цикл.
- Своих `Symbol("x")` нет: `Symbol.iterator` и `Symbol.asyncIterator` бывают только именами
  методов.

## 1. Генераторы

```ts
function* naturals() {
  let n = 0
  while (true) yield n++
}

function* evens(limit: int) {
  for (let i = 0; i < limit; i += 2) yield i
}

let sum = 0
for (const e of evens(1000)) sum += e    // компилируется в обычный цикл
```

- `function*`, `yield`, `yield*`, методы-генераторы `*name()`; у генератора `next()` и
  `return()` — как в JS. `throw()` нет: исключений нет ([errors.md](errors.md)).
- **Скорость.** Генератор, который тут же перебирают в `for...of`, встраивается в цикл: ни
  объекта в куче, ни вызовов `next()` — как если бы цикл написали руками. В остальных случаях
  генератор — объект с состоянием; состояние строится так же, как у async-функций (средствами
  LLVM).
- **Значение из `next(x)`** приходит результатом `yield`. Его тип пишут в
  `Generator<T, R, N>`, как в TS: без этого tsc выдаёт ошибку TS7057.
  ```ts
  function* talk(): Generator<string, void, string> {
    const name = yield "как тебя зовут?"
    console.log("привет,", name)
  }
  ```
- **`break` и `return` из `for...of`** вызывают `return()` у итератора — как в JS. `finally`
  в языке нет: файл и другие ресурсы, которые генератор открыл, закрываются сами, когда
  генератор освобождается — и при раннем выходе, и когда его бросили недоделанным
  ([errors.md](errors.md), раздел 6).

## 2. Свои коллекции

```ts
class Range {
  constructor(readonly from: int, readonly to: int) {}
  *[Symbol.iterator]() {
    for (let i = this.from; i < this.to; i++) yield i
  }
}

const r = new Range(0, 3)
for (const i of r) console.log(i)   // 0, 1, 2
const all = [...r]                  // [0, 1, 2]
const [first, second] = r           // 0, 1
const set = new Set(r)
```

- `for...of`, `[...x]`, `new Set(x)` — это и есть просьба «перебери», поэтому вызов
  `[Symbol.iterator]` здесь не считается скрытым. По типу видно, чей метод вызовется, и
  компилятор вызывает его напрямую, без поиска при выполнении. Коллекции из TS-библиотек
  (деревья, очереди, графы) компилируются как есть.
- Деструктуризация `const [a, b] = r` тоже вызывает `[Symbol.iterator]`, хотя выглядит как
  чтение.
- Итератор можно написать и без генератора; `{ done, value }` компилятор в куче не создаёт:
  ```ts
  class Countdown {
    constructor(private n: int) {}
    [Symbol.iterator]() {
      let n = this.n
      return {
        next: (): IteratorResult<int, undefined> =>
          n > 0 ? { done: false, value: n-- } : { done: true, value: undefined },
      }
    }
  }
  ```
- **Перебираемое принимают везде, где в JS:** `[...x]`, `const [a, b] = x`, `Array.from(x)`,
  `new Set(x)`, `new Map(x)`, `Promise.all(x)`, `yield* x`.
- **Типы:** `Iterable<T>`, `Iterator<T>`, `IterableIterator<T>`, `Generator<T, R, N>`. Функция с
  параметром `Iterable<int>` получает свою копию под каждый конкретный тип
  ([generics.md](generics.md)), без вызовов через таблицу.

## 3. Встроенные коллекции

- Строки — по кодовым точкам, как в JS: `"😀"` — один шаг, хотя `.length` у него 2 (уже есть).
- Массивы — `keys()`, `values()`, `entries()`.
- `Map` и `Set` — в порядке добавления (вместе со словарями, [objects.md](objects.md), шаг 7).
- Изменение коллекции во время перебора — как в JS: `push` внутри `for...of` удлиняет перебор
  (уже так), новые ключи `Map` тоже попадают в перебор.

## 4. Ленивые цепочки

```ts
// Методы массива: два новых массива, перебираются все numbers.
const a = numbers.filter(n => n > 100).map(n => n * 2).slice(0, 3)

// Ленивые: промежуточных массивов нет, перебор останавливается на третьем.
const b = numbers.values().filter(n => n > 100).map(n => n * 2).take(3).toArray()

// С бесконечным генератором:
naturals().filter(n => n % 3 == 0).map(n => n * n).take(4).toArray()   // [0, 9, 36, 81]
```

Методы из JS 2025 (есть в node 22+ и в TS): `map`, `filter`, `take`, `drop`, `flatMap`,
`reduce`, `toArray`, `forEach`, `some`, `every`, `find`, `Iterator.from`. Цепочку компилятор
сливает в один цикл. В node каждый шаг цепочки — отдельный объект и вызов на каждый элемент.

## 5. Символы

`Symbol.iterator` и `Symbol.asyncIterator` бывают только именами методов. Своих `Symbol("x")`
пока нет — ошибка с объяснением.

## 6. Что рядом

- **`safeTry` убран** ([errors.md](errors.md), раздел 3): ранний выход при `Err` — обычный
  `if (r.isErr()) return r`. `yield*` над `Result` — ошибка с этой подсказкой.
- **async-генераторы, `for await`, `Symbol.asyncIterator`** — вместе с async
  ([async.md](async.md), шаг 2). Например, `for await (const line of rl)` для `readline`.

## План реализации

На каждом шаге: тесты `tests/run` с `// same-as-node`, ошибки в `tests/errors`, прогон под
`TZ_CHECK_LEAKS=1`, обновление README.

1. **Генераторы** (сделано). `function*`, `yield`, `yield*`, `next(x)`; генератор как объект с
   состоянием; встраивание в `for...of`; `[...gen()]`, `Array.from`, деструктуризация из
   генератора; типы `Generator<T, R, N>` и `IterableIterator<T>` — встроенные, как `Array`.
   Новых зависимостей нет. `return()`, `throw()` и `finally` при выходе из `for...of` — после
   исключений ([errors.md](errors.md), шаг 2).
2. **Ленивые цепочки** (сделано) — методы итераторов, `Iterator.from`, слияние цепочки в один цикл;
   `keys()` / `values()` / `entries()` у массивов.
3. **Свои коллекции** (сделано) — `[Symbol.iterator]` в классах ([objects.md](objects.md), шаг 3),
   итератор руками с `{ done, value }` (объединения, шаг 6), параметры `Iterable<T>`
   ([generics.md](generics.md), шаг 2), `new Set(x)` / `new Map(x)` (словари, шаг 7).
4. **Async-генераторы и `for await`** (сделано) — вместе с async ([async.md](async.md), шаг 2).
5. **Поправки 26 октября** (сделано 26–27 октября; [errors.md](errors.md), шаги 1 и 5): убрать `throw()` у
   генераторов и `finally` в них (ресурсы закрываются сами), `safeTry` и `yield*` над `Result`.
