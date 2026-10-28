# Явный стиль библиотеки

Правила приняты 28 октября 2026 года на прототипе сервера (`tmp/design/server`). Они для кода
библиотеки (`library/`), прототипов и примеров в описаниях (`@example`). Программы пишут как
хотят: компилятор этих правил не проверяет. Старая библиотека под них ещё не переписана — что
сделано, а что нет, сказано в конце, в разделе «Что сделано».

Все правила об одном: **что происходит, видно прямо в строке.** Откуда взялась ошибка и что в
ней лежит, что может поменяться, а что нет, в каком состоянии объект — всё это читается с
экрана, без прыжков по другим местам и без знания умолчаний.

## Коротко

- **Ошибка — на месте.** Отдельный `if` в фигурных скобках, `return err({ ... })` с объектом
  прямо там, по полю на строку. Чужую ошибку насквозь не отдают.
- **Без вложенных вызовов.** Результат вызова сначала получает имя в `const`, потом идёт
  дальше. Успех тоже: `const listening = ...;` и `return ok(listening);`.
- **Поля задаёт конструктор.** У объявления поля только модификаторы и тип.
- **Модификаторы всегда.** `public`, `protected` или `private` у каждого поля, метода и
  конструктора, `public` тоже.
- **Подписи с типами.** Параметры и результат методов подписаны, локальные `const` — нет.
- **Типы через `type`.** Варианты — объединение объектов с меткой прямо в типе.
- **Неверное состояние не записать.** Состояние — это тип: `Server` настроен, `Listening`
  слушает.
- **Неизменяемость.** `readonly` задаёт только конструктор. То, что объект меняет сам, лежит в
  закрытом поле и читается методом.
- **Внутреннее закрыто.** `private`, мосты в рантайм — в `tz:internal`, имён с `_` нет.
- **В коде нет комментариев.** Описание с примерами — в объявлении `.d.tz`.

Слово «явный» собирает известные идеи:

- ошибки — значения, а не исключения (Go, Rust, Zig);
- нет скрытого хода выполнения (Zig): ни исключений, ни геттеров, ни неявных вызовов;
- неверное состояние нельзя записать (make illegal states unrepresentable), состояние — это
  тип (typestate);
- неизменяемость;
- объявление отдельно от кода (заголовки C, `.d.ts`).

## 1. Ошибки

```ts
public listen(): Result<Listening, ListenError> {
  const opened = netListen({
    hostname: this.hostname,
    port: this.port,
    maxBodySize: this.maxBody,
    keepAliveTimeout: this.keepAlive,
    headersTimeout: this.headersTimeout,
    transport: this.transport,
  });

  if (opened.isErr()) {
    return err({
      kind: "SERVER",
      code: opened.error.code,
      message: opened.error.message,
      errno: opened.error.errno,
      syscall: opened.error.syscall,
      address: this.hostname,
      port: this.port,
    });
  }

  const listening = new Listening(
    opened.value,
    this.hostname,
    this.handler,
    this.signal,
  );

  return ok(listening);
}
```

- **Ветка ошибки — свой `if`** в фигурных скобках, с пустой строкой до и после. Её видно сразу,
  даже когда листаешь.
- **Объект ошибки строится прямо в `err({ ... })`**, по полю на строку, и по каждому полю видно,
  откуда оно: `code`, `message`, `errno`, `syscall` — из ответа рантайма (`opened.error`),
  `address` и `port` — из настроек сервера.
- **Чужую ошибку насквозь не отдают:** ни `err(opened.error)`, ни `return opened`. Тогда не
  видно, что попадает наружу.
- **Ошибку не прячут в функцию:** `err(listenError(opened, this.hostname))` — то же самое,
  содержимое уехало в другое место.
- **Разные причины — разные `if`.** Не `if (got.isErr() || got.value == 0)`, а два `if`, у
  каждого своя ошибка.
- `err(...)` запоминает строку, где его вызвали ([errors.md](errors.md), раздел 2), поэтому
  местом ошибки становится именно эта ветка.

Не так:

```ts
if (opened.isErr()) return err(opened.error);       // что внутри — не видно

if (opened.isErr()) {
  return err(listenError(opened, this.hostname));   // спрятано в функцию
}

if (got.isErr() || got.value == 0) {                // две причины — одна ошибка
  return err({ code: "EMPTY" });
}
```

### Тип ошибки

```ts
export type ListenError = {
  kind: "SERVER";
  code: ListenErrorCode;
  message: string;
  errno: int;
  syscall: string;
  address: string;
  port: int;
};

export type ListenErrorCode =
  | "EADDRINUSE"
  | "EACCES"
  | "EADDRNOTAVAIL"
  | "ENOTFOUND"
  | "ERR_INVALID_ARG_VALUE"
  | "ERR_TLS_CERT"
  | "ERR_TLS_KEY";
```

- **У каждого API своя ошибка**, и в ней только те поля, что правда бывают. У `ListenError`
  нет `path` и `dest`: при `listen` их не бывает. Общей `SystemError` на всю библиотеку не
  будет.
- **Метка вида — строка:** `kind: "SERVER"` (ошибка ядра сервера: сокет, сертификат). Не
  `enum`: строковые литералы tz и так проверяет.
- **Код — объединение строк.** `switch` по `code` проверяется на полноту, а опечатка в коде —
  ошибка компиляции.
- **Поля выписаны все**, без `extends`: всю ошибку видно в одном месте.

## 2. Без вложенных вызовов

```ts
private static async answer(
  exchange: Exchange,
  handler: Handler,
): Promise<void> {
  const request = Request.readFrom(exchange);
  const client = netClientOf(exchange);
  const handled = handler(request, client);
  const response = await ahead(handled);
  const headOnly = request.method == "HEAD";
  const written = response.writeTo(exchange, headOnly);

  if (written.kind == "streaming") {
    await written.finished;
  }
}
```

Читается сверху вниз: запрос → клиент → обработчик → ответ → запись. У каждого шага есть имя.

- **Вызов не бывает аргументом другого вызова.** Его результат сначала кладут в `const` с
  именем. Аргумент — это имя, поле (`this.hostname`, `opened.value`) или литерал. Условие,
  которое уходит в вызов, тоже получает имя (`headOnly`).
- **`await` — не вызов:** `const exchange = await netNextRequest(listener);` можно.
- **Успех — тоже через имя:** `const listening = new Listening(...);`, пустая строка,
  `return ok(listening);`.
- **Объекты — по полю на строку:** в `const`, в аргументе (`netListen({ ... })`), в
  `err({ ... })`.
- **Пустая строка перед последним `return`**, если в функции больше одной инструкции.
- **Исключение — ошибки:** объект ошибки строится прямо в `err({ ... })` (раздел 1).

Не так:

```ts
const response = await ahead(handler(request, client));
response.writeTo(exchange, request.method == "HEAD");
this.url = new URL(netListenerUrl(listener));
return ok(new Listening(opened.value, this.hostname, this.handler, this.signal));
```

## 3. Поля задаёт конструктор

```ts
export class Server {
  private readonly hostname: string;
  private readonly port: int;
  private readonly maxBody: double;
  private readonly keepAlive: int;
  private readonly headersTimeout: int;
  private readonly transport: ServeTransport;
  private readonly signal?: AbortSignal;
  private readonly handler: Handler;

  public constructor(options: ServeOptions, handler: Handler) {
    this.hostname = options.hostname ?? "0.0.0.0";
    this.port = options.port ?? 8000;
    this.maxBody = options.maxBodySize ?? 128 * 1024 * 1024;
    this.keepAlive = options.keepAliveTimeout ?? 5000;
    this.headersTimeout = options.headersTimeout ?? 60000;
    this.transport = options.transport ?? { secure: false };
    this.signal = options.signal;
    this.handler = handler;
  }
}
```

- **У объявления поля — только модификаторы и тип**, без `= значение`.
- **Все значения задаёт конструктор**, и значения по умолчанию тоже (`options.port ?? 8000`).
  Начальное состояние объекта видно в одном месте.
- **Сначала поля, потом вызовы.** Пока заданы не все поля, `this` можно только писать
  ([objects.md](objects.md), раздел 6, «Инициализация»). Поэтому свой метод конструктор зовёт
  после всех полей (`this.stopOnSignal(signal)` в конце у `Listening`), а то, что нужно для
  самого поля, — закрытый статический метод:
  `this.accepting = Listening.acceptAll(listener, handler);`.

Не так (так сейчас в `library/http`):

```ts
private stopping = false;
readonly port: int = 0;
```

## 4. Модификаторы и подписи

```ts
export class Listening {
  public readonly url: URL;
  public readonly hostname: string;
  public readonly port: int;

  private readonly listener: Listener;
  private readonly accepting: Promise<void>;
  private stopping: boolean;

  public constructor(listener: Listener, hostname: string, handler: Handler, signal?: AbortSignal) { … }
  public async stop(): Promise<void> { … }
  private stopOnSignal(signal?: AbortSignal): void { … }
  private static async acceptAll(listener: Listener, handler: Handler): Promise<void> { … }
  private static async answer(exchange: Exchange, handler: Handler): Promise<void> { … }
}
```

- **Модификатор у всего:** у каждого поля, метода и конструктора — `public`, `protected` или
  `private`. `public` тоже, хотя он по умолчанию: читателю не надо помнить умолчания, по строке
  видно, что программе можно трогать. В `.d.tz` — так же.
- **Параметры и результат подписаны** (`: Promise<void>`, `: Result<Listening, ListenError>`) — у
  методов и у функций. Контракт читается без тела.
- **Локальные `const` — без типа.** Тип выводит компилятор: `const handled = handler(request,
  client);` — это `Response | Promise<Response>`. Сам компилятор подписей не требует нигде; это
  правило только для подписей библиотеки.

## 5. Типы

```ts
export type ServeOptions = {
  port?: int;
  hostname?: string;
  signal?: AbortSignal;
  maxBodySize?: double;
  keepAliveTimeout?: int;
  headersTimeout?: int;
  transport?: ServeTransport;
};

export type ServeTransport =
  | {
      secure: false;
    }
  | {
      secure: true;
      cert: string;
      key: string;
    };
```

- **`type`, а не `interface`.**
- **Варианты — объединение объектов с меткой прямо в типе**, каждый вариант раскрыт, по полю на
  строку. `secure: true` без `cert` и `key` не скомпилируется, `secure: false` с ними — тоже.
- **Общие поля — вне вариантов:** `port` и `hostname` лежат в `ServeOptions` и не повторяются в
  каждом варианте `transport`.
- **Все поля выписаны**, без `extends`.
- **Необязательное поле — `x?: T`**, а не `x: T | undefined`.
- **Метки и коды — строки** (`"SERVER"`, `"EADDRINUSE"`, `"streaming"`), а не `enum`.

Не так:

```ts
export interface ServeOptions {
  port?: int;
  secure?: boolean;
  cert?: string;      // нужен только при secure: true, но тип этого не знает
  key?: string;
}
```

## 6. Неверное состояние нельзя записать

```ts
const server = new Server({ port: 3000 }, handler);   // настройки и обработчик, не меняется
const listening = server.listen();                    // Result<Listening, ListenError>

if (listening.isErr()) {
  console.error(listening.error.message);
  process.exit(1);
}

console.log(listening.value.url.href);                // без ?. и без нулей
await listening.value.stop();
```

- **Состояние — это тип.** `Server` — только настройки, и они не меняются. Слушающий сервер —
  другой объект, `Listening`. Без сокета его не бывает, поэтому `url`, `port` и `stop()` у него
  есть всегда.
- **Переход в другое состояние отдаёт новый объект** (`listen()` → `Listening`), а не меняет
  флаги у старого.
- **Нет заглушек «ещё не задано»:** ни `port = 0` до `listen`, ни `url?`, который появится
  потом, ни пар необязательных полей, которые задаются вместе.
- **Если создание может не получиться**, конструктор закрыт, а объект делает статическая
  функция с `Result` ([objects.md](objects.md), раздел 6): `Port.parse(text)`.

Не так (так сейчас в `library/http`):

```ts
export class Server {
  readonly port: int = 0;           // 0, пока не слушает
  readonly url?: URL;               // есть только после listen
  readonly finished: Promise<void>;
  private listener?: object;        // есть только после listen
  private listened = false;         // второй listen — ошибка ERR_SERVER_ALREADY_LISTEN
  private stopping = false;
}
```

## 7. Неизменяемость

- **Настройки — в конструкторе, поля `readonly`.** У `Server` все поля `private readonly`: после
  создания их не меняет никто.
- **`readonly` задаёт только конструктор**, как в TypeScript: в своих методах класс тоже не
  пишет в `readonly`-поле.
- **То, что объект меняет сам, закрыто и читается методом**, названным как значение. Поле и
  метод с одним именем быть не могут, поэтому поле называется иначе (`count` у `size()`,
  `isAborted` у `aborted()`), а когда таких значений много — они лежат в одной закрытой записи
  `state`, как части адреса:

  ```ts
  type UrlParts = {
    pathname: string;
    search: string;
  };

  export class URL {
    private state: UrlParts;

    public constructor(pathname: string, search: string) {
      this.state = {
        pathname: pathname,
        search: search,
      };
    }

    public pathname(): string {
      return this.state.pathname;
    }

    public setPathname(value: string): void {
      this.state.pathname = value;
    }
  }
  ```

  Программа: `url.pathname()` читает, `url.setPathname("/x")` меняет, `url.pathname = "/x"` —
  ошибка. Так же будут `params.size()`, `signal.aborted()`, `stream.locked()` — как уже сейчас
  `bodyUsed()` и `desiredSize()`.
- **Неизменная ссылка остаётся полем:** `url.searchParams` — всегда один и тот же объект, это
  `public readonly`-поле.
- **Своё рабочее состояние** (`private stopping: boolean;` у `Listening`) — просто закрытое поле
  без `readonly`.

## 8. Внутреннее закрыто

- **Чего программе не нужно — `private`:** поля, методы, статические методы (`acceptAll`,
  `answer`). `protected` — только для крючков, которые переопределяет наследник (`onRead`,
  `onTransform` у потоков).
- **Имён с `_` и `__` нет.**
- **Объекты библиотеки говорят друг с другом только через открытое**, и каждый открытый метод
  держит объект в порядке, кто бы его ни вызвал. Пометок «только для библиотеки» нет. Приёмы:
  объект сам читает и пишет себя (`Request.readFrom(exchange)`,
  `response.writeTo(exchange, headOnly)`), фабрика отдаёт право менять
  (`AbortSignal.withAbort()`), наблюдатель (`URL` и его `searchParams`).
- **Мосты в рантайм** — `declare function` с телом на C (`netListen` → `tz_net_listen`). Их
  место — внутренние модули `tz:internal/…`. Ручки рантайма — свои типы (`Listener`,
  `Exchange`), а не `object`: одну нельзя отдать вместо другой.
- **Примитивы компилятора** (`ahead`, `order`) — из `tz:intrinsics`, его импортирует только
  библиотека.

## 9. Комментарии и объявления

- **В `.tz` комментариев нет.**
- **Описание — в объявлении `X.d.tz` рядом с `X.tz`:** открытая часть с JSDoc и `@example`. Его
  показывает редактор, из него tz делает `.d.ts` для TypeScript. Компилятор сверяет, что в
  `.d.tz` ровно то, что открывает `.tz`, с теми же типами и модификаторами.
- **Примеры в `@example` — в том же стиле:** ошибка отдельным `if`, у шагов имена.
- **Раскладка:** класс — в своей папке, файл назван как папка (`server/server.tz`,
  `listening/listening.tz`). Части — рядом, с суффиксом: `server.params.tz` (типы параметров),
  `server.errors.tz` (ошибки), `.results` — если есть. Часть есть, только если в ней что-то есть.
  У каждого файла — свой `.d.tz`.

## 10. Всё вместе

Класс `Listening` из прототипа: запущенный сервер.

```ts
import { Request, Response } from "tz:fetch";
import { URL } from "tz:url";
import { AbortSignal, Event } from "tz:web";
import { ahead } from "tz:intrinsics";
import {
  Exchange,
  Listener,
  netClientOf,
  netCloseListener,
  netListenerPort,
  netListenerUrl,
  netNextRequest,
} from "tz:internal/net";
import { Handler } from "../server/server.params";

export class Listening {
  public readonly url: URL;
  public readonly hostname: string;
  public readonly port: int;

  private readonly listener: Listener;
  private readonly accepting: Promise<void>;
  private stopping: boolean;

  public constructor(
    listener: Listener,
    hostname: string,
    handler: Handler,
    signal?: AbortSignal,
  ) {
    this.listener = listener;
    this.stopping = false;
    const href = netListenerUrl(listener);
    this.url = new URL(href);
    this.hostname = hostname;
    this.port = netListenerPort(listener);
    this.accepting = Listening.acceptAll(listener, handler);
    this.stopOnSignal(signal);
  }

  public async stop(): Promise<void> {
    if (!this.stopping) {
      this.stopping = true;
      netCloseListener(this.listener);
    }
    await this.accepting;
  }

  private stopOnSignal(signal?: AbortSignal): void {
    if (signal == undefined) {
      return;
    }
    if (signal.aborted()) {
      void this.stop();
      return;
    }
    signal.addEventListener(
      "abort",
      (event: Event) => {
        void this.stop();
      },
      { once: true },
    );
  }

  private static async acceptAll(
    listener: Listener,
    handler: Handler,
  ): Promise<void> {
    for (;;) {
      const exchange = await netNextRequest(listener);
      if (exchange == undefined) {
        break;
      }
      void Listening.answer(exchange, handler);
    }
  }

  private static async answer(
    exchange: Exchange,
    handler: Handler,
  ): Promise<void> {
    const request = Request.readFrom(exchange);
    const client = netClientOf(exchange);
    const handled = handler(request, client);
    const response = await ahead(handled);
    const headOnly = request.method == "HEAD";
    const written = response.writeTo(exchange, headOnly);

    if (written.kind == "streaming") {
      await written.finished;
    }
  }
}
```

## Не решено

- **Цепочки вызовов.** `text.trim().toLowerCase()` и `r.map((n) => n * 2).unwrapOr(0)` — это
  вызов на результате вызова. По разделу 2 каждый шаг идёт в свой `const`. Или цепочку в одну
  строку можно?

## Что сделано

- Правила приняты 28 октября 2026 года. Прототип сервера (`tmp/design/server`) им следует,
  новый код библиотеки пишется так.
- **Старая библиотека** (`library/`, около 11 тысяч строк) не переписана: будет по модулям,
  отдельной задачей.
- **`readonly` только в конструкторе** компилятор проверяет с 28 октября, и в библиотеке тоже;
  то, что объект меняет сам, переведено на методы ([objects.md](objects.md), раздел 6).
- **`.d.tz`** компилятор читает и сверяет с модулем с 28 октября ([modules.md](modules.md),
  раздел 7); объявлен пока `tz:http` (`library/http/http.d.tz`). Остальное описание лежит в
  `editors/types/tz.d.ts`, мосты — прямо в модулях, ручки — `object`.
- **`SystemError`** ещё есть: своя ошибка у каждого API — переделка по модулям.
