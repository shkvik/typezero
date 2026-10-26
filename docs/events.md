# События: `EventEmitter`

Решения, принятые 24 октября 2026 года, и поправки 26 октября: событие `error` без исключений
(раздел 4, [errors.md](errors.md), раздел 5). Шаги 1–4 плана сделаны 24 октября; что осталось —
в конце, в разделе «План реализации».

## Коротко

- `node:events` — как в node: `EventEmitter`, `once()`, `on()`. `EventTarget`, `Event`,
  `CustomEvent` — как в браузере.
- Типы событий компилятор выводит сам — из всех `emit` в программе. Подписка на событие,
  которое никто не шлёт, — ошибка компиляции (обычно это опечатка). Явная карта событий —
  можно; обязательна, если имя события берётся из переменной.
- async-слушатель разрешён: он сам считается «запустить и не ждать». Слушатель, который
  возвращает `Result`, — ошибка: его никто не прочитает.
- Событие `error` без исключений: подписался на события объекта, который может испустить
  `error`, — подпишись и на `error`; свой объект шлёт `error`, а слушателя нет нигде — ошибка
  компиляции. `once()` отдаёт `Result`.
- Код — перенос `lib/events.js` из node: порядок вызова, служебные события и тексты
  предупреждений совпадают.

## 1. Как пишут

```ts
import { EventEmitter, once } from "node:events"

const bus = new EventEmitter()
bus.on("user", user => console.log(user.name))   // user: User — выведено из emit ниже
bus.once("user", user => console.log("первый"))
bus.emit("user", ann)                            // слушатели вызываются сразу, по порядку
bus.off("user", handler)
const r = await once(server, "listening")        // дождаться события: Result<[port: int], …>
if (r.isErr()) return r                          // раньше пришло error
const [port] = r.value
```

`EventEmitter` — основа процессов ([process.md](process.md)), потоков node и `node:http`.

## 2. Типы событий

**Выводятся из `emit`.** Для каждого места, где создаётся `new EventEmitter()` (и для каждого
класса-наследника), компилятор собирает все `emit` на объектах оттуда — по всей программе, через
переменные и параметры, как тип метаданных в декораторах ([decorators.md](decorators.md)).
`bus.emit("user", ann)` — значит, у события `user` аргументы `[User]`.

```ts
bus.on("usr", () => {})          // ошибка: событие usr никто не шлёт (опечатка? есть user)
bus.emit("count", 1)
bus.emit("count", "два")         // ошибка: count шлют с number и со string — оба места в сообщении
```

- **Явная карта** — как в `@types/node`: имя события → список аргументов. Для своих классов
  это ещё и документация:
  ```ts
  class Server extends EventEmitter<{ listening: [port: int]; close: [] }> { … }
  const users = new EventEmitter<{ user: [user: User] }>()
  ```
  С картой `emit` и `on` проверяются по ней: лишнее событие, не те аргументы — ошибка.
- **Имя из переменной** (`bus.emit(topic, msg)`) вывести нельзя — нужна карта:
  `new EventEmitter<{ [topic: string]: [msg: Message] }>()`.
- **Встроенные** (`ChildProcess`, потоки, сервер) — карты уже описаны: в
  `child.on("close", code => …)` `code` — `int | undefined` (`null` в tz нет).
- Пока своего языкового сервера нет, tsc в редакторе показывает у слушателей без карты `any`.

## 3. async-слушатель

```ts
bus.on("user", async user => { await save(user) })   // ок
```

`emit` не ждёт слушателей — он всегда «запустил и пошёл дальше», поэтому async-слушатель сам
считается `void save(user)`. Это не ловушка `forEach` ([async.md](async.md), раздел 6): там код
после `forEach` думает, что всё уже сделано, а `emit` никто и не ждёт.

Поэтому и ошибку слушатель разбирает сам. Слушатель, который возвращает `Result` или
`Promise<Result>`, — ошибка компиляции: `emit` результат не читает, и `Err` пропал бы молча
([errors.md](errors.md), раздел 4).

```ts
bus.on("user", async user => save(user))          // ошибка: save возвращает Promise<Result>
bus.on("user", async user => {
  const r = await save(user)
  if (r.isErr()) console.error(r.error)            // ок: ошибка разобрана здесь
})
```

`captureRejections` нет: `Promise` не отклоняется.

## 4. Событие `error`

В node `emit("error", e)` без слушателя `error` бросает `e`, и процесс падает. У нас
исключений нет, поэтому ошибка должна прийти туда, где компилятор заставит её разобрать:

- **Подписался на события объекта, который может испустить `error`** (это видно по его карте
  событий), — подпишись и на `error` этого же объекта, в той же функции, на каждом пути.
  Проверка по путям — как у полей, которые конструктор задаёт на каждом пути.
  ```ts
  const s = createReadStream("a.txt")
  s.on("data", chunk => use(chunk))
  // ошибка: s может испустить error (нет файла, нет прав), а слушателя error нет;
  //         добавь s.on("error", e => …) или читай через for await
  ```
- **Свой `EventEmitter` шлёт `error`, а слушателя `error` у него нет нигде в программе** —
  ошибка компиляции: иначе ошибка пропадает молча. `emit` возвращает, был ли слушатель, —
  можно разобрать самому.
- **Ждать события — `Result`:** `once(emitter, name)` — `Promise<Result<[...], E>>`: `Err`, если
  раньше пришло `error`. Каждый элемент `for await (… of on(emitter, name))` — `Result`.
- `process.on("uncaughtException")` и `process.on("unhandledRejection")` — ошибки: ловить
  нечего.

## 5. Как в node

- Всё API: `on` / `addListener`, `once`, `off` / `removeListener`, `removeAllListeners`, `emit`,
  `prependListener`, `prependOnceListener`, `listenerCount`, `listeners`, `rawListeners`,
  `eventNames`, `setMaxListeners`; `once(emitter, name)` — `Promise<Result>` (с `signal`);
  `on(emitter, name)` — для `for await`. Кроме того, что про ошибки (раздел 4).
- Больше 10 слушателей одного события — предупреждение о возможной утечке памяти в stderr, как в
  node; `setMaxListeners` меняет предел.
- `this` в слушателе-функции — сам объект; у стрелки — как обычно.
- Имена событий — только строки: своих `Symbol` в tz нет ([iterators.md](iterators.md)).
- `EventTarget`, `Event`, `CustomEvent<T>` (у `detail` — тип `T`) — как в браузере; на
  `EventTarget` держится `AbortSignal` ([net.md](net.md)).

## План реализации

На каждом шаге: тесты `tests/run` с `// same-as-node`, ошибки в `tests/errors`, обновление
README.

1. **`EventEmitter` с явной картой** (сделано) — перенос `lib/events.js` на tz. Нужны классы с
   наследованием ([objects.md](objects.md), шаг 4), списки параметров в generics
   ([generics.md](generics.md), раздел 9), словари ([objects.md](objects.md), шаг 7).
2. **Вывод карты из `emit`** (сделано) по всей программе; ошибки на событие, которое никто не шлёт, и на
   разные типы одного события.
3. **`once()` и `on()`** (сделано) — после async ([async.md](async.md), шаг 1) и `for await`
   ([iterators.md](iterators.md), шаг 4); async-слушатели.
4. **`EventTarget`, `Event`, `CustomEvent`**, `AbortController` / `AbortSignal` на них (сделано).
5. Дальше на этом — потоки node, асинхронные функции `node:child_process`
   ([process.md](process.md)), `node:http` ([net.md](net.md)).
6. **`error` без исключений** (раздел 4; [errors.md](errors.md), шаг 3): правило подписки на
   `error` по путям, ошибка на `error` без слушателя у своего объекта, ошибка на слушатель с
   `Result`, `Result` у `once()` и `on()`, убрать `captureRejections`, `uncaughtException` и
   `unhandledRejection`. Переписать `library/events/*.tz`.
