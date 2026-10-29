# typezero

Язык с синтаксисом TypeScript, который компилируется в нативный исполняемый файл, как Go.
Компилятор `tz` написан на C++ и генерирует код через LLVM; линковщик LLD встроен в него.

```ts
function fib(n: int): int {
  if (n < 2) return n
  return fib(n - 1) + fib(n - 2)
}

console.log("fib(30) =", fib(30))
```

```
tz run fib.tz            # скомпилировать и запустить
tz build fib.tz          # получить fib.exe
tz build --emit-llvm fib.tz   # посмотреть LLVM IR (fib.ll)
tz build --explain-parallel app.tz   # что пойдёт параллельно и почему (см. «Async»)
tz init                  # проект: tz.json, main.tz и .gitignore
tz add zod               # пакет (см. «Пакеты»)
tz run                   # без файла — "main" из tz.json
```

Программа из нескольких файлов собирается так же, из главного: `tz build main.tz` сам находит
файлы, которые импортирует `main.tz`, и те, что импортируют они (см. «Модули»).

Примеры программ лежат в [examples](examples): типы и преобразования, строки, массивы,
Фибоначчи, простые числа, последовательность Коллатца, разбор и печать чека
([receipt.tz](examples/receipt.tz)), калькулятор с `enum` и `switch` ([calc.tz](examples/calc.tz)),
точки на плоскости с `Math` и метками ([points.tz](examples/points.tz)). Сравнение скорости и памяти с Go на
одинаковых программах — в [bench](bench).

## Что уже работает

- Типы `int` (64 бита на любом процессоре), `int8`, `int16`, `int32`, `uint8`, `uint16`, `uint32`,
  `uint64` (ровно свои пределы), `double` (он же `number`), `decimal` (точные десятичные,
  см. «Точные десятичные числа»), `boolean`, `string`, `void`; псевдонимы `type Grid = int[][]`;
  `enum` с числами (`Color[value]` даёт имя члена) или строками.
- Функции с аннотированными параметрами, тип возврата выводится из `return` (и у рекурсивных
  тоже), необязательные параметры (`x?: T`) и значения по умолчанию (`x: T = value`; без
  аннотации тип даёт само значение: `n = 10`, `max = limit * 2`), rest-параметры
  (`...xs: int[]`), рекурсия, вызов до объявления. Spread в вызове: `f(...a)`, `Math.max(...a)`,
  `a.push(...b)`. Хвостовые вызовы гарантированы (см. «Функции и замыкания»).
- Функции как значения: стрелки (`x => x * 2`, `(a: int, b: int): int => { ... }`),
  функциональные выражения, функции, объявленные внутри блоков, типы функций
  (`(x: int) => int`, `f?: () => void`), вызов через значение и `f?.()`. Замыкания видят
  переменные вокруг, как в JS, и `for (let i ...)` даёт каждой итерации свою `i`. Встроенные
  функции передаются по имени туда, где ждут функцию: `["1.5"].map(parseFloat)`,
  `a.map(Math.sqrt)`.
- Обобщённые функции, классы, интерфейсы и псевдонимы типов (см. «Generics»):
  `function first<T>(xs: T[])`, `class Stack<T>`, `interface Repo<T>`,
  `type Pair<A, B> = { first: A; second: B }`, `type Maybe<T> = T | undefined`,
  `const id = <T>(x: T) => x` на верхнем уровне. Аргументы типа выводятся из аргументов (и из
  аргументов конструктора: `new Box(1)` — `Box<int>`), затем из ожидаемого типа
  (`const names: string[] = emptyList()`), затем из стрелок (`mapAll(xs, x => x * 2)`); их можно
  написать (`first<int>(xs)`) или задать по умолчанию (`<T = int>`). Ограничения `T extends
  number`, `T extends string | int`, `T extends { name: string }` (хотя бы эти поля),
  `T extends Animal`, `K extends keyof T`. Обобщённые методы (`map<U>(f: (x: T) => U)`), классы,
  которые расширяют экземпляры (`class IntStack extends Stack<int>`), обёртки над любой функцией
  (`...args: A` с `A extends unknown[]`, `[...A, ...B]`; `args.length`, `args[i]`,
  `args.join(", ")`, `fn(...args)`, `f.apply(this, args)`). Вычисления над типами: `keyof`, `T[K]`,
  `Partial`, `Required`, `Readonly`, `Pick`, `Omit`, `Exclude`, `Extract`, `NonNullable`,
  `ReturnType`, `Parameters`, `Awaited`, свои отображённые типы (`{ [K in keyof T]?: T[K] }`, с
  `as` для новых имён ключей), `typeof` в типах, `never`, условные типы с `infer` и распределением
  по объединениям, кортежи в типах (`[...A, B]`, `T["length"]`), шаблонные строковые типы
  (`` `on${Capitalize<K>}` ``, `` `${number}px` ``). Обобщённая функция как значение — там, где
  известны её типы: `lists.map(first)`, `first<int>`.
- `let` / `const`, блочная область видимости, глобальные переменные верхнего уровня.
- Деструктуризация массивов и объектов: `const [a, , b = 1, ...rest] = xs`,
  `const { id, name: who, limit = 10, ...rest } = user` (остаток — новый объект с остальными
  полями), вложенная (`const { pos: { x }, tags: [first] } = hero`), в параметрах функций и стрелок
  (`({ x, y }: Point) => x * y`, `points.map(({ x }) => x)`), в `for...of`
  (`for (const [k, v] of pairs)`, `for (const { name } of users)`) и в присваивании
  (`[a, b] = [b, a]` — без создания массива, `({ x: p.x, y } = o)`). Строка разбирается на символы,
  как и `[...s]`.
- `if` / `else`, `while`, `do ... while`, `for`, `for...of` (по массивам, по кодовым точкам
  строк и по итераторам), `for await...of` (по async-итераторам, строкам `readline` и массивам
  промисов), `for...in` (по ключам объекта), `switch`, `break`, `continue` (и с меткой:
  `break outer`), `return`.
- Генераторы (см. «Генераторы и итераторы»): `function*` (и в блоке, и как выражение,
  и обобщённые), методы-генераторы классов и литералов (`*items() { ... }`, и `static`),
  `yield`, `yield*` массива, строки или другого итератора (его значение — то, что
  тот вернул), `next()` и `next(x)`, типы `Generator<T, R, N>` и `IterableIterator<T>`.
  `for...of` по вызову генератора компилируется в обычный цикл. `[...gen()]`, `Array.from(gen())`,
  `const [a, b] = gen()` (берёт два значения, как в JS), spread в rest-параметр `f(...gen())`.
  Async-генераторы `async function*` и методы `async *items()`, `yield*` async-итератора,
  тип `AsyncGenerator<T, R, N>`.
- Ленивые цепочки из JS 2025: `map`, `filter`, `take`, `drop`, `flatMap`, `reduce`, `toArray`,
  `forEach`, `some`, `every`, `find` у любого итератора, `Iterator.from(x)`, `keys()`,
  `values()` и `entries()` у массивов: `numbers.values().filter(n => n > 100).map(n => n * 2)
  .take(3).toArray()` — один цикл без промежуточных массивов.
- Свои коллекции (см. «Генераторы и итераторы»): класс или литерал с `[Symbol.iterator]()` —
  методом-генератором `*[Symbol.iterator]()` или методом, который возвращает итератор, написанный
  руками (`next()` даёт `{ done, value }`, тип `IteratorResult<T, R>`), — перебирается везде, где
  в JS: `for...of`, spread, деструктуризация, `Array.from`, `yield*`, `new Set(x)`, `new Map(x)`.
  Параметр `Iterable<T>` принимает массивы, строки, `Map`, `Set`, генераторы и свои коллекции
  (копия функции на каждый вид), `Iterator<T>` — любой итератор, и написанный руками тоже.
- Арифметика, `**`, сравнения, `&&` / `||` с коротким замыканием, побитовые операции и сдвиги,
  `?:`, `++` / `--`, составное присваивание, `||=` / `&&=`, `typeof`, оператор «запятая»
  (`for (let i = 0, j = n; i < j; i++, j--)`).
- `void f()` — вычислить и дать `undefined`; `debugger` — остановиться, если программу запустили
  под отладчиком (в Windows), иначе ничего; литерал `5.` (`double`, как `5.0`) и
  `5..toString()`. `+s` и `-s` у строки — ошибка с подсказкой `Number(s)`, который даёт
  `Result`; у `boolean` — `Number(b)`.
- Истинность как в JS, но без чисел: в условии (`if`, `while`, `for`, `?:`, `!`, `&&`, `||`,
  `Boolean(x)`, `filter(Boolean)`) можно `boolean`, строку (пустая — ложь) и `T | undefined` у
  `boolean`, строк, массивов, функций, объектов и классов как значений (`typeof User |
  undefined`, `Constructor | undefined`; `undefined` — ложь). `&&` и `||` дают одно
  из значений: `nick || "аноним"` — строка, `list && list.length` — `int | undefined`. Проверка
  без вызовов: у строки — длина, у `T | undefined` — есть ли значение. `filter(Boolean)` и
  `filter(x => x != undefined)` оставляют элементы со значением, и тип это знает:
  из `(string | undefined)[]` получается `string[]`.
- Все функции и константы `Math` и `Math.clamp(x, lo, hi)`; `Number(x)`, `parseInt`,
  `parseFloat` (у строки — `Result`, см. «Чем отличается от TypeScript»), `Number.isInteger`,
  `Number.isSafeInteger`, `Number.MAX_VALUE` и остальные константы `Number`. `NaN`,
  `Infinity`, `isNaN` и `isFinite` нет (см. там же),
  `x.toFixed(n)`, `x.toString(radix)` с основанием от 2 до 36 (цифры и дробь — как пишет V8:
  `(255).toString(16)` — `"ff"`, `(0.5).toString(2)` — `"0.1"`); основание вне 2…36 и число
  цифр `toFixed` вне 0…100 — ошибка компиляции, если не доказано, что они в пределах (см.
  «Доказательства вместо падений»).
- `decimal` — точные десятичные числа любой длины, как в BigNumber.js, но с обычными операторами
  ([decimal.md](docs/decimal.md)): `const price: decimal = 19.99`, `price * 3 + 0.01`,
  `0.1 + 0.2 == 0.3`. Литерал там, где ждут `decimal`, и рядом с `decimal` читается из текста
  программы, точно (и `123456789012345678901234567890`, и `(0.1 + 0.2) * price`). Операторы
  `+ - * / % **` (показатель — целое), унарный `-`, сравнения по значению (`0.30 == 0.3`),
  составное присваивание, `++` / `--`. Целое становится `decimal` через `n.toDecimal()`, `double`
  — через `decimal(x)` (кратчайшая запись, как у `String(x)`), обратно — `Number(x)`;
  `decimal("1_000.5")` читает строку, как BigNumber.js: литерал проверяется при сборке и даёт
  `decimal`, а строка, известная только при выполнении, — `Result<decimal, DecimalError>`
  (`code: "ERR_INVALID_DECIMAL"`, `message`, `text`). `/` даёт 20 знаков после точки с
  округлением half-up. Печать, `String(x)` и шаблоны — без степени. Методы BigNumber.js, которые
  не заменены операторами: `div(y, places, mode)`, `round(places, mode)`, `toFixed`, `toFormat`,
  `toString(base)`, `toExponential`, `toPrecision`, `sqrt`, `abs`, `isInteger`, `decimalPlaces()`,
  `shiftedBy`, `toFraction`, `decimal.max` / `min` / `sum` (и со spread: `decimal.sum(...prices)`);
  режимы округления — строками: `"up"`, `"down"`, `"ceil"`, `"floor"`, `"half-up"`, `"half-down"`,
  `"half-even"`, `"half-ceil"`, `"half-floor"`.
- Строки с семантикой JS: `+` и `+=` между строками, шаблонные строки `` `x = ${x}` `` (числа
  и `boolean` в них превращаются в строку; число + строка — ошибка, см. ниже), сравнения,
  `.length`, `s[i]`, `charAt`, `charCodeAt`, `codePointAt`,
  `at`, `slice`, `substring`, `indexOf`, `lastIndexOf`, `includes`, `startsWith`, `endsWith`,
  `repeat`, `concat`, `split`, `trim`, `trimStart`, `trimEnd`, `padStart`, `padEnd`,
  `toUpperCase` / `toLowerCase` (по всем правилам Unicode, как в V8: `"ß"` → `"SS"`),
  `replaceAll` со строкой (`$&` и прочие подстановки работают), `String(x)`, `x.toString()`,
  `String.fromCharCode`, `String.fromCodePoint`; по правилам языков — `localeCompare` и
  `normalize` (см. «Языки: Intl»). `encodeURIComponent` и `encodeURI` — как в
  node, с `URIError: URI malformed` у одиночного суррогата; `decodeURIComponent` и `decodeURI`
  отдают `Result<string, UriError>`, где `UriError = { kind: "URI"; code: "ERR_INVALID_URI"; message: string }`, —
  `Err` у неверного `%`-кода; это функция рантайма над однобайтовой или двухбайтовой строкой, а строка, в которой
  нечего менять, возвращается без копии.
- Регулярные выражения (см. «Регулярные выражения»): литералы `/…/` со всем синтаксисом JS и всеми
  флагами `dgimsuvy` — машинный код без взрыва перебора, `RegExp.parse(s, flags)` с `Result`,
  `test`, `exec`,
  `match`, `matchAll`, `search`, `replace` / `replaceAll` (с текстом замены и с функцией),
  `split`, `lastIndex`, `source`, `flags`, `global` и другие флаги, `RegExp.escape`.
- Даты (см. «Даты»): `Date` как в JS, но время и поля — целые (`int`): `new Date()`, `new Date(ms)`,
  `new Date("2026-10-23T10:00")`, `new Date(2026, 9, 23)`, `Date.UTC`, `Date.parse`, все `get…` и
  `set…` (местные и UTC), `getTimezoneOffset`, `toISOString`, `toJSON`, `toString`,
  `toDateString`, `toTimeString`, `toUTCString`; `toLocaleString`, `toLocaleDateString`,
  `toLocaleTimeString` с языком и опциями и `Intl.DateTimeFormat` (`format`, `formatToParts`,
  `resolvedOptions`); сравнение и вычитание дат, `+d`. Пояс — из `TZ` или системы, как в node.
- Двоичные данные (см. «Байтовые массивы»): `ArrayBuffer`, `Uint8Array` и остальные типизированные
  массивы (`Int8Array`, `Uint8ClampedArray`, `Int16Array`, `Uint16Array`, `Int32Array`,
  `Uint32Array`, `Float16Array`, `Float32Array`, `Float64Array`), `DataView` со всеми `get…` /
  `set…`, `Buffer` из node с кодировками `utf8`, `utf16le`, `latin1`, `ascii`, `base64`,
  `base64url`, `hex`, `TextEncoder` и `TextDecoder` (и со старыми кодировками вроде
  `windows-1251` и `koi8-r`), `embedBytes(path)`. Значение, которое не влезает в элемент
  (`u8[0] = 300`), — ошибка компиляции, если не доказано, что влезает.
- Языки (см. «Языки: Intl»): `toLocaleString` чисел и `decimal`, `Intl.NumberFormat`,
  `Intl.Collator` и `localeCompare`, `normalize`, `Intl.Segmenter`, `Intl.PluralRules`,
  `Intl.RelativeTimeFormat`, `Intl.ListFormat`, `Intl.DisplayNames`, `Intl.getCanonicalLocales`,
  `supportedLocalesOf` — как в node.
- Шаблоны с тегом: `` tag`a${x}b${y}c` `` — это `tag(["a", "b", "c"], x, y)`: тег получает
  массив текстов (`string[]`, тип `TemplateStringsArray` тоже подходит) и значения, которые
  проверяются как аргументы обычного вызова (например, rest-параметром `...values: int[]`).
  `` String.raw`C:\dir\${name}` `` оставляет escape-последовательности как написаны.
- `undefined` и тип `T | undefined`: сравнения `== undefined`, `??`, `??=`, `?.`
  (`a?.length`, `a?.[i]`, `s?.trim()`), сужение типа после проверки
  (`if (x != undefined) x + 1`, `if (s) s.length`, после `if (!s) return`). Функция с типом
  `T | undefined` может просто закончиться — она вернёт `undefined`. `x!` нет: вместо него
  проверка (`if (x == undefined) return …`) или значение по умолчанию (`x ?? 0`).
- Объединения и литеральные типы (см. «Объединения»): `string | int`, `Circle | Square`,
  `type Dir = "up" | "down"`, `0 | 1`, `true`; сужение по метке (`s.kind == "circle"`, `switch`),
  `typeof`, `in`, `instanceof`, `Array.isArray`, `== "up"`, по присваиванию; `switch` по
  литеральному типу обязан разобрать все значения; `never`. Тип выводится объединением там, где
  значения разные: `[1, "a"]`, ветки `?:` и `??`, `return`, `yield`, ошибки `Result`.
- Функции-проверки (см. «Проверки»): функция, которая возвращает `boolean`, сужает в `if` так же,
  как условие из её тела (`if (isCircle(s)) s.r`, `if (validIndex(a, i)) a[i] += 1`), `x is T`
  и `this is T` проверяются по телу, `filter(isCircle)` даёт `Circle[]`; `asserts x is T` —
  ошибка (исключений нет).
- Массивы `T[]` (или `Array<T>`) любых из этих типов, вложенные: литералы `[1, 2]` и
  `[...a, x]`, `a[i]`, `a[i] = x`, `.length` (чтение и запись), `new Array<T>(n)` (длина
  доказана не отрицательной), `push`, `pop`,
  `shift`, `unshift`, `splice`, `at`, `slice`, `concat`, `indexOf`, `lastIndexOf`, `includes`,
  `join`, `reverse`, `fill`, `sort(compare)` и `sort()` (у массива чисел — ошибка, см. ниже),
  `toString`; с колбэком — `forEach`,
  `map`, `filter`, `flatMap`, `reduce`, `reduceRight`, `find`, `findIndex`, `findLast`,
  `findLastIndex`, `some`, `every`. `reduce` без начального значения даёт `T | undefined`, пока
  не доказано, что массив не пуст (в JS — `TypeError` у пустого).
- Объекты (см. «Объекты»): типы `interface Point { x: double; y: double }`,
  `type Size = { width: int; height?: int }` и прямо в аннотации, литералы `{ x: 1, y }` (короткая
  запись, ключи-строки вроде `"content-type"`, ключи в скобках `{ [key]: v }`, spread
  `{ ...user, name: "Anna" }`), чтение и запись полей, в том числе `o[key]` с ключом-константой,
  `+=`, `++`, `??=`, `||=`, `&&=` у полей, `p?.x`, необязательные поля, `readonly`, вложенные
  объекты, массивы объектов со всеми методами массивов, поля с функциями (`o.onClick(x)`),
  рекурсивные типы (списки, деревья), объекты в параметрах, результатах и замыканиях, `==` (тот же
  ли это объект), `typeof` — `"object"`, `Point | undefined` с проверками, `??` и `?.`,
  `Object.keys` / `values` / `entries`.
- Кортежи `[string, int]` (и `readonly [int, int]`, и пустой `[]`): `t[0]`, `t[1] = v`, `t.length`,
  деструктуризация, `[...t]`, если у элементов один тип (см. «Объекты»).
- Словари (см. «Словари»): `Map<K, V>` и `Set<T>` со всеми методами JS (`get`, `set`, `has`,
  `delete`, `clear`, `size`, `forEach`, `keys` / `values` / `entries` и методы множеств из ES2025:
  `union`, `intersection`, `difference`, `symmetricDifference`, `isSubsetOf`, `isSupersetOf`,
  `isDisjointFrom`), `new Map(пары)` и `new Set(значения)` из любого перебираемого, `Map.groupBy`;
  `Record<string, T>` и `{ [key: string]: T }` с синтаксисом объекта: `r[k]`, `r.k`, `r[k] = v`,
  `r[k] ??= v`, `delete r[k]`, `k in r`, `Object.keys` / `values` / `entries`,
  `Object.fromEntries`, `Object.groupBy`, `for...in`, литералы с ключами в скобках и spread. Ключи
  идут в порядке добавления; `for...of`, spread, деструктуризация, `Array.from`, `yield*` и
  ленивые цепочки перебирают `Map` и `Set`, как в JS. `process.env` — словарь всех переменных.
- Классы (см. «Классы»): поля с начальными значениями, конструктор, параметры-свойства
  (`constructor(public x: double)`), методы и `this`, `new`, `static` поля и методы,
  `private` / `protected`, `readonly`, `instanceof`, стрелки в полях, метод как значение
  (`const f = counter.inc` — привязан к объекту), перегрузка методов, статических методов и
  конструкторов (и обобщённые варианты); экспорт и импорт классов. Методы в литералах:
  `{ n: 0, inc() { this.n++ } }`.
- Наследование и интерфейсы (см. «Наследование и интерфейсы»): `extends`, `super(...)`,
  `super.m()`, `override`, `abstract` классы и методы, `implements`, интерфейсы с методами
  (`area(): double`) и с несколькими сигнатурами метода, `interface User extends Named`,
  литералы интерфейсов с методами; `instanceof` сужает переменную до класса наследника.
- Циклы ссылок освобождаются без сборщика мусора (см. «Циклы ссылок»): деревья с `parent`,
  двусвязные списки, графы, наблюдатели, чьи замыкания держат субъект, `Map` объектов, которые
  держат сам `Map`. `WeakRef<T>`: `deref()` даёт `T | undefined`.
- Декораторы (см. «Декораторы»), как `experimentalDecorators` в TS, но без флага: у классов,
  полей, методов, параметров методов и конструкторов, `static`-членов; `@Log`, `@a.b`,
  `@Table("users")`, `@(выражение)`, в том порядке, в каком их применяет TS. Декоратор получает
  сам класс, а декоратор метода — его описание `MethodDescriptor<A, R>` и может подменить метод.
  Метаданные встроены: `Reflect.defineMetadata`, `getMetadata`, `getOwnMetadata`, `hasMetadata`,
  `hasOwnMetadata`, `deleteMetadata`, `@Reflect.metadata(key, value)`; записи по ключу-объекту
  (самому декоратору) имеют выведенный тип, по строке — `unknown`; компилятор пишет
  `design:type`, `design:paramtypes` и `design:returntype`. Декоратор класса может вернуть класс,
  который его расширяет, — тогда имя означает этот класс.
- `unknown` (см. «unknown и object») — значение любого типа, которое можно хранить, передавать,
  сравнивать и печатать, а остальное — после проверки (`typeof`, `instanceof`, `Array.isArray`,
  `!= undefined`); `object` — любой объект языка.
- Классы как значения (см. «Классы как значения и отражение»): `const K = User`, `kind: typeof
  User`, `new K(...)`, `K.name`, `K.create()`, печать как в node, `[class User] { count: 0 }` со
  статическими полями (`String`, `Number`, `Date` и другие встроенные — как `[Function:
  String]`); `Constructor<T>` — класс, известный только при выполнении, в массивах и ключах
  `Map`, `new k()`, `x instanceof k`; class-выражения, примеси (`function Mixin<B extends
  Constructor>(Base: B) { return class extends Base { ... } }`), `ConstructorArgs`,
  `InstanceType<typeof K>`. Отражение: `Reflect.get`, `Reflect.apply`, `Reflect.construct` с
  проверкой при выполнении (`Result<T, ReflectError>`).
- Ошибки (см. «Ошибки» и «Result»): значения `Result<T, E>`, ранний выход через `if`, ошибки —
  объекты с меткой `code`; `throw`, `try` / `catch` / `finally`, `Error` и его виды — ошибки
  компиляции с подсказкой. Стек — 1 ГБ на поток; фатальны только его переполнение, нехватка
  памяти и то немногое, что компилятор не может проследить (см. «Что фатально»): `panic: …` и
  код 2.
- Async (см. «Async»): куски, которые трогают только свои данные, идут на всех ядрах, а результат —
  такой, какой мог бы выдать node; `async` функции, стрелки и методы,
  `await` (и на верхнем уровне файла), `Promise<T>` с `then`, `new Promise((resolve) => ...)`,
  `Promise.resolve` / `all` / `race` / `withResolvers`; `Promise` не отклоняется: ошибка —
  `Promise<Result<T, E>>`, а `reject`, `catch`, `finally`, `Promise.reject`, `allSettled` и `any`
  — ошибки компиляции с подсказкой; `queueMicrotask`, `process.nextTick`, `setTimeout`,
  `setInterval`, `setImmediate`, `clearTimeout` / `clearInterval` / `clearImmediate`, у таймеров
  `ref()` / `unref()` / `hasRef()` / `refresh()`. Порядок вывода — как у node; `Promise`, которого
  никто не ждёт, — ошибка компиляции, `void p` отпускает его явно. Ввод-вывод ждёт пул потоков:
  `node:fs/promises`, `node:readline`. `node:timers/promises` (`await setTimeout(ms, value)`,
  `setImmediate`, `setInterval` как async-итератор, `scheduler.wait` / `yield`). Отмены нет:
  `AbortController` и `AbortSignal` убраны, время `fetch` ограничивает его опция `timeout`.
- Сеть (см. «Сеть»): сервер `new Server(options, handler)` из `"tz:http"` (`server.listen()`
  открывает сокет и отдаёт `Listening`), где обработчик получает
  `Request` и того, кто его прислал, и отдаёт `Response`, как в Deno и Bun (и `app.fetch` из
  hono), и клиент `fetch`, как в
  node: keep-alive и пул соединений, редиректы, gzip / deflate / br, `timeout`, ошибки —
  `Err` с `FetchError` (`fetch failed` и причина). `URL` и `URLSearchParams` по стандарту WHATWG, `Headers`,
  `Request`, `Response`, `ReadableStream`, `FormData` — глобальные, как в node; https — на OpenSSL.
- Ошибки как значения: встроенный `Result<T, E>` (см. «Result»): `ok(v)`, `err(e)`,
  `isOk()` / `isErr()` с сужением, ранний выход `if (r.isErr()) return r`, `value`, `error`,
  `map`, `mapErr`, `andThen`, `orElse`, `match`, `unwrapOr`, `andTee`, `orTee`, `andThrough`,
  `Result.combine`, `Result.combineWithAllErrors`; `Err` помнит, где его создали. Асинхронная
  ошибка — `Promise<Result<T, E>>`. Забытый `Result`, `==` между ними и лишний `return await` —
  ошибки компиляции.
- `console.log` с любыми из этих типов (и `console.log(...xs)` — элементы массива как отдельные
  аргументы); массивы, объекты, кортежи, словари и итераторы
  (`Object [Generator] {}`) печатаются ровно как в Node.js
  (длинные — по строкам, глубже двух уровней — `[Object]`, объект, который ссылается на себя, —
  `<ref *1>` и `[Circular *1]`). `console.info` и `console.debug` печатают так же, `console.error` и
  `console.warn` — так же, но в stderr.
- Процесс (см. «Процесс и вывод»), глобальные имена без импорта, как в node: `process.argv`
  (аргументы — с индекса 2: `process.argv.slice(2)`), `process.env.HOME` и `process.env[name]` —
  `string | undefined`, `process.env` целиком — `Record<string, string>` (`Object.keys`, перебор,
  присваивание и `delete` меняют переменные процесса), `process.exit(code)`, `process.exitCode`,
  `process.cwd()`,
  `process.platform`, `process.execPath`, `process.stdout.write(s)` и `process.stderr.write(s)` (без
  перевода строки; строка, `Buffer` или `Uint8Array`), `process.stdout.isTTY`, `process.stdin` —
  `Readable`, а `process.stdout` и `process.stderr` — `Writable` (см. «Потоки»); `Date.now()` и
  `performance.now()`.
- Встроенные модули (см. «Встроенные модули»), API node, импорт по имени: `node:path` (все функции,
  `sep`, `delimiter`, `posix` и `win32`), `node:os`, `import { argv, env } from "node:process"`,
  файлы из `node:fs` (`readFileSync(path, "utf8")`, `writeFileSync`, `appendFileSync`, `existsSync`,
  `mkdirSync`, `readdirSync`, `statSync`, `rmSync`, `renameSync`, `copyFileSync` и другие) дают
  `Result`: ошибка ОС — `Err` с `SystemError`, как в node (`code`, `errno`, `syscall`, `path`), те
  же функции из `node:fs/promises` дают `Promise<Result>` (`await readFile(path, "utf8")`),
  `node:readline` поверх stdin
  (`rl.question(...)`, `for await (const line of rl)`) и `readLine` из `"tz:io"` — строка ввода.
- События (см. «События»): `EventEmitter` из `node:events` со всеми методами node, тем же порядком
  слушателей, `newListener` / `removeListener`, событием `error` и предупреждением о лишних
  слушателях; карта событий пишется (`new EventEmitter<{ data: [chunk: string]; end: [] }>()`,
  `class Server extends EventEmitter<Events>`) или выводится из всех `emit` программы;
  async-слушатели, `once(emitter, name)` — `Promise` (с `Result`, если эмиттер может испустить
  `error`), `on(emitter, name)` — async-итератор для `for await`; событие `error` без
  исключений: кто подписан на эмиттер с `error`, подписан и на `error`. Глобальные, как в node:
  `EventTarget`, `Event`, `CustomEvent<T>`, `DOMException`.
- Потоки (см. «Потоки»): `node:stream` — `Readable`, `Writable`, `Duplex`, `Transform`,
  `PassThrough`, `pipeline`, `finished`, `compose`, `Readable.from`, `for await`, тот же
  порядок событий, что в node; ошибки — `Result` (каждый кусок `for await`, `pipeline`,
  `finished`, `end()`), `pipe()` нет; тип кусков известен при компиляции (`Readable<string>`, в
  `objectMode` — выводится из `push()`), `setEncoding` меняет его дальше по коду;
  `createReadStream` / `createWriteStream` из `node:fs`, `node:stream/promises`,
  `node:stream/consumers`, `node:string_decoder`.
- Криптография (см. «Криптография»): `node:crypto` на OpenSSL 3.5, как в node 24 — хеши
  (`createHash`, `hash`), HMAC, `randomBytes` / `randomUUID` / `randomInt` / `getRandomValues`,
  `timingSafeEqual`, `pbkdf2Sync`, `scryptSync`, `argon2Sync`, `hkdfSync`, шифры (`createCipheriv`:
  AES-GCM / CBC / CTR, ChaCha20-Poly1305 и остальные из `getCiphers()`), ключи `KeyObject` в PEM /
  DER / JWK, подписи RSA / ECDSA / Ed25519, `generateKeyPairSync`, ECDH и X25519,
  `X509Certificate`. То, что зависит от данных (ключ, тег, PEM, подпись), даёт `Result` с
  `CryptoError`. OpenSSL попадает в exe, только если программа им пользуется, а алгоритм с
  именем в коде — без остального OpenSSL; `tz licenses app.exe` печатает лицензии того, что в exe.
- Необязательные точки с запятой (ASI), комментарии, escape-последовательности.
- Программы из нескольких файлов (см. «Модули»): `import { a, b as c } from "./x"`, `export`
  перед объявлением, `export { a, b as c }`, `export { a } from "./x"`, `export * from "./x"`;
  пути `"./math"` и `"./math.tz"`, каталог — его `index.tz`; циклические импорты. Файлы программ
  на tz — `.tz`; на `.ts` и `.js` — ошибка с подсказкой переименовать.
- Пакеты (см. «Пакеты»): `import { ms } from "ms"` по имени из `tz.json`, пакеты из каталога, git
  и JSR, `tz.lock` с версиями и хешами, общий кэш исходников, `tz init`, `tz add`, `tz remove`,
  `tz update`, `tz build` и `tz run` без файла; `tz port` переносит TS-библиотеку в файлы `.tz`.
- Редактор (см. «Редактор»): ошибки по мере набора (`tz lsp`), подсветка и форматирование файлов
  `.tz` в VS Code.
- Вопросы к компилятору (см. «Вопросы к компилятору и инструменты для ИИ»): `tz map`, `tz context`,
  `tz query`, `tz impact` и сервер MCP `tz mcp` для ИИ-агентов, с проверкой правки без записи.
- JSON (см. «JSON»): тип `Json` (`null` бывает только в нём), `JSON.parse(text)` с `Result`,
  `JSON.stringify(x, null, space)` любого значения, чтение и запись ключей (`j.users[0].name`),
  сужение `typeof`, `Array.isArray` и `!= null` — и у полей (`data.name`), и через функцию-проверку;
  свой тип из `Json` — проверкой и литералом; файлы, вшитые в exe: `embedText("./page.html")`,
  `JSON.parse(embedText("./config.json"))` (проверяется при сборке); импорт файла JSON —
  `import config from "./config.json" with { type: "json" }`: значение вшито как литерал, тип
  точный (`null` и смешанное — `Json`; docs/modules.md, раздел 5).
- То, что нужно tsc, принимается и ничего не меняет: `readonly T[]` и `ReadonlyArray<T>`
  (изменения массива компилятор не запрещает). Целый литерал больше 64 бит — `double`, как в JS.

Код верхнего уровня — это точка входа программы; в программе из нескольких файлов — код
верхнего уровня всех файлов в порядке их загрузки.

## Строки и память

Длина и индексы строк считаются в единицах UTF-16, как в JS: `"привет".length` — 6,
`"😀".length` — 2. Внутри строка — значение из двух слов (указатель и длина с флагами), а
данные хранятся как в V8: по байту на символ, если хватает Latin-1, иначе по два. Строка до 8
таких однобайтовых символов лежит прямо в значении, на месте указателя: `` `w${i}` `` или `s[i]`
не выделяют памяти, и счётчика ссылок у такой строки нет. Подстроки
длиной от 13 символов не копируются, а ссылаются на исходную строку. Длинная конкатенация
создаёт rope (узел из двух частей), которая склеивается при первом чтении в буфер с запасом
места, а следующие `+=` дописывают в этот запас. Поэтому сборка строки через `+=` линейна, даже
если строку читают между дописываниями (V8 на таком цикле квадратичен).

Поиск (`indexOf`, `includes`, `split`, `replaceAll`) просматривает по 16 байт за раз. `split`
сначала считает разделители и создаёт массив сразу нужной длины, а `join` и `replaceAll` заранее
считают длину результата и пишут его в один буфер. `s.charCodeAt(i)`, `s[i]` и `for...of` по
однобайтовой строке встраиваются в цикл без вызовов. Число в строку переводит алгоритм Ryū
(кратчайшие цифры, как в V8), строку в число — обычно без `strtod`. Цифры с Go — в
[bench](bench/README.md).

Память строк, массивов и объектов освобождается подсчётом ссылок, без сборщика мусора. Литералы
лежат в exe и не считаются. Переменная освобождает своё значение при выходе из области видимости
и при присваивании, массив — свои элементы, а объект — значения своих полей, когда умирает сам.
Объекты, которые могут ссылаться друг на друга по кругу, освобождаются группами (см. «Циклы
ссылок»). В конце программы память отдаёт операционная система, без обхода всех объектов. С
переменной окружения `TZ_CHECK_LEAKS=1` программа, наоборот, освобождает всё и сообщает о
неосвобождённых объектах и о группах, которые остались, с типами их объектов; тесты запускаются
именно так.

Память выделяет собственный аллокатор рантайма (как mimalloc, упрощённый для одного потока):
сегменты по 4 МБ, выровненные на 4 МБ, страницы по 64 КБ для блоков одного размера, большие
объекты — отдельными кусками у ОС. Выделение и освобождение маленького блока — несколько
инструкций; на миллионах строк это вдвое быстрее `malloc` из C. Четыре последних освобождённых
больших блока до 1 МБ остаются для следующих: поток файла кусками по 64 КБ (и строки из них) не
ходит за каждым к ОС.

## Функции и замыкания

Значение функции — два слова: код и окружение с захваченными переменными. Функция, которая
ничего не захватывает, памяти не выделяет. В окружение попадают только переменные, которые
функция использует (в V8 замыкания одной области делят общий контекст, и одно из них может
держать в памяти то, что нужно только другому). Переменная, которая после создания замыкания
не меняется, копируется в окружение; изменяемая живёт в «коробке» в куче, общей для функции, где
она объявлена, и всех замыканий. Функции, объявленные в одном блоке, делят одно окружение и
вызывают друг друга (и себя) без захвата друг друга, так что циклов ссылок не возникает; так же
стрелка в `const f = () => ... f() ...` вызывает себя напрямую.

Колбэк, написанный прямо в вызове встроенного метода (`a.map(x => x * 2)`, `sort((a, b) => a -
b)`), держит окружение на стеке, без выделения памяти и подсчёта ссылок, и обычно встраивается:
`map` с такой стрелкой компилируется в простой цикл.

**Хвостовые вызовы гарантированы.** Вызов функцией самой себя, или другой функции того же цикла
таких вызовов, в хвостовой позиции становится переходом: аргументы записываются в параметры, и
управление уходит в начало тела. Стек не растёт, и так на любом уровне оптимизации:
`sumTo(10_000_000, 0)` с `return sumTo(n - 1, acc + n)` работает (node падает с переполнением
стека). Хвостовая позиция — `return f(...)`, ветки `?:` и правая часть `&&`, `||`, `??` в
`return`, а в функции, которая ничего не возвращает, — вызов последним оператором (или перед
`return;`). Цикл образуют функции верхнего уровня или функции одного блока; стрелка может
вызывать так только себя. Вызов через значение функции и нехвостовой вызов (`1 + f(n - 1)`)
растят стек как обычно.

Стек — 1 ГБ на поток, у главного тоже (это резерв адресов: память выделяется по мере роста),
поэтому кончиться он может только при бесконечной рекурсии. Проверок стека в начале функций нет:
переполнение ловит защитная страница, и программа завершается — `panic: stack overflow`, код
выхода 2 (см. «Ошибки»).

## Ошибки

Исключений в tz нет ([errors.md](docs/errors.md)): ошибка — значение, которое функция возвращает
в `Result` (см. «Result»), а ранний выход — обычные `if` и `return`.

```ts
type User = { id: int; name: string }
type NotFound = { code: "not_found"; message: string; id: int }

function find(users: User[], id: int): Result<User, NotFound> {
  for (const u of users) if (u.id == id) return ok(u)
  return err({ code: "not_found", message: `нет пользователя ${id}`, id })
}
const user = find([{ id: 1, name: "Ann" }], 7)
if (user.isErr()) console.log(user.error.message)
else console.log(user.value.name)
```

- **Чего нет**: `throw`, `try` / `catch` / `finally`, `Error`, `TypeError`, `RangeError` и
  другие виды, `class HttpError extends Error`, `throw()` у генераторов — ошибки компиляции с
  подсказкой, что писать вместо: `return err(...)`, тип объекта с меткой, `Result` и `if`.
  `throw` и `try` — даже не синтаксис: их называет разборщик, и до проверки типов дело не
  доходит. `process.on("uncaughtException")` — тоже ошибка: ловить нечего.
- **Ресурсы закрываются сами** ([errors.md](docs/errors.md), раздел 6): файл, который открыл
  поток (`createReadStream`, `createWriteStream`), закрывается, когда на поток больше никто не
  ссылается, — без `close()` и `destroy()`; не удалось закрыть — сообщение в stderr, программа
  идёт дальше. Тот же механизм в коде библиотеки делает уборку вместо `finally`: `on()` снимает
  слушателей, перебор `body` и потока отпускает читателя, когда цикл `for await` прервали.
  Известный недочёт: поток, которого ждали через `once()`, пока живёт до конца программы, и его
  файл закрывается только тогда.
- **Ошибка — объект с меткой.** Свои ошибки — типы объектов с полем `code`; ошибки библиотеки —
  такие же встроенные типы (`SystemError`, `JsonError`, `FetchError`, `StreamError`…). Разные
  ошибки в одном `Result` дают объединение, и `if (e.code == "ERR_INVALID_REGEX")` сужает его до одной.
- **`Promise` не отклоняется** (см. «Async»): операция, которая может не получиться, отдаёт
  `Promise<Result<T, E>>`.
- **Что фатально.** То, что компилятор не может проверить при сборке, завершает программу: в
  stderr — `panic: текст`, код выхода 2, а напечатанное до этого дописывается. Это переполнение
  стека (`panic: stack overflow`), нехватка памяти, строка, массив или буфер длиннее предела,
  `decimal` больше 10^10000000. Операции с числами компилятор доказывает
  ([numbers.md](docs/numbers.md)): деление на 0 (и у `decimal`), корень из отрицательного
  `decimal`, сужение, выход за тип — ошибки компиляции; паника (`panic: Division by zero`,
  `panic: 1.5e+21 does not fit in int`) остаётся только там, где значение приходит из данных,
  за которыми компилятор не следит (поле, которое меняет программа). Ошибка в коде самой
  библиотеки — `panic: internal error: …`. Поймать панику нельзя.
- **Как это устроено.** `Result` — значение без памяти в куче, а ранний выход — обычный
  `return`: ни ячейки брошенной ошибки, ни проверок после вызовов, ни таблиц раскрутки. Ошибка
  стоит столько, сколько `if`, который написал программист.

## Массивы

Массив — ссылка, как в JS: после `let b = a; b.push(1)` новый элемент видно и через `a`.
Элементы хранятся без упаковки по статическому типу: `int[]` — подряд лежащие 64-битные числа,
`boolean[]` — по байту, `string[]` — по 16 байт. Видов элементов, переходов между ними и дыр,
как в V8, нет: тип известен при компиляции. Новый массив — одна аллокация (заголовок и
элементы рядом), при росте элементы переезжают в отдельный буфер с запасом в полтора раза.
`shift()` сдвигает начало и не копирует элементы, `unshift()` оставляет запас спереди, так что
очередь и дек работают за O(1) на операцию (в V8 `shift` на массивах больше ~16 тысяч
элементов копирует весь массив).

**Индексы.** Если компилятор доказал, что индекс в пределах массива или строки, `a[i]` имеет
тип элемента и читается без проверки. Иначе, как в JS, результат может быть `undefined`, и тип
— `T | undefined`:

```ts
for (let i = 0; i < a.length; i++) sum += a[i]   // int, проверки нет
if (i >= 0 && i < a.length) use(a[i])           // int
const x = a[n]                                   // int | undefined
total += a[n] ?? 0
```

Компилятор следит за фактами вида `i < a.length`, `i >= 0`, `a.length == n + 1`: их дают
условия, объявления и присваивания (`n = Math.min(a.length, b.length)` — не больше каждой),
проверки вида `if (i >= a.length) return`, циклы с шагом `++`, `--` или постоянным `+= k`.
Индекс, значения которого видны по его виду, — маска, сдвиг, остаток, элемент байтового массива
(`t[x & 0xff]`, `hex[b >> 4]`, `table[bytes[i]]`), — в пределах, если его наибольшее значение
меньше длины. Факт забывается, когда то, о чём он, может измениться: присваивание, `pop()`,
вызов функции (она может изменить глобальные переменные, переменные, которые меняют замыкания,
и любой массив).
Переполнение целых — ошибка компиляции, поэтому `i + 1` всегда на единицу больше `i`. В тех же
правилах `pop()` и `shift()` дают `T`, если массив заведомо не пуст, а `charCodeAt` за пределами
строки — `int | undefined`, как `s[i]` (JS даёт `NaN`).

**Запись** `a[i] = x` разрешена, если доказано `i <= a.length` (`a[a.length] = x` добавляет
элемент, как в JS); дальше в JS остались бы дыры, поэтому без доказательства — ошибка
компиляции с подсказкой проверить индекс или использовать `push()`.

### Доказательства вместо падений

Где JS бросает `RangeError` или `TypeError` от значения вне границ, tz требует, чтобы значение
было доказано в границах при сборке ([errors.md](docs/errors.md), раздел 7). Доказательство —
по тем же фактам, что у индексов: константы, проверки в `if`, длины, маски, `Math.min` /
`Math.max`, индекс колбэка `map` и других (не меньше 0). Ошибка называет проверку, которой
хватит:

```ts
function pad(text: string, width: int) {
  const room = width - text.length
  if (room <= 0) return text
  return " ".repeat(room) + text          // без проверки — ошибка: room может быть < 0
}
function word(b: Buffer, at: int) {
  if (at < 0 || at > b.length - 4) return undefined
  return b.readUInt32LE(at)               // без проверки — ошибка
}
```

- **Не меньше нуля**: длина `new Array<T>(n)`, `a.length = n`, `new Uint8Array(n)`,
  `new ArrayBuffer(n)`, `Buffer.alloc(n)`, `allocUnsafe(n)`, `Buffer.concat(list, n)`; счёт
  `repeat(n)`, `take(n)`, `drop(n)`. У массива массивов, функций и объектов `a.length = n` ещё и
  не больше прежней длины (новым элементам нечего взять).
- **В границах**: `toFixed(n)` — 0…100, `toString(radix)` — 2…36, `String.fromCodePoint(x)` —
  целое 0…0x10FFFF.
- **Байты** ([bytes.md](docs/bytes.md)): смещения `read…` / `write…` у `Buffer` и `get…` /
  `set…` у `DataView` (`0 <= at <= b.length - 4` для 32 бит), `readUIntLE(at, n)` с `n`
  числом 1…6, `set(src, at)` (`at + src.length <= u.length`), `with(i, x)`, `copy`, `compare`,
  `write`, `fill`, виды `new Uint32Array(buffer, offset, length)` (смещение — число, кратное
  размеру элемента), `new DataView(buffer, offset, length)`, `Buffer.from(buffer, offset,
  length)`. `new Uint8Array(x.buffer, x.byteOffset, x.byteLength)` всегда в границах.
  Длина `const`-массива байтов известна везде, где он виден; сумма двух слагаемых
  (`at + src.length <= u.length`) — тоже факт.
- **Пустое — `undefined`**: `reduce` и `reduceRight` без начального значения (у массивов,
  типизированных массивов и итераторов), `Math.min(...xs)` / `Math.max(...xs)` и
  `decimal.min(...xs)` / `decimal.max(...xs)` дают `T | undefined`, пока не доказано, что массив
  не пуст. `Math.min()` и `Math.max()` без аргументов — ошибка.
- **`x!` нет** — проверка `if (x == undefined) return …` или `x ?? значение`.
- Код библиотеки tz (`library/*.tz`) повторяет алгоритмы node, где границы следуют из логики, а не
  из проверок; там недоказанное проверяется при выполнении, и выход за границы — внутренняя
  ошибка tz: сообщение и код 2.
- Где tz не бросает вовсе: `swap16` / `swap32` / `swap64` у буфера некратной длины переставляют
  целые группы и оставляют хвост, `encodeURIComponent` и `encodeURI` пишут одиночный суррогат
  как U+FFFD (`%EF%BF%BD`).

## Объекты

Объект — ссылка, как в JS, на блок в куче: счётчик ссылок, указатель на описание и поля без
упаковки (`int` — 8 байт, `int32` — 4, `uint8` — 1, `double` — 8, `boolean` — 1, строка — 16,
ссылки — 8). Порядок полей
в памяти выбирает компилятор — по выравниванию, без дыр, поля с функциями последними, а поля
родителя и поля литеральных типов (метки объединений, см. «Объединения») первыми (см.
«Наследование и интерфейсы»), — поэтому чтение поля — одна загрузка по
постоянному смещению; прототипов и словаря свойств нет. `{ x: 1.5, y: 2.5 }` — блок в 32
байта. `Point | undefined` — та же ссылка или ноль.

Описание — константа в exe: функция освобождения, а для печати — имена полей, их смещения и
порядок ключей. Порядок ключей берётся из литерала, который создал объект: node печатает ключи в
порядке добавления. Объект освобождается вместе с тем, что держат его поля; длинный список
(`next?: Node`) освобождается циклом, без рекурсии, на любой длине.

Типы точные: объект подходит к типу, только если у него ровно те же поля тех же типов, а
одинаковый набор полей — один и тот же тип, как бы он ни назывался (`interface`, `type` или
литерал). Тип полей литерала берётся из места, где он стоит:

```ts
interface Point { x: double; y: double }
function len(p: Point) { return Math.hypot(p.x, p.y) }
const p: Point = { x: 1, y: 2 }   // x и y — double
len({ x: 3, y: 4 })               // тоже: параметр len — Point
const q = { x: 1, y: 2 }          // тоже Point: q дальше идёт в len
len(q)
console.log(q.x / 2)              // 0.5
const r = { x: 1, y: 2 }          // { x: int; y: int }: никуда с типом не идёт
```

Если у литерала нет типа там, где он написан, компилятор смотрит, куда переменная идёт дальше в
своём блоке (и в функциях, если она глобальная): в аргумент функции, известной по имени, в
переменную, поле или элемент массива с типом, в `push`, в `return` функции с объявленным типом,
в присваивание `q = point`. Если эти места согласны, литерал получает их тип, как будто он был
написан в объявлении: так же целые литералы становятся `double`, необязательных полей можно не
писать, а `[1, 2]`, который идёт в `double[]`, — массив `double`. Если одно место ждёт
`{ x: int }`, а другое `{ x: double }`, — ошибка у одного из них с указанием на другое. Так же
одна ветка `?:` даёт тип другой (`flag ? { x: 1, y: 2 } : p`), литерал в массиве — тип элементов
перед ним (`[p, { x: 5, y: 6 }]`), `return` литерала — тип объекта из прошлых `return`, а колбэк
`map` — тип элементов массива, который ждут от `map`.

**Spread** `{ ...user, name: "Anna" }` создаёт новый объект, его тип — итоговый набор полей
(здесь снова `User`). Части вычисляются по порядку, и более поздняя заменяет значение поля, но не
его место среди ключей. Необязательное поле, в котором `undefined`, не копируется:
`{ ...defaults, ...options }` сохраняет значения по умолчанию, которых нет в `options` (в JS явный
`undefined` их затирает). **Деструктуризация** читает поля по имени, остаток (`...rest`) — новый
объект без названных полей.

**Ключи в скобках** (`{ [key]: v }`, `o[key]`) должны быть известны при компиляции: строковый
литерал, константа с ним (`const key = "name"`) или член строкового `enum`. Тогда `o[key]` — то
же, что `o.name`. Ключи, которые вычисляются при выполнении, — это словарь (`Record`, см.
«Словари»).

**`Object.keys` / `values` / `entries` и `for...in`** перебирают поля, в которых есть значение,
в том порядке, в каком объект их печатает. У `values` и `entries` все значения попадают в один
массив, тип его элементов — объединение типов полей (`{ a: 1, b: "x" }` даёт `(int | string)[]`,
`limit?: int` рядом с `int` — просто `int`). `entries` даёт массив кортежей `[string, T][]`.

**Кортеж** `[string, int]` — массив фиксированной длины, где у каждого места свой тип. Внутри это
объект с полями `0`, `1`... (ссылка, как массив в JS), печатается как массив. Номер элемента —
только константа (`t[1]`), `t.length` известен при компиляции; `push`, `pop`, `splice`, запись
`length` и методы массивов — ошибки: кортеж разбирают деструктуризацией или копируют в массив
`[...t]` (если у элементов один тип). Литерал `["Ann", 30]` становится кортежем там, где ждут
кортеж (в том числе в переменной, которая потом идёт туда).

**Сужение полей.** Поле типа `T | undefined` после проверки (`if (q.limit != undefined)`,
`if (node.next)`, `while (cur.next) cur = cur.next`) имеет тип `T`, как переменная.
Компилятор забывает это, когда поле могло измениться: при присваивании полю с тем же именем у
любого объекта, при присваивании переменной, от которой идёт цепочка полей, и при вызове функции,
если где-то в программе есть присваивание полю с этим именем. Поэтому в коде, который только
строит деревья литералами и читает их, `return 1 + check(t.left) + check(t.right)` после
`if (t.left == undefined || t.right == undefined) return 1` компилируется как есть.

## Классы

Класс — тип объекта, который отличается от других по имени: `Point` и `Size` с одинаковыми полями
— разные типы, а литерал `{ x: 1, y: 2 }` никогда не становится `Point` (экземпляр создаёт только
`new Point(...)`). Экземпляр устроен как любой объект: поля без упаковки по постоянным смещениям, а
описание в начале объекта ещё называет класс и даёт его номер. Прототипов нет: методы принадлежат
классу, вызов `p.length()` — прямой вызов функции, которую LLVM может встроить; метод получает
объект как `this` взаймы, без счётчика ссылок. Статические поля — глобальные переменные (их
значения задаются там, где стоит объявление класса), статические методы — обычные функции, и
хвостовые вызовы у них гарантированы, как у функций (у остальных методов — пока нет).
Объявляют классы на верхнем уровне файла (class-выражения — где угодно, см. «Классы как значения
и отражение»); их экспортируют и импортируют, как остальное.

```ts
class Counter {
  count = 0
  static created = 0
  constructor(public step: int) { Counter.created++ }
  inc() { this.count += this.step; return this.count }
}
const c = new Counter(2)
const inc = c.inc        // привязан к c: inc() работает (в JS this потерялся бы)
inc()
console.log(c, inc == c.inc)   // Counter { step: 2, count: 2 } true
```

- **Печать как в node**: `Point { x: 1, y: 2 }`, пустой — `Empty {}`, глубже двух уровней —
  `[Point]`. Параметры-свойства идут первыми, потом поля в порядке объявления. Приватные поля
  печатаются и попадают в `Object.keys` и `JSON.stringify`, как `private` у node: печать — для
  отладки, а что отдать наружу, решает свой метод (`toJSON()`). `typeof` — `"object"`, `==` — тот
  же ли это объект.
- **Поля получают значения до того, как объект используют.** Сначала начальные значения полей
  по порядку, потом параметры-свойства, потом тело конструктора — тот же порядок, что в JS. Поле
  без `?` должно получить значение при объявлении или в каждом пути каждого конструктора. Пока
  заданы не все поля, `this` можно только задавать поля и читать уже заданные: вызов метода,
  передача `this` куда-то или стрелка, которая его использует, — ошибка (как в Swift; в node
  такой метод увидел бы `undefined`). `x!: T` («задам потом») — ошибка, `x?: T` — пожалуйста.
- **Тип поля без аннотации** берётся из начального значения, как у `let`: `count = 0` — `int`,
  `items = [1, 2]` — `int[]`, `total = this.items.length + base` — `int`.
- **`private` и `protected`** проверяются при компиляции: снаружи класса — ошибка.
  `protected` открыт ещё и классам-наследникам. `#x` — ошибка с подсказкой `private x`
  ([objects.md](docs/objects.md), раздел 6): одна система модификаторов, и `private` работает в
  параметрах-свойствах. Подкласс не может объявить член с именем приватного члена родителя.
  `readonly` поле задаёт только конструктор класса, который его объявил.
- **Метод как значение привязан к своему объекту**: `onClick(counter.inc)` и
  `names.map(formatter.format)` работают, `c.inc == c.inc` — `true` (а `a.inc == b.inc` у разных
  объектов — `false`). Заменить метод нельзя: `s.save = ...` — ошибка с подсказкой
  объявить поле с функцией.
- **Стрелка в поле** (`onClick = () => { this.clicks++ }`), которую нигде не переприсваивают,
  компилируется как метод: в каждом объекте нет отдельной функции и нет цикла ссылок
  «объект → функция → объект». Печатается она, как в node, полем `onClick: [Function: onClick]`.
- **`instanceof`** — сравнение номера класса в описании объекта (с наследниками — см.
  «Наследование и интерфейсы»). `x instanceof C` для `C | undefined` — проверка «есть ли» и
  сужает тип; для объекта класса, который не связан с `C` наследованием, или литерала — ошибка
  «всегда ложь».
- **Геттеров и сеттеров нет**: чтение поля никогда не выполняет код, поэтому `get area()` и
  `set width(v)` — ошибки с подсказкой написать методы `area()` и `setWidth(v)`. `toString`
  — обычный метод: `` `${price}` `` — ошибка с подсказкой `` `${price.toString()}` ``.
- **Перегрузка** ([overloads.md](docs/overloads.md)): у методов, статических методов и
  конструкторов может быть несколько вариантов с одним именем, у каждого своё тело. Вызов выбирает
  вариант по типам аргументов при компиляции, без неявных приведений чисел
  ([numbers.md](docs/numbers.md), раздел 1): переменная подходит только к параметру точно своего
  типа (`add(n)` при `n: int32` и вариантах `add(k: int)`, `add(k: double)` — ошибка с подсказкой
  `n.toInt()`); литерал сначала ищет вариант со своим типом (`5` — `int`, `1.5` — `double`),
  иначе — единственный, куда он помещается; таких несколько — ошибка с подсказкой уточнить
  литерал: при `scale(k: double)` и `scale(k: decimal)` вызов `scale(2)` — ошибка, `scale(2.0)` —
  `double`. Вариант без пропущенных параметров и rest лучше. Литерал с дробью идёт в `decimal`
  точно, если у вариантов на его месте есть `decimal`, но нет `double`: `m.times(1.5)` при
  `times(k: int)` и `times(k: decimal)`. Каждый вариант —
  отдельная функция со своим выведенным типом результата. Ни одного подходящего или два
  одинаково хороших — ошибка со списком вариантов; варианты, которые различаются только типом
  результата или одинаково подходят к одному вызову, — ошибка в объявлении. Метод с вариантами
  как значение выбирается по типу места: `const f: (k: double) => Vec = v.add`. Вариант может
  быть обобщённым (`show(x: int)` и `show<T>(x: T[])`): его параметры типа берутся из аргументов, и
  подходит он так, как подходят получившиеся типы; при прочих равных вариант без generics лучше
  (`exact(x: int)` и `exact<T>(x: T)` с `exact(1)` — первый), как в C#. node такой код
  выполняет неправильно (последнее тело затирает остальные), поэтому он работает только в tz.
  Декоратор на варианте получает этот вариант, `design:paramtypes` — его параметры. Отражение
  выбирает вариант при выполнении по тем же правилам, по тому, что лежит в аргументах:
  `Reflect.get(obj, "add")` даёт функцию, которая выбирает вариант при вызове (`2` — `add(k:
  int)`, `2.5` — `add(k: double)`, объект `Dog` — `pet(d: Dog)`, а не `pet(a: Animal)`), так же
  `new cls(...)` и `Reflect.construct` выбирают конструктор; ни одного подходящего или два
  одинаково хороших — `Err` (`ERR_SEVERAL_VARIANTS: Reflect.apply: the arguments fit several
  variants of Vec.label equally well`).

```ts
class Vec {
  constructor(public x: double, public y: double) {}
  constructor(p: Vec) { this.x = p.x; this.y = p.y }
  add(v: Vec) { return new Vec(this.x + v.x, this.y + v.y) }
  add(k: double) { return new Vec(this.x + k, this.y + k) }
}
new Vec(1, 2).add(new Vec(3, 4))   // add(v: Vec)
new Vec(1, 2).add(2)               // add(k: double)
```

**Методы в литералах.** `{ n: 0, inc() { this.n++ } }` — экземпляр своего, безымянного класса:
`this` — сам объект, метод как значение тоже привязан к нему, печатается литерал как в node:
`{ n: 1, inc: [Function: inc] }`. Методы видят переменные вокруг литерала, как замыкания;
захваченные значения хранятся в самом объекте, после его полей. Тип такого литерала свой у
каждого места в коде: два литерала с методами из разных мест нельзя положить в один массив,
если там не ждут интерфейс с этими полями и методами (тогда литерал его реализует, см. ниже).

## Наследование и интерфейсы

```ts
abstract class Shape {
  constructor(public name: string) {}
  abstract area(): double
  describe() { return `${this.name}: ${this.area().toFixed(2)}` }
}
class Circle extends Shape {
  constructor(public r: double) { super("circle") }
  area() { return Math.PI * this.r ** 2 }
}
class Rect extends Shape {
  constructor(public w: int, public h: int) { super("rect") }
  area() { return this.w * this.h }   // double: так объявлено в Shape
}
const shapes: Shape[] = [new Circle(1), new Rect(2, 3)]
for (const s of shapes) console.log(s.describe())   // circle: 3.14, rect: 6.00
```

Подтипы — только объявленные ([objects.md](docs/objects.md), раздел 2): класс — это ещё и класс,
который он расширяет (`extends`, один), и интерфейсы, которые он реализует (`implements`);
интерфейс — ещё и те, что он расширяет (`interface User extends Named`). Объект с теми же полями,
но без такого объявления, к типу не подходит, как и раньше.

- **Поля родителя лежат в начале объекта** в том же порядке, поэтому поле читается одной загрузкой
  и через тип родителя. Печать — как в node: `Dog { name: 'Rex', breed: 'lab' }`, сначала поля
  родителя (поле, объявленное в наследнике заново, остаётся на своём месте).
- **Сужение поля.** Наследник может объявить поле родителя заново с типом объектов ниже по цепочке
  (`pet: Dog = ...` вместо `pet: Animal`) или сделать его `readonly`; так же интерфейс, который
  расширяет другой, и класс, который реализует интерфейс. `Dog[]` подходит туда, где ждут
  `Animal[]`. Запись через более широкий тип, которую не позволяет свой (в `pet` — не `Dog`, в
  массив — не `Dog`), ищется по всей программе, и метод родителя, вызванный у наследника, тоже её
  делает; найденная — ошибка у вызова или присваивания со строкой записи ([objects.md](docs/objects.md),
  раздел 2; подробнее — в «Чем отличается от TypeScript»).
- **Конструктор.** В классе-наследнике `super(...)` — отдельная инструкция в теле конструктора,
  один раз; до неё `this` нельзя. Вариант конструктора родителя выбирается по аргументам, как у
  `new`. Сразу после `super(...)` получают значения поля наследника с начальными значениями, потом
  параметры-свойства — порядок как в JS. Класс без своего конструктора берёт варианты ближайшего
  родителя, у которого они есть. Правило «пока заданы не все поля» действует и здесь: поля
  родителя заданы `super(...)`, свои — как в любом конструкторе.
- **Переопределение.** Метод с тем же именем и теми же параметрами, что у родителя, его
  переопределяет — с `override` или без, как в TS (`override` без такого метода — ошибка). Метод с
  другими параметрами — ещё один вариант имени, варианты родителя участвуют в выборе
  ([overloads.md](docs/overloads.md), раздел 4). Тип результата у переопределения тот же, что у
  метода родителя: без аннотации он берётся оттуда (`Rect.area` выше возвращает `double`);
  объект можно вернуть более узкого класса (`Dog` вместо `Animal`). Абстрактный метод без
  аннотации получает тип первой реализации. `super.m()` вызывает метод родителя напрямую.
- **Вызов.** Если метод не переопределяет ни один класс, к которому может относиться объект, вызов
  прямой и встраивается, как без наследования. Иначе код берётся из таблицы методов в описании
  класса объекта: две загрузки и косвенный вызов (`bench/shapes` — 136 мс против 150 мс у Go с
  интерфейсами). Абстрактный метод с одной реализацией вызывается напрямую. Метод как значение
  берёт из таблицы и тело класса объекта.
- **`instanceof`.** Классы пронумерованы так, что наследники класса идут сразу за ним, и проверка —
  одно сравнение номера с диапазоном. Она сужает переменную: в `if (a instanceof Dog)` у `a`
  есть поля и методы `Dog`. Dog и Cat вместе дают их общего родителя: `flag ? dog : cat` —
  `Animal`.
- **Конструктор родителя не может вызывать метод, который переопределяет наследник**, ни прямо, ни
  через свои методы: поля наследника ещё не заданы (в node — `undefined` или `TypeError`). Это
  ошибка в родителе.
- **`abstract`.** Абстрактный класс нельзя создать через `new`; класс, который можно создать, должен
  реализовать все абстрактные методы своих родителей.
- **Статические члены** наследуются: `Dog.create()` — статический метод `Animal`.

**Интерфейсы.** Класс, который реализует интерфейс, должен иметь все его поля с теми же типами
(поле, которое интерфейс разрешает менять, не может быть `readonly`) и все его методы с теми же
параметрами; метод без аннотации результата получает тип из интерфейса. Метод интерфейса
(`greet(other: string): string`) — поле с функцией, которое через интерфейс нельзя заменить, как
написано в [objects.md](docs/objects.md), раздел 5: его реализует и метод класса, и функция в поле
литерала (`{ name: "Lit", greet: o => ... }`), и метод литерала (`{ name: "Lit", greet(o) {...} }`
— параметры получают типы из интерфейса). Поэтому литералы с методами из разных мест кладутся в
один массив `Greeter[]`. Метод с несколькими сигнатурами (`add(k: double): Vec; add(v: Vec): Vec`)
реализует только класс — каждый своим вариантом; вызов выбирает сигнатуру по аргументам.

Код, который знает объект по интерфейсу, читает поле прямо, если у всех типов, реализующих
интерфейс, оно лежит там же, где у самого интерфейса (класс, который реализует интерфейс и ничего не
расширяет, кладёт его поля первыми). Иначе смещение и код метода берутся из таблицы интерфейса,
которую даёт описание объекта: на одну-две загрузки больше. У объекта, где метод — функция в поле,
в таблице маленькая заглушка, которая её вызывает.

## Циклы ссылок

Счётчик ссылок сам не освобождает объекты, которые держат друг друга по кругу. Сборщика мусора
нет: компилятор находит такие объекты заранее, а рантайм освобождает их группами
([objects.md](docs/objects.md), раздел 11).

```ts
class TreeNode {
  children: TreeNode[] = []
  parent?: TreeNode
}
function grow() {
  const root = new TreeNode()
  const kid = new TreeNode()
  kid.parent = root          // root и kid — одна группа
  root.children.push(kid)    // и массив children тоже
}                            // здесь освобождаются все трое
```

- **Какие типы.** По всей программе строится граф: что может держать что — поля объектов,
  элементы массивов, ключи и значения словарей, переменные, захваченные замыканиями, наследники и
  реализации интерфейсов, члены объединений, `unknown`, экземпляры обобщений. Тип на цикле этого
  графа, где хотя бы одна ссылка ставится после создания объекта (присваивание поля, `push`,
  `set`), — циклический. Поле, которое задаёт только конструктор, указывает на объекты, созданные
  раньше, и цикла не даёт: список из `new Node(value, next)` и binary-trees остаются обычными
  объектами, и в их IR нет ни одного вызова групп.
- **Группы.** Объект циклического типа (и массив, словарь, окружение замыкания, коробка
  переменной) — член группы: перед ним 16 байт, вожак группы и следующий член кольца. У группы
  один счётчик — ссылок снаружи; ссылки между её членами не считаются. Запись ссылки на члена
  другой группы сливает группы: меньшая присоединяется к большей за время своего размера. Группы
  не делятся. Когда снаружи ссылок не остаётся, вся группа освобождается сразу и без рекурсии.
- **Что держит членов.** Тип, который держит членов и сам может лежать в члене (массив `T[]` в
  объекте, который держит и сам `T`, источник потока с замыканиями в состоянии потока), тоже
  член: обычный объект считал бы свою ссылку снаружи, и группа не освободилась бы никогда. Тот,
  кого не держит ни один член (локальный массив узлов), остаётся обычным объектом, и группы
  остаются размером с сами циклы. В программе с циклами членами бывают и `Promise` (с кадром
  async-функции и тем, чем он разрешён), реакции `then`, коробки `unknown` и обёртки функций,
  которые меняют тип результата.
- **Что группы не сливает.** Ожидание: кадр, который ждёт `Promise`, не сливается с его группой
  (реакция держит кадр счётчиком, если они ещё не в одной группе), — иначе каждый `Promise`,
  которого дождался долгий цикл `await`, жил бы, пока жив цикл. Замыкание, которое сразу отдано
  `setTimeout`, `setInterval`, `setImmediate`, `queueMicrotask`, `process.nextTick`, `then` или
  `new Promise`, членом не бывает: таймер или реакция держат его счётчиком и отпускают после
  вызова, а то, что оно захватило (`this`, кусок данных), не входит из-за него в чужую группу.
- **Вечные группы.** Глобальная переменная, которой больше ничего не присваивают, держит свою
  группу всё время работы: ссылки на неё не считаются и ни с чем её не сливают. Поэтому объекты,
  которые ссылаются на реестр программы, не попадают в его группу.
- **`WeakRef<T>`**: `new WeakRef(obj)`, `ref.deref()` даёт `T | undefined`. Слабая ссылка не
  держит объект и не сливает группы; она становится `undefined` ровно тогда, когда объект умер.
- **Слой 1.** Объект, про который компилятор доказал, что его никуда не записывают (ни в поле, ни
  в элемент массива или словаря, ни в замыкание), создаётся обычным объектом, даже если его тип
  циклический: временные узлы, которые только читают, групп не создают.
- **Слой 2: владелец и обратная ссылка.**
  ```ts
  class Dialog {
    ok = new Button("OK")
    constructor() {
      this.ok.onClick = () => this.close()   // кнопка держит диалог
    }
    close() {}
  }
  ```
  Если `ok` задаётся только при создании `Dialog` и не читается как значение (только
  `d.ok.click()`), а `Button` не отдаёт свой `this` и `onClick` только вызывает, то стрелка
  держит `Dialog` без счёта: ни `Dialog`, ни `Button` не становятся группами. Пока только для
  стрелок и функций, не для методов, взятых как значения.
- **Отладочный режим** — в программе, собранной с `TZ_CHECK_LEAKS=1` (так собираются тесты).
  Группа, которая доросла до 4096, 8192... членов, обходится, и программа сообщает, сколько её
  членов уже не достать снаружи и что их держит:
  `tz: group of Player: 4094 of 4097 objects cannot be reached, held by Player.opponent at
  main.tz:20; ...`. Доказательства слоёв 1 и 2 проверяются во время работы
  (`tz: debug check failed: ...`), а в конце печатаются оставшиеся группы с типами членов.
- **Скорость.** Группа из 10 млн узлов со строкой освобождается за 190 мс, если узлы идут в
  кольце подряд по памяти, и за 1,3 с, если разбросаны (1 млн — 20 мс, 100 тыс. — 1 мс).
  Освобождение — два прохода: сначала то, что держат члены (ссылки внутри группы пропускаются),
  потом память; второй проход идёт по 16 цепочкам кольца сразу, чтобы процессор загружал узлы
  параллельно, а не ждал каждый (на разбросанных 10 млн это вдвое быстрее). Цифры с Go —
  `cycles_list` и `cycles_tree` в [bench](bench/README.md).

Ограничения:

- Части, которые отцепили, держатся до смерти всей группы: игроки, которые сыграли друг с другом
  через `opponent`, остаются в одной группе, пока жив хоть один. Отладочный режим находит такие
  связи; лечится `WeakRef` или очисткой поля.
- Большие группы освобождаются на месте, а не в фоновом потоке, как в docs/objects.md: для этого
  нужны списки блоков, освобождённых чужим потоком, в аллокаторе.
- Замыкание, которое отдают не прямо таймеру, а дальше (колбэк в чужую функцию) и которое
  захватывает объект долгой группы, — её член до её смерти: колбэк на каждый кусок потока копил
  бы память. Библиотека потоков таких не создаёт; в своём коде то, что меняется на каждом шаге,
  лучше держать в поле, а замыкание — одно.
- Итераторы, генераторы и таймеры не бывают членами групп: цикл через то, что они держат,
  освобождается, только когда они заканчиваются. Так же локальные переменные незаконченной
  async-функции считаются снаружи её группы: цикл через них освобождается, когда она закончится.
- `WeakRef` — только объектов классов, интерфейсов и литералов; массивов, словарей, дат,
  регулярок и байтов пока нет. `FinalizationRegistry` нет.

## Декораторы

```ts
function Table(name: string) {
  return (cls: object) => Reflect.defineMetadata(Table, name, cls)
}

function Log<A extends unknown[], R>(_cls: object, name: string, method: MethodDescriptor<A, R>) {
  const original = method.value
  method.value = function (this: unknown, ...args: A): R {
    const result = original.apply(this, args)
    console.log(`${name}(${args.join(", ")}) → ${String(result)}`)
    return result
  }
}

@Table("users")
class User {
  constructor(public name: string) {}
  @Log greet(who: string) { return `${this.name} greets ${who}` }
}
new User("Ann").greet("Bob")           // greet(Bob) → Ann greets Bob
Reflect.getMetadata(Table, User)        // "users", тип string | undefined
```

Синтаксис и порядок — как у `experimentalDecorators` в TS ([decorators.md](docs/decorators.md)),
флага нет. Декораторы выполняются, где стоит объявление класса, сразу после того, как статические
поля получили значения: сначала члены объектов в порядке объявления, потом `static`-члены, потом
класс с параметрами конструктора. У каждого объявления выражения декораторов вычисляются сверху
вниз, а применяются снизу вверх, декораторы его параметров — первыми.

- **Что получает декоратор.** Всегда сам класс, и у членов тоже (в TS декоратор поля и метода
  получает прототип, а прототипов в tz нет: `User.prototype` и `target.constructor` — ошибки с
  подсказкой). Класс — `(класс)`, поле — `(класс, имя)`, метод — `(класс, имя, описание)`,
  параметр — `(класс, имя метода или undefined у конструктора, номер)`. Декоратор берёт столько из
  этого, сколько у него параметров; если ему нужно больше, чем дают на этом месте, или третьим он
  ждёт описание метода там, где дают номер параметра (и наоборот), — ошибка «не на своём месте».
  Декоратор с параметрами (`@Table("users")`) — функция, которая возвращает декоратор.
- **Каждое применение — отдельно**, как вызов обобщённой функции: у `@Log` над
  `add(a: int, b: int)` своя копия `Log` с `A = [int, int]`, `R = int`.
- **Описание метода** — `MethodDescriptor<A, R>`: `value` типа `(this: unknown, ...args: A) => R`,
  `writable`, `enumerable` (false), `configurable`. Если декоратор метода его берёт, вызовы метода
  идут через то, что оставлено в `value`: метод становится заглушкой, которая вызывает это значение
  с объектом (у `static` — с классом) как `this`, — через класс, таблицу методов, интерфейс и
  метод как значение. Сначала там лежит сама функция метода, она проверяет, что `this` — объект его
  класса (иначе `TypeError`). Декоратор может вернуть описание — оно и остаётся. Вызов декоратора,
  которому описание не нужно, ничего не стоит методу. У async-метода `R` — его `Promise<T>`, и
  обёртка может быть async (`MethodDescriptor<A, Promise<R>>`); переопределённый метод
  декорируют отдельно, а `super.m()` идёт через декораторы метода родителя.
- **Функции с `this`.** `function (this: unknown, ...args: A): R` получает `this` первым
  параметром; вызывают её `f.call(obj, ...)` и `f.apply(obj, args)`, простой вызов — ошибка.
- **Метаданные** — как в пакете reflect-metadata, но встроенные. Ключ-объект — функция (обычно сам
  декоратор), класс или константа: тип записей по нему — тип первой записанной, по всей программе
  (функции, которые пишут, проверяются раньше, если запись читают до них; или тип даёт место, куда
  идёт чтение: `const names: string[] = Reflect.getOwnMetadata(Column, cls) ?? []`). Ключ-строка —
  записи `unknown`. Цель — объект; `getMetadata` ищет и выше по цепочке: у объекта — в его классе,
  у класса — в классе, который он расширяет. `design:type` (класс поля, у метода — `Function`),
  `design:paramtypes` (классы параметров метода или конструктора) и `design:returntype` компилятор
  пишет для каждого декорированного члена, до его декораторов: `Number`, `String`, `Boolean`,
  `Array`, `Function`, `Date`, `RegExp`, `Map`, `Set`, `Promise`, `Uint8Array` и другие байтовые,
  `Object` или сам класс (у объединения — общий класс его членов, иначе `Object`, у литерального
  типа — класс основы, как в TS); читаются они как `Constructor | undefined` и `Constructor[] |
  undefined` (DI-контейнер делает по ним объекты параметров конструктора: `Reflect.construct`), а
  записывать их программе нельзя.
- **Подмена класса** ([decorators.md](docs/decorators.md), раздел 5). Декоратор класса может
  вернуть класс, который расширяет полученный, — обычно class-выражение обобщённой функции
  `<T extends Constructor>(cls: T)`. С места объявления имя означает этот класс: `new Order(100)`
  делает его, и `.id`, которое он добавил, видно и проверяется; тип `Order` в аннотациях, и в
  сигнатурах, разобранных раньше, — его тип (а `this` в методах самого `Order` — класса, как он
  объявлен); класс, объявленный с `extends Order`, расширяет его, как в JS. Декораторы класса
  применяются снизу вверх, и следующий получает класс, который дал предыдущий.
  `ConstructorArgs` — параметры конструктора расширяемого класса: `constructor(...args:
  ConstructorArgs) { super(...args) }`. Вернуть полученный класс можно; другой класс,
  `Constructor` (класс, известный только при выполнении) — ошибки, как и статическое поле, которое
  хранит объект класса, сделанный до подмены.
- **Все записи — на самом классе**, по имени члена, поэтому `static`-член и член объектов с одним
  именем (или два варианта перегрузки), оба с декораторами, — ошибка; так же два конструктора с
  декорированными параметрами: `design:paramtypes` класса был бы один на оба.
- **Обобщённый класс** декорируется один раз, как сам класс (в TS он и есть один): цель —
  `Stack`, а не `Stack<int>`, а `design:*` — по объявлению, где параметр типа — `Object`, `T[]` —
  `Array`. Декоратор, который берёт описание метода обобщённого класса, пока ошибка: у каждого
  экземпляра свой метод со своими типами.
- **Ошибки:** декоратор у функции, переменной, литерала, конструктора, абстрактного метода
  и поля `accessor` (оно в tz — обычное поле, запись в него не выполняет код),
  у параметра обычной функции; декоратор поля или параметра, который что-то возвращает.

## Классы как значения и отражение

```ts
const Kind = Dog                                  // typeof Dog: сам класс
new Kind("rex").sound()                           // как new Dog("rex")
const kinds: Constructor<Animal>[] = [Dog, Cat]   // классы, известные только при выполнении
for (const k of kinds) console.log(k.name, new k("pet").sound())

let clock = 100
function Timestamped<B extends Constructor>(Base: B) {
  return class extends Base { stamp = clock++ }   // примесь
}
const TimedUser = Timestamped(User)
new TimedUser("ann").stamp                        // 100, int

const controller = new route.cls()                // route.cls: Constructor; так можно, если
                                                  // каждый класс, что может тут быть, берёт ()
const method = Reflect.get(controller, route.method)
if (typeof method == "function") Reflect.apply(method, controller, [])   // Result<unknown, …>
```

Решения — в [decorators.md](docs/decorators.md), разделы 5 и 6.

- **`typeof C`** — сам класс, известный при компиляции, где бы он ни лежал: в переменной, в
  параметре (`kind: typeof Cat`), в поле. `new` от него — `new C(...)` с проверкой аргументов
  при компиляции, `K.count`, `K.count = 1` и `K.create()` — статические члены `C`, `K.name` —
  имя. Другой класс в такую переменную не кладётся.
- **`Constructor<T>`** — какой-то класс, объекты которого — `T`; просто `Constructor` — любой
  класс. В него превращается класс, объекты которого — `T` (сам `T`, наследник, класс, который
  реализует интерфейс `T`); `[Dog, Cat]` — `Constructor<Animal>[]`. Его хранят в массивах и
  ключах `Map`, сравнивают, печатают (`[class Dog extends Animal]`), берут `name`; `new k(...)` и
  `x instanceof k` (одно сравнение номеров классов; сужает `x` до `T`). `typeof` класса —
  `"function"`, как в JS. `InstanceType<typeof K>` — тип объектов класса `K`. Обобщённый класс
  (`Box<T>`, `EventEmitter`) — не `Constructor`: каждый список аргументов типа делает свой класс, и
  `new` такого значения не сделал бы ни одного; подойдёт класс, который его расширяет
  (`class IntBox extends Box<int> {}`, `class Bus extends EventEmitter {}`), или `object`.
- **class-выражения**: `const Point = class { ... }` (имя — у константы, как в JS),
  `class Inner { ... }`, `class extends Base { ... }` — где угодно, и в функциях. Выражение
  делает один класс, сколько бы раз оно ни выполнялось (в JS — новый каждый раз), поэтому его
  статические поля получают значения при первом выполнении, а переменные функции вокруг ему
  недоступны (ошибка с подсказкой передать значение в конструктор). Объект класса без имени
  печатается именем ближайшего класса с именем выше по цепочке, как в node, а класс, чей
  `extends` — вызов (`class A extends Mixin(B)`), печатается как `[Function: A]`, как там же.
- **Примеси.** Обобщённая функция с class-выражением — шаблон: её тело проверяется не один раз
  обобщённо, а в каждой копии, каждая копия делает свой класс (`Timestamped(User)` расширяет
  `User`, `Timestamped(Post)` — `Post`) и сама находит свой тип результата; ошибка в копии —
  у вызова, которому она нужна. Класс, который расширяют, должен быть известен при компиляции:
  поля объекта лежат на постоянных местах, и `class extends cls` с `cls: Constructor` — ошибка.
  `B extends Constructor<User>` даёт телу члены `User`: переопределение и `super.greet()`
  работают, и через тип `User` вызывается метод примеси. Метод с именем, которое объявляет
  какое-нибудь class-выражение программы, поэтому вызывается через таблицу методов. Объявление с
  вызовом в `extends` (`class Moderator extends Timestamped(User) {}`) делается там, где
  выполняется.
- **Отражение** — для классов, известных только при выполнении. `Reflect.construct(k, args)`
  проверяет аргументы по конструктору (у нескольких вариантов — по тому, который подходит лучше,
  см. «Перегрузка» в «Классах»); `Reflect.get(obj, name)` даёт поле как `unknown`, метод — как
  функцию `(this: unknown, ...)`, которая проверяет свой `this` (у метода с вариантами —
  функцию, которая выбирает вариант при вызове), а чего нет (и у того, что не объект) —
  `undefined`; `Reflect.apply(f, thisArg, args)` вызывает функцию из `unknown`, проверяя
  аргументы по её параметрам. `Reflect.apply` и `Reflect.construct` отдают `Result<T,
  ReflectError>`: не тот аргумент, не столько аргументов, не тот `this`, абстрактный класс —
  `Err` с `code` (`"ERR_INVALID_ARGUMENT"`, `"ERR_INVALID_ARITY"`, `"ERR_INVALID_THIS"`, `"ERR_ABSTRACT_CLASS"`, `"ERR_NO_VARIANT"`,
  …) и объяснением: `new Point: argument 2: expected int, got 2.5`.
- **`new k(...)` не может не получиться**, как любой конструктор. Компилятор знает, какие классы
  становятся значениями этого `Constructor<T>` (и какие значения `Constructor<A>` где-то
  становятся `Constructor<T>`), и `new k(...)` компилируется, только если каждый из них берёт эти
  аргументы; иначе ошибка с подсказкой `Reflect.construct`. Так `new route.cls()` у маршрутов,
  где одни контроллеры с пустыми конструкторами, — можно, а `new cls()` DI-контейнера, через
  который идут и сервисы с параметрами, — нет.
  Аргументы — массив любых значений. Описание класса попадает в exe, только если программа
  ищет в нём (`new` класса из значения, `Reflect.get` метода по имени из строки), и только у классов, до которых это доходит: тех, что стали `Constructor`, и
  тех, чьи объекты стали `object` или `unknown` или попали в `Reflect.get`, с их наследниками.
- **Известный класс — без отражения** ([decorators.md](docs/decorators.md), шаг 5).
  `Reflect.construct(User, [a, b])` — это `new User(a, b)`, `Reflect.get(user, "name")` — чтение
  поля. Обобщённая функция, которая берёт `cls: Constructor<T>` или объект `T`, для
  `make(Point, ...)` получает копию, где `cls` — сам `Point`: `new cls(...)` и `x instanceof cls`
  в ней прямые, а аргументы конструктора проверяются при компиляции (ошибка
  — у вызова). `Reflect.get(obj, "greet")` у объекта известного класса даёт функцию метода без
  описания класса (метод с этим именем не объявляет ни один наследник), а `Reflect.apply` этой
  функции (из константы или прямо из `Reflect.get`) — прямой вызов `obj.greet(...)`, если `this`
  — объект этого класса, а аргументы — литерал массива, который проверки пропустили бы
  (`int` в `double`, объект класса-наследника); иначе проверки остаются, с теми же `Err`.
  `Reflect.construct(User, args)` с аргументами, известными только при выполнении, проверяет их
  функцией класса, тоже без описания. Так `container.get(UserService)` у `get<T>(cls:
  Constructor<T>)` с `new cls()` или `Reflect.construct` делает объект напрямую, и в exe нет
  описания `UserService` ([tests/cli/reflect_unfold.cmake](tests/cli/reflect_unfold.cmake) ищет
  его в `--emit-llvm`); `bench/di_known` — 103 мс против 579 мс у того же DI через описание
  (`bench/di_reflect`), exe — 184 КБ против 264 КБ.
- **`fromJson(cls, j)`** — убран 29 октября вместе с `fromJson<T>` и `JSON.parse<T>`
  ([narrowing.md](docs/narrowing.md), раздел 3): объект класса из `Json` программа делает сама —
  проверяет поля и зовёт конструктор.

DI-контейнер и маршрутизатор — библиотеки на tz поверх всего этого, не часть языка: пример — в
[tests/run/decorators_di.tz](tests/run/decorators_di.tz).

## unknown и object

`unknown` — значение любого типа, `undefined` тоже ([decorators.md](docs/decorators.md), раздел
4). Его можно хранить, передавать, сравнивать (`==` — как `===`), печатать (как node) и писать в
JSON; всё остальное — после проверки, которая сужает переменную:

```ts
function describe(v: unknown): string {
  if (v == undefined) return "nothing"
  if (typeof v == "string") return v.toUpperCase()   // string
  if (typeof v == "number") return `${v * 2}`         // double
  if (v instanceof Point) return `${v.length()}`      // Point
  if (Array.isArray(v)) return `${v.length} items`    // unknown[]
  return typeof v                                     // "boolean", "object", "function"
}
```

- В `unknown` превращается что угодно; условие, строка (`${v}`), арифметика, поля и методы до
  проверки — ошибки с подсказкой. `typeof v == "object"` даёт `object`, у которого потом
  проверяют класс. `unknown | undefined` — это `unknown`, как и любое объединение с `unknown`;
  функция типа `unknown` может просто закончиться. Значение объединения попадает в `unknown` тем,
  что в нём лежит (`int | string` — числом или строкой, литерал `"up"` — строкой).
- `instanceof Date`, `instanceof Uint8Array` (и других байтовых массивов) и `instanceof Buffer`
  сужают `unknown` до этого типа; `Promise`, `Map`, `Set` и регулярки лежат в `unknown` как
  объекты (печать — как в node), но `instanceof Map` и `instanceof Set` у `unknown` — ошибки:
  типы ключей и значений по объекту не узнать.
- `unknown[]`: `includes`/`indexOf` сравнивают то, что лежит внутри (как `===`); `join()` и
  `sort()` без функции сравнения — ошибки (объект стал бы `[object Object]`), ключ словаря
  типа `unknown` — тоже.
- `object` — любой объект языка: экземпляр класса, литерал, дата, словарь, итератор, класс как
  значение (массив и функция — не `object`). До `instanceof` у него нет ни полей, ни методов.
- **В памяти** `unknown` — ссылка: объект языка как есть, любое другое значение — в коробке
  рантайма с описанием своего типа, `undefined` — ноль. Превращение объекта в `unknown` ничего не
  стоит.

## Словари

```ts
const counts = new Map<string, int>()
for (const w of text.split(" ")) counts.set(w, (counts.get(w) ?? 0) + 1)
const byTeam: Record<string, string[]> = {}
for (const u of users) (byTeam[u.team] ??= []).push(u.name)
const tags = new Set(["ts", "go", "ts"])            // Set(2) { 'ts', 'go' }
console.log(counts.get("the"), byTeam.red, tags.union(new Set(["rust"])))
```

- **Одна хеш-таблица** рантайма ([map.c](runtime/core/map/map.c)) для `Record`, `Map` и `Set`: записи лежат
  массивом в порядке добавления, как в упорядоченных таблицах V8 и словарях CPython, удалённая
  запись остаётся дырой до уплотнения, а ключ находит индекс позиций с открытой адресацией.
  Словарь до 8 записей обходится без индекса и сравнивает хеши записей подряд. Хеш ключа хранится в
  записи и при росте таблицы не считается заново; ключи и значения лежат без упаковки, как элементы
  массива. Короткая строка-ключ (до 8 однобайтовых символов — она живёт в самом значении)
  хешируется одним умножением и сравнивается одним сравнением слов.
- **Типы**: `Map<K, V>`, `Set<T>`, `Record<string, T>` и `{ [key: string]: T }`; `ReadonlyMap` и
  `ReadonlySet` — те же типы (изменения не запрещаются, как у `ReadonlyArray`). `new Map()` берёт
  типы из места, где стоит (`const m: Map<string, int> = new Map()`, поле, `return`), а
  `new Map([["a", 1]])` и `new Set([1, 2])` — из значений. Обобщённые функции принимают и возвращают
  словари (`function countAll<T>(xs: T[]): Map<T, int>`), и объект-литерал, переданный туда, где
  ждут `Record<string, V>`, даёт тип `V`.
- **Ключи** сравниваются, как в `Map` JS (SameValueZero): строки и числа — по значению, `-0` —
  это `0`; объекты, массивы, функции и словари — по ссылке, `Json` — как `===`,
  `undefined` у `T | undefined` — тоже ключ. `decimal` — по значению, как его сравнивает `==`:
  `0.30` и `0.3` — один ключ.
- **`Record`** — словарь с синтаксисом объекта: `r[k]` и `r.k` дают `T | undefined`, `r[k] = v`,
  `r[k] ??= v`, `delete r[k]`, `k in r`, `Object.keys` / `values` / `entries`, `for...in`; литерал
  `{ a: 1, [k]: v, ...r, ...obj }` там, где ждут `Record`, а spread словаря и без типа делает
  `Record`. Остаток деструктуризации `const { a, ...rest } = r` — тоже словарь.
- **Перебор** — в порядке добавления; изменения во время перебора — как в JS: удалённые ключи
  пропускаются, новые попадают в перебор, ключ, удалённый и добавленный снова, идёт в конце,
  `clear()` заканчивает перебор старых. Пока идёт перебор или жив начатый итератор, записи не
  переезжают: уплотнение ждёт. `for...of`, spread, деструктуризация, `Array.from`, `yield*`,
  `new Set(x)` и цепочки (`m.keys().filter(...)`) — один цикл по записям, без объекта-итератора;
  `entries()` делает новую пару `[k, v]` на каждую запись, как JS. Итератор как значение
  (`const it = m.keys()`) — корутина, как у массива, и печатается как в node, с тем, что в нём
  осталось: `[Map Iterator] { 'b', 'c' }`.
- **Печать** как в node: `Map(2) { 'a' => 1, 'b' => 2 }`, `Set(1) { 1 }`, `Record` — как объект:
  `{ a: 1, 'b-c': 2 }` (у `Object.groupBy` — `[Object: null prototype] { ... }`); глубже двух
  уровней — `[Map]`, `[Set]`, `[Object]`; после ста записей — `... 20 more items`.
- **Память**: словарь освобождается вместе со всем, что держит, без рекурсии на любой глубине
  (бор в миллион уровней, словари словарей): словари, которые умирают, пока освобождается другой,
  ждут своей очереди.
- **Скорость**: `bench/wordcount` — 3,5 млн слов текста, 160 тыс. разных, в `Map<string, int>` —
  360 мс против 450 мс у Go с `map[string]int`; сам подсчёт — 58 мс против 84 мс (у tz дольше всего
  идёт `split`, см. [bench](bench)).

## Объединения

```ts
interface Circle { kind: "circle"; r: double }
interface Square { kind: "square"; side: double }
type Shape = Circle | Square
type Dir = "up" | "down"

function area(s: Shape) {
  switch (s.kind) {                            // без default: нужны и "circle", и "square"
    case "circle": return Math.PI * s.r ** 2   // здесь s — Circle
    case "square": return s.side ** 2
  }
}
function show(x: string | int) {
  return typeof x == "string" ? x.toUpperCase() : `${x + 1}`
}
```

- **Как лежит значение.** Строковый литеральный тип и объединение таких (`"up" | "down"`) — `int`,
  номер литерала среди строковых литералов программы: сравнение — сравнение чисел, текст берётся
  из постоянной таблицы строк, без выделения памяти. Числовые и логические литеральные типы
  (`0 | 1`, `true`) — значения своего типа. Объединение объектов (`Circle | Square`, классы) — та же
  ссылка, а какой член в ней, говорит описание объекта: номер класса, у объекта не класса — свой
  номер, выше номеров классов. Остальные объединения хранят метку — номер «места» члена: строка,
  строковые литералы, целые, `double`, `boolean`, `decimal`, объекты, массив, функция,
  итератор; он один во всех объединениях, поэтому переход из одного объединения в другое метку не
  меняет — и значение этого места в одном или двух словах: `{ i32 метка, i64, i64 }`, 16 байт
  (`int | boolean`, `Circle | int`) или 24, если среди членов строка, `decimal` или функция; куча
  не нужна. `undefined` в `A | B | undefined` — метка 0 (у ссылки — ноль, у литерала — номер 0),
  без отдельного флага.
- **Сужение** — по метке объединения объектов (`s.kind == "circle"`, `switch (s.kind)`,
  `if (r.ok)` у меток `true` и `false`), `x == "up"`, `typeof x == "string"`, `"radius" in s`,
  `x instanceof Dog`, `Array.isArray(x)`, в обеих ветках, у переменных и у полей
  (`slot.shape.kind == "circle"` — потом `slot.shape.r`). `||` объединяет: после
  `typeof j == "string" || typeof j == "number"` у `Json` тип `string | double`. Строка и целое
  сужаются сравнением с литералом: после `if (s == "decimal" || s == "percent")` у переменной
  типа `string` тип `"decimal" | "percent"`, после `if (code == 204)` у `int` — `204`; так
  значение из программы попадает туда, где ждут литеральный тип. Присваивание тоже
  сужает: после `let s: Shape = { kind: "circle", r: 1 }` у `s` есть `s.r`, пока ей не присвоят
  другое. Факт о поле забывается по тем же правилам, что «поле не `undefined`» (см. «Объекты»).
- **Поле, которое есть у всех членов**, читается через объединение: одна загрузка, если у всех оно
  на одном месте, иначе место выбирается по номеру класса. Так же у байтовых массивов:
  `byteLength`, `byteOffset`, `buffer` и `length` (`d.byteLength` у `Uint8Array | DataView |
  Buffer | ArrayBuffer`), если они есть у каждого члена. Поле литерального типа, метка, лежит
  первым, сразу за заголовком объекта, поэтому `s.kind` — всегда одна загрузка, а `switch` по нему
  — `switch` по числам. Метод, который есть у всех членов,
  вызывается через класс или интерфейс, которым они все объявлены, а если такого нет — у каждого
  свой, по номеру класса. Записать можно поле одного типа у всех членов.
- **`switch` по литеральному типу** или по метке объединения без `default` разбирает все
  значения, иначе ошибка: `the switch does not handle "left"`. Тогда после него не нужен
  `return`, и тип результата функции не получает `undefined`. В `default`, когда разобрано всё, у
  переменной тип `never`: `const rest: never = s` и `assertNever(s)` с параметром `never` проверяют,
  что ничего не забыто; функция, которая только бросает, может вернуть `never`.
- **Литерал** получает литеральный тип там, где такой ждут: в переменной, поле, параметре,
  сравнении и `case`. Литерал объекта там, где ждут объединение, становится тем членом, чьи поля и
  значения меток (`kind: "circle"`) он называет, и так же, если идёт туда дальше
  (`const c = { kind: "circle", r: 1 }; area(c)`).
- **Объединение выводится** из разных значений: `[1, "a"]` — `(int | string)[]`, `flag ? 1 : "one"`,
  `x ?? 0`, разные `return` и `yield`, `Object.values`, `next()` генератора, который возвращает
  другой тип, ошибки `Result`. Целый литерал берёт тип остальных чисел (`[1, 2.5]` — `double[]`),
  классы — до общего родителя.
- **Печать, `JSON.stringify`, `String(x)`, шаблоны, `==`, `includes`, `indexOf`, `join`** — по
  члену, который в значении; `typeof` у объединения с меткой — по метке.
- В **обобщённых функциях** объединение — такой же аргумент типа (`count<"a" | "b" | int>`), `T`
  выводится и из объединения (`T | string` и `int | string` дают `int`); копия — по
  представлению: объединение объектов там, как и объект, — одна ссылка.
- **Даты и словари** — тоже члены: `Date | string`, `Record<string, int> | int[]`,
  `Map<string, int> | Set<string>`; дата и словарь лежат на месте объектов со своими номерами, и
  их отличают `x instanceof Date`, `instanceof Map`, `instanceof Set` (ошибка — там, где ответ
  известен по типу), `typeof` и `Array.isArray`. Литерал массива, `new Map()` и литерал объекта
  там, где ждут такое объединение, берут тип его члена (массива, `Map`, `Record`). Ключи `Map` и
  элементы `Set` типа-объединения (`Set<string | int>`) сравниваются по члену: `1` и `"1"` —
  разные ключи, как в JS.
- **`Record<"up" | "down", T>`** — как в TS, объект ровно с этими полями, а не словарь;
  `r[k]` с ключом `k` такого литерального типа читает и пишет (`counts[f.kind] += 1`) поле, которое
  он называет, — выбор места по номеру литерала. Писать так можно только в поля простого типа
  (не `T | undefined` и не объединение): о них компилятор ничего не знает по присваиваниям по
  имени.

## Проверки

```ts
function isCircle(s: Shape) { return s.kind == "circle" }
if (isCircle(s)) console.log(s.r)          // как если бы условие было написано здесь
function validIndex(a: int[], k: int) { return k >= 0 && k < a.length }
if (validIndex(arr, i)) arr[i] += 1        // индекс доказан
shapes.filter(isCircle)                    // Circle[]
```

- Функция, которая возвращает `boolean`, при проверке тела запоминает для каждого `return`, что
  известно о параметрах, когда он даёт `true` и когда `false` (как ветки `if` с этим условием), и
  соединяет это по всем `return`. Вызов в условии переносит это на аргументы, в обе ветки: тип
  переменной (член объединения, есть ли значение), поля, в которых есть значение (`hasEmail(p)` —
  потом `p.email` — строка), и факты о числах и длинах, которые доказывают индексы
  (`validIndex(a, i)`, `nonEmpty(xs)` — потом `xs.pop()` даёт `T`). Факт о переменной
  переносится, только если вызов не мог её изменить, а факт о поле живёт, как любой факт о поле.
  Функция, вызванная раньше, чем проверено её тело, проверяется для этого на копии.
- Написанные `x is T` и `this is T` проверяются по телу: каждый `return`, который может дать
  `true`, должен показать, что `x` — `T`, теми же проверками, что сужают тип, иначе ошибка. Вызов
  сужает `x` до `T`, и в обобщённой `isDefined<T>(x: T | undefined): x is T` тоже.
- **`asserts x is T` — ошибка** ([narrowing.md](docs/narrowing.md)): такая функция сужает тип
  тем, что при неудаче не возвращается, а бросает, а исключений нет. Подсказка — `x is T` и
  проверка у вызова: `if (!isCircle(s)) return err(...)`. То же у `asserts cond` (подсказка —
  `boolean`) и в типе функции; `tz port` вносит `asserts` в список того, что менять руками.
- `filter` с такой функцией или стрелкой оставляет тип, который она показывает:
  `shapes.filter(s => s.kind == "circle")` — `Circle[]`, `[1, undefined, 3].filter(x => x !=
  undefined)` — `int[]`; так же у `filter` итераторов.

## Точные десятичные числа

Значение `decimal` — два слова, как строка. Число, которое помещается в 64-битное целое с
масштабом (`19.99` — это 1999 и 2), лежит прямо в значении, без памяти в куче: сложение,
вычитание, умножение и сравнение таких чисел — целочисленные инструкции с проверкой переполнения,
встроенные в программу, а при переполнении число переходит в длинную форму. Длинное число — цифры
по 10⁹ в куче со счётчиком ссылок, как у строк; длинный литерал лежит в exe и не считается.
Ожидаемый вывод тестов посчитан BigNumber.js на тех же значениях.

Сумма миллиона цен, умноженных на количество, десять раз (`bench/decimal`) — 33 мс: столько же,
сколько у Go с копейками в `int64` вручную, и в пять раз быстрее, чем `math/big` у Go.

Как в BigNumber.js по умолчанию, `decimal` доходит до 10^10000000; меньше 10^-10000000 — ноль.
Отличия от BigNumber.js — в «Чем отличается от TypeScript».
## Result

Ошибку возвращают как значение `Result<T, E>` — `Ok` со значением или `Err` с ошибкой
([errors.md](docs/errors.md)). Имена `ok`, `err`, `Result`, `Ok`, `Err` встроены, импорт не
нужен; свои переменные и функции с этими именами их перекрывают. `import` и `export` из
`"neverthrow"` — ошибка с подсказкой: с neverthrow tz больше не совместим.

```ts
function parseAge(s: string): Result<int, string> {
  const n = parseInt(s)
  if (n.isErr()) return err("не число")
  return n.value >= 0 && n.value <= 150 ? ok(n.value) : err("не возраст")
}

const text = parseAge("42")
  .map(n => n * 2)
  .match(n => `получилось ${n}`, e => `ошибка: ${e}`)   // "получилось 84"
```

- **Значение, а не объект.** `Result` — это обе половины, флаг и место, где создан `Err`, без
  выделения памяти: половина, которой нет, — ноль, как `undefined` в `T | undefined`.
  `Result<int, string>` занимает 32 байта (половины лежат по выравниванию), `ok(1)` — константа.
  Методы — ветвление по флагу, а стрелки в них встраиваются: цепочка
  `parse(n).map(...).andThen(...).match(...)` в цикле становится обычными сравнениями (LLVM даже
  векторизует такой цикл).
- **Типы.** `ok(v)` знает только своё значение: там, где больше ничего не известно, это
  `Result<int, never>` — `Err` у него быть не может, и `ok(1).value` читается без проверки. Где
  ждут `Result` (тип переменной, параметра, возврата), `ok(1)` сразу получает этот тип:
  `const half: Result<double, string> = ok(1)` — это `1.0`, а в `Result<decimal, string>`
  литерал `ok(19.99)` точный. Иначе половины сходятся, как `return` функции и ветки `?:`:
  `return err("пусто")` и `return ok(n)` дают `Result<int, string>`; так же переменная
  `let r = ok(0)`, которой ниже присваивают `err("x")`. `Result` превращается в другой, если
  превращается каждая половина. `ok()` — `Result<void, E>`, его значение печатается как
  `undefined`. `Ok<T, E>` и `Err<T, E>` как типы — `Result`, который заведомо `Ok`
  (`Result<T, never>`) или `Err`.
- **Сужение.** После `if (r.isOk())` (и после `if (r.isErr()) return ...`) у переменной `r` можно
  читать `r.value`, в другой ветке — `r.error`; без проверки это ошибка компиляции, как в TS.
  Присваивание переменной и вызовы, которые могут её изменить, сужение снимают.
- **Ранний выход** — только `if`:
  ```ts
  function doubled(s: string) {
    const r = parseAge(s)
    if (r.isErr()) return r            // Result<never, string>
    return ok(`${r.value * 2}`)
  }                                    // Result<string, string>
  ```
  После проверки в `r` нет значения, поэтому его можно вернуть из функции, у которой значение
  другого типа, и место ошибки сохраняется. Короткой записи (`safeTry` с `yield*`, `?`, `try`)
  нет.
- **Методы:** `map`, `mapErr`, `andThen`, `orElse`, `match`, `unwrapOr`, `andTee`, `orTee`,
  `andThrough`, `isOk`, `isErr`. Ошибки разных типов из `andThen`, `Result.combine` и разных
  `return` дают объединение: `Result<int, ParseError>` и `andThen` с `Result<int, string>` —
  `Result<int, ParseError | string>`; так же значения `orElse`, `match` и `unwrapOr`. Ошибка с
  подсказкой `mapErr` — только там, где объединения быть не может (`int[] | string[]`). Функция
  для половины, которой у `Result` быть не может, не вызывается и не компилируется.
- **`Result.combine(results)`** — `Ok` с массивом значений или первый `Err`;
  **`Result.combineWithAllErrors`** — `Ok` или `Err` с массивом всех ошибок. Список `Result` со
  значениями разных типов даёт кортеж: `Result.combine([parseName(s), parseAge(s)])` —
  `Result<[string, int], E>`.
- **Печать:** `Ok { value: 84 }`, `Err { error: 'не число' } at parseAge (main.tz:3)` — у `Err`
  функция и строка его `err(...)`, путь от папки главного файла; в массивах и объектах тоже,
  глубже двух уровней — `[Ok]`. Методы, которые передают `Err` дальше (`map`, `mapErr`,
  `andThen`, преобразование в другой `Result`, `Result.combine`), место сохраняют;
  `combineWithAllErrors` ставит место своего вызова. Указатель на место — 8 байт в `Err`, сами
  строки лежат в exe.
- **Забытый `Result` — ошибка компиляции**: `save("x")` (и `store.save("x")`) отдельной
  инструкцией, стрелка в `forEach`, которая возвращает `Result`, функция или метод, переданные в
  `forEach` по имени (`names.forEach(store.save)`), `xs.map(x => save(x))`, результат которого
  никто не берёт. Его обрабатывают (`match`, `isOk()`, `unwrapOr`) или кладут в переменную; `void save("x")`
  выбрасывает его явно.
- **В обобщённых функциях** `Result<T, E>` — такой же тип, как другие:
  `function wrap<T>(x: T) { return ok(x) }`, `valueOr<T, E>(r: Result<T, E>, d: T)`,
  `type Parsed<T> = Result<T, string>`, ранний выход внутри. Типы выводятся и из `Result`: у
  `valueOr(ok(1), 0)` `E` — `never`, как в TS; `mapAll(xs, ok)` делает `Result` из каждого
  элемента. Копия — по представлению половин: `Result<User, E>` и `Result<Post, E>` — одна копия.
  Забытый `Result`, `==` между ними и превращение в строку в копии для `T = Result<...>` — ошибка
  у вызова, которому она нужна.
- **Забытый `Result`** — ошибка: и вызов, чей `Result` никто не берёт, и (с 29 октября)
  переменная с `Result` или `Promise<Result>`, которую ни разу не читают (`errors.md`, раздел 1).
- **Async.** Операция, которая может не получиться, — `Promise<Result<T, E>>` (см. «Async»):
  сначала `await`, потом методы — `(await getUser(id)).map(...)`; `getUser(id).map(...)` —
  ошибка с этой подсказкой. Несколько сразу — `Result.combine(await Promise.all(ps))`. Отдельного
  `ResultAsync` нет.
- **Библиотека** отдаёт `Result` там, где node бросает: `fetch` (`FetchError`) и чтение тел
  (`BodyError`), `JSON.parse` (`JsonError`), `decodeURIComponent` и `decodeURI` (`UriError`), `RegExp.parse`
  (`RegexError`), `URL.parse` (`UrlError`), `Date.parse` (`DateError`), функции `node:fs`
  (`SystemError`) и `node:fs/promises` (`Promise<Result>`), `execSync`, `execFileSync` и
  `spawnSync` (`ExecError`), `node:crypto` там, где важны данные (`CryptoError`), и
  `crypto.subtle` (`Promise<Result>` с `SubtleError`), `Reflect.apply` и `Reflect.construct`
  (`ReflectError`), `Intl.Locale.parse` (`LocaleError`), `decimal(text)` (`DecimalError`),
  `decode` у `TextDecoder` с `fatal` (`DecodeError`). Ошибки — объекты с меткой `code`, их типы встроены. `Err` из кода
  библиотеки получает место вызова в программе. Конструкторы не бросают: что может не
  получиться, проверяет тип аргумента (`HttpStatus`) или проверка при сборке (`new URL`,
  `new Request`, `new Intl.Locale`, `new k()` класса-значения), а разбор текста — статический `parse`.
- **Лишний `await` — ошибка:** `return await x` и `async () => await x` ничего не меняют,
  async-функция и так дождётся `Promise`, который возвращает.

## Регулярные выражения

Синтаксис и поведение — как в JS ([regex.md](docs/regex.md)): все флаги, группы и именованные
группы (одно имя и в разных альтернативах), `(?=…)`, `(?!…)`, `(?<=…)`, `(?<!…)`, `\1` и
`\k<name>`, модификаторы `(?i:…)`, ленивые повторы, с флагом `v` — операции над классами
(`[\p{L}--[a-z]]`, `&&`) и строки `\q{abc|d}`. Без `u` и `v` работает и наследие браузеров из
Annex B (`]` и `{` как символы, восьмеричные `\101`). Результаты сверены с node на тысячах
случайных регулярок.

```ts
const date = /(?<year>\d{4})-(?<month>\d{2})(-\d{2})?/
const m = "on 2024-01".match(date)          // RegExpMatchArray | undefined
if (m) {
  m[1]                 // string: группа есть в каждом совпадении
  m[3]                 // string | undefined
  m.groups.year        // string
  m[4]                 // ошибка: групп три
}
"a1b22".match(/\d+/g)                         // string[] | undefined
"2024-01-15".replace(/(\d+)-(\d+)-(\d+)/, (all, y, mon, d) => `${d}.${mon}.${y}`)
for (const { groups } of text.matchAll(/(?<key>\w+)=(?<value>\w+)/g)) console.log(groups.key)
```

- **Литерал компилируется при сборке** тем же компилятором регулярок, что в рантайме
  (`runtime/library/regex/regex_compile/regex_compile.c`, собран и в `tz`): синтаксическая ошибка — ошибка компиляции, а
  программа сопоставления — константа в exe, которую компилятор ещё и переводит в машинный код
  (см. «Сопоставление»). Литерал без `g` и `y` — константный объект (его состояние ничего не
  значит), с `g` или `y` — новый объект при каждом вычислении, как в JS, со своим `lastIndex`.
- **Типы из регулярки.** Группы, их имена и то, есть ли группа в каждом совпадении, компилятор
  знает у литерала и у переменной, которая держит литерал всё время (`const` или `let` без
  присваиваний): `m[k]` — `string` или `string | undefined`, `m.groups` — объект с полем на имя,
  лишний `m[4]` и `"$4"` в замене — ошибки. Нет совпадения — `undefined`. Колбэк `replace`
  получает совпадение, группы с их типами, позицию, строку и объект именованных групп.
- **Регулярка, известная только при выполнении** (параметр, поле, `RegExp.parse(s)`): флаги и группы
  неизвестны, поэтому `match` — ошибка с подсказкой `exec` или `matchAll`, `m[k]` —
  `string | undefined`, колбэк `replace` получает только совпадение, а `m.groups` пока не
  поддерживается. `new RegExp` с литералами (`new RegExp("\\d+", "g")`) — то же, что литерал.
- **`RegExp.parse(source, flags?)`** разбирает шаблон из строки, известной только при
  выполнении, и отдаёт `Result<RegExp, RegexError>`, где `RegexError = { kind: "REGEX"; code: "ERR_INVALID_REGEX";
  message: string }`, а `message` — текст `SyntaxError` из node (`Invalid regular expression:
  /a(b/i: Unterminated group`, `Invalid flags supplied to RegExp constructor 'gg'`).
  `new RegExp(s)` с переменной — ошибка компиляции с подсказкой `RegExp.parse`: `new` не может
  отдать `Result`. Можно только `new RegExp(RegExp.escape(s), "флаги-литерал")`: такой шаблон
  разбирается всегда.
- **Ловушки JS — ошибки компиляции**: `test()` с `g`, `replace("-", "+")` со строкой (подсказка —
  `replaceAll` или регулярка), `replaceAll` и `matchAll` с регуляркой без `g`, `match`, `matchAll`
  и `search` со строкой, `"$4"` при трёх группах и `"$<x>"` без группы `x`, `/[😀]/` без `u`,
  `\p{L}` без `u` (это текст `p{L}`), `\2` без второй группы (в JS — восьмеричный код),
  `RegExp.$1`, `re.compile()`.
- **Сопоставление** — перебор с возвратами по правилам JS: группы в повторе сбрасываются на каждой
  итерации, итерация, которая ничего не съела, заканчивает повтор, `(?<=…)` сопоставляется справа
  налево. Стек возвратов — явный, без рекурсии, поэтому длинный текст не переполняет стек C. Как в
  V8, с `u` совпадение может начаться между половинами суррогатной пары, но символ там не
  читается, а ссылка назад (`\1`) там не кончается.
- **Каждый литерал — машинный код**
  ([regex_native_lowerer.cpp](compiler/library/regex/regex_native_lowerer/regex_native_lowerer.cpp) и `matcher_*/` рядом): программа регулярки,
  переведённая в LLVM IR инструкция за инструкцией, — своя функция поиска, которую LLVM
  оптимизирует вместе с программой, в двух вариантах: для строк по байту и по два байта на символ.
  Результаты — те же, что у интерпретатора (сверено с node на тысячах случайных регулярок, в том
  числе с запоминанием из следующего пункта с первого же шага). Интерпретатор выполняет только
  регулярки `RegExp.parse(s)` и попадает в exe, только если они в программе есть. Срезано только то,
  что всё равно не удалось бы:
  - начало совпадения ищется по 32 байта за раз по байтам, которые совпадение может иметь на
    одном–трёх смещениях от начала (у `(\d{4})-` на смещении 4 — `-`), а без них — по множеству
    первых символов;
  - выбор между путями сначала смотрит на символ в позиции: путь, который с него начаться не может,
    не пробуется и не откладывается, поэтому `error|warning|fatal` — одно сравнение на путь, без
    точек возврата;
  - буквы подряд сравниваются по 8 байт, с `i` обе формы буквы сразу (`agggtaaa` с `i` — одно
    сравнение);
  - повтор одного символа не ставит точку возврата на каждый символ, а за которым идёт буква
    (`[a-z]+ing`), сразу кончается у её последнего вхождения; если регулярка начинается с
    `[a-z]+`, то после неудачи с начала слова его остальные позиции не пробуются;
  - с `g` один вызов находит сразу много совпадений.
- **Без взрыва перебора.** Если работа поиска (выборы, итерации циклов, прочитанные повтором
  символы) превышает бюджет, поиск начинает запоминать, где он был и не нашёл совпадения
  (инструкция с состоянием счётчиков циклов вокруг и позиция), и туда больше не идёт: исход оттуда
  тот же. Результат не меняется, а время — не больше длины текста на размер регулярки, как у
  bounded backtracker в Rust: `/(a+)+$/` на миллионе `a` и `!` — около 20 мс (node на 30 буквах —
  25 с), `/a.*x/` на миллионе `a` — не квадратично. Только с обратной ссылкой (`\1`) перебор не
  ограничен, как в JS, и у `RegExp.parse(s)`: интерпретатор перебирает, как V8.
- **Unicode** — свойства `\p{…}` (General_Category, Script, Script_Extensions и двоичные, с
  короткими именами) и регистр для `/i` (простое приведение регистра с `u` и `v`, `toUpperCase`
  одного символа без них) берутся из таблиц, которые
  [gen_regex_tables.mjs](runtime/library/regex/regex_compile/gen_regex_tables.mjs) получает из самого Node.js. Таблицы
  свойств попадают в exe, только если в программе есть `RegExp.parse` с неизвестным при сборке
  шаблоном.
- **`matchAll`** — ленивый итератор `IterableIterator<RegExpMatchArray>`, как в JS: совпадение
  ищется, когда его просят. `for...of`, spread, `Array.from` и цепочки методов прямо по
  `s.matchAll(re)` — обычный цикл без объекта-итератора. Итератор, сохранённый в переменной,
  держит строку и копию регулярки со своим `lastIndex` (сама регулярка его не меняет), у него
  есть `next()` и методы итераторов, печатается он как `Object [RegExp String Iterator] {}`.
  Флаг `g` у регулярки, неизвестной при сборке, проверяется, когда итератор создаётся.
- Регулярку можно хранить в полях (и `static`), массивах и `Result`, передавать (и как `T`
  обобщённой функции) и печатать (`/a/g`); `console.log` совпадения печатает его как node: массив,
  `index`, `input`, `groups`.

Скорость на 20 МБ текста из `regex_*` в [bench](bench), только сама операция, лучшее из пяти, мс
(node 24 — та же программа; Rust и Go — из [regex.md](docs/regex.md)):

| | tz | node | Rust | Go |
|---|---:|---:|---:|---:|
| `match(/[a-z]+ing/g)` — массив совпадений | 39 | 48 | 24 | 418 |
| даты — `replace(…, "$3.$2.$1")` | 19 | 20 | 24 | 347 |
| `agggtaaa\|tttaccct` без регистра | 7 | 5 | 13 | 496 |
| `\b(?:error\|warning\|fatal)\b` | 12 | 9 | 16 | 347 |
| `(\w+)ing (\w+)` — `matchAll` с группами | 68 | 70 | 138 | 571 |

До машинного кода интерпретатору на тех же тестах нужно было 130, 54, 71, 43 и 205 мс. В двух
тестах, где tz медленнее node, больше всего стоит не поиск, а сотни тысяч строк-совпадений в
массиве; Rust их только считает. В строках по два байта на символ поиск тот же (по 8 символов за
раз): 49, 23, 9, 18 и 71 мс.

## Даты

Дата — ссылка на объект, как в JS ([date.md](docs/date.md)): в нём одно поле, время в
миллисекундах с 1970 года по UTC, `int`. `set…` меняют сам объект, и это видят
все, у кого он есть. Время (`getTime()`, `a - b`, `a < b`) читается одной загрузкой из памяти, а
поля, установку полей, строки и разбор считает рантайм ([date.c](runtime/library/date/date/date.c)) по тем же
формулам, что V8. `console.log(d)` печатает как node: `2026-10-22T18:00:00.000Z`, в массивах и
объектах тоже. Неправильной даты (`Invalid Date`) нет, края, как у JS в 275760 году, — тоже.

```ts
const d = new Date(2026, 9, 23, 10, 30)     // местное время
d.setDate(d.getDate() + 10)                 // 2 ноября
console.log(d.toISOString(), d.getTimezoneOffset())  // при TZ=Asia/Bishkek: ...T04:30:00.000Z -360
console.log(new Date("2026-10-23T10:00Z") < d, (d - new Date(2026, 0, 1)) / 86400000)  // дней: int
d.toLocaleDateString("ru-RU", { day: "numeric", month: "long", year: "numeric" })
                                            // "2 ноября 2026 г."
new Intl.DateTimeFormat("en-US", { dateStyle: "full", timeZone: "Asia/Bishkek" }).format(d)
                                            // "Monday, November 2, 2026"
```

- **Числа** `get…`, `set…`, `getTime()`, `Date.now()`, `Date.UTC`, разность дат — `int`
  ([date.md](docs/date.md), [numbers.md](docs/numbers.md)): время — целые миллисекунды. Аргументы
  `new Date(ms)`, `new Date(y, m, d…)` и `set…` — целые; дробное — через
  `Math.trunc(x).toInt()`. `performance.now()` остаётся `double`.
- **Местное время** — по правилам пояса из переменной `TZ` (имя IANA, `TZ=Asia/Bishkek`), а без
  неё — пояса Windows, который ICU узнаёт по стране пользователя, как в node. Время, которое
  пропускает перевод часов, и время, которое бывает дважды, читается со смещением до перевода;
  смещение с секундами старого местного времени (`GMT-0456` у Нью-Йорка до 1883 года) и имя пояса
  в `toString()` на языке системы — как в node. Правила, имена поясов и данные языков в Windows
  берутся из ICU самой системы (`icu.dll`, Windows 10 1903 и новее): exe не растёт, а `icu.dll`
  загружается, только когда программа впервые обращается к местному времени или языкам, —
  программа на `Date.now()` и UTC её не загружает. В Linux и macOS базы поясов и языков пока
  нет (будет встроенная в exe, раздел 2 [date.md](docs/date.md)).
- **Разбор строк** — порт разборщика V8: формат ISO (дата без времени — UTC, со временем —
  местное) и всё, что V8 читает сверх него: `"2026/10/23"`, `"Oct 23 2026 10:00"`,
  `"Fri, 23 Oct 2026 10:00:00 GMT"`, текст `toString()`. Результаты сверены с node на сотнях
  тысяч случайных строк в разных поясах. Строку из переменной разбирает `Date.parse(text)` — он
  отдаёт `Result<int, DateError>` с миллисекундами, где `DateError = { kind: "DATE"; code: "ERR_INVALID_DATE";
  message: string; text: string }`; `new Date(text)` с переменной — ошибка компиляции с
  подсказкой `Date.parse` (`new Date` не может отдать `Result`), литерал проверяется при сборке.
- **Дата всегда правильная, и края JS у неё нет.** Время даты — любое `int`: `new Date(ms)`,
  `setTime(ms)` и `format(ms)` ничего не проверяют, а даты за 275760 годом, где у JS край и
  `Invalid Date`, просто продолжаются: `new Date(8640000000000000)` плюс день —
  `+275760-09-14T00:00:00.000Z`, `new Date(9000000000000000)` — `+287168-08-24T16:00:00.000Z`,
  год дальше ±999999 пишется всеми цифрами. Части у `new Date(y, m, d…)`, `Date.UTC` и `set…`
  компилятор доказывает так, чтобы время помещалось в `int` (около ±292 млн лет): число из
  ввода сначала проверяют, подсказка — обычные границы части: `if (v >= 1 && v <= 31)` у дня.
  Поля существующей даты — доверенные: `d.setDate(d.getDate() + 10)` и
  `new Date(d.getFullYear(), d.getMonth() + 1, 1)` проверки не требуют; если такая дата всё же
  дойдёт до края `int`, время на нём остановится. Местное время дат за краем JS — по правилам
  «равного» года рядом с нашим (тот же день недели 1 января, та же високосность), `Intl`
  пишет и их (григорианский календарь — сдвигом на 400-летние циклы). Строки (`Date.parse`,
  литералы) — в пределах JS, как в node. Поэтому `toISOString()` и `format()` не падают, а
  `toJSON()` — `string` (в JS у неправильной даты — `null`).
- **Языки**: `toLocaleString` и `Intl.DateTimeFormat` собирают из опций скелет шаблона
  (`yMMMMd`) и берут у ICU лучший шаблон языка, как V8, с теми же правилами часов (`hour12`,
  `hourCycle`, `-u-hc-`), частей по умолчанию и `dateStyle` / `timeStyle`. Без языка — язык
  системы; языка, которого нет в ICU, — ближайший (`"en-XX"` — `"en"`) или язык системы.
  Формат, который `toLocaleString` повторяет в цикле, создаётся один раз. Как и V8, `format`
  пишет обычный пробел там, где ICU ставит узкий неразрывный (перед «PM»).
- **Ловушки, видные в коде, — ошибки компиляции**: строка, которую JS не разберёт
  (`new Date("23.10.2026")`), дата без времени (`"2026-10-23"` — полночь UTC: ошибка с
  подсказками `"2026-10-23T00:00"` и `"2026-10-23T00:00Z"`), несуществующий день
  (`new Date(2026, 1, 31)` и `"2026-02-30T10:00"`), месяц вне 0…11 (`new Date(2026, 12, 1)`), год
  0…99 (в JS — 1900…1999), часы, минуты, секунды и миллисекунды вне своих границ, `getYear`,
  `setYear`, `toGMTString`; то же у `Date.parse`, `Date.UTC` и сеттеров (`d.setMonth(12)`,
  `d.setHours(24)`), и для констант (`const m = 12`); вычисленное число сеттер переносит, как JS
  (`d.setDate(d.getDate() + 10)`).
  День `0` разрешён: `new Date(2026, 2, 0)` — последний день февраля. Неверный язык или опция,
  написанные в коде (`"en_US"`, `{ month: "lnog" }`, `dateStyle` вместе с `hour`), — тоже ошибки
  (в JS — `RangeError` и `TypeError` при выполнении).
- `a - b` двух дат — разница в миллисекундах, `<` и `>` сравнивают время (tsc вычитание не
  разрешает, а node выполняет так же); `==` — тот же ли это объект, как в JS. `d + 1` (в JS —
  склейка текстов) и дата минус число — ошибки с подсказкой `new Date(d.getTime() + ms)`.
- Дату хранят в полях (и классов-наследников, и интерфейсов), массивах, словарях (ключ `Map` и
  `Set` — по ссылке, как в JS: две даты с одним временем — два ключа) и `Result`, передают как
  `T` обобщённой функции и отдают из генераторов; `` `${d}` ``, `String(d)` и `join()` массива
  дат — текст `toString()`. `sort()` массива дат без функции сравнения — ошибка с подсказкой
  `sort((a, b) => a.getTime() - b.getTime())`: в JS он сравнил бы тексты `toString()`, то есть
  по названиям дней недели.
- **JSON**: `JSON.stringify` пишет дату её `toJSON()` — ISO-текстом, и в объектах, массивах и
  классах тоже. В JSON дат нет: дату из `Json` делает программа — `Date.parse(text)` строки.

## Байтовые массивы

Двоичные данные — как в node ([bytes.md](docs/bytes.md)): `ArrayBuffer` с байтами, типизированные
массивы и `DataView` поверх него, `Buffer`, `TextEncoder` и `TextDecoder`. Элемент имеет тип
своего вида (`uint8` у `Uint8Array` и `Buffer`, [numbers.md](docs/numbers.md), раздел 9),
поэтому цикл по байтам компилируется в тот же код, что и в C, а целое в элемент кладётся явно,
с доказательством, что влезает.

```ts
const text = new TextEncoder().encode("Привет")     // Uint8Array(12)
new TextDecoder("windows-1251").decode(new Uint8Array([0xcf, 0xf0, 0xe8]))   // "При"
const b = Buffer.from("aGVsbG8=", "base64")         // <Buffer 68 65 6c 6c 6f>
b.toString("hex"), b.readUInt16BE(0)                // "68656c6c6f" 26725
const view = new DataView(new ArrayBuffer(16))
view.setFloat64(0, Math.PI, true)
let sum = 0
for (let i = 0; i < text.length; i++) sum = (sum + text[i].toInt()) & 0xff   // векторизуется
const out = new Uint8Array(4)
out[0] = sum.toUint8()  // можно: маска доказала 0…255
out[1] = 300            // ошибка: integer literal does not fit in type 'uint8'
out[2] = text[0] ?? 0                               // можно: элемент — uint8
out[3] = text[0] + text[1]  // ошибка: 'text[0] + text[1]' may overflow uint8
```

- **Как устроено.** `ArrayBuffer` — объект со счётчиком ссылок, байты лежат в нём же, одним
  выделением памяти. Типизированный массив и `DataView` — окно в `ArrayBuffer`: указатель на
  первый байт и длина; `subarray`, `new Uint8Array(buffer, offset, length)` и `Buffer.from(ab)`
  делят ту же память, `slice` копирует, как в node. Длина не меняется (и `length = n` — ошибка),
  поэтому факты о ней переживают вызовы функций.
- **Элементы**: `Int8Array` — `int8`, `Uint8Array`, `Uint8ClampedArray` и `Buffer` — `uint8`,
  `Int16Array` — `int16`, `Uint16Array` — `uint16`, `Int32Array` — `int32`, `Uint32Array` —
  `uint32`, `Float16Array`, `Float32Array`, `Float64Array` — `double`. Методы `DataView` и
  `Buffer` (`getUint16`, `readUInt32LE`, `setUint8`, `writeInt16BE`…) берут и отдают `int`, как
  в node, с доказательством, что значение влезает. Загрузки длины и указателя помечены для LLVM отдельно от байтов
  (TBAA), так что запись байта их не портит: они выносятся из цикла, и циклы `xor`, суммы,
  копирования и заполнения становятся векторными (`<16 x i8>`).
- **Индексы** — как у массивов: доказанный индекс даёт `T`, иначе `T | undefined`, а запись по
  недоказанному индексу — ошибка (в JS она молча пропала бы: длина не растёт) с подсказкой
  проверки. Смещение `getUint32(off)` / `readUInt32LE(off)` у `DataView` и `Buffer` должно
  быть доказано (см. «Доказательства вместо падений»), и чтение идёт без проверки.
- **Значение должно влезать** при любой записи: `u[i] = x`, `u[i] += x`, `fill`, `set`, `with`,
  `new Uint8Array([…])`, `of`, `from`, `map`. Значение элемента — его тип: `int` кладётся через
  `x.toUint8()` и другие, и компилятор доказывает, что влезает: литерал из диапазона, маска
  (`(x & 0xff).toUint8()`), сдвиг, остаток, проверка условием (`if (x >= 0 && x < 256)`) или
  `Math.min` / `Math.max`. Арифметика элементов — в их типе (`u[i] + 1` — `uint8`), и она тоже
  обязана влезть: шире считают явно, `u[i].toInt() + 1`. `setUint8` / `setInt16` / …,
  `writeUInt8` / … и `buffer.fill(x)` берут `int` и требуют того же доказательства.
  `Uint8ClampedArray` принимает в элемент любой `int` и прижимает его к 0…255, как JS, массивы
  чисел с плавающей точкой — любое число; `double` в целый массив без преобразования
  (`Math.round(x).toUint8()`) не пишется.
- Копия из массива другого вида (`new Uint8Array(int16s)`, `u8.set(int16s)`,
  `new Uint8Array(ints)`) — тоже ошибка, если его элементы могут не влезть, с подсказкой
  `Uint8Array.from(a, x => x & 0xff)`.
- **Методы** типизированных массивов: `at`, `indexOf`, `lastIndexOf`, `includes`, `join`,
  `toString`, `fill`, `set`, `subarray`, `slice`, `copyWithin`, `reverse`, `sort` (байты —
  подсчётом, числа с плавающей точкой — как в JS: `-0` перед `0`), `toSorted`,
  `toReversed`, `with`, `every`, `some`, `find…`, `forEach`, `map`, `filter`, `reduce`,
  `reduceRight`, `keys`, `values`, `entries`; `for…of`, spread `[...u]` и деструктуризация;
  `Array.from(u)`, `Uint8Array.from(it, f)`, `BYTES_PER_ELEMENT`, `ArrayBuffer.isView`.
- **`Buffer`** — `Uint8Array` с методами node: `from` (строка, массив, `ArrayBuffer`, другой
  массив), `alloc`, `allocUnsafe`, `concat`, `byteLength`, `compare`, `isBuffer`, `isEncoding`;
  `toString`, `write`, `equals`, `compare`, `copy`, `indexOf` / `includes` (байт, строка,
  байты), `fill`, `read…` / `write…` для всех чисел (и `readUIntLE(off, n)`), `swap16/32/64`,
  `toJSON`, `subarray`. Кодировки — `utf8`, `utf16le` (`ucs2`), `latin1` (`binary`), `ascii`,
  `base64`, `base64url`, `hex` с разбором и заменой неправильных байтов, как в node; кодировка,
  написанная в коде, проверяется при компиляции. `buf.slice()` — ошибка (в node он делит память,
  а у `Uint8Array` копирует; подсказки `subarray` и `Buffer.from(buf.subarray(…))`),
  `new Buffer(…)` — ошибка, как устаревший в node.
- **`TextEncoder` / `TextDecoder`**: UTF-8 (`encode`, `encodeInto`), UTF-16LE / BE и все
  однобайтовые кодировки WHATWG (`windows-1251`, `koi8-r`, `iso-8859-5`, `ibm866`, …) с их
  метками, `fatal` и `ignoreBOM`. С `fatal` `decode` отдаёт `Result<string, DecodeError>` (`Err`
  с кодом node `ERR_ENCODING_INVALID_ENCODED_DATA` на неправильных байтах вместо `TypeError`):
  `new TextDecoder("utf-8", { fatal: true })` — тип `FatalTextDecoder`, а без `fatal` (или с
  `fatal: false`) — `TextDecoder`, и `decode` у него — просто `string`. Таблица кодировки
  попадает в программу, только если метку видно в коде (одна — 256 байт; метка, известная
  только при выполнении, — все 29, ~7 КБ);
  таблицы и метки [gen_encodings.mjs](runtime/library/bytes/encodings/gen_encodings.mjs) берёт у самого node.
- **Многобайтовые кодировки** `TextDecoder`: `gbk` и `gb18030`, `big5`, `euc-jp`, `iso-2022-jp`,
  `shift_jis`, `euc-kr` — декодеры стандарта WHATWG ([cjk.c](runtime/library/bytes/cjk/cjk.c)) с его таблицами,
  которые [gen_cjk_tables.mjs](runtime/library/bytes/cjk/gen_cjk_tables.mjs) делает из файлов
  `index-*.txt` стандарта ([cjk_tables.h](runtime/library/bytes/cjk/cjk_tables.h), лицензия BSD-3-Clause —
  её печатает `tz licenses`). Таблица и декодеры (3 КБ) тоже попадают в exe, только если
  кодировку видно в коде: `gbk` и `gb18030` — +53 КБ, `euc-kr` — +51 КБ, `big5` — +43 КБ,
  `euc-jp` — +40 КБ, `shift_jis` и `iso-2022-jp` — +26 КБ (у трёх японских одна таблица JIS X
  0208, все три — +40 КБ), метка, известная только при выполнении, — все таблицы, +196 КБ. Неправильная
  последовательность — «�», ASCII-байт после ведущего начинает заново, с `fatal` — `Err`
  `The encoded data was not valid for encoding euc-kr`. TextEncoder, как и по стандарту, —
  только UTF-8. node 24 декодирует `big5`, `euc-jp`, `shift_jis` и `euc-kr` конвертерами ICU,
  а не по стандарту: у него нет символов HKSCS в Big5 (`0x8740` — `U+F266` вместо `䏰`) и
  расширения windows-949 в EUC-KR (`0x8141` — `U+0081 A` вместо `갂`), `0x80` в Shift_JIS — «�»
  вместо `U+0080`, а `0x1A`, `0x1C` и `0x7F` переставлены; tz декодирует, как браузеры.
- **`decode(bytes, { stream: true })`** — кусок потока: символ, который начался в нём, но не
  кончился, ждёт следующего вызова, BOM пропускается только в начале потока, вызов без
  `stream` кончает поток (незаконченный символ — «�»), и следующий начинает новый. Так у всех
  кодировок, и у UTF-8 и UTF-16 тоже. node 24 у `gbk` и `gb18030` бросает `TypeError` и без
  `fatal`, если неправильная четырёхбайтовая последовательность разрезана между кусками; tz
  даёт «�».
- **Длины и смещения** доказываются при сборке (см. «Доказательства вместо падений»):
  `new Uint8Array(-1)` и смещение вида не кратное размеру элемента — ошибки компиляции.
  **Ошибки при выполнении** пока остались у кодировки из переменной: `Buffer.from(s, enc)` —
  `TypeError: Unknown encoding: x`, как в node.
- **Вывод**: `console.log` печатает как node: `Uint8Array(3) [ 1, 2, 3 ]` (с колонками у длинных),
  `<Buffer 68 65 6c 6c 6f>`, `ArrayBuffer { [Uint8Contents]: <00 00>, [byteLength]: 2 }`,
  `DataView`, `TextEncoder`, `TextDecoder`; `JSON.stringify` — `{"0":1,"1":2}` и
  `{"type":"Buffer","data":[…]}`, `String(u)` — `"1,2,3"`.
- **В объединениях и словарях**: `Uint8Array | string`, `Packet | Float64Array` и другие сужают
  `typeof`, `instanceof Uint8Array` (и другого вида) и `Buffer.isBuffer(x)`. `Buffer` — это
  `Uint8Array`, поэтому `instanceof Uint8Array` верно и для `Buffer`, а `u instanceof Buffer` у
  `Uint8Array` проверяется при выполнении и сужает `u` до `Buffer`. Без сужения через объединение
  читаются `byteLength`, `byteOffset`, `buffer` и `length`, если они есть у всех членов: у
  `Uint8Array | Float64Array` `byteLength` умножает длину на размер элемента того, что лежит в
  значении. В `Map`, `Set`, `Result`, `Promise` и обобщённых функциях байтовые массивы лежат по
  ссылке, как в node.
- **`embedBytes("table.bin")`** вшивает файл в exe и даёт `Uint8Array` его байтов: один на всю
  программу, без копирования при запуске, с длиной, известной компилятору, поэтому
  `t[x & 0xff]` у таблицы из 256 байт — без проверки.
- Пока нет: `resize` и `transfer` у `ArrayBuffer`, `SharedArrayBuffer` и `Atomics` (ошибки
  «пока не поддерживается»), `BigInt64Array`, `getBigInt64` и `readBigInt64LE` — ошибки, потому что
  нет `bigint`. Длина — до 2^30 − 1 байт.

## Языки: Intl

Числа, сравнение строк и слова — по правилам языков, как в node ([intl.md](docs/intl.md)).
Объекты `Intl` делает рантайм ([intl.c](runtime/library/intl/intl/intl.c) и файлы после него): опции читаются в
том порядке и с теми же проверками, что в V8, а пишет и сравнивает ICU. Даты — в «Датах».

```ts
(1234567.891).toLocaleString("ru-RU")                        // "1 234 567,891"
new Intl.NumberFormat("ru-RU", { style: "currency", currency: "RUB" }).format(1500)
                                                             // "1 500,00 ₽"
["ёж", "яблоко", "арбуз"].sort((a, b) => a.localeCompare(b, "ru"))
                                                             // [ 'арбуз', 'ёж', 'яблоко' ]
names.sort(new Intl.Collator("sv").compare)                  // compare привязан к объекту
new Intl.PluralRules("ru-RU").select(5)                      // "many" — «5 яблок»
new Intl.RelativeTimeFormat("ru").format(-3, "days")         // "3 дня назад"
new Intl.ListFormat("ru").format(["a", "b", "c"])            // "a, b и c"
[...new Intl.Segmenter("en").segment("👨‍👩‍👧e\u0301")].length   // 2 буквы
const locale = Intl.Locale.parse(tag)                        // язык из данных — Result
if (locale.isOk()) price.toLocaleString(locale.value, { style: "currency", currency: "EUR" })
```

- **Числа**: `toLocaleString(locale?, options?)` у целых, `double` и `decimal` и
  `Intl.NumberFormat` — стили `decimal`, `percent`, `currency` (`currencyDisplay`,
  `currencySign`), `unit` (единицы ECMA-402 и пары через `-per-`, `unitDisplay`), записи
  `compact`, `scientific`, `engineering`, все опции цифр (`minimum…`/`maximum…`, `roundingMode`,
  `roundingPriority`, `roundingIncrement`, `trailingZeroDisplay`), `useGrouping`, `signDisplay`,
  `numberingSystem` и `-u-nu-`; `format`, `formatToParts`, `resolvedOptions`. Опции становятся
  скелетом форматтера ICU (`currency/RUB rounding-mode-half-up`), как в V8. `decimal`
  форматируется из своего текста — без потерь: `decimal("12345678901234567890.125")` даёт все
  цифры, как node даёт их у строки. Строку `format` не берёт (ошибка с подсказкой `decimal(s)`).
- **Сравнение строк**: `localeCompare(b, locale?, options?)` и `Intl.Collator` (`sensitivity`,
  `numeric`, `caseFirst`, `ignorePunctuation`, `usage`, `collation` и `-u-co-`/`-u-kn-`/`-u-kf-`);
  `compare` даёт `int`. `nf.format`, `collator.compare` и `dtf.format`, взятые как значения, —
  функции, привязанные к своему объекту, как геттеры JS: `nums.map(nf.format)`,
  `names.sort(collator.compare)`; тип аргумента берётся из места (`map` массива `int` передаёт
  `int`).
- `normalize("NFC" | "NFD" | "NFKC" | "NFKD")` — таблицами Unicode из ICU системы; ASCII и текст,
  который уже в нужной форме, остаются той же строкой без копии.
- **Сегменты**: `Intl.Segmenter` (`grapheme`, `word`, `sentence`): `segment(text)` даёт сегменты
  `{ segment, index, input, isWordLike? }`, их обходит `for...of`, spread и `Array.from`, и
  `containing(i)` — `Intl.SegmentData | undefined`.
- **Остальное**: `Intl.PluralRules` (`select` по числу, как его пишут опции цифр: `1.0` с
  `minimumFractionDigits: 1` — `"other"`; `pluralCategories`), `Intl.RelativeTimeFormat`
  (`numeric: "auto"` — «вчера»; `formatToParts` с `unit`), `Intl.ListFormat`,
  `Intl.DisplayNames` (`language`, `region`, `script`, `currency`, `calendar`,
  `dateTimeField`; `of` — `string | undefined` при `fallback: "none"`),
  `Intl.getCanonicalLocales` и `supportedLocalesOf` у каждого класса.
- **Типы**: `Intl.NumberFormat` и другие классы, `Intl.NumberFormatOptions` (и опции других
  классов, все поля `T | undefined`; у опций со списком значений — объединение литералов,
  `style?: "decimal" | "percent" | "currency" | "unit"`, у цифр — целые в границах, `0..100`),
  `Intl.ResolvedNumberFormatOptions`,
  `Intl.NumberFormatPart`, `Intl.SegmentData`, `Intl.LDMLPluralRule` пишут в аннотациях, как в
  TS. `console.log` печатает объекты как node: `NumberFormat [Intl.NumberFormat] {}`.
- Объекты `Intl` лежат в `Map`, `unknown` и обобщённом `T` по ссылке и стоят в объединениях с
  другими типами (`Intl.Collator | Date` сужает `instanceof Date`); два вида `Intl` в одном
  объединении — ошибка: при выполнении их не различить. `Iterable<T>` принимает сегменты
  `segment(text)`, а `collator.compare` годится в `sort` любого массива строк.
- **Язык**: строкой, известной при сборке (`"ru-RU"`, `"de-u-co-phonebk"`; константа, выбор
  из литералов `n > 1 ? "ru" : "en"`, строка после `if (lang == "ru")`, литеральный тип
  `"ru" | "en"`), или `Intl.Locale`; `undefined` или без аргумента — язык системы; языка,
  которого нет в ICU, — ближайший (`"en-XX"` — `"en"`) или язык системы. Строка, известная
  только при выполнении, — ошибка с подсказкой `Intl.Locale.parse(tag)`: он отдаёт
  `Result<Intl.Locale, LocaleError>` (`code: "ERR_INVALID_LOCALE"`, текст ошибки V8, `tag`), а не
  `RangeError`. Массив языков в конструкторах не берётся (как и у дат); в `getCanonicalLocales`
  и `supportedLocalesOf` — берётся, если языки в нём написаны.
- **`Intl.Locale`**: `new Intl.Locale("en-latn-us")` (литерал проверяется при сборке; опций
  нет — их пишут в тег: `"th-TH-u-ca-buddhist"`), `Intl.Locale.parse(tag)`, поля `baseName`,
  `language`, `script`, `region`, `maximize()`, `toString()` — канонический тег, как в node
  ([locale.tz](library/intl/locale.tz)). Тип — `Intl.Locale`.
- **Ошибки компиляции** — то, что JS отверг бы при выполнении, когда оно написано в коде: язык не
  по BCP 47 (`"en_US"`), значение не из списка опции (`{ style: "currncy" }`), неизвестная опция,
  валюта не из трёх букв, единица не из ECMA-402, `style: "currency"` без `currency`,
  `Intl.DisplayNames` без `type`, цифры вне границ и минимум больше максимума, `roundingIncrement`
  не из списка, `normalize("NFX")`, единица `RelativeTimeFormat` вроде `"decade"`,
  `Intl.PluralRules(...)` без `new`. Значение опции со списком, известное только при выполнении,
  — тоже ошибка, пока проверка в `if` не сузит его до литералов (`if (s == "short" || ...)`);
  цифры — `int`, проверенный в `if (n >= 0 && n <= 100)`. Опции из переменной типа
  `Intl.NumberFormatOptions`, которые не сходятся между собой, рантайм сводит без исключения:
  максимум цифр поднимается до минимума, `roundingIncrement` берёт равные дробные цифры (как
  по умолчанию в ECMA-402) и не действует со значащими цифрами. `DisplayNames.of` кода, который
  не код, — `undefined`. Неверные значения без списка, пришедшие из данных (валюта, единица,
  `numberingSystem`, `collation`, часовой пояс), не действуют, как неверный заголовок у
  `Headers`: `{ style: "currency", currency: "XX1" }` печатает просто число (в node —
  `RangeError`).
- `toLocaleString` и `localeCompare`, повторяемые в цикле с теми же опциями, создают объект ICU
  один раз: сортировка 100 000 строк через `localeCompare` быстрее, чем в node.
- В Windows всё это — ICU самой системы (`icu.dll`): exe не растёт. В Linux и macOS данных языков
  пока нет (будут встроены в exe по правилу раздела 3 [date.md](docs/date.md)).

## Процесс и вывод

stdout буферизуется: в файл или другую программу вывод уходит кусками по 8 КБ, в консоль — по
строкам, а `process.stdout.write` в консоль пишет сразу, как node (это может быть приглашение к
вводу). Перед записью в stderr буфер сбрасывается, поэтому при `prog > out.txt 2>&1` строки stdout
и stderr идут в том же порядке, что в node. При любом выходе — обычном, через `process.exit()` или по
панике — напечатанное дописывается. Буфер сбрасывается и тогда, когда программе нечего делать (она
ждёт сеть, таймер, ввод): логи сервера в файле, Docker или systemd видны сразу, а не по 8 КБ, и не
пропадают, если процесс убили. Большой вывод подряд по-прежнему уходит крупными кусками. Ctrl+C,
Ctrl+Break и закрытие окна тоже сначала дописывают напечатанное.

Сигналы ОС слушают `process.on(signal, listener)`, `process.once` и `process.off` — только
`"SIGINT"` (Ctrl+C), `"SIGTERM"`, `"SIGHUP"` (в Windows — закрытие окна консоли) и `"SIGBREAK"`
(Ctrl+Break); в Windows `SIGTERM` не приходит, но слушать его можно, как в node. Пока есть
слушатель сигнала, по этому сигналу программа сама не выходит — решает слушатель; живой программу
слушатель не держит. Слушатель — функция без аргументов, может быть `async`:

```ts
process.once("SIGTERM", async () => {
  await listening.stop()
  process.exit(0)
})
```

Другие события `process` (`"exit"`, `"uncaughtException"`…) — ошибки компиляции.

`process.exit(code)` завершает программу сразу, и компилятор знает, что код после вызова не
выполняется: после `if (!file) process.exit(1)` переменная `file` — уже строка, а функция, которая
кончается вызовом `process.exit`, может не возвращать значение. Без кода программа завершается с
`process.exitCode`, как в node; дойдя до конца — тоже с `process.exitCode` (или 0).

`process.argv` — один массив на всю программу, как в node. На месте пути к скрипту (у exe его нет)
— снова путь к exe, поэтому `process.argv.slice(2)` — аргументы, как в любом скрипте для node. В
Windows аргументы, переменные окружения и пути берутся у системы в UTF-16, а в консоль текст
пишется через `WriteConsoleW`, так что русские буквы работают при любой кодовой странице консоли.

## Встроенные модули

```ts
import { readFileSync, writeFileSync, readdirSync } from "node:fs"
import { join, basename } from "node:path"
import { homedir } from "node:os"
import { readLine } from "tz:io"

const name = readLine("Имя? ")                                  // string | undefined
const text = readFileSync(join(homedir(), "notes.txt"), "utf8")  // Result<string, SystemError>
if (text.isErr()) {
  console.log(text.error.code == "ENOENT" ? "заметок нет" : text.error.message)
  process.exit(1)
}
const saved = writeFileSync("out.txt", text.value.toUpperCase())  // Result<void, SystemError>
if (saved.isErr()) console.log(saved.error.message)
for (const entry of readdirSync(".", { withFileTypes: true }).unwrapOr([]))
  if (entry.isFile()) console.log(basename(entry.name, ".txt"))
```

- **Импорт** — как в node, по имени ([io.md](docs/io.md)): `"node:fs"` и `"fs"` — одно и то же.
  `import fs from "fs"` и `import * as path from "node:path"` — ошибки с подсказкой, какие имена
  импортировать: те, что файл берёт через `fs.`. Функции с колбэком (`readFile`, `mkdir`...) —
  ошибка с подсказкой взять ту же функцию из `node:fs/promises` или `readFileSync`; то, чего ещё
  нет (`openSync`, `open` из `node:fs/promises`, `node:http`...), — ошибка, которая так и говорит и
  называет документ, где это будет. Функцию модуля можно только вызвать: где ждут функцию, пишут
  стрелку (`files.map(f => basename(f))`).
- **`node:path`** — `join`, `resolve`, `normalize`, `relative`, `dirname`, `basename`, `extname`,
  `parse`, `format`, `isAbsolute`, `toNamespacedPath`, `sep`, `delimiter`. Стиль — системы (в
  Windows `\`), через `posix` и `win32` (`posix.join(a, b)`, и `"node:path/posix"`) — другой.
  Алгоритмы — те же шаги, что в `path.js` из node, на C (`runtime/library/path/path/path.c`); результаты сверены с
  node на тысячах случайных путей, со всеми особыми случаями Windows (`\\server\share`, `\\?\`,
  `C:`, имена устройств вроде `CON:`).
- **`node:os`** — `EOL`, `devNull`, `platform()`, `homedir()`, `tmpdir()`, `hostname()`, `cpus()`,
  `availableParallelism()`, `totalmem()`, `freemem()`, `arch()`, `type()`, `release()`, `version()`,
  `machine()`, `uptime()`, `endianness()`, `loadavg()`, `userInfo()` — из тех же источников, что у
  node (libuv): `release()` — настоящая версия Windows, `version()` — «Windows 11 Home».
- **`node:process`** — `argv`, `env`, `exit`, `exitCode`, `cwd`, `platform`, `execPath`, `stdin`,
  `stdout`, `stderr`: то же, что у глобального `process`.
- **`node:buffer`** — `Buffer`: тот же, что глобальный.
- **`node:fs`** — синхронные функции. Все, кроме `existsSync`, дают `Result<T, SystemError>`:
  ошибка ОС — `Err`, а не исключение (`Result<void, SystemError>` у тех, что ничего не
  возвращают). Путь с нулевым символом — тоже `Err`, с кодом и текстом node
  (`ERR_INVALID_ARG_VALUE`; node бросает `TypeError`). Функции: `readFileSync`, `writeFileSync`, `appendFileSync`,
  `existsSync`, `accessSync`, `chmodSync`, `truncateSync`, `statSync` и `lstatSync`
  (`{ throwIfNoEntry: false }` — `Stats | undefined`), `readdirSync` (`recursive`, `withFileTypes` —
  `Dirent` с `isFile()`, `isDirectory()` и т.п.), `mkdirSync` (`recursive`; результат — первый
  созданный каталог, как в node), `mkdtempSync`, `rmdirSync`, `unlinkSync`, `rmSync` (`recursive`,
  `force`), `renameSync`, `copyFileSync` (`constants.COPYFILE_EXCL`), `realpathSync`, `constants`;
  `Stats` и `Dirent` — типы (`const s: Stats = statSync(p).unwrapOr(fallback)`), печатаются как в node:
  `Stats { dev: ..., mode: 33206, ... }`. Даты `atime`, `mtime`, `ctime` и `birthtime` делаются
  при чтении из `atimeMs` и других, округлённых до миллисекунды, как их делают геттеры node,
  поэтому ни `console.log`, ни `JSON.stringify` их не показывают; каждое чтение — новая дата.
  Файл кусками — `createReadStream` и `createWriteStream` (см. «Потоки»).
- **Текст и байты**: `readFileSync(path, "utf8")` (или `{ encoding: "utf8" }`) — `string`, битый
  UTF-8 становится «�», как в node; другие кодировки `Buffer` (`"latin1"`, `"hex"`, `"base64"`...)
  тоже дают `string`, а без кодировки — `Buffer` с байтами файла. `writeFileSync` и
  `appendFileSync` пишут строку (в кодировке из опций, по умолчанию UTF-8) или байты
  типизированного массива, `Buffer` или `DataView` как есть; кодировка при байтах — ошибка (node её
  молча не замечает). `readFileSync(0, "utf8")` — весь stdin, `writeFileSync(1, s)` — в stdout, по
  порядку с `console.log`. Опции пишут литералом в вызове: от `encoding` и `withFileTypes` зависит
  тип результата; `flag` — литерал из списка node (`"r"`, `"wx"`, `"a+"`...), другой — ошибка
  компиляции (node бросает `TypeError`). Имена у `readdirSync` и других — только в UTF-8.
- **`node:fs/promises`** (и `import { promises } from "node:fs"`) — `readFile`, `writeFile`,
  `appendFile`, `access`, `chmod`, `truncate`, `stat`, `lstat`, `readdir`, `mkdir`, `mkdtemp`,
  `rmdir`, `unlink`, `rm`, `rename`, `copyFile`, `realpath`, `constants`: те же аргументы и опции,
  что у функций `...Sync`, а результат — `Promise<Result>` того же: ошибка ОС и ноль в пути —
  `Err` с тем же `SystemError`, `Promise` не отклоняется. Различия с `...Sync` — как
  в node: `readdir` с `recursive` идёт, как его async-версия, — каталог, встреченный в списке,
  читается сразу, а обходится последним (стек, а не уровни); `realpath` — путь, который даёт
  система (`GetFinalPathNameByHandle`), с буквами, как на диске; ошибка `mkdir` с `recursive`
  показывает путь, на котором она случилась. `readFile` без кодировки даёт `Buffer`, а
  `writeFile` и `appendFile` пишут и байты, как их `...Sync`; `open` и `FileHandle` пока нет.
- **Как ждут.** Вызов — одна работа для пула потоков (`UV_THREADPOOL_SIZE` потоков, по умолчанию
  4, как в node): `readFile` в потоке открывает, читает и закрывает файл, а строку и `SystemError`
  делает главный поток, в фазе poll цикла событий, после каждой — микрозадачи, как после колбэка
  node. В Windows фаза poll ждёт на одном порте завершения (IOCP), а время до следующего таймера
  отмеряет таймер высокого разрешения. Порядок — такой, какой мог бы дать node: там `readFile` —
  четыре похода в пул (open, fstat, read, close), каждый в свой оборот цикла, поэтому результат
  ждёт столько же оборотов, и цепочка из трёх `setImmediate`, начатая вместе с ним, всегда
  успевает раньше, как в node. 10 тыс. маленьких файлов через `Promise.all` — 97 мс против 86 мс у
  Go с горутинами (с `UV_THREADPOOL_SIZE=16` — 58 мс); node на той же программе падает с `EMFILE`:
  он сначала открывает все 10 тыс. файлов.
- **`node:readline`** и **`node:readline/promises`** — `createInterface({ input: process.stdin,
  output: process.stdout })` (или `(process.stdin, process.stdout)`): `for await (const line of rl)`
  даёт строки stdin без конца строки (`\n`, `\r\n`, одиночный `\r`, U+2028 и U+2029, как у node;
  последняя может кончиться вместе с вводом), `rl.question(q)` пишет вопрос в `output` и даёт
  `Promise` следующей строки (`Err`, если интерфейс закрыт), в `node:readline` —
  `rl.question(q, answer => ...)`, `rl.close()`. Выход из `for await` (`break`) закрывает
  интерфейс, как в node. Строки берутся из того же буфера stdin, что у `readLine`, и только когда
  их просят, поэтому строка не теряется: в node строки файла, пришедшие между двумя `question`,
  уходят слушателям `line` или пропадают. Интерфейс, у которого строк не просят, ничего не читает
  и не держит программу до конца ввода. События (`rl.on("line", ...)`) — с `EventEmitter`
  ([events.md](docs/events.md)), до тех пор ошибка с подсказкой.
- **`SystemError`** — ошибка в `Err` функций файлов, встроенный тип объекта с полями `code`
  (`"ENOENT"`...), `message`, `errno` (число, которое дал бы node: в Windows `-4058`), `syscall`,
  `path` и `dest`; `message` — как в node: `ENOENT: no such file or directory, open
  'C:\app\config.txt'`. Коды те же, что у node на этой ОС: ошибка Windows переводится так же, как
  это делает libuv (`EPERM` у `unlinkSync` каталога, `EISDIR` у чтения каталога). `rmSync`
  каталога без `recursive` — `SystemError` с кодом `ERR_FS_EISDIR`, как в node. `Stats` и `Dirent`
  делает только рантайм: `new` и `extends` с ними — ошибки.
- **Пути в Windows** — как у node: путь разрешается (`resolve`) и идёт в систему в виде
  `\\?\C:\...`, поэтому длинные пути работают, а ошибки показывают его полным, без префикса. Все
  вызовы — «W»-функции Windows: имена файлов на любом языке работают при любой кодовой странице.
- **`readLine(question?)`** из `"tz:io"` печатает вопрос как есть (сначала дописав буфер вывода),
  читает строку без `\n` или `\r\n` и в конце ввода даёт `undefined`. Из консоли Windows строка
  читается через `ReadConsoleW`, поэтому русские буквы вводятся при любой кодовой странице; из файла
  или другой программы — UTF-8. `readLine` и `readFileSync(0)` читают stdin через один буфер: что не
  взяла одна, получит другая.
- Объекты, которые делает рантайм (`SystemError`, `Stats`, `Dirent`, результат `parse()`, `cpus()`),
  он строит функцией программы: заполняет структуру с полями, а функция кладёт их в объект нужной
  раскладки.

```ts
import { execFileSync, execSync, spawnSync } from "node:child_process"

const head = execFileSync("git", ["rev-parse", "HEAD"], { encoding: "utf8" })   // Result<string, ExecError<string>>
const files = execSync("dir /b", { cwd: "C:\\work" })                           // Result<Buffer, ExecError<Buffer>>
const r = spawnSync("git", ["status"], { encoding: "utf8", timeout: 5000 })
if (r.isErr()) console.log(r.error.code, r.error.message)  // не запустилась, таймаут
else if (r.value.status != 0) console.log(`код ${r.value.status}`, r.value.stderr)
const tests = execSync("npm test", { stdio: "inherit" })   // вывод сразу в консоль
if (tests.isErr()) process.exit(tests.error.status ?? 1)
```

- **`node:child_process`** — синхронные функции, как в node ([process.md](docs/process.md)):
  `execFileSync` даёт `Result` того, что программа напечатала (`string` с `encoding`, `Buffer` без
  него), `execSync` — то же через оболочку (`cmd.exe /d /s /c "…"` в Windows, `/bin/sh -c` в Linux;
  что подставлено в строку, оболочка и выполнит, как в node), `spawnSync` — `Result` объекта
  `{ status, signal, output, pid, stdout, stderr }`: код выхода он ошибкой не считает. Опции — те же, что у node: `cwd`, `env`,
  `input` (текст в кодировке вызова или байты), `stdio` (`"pipe"`, `"inherit"`, `"ignore"`, номер
  дескриптора или массив из трёх), `encoding`, `timeout` и `killSignal`, `maxBuffer` (1 МБ, сверх —
  `ENOBUFS`, программа убита), `shell` (`true` или путь), `windowsHide`, `windowsVerbatimArguments`,
  `detached`, `argv0`, `uid` и `gid` (в Windows — `ENOTSUP`, как в node). `encoding` и `stdio` пишут
  литералами: от них зависит тип результата, а поток, который не канал, даёт `undefined`.
- **Ошибки — `Err` с `ExecError<Out>`**, встроенным типом объекта: `code`, `message`, `errno?`,
  `syscall?`, `path?`, `spawnargs?`, `status?`, `signal?`, `stdout?` и `stderr?` типа `Out` (`string`
  или `Buffer`, по `encoding` вызова). Программа вышла не с 0 или её убили — `code: "exit"`, текст
  `Command failed: git status` и то, что программа написала в stderr (в node у этой ошибки `code`
  нет). Программа не запустилась (её нет — `ENOENT`; `.bat` и `.cmd` без `shell` — `EINVAL`, как в
  node с 2024 года), таймаут (`ETIMEDOUT`) и переполнение (`ENOBUFS`) — код системы, текст node
  `spawnSync git ENOENT`, `errno`, `syscall`, `path` и `spawnargs`; у `spawnSync` это тоже `Err`, а
  не поле `error`. Неверные значения опций — `Err` с кодом и текстом node (`ERR_INVALID_ARG_VALUE`
  для нулевого символа и пустого имени, `ERR_OUT_OF_RANGE`, `ERR_UNKNOWN_SIGNAL`), node бросает
  `TypeError` и `RangeError`. Что программа написала в stderr, `execSync` и `execFileSync` без
  опции `stdio` дописывают в stderr своей программы.
- **Запуск в Windows** — шаги node и libuv: аргументы в кавычках по правилам `quote_cmd_arg`
  libuv (сверено с node на сотнях случайных аргументов с пробелами, кавычками, `\`, `&` и буквами
  любых языков), поиск программы в её рабочем каталоге и в `PATH` её окружения с `.com` и `.exe`
  (без `PATHEXT`, как libuv), окружение отсортировано и дополнено `SYSTEMROOT` и другими нужными
  Windows переменными, всё — через «W»-функции. Оба вывода читаются одновременно, пока программа
  работает (каналы с overlapped I/O: один цикл ждёт процесс, чтение, запись `input` и таймаут),
  поэтому большой вывод или ввод не зависает; по таймауту — `TerminateProcess`, как в libuv.
  Ребёнку достаются только три его дескриптора, а программы без `detached` заканчиваются вместе с
  родителем (job object, как в libuv). Перед запуском с унаследованным потоком буфер stdout
  сбрасывается, чтобы вывод не перепутался. В Linux — `posix_spawn` и `poll()` (tz пока не собирает
  программы для Linux).
- Асинхронные `spawn`, `exec` и `execFile` — ошибка «пока нет» с подсказкой синхронной функции:
  они придут с `EventEmitter` и потоками ([process.md](docs/process.md), шаг 2). `fork` — ошибка:
  второго node в tz нет.

## События

```ts
import { EventEmitter, once } from "node:events"

class Server extends EventEmitter {             // карта событий — из emit ниже
  listen(port: int) {
    this.emit("listening", port)
    return this
  }
}
const server = new Server()
server.on("listening", (port) => console.log(port + 1))   // port: int
server.listen(8080)

const bus = new EventEmitter<{ user: [user: User]; done: [] }>()   // карта явно
setTimeout(() => bus.emit("user", ann), 10)
const [user] = await once(bus, "user")
```

- **Код — node.** `EventEmitter` написан на tz ([library/events/events.tz](library/events/events.tz)) переносом
  `lib/events.js` из node 24: `_events` — один слушатель или массив, `newListener` — до добавления,
  `removeListener` — после удаления, обёртка `once()` снимает себя перед вызовом, слушатель,
  добавленный во время `emit`, работает со следующего; `emit("error")` без слушателя
  возвращает `false` (в node — исключение `ERR_UNHANDLED_ERROR`); больше `getMaxListeners()` слушателей одного события — предупреждение
  `MaxListenersExceededWarning` в stderr с текстом node, на следующем ходу. Печать — как в node:
  `EventEmitter { _events: [Object: null prototype] { data: [Function (anonymous)] },
  _eventsCount: 1, _maxListeners: undefined, Symbol(shapeMode): false, Symbol(kCapture): false }`,
  у наследника его поля — перед символами; `Object.keys` и `JSON.stringify` — тоже как в node.
- **Карта событий** — тип: имя события → кортеж аргументов его слушателей (`end: []` — без них;
  `{ [name: string]: [msg: Msg] }` — любые имена, и имя может быть переменной). Каждый вызов
  программы проверяется по ней: событие, которого нет в карте, — ошибка с подсказкой похожего
  имени (`did you mean 'user'?`), аргументы `emit` и параметры слушателя — по кортежу, стрелка
  получает типы параметров оттуда; функция с `this` (`function (this: Server, port) {...}`)
  получает эмиттер как `this`, как в node; `on`, `once` и другие возвращают сам объект его типом
  (`server.on(...).listen(80)`). `newListener` и `removeListener` есть у каждого эмиттера: их
  слушатели получают `(eventName: string, listener: unknown)`. Код эмиттера видит слушателей как
  `unknown`, как видит их JS: компилятор превращает вызовы программы в вызовы этого кода, а `emit`
  — в копию `emit<A>` для кортежа события, которая зовёт слушателей напрямую, без памяти на вызов;
  массив слушателей копируется, только если его меняют во время `emit`.
- **Без карты** (`new EventEmitter()`, `class Server extends EventEmitter`) она выводится из всех
  `emit` программы, своя у каждого места, где эмиттер создан: через переменные, поля, параметры
  (`b: typeof bus`), методы и файлы. Для этого компилятор проверяет программу в несколько проходов,
  пока находятся новые события (слушатель может слать другое событие с тем, что получил), а ошибки
  (и о переполнении) сообщает и компилирует только последний; пакеты
  проекта читаются в каждом проходе, а `tz.json` и `tz.lock` — один раз. Слушатель события, которое никто не шлёт, — ошибка
  (`no code of the program emits 'usr' on this emitter; it emits 'user'`); одно событие с разными
  типами — ошибка, которая называет оба места; `int` и `double` дают `double`.
- **async-слушатель** разрешён: `emit` его не ждёт, его `Promise` отпускается; слушатель, который
  отдаёт `Result` или `Promise<Result>`, — ошибка: `Err` пропал бы. `captureRejections` нет:
  `Promise` не отклоняется.
- **Событие `error`** ([events.md](docs/events.md), раздел 4). Своя программа шлёт `error`, а
  слушателя `error` у этого эмиттера нет нигде — ошибка компиляции (в node такое `error`
  завершает программу). Функция, которая подписывается на события эмиттера, у которого есть
  `error` (потоки, сервер, свой эмиттер с `error` в карте), подписывается и на `error` того же
  эмиттера — в той же функции, на каждом пути (проверка по путям, как у полей конструктора);
  иначе ошибка с подсказкой. Это же делают `pipeline`, `finished`, `once()`, `on()` и
  `for await` над потоком: они ошибку забирают сами.
- **`once(emitter, name)`** — `Promise` кортежа аргументов следующего события; если эмиттер может
  испустить `error` — `Promise<Result<[...], E>>`, где `E` — тип ошибки события `error`: `Err`,
  если раньше пришло `error`. У `EventTarget` — `[event]`. Опции `signal` нет: `AbortSignal` в tz
  убран. Есть `EventEmitter.once`, `getEventListeners`, `listenerCount`, `getMaxListeners`,
  `setMaxListeners`, `EventEmitter.defaultMaxListeners`.
- **`for await (const event of on(emitter, "data", { close }))`** — события по одному, каждое —
  кортеж аргументов его слушателей (у эмиттера с `error` — в `Result`), как `on()` node: слушает с
  вызова, а события, которых ещё никто не ждёт, ждут в очереди; событие `error` даёт `Err` после
  событий до него и кончает цикл, событие из `close` — просто кончает его; когда цикл кончается (и
  `break`), слушатели снимаются. `on()` у `EventTarget` — ошибка «пока нет».
- **`EventTarget`, `Event`, `CustomEvent<T>`, `DOMException`** ([library/events/web.tz](library/events/web.tz))
  — глобальные, и тоже как в node: порядок слушателей (и добавленных во время `dispatchEvent`),
  `once`, `capture`, объект с `handleEvent()`, один слушатель — один раз,
  `preventDefault()`, `stopImmediatePropagation()`; ошибка слушателя — на следующем ходу.
  Слушатель, который берёт `(e: CustomEvent<int>) => ...`, получает только такие события (другое —
  `TypeError`), и `e.detail` у него — `int`; без аннотации — `Event`.
- **`AbortController` и `AbortSignal` нет** (убраны 28 октября, [async.md](docs/async.md),
  раздел 7): громоздкий костыль ради отмены, который в сообществе не любят, а сервер платил за
  сигнал каждого запроса втрое меньшей скоростью. Ожидания идут до конца; время `fetch`
  ограничивает его опция `timeout` (см. «Сеть»), сервер останавливает `listening.stop()`. Имена
  `AbortController` и `AbortSignal` — просто неизвестные. `DOMException` остался.
- То, что событие меняет само (`target()`, `currentTarget()`, `eventPhase()`, `isTrusted()`,
  `defaultPrevented()`, `srcElement()`), — методы (в node — свойства); `event.immediateStopped()`
  говорит, был ли
  `stopImmediatePropagation()`, а `event.dispatchOn(target, run)` — то, через что
  `dispatchEvent` рассылает событие (одно событие — одна рассылка за раз).
- **Память.** Эмиттер и его слушатели сами цикла ссылок не образуют (обёртка `once()` не держит
  эмиттер), но слушатель, который захватывает свой эмиттер
  (`this.on("x", () => this.y())` в его классе), — цикл, а циклы пока не освобождаются (см.
  «Строки и память»).

## Потоки

```ts
import { createReadStream, createWriteStream } from "node:fs"
import { Readable, Transform } from "node:stream"
import { pipeline } from "node:stream/promises"

const upper = new Transform({
  transform(chunk, encoding, callback) {                        // chunk: Buffer
    callback(undefined, chunk.toString().toUpperCase())
  },
})
const copied = await pipeline(createReadStream("in.txt"), upper, createWriteStream("out.txt"))
if (copied.isErr()) console.log(copied.error.message)          // нет файла, нет прав

for await (const text of createReadStream("log.txt", "utf8")) {  // Result<string, StreamError>
  if (text.isErr()) break
  console.log(text.value.length)
}
const users = Readable.from([ann, bob])                         // Readable<User>
```

- **Код — node.** `node:stream` написан на tz ([library/stream/stream.tz](library/stream/stream.tz)) переносом
  `lib/internal/streams` из node 24 поверх `EventEmitter`: `Readable` (поток и пауза, `read()`,
  `unshift`, событие `readable`), `Writable` (`cork` / `uncork`, `writev`, `final`), `Duplex`,
  `Transform`, `PassThrough`, `pipeline` и
  `finished` (с колбэком и из `node:stream/promises`, где `pipeline` берёт и async-генераторы),
  `compose`, `Readable.from` (массив, итератор, async-итератор, строка),
  `for await` (выход из цикла уничтожает поток), `destroy` с ошибкой, `construct`. «Не успеваю» —
  как в node: `highWaterMark` (16 КБ в Windows, 16 кусков в `objectMode`), `write()` даёт `false`,
  потом `drain`, `pipeline` ставит источник на паузу. Порядок событий и вызовов `read` и
  `write` — тот же: тесты сверены с node строка в строку. Функции `isReadable`, `isWritable`,
  `isDestroyed`, `isErrored` и `isDisturbed` есть и статическими методами `Stream`, как в node
  (`Stream.isErrored(s)`). `addAbortSignal` и опции `signal` нет, функции в `pipeline` получают
  только источник, без `{ signal }`.
- **Состояние потока закрыто** ([streams.md](docs/streams.md), раздел 6): у node оно открыто под
  именами на `_` (`_readableState`, `_writableState`), и программа, которая его меняет, ломает
  поток. В tz его нет снаружи: `r.readableState` — ошибка (`private`), а то, что поток меняет
  сам, читается методами (`r.readableLength()`, `r.destroyed()`, `w.writableEnded()`…; в node —
  свойства): методы читают состояние прямо, без копии в полях.
- **Ошибки — `Result`** ([streams.md](docs/streams.md), раздел 3): каждый кусок `for await` —
  `Result<T, StreamError>` (ошибка потока — `Err` в конце); `pipeline` и `finished` из
  `node:stream/promises` — `Promise<Result<...>>`; `end()` отдаёт `Promise<Result<void, StreamError>>`,
  который выполняется, когда поток дописан (колбэка у `end()` нет); `text`, `json`, `buffer`,
  `arrayBuffer` из `node:stream/consumers` — тоже `Result`. `pipe()` — ошибка с подсказкой
  `pipeline`: `pipe` не передаёт ошибку дальше. Слушатели событий потока требуют слушателя
  `error` (см. «События»).
- **Тип кусков** известен при компиляции ([streams.md](docs/streams.md)): `Readable<T>`,
  `Writable<T>`, `Duplex<W, R>`, `Transform<W, R>`. По умолчанию — `Buffer`; кодировка при создании
  (`{ encoding: "utf8" }`, `createReadStream(path, "utf8")`) — `string`; в `objectMode` — тип того,
  что программа кладёт в `push()`, пишет в `write()` и `end()` и отдаёт колбэку `transform`
  (выводится по всей программе, как карта событий из `emit`), или явно:
  `new Readable<User>({ objectMode: true, read() {} })`. Этот тип получают слушатель `data`,
  `read()`, `for await` и `onWrite`; `write("текст")` в поток байтов можно, как в node, а `push(42)`
  в него и `pipeline` потока `User` в поток `Buffer` — ошибки компиляции.
- **`setEncoding("utf8")`** меняет тип кусков у той же константы (или пути через `readonly`
  поля) с этого места до конца блока: дальше `on("data", s => ...)` получает `string`, а половина
  буквы, которую разрезал кусок, ждёт следующего. Если поток до этого уже ушёл — передан в функцию,
  сохранён, получил слушателей, его видит функция, написанная раньше, — или `setEncoding` стоит в
  `if` или цикле, это ошибка с подсказкой задать кодировку при создании.
- **Свои классы**: `class Upper extends Transform` заменяет `onRead`, `onWrite`, `onWritev`,
  `onTransform`, `onFlush`, `onFinal`, `onDestroy` и `onConstruct` (в node — `_read`, `_write`…;
  `tz port` переименовывает их сам) с теми параметрами, что передаёт поток; их типы можно не
  писать, а `onRead()` — без `size`, как в node. Это `protected`-методы: их зовёт сам поток, а
  снаружи `counter.onRead()` — ошибка, если класс не сделал свой метод открытым. Метод с другими
  параметрами — ошибка: иначе это был бы ещё один вариант перегрузки, которого поток не вызывает.
  Наследнику открыто ещё несколько операций: `pushChunk` (положить кусок, тип которого компилятор
  не знает; кусок не того типа кончает поток ошибкой), `readChunk`, `iterate<C>()`.
  Запись объектом, `new Readable({ read() {} })`, — как в node.
- **Отличия от node**: конец потока — `push(undefined)` (`null` в tz нет), `read()` без данных
  даёт `undefined`.
- **Файлы**: `createReadStream(path, options)` и `createWriteStream(path, options)` из `node:fs` —
  `ReadStream` и `WriteStream` с опциями node (`encoding`, `start` и `end`, `highWaterMark` —
  64 КБ, `flags`, `mode`, `fd`, `autoClose`, `emitClose`, `flush`), событиями `open`,
  `ready` и `close`, полем `path` и методами `fd()`, `bytesRead()`, `bytesWritten()`,
  `pending()` (в node — свойства). Чтение и запись — в
  пуле потоков рантайма (`runtime/library/fs/fs_async/fs_async.c`), как у node через libuv.
- **`process.stdin`** — `Readable` (байты, после `setEncoding` — строки): читает, только когда его
  слушают, из того же буфера, что `readLine` и `readFileSync(0)`. **`process.stdout`** и
  **`process.stderr`** — `Writable`, когда программа пользуется ими как потоком (`pipeline` в них,
  `write` с кодировкой или колбэком, `on`, передаёт их дальше): пишут сразу, в одном порядке с
  `console.log`. Так же — `stdin`, `stdout` и `stderr` из `node:process`. Простой
  `process.stdout.write(x)` строки или байтов потоков не подключает: с ними сборка дольше примерно
  на секунду.
- **`node:stream/consumers`** — `text`, `json` (даёт `Json`), `buffer`, `arrayBuffer`, все с
  `Result`;
  **`node:string_decoder`** — `StringDecoder` (`utf8`, `utf16le`, `latin1`, `base64`, `hex` и
  другие) с теми же строками, что у node, при любом разрезе кусков.
- **Скорость** (`bench/stream_lines`): 32 МБ, 800 тыс. строк через `createReadStream` → `Transform`
  на строки → `Transform` с `includes` и `toUpperCase` → `Writable` — 136 мс против 150 мс у Go
  (горутины и каналы) и 187 мс у node.
- **Пока нет**: web-потоки (`ReadableStream`, `toWeb` / `fromWeb`), `Readable.wrap`, операторы
  `Readable` (`map`, `filter`, `toArray`...), `Duplex.from`, `compose` функций, свои события у
  наследников потоков, асинхронные функции `node:child_process`; `Hash`, `Cipheriv` и другие
  объекты `node:crypto` — ещё не потоки.

## Криптография

```ts
import { createHash, createHmac, createCipheriv, randomBytes, scryptSync, sign, generateKeyPairSync } from "node:crypto"

createHash("sha256").update("abc").digest("hex")            // "ba7816bf..."
createHmac("sha256", secret).update(body).digest("base64")   // подпись вебхука
const key = scryptSync(password, salt, 32)                   // ключ из пароля
const iv = randomBytes(12)
const c = createCipheriv("aes-256-gcm", key, iv)              // Result<Cipheriv, CryptoError>
if (c.isErr()) return c                                        // ключ или IV не той длины
const head = c.value.update(text, "utf8")
const tail = c.value.final()                                   // Result<Buffer, CryptoError>
const tag = c.value.getAuthTag()                               // Result<Buffer, CryptoError>
const pair = generateKeyPairSync("ed25519")                    // Result<KeyPair, CryptoError>
if (pair.isOk()) console.log(sign(undefined, Buffer.from("message"), pair.value.privateKey))
```

- **Внутри — OpenSSL 3.5.9**, та же библиотека и линия версий, что в node 24
  ([crypto.md](docs/crypto.md)). Его собирает CMake при настройке сборки tz:
  [build.ps1](third_party/openssl/build.ps1) скачивает исходники, Strawberry Perl, NASM и jom по
  закреплённым версиям и хешам и собирает статически, с CRT `/MT`, через `clang-cl` того же LLVM
  (у него есть `__int128` для быстрого кода P-256 и Ed25519). Готовая сборка (~35 с) хранится в
  `%LOCALAPPDATA%\typezero\deps` под хешем рецепта, и следующие деревья сборки её копируют.
  Убрано то, что node 24 выключил сам (RC2, RC4, RC5, Blowfish, CAST, IDEA, SEED, MD4, MDC2,
  Whirlpool, одинарный DES — провайдер legacy), и то, чем API node не пользуется (SSLv3, DTLS,
  SRP, CMS, CMP, движки, устаревший API); 3DES, DH, DSA, SM2 / SM3 / SM4, RIPEMD-160, Camellia,
  ARIA остались.
- **Два слоя.** Хеш или шифр, названный в коде строкой, вызывается напрямую: программа получает
  его функции и ассемблер из `libcrypto_direct.lib` — объектов libcrypto, которые эти функции
  вызывают ([direct.txt](third_party/openssl/direct.txt)), без общих таблиц OpenSSL. Хеши (md5,
  sha1, sha224 / 256 / 384 / 512, sha512-224 / 256, sha3-*, shake128 / 256, ripemd160, sm3,
  md5-sha1, blake2b512, blake2s256), HMAC, PBKDF2, HKDF, scrypt, Argon2, AES-GCM / CBC / CTR и
  ChaCha20-Poly1305 — этот слой; случайные числа берутся у системы (`BCryptGenRandom`). Имя из
  переменной, ключи, подписи, ECDH, сертификаты и остальные шифры (CCM, OCB, Camellia, 3DES...)
  — весь `libcrypto.lib`. Без криптографии в exe ничего из этого нет. Размер: `console.log` —
  171 КБ, плюс SHA-256 с именем в коде — 287 КБ (сам SHA-256 с ассемблером — около 30 КБ,
  остальное — кодировки `hex` и ошибки рантайма), AES-GCM — 1 МБ (почти всё — ассемблер AVX-512
  OpenSSL, на котором и держится скорость), всё OpenSSL — 4,1 МБ.
- **Ошибки — `Result<T, CryptoError>`**, где `CryptoError = { code: string; message: string }`, с
  кодами и текстами node (`ERR_CRYPTO_INVALID_KEYLEN`, `ERR_OSSL_BAD_DECRYPT` и `error:1C800064:
  Provider routines::bad decrypt`...; где у node кода нет, у tz свой: `ERR_CRYPTO_AUTH_FAILED` —
  тег AES-GCM не совпал). `Result` дают: `createCipheriv` и `createDecipheriv` (длина ключа, IV и
  тега), `final()` и `getAuthTag()`, `createPrivateKey` и `createPublicKey`, `key.export()`,
  `sign` и `verify` и одноимённые методы `Sign` и `Verify`, `generateKeyPairSync`,
  `diffieHellman`, у `ECDH` — `computeSecret`, `setPrivateKey`, `getPublicKey`, `getPrivateKey`,
  у сертификата — `checkPrivateKey` и `verify`, а ещё любая функция с именем алгоритма из
  переменной (`createHash(name)`, `createHmac(name, key)`, `pbkdf2Sync(…, digest)`...) и `createHmac`
  с `KeyObject`. Сертификат читает `X509Certificate.parse(pem)`; `new X509Certificate(...)` —
  ошибка с этой подсказкой. Числа вне диапазона (`randomBytes(-1)`, `randomInt(5, 5)`) — ошибки
  компиляции, пока проверка в `if` не докажет диапазон (в node — `RangeError`).
- **Хеш не закрывается** (как в Python и Go): `digest()` можно позвать снова или дописать данные
  после него; в node второй `digest()` бросает `Digest already called`. **Ошибки порядка у шифра**
  (`setAAD` у шифра без AEAD, тег не той длины, `update` после `final`) запоминаются, и их отдаёт
  ближайший `final()`; в node они бросают сразу.
- **Как в node**: те же имена алгоритмов и их синонимы (`"SHA-256"`, `"RSA-SHA256"`, OID),
  `getHashes()`, `getCiphers()` и `getCurves()` — те же списки. Выход `update()` у шифра
  такой же по кускам: дешифратор CBC держит последний блок до `final()`. Тесты сверяются с node
  байт в байт: хеши, HMAC, KDF, шифры с ключом и IV, подписи Ed25519 и RSA PKCS#1 v1.5, X25519,
  ECDH, PEM, DER и JWK; AES — и на всех путях кода OpenSSL (AVX-512 с VAES, только AES-NI, без них;
  `OPENSSL_ia32cap` их выключает).
- **Имя алгоритма в коде проверяется при компиляции**: `createHash("sha265")` — «unknown hash»,
  `createHash("md4")` и `createCipheriv("des-cbc", ...)` — ошибки «выключен в node 24» (node
  бросил бы `ERR_OSSL_EVP_UNSUPPORTED` при работе), `createHmac("shake128", ...)` — у SHAKE нет
  длины для HMAC, `aes-*-ccm` без `authTagLength` — ошибка, `createECDH("P-999")` — «unknown
  curve». Имя из переменной проверяет OpenSSL при работе, и вызов даёт `Result`.
- **Функции с колбэком** — ошибки с подсказкой: `import { scrypt }` → `scryptSync` (так же
  `pbkdf2`, `hkdf`, `argon2`, `randomFill`, `generateKeyPair`, `generateKey`), `randomBytes(n, cb)` →
  `randomBytes(n)`, так же `randomInt` и `sign` / `verify` с колбэком.
- **Объекты, которые в node ещё и потоки** (`Hash`, `Hmac`, `Cipheriv` / `Decipheriv`,
  `Sign` / `Verify`): `write`, `pipe`, `on` и другие методы потоков — ошибка «пока нет»
  с подсказкой `update()` и `digest()` / `final()`: потоков в tz ещё нет
  ([streams.md](docs/streams.md)). Объединение двух таких типов (`Hash | Hmac`) — ошибка:
  их объекты не различить при работе, как объекты `Intl`.
- **Объекты**: `Hash` (`update`, `digest`, `copy`), `Hmac`, `Cipheriv` / `Decipheriv` (`update`,
  `final`, `setAAD`, `getAuthTag`, `setAuthTag`, `setAutoPadding`), `KeyObject` (`type`,
  `asymmetricKeyType`, `symmetricKeySize`, `export`, `equals`), `Sign` / `Verify`, `ECDH`
  (`generateKeys`, `computeSecret`, `getPublicKey` в формате `uncompressed` / `compressed` /
  `hybrid`, `getPrivateKey`, `setPrivateKey`), `X509Certificate` (`subject`, `issuer`,
  `subjectAltName`, `infoAccess`, `validFrom`, `validTo`, `validFromDate`, `validToDate`,
  `serialNumber`, `fingerprint*`, `keyUsage`, `ca`, `raw`, `publicKey`, `checkHost`, `checkEmail`,
  `checkIP`, `checkIssued`, `checkPrivateKey`, `verify`, `toString`). Их импортируют и как типы:
  `const k: KeyObject = ...`. Печатаются как `Hash {}` и `SecretKeyObject {}` (node показывает
  своё внутреннее устройство). Ключи: `createSecretKey`, `createPublicKey` и `createPrivateKey`
  из PEM, DER (`{ format: "der", type: "spki" }`), JWK (`{ key: jwk, format: "jwk" }`, `jwk` —
  `Json`) и зашифрованного PKCS#8 с `passphrase`, `export` в те же форматы, `generateKeySync`,
  `generateKeyPairSync` для `rsa`, `rsa-pss`, `dsa`, `ec`, `ed25519`, `ed448`, `x25519`, `x448` (с
  `publicKeyEncoding` / `privateKeyEncoding` результат — строка, `Buffer` или `Json`), `sign` /
  `verify` с `padding`, `saltLength` и `dsaEncoding: "ieee-p1363"`, `diffieHellman`, `constants`.
  Глобальный `crypto` Web Crypto — `crypto.randomUUID()`, `crypto.getRandomValues(a)` и
  `crypto.subtle` (ниже), в том числе как `globalThis.crypto`.
- **Скорость** (Ryzen 9 9950X3D, Windows, [bench](bench/README.md)): AES-256-GCM — 22 ГБ/с (Go —
  7,5 ГБ/с, node на той же программе — в 12 раз медленнее tz), ChaCha20-Poly1305 — 5,9 ГБ/с (Go
  с `golang.org/x/crypto` — 3,5), SHA-256 — 2,7 ГБ/с, как у Go; подпись RSA-2048 — 5 600 в
  секунду (Go — 1 800), ECDSA P-256 — 79 000, как у Go, X25519 — 52 000 (Go — 40 000), Ed25519 —
  55 000 (Go — 82 000: у Go здесь своя быстрая реализация).
- **Лицензии**: `tz licenses app.exe` печатает лицензии чужого кода в exe — OpenSSL (Apache 2.0),
  если он там есть, и Node.js (MIT) и V8 (BSD), по которым написаны части рантайма, — их отдают
  вместе с exe (`tz licenses app.exe > LICENSES.txt`). Список пишется в exe при сборке (секция
  `.tzlic`), тексты лежат рядом с `tz.exe` в `licenses`. ICU и библиотеки Windows не вшиваются.
- Пока нет: `publicEncrypt` / `privateDecrypt`,
  `createDiffieHellman`, `generatePrime`, `checkPrime`, `asymmetricKeyDetails` (у node в нём
  `bigint`), `KeyObject.from`, `ECDH.convertKey`. Опции пишут объектом-литералом в вызове, как у
  `node:fs`; `argon2Sync` принимает литерал без spread.

**Web Crypto** — `crypto.subtle`, как в node, на том же OpenSSL; написан на tz
([library/crypto/webcrypto.tz](library/crypto/webcrypto.tz), по коду node 24):

```ts
const key = await crypto.subtle.importKey("raw", secret, { name: "HMAC", hash: "SHA-256" }, false, ["sign"])
if (key.isErr()) return key                                            // Result<CryptoKey, SubtleError>
const mac = await crypto.subtle.sign("HMAC", key.value, data)          // так пишут hono и jose
const pair = await crypto.subtle.generateKey({ name: "ECDSA", namedCurve: "P-256" }, true, ["sign", "verify"])
const sealed = await crypto.subtle.encrypt({ name: "AES-GCM", iv }, aesKey, data)
const jwk = (await crypto.subtle.exportKey("jwk", publicKey)).unwrapOr({ kty: "EC" })
```

- **Что есть**: `digest` (SHA-1, SHA-256, SHA-384, SHA-512), `generateKey`, `importKey` /
  `exportKey` в `raw`, `pkcs8`, `spki` и `jwk`, `sign` / `verify` (HMAC, ECDSA, Ed25519,
  RSASSA-PKCS1-v1_5, RSA-PSS), `encrypt` / `decrypt` (AES-GCM, AES-CBC, AES-CTR, RSA-OAEP),
  `deriveBits` / `deriveKey` (PBKDF2, HKDF, ECDH, X25519), `wrapKey` / `unwrapKey` (AES-KW и шифры
  выше). `CryptoKey` (`type`, `extractable`, `algorithm`, `usages`) печатается как в node; ключ
  OpenSSL в нём закрыт, а операции — методы ключа (`key.sign(alg, data)`, `verify`, `encrypt`,
  `decrypt`, `deriveBits`, `deriveKey`, `wrap`, `unwrap` — отдаёт данные ключа, `export`): они
  сами проверяют `usages` и `extractable`, а `crypto.subtle` зовёт их. Типы
  lib.dom — `CryptoKey`, `CryptoKeyPair`, `JsonWebKey`, `BufferSource`, `KeyUsage`,
  `HmacImportParams` и другие словари — видны без импорта; все словари алгоритмов — один тип
  `Algorithm` с необязательными полями, поэтому литерал алгоритма в переменной или в `return`
  подходит без аннотаций. `generateKey` с алгоритмом в вызове даёт `CryptoKey` или `CryptoKeyPair`,
  `exportKey("jwk")` — `JsonWebKey`, иначе `ArrayBuffer`. JWK из текста — `JSON.parse` и своя
  проверка полей (литерал `JsonWebKey`).
- **Ошибки — `Promise<Result<T, SubtleError>>`**, а не отказ `Promise`: `SubtleError = { kind:
  "SUBTLE"; code; message }`. `code` — наш вид имени `DOMException` или `TypeError`, которыми
  node отвергает (29 октября): `NotSupportedError` → `"ERR_NOT_SUPPORTED"`, `DataError` →
  `"ERR_INVALID_DATA"`, `OperationError` → `"ERR_OPERATION_FAILED"`, `InvalidAccessError` (и
  `InvalidAccessException` у `key is not extractable`) → `"ERR_INVALID_ACCESS"`, `SyntaxError` →
  `"ERR_INVALID_KEY_USAGE"`, `TypeError` → `"ERR_INVALID_ARG_TYPE"`, `RangeError` →
  `"ERR_OUT_OF_RANGE"`. Текст — как в node (`Unrecognized algorithm name`, `Unable to use this key
  to sign`, `Invalid keyData`, `Usages cannot be empty when creating a key.`, `Failed to normalize
  algorithm: ... because 'hash' is required in 'HmacImportParams'.`).
- **Как в node**: имена алгоритмов без учёта регистра; ключи JWK — в порядке node. Тесты
  сверяются с node байт в байт: ключи и векторы фиксированы, у случайного печатаются длины и
  результаты `verify`.
- **Работа — в пуле потоков**, как у node в libuv: хеши, подписи, шифры, KDF и генерация пар ключей
  не держат поток программы. PBKDF2 на 3 млн итераций идёт 0,2 с, а таймеры тикают; четыре таких
  через `Promise.all` идут параллельно. Данные копируются при вызове, как требует спецификация.
  `importKey` и `exportKey` — сразу, как в node. Результат приходит в следующем обороте цикла:
  микрозадачи, поставленные раньше, выполняются до него.
- **Ошибки компиляции** — то, что node отверг бы при работе, если это видно в коде: в вызове или в
  константе (`const algorithm = { name: "HMAC", hash: "SHA-256" }`, как у hono). Неизвестный
  алгоритм или хеш, экспериментальный алгоритм node 24, алгоритм не той операции (`digest("HMAC")`),
  пропущенный параметр с примером (`'iv: new Uint8Array(16)'`), неверная длина ключа AES или тега
  AES-GCM, неизвестная кривая, недопустимые usages (`'deriveBits' is no usage of a public ECDH key;
  it takes none: pass []`), пустые usages, формат, в котором семейство не импортируется, данные не
  того вида для формата, `new CryptoKey()`. Остальное — при выполнении, как в node.
- **Два слоя**: что ключ умеет, решает его семейство, а `importKey` / `generateKey` / `deriveKey` /
  `unwrapKey` с алгоритмом в коде вызывают код только своего семейства. Поэтому программа с HMAC,
  AES, PBKDF2 и HKDF берёт прямой слой OpenSSL (`digest` — 382 КБ, HMAC — 472 КБ, AES-GCM —
  1,2 МБ), а с асимметричными ключами — весь (4,3 МБ).
- **Скорость** ([bench](bench/README.md)): 2 ГиБ SHA-256 кусками по 16 МиБ через `Promise.all` —
  0,65 с (node — 0,89 с), 200 000 подписей HMAC — 0,37 с (node — 1,16 с); у Go с горутинами на
  32 ядрах — 0,35 и 0,22 с: пул tz, как у node, — 4 потока (`UV_THREADPOOL_SIZE`).
- Пока нет: экспериментальные алгоритмы node 24 (SHA-3, cSHAKE, Ed448, X448, AES-OCB,
  ChaCha20-Poly1305, KMAC, Argon2, ML-DSA, ML-KEM) — ошибка компиляции с именем в коде и
  `ERR_NOT_SUPPORTED` иначе; форматы `raw-public` / `raw-secret` / `raw-seed`, `getPublicKey`,
  `SubtleCrypto.supports`, `KeyObject.toCryptoKey`; `webcrypto` и `subtle` из `node:crypto` —
  ошибка с подсказкой взять глобальный `crypto`. `CryptoKeyPair` печатается без
  `[Object: null prototype]`.

## Модули

Каждый `.tz`-файл — модуль: его имена видны только ему, а другим — то, что он экспортирует и
они импортируют. `tz build main.tz` читает `main.tz`, находит по импортам остальные файлы и
собирает всё в один exe; файлы читаются при компиляции, рядом с exe они не нужны.

```ts
// geometry/index.tz — «витрина» каталога
export { area } from "./circle"   // реэкспорт по имени
export * from "./rect"            // всё, что экспортирует rect.tz

// main.tz
import { add, PI } from "./math"
import { add as plus } from "./math.tz"   // тот же файл, другое имя
import { area, Rect } from "./geometry"   // типы импортируются как всё остальное
```

- Путь — только относительный: `"./math"` и `"./math.tz"` — это `math.tz`; каталог — его
  `index.tz` (если рядом нет `geometry.tz`). Регистр букв в пути должен совпадать с именем файла:
  в Windows файл нашёлся бы и так, а в Linux — нет. Файлы tz — только `.tz`: `"./math.ts"` и
  `"./math.js"` — ошибка с подсказкой `import './math' or './math.tz'`, а если рядом лежит
  `math.ts` — ещё и `rename math.ts to math.tz`. Node.js запускает файлы `.tz` только копией с
  расширением `.ts`, поэтому тесты, которые сверяются с node, импортируют с расширением
  (`"./math.tz"`): в копии оно становится `.ts`.
  Типы node импортирует только через `import type`, которого здесь нет, поэтому программы,
  которые импортируют типы, с node не сверяются.
- Импортировать можно функции, переменные, типы, `interface` и `enum`; одно имя — и тип, и
  значение, если файл объявил и то и другое. Импортированное только читают: `counter = 5` —
  ошибка, а изменения, которые делает сам файл (`increment()`), видны сразу, как в JS: это та же
  переменная, а не копия.
- Порядок загрузки — как в JS: сначала файлы, которые модуль импортирует (по порядку импортов),
  потом он сам, каждый файл один раз; код верхнего уровня выполняется при загрузке. Функции,
  типы и `enum` всех файлов готовы ещё при компиляции и от порядка не зависят.
- Циклические импорты разрешены: `a.tz` импортирует `b.tz`, а `b.tz` — `a.tz`. Тогда `b.tz`
  загружается раньше `a.tz`, и его код верхнего уровня не может читать переменные `a.tz`: в node
  это `ReferenceError` при запуске, здесь — ошибка компиляции с цепочкой импортов, прямо
  (`const s = "b sees " + aName`) или через вызванную функцию (`const v = getA()`):
  `b.tz reads 'aName' of a.tz while it loads, but a.tz is not loaded yet: main.tz -> a.tz ->
  b.tz -> (a.tz already started)`. Внутри функций и стрелок, которые вызовут позже, читать их
  можно. Если для типа стрелки нужен тип переменной файла, который загрузится позже, компилятор
  проверяет этот файл заранее, до её объявления.
- Та же проверка и внутри одного файла: код верхнего уровня, который вызывает функцию раньше, чем
  объявлена глобальная переменная, которую она читает, — ошибка
  (`'total' uses 'base' before it is declared`). Вызовы через значения функций (поле, параметр,
  элемент массива) не прослеживаются.
- `export *` из двух файлов с одним именем — ошибка, как в tsc; явный экспорт имени важнее
  звёздочки, одно и то же объявление через два пути — не конфликт.
- Из импортированных файлов в exe попадают только функции, до которых программа может дойти;
  главный файл компилируется целиком. Функции и глобальные переменные других файлов называются в
  IR по файлу: `tz.geometry.circle.area`.
- Ошибки с подсказкой: `import x from`, `export default`, `import * as`, `export * as`,
  `import "./x"` (ради побочных действий), `import type` и `{ type T }`, `import x = require()`,
  `export =`, `namespace`, `declare module` / `declare global` / `declare`, `/// <reference>`,
  `require`, `module.exports`, динамический `import()`, `import.meta`. Пакеты (`"zod"`) — см.
  «Пакеты»; `import` из `"neverthrow"` — ошибка, `Result` встроен (см. «Result»); встроенные
  модули node и tz — см. «Встроенные модули».

## Пакеты

Пакет — исходники на TS, которые компилируются вместе с программой
([docs/packages.md](docs/packages.md)). Проект — каталог с `tz.json`:

```json
{
  "name": "app",
  "main": "main.tz",
  "dependencies": {
    "ms": "file:../ms",
    "greet": "jsr:@tz/greet@^2.0.0",
    "lib": "github:user/lib@v1.2.0"
  }
}
```

```ts
import { ms } from "ms"                 // по имени из dependencies
import { red } from "@std/fmt/colors"   // путь внутри пакета — из его exports
```

- **Откуда.** `"jsr:@scope/name@<диапазон>"` — JSR; `"github:user/repo@<тег>"` и
  `"git:<адрес>@<тег>"` — git (тег, ветка или коммит; без него — ветка по умолчанию);
  `"file:../lib"` или `"../lib"` — каталог, как он есть. `npm:` и версия без источника (`"^4"`) —
  ошибки с подсказкой.
- **tz.json**: `name`, `version`, `main` (что собирают `tz build` и `tz run` без файла и что
  импортирует `"пакет"`), `exports` (`{ ".": "./mod.tz", "./mini": "./mini.tz" }`: импортировать
  можно только эти пути) и `dependencies`. Комментарии и запятая в конце разрешены, неизвестное
  поле — ошибка. Имя пакета — `zod` или `@scope/name`, но не имя встроенного модуля (`fs`, `path`,
  `util`...). Пакет без `tz.json` говорит о себе в `jsr.json` или `deno.json`; если и их нет, его
  вход — `mod.tz`, `index.tz` или `src/index.tz`.
- **Команды.** `tz init [каталог]` — `tz.json`, `main.tz` и `.gitignore`. `tz add zod` — пакет,
  адаптированный для tz (`jsr:@tz/zod`, импорт — `"zod"`), с диапазоном `^` от новейшей версии;
  `tz add @std/path@^1`, `tz add github:user/repo@v1.0.0`, `tz add ../lib` — остальные (имя для
  импорта — `name` из `tz.json` пакета или имя репозитория). `tz remove zod`, `tz update [имя...]` —
  новейшие версии, которые разрешает `tz.json`. Если версии не сходятся, `tz.json` остаётся как был.
  `tz run` без файла запускает `main`; аргументы программы — всё, что не `.tz`, и всё после `--`.
  `tz build` без файла называет exe по `name`.
- **tz.lock** лежит рядом с `tz.json`: точная версия (у git — коммит) и хеш исходников каждого
  пакета, с зависимостями зависимостей; локальных каталогов в нём нет. Если `tz.lock` подходит к
  `tz.json`, сборка берёт ровно его и сеть не нужна; если нет (новый пакет, другой диапазон),
  `tz build` сам выбирает версии и переписывает `tz.lock`, как cargo. Хеш — SHA-256 списка файлов
  пакета с их SHA-256: каждый файл JSR сверяется с хешем из реестра при скачивании, а исходники в
  кэше — с `tz.lock` при каждой сборке.
- **Версии** — semver, диапазоны как в npm и JSR: `^1.2`, `~1.2.3`, `1.x`, `>=1 <2`, `1.2 - 1.4`,
  `||`. Пререлиз берётся, только если диапазон его называет, отозванная (yanked) версия — никогда.
  Каждый пакет — в одной версии на программу; несовместимые диапазоны — ошибка со списком, кто чего
  хочет:
  ```
  tz: no one version of jsr:@std/fmt fits every package that needs it:
    tz.json wants ~1.0 (newest: 1.0.0)
    jsr:@tz/old 1.0.0 wants ^2.0.0 (newest: 2.0.0)
  ```
- **JSR** — как в его [API](https://jsr.io/docs/api): `meta.json`, `<версия>_meta.json` и файлы по
  одному. Зависимости пакета JSR — импорты `"jsr:@scope/name@^1/путь"` в файлах, до которых
  доходят его `exports` (так JSR пишет их при публикации; импорты тестов пакета не в счёт). Адрес
  реестра — `TZ_JSR_URL` (по умолчанию `https://jsr.io`), в том числе `file://` — зеркало на
  диске. HTTP — WinHTTP в Windows, программа `curl` в остальных системах.
- **git** — программа `git` из `PATH`: зеркало репозитория (`git clone --mirror`), коммит тега или
  ветки, файлы коммита без `.git`; перевод концов строк выключен, чтобы хеш везде был один.
- **Кэш** общий для всех проектов: `TZ_CACHE`, иначе `%LOCALAPPDATA%\tz\cache` в Windows и
  `~/.cache/tz` в остальных системах; внутри `jsr/@scope/name/<версия>/`,
  `git/<репозиторий>.git` и `git/<репозиторий>/<коммит>/`. Скачанное не меняется: оно пишется во
  временный каталог и переименовывается целиком, поэтому несколько `tz` сразу не мешают друг другу.
- **Импорт.** Имя без точки — пакет из `dependencies` того пакета, чей это файл (у каждого пакета
  они свои); `"zod/mini"` — путь из его `exports`, а без `exports` — файл пакета. Файлы пакета
  называются по нему (`@std/fmt/colors.tz`), пакет из JSR или git импортирует только свои файлы и
  зависимости. `"jsr:..."` в коде проекта — ошибка с подсказкой `tz add`. Правила tz для пакетов те
  же, что для программы.
- **`tz port <пакет>`** — копия TS-библиотеки (каталог, `jsr:`, `github:` или `git:`) для
  адаптации под tz: файлы, до которых доходят её экспорты, лицензия, README и новый `tz.json`. Сам
  заменяет `===` / `!==` на `==` / `!=`, `null` и `void 0` на `undefined` (кроме `Json`, который
  держит `null` сам: `Json | null` становится `Json`), `var` на `let`, `any` на
  `unknown`, `export default` и импорт по умолчанию — на имена (безымянная функция получает имя
  файла), `import * as ns` — на имена, которые файл берёт из `ns`, `import type` — на обычный
  импорт, `"jsr:@std/path@^1/join"` — на `"@std/path/join"` с зависимостью в `tz.json`, `+` со
  строкой — на шаблон (`n + " items"` → `` `${n} items` ``, `a + b + "!"` → `` `${a + b}!` ``),
  `#x` — на `private x`.
  Остальное печатает списком `файл:строка:столбец: подсказка`: `as`, `satisfies`, геттеры и
  сеттеры, прототипы, `arguments`, `require`, перегрузки функций, типы-шаблоны и `Uppercase<>`,
  пакеты npm, импорты ради побочных действий. Копия — в каталог с именем пакета или в `-o`;
  `--in-place` меняет сам каталог; `--entry` называет вход, если пакет его не называет.
- **Адаптированные пакеты** — [tests/packages/ms](tests/packages/ms): ms 4.0.0 после `tz port` и
  ручных правок (они перечислены в начале файла); его использует тест `tests/run/packages`.
  [tests/packages/uuid](tests/packages/uuid) — uuid 14.0.3 (`v1`, `v3`, `v4`, `v5`, `v6`, `v7`,
  `NIL`, `MAX`, `parse`, `stringify`, `validate`, `version`, `v1ToV6`, `v6ToV1`; правки — в начале
  `src/index.tz`): перегрузок у функций нет, поэтому `v4<TBuf extends Uint8Array = never>(options?,
  buf?: TBuf, offset?)` даёт `string | TBuf` — без буфера это `string`, а с буфером `string |
  Uint8Array`; `v3.DNS` (свойство функции) — экспорт `DNS` и `URL`. Тест
  `tests/run/packages_uuid` печатает ровно то же, что оригинальный uuid 14.0.3 в node.

## JSON

`Json` — встроенный тип значения JSON: `null`, `boolean`, число, строка, массив `Json` или объект
из них ([modules.md](docs/modules.md), раздел 5). Его даёт `JSON.parse(text)` (в `Result`).

```ts
const config = JSON.parse(embedText("./config.json"))   // текст вшит и проверен при компиляции
const parsed = JSON.parse(text)                   // Result<Json, JsonError>
if (parsed.isErr()) return parsed
const user = parsed.value                         // Json
console.log(user.address.city, user.tags[0])      // Json | undefined, как в JS
const name = user.name
if (typeof name == "string") console.log(name.toUpperCase())    // здесь name — string
const tags = user.tags
if (Array.isArray(tags)) for (const t of tags) console.log(t)    // tags — Json[]
const patch: Json = { phone: null, tags: ["a", 1] }               // null бывает только в Json
```

- **Чтение** — как в JS: `j.name`, `j["content-type"]`, `j[0]`, `j.length` у массива и строки дают
  `Json | undefined` (ключа нет — `undefined`), и цепочка `j.users[0].name` пишется без `?.`.
  Ключ у `undefined`, `null` и у значения без ключей — `undefined` (в JS — `TypeError: Cannot
  read properties of undefined`), поэтому `?.` не нужен.
- **Сужение**: у переменной или параметра типа `Json` проверка `typeof x == "string"`, `"number"`,
  `"boolean"` даёт `string`, `double`, `boolean`, `Array.isArray(x)` — `Json[]` (тот же массив:
  `push` виден через `x`), `x != null` и `x != undefined` убирают `undefined`. Методов у `Json`
  нет, пока проверка не показала, что в нём: `j.items.map(...)` — ошибка с подсказкой.
- **`null`** — значение только `Json`: `const j: Json = null`, `x == null`. `x == null` и
  `x == undefined` правдивы для обоих, как в JS; `??` и `??=` заменяют и `null`. В остальном языке
  `null` по-прежнему ошибка; `Json | null` — просто `Json`.
- **`==`** — как `===`: `j.age == 30`, `j.role == "admin"`, `j.ok == true` правдивы только для
  `Json` того же вида и значения; два `Json` равны, если это одно значение (объект или массив — тот
  же самый). `switch` по `Json` сравнивает так же, `case null:` — только `null`.
- **В `Json` превращаются** строки, числа, `boolean`, `Json[]` (тот же массив) и литералы там, где
  ждут `Json`: `{ phone: null, tags: [1, "a"] }`; ключ в скобках — любая строка, значение
  `undefined` выбрасывает ключ. `int[]` или объект своего типа — ошибка: в JS `Json` был бы тем же
  значением, а здесь мог бы быть только копией. `decimal` — ошибка с подсказкой `String(x)`.
- **Без проверки нельзя**: условие (`if (j.debug)` — там может быть `0`), строку (`` `${j}` ``,
  `String(j)`), арифметику; ошибки подсказывают `!= null`, `== true`, `typeof`, `??`,
  `JSON.stringify`.
- **Запись**: `j.x = v`, `j["x"] = v`, `j[i] = v`; `j.x = undefined` убирает ключ. Элемент массива
  за его длиной дописывает до себя `null` (дыр нет); запись ключа в строку, число, `null` или в
  массив по ключу, который не индекс, ничего не делает (в JS — `TypeError`). `Object.keys`,
  `Object.values`, `Object.entries` и `for...in` — как в JS, но `Object.keys(null)` — пустой
  массив, а `for...in` по `null` и `undefined` не делает ни шага.
- **Печать** — как в node, с тем же порядком ключей: ключи-индексы (`"1"`, `"2"`) первыми по
  возрастанию, остальные в порядке появления; повторный ключ в тексте заменяет значение, но не место.
- **`JSON.parse(text)`** читает по грамматике V8 и отдаёт `Result<Json, JsonError>`. Ошибка —
  встроенный тип `JsonError = { kind: "JSON"; code: "ERR_INVALID_JSON" | "ERR_CIRCULAR_STRUCTURE"; message: string }`:
  у неразобранного текста `code` — `"ERR_INVALID_JSON"`, `message` — текст `SyntaxError` из node
  (`Unexpected token 'x', "[1, x]" is not valid JSON`, `Expected ',' or '}' after property value
  in JSON at position 13 (line 3 column 3)`). Второй аргумент (reviver) —
  ошибка компиляции. `Response.json()` и `Request.json()` отдают
  `Promise<Result<Json, BodyError>>`, `json()` из `node:stream/consumers` —
  `Promise<Result<Json, StreamError | JsonError>>`.
- **`JSON.stringify(x, null, space)`** пишет любое значение: `Json`, объекты и экземпляры классов
  (поля в порядке печати, и приватные, без методов и функций), массивы, кортежи (как массивы),
  `decimal` — строкой (`"19.99"`, как BigNumber.js), `Result` — объектом со значением или
  ошибкой (`{"value":1}`, `{"error":"x"}`), `Record` — объектом, `Map`, `Set`, итератор и регулярка —
  `{}`, совпадение регулярки — массив его
  групп (`["12","1",null]`, без `index` и `groups`, как в node), дата — ISO-текстом, как её
  `toJSON()`; поле с `undefined` пропускается, в массиве `undefined` и
  функция — `null`. Для `T | undefined` и функции результат —
  `string | undefined`, иначе `string`; если тип может содержать себя (`Json`, класс с полем своего
  вида), — `Result` с `JsonError`. `space` — число (до 10) или строка. Вместо replacer —
  только `null` или `undefined`; класс с методом `toJSON` — ошибка компиляции: tz сам не
  вызывает код программы. Значение, которое содержит себя, — `Err` с кодом `"ERR_CIRCULAR_STRUCTURE"` и текстом
  `Converting circular structure to JSON` (без строк node о том, где круг).
- **Свой тип из `Json` — проверкой и литералом** ([narrowing.md](docs/narrowing.md), раздел 3;
  с 29 октября): `JSON.parse<T>`, `fromJson<T>`, `fromJson(cls, j)` и `embedJson` убраны — у JSON
  нет особых прав, компилятор не пишет за программиста код перевода. Программа проверяет `Json`
  своей функцией, факты проверки переходят в `if` (и после раннего `return` в ветке `false`), а
  поля `Json` сужаются, как переменные, — и вложенные (`d.owner.name`), и массивы:
  ```ts
  function isUserJson(data: Json): boolean {
    return typeof data == "object" && typeof data.name == "string" && typeof data.age == "number"
  }
  const parsedResult = JSON.parse(text)
  if (parsedResult.isErr()) return err(parsedResult.error.message)
  const data = parsedResult.value
  if (!isUserJson(data)) return err("not a user")
  const user: User = { name: data.name, age: data.age }   // data.name — string, data.age — double
  ```
- **`embedText(path)`** читает файл при компиляции; путь — строка-литерал относительно файла с
  вызовом (`"./config.json"`, `"../data/page.html"`), в exe лежит текст. `JSON.parse` текста,
  известного при сборке (литерал или `embedText`), проверяется при сборке: не JSON — ошибка
  компиляции со строкой и столбцом (`JSON.parse(embedText("./config.json"))`). `Result`
  остаётся. `embedBytes` даёт `Uint8Array` (см. «Байтовые массивы»).
- **В памяти** `Json` — ссылка на узел рантайма со счётчиком ссылок, `Json | undefined` — та же
  ссылка или ноль; `null`, `true` и `false` — по одному узлу на программу. Объект хранит ключи в
  порядке добавления и, когда их больше восьми, хеш-индекс; массив — обычный `Json[]`. Разбор и
  освобождение идут без рекурсии на любой глубине.

## Генераторы и итераторы

```ts
function* evens(limit: int) {
  for (let i = 0; i < limit; i += 2) yield i
}
let sum = 0
for (const e of evens(1000)) sum += e    // обычный цикл, генератора как объекта нет

function* talk(): Generator<string, void, string> {
  const name = yield "как тебя зовут?"    // то, что передали в next(x)
  console.log("привет,", name)
}
```

**В цикле.** Генератор, который вызывают прямо там, где перебирают значения (`for...of`, spread,
`Array.from`, деструктуризация, `yield*`, цепочка методов), компилируется на месте: его тело
встраивается в цикл, а на месте каждого `yield` оказывается тело цикла. Ни объекта в куче, ни
вызовов `next()`: оптимизированный IR такой же, как у цикла, написанного руками, а цикл по
`evens(n)` LLVM векторизует. Так встраиваются функции верхнего уровня (и копии обобщённых) с
не больше чем восемью `yield`, пока тело цикла копируется не больше 16 раз (вложенные циклы по
генераторам умножают копии); рекурсивный генератор (`yield* walk(t.left)`) внутри самого себя —
уже объект.

**Как объект.** Иначе генератор — объект рантайма: заголовок, как у объекта, и фрейм корутины
LLVM, в котором живут его переменные и состояние; `next()` продолжает её до следующего `yield`.
Вызов генератора только связывает параметры, тело начинает работать с первым `next()`, как в JS.
Объект освобождается подсчётом ссылок; если генератор не закончен, освобождается и то, что держат
его переменные в месте, где он остановился. Вложенный генератор держит окружение, из которого
его создали, а метод-генератор — свой объект: `for (const x of cart.lines())` и `cart.lines()`,
сохранённый в переменной, видят `this`, даже если на объект больше ничто не ссылается.

`yield*` перебирает массив, строку или итератор; `yield* r` с `Result` — ошибка.

**Ленивые цепочки.** Цепочка методов итератора, которую потребляют там же, где она написана
(`toArray`, `reduce`, `some`, `for...of`...), — один цикл: каждое значение проходит все шаги до
следующего, колбэки встраиваются, `take` останавливает перебор без лишнего `next()`. На `bench`
`iterators` (10 млн значений через генератор, `filter`/`map`/`take`/`reduce` над бесконечным
генератором и над `values()` массива) — 11 мс против 91 мс у Go с итераторами `iter.Seq` и около
550 мс у node. Цепочка, сохранённая в переменной (`const big = xs.values().filter(f)`), — объект,
как в JS: каждый шаг — своя корутина, которая держит свой колбэк и предыдущий итератор.

**Свои коллекции** ([iterators.md](docs/iterators.md), раздел 2) перебираются через
`[Symbol.iterator]()`, как в JS:

```ts
class Range {
  constructor(readonly from: int, readonly to: int) {}
  *[Symbol.iterator]() { for (let i = this.from; i < this.to; i++) yield i }
}
class Countdown {
  constructor(private n: int) {}
  [Symbol.iterator]() {
    let n = this.n
    return { next: (): IteratorResult<int, undefined> =>
               n > 0 ? { done: false, value: n-- } : { done: true, value: undefined } }
  }
}
for (const i of new Range(0, 3)) console.log(i)
const all = [...new Countdown(3)], set = new Set(new Range(5, 7))
function sum(xs: Iterable<int>) { let s = 0; for (const x of xs) s += x; return s }
sum([1, 2]), sum(new Set([3])), sum(new Range(0, 4)), sum(new Countdown(4))
```

- `for...of`, spread, деструктуризация, `Array.from`, `yield*`, `new Set(x)` и `new Map(x)` вызывают
  `[Symbol.iterator]()` класса или литерала напрямую: по типу видно, чей это метод. Метод-генератор
  даёт итератор как есть. Итератор, написанный руками, — объект (класса, литерала) с `next()`,
  которая даёт `{ done, value }`: `IteratorResult<T, R>` — это `{ done: false; value: T } |
  { done: true; value: R }`, и после `if (r.done)` у `r.value` тип `T`. Такой итератор перебирает
  копия встроенного генератора `iterate`, которая вызывает `next()`, пока он не скажет `done`, а
  `for...of` встраивает её в цикл: цикл вызывает `next()` напрямую, без объекта-генератора (сами
  объекты `{ done, value }` пока создаются в куче). `x[Symbol.iterator]()` можно вызвать и
  самому.
- **`Iterable<T>`** — тип параметра: он делает функцию (или метод) обобщённой с параметром типа
  «что угодно, что перебирается со значениями `T`», и у каждого вида — массив, строка, `Map`,
  `Set`, генератор, свой класс — своя копия функции, которая перебирает его, как код, написанный
  для него, без вызовов через таблицу. Значения должны быть ровно `T` (`Set<string>` для
  `Iterable<int>` — ошибка), литерал `[1, 2]` для `Iterable<double>` — массив `double`.
  `Iterable<T>` у переменной, поля или результата — ошибка с подсказкой: перебираемое там — не
  один тип.
- **`Iterator<T>`** — то же, что `IterableIterator<T>`: любой итератор; итератор, написанный
  руками, и объект с `[Symbol.iterator]()` становятся им там, где его ждут.
- Из символов JS есть только `Symbol.iterator` и `Symbol.asyncIterator`, и только как имена
  методов.

- Значение `yield` — то, что передали в `next(x)`; его тип пишут в `Generator<T, R, N>`, как
  требует TS (без этого — ошибка, как TS7057). `for...of`, spread и методы итераторов передают
  `undefined`, поэтому для генератора, которому нужно значение (`N` без `| undefined`), это ошибки
  компиляции. Если `next()` без значения продолжит такой `yield`, это паника (код 2).
- `next()` даёт новый объект `{ value, done }`, `value` — `T | R | undefined`: значение `yield`, а в
  конце — то, что вернул генератор (`R`); `for...of` возвращаемое значение не использует.
- Генератор, который остановили до конца, закрывается, как `return()` в JS: `for...of`, из
  которого вышли (`break`, `return`), `take`, `find`, `some` и другие, что перестают
  брать значения, и `yield*` закрываемого генератора закрывают итератор, даже если он живёт и в
  переменной, а освобождение недоделанного генератора закрывает его тоже; дальше он закончен.
  Метода `return()` у итераторов пока нет, `throw()` нет совсем.
- Если генератор оставили (освободили, не доведя до конца), когда он стоит на `yield` внутри
  выражения, части которого уже вычислены (`f(s + t, yield x)`), то строки и массивы из этих
  частей не освобождаются; `TZ_CHECK_LEAKS` о них сообщает.

**Async-генераторы и `for await`** ([iterators.md](docs/iterators.md), шаг 4):

```ts
async function* pages(url: string) {
  for (let page = 1; page <= 3; page++) yield await load(url, page)   // load — async-функция
}
for await (const text of pages("a")) console.log(text.length)
for await (const line of createInterface({ input: process.stdin })) console.log(line)
for await (const v of [fetchA(), fetchB()]) console.log(v)           // массив промисов
```

- `async function*`, методы `async *items()`, функциональные выражения; в теле — и `await`, и
  `yield`. `yield p` ждёт `p` и отдаёт его значение, `return x` тоже ждёт `x`. Тип —
  `AsyncGenerator<T, R, N>` (и `AsyncIterableIterator<T>`, `AsyncIterator<T>`, `AsyncIterable<T>`),
  обычно выводится из `yield`.
- Это корутина LLVM, как async-функция, и объект: очередь шагов, которые у него попросили, и
  состояние из спецификации (`AsyncGeneratorEnqueue`, `Yield`, `DrainQueue` — в
  `runtime/core/async/async.c`), за ними реактор, с которым он ждёт, и фрейм. Шаг `for await` — `Promise`
  значения (или с пометкой «конец»), который цикл ждёт; ни объекта `{ value, done }`, ни вызова
  `next()` через таблицу. Обороты очереди — как у V8: `yield` ждёт значение (оборот), `for await`
  ждёт шаг (ещё оборот), поэтому вывод вперемешку с другими цепочками промисов совпадает с node.
- `for await` идёт по async-итератору (async-генератор, `setInterval` из `node:timers/promises`,
  `readline`), а по массиву, строке или обычному итератору — ждёт каждый элемент и ещё оборот, как
  итератор спецификации, который оборачивает синхронный. Выход из цикла раньше конца (`break`,
  `return`) закрывает итератор (его `return()`) и ждёт, пока закроется. `yield*` в async-генераторе отдаёт значения другого async-итератора (не ожидая их
  ещё раз) или элементы массива и итератора, каждый дождавшись.
- Класс или литерал с `async *[Symbol.asyncIterator]()` `for await` обходит через этот метод, а
  параметр `AsyncIterable<T>` берёт такой объект так же, как async-генератор (и выводит из него
  `T` в обобщённой функции). Async-генераторы — и методы обобщённых классов, и с `unknown`, и под
  декораторами.
- Пока нет: `next()`, `return()` и `throw()` у async-итератора в программе (итерируют его
  `for await`), значения в `next(x)`, `[Symbol.asyncIterator]()`, который не async-генератор.
  Async-генератор, который оставили недоделанным, просто освобождается.

## Async

Async ведёт себя как в node, а куски async-кода, которые трогают только свои данные, идут
параллельно на всех ядрах ([async.md](docs/async.md), шаги 1–4 плана): компилятор находит, что
может идти одновременно, и объясняет это (`--explain-parallel`), а рантайм так и выполняет, в
порядке node там, где куски трогают общие данные (см. «Параллельное выполнение» ниже). Ввод-вывод
ждёт пул потоков, а его результаты приходят в главный поток (см. «Встроенные модули»).

```ts
const sleep = (ms: int) => new Promise<void>((resolve) => setTimeout(resolve, ms))

async function load(name: string) {
  await sleep(10)
  return name.length
}

console.log(await Promise.all([load("Анна"), load("Борис")]))   // [ 4, 5 ], через 10 мс
```

**Как устроено.** Async-функция — корутина LLVM, как генератор-объект, и вызов создаёт один
объект: заголовок `Promise`, место для значения и фрейм. Вызов выполняет тело до первого `await`,
а `await` ставит продолжение в очередь микрозадач, когда `Promise` выполнится; пока функция ждёт,
её объект живёт. `await` значения, которое не `Promise`, тоже занимает оборот очереди, как в node,
но ничего не выделяет; `await Promise.resolve(x)` — то же, что `await x`. `then` — объект
реакции на каждый вызов, а колбэк компилируется в функцию под типы значения.

**Порядок — как в node.** Очередь микрозадач, очередь `process.nextTick`, таймеры (списки по
длительности и куча по сроку, как в `lib/internal/timers.js`), `setImmediate` и фазы цикла libuv
повторены точно, поэтому программа без ввода-вывода печатает то же, что node, на каждом запуске.
Тесты `// same-as-node` сверяют с node и число оборотов: у `then`, `await`, `return` промиса из
async-функции и из колбэка `then`, `Promise.all` / `race`,
`queueMicrotask` и `nextTick`. Файл с `import`, `export` или `await` на верхнем уровне — ES-модуль,
как для node: его код верхнего уровня выполняется внутри микрозадачи, и микрозадачи идут раньше
`nextTick`; без них — CommonJS, где первыми идут `nextTick`.

**Ошибки.** `Promise` не отклоняется ([errors.md](docs/errors.md), раздел 4): операция, которая
может не получиться, отдаёт `Promise<Result<T, E>>`, и `Result` разбирают после `await`.
`reject` в `new Promise`, `.catch`, `.finally`, второй аргумент `then`, `Promise.reject`,
`allSettled`, `any` — ошибки компиляции с подсказкой; `void` над `Promise<Result>` (и над
`Result`) — тоже: задача, которую не ждут, разбирает ошибки сама. `process.on("uncaughtException")`
и `"unhandledRejection"` — ошибка: ловить нечего. Если код верхнего уровня ждёт `Promise`,
который больше некому выполнить, программа выходит с кодом 13 и тем же предупреждением, что node.

**Забытый `Promise` — ошибка компиляции**, как с правилом `no-floating-promises` typescript-eslint:
вызов async-функции отдельной инструкцией, async-стрелка в `forEach`, массив промисов из `map`,
который никто не берёт. `await p` ждёт, `void p` отпускает явно (но не `Promise<Result>`).
Колбэки таймеров, `queueMicrotask` и `process.nextTick` могут
быть async: их `Promise` отпускается, как с `void`, а ошибка в нём завершит программу, как
необработанная.

**`node:timers/promises`.** `await setTimeout(ms, value)` и `await setImmediate(value)` —
таймеры тех же списков, что у `setTimeout`, которые выполняют `Promise<T>` значением;
`for await (const v of setInterval(ms, value))` — интервал, который начинается с первого шага и
останавливается `break`; `scheduler.wait(ms)` и `scheduler.yield()`. Опция одна — `ref`
(`ref: false` не держит программу). Отмены нет: `AbortController` и `AbortSignal` убраны (см.
«События»).

**Что пойдёт параллельно** (шаг 3). Компилятор делит async-код на куски: от `await` до `await`,
от начала функции до первого `await`, код верхнего уровня и каждый колбэк (`then`, таймеры,
`queueMicrotask`, обработчик сервера). Для каждого он находит, что кусок читает и пишет:
- глобальные и захваченные переменные, которые где-то меняются;
- поля — по имени, у объекта, который создаёт выражение, если его держит константа (`const a = new
  Counter()`), иначе у любого объекта этого типа (с учётом наследников и интерфейсов);
- элементы массива отдельно от его длины, записи словарей, байты, время `Date`, `lastIndex`
  регулярки;
- вывод (stdout и stderr — одни данные), ввод, файлы (`node:fs`; ожидание `fs/promises` от времени
  зависит и в счёт не идёт), код выхода и окружение.

Вызов добавляет то, что делает начало вызванной функции, а `this` в ней — объект, на котором её
вызвали; вызов значения-функции — любую функцию программы этого типа, вызов метода интерфейса —
методы классов, которые его реализуют. Объекты, которые функция создаёт и никому не отдаёт, и
элемент массива по своему индексу в колбэке `map` (`results[i] = ...`) — не общие данные.

Встроенные модули на tz (`node:events`, `tz:http`, `fetch`, потоки, `URL`, `EventTarget`)
анализ не обходит, а знает, что они делают. Вызов читает то, что ему дали, и
меняет состояние объекта, на котором вызван; чтение поля такого объекта читает это состояние.
Обработчик сервера идёт позже,
на каждый запрос, во многих копиях сразу. Слушателей `on`, `once` и `addListener` эмиттера
запускает `emit` прямо там, слушателей `addEventListener` — `dispatchEvent`. `start` источника
`ReadableStream` идёт сразу, `pull` и `cancel` — позже. Ответ `fetch`, результат async-метода и
то, что даёт функция модуля, — новые объекты. Объекты `node:crypto` меняются, когда ими пользуются
(`update`, `digest`), `randomFill` пишет байты. `Reflect.apply` запускает метод одного из классов
объекта, `Reflect.construct` и `fromJson(cls, j)` — конструктор любого класса программы,
`spawnSync` и его родня трогают файлы, вывод и окружение.

Задачи — код верхнего уровня, каждый вызов async-функции, которого не ждут сразу (`void f()`,
`Promise.all`, `const p = f()`), и каждый колбэк. Куски одной задачи идут друг за другом, разные
задачи — одновременно, как и копии одной (вызов в цикле или в колбэке `map`, `setInterval`).
Задача начинается с куска, который её запускает, и идёт с ним и с тем, что может идти после. Задачи
`await Promise.all([...])` кончаются до кода после этого `await`. Два куска, которые могут идти одновременно и трогают одно место, причём хотя
бы один пишет, идут по очереди в порядке node, остальные — параллельно. Async-генератор идёт в
задаче цикла, который его перебирает. Так рантайм их и выполняет (см. ниже).

`tz build --explain-parallel app.tz` ничего не собирает и печатает для кода верхнего уровня, каждой
async-функции и каждого колбэка их куски в порядке исходника:

```
async function convert, line 7
  start: runs inside its caller
  after await at line 8 (in the tasks convert(name) at line 14)
    writes elements of results (line 11)
    writes the length of results (line 11)
    in order with the same piece in other tasks: elements of results, the length of results
    in order: line 11
```

Под куском — что он читает (`reads`) и пишет (`writes`), со строками: `line 12 in save` — через
вызов `save` в строке 12, в других файлах — `lib.tz:5`. Затем решение: `parallel` (может идти
одновременно с другими кусками, общих изменяемых данных с ними нет), `runs alone` (одновременно с
ним ничего не идёт) или строки `in order with <кусок>: <данные>` и `in order: lines A-B` —
участок от первого до последнего обращения к общим данным, который один и идёт по очереди. Кусок,
который идёт в нескольких задачах (`a.run()` и `b.run()`), объясняется для каждой: `in the task
a.run(2) at line 17:`. Анализ — `compiler/parallel/parallel_analysis/parallel_analysis.cpp`; на любом из тестов он занимает меньше 4 мс,
на программе в 11 тыс. строк с тысячей async-функций — около 90 мс. Сборка программы с async
идёт через него: по его решениям компилятор готовит код к параллельному выполнению.

**Параллельное выполнение** (шаг 4). Программа, у которой есть куски, способные идти одновременно с
другими, собирается с рантаймом потоков (`tzrt_mt`); остальные — с рантаймом одного потока
(`tzrt`) и ничего за потоки не платят.
- **Очередь.** Порядок node держит главный поток: цикл событий, микрозадачи, таймеры. Кусок после
  `await`, который может идти одновременно с другими, рантайм отдаёт свободному потоку пула, как
  только он встаёт в очередь микрозадач за тем, что главный поток выполняет. Поток выполняет его
  заранее, пока кусок трогает только то, что никто другой одновременно не меняет, — и перед первым
  обращением к общим данным (местам из решений анализа, выводу, очередям и таймерам рантайма) ждёт
  своей очереди. Когда главный поток доходит до этого куска, он отдаёт ему очередь и ждёт: кусок
  доделывается в своём потоке, в порядке node. Так `console.log` после долгого расчёта печатает в
  порядке node, а сами расчёты идут на всех ядрах.
- **Ввод-вывод.** Кусок, который ждал `readFile` (и другие функции `node:fs/promises`) один, уходит
  в поток сразу, как файл прочитан, а событие этого чтения главный поток проводит, когда кусок
  дойдёт до общих данных: когда придёт событие ввода-вывода, решает система, так что node мог бы
  увидеть то же самое. Пример `convert` из [async.md](docs/async.md) на восьми файлах — 4,0 с в
  одном потоке, 0,53 с на восьми.
- **Что стоит потока.** Передать кусок другому потоку — микросекунды. Каждый `await` учится, сколько
  его куски идут до первого обращения к общим данным, и короче 30 мкс выполняет их в главном потоке
  (изредка проверяя снова), поэтому миллион коротких `await` не медленнее, чем в одном потоке.
- **Вытеснение.** Если все потоки пула дольше 10 мс заняты каждый своим куском, а ещё есть, что
  отдать, пул берёт запасной поток (до стольких же, сколько у него есть), и ядра делит система.
- **Память.** У каждого потока своя куча; блок, освобождённый чужим потоком, возвращается в кучу
  владельца через атомарный список. Счётчики ссылок обычные, пока объект виден одному потоку; объект
  становится общим (счётчик атомарный), когда его может достать другой поток: его кладут в
  глобальную переменную или в общий объект, передают async-функции, чьи куски идут заранее, или
  выполняют им `Promise`. Тогда общим становится и всё, что в нём лежит. Группу объектов в циклах,
  которую видит один поток, он меняет без замка; общие группы — под одним замком.
- `TZ_THREADS=n` задаёт число потоков (с главным; по умолчанию — число процессоров, `1` — всё в
  главном потоке).
- **Сервер.** События соединений сервера приходят в потоки пула, и поток, который прочитал
  запрос, сам выполняет обработчик и ответ; главный поток нужен, только если обработчик трогает
  общие данные. Async-обработчик с `await` пока идёт через главный поток: его аргументы становятся
  общими, а с ними и его `Promise`.
- Пока в главном потоке: внутренности `fetch` и потоков (анализ не обходит код встроенных модулей)
  и куски до первого `await` функции (они идут в куске вызывающего).

Бенчмарк `async_parallel` (64 задачи считают простые числа, каждая после `await`): 1,1 с в одном
потоке, 76 мс на 32 потоках — как Go с горутинами (77 мс). Тесты run, где есть async-код,
выполняются ещё и в режиме, где каждый кусок, который может идти заранее, идёт в потоке пула
(`run/NAME/threads`).

**Скорость** (`bench`): миллион `await` async-функции и миллион `await` значения — 14 мс против
239 мс у Go с каналами и около 100 мс у node; 100 тыс. задач по 10 `await` и `Promise.all` — 21 мс
и 29 МБ против 539 мс и 399 МБ у горутин Go (в рантайме одного потока — 14 мс: куски там короткие,
и рантайм потоков только платит за учёт); 200 тыс. таймеров — 26 мс против 136 мс.

## Сеть

```ts
import { Server } from "tz:http"

const server = new Server({ port: 3000 }, async (req, client) => {
  const url = new URL(req.url)
  if (req.method == "POST" && url.pathname() == "/users") {
    const user = await req.json()                          // Result<Json, BodyError | JsonError>
    if (user.isErr()) return new Response(user.error.message, { status: 400 })
    return Response.json({ id: 1, user: user.value, from: client.address }, { status: 201 })
  }
  return new Response(`привет, ${url.searchParams.get("name") ?? "мир"}`)
})
const listened = server.listen()                                // Result<Listening, ListenError>
if (listened.isErr()) {
  console.error(listened.error.code, listened.error.message)    // EADDRINUSE, если порт занят
  process.exit(1)
}
const listening = listened.value

const r = await fetch(`http://localhost:${listening.port}/?name=tz`, { timeout: 5000 })
if (r.isOk()) console.log(r.value.status, await r.value.text())   // 200 Ok { value: 'привет, tz' }
await listening.stop()
```

- **Сервер** ([net.md](docs/net.md), раздел 1) — `new Server(options, handler)` из `"tz:http"`:
  настройки и обработчик, после создания не меняются. `server.listen()` открывает сокет и
  отдаёт `Result<Listening, ListenError>`: `Listening` — запущенный сервер с `url`, `hostname`,
  `port` (настоящий, и при `port: 0`) и `stop()`; `ListenError` — `{ kind: "SERVER", code,
  message, errno, syscall, address, port }`, `code` — `"EADDRINUSE"`, `"EACCES"`,
  `"ENOTFOUND"`, `"ERR_TLS_CERT"` и другие из `ListenErrorCode`. Каждый `listen()` — ещё
  один сокет с теми же настройками. Настройки: `port` (8000, `0` — любой свободный),
  `hostname` (`"0.0.0.0"`), `maxBodySize` (128 МБ,
  больше — ответ 413), `keepAliveTimeout` (5000 мс простоя соединения, как в node),
  `headersTimeout` (60 000 мс на голову запроса, потом 408 — защита от клиента, который шлёт по
  байту), `requestTimeout` (300 000 мс на весь запрос с телом, 0 — без предела; потом 408, а
  обработчик, который читает тело, получает `BodyError` вида `TIMEOUT`), `transport` — `{ secure: false }` или `{ secure: true, cert, key }`. Обработчик —
  `(req: Request, client: Client) => Response | Promise<Response>`, `client` — `{ address,
  port }` того, кто прислал запрос: подходят и обычная, и async-функция, и `app.fetch` из hono
  (стрелка в поле, у которой второй параметр — `unknown`, а третий необязательный).
  `stop()` перестаёт принимать соединения, закрывает те, что ждут запроса, и ждёт ответов на
  те, что уже идут; второй вызов ждёт того же; другого способа остановить сервер нет.
  `stop({ timeout: 10_000 })` ждёт не дольше 10 секунд, потом обрывает оставшиеся соединения:
  у потока ответа (SSE) вызывается `cancel`, зависший обработчик остаётся сам по себе;
  `timeout: 0` обрывает сразу. Ошибки обработчик отдаёт ответом сам: у него нет
  исключений, которые сервер мог бы превратить в `500`. Полный адрес запроса (`req.url`) и
  адрес сервера строит рантайм из соединения сразу.
- **Запросы одного соединения** идут по очереди (keep-alive, до `keepAliveTimeout` простоя, и
  конвейер), а разных соединений — одновременно; пока в одном потоке цикла событий, на всех ядрах
  — с параллельным рантаймом ([async.md](docs/async.md), шаг 4). Ответ — заголовки и тело одной
  записью в сокет, с `content-length` и `date`; тело-поток — кусками `transfer-encoding: chunked`.
- **Разбор HTTP/1.1 строгий**, как нужно серверу в интернете (RFC 9112 и правила против request
  smuggling): строки только с CRLF, имя заголовка — токен прямо перед двоеточием, без перенесённых
  строк и управляющих символов в значениях, один `Content-Length` (или тот же повторённый) и
  никогда вместе с `Transfer-Encoding`, который кончается на `chunked`, `Host` в каждом запросе
  HTTP/1.1, и в нём (как и в цели вида `http://хост/путь`) — хост по правилам URL: ASCII, без
  `@`, `%` и пробелов, IPv4 и IPv6 правильные, порт до 65535. Поэтому `req.url` всегда
  разбирается, и `new URL(req.url)` не может не получиться. Голова — до 16 КБ (`maxHeaderSize` node). Нарушения — `400`, слишком большая голова —
  `431`, другое кодирование тела — `501`, версия не 1.x — `505`, и соединение закрывается.
  `Expect: 100-continue` получает `100 Continue`, когда обработчик начинает читать тело; тело,
  которое он не прочитал, пропускается перед следующим запросом.
- **`fetch`** ([net.md](docs/net.md), раздел 2) — как в node: `fetch(url | URL | Request, init)`,
  `method`, `headers`, `body` (строка, `Uint8Array`, `ArrayBuffer`, `URLSearchParams`, `FormData`,
  `ReadableStream` с `duplex: "half"`), `redirect` (`"follow"` до 20 переходов, `"manual"`,
  `"error"`), `timeout` — время на весь запрос в миллисекундах вместо `AbortSignal` (соединение,
  отправка, голова ответа, редиректы и чтение тела; не успел — `"TIMEOUT_ERR"`, таймер не держит
  программу). Полей браузера (`mode`, `credentials`, `cache`, `integrity`, `referrer`,
  `referrerPolicy`, `keepalive`) нет: вне браузера они ничего не делают, и такое поле — ошибка
  компиляции. Соединения с keep-alive ждут в пуле по 4 с, как в undici, а запрос по
  соединению, которое сервер успел закрыть, уходит снова по новому. Ответ сжатый gzip, deflate (и
  без обёртки zlib) и br распаковывается по мере чтения; `data:` URL — без сети. Имена хостов
  ищутся асинхронно (`GetAddrInfoExW`), `localhost` — без поиска, сначала IPv4.
- **Ошибки — `Result`**: `fetch` отдаёт `Promise<Result<Response, FetchError<E>>>` (`E` — тип
  ошибки потока тела, по умолчанию `never`). `FetchError` —
  объект с меткой `kind`, которая говорит, что делать: `"REQUEST"` — программа собрала плохой запрос,
  чинить код (`ERR_INVALID_URL`, `ERR_CREDENTIALS_IN_URL`, `ERR_INVALID_METHOD`,
  `ERR_BODY_WITH_GET`, `ERR_DUPLEX_REQUIRED`, `ERR_REQUEST_USED`, `ERR_UNSUPPORTED_SCHEME`,
  `ERR_BLOCKED_PORT`, `ERR_STREAM_FAILED`; поле `url`); `"NETWORK"` — не дошли или оборвалось,
  можно повторить (`ENOTFOUND`, `EAI_AGAIN`, `ECONNREFUSED`, `ETIMEDOUT`, `ECONNRESET`… `UNKNOWN`;
  поля `errno`, `syscall`, `host`, `port`); `"RESPONSE"` — сервер ответил не по HTTP или закрыл
  соединение до ответа (`ERR_CLOSED`, `ERR_INVALID_RESPONSE`, `ERR_HEAD_TOO_LARGE`); `"TLS"` —
  коды проверки сертификата OpenSSL, как в node (`CERT_HAS_EXPIRED`,
  `DEPTH_ZERO_SELF_SIGNED_CERT`…), `ERR_TLS_CERT_ALTNAME_INVALID` и `ERR_TLS_HANDSHAKE`;
  `"REDIRECT"` (`ERR_UNEXPECTED_REDIRECT`, `ERR_TOO_MANY_REDIRECTS`, `ERR_INVALID_LOCATION`,
  `ERR_REDIRECT_WITH_STREAM`; поле `location`); `"TIMEOUT"` (`TIMEOUT_ERR`, поле `timeout`).
  `message` — что, где и почему простыми словами: `could not connect to 127.0.0.1:3000: nothing
  listens on this port`, `the certificate of expired.badssl.com has expired`, `a GET request
  cannot have a body: use POST, or drop the body`. Поля `cause` нет.
- **`Request` и `Response`** — как в node: `text()`, `json()`, `bytes()`, `arrayBuffer()`,
  `formData()` дают `Promise<Result<T, …>>` со своим типом ошибки у каждого (29 октября, ночь):
  `TextError`, `BytesError` (= `BodyError`), `JsonBodyError` (+ «не JSON»: `contentType?`,
  `start` — первые 200 символов тела), `FormDataError` (+ «не форма»).
  `BodyError` тоже с `kind`: `"USED"` (`ERR_BODY_USED`, `ERR_BODY_LOCKED`), `"NETWORK"`
  (`ERR_CLOSED` — другая сторона закрыла соединение до конца тела, `ECONNRESET`, `ERR_TLS_FAILED`…),
  `"TIMEOUT"` (`timeout` у `fetch` или `requestTimeout` у сервера), `"TOO_LARGE"`
  (`ERR_BODY_TOO_LARGE`, поле `limit`), `"MALFORMED"` (`ERR_INVALID_CHUNKED` — куски не по
  chunked, `ERR_INVALID_COMPRESSION` — gzip или br испорчены), `"CONTENT"` (`ERR_INVALID_JSON`,
  `ERR_NOT_A_FORM`, `ERR_NO_BOUNDARY`, `ERR_INVALID_MULTIPART`; только у `json()` и
  `formData()`), `"STREAM"` (свой поток тела упал). Тело читается один раз, тело как поток — `body()`
  (`for await (const chunk of req.body())`), `bodyUsed()`, `clone()`, `Response.json`, `redirect`,
  `error`; `status`, `statusText`, `ok`, `headers`, `redirected`, `url`, `type` — поля.
  `body()` и `bodyUsed()` — методы (в node — свойства): тело меняет сам объект (после `clone()`
  у исходного — новый поток), а то, что объект меняет сам, читается методом
  ([objects.md](docs/objects.md), раздел 6).
- **`new Response` не может не получиться** — его аргументы проверяют типы
  ([errors.md](docs/errors.md), раздел 7). `status` — `HttpStatus` (200 … 599): литерал подходит
  сам (`{ status: 700 }` — ошибка компиляции), а значение программы — после проверки, которая это
  доказывает: `if (isHttpStatus(code))` или `if (code >= 200 && code <= 599)`; так же с полем и
  переменной типа `HttpStatus`. У статусов 204, 205 и 304 тела нет: литерал с телом — ошибка
  компиляции, а статус из переменной просто оставляет ответ без тела (node бросает `TypeError`).
  `statusText` с переводом строки или символом выше U+00FF — ошибка компиляции у литерала, а в
  строке из переменной такие символы выбрасываются. `Response.redirect(url, status)` берёт
  разобранный `URL` и `RedirectStatus` (`301 | 302 | 303 | 307 | 308`, по умолчанию 302).
- **`new Request` тоже не может не получиться.** Адрес — литерал (проверяется при сборке), `URL`,
  `url.href()` или `req.url`; строку из переменной сначала разбирает `URL.parse`, или её отдают
  прямо `fetch`, который вернёт `Result`. Литерал метода, `GET` или `HEAD` с телом, поток без
  `duplex: "half"` (тип поля — `"half"`), адрес с логином — ошибки компиляции. Если то же
  выясняется только при работе (метод из переменной, `URL` с логином, запрос из уже прочитанного
  запроса), запрос создаётся, а `fetch` с ним отдаёт `Err` с `code: "bad_request"` и текстом node.
  `fetch(request)` забирает тело запроса: второй `fetch` того же запроса — `Err`, как в node.
- **`Headers`** — без учёта регистра, значения без пробелов по краям, `get` склеивает значения
  через `, `, кроме `set-cookie` (`getSetCookie()`), перебор по алфавиту. Имя или значение
  литералом проверяется при сборке; заголовок с неверным именем из переменной не добавляется
  (`has` — `false`), а перевод строки и NUL из значения выбрасываются — ничего не бросается, и
  подставить свой заголовок через значение нельзя. Заголовки можно менять всегда, и у ответа
  `fetch`, и у `Response.redirect` (в node — `TypeError: immutable`). `clone()` прочитанного
  тела не бросает: у копии тело тоже прочитано. **`ReadableStream<R, E>`** (`E` — тип ошибки
  потока, по умолчанию `never`; `read()` и `for await` — `Result<…, E | ReaderError>`,
  `ReaderError` — `USED` `ERR_STREAM_LOCKED` / `ERR_READER_RELEASED`; `failure()`) —
  `start` / `pull` / `cancel` (обычные и async), `getReader()`, `read()`, `releaseLock()`,
  `cancel()`, `locked()`, `disturbed()` (читали ли поток — сверх node), `for await` (выход из цикла
  отменяет поток); `locked()` и `desiredSize()` у
  контроллера — методы (в node — свойства). Поток, который может упасть, отдают телом методом:
  `Response.stream(поток, init?)`, `Request.stream(url, поток, init?)`, `fetch(url, { body:
  поток })` — тогда `FetchError` вида `REQUEST` `ERR_STREAM_FAILED` несёт ошибку потока
  целиком в `details`; `new Response` / `new Request` берут только поток без ошибок
  (docs/net.md, раздел 3). **`FormData`** — поля-строки
  (файлов пока нет), разбор `application/x-www-form-urlencoded` и `multipart/form-data`.
- **`URL` и `URLSearchParams`** — по стандарту WHATWG, тем же автоматом состояний, что у ada в
  node: особые схемы, IPv4 во всех формах (`0x7f.1`), IPv6, `.` и `..` в путях, процентное
  кодирование, относительные адреса, `canParse`, `parse`, `url.searchParams`, который читает и
  меняет запрос адреса (`url.href()` после `url.searchParams.set(...)` — уже новый: параметры
  сообщают адресу о переменах подпиской, `params.watch(f)`, а `params.replace(text)` заменяет
  их разбором текста), `params.size()`. Части адреса
  читаются методами (`url.pathname()`, `url.href()`; в node — свойства), меняют их методы: `setPathname`, `setSearch`, `setHash`, `setUsername`,
  `setPassword`; `setHref`, `setProtocol`, `setHost`, `setHostname` и `setPort`, значение которых
  адрес может не принять, дают `Result<void, UrlError>` (node молча оставляет старое), а литерал
  проверяется при сборке, и вызов с ним ничего не отдаёт: `url.setPort("8080")`. `url.pathname =
  "/x"` — ошибка с подсказкой `setPathname(...)`. `url.portNumber()` — порт числом, `int |
  undefined` (`undefined`, если порта в адресе нет; порт схемы по умолчанию разбор убирает:
  `https://a:443/` → `undefined`): разбор его уже проверил, так что ошибки нет и в типе, а
  `parseInt(url.port())` не нужен. Имена хостов не в ASCII — через NFKC,
  нижний регистр и Punycode, без таблиц UTS #46 (несколько символов они отображают иначе).
- **`new URL` не может не получиться.** Адрес из строки, известной только при выполнении,
  разбирает `URL.parse(input, base?)` — он отдаёт `Result<URL, UrlError>`, где `UrlError =
  { kind: "URL"; code: "ERR_INVALID_URL"; message: string; input: string }`. `new URL(text)` с переменной — ошибка
  компиляции с подсказкой `URL.parse`. В `new URL` можно передать литерал (он проверяется при
  сборке: схема, хост особых схем, порт до 65535), готовый `URL`, `url.href()` и `req.url` запроса;
  относительный литерал — только с таким же основанием: `new URL("/path", req.url)`.
- **Геттеров и сеттеров нет и у встроенных модулей** ([objects.md](docs/objects.md), раздел 6):
  значение, которое не меняется, — `readonly`-поле, его задаёт только конструктор (`res.status`,
  `req.url`, `server.port`); которое объект меняет сам, — закрытое поле и метод с именем
  значения (`url.pathname()`, `stream.locked()`, `params.size()`, `res.body()`,
  `res.bodyUsed()`).
- Печать: `Headers { 'content-type': '...' }` и `URLSearchParams { 'a' => '1' }` — как в node, а
  `URL`, `Response`, `Request`, потоки и эмиттеры печатают все свои поля, и внутренние
  (`URL { state: { href: ... }, record: UrlRecord {...} }`): печать — для отладки, а отладке нужно всё
  состояние (в node часть их скрыта).
- **Отличия:** `headers.get(name)`, `params.get(name)` и `form.get(name)` без значения — `undefined`
  (в JS `null`); `null` как тело — `undefined`: `new Response(undefined, { status: 204 })`;
  источник `ReadableStream` пишется
  полями-функциями: `{ start: c => { ... } }`, а не методами литерала; у `Request` вместо
  `signal` — `timeout`.
- **https** ([net.md](docs/net.md), раздел 4) — OpenSSL 3.5 с настройками node 24: TLS 1.2 и
  1.3, те же шифры, SNI, ALPN `http/1.1`. `fetch("https://...")` проверяет сертификат сервера,
  как node: цепочку до корневых сертификатов Mozilla (встроены, как в node), с теми, что в файле
  `NODE_EXTRA_CA_CERTS`, и с системными при `NODE_USE_SYSTEM_CA=1`; затем имя хоста. Ошибка —
  `FetchError` вида `TLS` с кодом node: `CERT_HAS_EXPIRED`,
  `ERR_TLS_CERT_ALTNAME_INVALID`, `DEPTH_ZERO_SELF_SIGNED_CERT`, `UNABLE_TO_VERIFY_LEAF_SIGNATURE`
  и другие. `new Server({ transport: { secure: true, cert, key } }, handler)` — сервер https
  (PEM, как у `createServer` node, цепочка — сертификаты после первого). TLS встраивается в exe,
  только если программа может пойти по https: `fetch` адреса, про который не известно, что он
  `http://` или `data:`, или `new Server` с `transport`, про который не известно, что он
  `{ secure: false }`; https-клиент — 5,8 МБ против 0,9 МБ того же клиента по http (у Go — 5,3 МБ).
- **Прокси у `fetch`** ([net.md](docs/net.md), раздел 2; 29 октября): `HTTPS_PROXY`,
  `HTTP_PROXY`, `NO_PROXY` из окружения (как Go и curl; `localhost` — мимо всегда), в коде —
  `proxy: { enabled: true, url } | { enabled: false }`; https — туннелем `CONNECT`, логин —
  Basic из адреса прокси; ошибки — `FetchError` вида `PROXY` (`ERR_PROXY_AUTH_REQUIRED` и
  другие). SOCKS и Digest — пока нет.
- **`node:net`** ([net.md](docs/net.md), раздел 7; 29 октября) — TCP: `new Socket({ host, port,
  timeout, noDelay, transport })` — поток `Duplex` node, `await socket.connect()` даёт
  `Result<void, ConnectError>` (`kind`: `REQUEST`, `NETWORK`, `TLS`, `TIMEOUT`), `startTls()`,
  `reset()`; `new Server({ port, hostname, idleTimeout }, async socket => …).listen()` —
  `Result<Listening, ListenError>`, `stop({ timeout })`. `isIP`, `isIPv4`, `isIPv6`.
- **WebSocket** ([net.md](docs/net.md), раздел 9; 29 октября): клиент — `new WebSocket({ url,
  protocols, headers })`, события `"message"` / `"close"` / `"error"` EventEmitter, `await
  socket.connect()` — `Result`, `send()` — `Result`, `close(code, reason)`; сервер —
  `upgradeWebSocket(request)` из `tz:http`, как в Deno. Кадры и рукопожатие — на tz.
- **`tz:packet`** ([net.md](docs/net.md), раздел 8; 29 октября) — разбор и сборка заголовков
  Ethernet, ARP, IPv4, IPv6, UDP, TCP, ICMP и их контрольных сумм на tz; ошибки — `PacketError`
  (`kind: "PACKET"`).
- **`node:dgram`** ([net.md](docs/net.md), раздел 10; 29 октября) — UDP: `new Socket({ type,
  port, address, broadcast, multicast })`, `await socket.bind()` — `Result<void, BindError>`,
  приём — событие `"message"` с `(data, from)`, `await socket.send(data, { address, port })` —
  `Result<void, SendError>` (`kind`: `REQUEST`, `NETWORK`, `TOO_LARGE`, `PERMISSION`,
  `CLOSED`), `connect()` / `disconnect()`, мультикаст — `addMembership()` и другие с `Result`.
  Без `type` — один сокет на IPv4 и IPv6.
- **Пока нет:** то, что в разделе 11 (HTTP/2, слой `tz:socket`, сокеты Unix, SOCKS-прокси,
  HTTP/3); сеть в Linux (epoll и свой DNS). `node:http` не будет.
- **Скорость** (`bench/http.ps1`, «Hello, World!», 128 соединений, 10 с): tz — 314 тыс.
  запросов в секунду на всех ядрах, Go (`net/http`) — 239 тыс., node — 61 тыс.

## Generics

Обобщённая функция компилируется копией под каждый набор типов, с которым её вызывают: `first<int>`
и `first<string>` — разные функции в exe, и каждая — тот же код, что написанный руками под свой тип
(`int[]` лежит по 8 байт на элемент, LLVM встраивает и векторизует, см. `generics` в
[bench](bench)). Копия делается не на тип, а на представление в памяти: все объекты для такого
кода — указатель (освобождается через описание, на которое указывает сам объект), все функции —
код и окружение, поэтому `first<User>` и `first<Post>` — одна копия `first<object>`.

Тело проверяется один раз, как в tsc: `T` внутри — «какой-то тип», с которым можно то, что можно с
любым значением (присвоить, передать, вернуть, положить в массив, `==`, `typeof`, напечатать,
превратить в строку). Ошибка там показывается в теле. Наши правила, которых нет в TS, проверяются у
каждой копии, и ошибка показывается у вызова, которому копия нужна:

```ts
function show<T>(label: string, x: T) { console.log(label + ": " + x) }
show("n", 5)       // ок
show("p", rex)     // ошибка: cannot call 'show' with T = Dog: an object cannot be converted
                   // to a string... (и где именно — в теле show)
```

- **`T extends number`** — это `double` ([numbers.md](docs/numbers.md), раздел 1): `int` под
  него не подходит, функцию для разных чисел пишут перегрузками или по одной на тип. Целый
  литерал там подстраивается: `half(7)` при `half<T extends number>(x: T)` — `double`.
  `T extends string | int` — копия на каждый вариант.
- **`typeof x == "string"`** сужает `x` до `string`. В копии ответ известен при компиляции: ветка,
  которая не выполнится, выбрасывается непроверенной (`describe<int>` не содержит проверки).
- **Рекурсия** `f<T>` → `f<T>` — одна копия, хвостовые вызовы гарантированы. Если каждой копии
  нужна новая (`f(x: T)` вызывает `f([x])` или `f(ok(x))`), это ошибка компиляции.
- **Значение**: `lists.map(first)` и `const f: (xs: int[]) => int | undefined = first` берут копию
  для типов, которые там ждут; `const f2 = first<int>` — копия для `int`; `const f3 = first` —
  другое имя для той же обобщённой функции.
- **Шаблоны.** Функция, в теле которой есть class-выражение (примесь), проверяется только в
  копиях: каждая делает свой класс и сама находит свой тип результата (см. «Классы как значения
  и отражение»). Параметр `cls: Constructor<T>`, которому дали класс, известный при компиляции,
  получает копию для этого класса: в ней `cls` — сам класс.
- **Ограничения с полями и классами.** `T extends { name: string }` — объект, у которого есть
  хотя бы эти поля тех же типов (метод класса годится за поле с функцией): `greet(user)` и
  `greet({ name: "x", id: 1 })`. Это единственное место, где лишние поля разрешены, и копия
  делается на каждый тип: она знает его раскладку, и `x.name` — одна загрузка. `T extends Animal`
  — класс или его наследник; результат сохраняет точный тип: `loudest([new Dog("Rex")])` — `Dog`.
- **Своя копия на тип**, а не на представление, — и когда в сигнатуре есть объект с `T` (`Box<T>`,
  `Pair<T, int>`, `Repo<T>`: их объекты отличаются описанием), и для `Iterable<T>`, списков
  параметров и `K extends keyof T`.

**Обобщённые классы и интерфейсы** ([generics.md](docs/generics.md), раздел 5):

```ts
class Stack<T> {
  items: T[] = []
  static created = 0
  push(x: T) { this.items.push(x) }
  pop(): T | undefined { return this.items.pop() }
  map<U>(f: (x: T) => U): Stack<U> { const s = new Stack<U>(); for (const x of this.items) s.push(f(x)); return s }
}
const ints = new Stack<int>()             // или const ints: Stack<int> = new Stack()
class IntStack extends Stack<int> { sum() { let s = 0; for (const x of this.items) s += x; return s } }
interface Repo<T> { get(id: int): T | undefined }
class MemRepo<T> implements Repo<T> { items: T[] = []; get(id: int) { return this.items[id] } }
const box = new Box(1)                    // Box<int>: тип из аргументов конструктора
```

- Каждый список аргументов типа — свой класс: `Stack<int>` держит `int[]`, `Stack<string>` —
  `string[]`. Экземпляр делается из текста обобщённого класса: поля и сигнатуры методов — его, с
  подставленными типами, а тела проверяются с этими типами. Наши правила, которых нет в TS, —
  ошибка там, где экземпляр понадобился впервые: `cannot use 'Show' with T = Dog: an object
  cannot be converted to a string`. Методы `Stack<int>` компилируются в тот же код, что у класса,
  написанного руками для `int` (оптимизированный IR совпадает; `generic_classes` в [bench](bench)).
- Аргументы типа пишут (`new Stack<int>()`) или они выводятся, как у функций: из аргументов
  конструктора, из ожидаемого типа (`const s: Stack<string> = new Stack()`, поле с типом), из
  значений по умолчанию (`class Cell<T = int>`).
- **Статические члены** принадлежат самому обобщённому классу и общие для всех экземпляров
  (`Stack.created`); `T` в них нельзя, как в TS. Статический метод может быть обобщённым сам.
- **Наследование**: класс расширяет экземпляр (`class IntStack extends Stack<int>`), обобщённый
  класс — экземпляр из своих параметров (`class Named<A, B> extends Pair<A, B>`: `Named<int,
  string>` расширяет `Pair<int, string>`) или обычный класс, и переопределяет методы. Метод,
  который переопределяет наследник одного экземпляра, вызывается через таблицу методов во всех
  экземплярах.
- **`instanceof Stack`** верно для всех экземпляров, как в JS, а проверяет тот экземпляр, о котором
  говорит тип значения: у `Box<int> | Stack<int> | string` — по членам. Если тип не говорит
  (`Animal`, которым может быть `Pet<T>` любого `T`), это ошибка.
- **Обобщённые методы** (`map<U>(...)`, `static of<U>(x: U)`) — копия на каждый набор типов, как у
  функций; переопределять их и брать как значение нельзя. Они могут быть вариантами перегрузки.
- **Интерфейсы** с параметрами: `interface Repo<T>`, `extends`, `implements Repo<T>`; обобщённый
  код над `Repo<T>` получает копию на каждый тип.
- Печать — как в node: `Box { value: 1 }`, `Stack { items: [ 1, 2 ] }`.

**Списки параметров** ([generics.md](docs/generics.md), раздел 9): `A extends unknown[]` — типы
параметров какой-то функции, и обёртка подходит к любой:

```ts
function logged<A extends unknown[], R>(name: string, fn: (...args: A) => R) {
  return (...args: A): R => { console.log("->", name, ...args); return fn(...args) }
}
const loggedAdd = logged("add", add)    // A = [int, int], R = int: (a0: int, a1: int) => int
function partial<A extends unknown[], B extends unknown[], R>(fn: (...args: [...A, ...B]) => R, ...head: A) {
  return (...tail: B): R => fn(...head, ...tail)
}
const plus3 = partial(add3, 1, 2)        // A = [int, int], B = [int]
```

- В копии список — обычные параметры: обёртка для `A = [int, int]` — функция `(a0: int, a1: int)`,
  а `fn(...args)` — `fn(a0, a1)`. Кортеж из них создаётся, только если `args` берут как значение
  (`return args`); `args.length` — константа, `args[0]` — параметр. Необязательные параметры
  (`A = [string, string?]`) и rest (`A = int[]`) сохраняются. Обёртка над методом
  (`logged("inc", c.inc)`) работает: метод уже привязан к объекту.
- В обобщённом коде `args` можно передать дальше (`fn(...args)` функции того же списка,
  `console.log(...args)`), взять `args.length`, `args[0]` (тип `A[0]`) и перебрать в `for...of`;
  каждый элемент — «какой-то тип».
- Список выводится из функции (`fn: (...args: A) => R` — её параметры, с необязательными и rest) и
  из аргументов rest-параметра (`...head: A`). Части `[X, ...A]`, `[...A, X]`, `[...A, ...B]` берут
  свои элементы; если частей-списков две, сначала выводится то, что видно из других аргументов, а
  если поделить нельзя, это ошибка с подсказкой указать типы.

**Вычисления над типами** ([generics.md](docs/generics.md), раздел 10) существуют только при
компиляции, и в копии всё уже конкретное:

```ts
interface User { id: int; name: string; email?: string }
type Key = keyof User                   // "id" | "name" | "email"
type Name = User["name"]                // string
function get<T, K extends keyof T>(o: T, k: K): T[K] { return o[k] }
get(user, "id")                         // int: K = "id", в копии — чтение поля
type Preview = Pick<User, "id" | "name">
type Checks<T> = { [K in keyof T]?: (value: T[K]) => boolean }
const defaults = { port: 8080, host: "localhost" }
type Config = typeof defaults
```

- `keyof T` — имена полей (у класса — открытых), `T[K]` — тип поля, элемента кортежа или массива;
  `K extends keyof T` берёт литерал из аргумента (`get(user, "id")` — `K = "id"`), и копия читает
  и пишет поле прямо.
- Готовые утилиты: `Partial`, `Required`, `Readonly`, `Pick`, `Omit` (отображённые типы, как в
  TS), `Record`, `Exclude`, `Extract`, `NonNullable`, `ReturnType`, `Parameters`. Типы точные:
  `Pick<User, "id" | "name">` — новый тип, и `const p: Preview = user` — ошибка (лишнее поле).
- Свои отображённые типы `{ [K in keys]: T }` с `?`, `-?`, `readonly`, `-readonly`; у
  `[K in keyof T]` поля сохраняют `?` и `readonly` полей `T`. `as` даёт ключу новое имя (`` [K in
  keyof T as `get${Capitalize<K & string>}`] ``), `never` вместо имени убирает поле (`as Exclude<K,
  "id">`). Это часть типа, а не оператор `as`, которого нет.
- `typeof x` в типе — тип переменной (и её полей: `typeof config.server`), функции или метода:
  `ReturnType<typeof area>`; псевдоним с `typeof` вычисляется, когда его используют.

```ts
type Head<T> = T extends [infer H, ...infer R] ? H : never
type Reverse<T> = T extends [infer H, ...infer R] ? [...Reverse<R>, H] : []
type Count<T, Acc extends unknown[] = []> = T extends [infer H, ...infer R] ? Count<R, [...Acc, H]> : Acc["length"]
type Round = Extract<Shape, { kind: "circle" }>        // член объединения с этим полем
type Event = `on${Capitalize<"click" | "hover">}`      // "onClick" | "onHover"
type Split<S> = S extends `${infer H}-${infer R}` ? [H, ...Split<R>] : [S]
function box<T>(x: T): T extends string ? { text: T } : { value: T } { ... }
```

- **Условные типы** `T extends U ? A : B`, как в TS: если `T` — параметр типа, объединение
  проверяется по членам (`IsString<string | int>` — `"yes" | "no"`; `boolean` — это `false |
  true`), а `[T] extends [U]` — целиком. `extends` значит то же, что в ограничениях: объект
  подходит, если у него есть хотя бы эти поля таких типов, литерал — как свой базовый тип, массивы,
  кортежи и функции — по элементам, параметрам и результату. `Exclude` и `Extract` устроены так же
  (`Extract<Shape, { kind: "circle" }>` — член с таким полем). Ветка вычисляется, только если её
  выбрали, поэтому псевдоним может ссылаться на себя в ветке.
- **`infer`** в образце берёт тип с этого места: элемент массива, элементы кортежа (`[infer H,
  ...infer R]`, `[...infer Rest, infer L]`), параметры и результат функции, значение Promise,
  аргументы типа класса (`Box<infer U>`), часть строки (`` `${infer H}-${infer R}` ``, как делит
  TS); `infer N extends int` читает из строки число. `Awaited<T>` разворачивает Promise.
- **Кортежи в типах**: `[...A, B]`, `T["length"]` — число элементов литералом (`[1, 2, 3]["length"]`
  — `3`), `T[number]`, пустой кортеж `[]`. Так считают в типах, как в TS: `Add<3, 4>` — `7`.
- **Шаблонные строковые типы**: объединения внутри дают все строки (`` `${"a" | "b"}${1 | 2}` `` —
  четыре литерала), `Uppercase`, `Lowercase`, `Capitalize`, `Uncapitalize` меняют литералы. С
  `string` или `number` внутри это строка по образцу: `` const w: `${number}px` = "10px" `` —
  литерал проверяется при компиляции, `"wide"` — ошибка, в памяти это обычная строка. Где ждут
  такой тип, шаблон получает тип своих частей.
- **В обобщённом коде** условный тип ждёт копии: `box("a")` — `{ text: string }`, `box(1)` —
  `{ value: int }`. Тело может вернуть значение любой ветки; копия проверяет ту, что выбрали её
  типы, и не проверяет код после `if`, который её типы решили и который всегда возвращает.
- **`ReturnType` и `Parameters`** обобщённой функции дают её сигнатуру с ограничениями вместо
  параметров типа (или `unknown`), метода с несколькими вариантами — последнего, как в TS.
- **Глубина**: вложенных экземпляров одного псевдонима — до 50, а ветка, которая сама экземпляр
  псевдонима (`Count<R, [...Acc, H]>`, хвостовой вызов), повторяется до 1000 раз, как в TS; дальше —
  ошибка с указанием, где экземпляр понадобился.

## Редактор

Программы на tz — файлы `.tz`. Расширение [editors/vscode](editors/vscode/README.md) учит VS Code
показывать их ошибки, подсвечивать и форматировать: ошибки компилятора подчёркиваются по мере
набора (языковой сервер `tz lsp`, [docs/lsp.md](docs/lsp.md)); язык TypeZero с грамматикой
TypeScript, скобками, комментариями и отступами как у TS; Alt+Shift+F (Format Document)
форматирует файл Prettier'ом, как TypeScript: отступ 2 пробела, точки с запятой, двойные кавычки.
Установка:

```
powershell -ExecutionPolicy Bypass -File editors\vscode\install.ps1
```

и `Developer: Reload Window`. Наведение, переход к определению, все ссылки, дополнение после
точки и переименование отвечает тот же сервер — по последней проверке.
Объявления встроенных имён tz для TypeScript ([tz.d.ts](editors/types/tz.d.ts)), `@types/node` и
настройки tsc лежат рядом с `tz.exe`, в `types`, — для плагина tsserver, который проверял бы
файлы `.tz` ([docs/modules.md](docs/modules.md), раздел 6); самого плагина пока нет.

## Вопросы к компилятору и инструменты для ИИ

Компилятор отвечает на вопросы о программе — из того, что он сам вывел при проверке, а не из
поиска по тексту ([docs/lsp.md](docs/lsp.md), шаг 2):

```
tz map                          обзор: модули, что в них объявлено (с выведенными типами), импорты,
                                сколько раз что используется; --budget 2000 — примерно в токенах
tz context centroid             всё про символ: сигнатура, исходник, ошибки в нём, что он использует,
tz context main.tz:12           кто его использует (со строкой каждого места), разделы docs и тесты
tz query definition Vec         где объявлен
tz query references Vec.add     все места
tz query callers parseAge       кто вызывает
tz query type main.tz:12:7      тип имени в точке, и у локальных тоже
tz impact Vec.add               что затронет правка: кто использует, кто использует тех, по уровням
```

Программа — `"main"` ближайшего `tz.json`, файл из места (`main.tz:12`) или `--entry app.tz`;
`--json` — то же данными. `tz query records` печатает сами записи модулей. Для ИИ-агентов те же
ответы и проверка правки без записи на диск — сервер MCP `tz mcp` (инструменты `map`, `context`,
`query`, `impact`, `check_edit`, `language_card` — памятка [docs/ai.md](docs/ai.md)); в этом
репозитории он подключён в `.mcp.json` (через `editors/mcp/tz-mcp.cjs`: копия `tz` во временном
каталоге, чтобы сборка могла заменить `tz.exe`).

## Чем отличается от TypeScript

- Файлы программ — `.tz`, а не `.ts` (см. «Модули»); синтаксис тот же.
- Есть целые типы ([numbers.md](docs/numbers.md)): `int` — ровно 64 бита на любом процессоре,
  `int8`, `int16`, `int32`, `uint8`, `uint16`, `uint32`, `uint64` — свои пределы (`long` нет:
  это `int`). `7 / 2` для `int` равно `3`. Каждое действие обязано влезть в свой тип: что
  компилятор не может доказать, — ошибка (см. ниже). `**` целых тоже остаётся целым,
  `x ** -1` равно `0` (а отрицательная степень-константа — ошибка компиляции с подсказкой
  `2.0 ** -1`). Литерал берёт тип второго операнда: `b + 55` при `b: uint8` — `uint8`,
  `top > 1` при `top: uint64` — сравнение `uint64`.
- Переполнение в константах — ошибка компиляции, тоже как в Go. Константы — это литералы,
  `const` с такими значениями, члены `enum` и операторы над ними: на
  `24 * 60 * 60 * 1000 * 365 * 1000000000` компилятор скажет, что 31536000000000000000 не
  помещается в `int`, и предложит `double`; на `small * 2` при `const small: int16 = 30000` —
  что 60000 не помещается в `int16`; на `1 << 64` — что сдвиг не меньше ширины типа.
- **Переполнение — ошибка компиляции** ([numbers.md](docs/numbers.md), раздел 2): где целое
  может выйти за свой тип, а доказать обратное нельзя, это ошибка с причиной и подсказкой
  (`'seconds * 1000' may overflow int: 'seconds' comes from parseInt on line 24; check the
  values before, or compute it in a wider type or as a 'decimal'`). Это значения из ввода, JSON,
  `parseInt`, `Number(строка)` и всё, что из них посчитано (`sum += p` по массиву из
  `JSON.parse<int[]>`, поля таких объектов), сумма, которая за известное число проходов
  перерастает тип, хеш не в `Bit.Wrap32`. Счётчик с шагом 1 у `int` (`lines++`) проходит: до конца 64
  бит ему не дойти. Пределы компилятор знает по всей программе: литералы и константы, длины,
  проверки выше (`if (x >= 0 && x < 86400)`) и функции-условия, `% n`, маски, `Math.min` /
  `Math.max`, `charCodeAt`, число проходов цикла и все вызовы каждой функции; если опасен один
  вызов, ошибка называет его строку. Async-функции проверяются так же: `await` — место, где
  работает другой код, а значение из ввода остаётся им и после него. Проверка сужает
  переменную, но не поле (`if (o.port < 65535) o.port + 1` — ошибка; `const port = o.port` и
  проверка — нет). У `int` компилятор не подозревает данные, за которыми не следит (поля и
  элементы, которые программа меняет, результаты рекурсии, время существующей даты): ошибка о
  каждом `node.count + 1` была бы шумом. У узких типов (`int32`, `uint8`...) так не выходит: и
  такие значения должны влезть (`b - a` у двух `uint8` — ошибка, `b.toInt() - a.toInt()` —
  нет), `<<` тоже: это умножение на 2ⁿ. Маска — доказательство: `(x & 0xff).toUint8()`.
- **Делитель доказан ≠ 0** у целых (`/`, `%`, отрицательная степень): `if (b != 0)`,
  `xs.length > 0 ? sum / xs.length : 0`; иначе ошибка `cannot prove that the divisor 'b' is not
  0`. `MIN / -1` — переполнение. Где доказано, деление идёт без проверок.
- **Тип числа меняется только явно** ([numbers.md](docs/numbers.md), раздел 3), как в Go и
  Rust: у операндов один тип, у результата тот же (`int32 + int32` — `int32`), а `int32 + int`,
  `n * 1.5` при `n: int`, `const d: double = n`, `Math.sqrt(n)` — ошибки с подсказкой
  `a.toInt() + n`, `n.toDouble() * 1.5`. Приведения — методы `x.toInt()`, `x.toInt8()`,
  `x.toInt16()`, `x.toInt32()`, `x.toUint8()`, `x.toUint16()`, `x.toUint32()`, `x.toUint64()`,
  `x.toDouble()`, `x.toDecimal()`: расширение точное, сужение компилятор доказывает (не
  доказал — ошибка с подсказкой проверки, а доказанное стоит ноль команд), `toDouble()` и
  `Number(n)` от целого — что |n| ≤ 2^53, где `double` хранит целые точно. Дробное в
  целое — сначала округление, которое говорит, что делать с дробью: `Math.trunc(d).toInt()`,
  `Math.round(d).toUint8()`; `d.toInt()` без него — ошибка. `Math.floor` и другие округления
  от целого — ошибка: у него нет дроби. Литерал подстраивается под второй операнд и под место
  (`d * 2`, `const b: uint8 = 200`), и так же константа без типа со значением-литералом
  (`const K = 1000`, `d * K`), как нетипизированные константы Go. Разные целые сравниваются
  по математике: `-1 < x` при `x: uint64` — `true`. `double ** n` с целым `n` — без приведения.
- **Разбор строки даёт `Result`** ([numbers.md](docs/numbers.md), раздел 9): `parseInt(s)` —
  `Result<int, NumberError>`, `parseFloat(s)` и `Number(s)` — `Result<double, NumberError>`.
  `Err` (`{ code: "ERR_INVALID_NUMBER", message }`) там, где JS дал бы `NaN` или бесконечность, и у
  `parseInt` — число длиннее 64 бит. Разбор как в JS: `parseInt("12px")` — `Ok(12)`,
  `Number("12px")` — `Err`. Значение из разбора — ввод: `parseInt(s).unwrapOr(0) * 1000` без
  проверки — ошибка о переполнении.
- **Побитовые операции — по ширине типа** ([numbers.md](docs/numbers.md), раздел 6): у `int` —
  64 бита (`Date.now() >> 10` — верный ответ, в JS — мусор), у `int32` — 32, у `uint8` — 8;
  операнды одного типа, литерал подстраивается. `<<` — умножение на 2ⁿ: результат обязан
  влезть в тип. Число сдвига доказано в 0 … ширина − 1 (`1 << 64` — ошибка, в JS — `1`). `>>`
  у знаковых копирует знак, у беззнаковых заполняет нулями; `>>>` — только у беззнаковых.
  Дробное в побитовой операции — ошибка с подсказкой `Math.trunc`. Идиомы `x | 0` и `x >>> 0` —
  ошибки: здесь они ничего не заворачивают; подсказка — `.toInt32()` для сужения и `Bit.Wrap32`
  для счёта по модулю 2^32.
- **Нет `NaN` и `Infinity`** ([numbers.md](docs/numbers.md), раздел 10): `double` — всегда
  конечное число, и компилятор доказывает, что действие его таким и оставит, по тому же правилу,
  что переполнение целых. `x * y`, `x + y`, `Math.exp(x)`, `x ** y` должны влезть в
  ±1.8·10³⁰⁸; делитель `/` и `%` — не 0 и не так близок к 0, чтобы частное вышло за эти
  пределы (`if (Math.abs(b) >= 0.000001)`; `if (b != 0)` для дробного мало); `Math.sqrt` — от
  неотрицательного, `Math.log` — от положительного, `Math.asin` и `Math.acos` — от −1 до 1,
  отрицательное основание `**` — только с целым показателем. Иначе ошибка с причиной
  (`'x * 2' may overflow double: 'x' comes from parseFloat on line 6`) и проверкой в подсказке.
  Неизвестен диапазон только у пришедшего снаружи (`parseFloat`, `Number(строка)`, JSON, байты),
  остальное компилятор знает: константы, `Math.random()`, `sin`, проверки в `if` (и
  `Math.abs(x) >= c`), вызовы функций. `Math.clamp(x, lo, hi)` — для случаев, когда выход за
  пределы — погрешность: `Math.acos(Math.clamp(dot, -1, 1))`. Где доказано, операции получают
  флаги LLVM `nnan` и `ninf`.
  - `NaN`, `Infinity`, `isNaN`, `isFinite`, `Number.NaN`, `Number.isNaN`, `Number.isFinite`,
    `Number.POSITIVE_INFINITY` и `NEGATIVE_INFINITY` — неизвестные имена; `Number(x)` от `T |
    undefined` — ошибка (было бы `NaN`); `Math.min()` и `Math.max()` без аргументов — ошибка,
    от пустого массива — `undefined`.
  - Дробное из байтов (`getFloat64`, `getFloat32`, `getFloat16`, `readDoubleLE/BE`,
    `readFloatLE/BE`) — `Result<double, NumberError>`: `Err`, если в байтах `NaN` или
    бесконечность. Число JSON больше 1.8·10³⁰⁸ (`1e400`) — `Err` разбора: `Number out of range
    in JSON`. Запись в `Float32Array` и `Float16Array` (и `setFloat32`, `writeFloatLE`) числа
    больше их предела даёт наибольшее (`3.4028234663852886e38`, `65504`), а не бесконечность.
  - Значения, за которыми анализ не следит (как у целых), не подозреваются; деление на них
    проверяется строго. Элемент `Float64Array` над чужими байтами (`new Float64Array(buf.buffer)`)
    может оказаться `NaN` — это пока не проверяется.
- **`Bit.Wrap32` и `Bit.Wrap64`** — беззнаковые целые, которые считают по модулю 2^32 и 2^64
  (хеши, генераторы случайных чисел, контрольные суммы): `+`, `-`, `*`, `<<`, `>>`, `&`, `|`,
  `^`, `~` заворачиваются, второй операнд — любое целое, сдвиг на ширину и больше даёт 0.
  `let h: Bit.Wrap32 = 2166136261`, `h = (h ^ c) * 16777619`; `h.value` — `uint32` (`uint64`),
  `Bit.Wrap32.from(x)` берёт младшие биты любого целого. Функции `Bit`: `rotateLeft32`,
  `rotateRight32`, `leadingZeros32`, `trailingZeros32`, `onesCount32`, `reverseBytes32` и их
  64-битные пары, `mul64(a, b)` — `{ hi, lo }`, `add64(a, b, carry)` — `{ sum, carry }`,
  `sub64(a, b, borrow)` — `{ diff, borrow }`. `Math.imul` — `int32` от двух `int32`,
  `Math.clz32` — `int`.
- Число или `boolean` + строка — ошибка компиляции, и в `+=` тоже: в JS `10 + "1"` — `"101"`, и
  `count + input` со строкой из ввода склеивает вместо сложения. Текст собирают шаблоном,
  подсказка даёт готовый: `"Итого: " + total` → `` `Итого: ${total}` ``, `"" + n` →
  `String(n)`, `s += 5` → `s += String(5)`; для `count + input` — оба смысла:
  `count + Number(input)` или `` `${count}${input}` ``. Строка + строка — можно.
- `sort()` без функции сравнения у массива чисел — ошибка с подсказкой `sort((x, y) => x - y)`:
  JS сравнивает числа как строки (`[2, 10, 1]` → `[1, 10, 2]`), а сортировать иначе, чем node,
  хуже. У строк и `boolean` `sort()` — как в JS.
- `as` нет вообще. В TS он ничего не делает при выполнении, поэтому преобразование через него
  вело бы себя иначе, чем в tsc. Компилятор подсказывает, что написать вместо него. `satisfies`
  тоже нет: тип пишут у переменной, `const x: T = ...`.
- `1 in arr` и `delete arr[0]` — ошибки с подсказкой: дыр в массивах нет, поэтому вместо них
  `1 < arr.length` и `arr.splice(0, 1)` или `arr[0] = ...`. `for...in` по массиву или строке —
  ошибка: в JS индекс там строка, и `i + 1` даёт `"01"`; подсказка — `for...of`,
  `for (let i = 0; ...)` или `forEach`.
- `eval(...)`, `new Function(...)` и `new Proxy(...)` — ошибки: код из строки в скомпилированной
  программе выполнить нечем, а поля читаются и пишутся прямо в памяти, перехватить это нельзя.
- `escape` и `unescape` из приложения B — ошибки, как `RegExp.$1`: их `%uXXXX` не читает никто,
  кроме них самих. Подсказка — `encodeURIComponent` / `decodeURIComponent`, а для старого
  способа получить байты UTF-8, `unescape(encodeURIComponent(s))`, — `new TextEncoder().encode(s)`
  (и `new TextDecoder().decode(bytes)` для обратного `decodeURIComponent(escape(s))`).
- `void x` годится там же, где литерал `undefined`: в `T | undefined`, в `== undefined`, в
  `console.log`. Функция, которая возвращает только `void f()` (как `() => void f()`), ничего не
  возвращает — её тип возврата `void`.
- В шаблоне с тегом тексты приходят как обычный `string[]`, уже с применёнными
  escape-последовательностями; `strings.raw` — ошибка, сырой текст даёт только `String.raw`.
  Массив текстов создаётся при каждом вычислении шаблона (в JS он один на место в коде).
- Сравнение одно — `==` и `!=`; `===` и `!==` — ошибка компиляции с подсказкой. `==` ничего не
  приводит: типы с двух сторон должны совпадать (`x == "0"` у `int` — ошибка), поэтому второй
  формы не нужно, а результат тот же, что у `===` в node.
- Целочисленный литерал принимает тип из контекста: `let x: double = 1 / 2` даёт `0.5`.
- Число в условии — ошибка: в JS `0` — ложь, и `port || 3000` теряет ноль. Подсказка
  зависит от типа: `if (count)` → `count != 0`, `while (stack.length)` → `stack.length > 0`, у
  `double` — на выбор `!= 0` или `> 0`, у `int | undefined` — `!= undefined`
  или `??` (`timeout || 1000` → `timeout ?? 1000`).
- Массив, функция, объект и класс без `| undefined` — всегда правда, проверять их — ошибка: `if
  (items)` → `items.length > 0`, `if (check)` → `check()`, `items || []` — правая часть не нужна,
  `if (User)` — «a class is always truthy». Так же
  для `T | undefined`, уже проверенного выше (`if (!list) return`, потом `if (list)`).
- Присваивание в условии — ошибка, и в двойных скобках тоже: `if (level = 0)` → `level == 0`,
  `while (line = next())` → `while ((line = next()) != undefined)`.
- Если значения `&&` / `||` не помещаются в один тип (`flag && "да"` — `boolean` или строка),
  это ошибка: такое пишут через `?:`.
- `let` без значения — только с типом: `let msg: string`. Тип из присваиваний ниже не выводится
  (tsc выводит), но ошибка подсказывает его: `let total` → `let total: int`,
  `let empty = undefined` → `let empty: int | undefined`.
- Тип возврата без аннотации выводится из `return`, как в TS, но и у рекурсивных функций (tsc в
  строгом режиме требует для них аннотацию): `fib` с `return n < 2 ? n : fib(n - 1) + fib(n - 2)`
  возвращает `int`. Тип находится по ветвям без рекурсии и расширяется, пока не перестанет
  меняться; так же для функций, которые вызывают друг друга. Аннотация нужна, только если
  каждое возвращаемое значение берётся из самой рекурсии. Если верхний уровень вызывает функцию
  раньше, чем объявлена глобальная переменная, которую она использует, тип вывести нельзя — JS
  на таком вызове тоже упал бы.
- Параметры объявленных функций нужно аннотировать; тип может дать и значение по умолчанию,
  любое выражение, как у `let`: `n = 10` — `int`, `max = limit * 2` — тип `limit * 2`. Параметры
  стрелки берут типы из места, где её передают: в `a.map(x => x * 2)` `x` — элемент `a`.
  Стрелка без такого места и без аннотаций — ошибка.
- У обычных функций нет `this` (`this` в `function` — ошибка с подсказкой написать стрелку,
  которая видит `this` метода вокруг), `arguments` (есть rest-параметры), вызова через `new` и
  свойств (`f.cache = ...`). `call`, `apply` и `bind` не нужны: метод как значение уже привязан
  к своему объекту.
- Если аргумент типа вывести не из чего (`const xs = emptyList()`), это ошибка с подсказкой
  написать его (`emptyList<int>()`): tsc сделал бы `T` типом `unknown`.
  Аргумент-литерал получает тип, найденный по остальным: в `pair(1, 2.5)` `T` — `double`, и `1`
  тоже `double`.
- `T` без ограничения в условии — ошибка: в копии для чисел `0` стал бы ложью. Подсказка —
  `x ?? fallback` или `x != undefined`. С `T extends string` проверка разрешена.
- Обобщённые функции объявляются на верхнем уровне (`function f<T>` или `const f = <T>(...) =>`);
  во вложенных блоках и в выражениях их пока нет. Тип «любая обобщённая функция»
  (`f: <X>(x: X) => X`) — ошибка: копию нужно выбрать при компиляции, поэтому обобщённой делают
  внешнюю функцию. Пока нет ограничений с полями (`T extends { name: string }`), обобщённых
  интерфейсов и классов. У списка параметров `A extends unknown[]` в обобщённом коде есть только
  `length`, `join` и передача дальше (`...args`, `apply`); в копии это кортеж, и его элементы —
  обычные параметры. Обобщённая функция пока не может вернуть литерал с методами: у каждой её копии
  он был бы своим классом (поля-функции и классы — можно).
- Обычные функции не перегружаются: у каждого варианта своё имя (`circleArea`, `rectArea`).
  Сигнатуры перегрузок из TS (объявления без тела перед реализацией) — ошибка, два
  `function area` в одной области — ошибка «повторное объявление». Перегрузка есть у методов и
  конструкторов, с отдельным телом у каждого варианта (см. «Классы»).
- Типы функций совпадают точно: `(x: int) => int` не подходит туда, где ждут
  `(x: double) => double`, — нужна стрелка, которая преобразует аргумент. Функцию, известную по
  имени, можно передать туда, где её вызывают с бо́льшим или меньшим числом аргументов или где
  результат не нужен.
- Функция, переданная по имени во встроенный метод, не может получить индекс или массив в
  необязательный параметр: `["1", "2"].map(parseInt)` — ошибка компиляции (в JS индекс стал бы
  основанием системы счисления, и вышло бы `[1, NaN]`), так же `forEach(console.log)`,
  `map(Math.max)` и свои функции с необязательными параметрами. Нужна стрелка:
  `s => parseInt(s)`.
- Функцию нельзя превратить в строку (в JS получился бы её исходный код). `==` сравнивает
  функции, как JS: два вычисления одной стрелки дают разные функции.
- Стрелка не может использовать переменную, объявленную в блоке позже неё, даже если её
  вызывают после объявления; объявление нужно поднять или написать `function`, которая может.
  Если блок вызывает свою функцию раньше, чем объявлена переменная, которую она читает, — это
  ошибка компиляции (JS бросил бы ReferenceError).
- Функцию нельзя объявить прямо в `case`; её объявляют до `switch`.
- `enum` как тип — это `int` (или `string`): переменной `Color` можно присвоить любое `int`.
  Значения членов — константы: числа, строки и выражения из них и других членов; числа должны
  быть целыми и помещаться в `int`, а смешивать числа и строки в одном `enum` нельзя.
- `type int = number` (так `int` объясняют tsc) разрешено и ничего не меняет; другие
  определения встроенных типов — ошибка.
- Переменную, объявленную в одном `case`, нельзя использовать в следующих: JS выбросил бы
  исключение, если управление не прошло через объявление. Её нужно объявить до `switch`.
- `sin`, `exp`, `log` и другие трансцендентные функции `Math` считает библиотека C, а V8 — своя
  (порт fdlibm), поэтому в редких случаях последняя цифра может отличаться. `sqrt`, `abs`,
  `min`, `max`, округления и `toFixed` совпадают с V8 всегда.
- `var` нет.
- `.length`, результаты `indexOf` и `charCodeAt` — типа `int` (`charCodeAt` с недоказанной
  позицией — `int | undefined`, см. «Массивы»).
- `unknown` сравнивается через `==` как через `===` (`1 == "1"` ложно); `Array.isArray` у `unknown`,
  где лежит, например, `int[]`, даёт `unknown[]` — новый массив тех же элементов, который
  `unknown` с тех пор и хранит (исходный `int[]` его изменений не видит). Кортеж в `unknown` — тоже
  массив для `Array.isArray`, но копия при каждой проверке.
- Декораторы — как `experimentalDecorators`, но получают класс, а не прототип, а
  `design:*` пишутся всегда (без `emitDecoratorMetadata`) и только для декорированных членов.
  Обёртка метода получает все его параметры, и пропущенные тоже (`undefined`), поэтому
  `args.length` у неё постоянен. Метод, который возвращает `void`, даёт `R = void`, и обёртка с
  `const result = original.apply(...)` для него — ошибка (значения `void` не хранят).
  `getMetadataKeys` нет. Класс, который вернул декоратор класса, меняет и тип имени: в TS
  `new Order(1).id` подчёркнут, а в tz он есть.
- class-выражение делает один класс на копию функции, а не новый при каждом выполнении:
  `make() == make()` правдиво, статические поля получают значения один раз, переменные функции
  вокруг класс не видит. Полиморфного `this` нет: метод, который возвращает `this`, возвращает тип
  своего класса.
- Отражение проверяет значения по типам при выполнении: `new k("x")` у класса с `constructor(n:
  int)` — `TypeError`, а не объект с `n = "x"`; лишние и недостающие аргументы — тоже. В копии
  обобщённой функции для известного класса это ошибки компиляции.
- `null` бывает только в значениях `Json` (см. «JSON»): в остальном и `null`, и `T | null` —
  ошибки с подсказкой `undefined`. Убрать `undefined` можно проверкой или `??`.
- Объединение, члены которого нельзя различить при выполнении, — ошибка: `int | double`,
  `int | uint8` (`typeof` у обоих даёт `"number"`), два массива (`int[] | string[]` — подсказка
  `(int | string)[]`), две функции, два итератора; `Result` и `Json` в объединении — тоже ошибки.
  `==` у объединений — как `===`: `1` и `"1"` типа `string | int` не равны (в JS `==` их приводит).
- Литерал получает литеральный тип только там, где его ждут (`const d: Dir = "up"`,
  `s.kind == "circle"`, `case "up":`). `const d = "up"` и `const e = fast ? "up" : "down"`
  подходят туда, где ждут литералы (`move(d)`, `obj[e]`), как в TS; `let` и `const d: string` —
  строка (с 29 октября). Литерал объекта, который идёт в объединение, становится тем членом, чьи
  поля и метки он называет.
- `switch` по литеральному типу или по метке объединения без `default` должен разобрать все
  значения, иначе ошибка «does not handle "left"» (в TS — только с `never` в `default`). `sort()`
  без функции у массива объединения с числами или объектами — ошибка, как у чисел.
- Функция, которая возвращает `boolean`, сужает тип и без `x is T`; написанный `x is T`
  проверяется по телу: `return`, после которого `x` не доказан как `T`, — ошибка (TS тело не
  проверяет). Ветка `false` сужается только тем, что показало тело. `asserts x is T` — ошибка.
- `Json` — тип значений JSON вместо `any`, которое дают `JSON.parse` и `require` файла JSON в TS:
  читать ключи можно сразу, а условие, строка, арифметика и методы — только после проверки
  (`typeof`, `Array.isArray`). `==` у него — как `===` (`0 == false` ложно); `j.x = undefined`
  убирает ключ (в node остаётся `x: undefined`). `JSON.parse` отдаёт `Result` с `JsonError`, а ключ у `undefined` и `null` — `undefined` (в JS — `TypeError`).
- `[]` без типа — ошибка: нужен тип элементов (`let a: int[] = []`). `int[]` нельзя присвоить в
  `double[]` — у них разное представление в памяти; tsc считает оба `number[]`.
- В массивах нет дыр: `new Array<int>(3)` — это `[0, 0, 0]`, а не три пустых места; увеличение
  `length` дописывает нули, `false`, `""` или `undefined`. Такой `new Array(n)` есть только для
  этих типов элементов; у массива массивов, функций или объектов `length` можно только уменьшить,
  и это должно быть доказано.
- `x!` нет: TS его не проверяет, и `undefined` идёт дальше; в tz — проверка или `??`.
- Деструктуризация итератора даёт `T | undefined`, как деструктуризация массива: генератор мог
  кончиться раньше (`const [a, b] = naturals()` — две `int | undefined`).
- `next()` в конце печатается как `{ done: true }` (node: `{ value: undefined, done: true }`):
  `value` — необязательное поле (см. «Объекты»).
- `take(n)` и `drop(n)` берут целое число (JS берёт любое), доказанно не
  отрицательное (в JS отрицательное — `RangeError`). `flatMap` с колбэком, который возвращает строку, —
  ошибка компиляции (JS бросает `TypeError`), итератор нельзя превратить в строку (в JS —
  `"[object Generator]"`).
- Деструктуризация массива подчиняется тем же правилам, что `a[i]`: элемент, существование
  которого не доказано, имеет тип `T | undefined` (`const [y, m] = s.split("-")` — две
  `string | undefined`; после `if (parts.length >= 2)` — просто строки). Присваивание с
  деструктуризацией — только отдельной инструкцией (`({ a, b } = o)`), не внутри выражения.
- Spread в вызове — только в rest-параметр, `Math.min` / `Math.max` и `push`. `Math.max(...a)`
  даёт `T`, только если доказано, что массив не пуст, иначе `T | undefined` (от пустого JS
  вернёт `-Infinity`).
- `||=` и `&&=` проверяют цель по тем же правилам, что условие: `name ||= "аноним"` у строки
  можно, у числа — ошибка с подсказкой (`??=` или `if (n == 0) n = ...`). Цель — переменная
  или элемент массива с простым индексом.
- `process.stdout.isTTY` — `boolean`, как в типах node: `false`, если вывод не в терминал (сам node
  даёт `undefined`). `process.stdout.write(x)` берёт строку, `Buffer` или `Uint8Array` и
  возвращает `true`; с кодировкой, колбэком или как значение `process.stdout` — `Writable`
  (см. «Потоки»);
  `process.exit` и `process.exitCode` — целое `int` (или `undefined`).
- `process.env` — `Record<string, string>`: значение по имени — `string | undefined`, а `delete` и
  присваивание меняют переменные процесса (их увидят программы, запущенные потом). Присвоить можно
  только строку (node превращает в строку что угодно, и `process.env.X = undefined` даёт
  `"undefined"`).
- Встроенный модуль — это имена, а не объект: `import fs from "fs"` и `import * as` — ошибки,
  `posix` из `node:path` используется только как `posix.join(...)`, а функцию модуля можно только
  вызвать (`xs.map(basename)` — ошибка: в JS индекс стал бы вторым аргументом; пишут
  `x => basename(x)`).
- Опции функций `node:fs` пишут объектом-литералом в вызове: `encoding` и `withFileTypes`, от
  которых зависит тип результата, и `flag` — литералами. `mkdirSync` всегда даёт `string | undefined` (в
  типах node без `recursive` — `void`).
- Дату `Stats` (`st.mtime`) нельзя заменить присваиванием, и каждое чтение даёт новую дату (в node
  геттер запоминает первую); `userInfo().shell` в Windows —
  `undefined` (в node — `null`). Ошибки `rmSync` внутри удаляемого дерева — в общем виде,
  `EPERM: operation not permitted, rm '...'` (node даёт для них другие тексты).
- `node:child_process`: где node даёт `null`, у tz `undefined` (`status` программы, убитой
  сигналом, `signal`, `output[0]`), и `console.log` таких полей не показывает. Опции `encoding` и
  `stdio` — литералы: от них зависит тип результата. `stdout` потока-канала —
  `string` или `Buffer` без `| undefined` (в типах node — тоже, хотя у программы, которая не
  запустилась, node даёт там `undefined`). Ошибки — `Err` с `ExecError`, а не исключения: у
  «вышла не с 0» `code: "exit"` (в node `code` нет), в `ExecError` нет `name`, `output` и `pid`, а
  у `spawnSync` нет поля `error` — таймаут, переполнение и незапуск у него тоже `Err`. `status` — `int`:
  код больше 2^31 (падение программы в Windows, `0xC0000005`) отрицателен. Предупреждение node
  DEP0190 об аргументах с `shell: true` tz не печатает; в `stdio` — только три потока.
- `new Array(n)` и `a.length = n` — с доказательством, что `n` не меньше нуля (в JS —
  `RangeError: Invalid array length`).
- Исключений нет: `throw`, `try` / `catch` / `finally`, `Error` и его виды — ошибки компиляции,
  ошибка — значение `Result` (см. «Ошибки»), а `Promise` не отклоняется.
- Типы объектов точные (см. «Объекты»): объект с лишним полем не подходит туда, где ждут меньше,
  ни в литерале, ни при передаче (`{ x, y, z }` вместо `{ x, y }`; tsc второе пропускает), и
  ошибка называет поле. Одинаковый набор полей — один тип под любым именем; исключение —
  рекурсивный тип (`interface Node { next?: Node }`), он свой у каждого объявления.
- Литерал, тип которого не дают ни место, где он написан, ни места, куда он идёт дальше (см.
  «Объекты»), берёт типы значений: `{ x: 1 }` — это `{ x: int }`, а `{ a: undefined }` — ошибка.
  Места, куда идёт объект, компилятор ищет по виду кода в блоке объявления: аргументы функций,
  известных по имени, переменные и поля с типом, `push`, `return`. Если объект уходит туда через
  другой массив или объект (`[p]`, `{ inner: p }` без типа), тип нужно написать у переменной.
- Литерал из разных по типу значений без ожидаемого типа (`["Ann", 30]`) — массив объединения
  (`(string | int)[]`), как в TS; кортежем он становится только там, где ждут кортеж.
- «Поля нет» и «поле равно `undefined`» — одно и то же: `x: T | undefined` можно не писать в
  литерале, как `x?: T`, а `console.log` не показывает необязательное поле со значением
  `undefined` (node показывает `limit: undefined`). Так же его пропускают `Object.keys`,
  `for...in` и остаток деструктуризации, а spread не копирует `undefined` из необязательного поля:
  `{ ...{ limit: 20 }, ...query }.limit` — 20, если в `query.limit` — `undefined` (в node —
  `undefined`). Поле, которого не было в литерале и которое задали позже, печатается после полей
  литерала, в порядке объявления типа (в node — в порядке присваиваний).
- Порядок ключей объекта, созданного spread, берётся из типов частей: поля объекта-источника идут
  в порядке его типа, а не в порядке, в каком их задал литерал, создавший этот объект (node берёт
  второй). Разница видна, только если литерал перечислил поля не в порядке объявления типа.
- У объектов нет ничего от `Object.prototype`: `toString`, `valueOf`, `hasOwnProperty` — ошибки,
  как и `` `${obj}` ``, `"" + obj`, `String(obj)`, `join` у массива объектов и `sort()` без функции
  сравнения (в JS везде вышло бы `"[object Object]"`). У дат и ошибок текст свой (`toString()`).
- `delete o.x` и `"x" in o` — ошибки: у объекта всегда есть все поля его типа. Для
  необязательного поля подсказка — `o.limit = undefined` и `o.limit != undefined`. `o[key]` и
  `{ [key]: v }` — только с ключом, известным при компиляции (`o["content-type"]`,
  `const key = "name"`), произвольная строка — ошибка: нужен словарь `Record` (см. «Словари»).
- Ключ словаря читается как `T | undefined` (`m.get(k)`, `r[k]`, `r.k`; в TS без
  `noUncheckedIndexedAccess` у `r[k]` тип `T`), поэтому `r[k] += 1` и `r[k]++` — ошибки с подсказкой
  `r[k] = (r[k] ?? 0) + 1`. Ключи `Record` идут в порядке добавления, и ключи-числа тоже (node ставит
  их вперёд, по возрастанию). У словаря нет ничего от `Object.prototype`: `r["toString"]` —
  `undefined`, `"constructor" in r` — `false`, `__proto__` — обычный ключ. `Record<int, T>` —
  ошибка (JS сделал бы ключ строкой: `r[1]` и `r["1"]` — одно); для чисел есть `Map<int, T>`, а
  `r[i]` с числом — ошибка с подсказкой `String(i)`.
- Ловушки JS со словарями — ошибки: `m[k]`, `m[k] = v`, `delete m[k]` и `k in m` у `Map` (это
  свойства объекта, а не записи; подсказки — `get`, `set`, `delete`, `has`), `Object.keys(m)` и
  `for...in` по `Map` и `Set` (всегда пусто), `{ ...m }`, `` `${m}` ``. `new Map()` без типов —
  ошибка с подсказкой `new Map<string, int>()` (tsc делает `Map<any, any>`). Ключ `decimal`
  сравнивается по значению, как `==` (`0.30` и `0.3` — один ключ); `Result` ключом быть не может.
  `WeakMap` и `WeakSet` пока нет.
- `readonly` у поля проверяется: запись в него — ошибка. Объект можно передать туда, где поле
  `readonly`, и обратно — если через тот тип в поле никто не пишет (см. ниже о записи через более
  широкий тип; tsc такую запись пропускает).
- Функция в поле не может обратиться к своему объекту через переменную, которой он ещё не
  присвоен: `const o = { inc: () => o.n++ }` — ошибка; для этого есть методы:
  `{ n: 0, inc() { this.n++ } }`.
- Из `Object` есть только `keys`, `values` и `entries`.
- У кортежа фиксированная длина: `t.push(5)` — ошибка (в TS — нет, и из пары становится тройка),
  `t[j]` с переменной `j` — ошибка (только номер-константа или деструктуризация), кортеж нельзя
  присвоить в массив — подсказка `[...t]`. Кортеж нельзя превратить в строку (в JS — `"Ann,30"`).
- Криптография (см. «Криптография»): имя хеша или шифра, написанное в коде, которого нет или которое
  node 24 выключил (`"md4"`, `"des-cbc"`), — ошибка компиляции (в node — при работе), функции с
  колбэком — ошибки с подсказкой `…Sync`. `null` нет, поэтому ECB берёт пустой `Buffer` вместо IV, а
  `sign(undefined, data, key)` пишут с `undefined`. У ошибок нет `code` (`ERR_CRYPTO_…`): `e.message`
  тот же. `createHmac("shake128")` — ошибка (у node — непонятная ошибка OpenSSL при работе), а
  повторный `digest()` у `Hmac` даёт пустой результат, как в node. `KeyObject` и другие объекты
  печатаются коротко: `Hash {}`.
- `bigint` нет: `10n`, `BigInt(x)`, тип `bigint` и `typeof x == "bigint"` — ошибки с подсказкой:
  целые до 9.2·10¹⁸ — `int`, длиннее — `decimal`.
- `decimal` и `double` сами не смешиваются: `price * rate` с `rate: double` — ошибка с
  подсказкой `price * decimal(rate)`, `const x: double = price` — с подсказкой `Number(x)`.
  `decimal` в условии — ошибка, как число.
- Деление `decimal` (`x / n`, `x % n`, `x.div(n)`, `x ** -k`) — ошибка компиляции, если не
  доказано `n != 0`, `x.sqrt()` — если не доказано `x >= 0`, как у целых: `if (n != 0)`,
  `if (x >= 0)`, литерал или `abs()` доказывают (у BigNumber.js — `Infinity` и `NaN`). Счёт
  точный, поэтому `0.1 + 0.2 - 0.3` — ноль, и делитель из него — ошибка. Результат больше
  10^10000000 — паника и код 2 (см. «Ошибки»). `Number(x)` от `decimal` больше 1.8·10³⁰⁸ по модулю даёт `Number.MAX_VALUE` с
  его знаком: бесконечности у `double` нет.
- Чем `decimal` отличается от BigNumber.js: `==` сравнивает значения; `console.log`, `String(x)`
  и `x.toString()` печатают число без степени (`decimal(2) ** 100` — все 31 цифра), а не
  внутренности объекта; `-0` нет (но `toFixed(2)` отрицательного числа, которое округлилось до
  нуля, — `"-0.00"`, как у BigNumber.js); `typeof x` — `"object"`, как у объекта BigNumber.js.
  Общей настройки (`BigNumber.config`) нет: знаки и округление деления — аргументы `div`; `round`
  заменяет `decimalPlaces(n, mode)` и `integerValue`; `toFraction()` даёт `decimal[]` из двух чисел
  (`const [n, d] = x.toFraction()` — оба `decimal`); у `toFormat` нет третьего аргумента. Литерал
  в `decimal(...)` читается точно (BigNumber.js получил бы `double`), а выражение — как `double`:
  `decimal(0.1 + 0.2)` — `0.30000000000000004`.
- `sort()` без функции сравнения у `decimal[]` — ошибка, как у чисел (`sort((x, y) => x - y)`
  работает); в `console.log` массивы `decimal` выглядят, как массивы чисел.
- Неправильной даты нет, и `toJSON()` — `string` (в JS у неправильной даты — `null`).
  `x instanceof Date` вне объединения — ошибка: тип и так говорит, дата ли это.
- Даты за 275760 годом (и до 271821 года до н. э.) — обычные даты, а в JS — `Invalid Date`,
  и `toISOString()` бросает `RangeError`.
- В Windows пояса и языки берутся из ICU системы, а она бывает старше, чем в node 24 (в
  Windows 11 — ICU 72 с базой поясов 2022g и CLDR 42, у node — ICU 78, 2025c и CLDR 48).
  Поэтому мелочи форматирования языков могут отличаться: вид пробела, запятые, `GMT` вместо
  `GMT+0`, цифры у `ar`, `8/4/2000` вместо `08/04/2000` у `it`, — как и правила поясов, которые
  менялись после 2022 года. Пояс системы без `TZ` ICU 72 тоже может назвать по-старому: «Центральная
  Азия» Windows для страны без своего пояса — `Asia/Almaty`, а у node — `Asia/Bishkek`.
- То же у остального `Intl` (CLDR 42 против 48): `ar` в Windows пишет цифры `١٬٢٣٤`, а node —
  `1,234`; у `it` — `1.234,50 USD` и `1500` в компактной записи, у node — `1234,50 USD` и
  `1,5K`; есть и мелочи названий (у `uk` — «кілометрів на годину» и «за годину», у `es` — «yen» и
  «yen japonés»). В ICU Windows нет диалектных и коротких названий языков и регионов:
  `Intl.DisplayNames` пишет «English (United States)», а не «American English», и «United
  States» при `style: "short"`, а не «US»; `islamic-civil` там — «Islamic Calendar (tabular, civil
  epoch)», у node — «Hijri Calendar». `formatRange`, `selectRange`, опции `Intl.Locale`, его
  `minimize`, `calendar` и прочие поля расширений, `Intl.supportedValuesOf` пока не
  поддерживаются.
- Классы различаются по имени: `Size` нельзя передать туда, где ждут `Point` с теми же полями,
  а литерал — туда, где ждут класс (tsc пропускает оба). `instanceof` с классом, которым объект
  быть не может, — ошибка «всегда ложь».
- Метод, взятый как значение, привязан к объекту: `const f = o.m; f()` работает (в JS `this`
  теряется), `o.m == o.m` — `true`, но `a.m == b.m` у разных объектов — `false` (в JS — `true`,
  это одна функция прототипа). Метод нельзя заменить у объекта.
- Пока у объекта заданы не все поля, конструктор не может вызывать его методы и отдавать `this`
  (см. «Классы»); `x!: T` — ошибка. Начальное значение поля не может читать параметр-свойство:
  оно выполняется раньше (в node получился бы `undefined`, tsc тоже сообщает об ошибке).
- Класс объявляется только на верхнем уровне файла; `class` как выражение
  (`const C = class {}`), обобщённые классы и методы, `get` / `set`, `static { }`, `declare` и
  абстрактные поля — ошибки. Класс как значение (`const K = Point`, тип `typeof Point`) пока
  умеет только `K.name`, `==`, печать и передачу туда, где ждут `object` или `unknown`; `new K()` —
  ошибка. `accessor x = 1` — обычное поле. Использовать класс в коде верхнего уровня раньше его объявления —
  ошибка, как ReferenceError в JS. `this` в статическом методе или поле — ошибка с подсказкой
  имени класса.
- Подтипы только объявленные: класс подходит к интерфейсу, только если написано
  `implements` (tsc сравнивает поля), интерфейс к интерфейсу — только через `extends`.
- Запись через более широкий тип проверяется по всей программе ([objects.md](docs/objects.md),
  раздел 2): `Dog[]` можно передать туда, где ждут `Animal[]`, наследник и интерфейс, который
  расширяет другой (и класс, который его реализует), могут сузить поле до объектов типа ниже по
  цепочке (`pet: Dog` вместо `pet: Animal`) или сделать его `readonly`, а объект с `readonly`-полем
  можно передать туда, где поле можно менять, — если через более широкий тип никто не делает
  запись, которую не позволяет свой: не кладёт в массив другое `Animal`, не пишет в поле не `Dog`,
  не меняет `readonly`-поле. Компилятор следит, куда значение попадает: в переменные и параметры,
  в `this` методов (метод родителя, вызванный у наследника, видит его как родителя), в колбэки, в
  поля и элементы массивов, в результаты функций, в функции-значения, а брошенное — туда, где
  ошибки ловят (`catch`). Новый массив из элементов (`[...dogs]`, `slice`, `filter`) — уже не тот массив. Иначе ошибка у вызова или присваивания,
  со строкой записи: `adopt(dogs)` — «line 6 puts a 'Cat' into it, and an array of 'Dog' cannot
  hold one; pass a copy, '[...dogs]'». tsc такие записи пропускает. Анализ осторожный: запись
  через функцию-значение он связывает со всеми функциями, которые программа берёт как значения,
  поэтому может указать и на запись, которой с этим значением не бывает. Поле интерфейса, которое
  можно заменить (`onClick: () => void`), класс по-прежнему реализует полем, а не методом.
- `super(...)` — только отдельной инструкцией в теле конструктора и один раз (tsc разрешает
  его внутри `if`, если нет полей с начальными значениями). `super.x` для поля — ошибка (в JS он
  читает прототип и даёт `undefined`). Конструктор родителя не может вызывать метод, который
  переопределяет наследник (в node метод увидел бы незаданные поля).
- Метод наследника с тем же именем, но другими параметрами — не переопределение, а ещё один
  вариант (перегрузка, как в C#), а переопределение должно возвращать тот же тип. Метод наследника
  не может заменить стрелку в поле родителя: в JS поле объекта закрыло бы метод, и он бы не
  вызывался. Поле родителя, объявленное в наследнике заново без значения (`name: string`), —
  ошибка: в JS оно стало бы `undefined` после конструктора родителя.
- Spread объекта, которого знают по интерфейсу, копирует поля интерфейса, а метод класса
  становится функцией, привязанной к объекту (в node spread берёт только собственные поля
  объекта, без методов класса). `Object.values` и `Object.entries` смотрят и на поля наследников.
- Пока нет числовых ключей.
- `WeakRef` становится `undefined` ровно тогда, когда объект умер, а не после сборки мусора, как в
  node; `FinalizationRegistry` нет. Объекты, которые хоть раз ссылались друг на друга, остаются в
  одной группе, пока жив хоть один из них (см. «Циклы ссылок»).
- Обобщённые классы (см. «Generics»): каждый список аргументов типа — класс со своим номером, и
  `x instanceof Box` проверяет тот экземпляр, о котором говорит тип `x`; если тип не говорит
  (`Animal`, которым может быть `Pet<T>` любого `T`), это ошибка — в TS было бы `Pet<any>`.
  Обобщённый метод нельзя переопределить, сделать абстрактным и взять как значение; абстрактному
  методу обобщённого класса нужен тип результата. `keyof` у класса — только открытые поля, без
  методов.
- Вычисления над типами (см. «Generics»): пересечений типов нет, кроме двух приёмов TS: `K &
  string` (что из `K` — строка, как `Extract<K, string>`) и `T & {}` (`T` без `undefined`).
  Литерал, переданный в `T extends string`, делает `T` строкой, а не литеральным типом (`tag<"b">(
  "b")` — литеральным). В образце условного типа функция `(...args: unknown[]) => unknown` подходит
  к любой функции, как `(...args: any[]) => any` в TS (`any` нет). `Uppercase` и другие меняют
  буквы латиницы, греческого и кириллицы, остальные символы оставляют как есть.
- `Iterable<T>` бывает только у параметра (он делает функцию обобщённой); у переменной, поля и
  результата пишут сам тип (`int[]`) или `Iterator<T>`.
- В копии обёртки `(...args: A)` необязательный параметр, который вызов не передал, — `undefined`
  в списке: `console.log(...args)` его напечатает, и `args.length` его считает (в JS `args`
  короче).
- `Result` (см. «Result») нельзя сравнить через `==` или `switch`, найти через `includes` /
  `indexOf`, проверить как условие (`if (r)` — всегда правда, подсказка `r.isOk()`) и превратить в
  строку: в JS это объекты, и `ok(1) == ok(1)` — `false`. Забытый `Result` — ошибка (в JS для
  этого ставят правило линтера). `r.value` без проверки `isOk()` — ошибка, как в tsc.
- `yield* r` с `Result` — ошибка: `Result` не перебирают.
- Async (см. «Async»): `resolve` у `new Promise` берёт значение, а не другой `Promise` (его
  возвращают из async-функции или колбэка `then`). `Promise<T>` не превращается в `Promise<U>`,
  даже если `T` превращается в `U` (`Promise<int>` туда, где ждут `Promise<double>`, — ошибка);
  `==` между промисами — тоже ошибка. `Promise.race` промисов разных типов даёт их объединение. `await` объединения с `Promise` (`int | Promise<int>`, `Promise<T> | undefined`)
  ждёт промис, если он там, а иначе один ход очереди, как в JS; в объединении — один тип
  `Promise`, а `Promise.all` и другие значений, которые могут быть промисами, пока ошибка.
  Async-колбэк там, где метод берёт его результат (`replace`, `filter`, `find`, `some`, `reduce` и
  другие), — ошибка с подсказкой: в JS `replace` вставил бы `[object Promise]`, а `filter` оставил
  бы всё. `Promise.all` промисов `void` — `Promise<void>`. Аргументы колбэку после
  задержки (`setTimeout(f, 10, x)`) и у `process.nextTick` — ошибка с подсказкой стрелки; таймеры
  печатаются как `Timeout {}` и `Immediate {}`. Async-генераторов и `for await` пока нет. `await`
  на верхнем уровне в нескольких файлах выполняется в порядке загрузки файлов, по очереди (в node
  модули без зависимостей друг от друга ждут одновременно). Стек ошибки, пойманной выше по цепочке
  `await`, не содержит тех, кто ждал её выше места, где её поймали.
- События (см. «События»): слушатель-функция получает `this`, только если объявляет его
  (`function (this: Server, ...)`), и тогда всегда эмиттер, также при вызове из `rawListeners()`
  (в node там `undefined`); `rawListeners()` даёт для слушателя `once()` функцию, которая
  печатается `[Function (anonymous)]` (в node — `[Function: bound onceWrapper] { listener: ... }`);
  `currentTarget()` после отправки события — `undefined` (в node — `null`); имена событий — только
  строки, поэтому нет `errorMonitor` и `captureRejectionSymbol`; нет `captureRejections`,
  `addAbortListener` и
  `EventEmitterAsyncResource`; `Object.values` и `Object.entries` эмиттера — ошибки; третий аргумент
  `addEventListener` — только объект опций (не `true`); `AbortController` и `AbortSignal` нет.
- Регулярные выражения (см. «Регулярные выражения»): нет совпадения — `undefined`, а не `null`;
  `match` с `g` — `string[] | undefined`, без `g` — совпадение, где `m[1]` группы, которой может не
  быть, — `string | undefined` (в TS — `string`), а `m.groups` — объект с полем на каждое имя (в
  TS — словарь, который может быть `undefined`). `split` регуляркой с группой, которой может не
  быть, даёт `(string | undefined)[]`, как и есть в JS. `lastIndex` — `int`.
- Ошибки регулярок при выполнении: неверный шаблон — `Err` у `RegExp.parse` с текстом node
  (`Invalid regular expression: /(/: Unterminated group`); `matchAll` и `replaceAll` с
  регуляркой без `g`, известной только при выполнении, идут по всем совпадениям (в JS — `TypeError`).
- Свойства строк из `v` (`\p{RGI_Emoji}` и другие наборы эмодзи из нескольких символов) пока не
  поддерживаются. Где V8 отходит от спецификации, tz следует спецификации: с `/iv` `[\q{δ}]`
  совпадает и с `Δ`, `\p{ASCII}` — и с `ſ` (как `[\x00-\x7f]` в V8); одно имя у двух групп, одна
  из которых внутри другой, — ошибка.
- Байтовые массивы (см. «Байтовые массивы»): значение, которое не влезает в элемент, и запись за
  длиной — ошибки компиляции (JS молча обрезает `u8[0] = 300` до 44 и пропускает запись за
  длиной). `buf.slice()` у `Buffer` и `new Buffer(…)` — ошибки. `keys()`, `values()` и
  `entries()` типизированного массива идут по копии, сделанной при вызове (в JS обход видит
  записи, сделанные во время него). Длина — до 2^30 − 1 байт (в node больше); длиннее — паника, которая
  называет этот предел. `Math.max(...u8)`, `u instanceof Uint8Array`,
  конструктор как значение (`const T = Uint8Array`) и `toLocaleString` пока не поддерживаются.

## Сборка

Нужно (Windows x64):

- Visual Studio 2022 или Build Tools с компонентом C++ (MSVC, CMake, Ninja).
- LLVM 23.1.4 — архив `clang+llvm-23.1.4-x86_64-pc-windows-msvc.tar.zst` с
  [GitHub releases](https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.4)
  (не `.msi`: в инсталляторе нет библиотек для разработки). Архив сжат с длинным окном,
  встроенный `tar` его не открывает: `zstd -d --long=31 -c архив | tar -xf - --strip-components=1`.
- [vcpkg](https://github.com/microsoft/vcpkg): из него приходят zlib, zstd и libxml2, которые
  нужны LLVM, но не входят в архив.
- Интернет при первой настройке: OpenSSL для `node:crypto` собирается из исходников
  ([build.ps1](third_party/openssl/build.ps1)), и Perl, NASM и jom для этого скачиваются сами, по
  закреплённым хешам. Всё хранится в `%LOCALAPPDATA%\typezero\deps` (переменная CMake
  `TZ_DEPS_CACHE`), так что следующие деревья сборки только копируют готовое.

Пути задаются переменными окружения `LLVM_ROOT` и `VCPKG_ROOT` или в локальном
`CMakeUserPresets.json` (он в `.gitignore`):

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "dev",
      "inherits": "windows",
      "environment": { "VCPKG_ROOT": "D:/Programs/vcpkg", "LLVM_ROOT": "D:/Programs/LLVM" }
    }
  ],
  "buildPresets": [{ "name": "dev", "configurePreset": "dev" }],
  "testPresets": [{ "name": "dev", "inherits": "windows", "configurePreset": "dev" }]
}
```

В «x64 Native Tools Command Prompt for VS 2022»:

```
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Результат — `build/dev/bin/tz.exe`, рядом с ним каталог `lib` со встроенными модулями, написанными
на tz (`library/`), `types` — объявления и настройки для редактора (см. «Редактор»),
`tzcrypto.lib`, `libcrypto.lib` и `libcrypto_direct.lib` для программ с `node:crypto`,
`licenses` для `tz licenses` и рантайм в двух видах: `tzrt.lib` линкуется в каждую
программу, а `tzrt.bc` — тот же код в виде LLVM bitcode; и так же `tzrt_mt.lib` и `tzrt_mt.bc` —
рантайм потоков для программ, чей async-код идёт параллельно (`TZ_PARALLEL` в
[tzrt.h](runtime/core/tzrt/tzrt.h)). Рантайм собирает clang из того же LLVM
(CMake не умеет собирать C через clang, когда C++ собирает MSVC, поэтому это отдельные команды в
[runtime/CMakeLists.txt](runtime/CMakeLists.txt)). Готовый LLVM собран в Release со статическим
CRT, без RTTI и исключений, поэтому `tz` собирается так же; Debug-сборка не поддерживается.

## Как устроен компилятор

```
.tz → LoadPass → DeclarePass → InferPass → CheckPass → InstancesPass → WholeProgramPass
    → OverflowPass → ParallelPass → MemoryPass → LowerPass → LLVM IR
    → OptimizePass (+ рантайм) → EmitPass → .obj → LinkPass (LLD) → .exe
```

Весь путь — две функции в [pipeline.cpp](compiler/pipeline/pipeline/pipeline.cpp):
`checkProgram` (от загрузки до проверки переполнений; круги карт событий из `emit` повторяют её
начало) и `buildProgram` (от планов до exe). Проход — класс с `run`; ошибки он отдаёт значением,
`Errors`, а не печатает.

Быстрые пути рантайма, которые программы вызывают в циклах (выделение и освобождение памяти,
сравнение строк, `charCodeAt`, `startsWith`, `shift` и т.п., в C помечены `TZ_HOT`), перед
оптимизацией копируются из `tzrt.bc` в модуль программы, и LLVM встраивает их прямо в её код:
вызов исчезает, а проверки, которые не меняются в цикле, выносятся из него. Всё остальное
вызывается из `tzrt.lib`. Флаг `-O0` (его нет в `--help`, он для тестов и отладки компилятора)
выключает и оптимизации, и это встраивание.

Код лежит в трёх деревьях, по виду кода:

- `compiler/` — весь C++, из него собирается `tz.exe`;
- `runtime/` — весь C, из него `tzrt.lib`, `tzrt.bc` и `tzcrypto.lib`, которые входят в программы:
  `core/` — рантайм языка, `library/` — C-часть тем библиотеки;
- `library/` — код на tz, который лежит рядом с `tz.exe` в `lib` и компилируется вместе с
  программой.

Темы библиотеки называются одинаково во всех трёх деревьях: у `node:crypto` C++ лежит в
`compiler/library/crypto/`, C — в `runtime/library/crypto/`, код на tz — в `library/crypto/`.
Каждый класс лежит в своей папке с его именем (`compiler/load/lexer/lexer.cpp`), модуль на C — так
же (`runtime/core/string/string.c` и его `string.h`). Классы прохода, которым нужно общее состояние,
находят его и друг друга через класс прохода: `Checker` у проверки, `Lowering` у построения IR.
Темы подключены через `Feature` и `FeatureRegistry`: проверка и построение IR зовут крючки тем, а не
называют их. Подробнее — [docs/structure.md](docs/structure.md).

| Каталог | Что внутри |
|---|---|
| `compiler/pipeline` | конвейер проходов (`pipeline/`), опции сборки (`build_options/`), где лежат рантайм и библиотеки (`runtime_files/`) |
| `compiler/cli` | `main.cpp` и команды `tz`: разбор командной строки (`args/`, `command_line/`), `build`, `run`, `init`, `add`, `remove`, `update`, `port`, `licenses`, `lsp`, `mcp`, `version` (`*_command/`), `map` / `context` / `query` / `impact` (`knowledge_command/`), печать ошибок (`error_printer/`) |
| `compiler/knowledge` | записи модулей и ответы по ним ([docs/lsp.md](docs/lsp.md), шаг 2): данные — символы, ссылки, типы в точках, импорты (`knowledge/`), `KnowledgePass` — записи из проверенной программы (`knowledge_pass/`, обход AST — `knowledge_walk/`, сигнатуры — `symbol_text/`), разбор цели `имя` / `Класс.член` / `файл.tz:строка:столбец` (`knowledge_target/`), разделы docs и тесты по тексту (`project_text/`), ответы `map`, `context`, `query`, `impact` текстом и JSON (`knowledge_answers/`) |
| `compiler/mcp` | `tz mcp`: сервер MCP для ИИ-агентов (`mcp_server/`): инструменты зовут дочерний `tz` (`map`, `context`, `query`, `impact`; `check_edit` — `tz lsp --check` с текстом правки) |
| `compiler/lsp` | `tz lsp`, языковой сервер ([docs/lsp.md](docs/lsp.md)): кадры JSON-RPC через stdin / stdout (`lsp_channel/`), открытые файлы и их правки в столбцах UTF-16 (`text_document/`), `file://`-адреса (`file_uri/`), сервер: какие программы проверять, пауза в наборе, публикация ошибок (`lsp_server/`); проверка идёт в дочернем `tz lsp --check` (`check_process/`, `lsp_check/`), ошибки как данные — файл, строка, столбцы UTF-16, заметки (`check_report/`, `source_places/`) |
| `compiler/program` | что идёт по конвейеру: `Program` (`program/`), файл программы (`module/`, `source_file/`), по папке на узел AST (`ast/`; `ast.h` подключает их все), символы (`symbols/`), типы (`types/`: класс `Type` и модули функций над типами), диагностика и `Errors` |
| `compiler/result` | `Result`, `ok` и `err`, как в neverthrow |
| `compiler/load` | `LoadPass`: файлы программы по импортам и их порядок (`loader/`, `module_resolver/`), текст открытых в редакторе файлов вместо текста на диске (`source_overlay/`), встроенные модули на tz и глобальные `tz:web`, `tz:url`, `tz:fetch` (`library_modules/`); токены (`token/`), лексер (`lexer/`): литерал регулярки лексер читает, когда парсер встречает `/` там, где начинается выражение; текст литералов (`literal_text/`); парсер рекурсивным спуском (`parser/`) и его части: токены по одному (`token_cursor/`), операторы, `let`, деструктуризация (раскрывается в обычные объявления через скрытые переменные), выражения, вызовы, первичные выражения, шаблоны, функции, классы, `type` / `interface` / `enum`, `import` / `export`, типы (`*_parser/`); `throw` и `try` разборщик только называет ошибкой и пропускает |
| `compiler/declare` | `DeclarePass`: встроенные типы ошибок и строк (`prelude/`), функции и типы каждого файла (`module_declarer/`), импорты к экспортам (`import_binder/`), псевдонимы, интерфейсы, `enum` и целые константы (`type_declarer/`, `constant_folder/`), классы и цепочки наследования (`class_declarer/`), имена в функциях (`function_names/`); классы тем объявляют их крючки `declare` |
| `compiler/infer` | `InferPass`: циклы вызовов функций без типа возврата (`call_cycles/`) и тип возврата по `return` (`return_inferrer/`) |
| `compiler/check` | `CheckPass` и `Checker` (`checker/`): разрешение имён и проверка типов; неявные преобразования становятся узлами `CastExpr`. По папке на часть языка: выражения (`expr/`: вызовы, члены, присваивания, истинность…), операторы (`stmt/`), поток и доказательства индексов (`flow/`, `facts/`), типы (`types/`: разрешение, отношения, условные и шаблонные типы, тип литерала по месту назначения, объединения, `unknown`), значения (`values/`: методы чисел, строк, массивов), объекты и словари (`objects/`), функции и замыкания (`functions/`), классы, наследование, перегрузка, декораторы (`classes/`), generics (`generics/`), генераторы и итераторы (`iterators/`), async, `Promise` и таймеры (`async/`); круги карт событий (`event_rounds/`) |
| `compiler/instances` | `InstancesPass`: классы из class-выражений (`late_classes/`), копии generics и вызовы конструкторов (`constructor_calls/`) |
| `compiler/whole_program` | `WholeProgramPass`: порядок загрузки (`load_order/`), неиспользуемые функции (`unused/`), запись через более широкий тип (`wide_writes/`, `flows/` и `flow_*`) |
| `compiler/overflow` | `OverflowPass`: пределы значений целых по всей программе и ошибки о переполнении, делителе 0, сужении и диапазоне дат: пределы (`value_range/`), что в программе есть (`program_scan/`), места значений (`slots/`), операторы и выражения (`executor/`, `evaluator/`, `assign_evaluator/`, `builtin_evaluator/`), вызовы и циклы (`call_ranges/`, `loop_ranges/`), сужение условиями (`narrowing/`), сообщения (`overflow_warnings/`); доказанное помечает `Expr::rangeProven`, и код идёт без проверок |
| `compiler/parallel` | `ParallelPass`: куски async-кода, что каждый читает и пишет, и что из этого пойдёт параллельно (`pieces/`, `code_walk/`, `expr_walk/`, `call_walk/`, `alias_analysis/`, `escape_scan/`, `library_effects/`, `builtin_effects/`, `conflicts/`, `parallel_analysis/`); `--explain-parallel` (`explainer/`, `explain_names/`, `explain_titles/`) |
| `compiler/memory` | `MemoryPass` → `MemoryPlan`: какие типы могут образовать циклы, и их объекты становятся членами групп (`cycle_scanner/`, `cycle_finder/`, `held_members/`, `memory_analysis/`), владение без счёта (слой 2, `owner_scanner/`), объекты, которые никуда не записываются (слой 1, `store_scanner/`) |
| `compiler/lower` | `LowerPass` и `Lowering` (`lowering/`): LLVM IR через `IRBuilder`; порядок — объявления, глобальные, функции (`module_builder/`), функции рантайма (`runtime_functions/`), раскладки и описания типов (`layouts/`); по папке на часть языка, как в `check/`: выражения, операторы, значения, объекты и словари, функции, окружения и хвостовые вызовы, классы, таблицы методов и декораторы, объединения и `unknown`, места в программе для `Err` и памяти (`places/`), выходы из циклов и функций (`stmt/`), генераторы как корутины LLVM и на месте, ленивые цепочки (`iterators/`), async-функции, `Promise` и реакции (`async/`), счётчики ссылок, группы, общие объекты и проверки отладочного режима (`memory/`) |
| `compiler/optimize` | `OptimizePass`: машина под цель (`target/`), копирование быстрых путей из `tzrt.bc` (`runtime_linker/`), оптимизации |
| `compiler/emit` | `EmitPass`: объектный файл; `--emit-llvm` (`ir_writer/`) |
| `compiler/link` | `LinkPass`: встроенный LLD (`linker/`); какой OpenSSL нужен программе, список чужого кода в exe и `tz licenses` (`licenses/`, `licenses_pass/`) |
| `compiler/packages` | проекты и пакеты, по классу на папку: `tz.json` (`manifest/`), `tz.lock` (`lock/`), выбор версий и граф пакетов (`resolver/`, `package_graph/`), semver (`version/`, `range/`), кэш, JSR и git (`jsr_client/`, `git_client/`, `package_cache/`), HTTP через WinHTTP (`http_client/`), JSON с позициями для ошибок и правок (`json_parser/`), импорты файла без парсера (`scanner/`), `tz port` (`port/`) |
| `compiler/library` | C++-часть тем библиотеки: крючки темы (`feature/`), список тем (`feature_registry/`), и по папке на тему, в каждой `<тема>_feature/` и классы проверки и IR. `bytes`: байтовые массивы, `Buffer`, `TextEncoder` / `TextDecoder`, `embedBytes` и доказательства, что значение влезает в элемент; вызовы по группам (`*_calls/`). `child_process`: `spawnSync`, `execFileSync` и `execSync`. `crypto`: `node:crypto`, имена алгоритмов и выбор слоя OpenSSL, `crypto.subtle`. `date`: даты и `Intl.DateTimeFormat`; разборщик дат рантайма, собранный и в `tz` (`date_syntax/`). `decimal`: литералы, операторы и методы. `events`: карты событий, эмиттеры и `EventTarget`, `once()`. `builtin_modules`: имена модулей `node:*` и `tz:io`, привязка импортов, `node:timers/promises` и объекты, которые делает рантайм; что делает модуль, решает его тема через крючки `exportProblem`, `checkModuleCall`, `checkModuleValue` и `checkModuleMember`. `fs`: `node:fs`, `node:fs/promises`, `node:readline` и `tz:io`. `path`: `node:path`. `os`: `node:os`. `intl`: `Intl`, `toLocaleString`, `localeCompare` и `normalize`. `json`: `Json` и `JSON`, проверка `JSON.parse` текста, известного при сборке. `console`, `math`, `process`. `reflect`: отражение и метаданные. `regex`: литералы, методы, машинный код литералов (`regex_native_lowerer/`, `matcher_*/`), компилятор регулярок рантайма, собранный и в `tz` (`regex_syntax/`). `result`: `Result` и место создания `Err` (`err_sites/`). `stream`: типы кусков `Readable<T>` и остальных классов `node:stream`. `tz_modules`: встроенные модули на tz, их `declare function` и `tz:intrinsics`. `weakref`: `WeakRef` |
| `runtime/core` | рантайм языка, у каждого модуля свой заголовок: общее — `TZ_HOT`, дескрипторы типов, раскладки (`tzrt`); вывод в stdout и stderr, форматирование чисел, граница стека, сбой рантайма (`output`); аллокатор, куча на каждый поток (`alloc`); потоки программы, пул, очередь node и куски, которые идут заранее (`parallel`); общие объекты и атомарные счётчики (`publish`); исключение в полёте, места стека ошибки, текст `e.stack` (`error`); строки (`string`); массивы (`array`); разбор чисел и `toFixed` (`number`); окружения замыканий (`closure`); `Object.keys` / `values` / `entries` (`object`); регистр букв по таблицам Unicode, которые [gen_unicode_tables.mjs](runtime/core/unicode/gen_unicode_tables.mjs) берёт из самого Node.js (`unicode`); `Promise`, очередь микрозадач, таймеры и цикл событий, как в node (`async`); пул потоков и ожидание ввода-вывода — IOCP в Windows (`io`); хеш-таблица словарей (`map`); коробки `unknown` (`unknown`); группы объектов в циклах, `WeakRef` и отладочный режим памяти (`group`) |
| `runtime/library` | C-часть тем, по папке на модуль. `console`: `console.log` для массивов, объектов, `Result`, итераторов, `Json` и объединений как в Node (`inspect`). `math`: `Math`. `decimal`: точные десятичные числа. `json`: значения `Json` (`json`, `json_values`), `JSON.parse` как в V8 (`json_parse`), `JSON.stringify` любого значения (`json_stringify`). `regex`: разбор и компиляция в программу (`regex_compile`, общий с `tz`) с таблицами Unicode от [gen_regex_tables.mjs](runtime/library/regex/regex_compile/gen_regex_tables.mjs), интерпретатор, методы и то, что зовёт машинный код регулярок (`regex`). `date`: поля, установка полей и строки (`date`), разбор строк, как в V8 (`date_parse`, общий с `tz`), местный пояс (`date_zone`) и языки (`date_format`). `intl`: ICU системы, которую `icu` загружает при первом обращении; языки, опции и части `formatToParts` (`intl`), числа и `PluralRules` (`intl_number`), `Collator`, `normalize` и `Segmenter` (`intl_text`), `RelativeTimeFormat`, `ListFormat` и `DisplayNames` (`intl_names`). `process`: аргументы, переменные окружения, код выхода и часы. `path`: `node:path` в обоих стилях. `fs`: общее функций файлов (`fs`), ошибки ОС `SystemError` как в libuv (`fs_errors`), синхронные функции файлов и папок (`fs_sync`, `fs_dirs`), `node:fs/promises` (`fs_async`, `fs_async_dirs`), stdin и `readLine` (`stdin`), `node:readline` (`readline`), чтение и запись кусками для потоков файлов (`fs_stream`). `os`: `node:os`. `stream`: что просит `node:stream`: проверка куска, байты в stdout и stderr. `child_process`: подготовка вызова как в node, запуск как в libuv (кавычки аргументов, поиск в `PATH`, окружение, каналы с overlapped I/O, таймаут), в Linux — `posix_spawn` (`child`). `bytes`: `ArrayBuffer`, типизированные массивы и их методы (`bytes`); `Buffer`, кодировки node, `TextEncoder` / `TextDecoder` (`buffer`) с таблицами однобайтовых кодировок, которые [gen_encodings.mjs](runtime/library/bytes/encodings/gen_encodings.mjs) берёт у node (`encodings`), и многобайтовые кодировки (`cjk`). `reflect`: записи метаданных (`meta`); `Reflect.get`, `apply`, `construct`, `new` класса, известного при выполнении, и проверки `unknown` (`reflect`). `net`: сокеты TCP на порту завершения цикла (AcceptEx, ConnectEx, WSARecv, WSASend) и поиск имён (`net`), срок `timeout` у `fetch`, который обрывает ожидания соединения (`deadline`). `http`: HTTP/1.1: разбор голов и тел, ответы и запросы одной записью (`http`); распаковка тел `fetch` — inflate из zlib и декодер brotli из `third_party` (`decompress`). `tz_modules`: что просят встроенные модули на tz через `declare function` (`library`). Отдельно, в `tzcrypto.lib`: `crypto` — `node:crypto` поверх OpenSSL: общее без OpenSSL — объекты, HMAC, PBKDF2, HKDF, случайные числа, секретные ключи (`crypto`), хеши (`crypto_md`), AES (`crypto_aes`) и ChaCha20-Poly1305 (`crypto_chacha`), scrypt (`crypto_scrypt`) и Argon2 (`crypto_argon2`) прямого слоя, по объекту на алгоритм, и полный слой: имена из переменных и списки (`crypto_evp`), ключи, подписи и обмен ключами (`crypto_keys`), сертификаты (`crypto_x509`); Web Crypto (`crypto_subtle*`); `tls` — TLS соединений `net` для https: OpenSSL в памяти между сокетом и HTTP, проверка сервера как в node и корневые сертификаты Mozilla, которые [gen_tls_roots.mjs](runtime/library/tls/tls/gen_tls_roots.mjs) берёт у node |
| `library` | встроенные модули, написанные на tz, которые лежат рядом с `tz.exe` в `lib`. `events`: `node:events` (`events.tz`) и глобальные `EventTarget`, `Event`, `CustomEvent`, `DOMException` (`web.tz`). `http`: `tz:http` (`http.tz`), глобальные `URL` и `URLSearchParams` (`url.tz`), `fetch`, `Headers`, `Request`, `Response`, `ReadableStream`, `FormData` (`fetch.tz`). `stream`: `node:stream` (`stream.tz` — класс `Stream` со всей работой потока в закрытых методах, и его части `stream_duplex.tz` — `Readable`, `Writable`, `Duplex`, `Transform`, и `stream_pipeline.tz`, которые загрузчик читает как один модуль), `node:stream/promises` (`stream_promises.tz`), `node:stream/consumers` (`stream_consumers.tz`), `node:string_decoder` (`string_decoder.tz`), потоки файлов `createReadStream` / `createWriteStream` и `process.stdin` / `stdout` / `stderr` (`fs_stream.tz`). `crypto`: Web Crypto (`webcrypto.tz`). `core`: встроенные функции компилятора (`intrinsics.tz`, модуль `tz:intrinsics`: `panic`, `defer`, `asListener<F>`, `asClass<C>`, `chunkOf<T>` и другие), которые модули библиотеки импортируют по имени, а программа не видит. Глобальные получает сам файл, который их называет. Их код проверяется и компилируется вместе с программой; `declare function` в библиотеке — функция рантайма на C, имя которой выводится из имени: `netClose` — `tz_net_close` |
| `editors/types` | встроенные имена tz для TypeScript (`tz.d.ts`) и настройки tsc для программ tz (`tsconfig.json`) — для будущего плагина tsserver; лежат рядом с `tz.exe` в `types` |
| `editors/vscode` | расширение VS Code: ошибки по мере набора, подсветка и форматирование файлов `.tz` ([README](editors/vscode/README.md)) |
| `editors/mcp` | запуск `tz mcp` для `.mcp.json` этого репозитория из копии `tz` (`tz-mcp.cjs`) |
| `third_party` | рецепт сборки OpenSSL ([openssl](third_party/openssl)) и тексты лицензий, которые печатает `tz licenses` ([licenses](third_party/licenses)); inflate из zlib и декодер brotli как есть, для `fetch` (лицензии — рядом с ними); `@types/node` и undici-types для редактора ([types](third_party/types/README.md)); Prettier — форматтер расширения VS Code ([prettier](third_party/prettier/README.md)); `vscode-languageclient` — клиент `tz lsp` в нём ([vscode-languageclient](third_party/vscode-languageclient/README.md)) |
| `tests` | `run/*.tz` с ожидаемым выводом в `*.out`; `errors/*.tz` с `// expect-error:`; программы из нескольких файлов — каталоги с `main.tz`; `cli/*.cmake` — команды tz; `packages` — пакеты, адаптированные для tz |
| `bench` | одни и те же программы на typezero и Go и скрипт сравнения; сервер «Hello, World!» на tz, Go и node и нагрузка на него (`http.ps1`) |
| `docs` | принятые решения, которые ещё не реализованы или реализованы не до конца: объекты и классы ([objects.md](docs/objects.md)), generics ([generics.md](docs/generics.md)), ошибки и `Result` без исключений ([errors.md](docs/errors.md)), числа ([numbers.md](docs/numbers.md)), модули и JSON ([modules.md](docs/modules.md)), ввод-вывод ([io.md](docs/io.md)), async ([async.md](docs/async.md)), итераторы и генераторы ([iterators.md](docs/iterators.md)), декораторы ([decorators.md](docs/decorators.md)), мелочи языка ([small.md](docs/small.md)), точные десятичные `decimal` ([decimal.md](docs/decimal.md)), регулярные выражения ([regex.md](docs/regex.md)), даты ([date.md](docs/date.md)), пакеты ([packages.md](docs/packages.md)), байтовые массивы ([bytes.md](docs/bytes.md)), сеть ([net.md](docs/net.md)), криптография ([crypto.md](docs/crypto.md)), запуск процессов ([process.md](docs/process.md)), события ([events.md](docs/events.md)), потоки ([streams.md](docs/streams.md)), перегрузка методов ([overloads.md](docs/overloads.md)), проверки и сужение, JSON в свои типы ([narrowing.md](docs/narrowing.md)), языки `Intl` ([intl.md](docs/intl.md)) |

## Тесты

Каждый `tests/run/name.tz` компилируется дважды, как обычно и с `-O0`, запускается с проверкой
утечек, и stdout сравнивается с `name.out`. Код выхода и stderr задаются комментариями
`// expect-exit: N` и `// expect-stderr: текст`, а запуск — строками `// args: a "b c"`
(аргументы), `// env: ИМЯ=значение` (переменная окружения, по одной на строку) и
`// merge-stderr` (stderr идёт туда же, куда stdout, как при `2>&1`, и `name.out` проверяет
порядок строк обоих); stdin — файл `name.in` рядом с тестом, байт в байт (для `readLine` и
`readFileSync(0)`). Если рядом лежит `name.explain`, тест `explain/name` сверяет с ним то, что
печатает `tz build --explain-parallel` (для программы из нескольких файлов — `main.explain`). Каждый
`tests/errors/*.tz` обязан не скомпилироваться, а на строке под
комментарием `// expect-error: текст` должна быть ошибка с этим текстом (так проверяется, что
каждая ловушка в [tests/errors/bounds.tz](tests/errors/bounds.tz) поймана отдельно).

Программа из нескольких файлов — каталог: `tests/run/name/main.tz` с остальными файлами рядом и
ожидаемым выводом в `main.out`, или `tests/errors/name/main.tz`, где `// expect-error:` может
стоять в любом из файлов каталога.

Тесты с первой строкой `// same-as-node` ведут себя ровно как JavaScript: если установлен Node.js
23.6 или новее (он сам отбрасывает типы), они дополнительно запускаются в `node` с теми же
аргументами и переменными и должны напечатать тот же `name.out`, с тем же кодом выхода и stderr.
Так ожидаемый вывод сверяется с эталонной реализацией. После метки можно дать флаги для `node`:
тест с `enum` запускается с `--experimental-transform-types`, потому что `enum`, в отличие от
типов, нельзя просто отбросить. Перед тестом в node загружается заглушка
[tests/node/preload.cjs](tests/node/preload.cjs): она даёт то, что в tz встроено, а в node нет, —
глобальные `SystemError` (`instanceof` верно для `Error` со строковыми `code` и `syscall`),
`Result` с `ok` и `err`, функции, которые в tz дают `Result` (`JSON.parse`, `URL.parse`,
`Date.parse`, `RegExp.parse`, `decodeURIComponent`), `node:fs`, `node:fs/promises` и
`node:child_process` с `Result` ([tz-fs.mjs](tests/node/tz-fs.mjs),
[tz-fs-promises.mjs](tests/node/tz-fs-promises.mjs),
[tz-child-process.mjs](tests/node/tz-child-process.mjs)) и модули `"tz:io"` с `readLine` и
`"tz:http"`; `timeout` у `fetch` она передаёт node как `AbortSignal.timeout()`. То, что в tz
читается методом (`url.pathname()`, `res.body()`), у web-классов node — геттер, который коду теста отдаёт функцию, а коду node —
значение; состояние потоков node и `isTrusted` так не заменить, и заглушка убирает `()` после
них в тексте самого теста.
Дочерние процессы node получают её через `NODE_OPTIONS`.

Команды tz проверяют `tests/cli/NAME.cmake`: скрипт CMake с помощниками из
[tests/cli.cmake](tests/cli.cmake) запускает `tz init`, `add`, `run`, `port`... в своём каталоге
и сверяет код выхода, stdout, stderr и файлы. Сети тесты не трогают: кэш у каждого свой
(`TZ_CACHE`), JSR — зеркало на диске (`jsr_publish` раскладывает пакет из `tests/cli/jsr` так, как
отдаёт API JSR), git — голые репозитории, которые тест создаёт сам, а `cli/http` отдаёт зеркало по
HTTP сервером на node ([tests/cli/serve.mjs](tests/cli/serve.mjs)). Без git или node такие тесты
пропускаются.
