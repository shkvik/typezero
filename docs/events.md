# События: `EventEmitter`

Решения, принятые 24 октября 2026 года. Шаги 1–4 плана сделаны 24 октября; что осталось — в
конце, в разделе «План реализации».

## Коротко

- `node:events` — как в node: `EventEmitter`, `once()`, `on()`. `EventTarget`, `Event`,
  `CustomEvent` — как в браузере.
- Типы событий компилятор выводит сам — из всех `emit` в программе. Подписка на событие,
  которое никто не шлёт, — ошибка компиляции (обычно это опечатка). Явная карта событий —
  можно; обязательна, если имя события берётся из переменной.
- async-слушатель разрешён: он сам считается «запустить и не ждать».
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
const [port] = await once(server, "listening")   // дождаться события как Promise
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
  class Server extends EventEmitter<{ listening: [port: number]; close: [] }> { … }
  const users = new EventEmitter<{ user: [user: User] }>()
  ```
  С картой `emit` и `on` проверяются по ней: лишнее событие, не те аргументы — ошибка.
- **Имя из переменной** (`bus.emit(topic, msg)`) вывести нельзя — нужна карта:
  `new EventEmitter<{ [topic: string]: [msg: Message] }>()`.
- **Встроенные** (`ChildProcess`, потоки, сервер) — карты уже описаны: в
  `child.on("close", code => …)` `code` — `number | null`.
- Пока своего языкового сервера нет, tsc в редакторе показывает у слушателей без карты `any`.

## 3. async-слушатель

```ts
bus.on("user", async user => { await save(user) })   // ок
```

`emit` не ждёт слушателей — он всегда «запустил и пошёл дальше», поэтому async-слушатель сам
считается `void save(user)`. Это не ловушка `forEach` ([async.md](async.md), раздел 6): там код
после `forEach` думает, что всё уже сделано, а `emit` никто и не ждёт. Ошибка внутри слушателя —
как не пойманная ошибка в задаче: в stderr и выход, как в node. С `captureRejections: true` —
событие `error`, как в node.

## 4. Как в node

- Всё API: `on` / `addListener`, `once`, `off` / `removeListener`, `removeAllListeners`, `emit`,
  `prependListener`, `prependOnceListener`, `listenerCount`, `listeners`, `rawListeners`,
  `eventNames`, `setMaxListeners`; `once(emitter, name)` — `Promise` (с `signal`);
  `on(emitter, name)` — для `for await`.
- `emit("error")` без слушателя `error` — бросает, как в node.
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
