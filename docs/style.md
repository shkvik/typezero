# Явный стиль библиотеки

Правила приняты 28 октября 2026 года на прототипе сервера (`tmp/design/server`). Они для кода
библиотеки (`library/`), прототипов и примеров в описаниях (`@example`). Программы пишут как
хотят: компилятор этих правил не проверяет. Старая библиотека под них ещё не переписана — что
сделано, а что нет, сказано в конце, в разделе «Что сделано».

Все правила об одном: **что происходит, видно прямо в строке.** Откуда взялась ошибка и что в
ней лежит, что может поменяться, а что нет, в каком состоянии объект — всё это читается с
экрана, без прыжков по другим местам и без знания умолчаний.

## Коротко

- **Ошибка — на месте.** `Result` — в `const` с суффиксом `Result` (`openedResult`), сразу под
  ним отдельный `if` в фигурных скобках, `return err({ ... })` с объектом прямо там, по полю на
  строку. `Result` разбирают только `if` (без `map`, `unwrapOr`…), в аргумент не передают, чужую
  ошибку насквозь не отдают.
- **Без вложенных вызовов.** Результат вызова сначала получает имя в `const`, потом идёт
  дальше. Успех тоже: `const listening = ...;` и `return ok(listening);`. Цепочка
  `text.trim().toLowerCase()` — можно, кроме `Result`.
- **Скобки всегда, тернарник один.** `{ }` у каждого `if` и цикла; `a ? b : c` можно, вложенный
  — нет.
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
  const openedResult = netListen({
    hostname: this.hostname,
    port: this.port,
    maxBodySize: this.maxBody,
    keepAliveTimeout: this.keepAlive,
    headersTimeout: this.headersTimeout,
    transport: this.transport,
  });
  if (openedResult.isErr()) {
    return err({
      kind: "SERVER",
      code: openedResult.error.code,
      message: openedResult.error.message,
      errno: openedResult.error.errno,
      syscall: openedResult.error.syscall,
      address: this.hostname,
      port: this.port,
    });
  }

  const listening = new Listening(openedResult.value, this.hostname, this.handler);

  return ok(listening);
}
```

- **`Result` лежит в `const` с суффиксом `Result`:** `openedResult`, `parsedResult`. По имени
  видно, что внутри ещё не значение, а успех или ошибка.
- **Проверка — сразу под ним**, без пустой строки: получили `Result` и тут же разобрали. Ветка
  ошибки — свой `if` в фигурных скобках, после него пустая строка. Её видно сразу, даже когда
  листаешь.
- **`Result` разбирают только `if`:** `isErr()`, потом `.value` или `.error`. Методов `Result`
  (`map`, `mapErr`, `andThen`, `orElse`, `match`, `unwrapOr` и других) в библиотеке нет:
  запасное значение и преобразование — обычный код в ветке и после неё.
- **Объект ошибки строится прямо в `err({ ... })`**, по полю на строку, и по каждому полю видно,
  откуда оно: `code`, `message`, `errno`, `syscall` — из ответа рантайма (`openedResult.error`),
  `address` и `port` — из настроек сервера.
- **Чужую ошибку насквозь не отдают:** ни `err(openedResult.error)`, ни `return openedResult`.
  Тогда не видно, что попадает наружу.
- **Ошибку не прячут в функцию:** `err(listenError(openedResult, this.hostname))` — то же
  самое, содержимое уехало в другое место.
- **Разные причины — разные `if`.** Не `if (gotResult.isErr() || gotResult.value == 0)`, а два
  `if`, у каждого своя ошибка.
- **`Result` не передают в аргумент.** Его разбирают там, где получили, а дальше идёт уже
  значение (`openedResult.value`). Параметров типа `Result` у функций и методов нет: иначе то,
  что будет при ошибке, решает другая функция, и на месте вызова этого не видно.
- **Ошибки, которой не бывает, нет и в типе.** Если значение уже проверено, его отдают готовым
  типом, а не заставляют разбирать снова: парсер `URL` проверил порт и хранит число, поэтому
  `url.portNumber(): int | undefined`, а не `parseInt(url.port())` с веткой ошибки, которая
  никогда не сработает, и не `unwrapOr(0)`, который её прячет.
- `err(...)` запоминает строку, где его вызвали ([errors.md](errors.md), раздел 2), поэтому
  местом ошибки становится именно эта ветка.

Так:

```ts
function doubled(text: string): int {
  const parsedResult = parseInt(text);
  if (parsedResult.isErr()) {
    return 0;
  }

  return parsedResult.value * 2;
}
```

Не так:

```ts
const opened = netListen(options);                           // Result без суффикса

if (openedResult.isErr()) return err(openedResult.error);    // что внутри — не видно

if (openedResult.isErr()) {
  return err(listenError(openedResult, this.hostname));      // спрятано в функцию
}

if (gotResult.isErr() || gotResult.value == 0) {             // две причины — одна ошибка
  return err({ code: "EMPTY" });
}

return parsedResult.map((n) => n * 2).unwrapOr(0);           // методы Result вместо if

const doubled = doubleOr(parsedResult, 0);                   // Result ушёл в аргумент: что
                                                             // будет при ошибке, решает
function doubleOr(r: Result<int, string>, fallback: int) {   // другая функция
  ...
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
- **`message` — понятным языком:** что не вышло, где и почему, а если ясно, как чинить, — что
  сделать. Одна строка, по-английски: `could not connect to 127.0.0.1:3000: nothing listens on
  this port`. Кода в тексте нет — он в `code`, и `console.log(error)` печатает его вместе с
  остальными полями. Поля `cause` нет: всё нужное — в `message` и в полях ошибки. Не так:
  `fetch failed`, `listen EADDRINUSE: address already in use` (тексты node).
- **Коды и метки ошибок — SCREAM_CASE:** `"EADDRINUSE"`, `"TIMEOUT_ERR"`, `"ERR_INVALID_URL"`,
  `kind: "SERVER"`. Не `"bad_url"` и не `"badUrl"`: так же пишут коды node и системы, и все
  коды выглядят одинаково. Свои коды — с префиксом `ERR_` (`ERR_BODY_USED`), «кривое на входе» —
  `ERR_INVALID_…`; системные (`ECONNREFUSED`), OpenSSL, `TIMEOUT_ERR`, `UNKNOWN` — как есть.
  `kind` есть у **каждого** типа ошибки (29 октября): `RegexError` — `"REGEX"`, `UrlError` —
  `"URL"`, `JsonError` — `"JSON"` и так далее.
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
  именем. Аргумент — это имя, поле (`this.hostname`, `openedResult.value`) или литерал. Условие,
  которое уходит в вызов, тоже получает имя (`headOnly`).
- **`await` — не вызов:** `const exchange = await netNextRequest(listener);` можно.
- **Успех — тоже через имя:** `const listening = new Listening(...);`, пустая строка,
  `return ok(listening);`.
- **Цепочка — можно.** Вызов на результате вызова — не вложенный вызов:
  `text.trim().toLowerCase()`, `users.filter((u) => u.active).map((u) => u.name).join(", ")`,
  `this.pairs().filter(...)`, `reader.cancel().then(...)`. Длинная — по шагу на строку. Нельзя
  только вызов в скобках другого вызова (`parseInt(url.port())`) и цепочки у `Result`
  (раздел 1).
- **В `return` — сразу**, если там нет вызова в аргументе: `return parsedResult.value * 2;`,
  `return text.trim().toLowerCase();`.
- **Заголовок `for` — имя.** Цепочку в цикл не кладут: сначала
  `const parts = encoding.toLowerCase().split(",").reverse();`, потом `for (const part of parts)`.
- **Объекты — по полю на строку:** в `const`, в аргументе (`netListen({ ... })`), в
  `err({ ... })`.
- **Пустая строка перед последним `return`**, если в функции больше одной инструкции.
- **Исключение — ошибки:** объект ошибки строится прямо в `err({ ... })` (раздел 1).

Не так:

```ts
const response = await ahead(handler(request, client));
response.writeTo(exchange, request.method == "HEAD");
this.url = new URL(netListenerUrl(listener));
return ok(new Listening(openedResult.value, this.hostname, this.handler));
for (const part of encoding.toLowerCase().split(",").reverse()) { ... }
```

### Ветки и циклы

```ts
for (;;) {
  const exchange = await netNextRequest(listener);
  if (exchange == undefined) {
    break;
  }
  void Listening.answer(exchange, handler);
}

const fallback = https ? 443 : 80;
```

- **Фигурные скобки — у каждого `if`, `else`, `for` и `while`**, даже когда внутри одна строка,
  а не только у ветки ошибки. Где ветка кончается, видно сразу, и строку в неё добавляют, не
  трогая соседние.
- **Тернарник — один:** `https ? 443 : 80` можно. Вложенный нельзя: тогда `if`, у каждого случая
  своя ветка.

Не так:

```ts
if (exchange == undefined) break;
for (const pair of pairs) if (pair[0] == "host") continue;
const port = text == "" ? (https ? 443 : 80) : given;
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
  private readonly handler: Handler;

  public constructor(options: ServeOptions, handler: Handler) {
    this.hostname = options.hostname ?? "0.0.0.0";
    this.port = options.port ?? 8000;
    this.maxBody = options.maxBodySize ?? 128 * 1024 * 1024;
    this.keepAlive = options.keepAliveTimeout ?? 5000;
    this.headersTimeout = options.headersTimeout ?? 60000;
    this.transport = options.transport ?? { secure: false };
    this.handler = handler;
  }
}
```

- **У объявления поля — только модификаторы и тип**, без `= значение`.
- **Все значения задаёт конструктор**, и значения по умолчанию тоже (`options.port ?? 8000`).
  Начальное состояние объекта видно в одном месте.
- **Сначала поля, потом вызовы.** Пока заданы не все поля, `this` можно только писать
  ([objects.md](objects.md), раздел 6, «Инициализация»). Поэтому свой метод конструктор может
  звать только после всех полей, а то, что нужно для самого поля, — закрытый статический метод:
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

  public constructor(listener: Listener, hostname: string, handler: Handler) { … }
  public async stop(): Promise<void> { … }
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
const listeningResult = server.listen();              // Result<Listening, ListenError>
if (listeningResult.isErr()) {
  console.error(listeningResult.error.message);
  process.exit(1);
}

console.log(listeningResult.value.url.href());        // без ?. и без нулей
await listeningResult.value.stop();
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
  метод с одним именем быть не могут, поэтому поле называется иначе (`count` у `size()`), а
  когда таких значений много — они лежат в одной закрытой записи `state`, как части адреса:

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
  ошибка. Так же `params.size()` и `stream.locked()`, как `bodyUsed()` и `desiredSize()`.
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
  `response.writeTo(exchange, headOnly)`), наблюдатель (`URL` и его `searchParams`).
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

  public constructor(listener: Listener, hostname: string, handler: Handler) {
    this.listener = listener;
    this.stopping = false;
    const href = netListenerUrl(listener);
    this.url = new URL(href);
    this.hostname = hostname;
    this.port = netListenerPort(listener);
    this.accepting = Listening.acceptAll(listener, handler);
  }

  public async stop(): Promise<void> {
    if (!this.stopping) {
      this.stopping = true;
      netCloseListener(this.listener);
    }
    await this.accepting;
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

## Что сделано

- Правила приняты 28 октября 2026 года; 29 октября — `Result` (суффикс, проверка сразу под
  `const`, только `if`, не в аргумент), цепочки, скобки у каждого `if` и цикла, один тернарник, коды ошибок SCREAM_CASE, понятный `message` без кода
  и `url.portNumber()` (примеры —
  `tmp/design/chains.tz`). Прототип сервера (`tmp/design/server`) им следует,
  новый код библиотеки пишется так.
- **Старая библиотека** (`library/`, около 11 тысяч строк) не переписана: будет по модулям,
  отдельной задачей.
- **`readonly` только в конструкторе** компилятор проверяет с 28 октября, и в библиотеке тоже;
  то, что объект меняет сам, переведено на методы ([objects.md](objects.md), раздел 6).
- **`.d.tz`** компилятор читает и сверяет с модулем с 28 октября ([modules.md](modules.md),
  раздел 7); объявлен пока `tz:http` (`library/http/http.d.tz`). Остальное описание лежит в
  `editors/types/tz.d.ts`, мосты — прямо в модулях, ручки — `object`.
- **`SystemError`** ещё есть: своя ошибка у каждого API — переделка по модулям. Тексты
  `message` пока как у node — их перепишем там же.
