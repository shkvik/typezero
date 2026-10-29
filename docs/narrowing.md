# Проверки, сужение и JSON в свои типы

Решения, принятые 24 октября 2026 года, и поправки 26 октября: `asserts` убран, перевод JSON
отдаёт `Result` ([errors.md](errors.md)); 29 октября: у JSON нет особых прав в языке —
`JSON.parse<T>`, `fromJson<T>` и `fromJson(cls, j)` убираются (раздел 3). Шаги 1–3 плана сделаны
24 октября, шаг 4 — 28 октября, шаг 5 не сделан; план — в конце, в разделе «План реализации».

## Коротко

- Любая функция, которая возвращает `boolean`, сужает в `if` так же, как условие, написанное
  прямо в нём: компилятор сам смотрит её тело. Переносится не только тип, но и факты — поля
  объекта, пределы чисел, длины.
- `x is T` писать не нужно. Если написан — компилятор проверяет тело: проверка, которая
  врёт, — ошибка. В TS тело такой проверки не проверяется вовсе.
- У JSON нет особых прав: `JSON.parse(text)` отдаёт `Json`, форму проверяет обычная
  функция программы, а свой объект строится на виду, литералом.

## 1. Функция-условие сужает

```ts
function hasEmail(u: Person) { return u.email != undefined && u.email.length > 0 }
if (hasEmail(p)) send(p.email)              // p.email — string, без «!»

function validIndex(a: int[], k: int) { return k >= 0 && k < a.length }
if (validIndex(arr, i)) arr[i] += 1         // индекс доказан: int, запись можно

function isSmall(x: int) { return x >= 0 && x < 1000 }
if (isSmall(n)) total = n * 1000            // переполнение доказано: n < 1000

if (!hasEmail(p)) return err("no_email")
send(p.email)                               // после выхода по false — тоже string
```

- **Что переносится:** тип (объединения, `undefined`), поля объекта (`p.email`), пределы чисел
  — для индексов ([README](../README.md)), переполнения и деления ([numbers.md](numbers.md),
  раздел 2), записи в байтовые массивы ([bytes.md](bytes.md)) и проверки аргументов вроде
  `isHttpStatus` ([errors.md](errors.md), раздел 7), — длины массивов и строк. Для ветки
  `true` и для ветки `false`.
- Через модули и пакеты — тоже: программа компилируется целиком.
- **Факт о поле живёт, пока объект не мог измениться.** Присваивание полю, вызов функции,
  которая может писать в объект, параллельная задача, которая пишет в него
  ([async.md](async.md)), — и факт забыт. В TS тут дыра: он помнит сужение после любого
  вызова.
- **Сложная функция** (циклы, вызовы с побочными действиями) — компилятор докажет меньше, и
  сужения просто не будет. Неверного сужения не бывает.
- Как в TS 5.5: `[1, undefined, 3].filter(x => x != undefined)` — `int[]`.

## 2. `x is T`

```ts
function isCircle(s: Shape): s is Circle { return s.kind == "circle" }     // ок
function isDefined<T>(x: T | undefined): x is T { return x != undefined }  // ок

function isUser(x: unknown): x is User {
  return typeof x == "object" && x != null && "name" in x
}
// ошибка: x не доказан как User — проверки полей не дают тип объекта;
// для Json — JSON.parse<User> / fromJson<User> (раздел 3), для класса — instanceof
```

- В каждом `return`, который может дать `true`, `x` уже должен быть доказанно `T` — теми же
  проверками, что сужают тип в `if`: метка объединения, `typeof`, `instanceof`, `!= undefined`,
  `Array.isArray`, другая функция-условие. `this is T` у методов — так же.
- **`asserts x is T` нет** (убран 26 октября): такая функция сужает тип тем, что при `false` не
  возвращается, а бросает. Исключений нет, бросать нечем — ошибка с подсказкой написать
  `x is T` и `if (!isT(x)) return err(...)`.
- **Проверка полей не доказывает тип объекта.** Тип `User` в tz — не «любой объект, у
  которого есть `name`», а объект именно этого типа с раскладкой полей в памяти
  ([objects.md](objects.md)). А значение из JSON — вообще не объект `User`, а `Json`: его
  нужно не проверить, а перевести (раздел 3).
- Написанный `x is T` — документация: компилятор и без него вывел бы то же самое.

## 3. JSON в свои типы — проверкой, без особых прав

Решено 29 октября. До этого был `JSON.parse<User>(text)`: компилятор по типу сам писал перевод.
Убран, как и `fromJson<T>` и `fromJson(cls, j)`.

```ts
type User = {
  name: string;
  age: double;
};

function isUserJson(data: Json): boolean {
  return typeof data == "object" && typeof data.name == "string" && typeof data.age == "number";
}

const parsedResult = JSON.parse(text);              // Result<Json, JsonError>
if (parsedResult.isErr()) {
  ...
}

const data = parsedResult.value;
if (!isUserJson(data)) {
  ...
}

const user: User = {
  name: data.name,                                  // string — факт из isUserJson
  age: data.age,                                    // double
};
```

- **Граница для компилятора:** он выводит и проверяет — типы, факты, копии обобщений, — но не
  дописывает за программиста код, которого тот не мог бы написать сам. `JSON.parse<User>`
  дописывал: обходил поля `User` и собирал объект, и только для JSON.
- **JSON — один формат из многих** (YAML, TOML, CSV, MessagePack…), и особых прав в языке у него
  нет. Дать такое право всем — это шаблоны C++ с особым поведением под отдельные типы;
  обобщения в tz работают одинаково для любого `T` ([generics.md](generics.md)).
- **`JSON.parse(text)` отдаёт `Json`** — дерево, как у парсера любого формата.
- **Единственная особенность — `JSON.parse` встроен, и компилятор умеет выполнить его при
  сборке**, если текст известен заранее: литерал или `embedText(путь)`. Ошибка синтаксиса тогда —
  ошибка сборки с местом в самом файле (`config.json:3:5: unexpected "}"`), как у литералов
  `/…/`. `embedJson` поэтому не нужен и убирается: `JSON.parse(embedText("./config.json"))`.
  Результат и тут `Result`, и `if` на ошибку пишется всегда, хоть ветка и мёртвая — правило
  одно для любого текста.
- **Форму проверяет программа** обычной функцией. Компилятор переносит её факты о полях `Json` в
  `if` по общему правилу (раздел 1): `Json` — объединение, и `typeof data.name == "string"`
  сужает его так же, как `typeof x == "string"` сужает `string | int`. `data is User` не нужно
  и нельзя: `Json` — дерево, а `User` — структура с полями на своих местах (раздел 2).
- **Свой объект строится на виду** — литералом из проверенных полей. Число JSON — `double`;
  целое поле получают явным переводом после проверки ([numbers.md](numbers.md)).
- **Большие схемы** — библиотекой на обычном tz (как zod): схема — значение, проверка — обычный
  код, без особых прав.
- Обратно — `JSON.stringify(user)`, как в JS.

## План реализации

На каждом шаге: тесты `tests/run`, ошибки в `tests/errors`, обновление README.

1. **Сужение по функциям-условиям для типов** (сделано): вывод из тела, перенос в обе ветки `if`,
   проверка написанных `x is T` / `this is T`, `filter` с выводом. Нужны
   объединения ([objects.md](objects.md), шаг 6).
2. **Факты из функций-условий** (сделано): поля объектов, пределы чисел, длины — расширение того, что
   уже доказывает проверка индексов; правила, когда факт о поле забывается.
3. **`JSON.parse<T>` и `fromJson<T>`** (сделано; отменено 29 октября — шаг 5) — после `Json` ([modules.md](modules.md), шаг 3),
   исключений ([errors.md](errors.md), шаг 2) и generics ([generics.md](generics.md),
   шаг 1); объединения и словари — с шагами 6 и 7 [objects.md](objects.md).
4. **Поправки 26 октября** ([errors.md](errors.md), шаг 2; сделано 28 октября): `Result` у
   `JSON.parse<T>` и `fromJson<T>` вместо `TypeError`, ошибка на `asserts` (и в типе функции,
   и в списке `tz port`), проверки типа целых по [numbers.md](numbers.md).
5. **JSON без особых прав** (решено и сделано 29 октября). Убрать `JSON.parse<T>`,
   `fromJson<T>`, `fromJson(cls, j)` ([decorators.md](decorators.md), раздел 6) и `embedJson` —
   без подсказок, как всё убранное; `JSON.parse` с текстом, известным при сборке (литерал,
   `embedText`), разбирается при сборке, ошибка синтаксиса — ошибка сборки (сейчас
   `JSON.parse('{"a":')` собирается молча); факты из функций-условий о полях `Json` (сейчас после
   `isUserJson(data)` поля остаются `Json | undefined`); `JsonError` без `BAD_FIELD` и
   `BAD_CLASS`; JWK в `library/crypto/webcrypto.tz` — проверкой руками; тесты (28 файлов);
   modules.md, errors.md, io.md, decorators.md, numbers.md, net.md, README.

   Сделано: поля `Json` — пути `data.name`, `data.owner.name` — сужаются `typeof`,
   `Array.isArray` и `!= undefined`, как переменные (`JsonChecker::addJsonFacts`,
   `FieldFacts::fieldPath`); факты о полях выходят из функции-проверки в `if` и в ветку `false`
   (`PredicateChecker`: `narrowedFields` в сводке); `JSON.parse<T>`, `fromJson`, `embedJson`
   убраны (`fromJson` / `embedJson` — просто неизвестные имена); `JSON.parse` литерала или
   `embedText` проверяется при сборке; `webcrypto` разбирает JWK своей функцией. Остался код
   перевода по типу внутри компилятора (`FromJsonLowerer`, `json_typed.c`, `from_json` в описании
   класса) — до него больше нет пути, удалить отдельным шагом.