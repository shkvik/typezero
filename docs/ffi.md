# Вызов C из программы: FFI

Решения, принятые 15 ноября 2026 года; в тот же день сделаны шаги 1–6 плана, кроме того, что
перечислено в нём как не сделанное. Зачем: компилятор tz на самом tz
(ему нужен LLVM, а LLVM — это библиотека на C), HTTP/3 пакетом поверх ngtcp2
([net.md](net.md), раздел 12), любой `setsockopt` как у libc в Rust.

## Коротко

- Функция на C объявляется так же, как мосты в рантайм у библиотеки: `declare function`. Ручка
  из C — `declare class X {}`. Имя в C — ровно как написано.
- Код на C — готовая `.lib` или исходники `.c`. Исходники собирает clang, который живёт внутри
  `tz.exe`; компилятор не выбирается.
- Что подключить, описывает массив `native` в `package.json` — он едет вместе с пакетом.
- Через границу проходят числа, `boolean`, `cstring`, ручки, массивы, `Buffer`, `Out<T>`.
  Объекты tz в C не уходят.
- C вернул `NULL` там, где объявление его не допускает, — паника. Проверка после каждого вызова,
  по скорости бесплатна (замер — раздел 4).
- Память C освобождает `[Symbol.dispose]`: tz вызывает его сам на последней ссылке.
- Вызовы одной библиотеки C идут по одному; `Promise<T>` в объявлении — вызов в пуле потоков.
- Никаких произвольных флагов clang и скриптов сборки: `native` — только описание.

## 1. Объявления

```ts
import { cstring } from "tz:string"

declare class LLVMContext {}
declare class LLVMModule {}
declare class LLVMValue {}

declare function LLVMContextCreate(): LLVMContext
declare function LLVMContextDispose(context: LLVMContext): void
declare function LLVMModuleCreateWithNameInContext(name: cstring, context: LLVMContext): LLVMModule
declare function LLVMGetFirstFunction(module: LLVMModule): LLVMValue | undefined
```

- **`declare function`** — функция существует при выполнении, но написана на C. Видна только в
  своём файле; другим модулям — через `export declare function`, как в `tz:intrinsics`.
- **Имя в C — как написано.** У библиотеки tz остаётся прежнее правило: `netClose` →
  `tz_net_close`; библиотеку загрузчик узнаёт по месту.
- **`declare class X {}`** — ручка: указатель из C. Полей нет, `new` нельзя. Ручки разных
  классов не перепутать — классы по имени. tz в ручку не заглядывает, только отдаёт её обратно в
  C; `==` сравнивает адреса.
- **Функция объявлена, а ни одна подходящая запись `native` её не даёт** — ошибка сборки на
  строке `declare function`, а не текст линковщика.

## 2. Что проходит через границу

| в объявлении | в C | как передаётся |
|---|---|---|
| `int8` … `int64`, `uint8` … `uint64` | `int8_t` … `uint64_t` | как есть |
| `double` | `double` | как есть |
| `boolean` | `bool` | как есть (`LLVMBool` — это `int`, в объявлении `int32`) |
| `cstring` — параметр | `const char *` | указатель на байты строки, без копии |
| `cstring` — результат | `const char *` | сразу копия; память C tz не трогает |
| ручка | указатель | как есть |
| `ручка \| undefined` | указатель или `NULL` | `undefined` ↔ `NULL` |
| `T[]` — параметр | `T *` | указатель на элементы, живёт до конца вызова |
| `Buffer` | `uint8_t *` | указатель на байты, живёт до конца вызова |
| `Out<T>` | `T *` | ячейка; после вызова — `out.value` |
| `Promise<T>` — результат | — | вызов уходит в пул потоков рантайма |

- **Ошибка сборки:** `string` (только `cstring`), объект tz, функция, `Map`, необязательный
  параметр, rest, generic.
- **Строку, которую C выделил и которую надо освободить** (`char *` от
  `LLVMPrintModuleToString`), объявляют ручкой; текст копирует `cstring` из ручки, освобождает —
  функция C в `[Symbol.dispose]` обёртки.
- **Не в первой версии:** колбэки из C в tz (первыми после неё — без них нет пользовательских
  функций sqlite и `qsort`), структуры по значению, функции с `...`, `float` (32-битного дробного
  типа в tz нет), загрузка `.dll` во время работы.

## 3. Строки: `cstring`

`cstring` из `tz:string` (тип придуман 21 октября вместе с `utf8string` и `osstring`, ещё не
сделан) — UTF-8 с `\0` в конце и без `\0` внутри. В C уходит указатель на его байты, без копии:
имя, которое прошло проверку один раз, отдаётся в C сколько угодно раз.

```ts
LLVMAddFunction(module, "add", type)       // литерал стал cstring
LLVMAddFunction(module, "a\0dd", type)     // ошибка: в cstring не бывает \0
LLVMAddFunction(module, name, type)        // ошибка: string сам в cstring не превращается

const cname = name.toCString()             // cstring | undefined: undefined, если в name есть \0
if (cname == undefined) {
  return undefined
}
LLVMAddFunction(module, cname, type)
```

## 4. `NULL`: проверка всегда

Ручка или `cstring`, объявленные без `| undefined`, — обещание программиста про чужой код. tz
проверяет его после каждого вызова; пришёл `NULL` — паника на строке вызова:

```
panic: LLVMCreateBuilderInContext returned NULL, its declaration says it never does
```

Без проверки `NULL` не сломал бы сам tz — он в ручку не заглядывает, — но LLVM упал бы позже,
в другом месте и со стеком в C. Где `NULL` законен, его пишут в объявлении
(`LLVMValue | undefined`), и программа сужает тип сама.

Цена — `test` и `je` на редкий путь. Замер (clang `-O2`, две почти пустые функции C в другом
файле — худший случай, 10⁹ итераций, 5 запусков): без проверки 1,496–1,512 нс на два вызова, с
проверкой 1,493–1,520 нс — разницы нет.

## 5. Освобождение: `Symbol.dispose`

Память, которую дал C, живёт, пока кто-то не вызовет её функцию освобождения. Обёртка пишет
`[Symbol.dispose]`, как в JS (Explicit Resource Management, TS 5.2), и tz вызывает его сам,
когда на объект не осталось ссылок — тот же момент, что у ресурсов в
[errors.md](errors.md), раздел 6.

```ts
export class Module {
  public readonly handle: LLVMModule
  private readonly context: Context

  public constructor(context: Context, name: cstring) {
    this.handle = LLVMModuleCreateWithNameInContext(name, context.handle)
    this.context = context
  }

  public [Symbol.dispose](): void {
    LLVMDisposeModule(this.handle)
  }
}

function compile() {
  using context = new Context()
  const module = new Module(context, "main")
  emit(module)
}                                          // ← module, потом context
```

- **Ровно один раз**, на последней ссылке. Поля объекта освобождаются после — в `dispose` они
  живы.
- **`using`** — как в JS: `dispose` в конце блока. В tz это и так конец жизни объекта; `using`
  добавляет проверку, что значение не переживёт блок (сохранили наружу — ошибка). Код TS с
  `using` работает так же, как в node.
- **`using` в заголовке цикла** — тоже как в JS: `for (using file of files)` освобождает
  значение в конце каждого прохода, `for (using reader = open(); reader.more(); )` — когда цикл
  кончился. `for (using key in map)` — ошибка: ключ — строка, освобождать нечего. Пока это
  знает только разбор в self/.
- **Вызвать `x[Symbol.dispose]()` руками — ошибка**, иначе ручку освободили бы дважды. Явное
  закрытие — свой метод класса, как `close()` у файла ([errors.md](errors.md), раздел 6).
- **Наследник со своим `[Symbol.dispose]` обязан вызвать `super[Symbol.dispose]()`**, иначе
  ресурс родителя утечёт. Это единственный разрешённый вызов руками.
- **Сохранить `this` в `dispose` нельзя** — объект уже умирает.
- **Порядок** получается сам: модуль держит контекст, значит, контекст умрёт после. У объектов,
  которые ссылаются друг на друга по кругу (одна группа, [objects.md](objects.md), раздел 11),
  порядок внутри группы не задан.
- **Не только для C:** так же закрываются файлы, сокеты, замки. `defer` из `tz:intrinsics`, на
  котором библиотека закрывает файлы сейчас (`closeOnDrop` в `stream/fs_stream.tz`),
  заменяется на `[Symbol.dispose]` — один механизм у библиотеки и у программы.

## 6. Где идёт вызов и чего C делать нельзя

- **Вызовы одной библиотеки — по одному.** tz не видит, что C делает с памятью, поэтому для
  анализа параллельности ([async.md](async.md), раздел 2) библиотека — общие данные, как файл
  или `Pool`. Разные
  библиотеки (sqlite и LLVM) идут параллельно. Какая функция из какой записи `native`, tz знает
  по символам биткода и таблице символов `.lib`.
- **`Promise<T>` в объявлении** — вызов в пуле потоков рантайма, как `randomBytesAsync`; поток
  программы работает дальше.
- **C нельзя:** хранить указатель на строку, массив или `Buffer` после вызова (память
  временная), освобождать то, что дал tz, пользоваться ручкой после её освобождения. Нарушение —
  падение процесса, как в C. Граница доверия — `declare function`; внутри C гарантий tz нет.

## 7. Откуда код на C

- **Готовая библиотека** (`libraries`): `.lib` от DLL или статическая. Так подключается LLVM —
  из исходников его собирают часами.
- **Исходники** (`sources`): tz собирает `.c` в биткод своим clang с теми же флагами, что у
  рантайма, и кладёт в модуль программы, как `tzrt.bc`. Функции C встраиваются в код tz, CRT
  всегда `/MT`. Поправил `.c` — просто собрал заново.
- **clang — внутри `tz.exe`.** Замер (выпускная линковка, `/OPT:REF`): tz без clang — 81,3 МБ,
  с ним — 118,5 МБ (+37 МБ), в gzip 31,1 → 45,9 МБ. Заголовки clang (`stdint.h`,
  `immintrin.h`…) едут рядом: 16 МБ, в gzip 1,2 МБ, без обёрток под чужие процессоры меньше.
  Системные заголовки и `libcmt.lib` — из Visual Studio, она нужна tz и сейчас.
- **`import` из `.h` не будет:** в заголовке нет того, что нужно tz, — бывает ли `NULL` и кто
  освобождает память.
- **Потом:** `tz bind Core.h > llvm_c.tz` — объявления из заголовка один раз, дальше обычный
  файл, который правят руками.

## 8. `native` в `package.json`

В `package.json`, а не в `tzconfig.json`: define'ы sqlite — не выбор того, кто собирает, без них
код пакета не работает; пакет с C едет к другим со своим `package.json`, а `tzconfig.json`
зависимости tz не читает. Пути — от папки `package.json`.

```ts
type Native = NativeSources | NativeLibraries

type NativeSources = {
  name: string
  license: string
  licenseFile: string
  sources: string[]
  defines?: string[]
  includes?: string[]
  systemLibraries?: string[]
  os?: NativeOperatingSystem[]
  cpu?: NativeProcessor[]
  debug?: NativeDebug
}

type NativeLibraries = {
  name: string
  license: string
  licenseFile: string
  libraries: string[]
  dlls?: string[]
  systemLibraries?: string[]
  os?: NativeOperatingSystem[]
  cpu?: NativeProcessor[]
}

type NativeDebug = {
  defines: string[]
}

type NativeOperatingSystem = "win32" | "linux"
type NativeProcessor = "x64" | "arm64"
```

```json
{
  "name": "tzc",
  "version": "0.1.0",
  "main": "src/main.tz",
  "native": [
    {
      "name": "sqlite",
      "license": "blessing",
      "licenseFile": "vendor/sqlite/LICENSE.md",
      "sources": ["vendor/sqlite/sqlite3.c"],
      "defines": ["SQLITE_THREADSAFE=1", "SQLITE_OMIT_LOAD_EXTENSION"],
      "debug": { "defines": ["SQLITE_DEBUG"] }
    },
    {
      "name": "llvm",
      "license": "Apache-2.0 WITH LLVM-exception",
      "licenseFile": "vendor/llvm/LICENSE.TXT",
      "os": ["win32"],
      "cpu": ["x64"],
      "libraries": ["vendor/llvm/win32-x64/LLVM-C.lib"],
      "dlls": ["vendor/llvm/win32-x64/LLVM-C.dll"]
    }
  ]
}
```

- **Одна запись — одна библиотека** под одни условия. `name` — для ошибок
  («sqlite: vendor/sqlite/sqlite3.c:120: …») и для правила «по одному внутри библиотеки». У
  зависимости tz приписывает имя пакета — с записями программы не столкнётся.
- **`os` и `cpu`** — фильтр записи, имена как `process.platform` / `process.arch` и как поля
  `os` / `cpu` у npm. Нет поля — любая платформа. Записей с одним именем может быть несколько
  (llvm под x64 и под arm64), но к одной платформе подходит не больше одной: «llvm: two
  entries fit win32-x64» — ошибка.
- **`debug`** — добавки к той же записи для `tz build --debug`. Платформа меняет, что собирать,
  — это другая запись; режим меняет, как собирать, — это добавка.
- **tz ставит сам:** платформу, CRT `/MT`, уровень оптимизации, отладочную информацию, `NDEBUG`
  в обычной сборке. Разницу в самом коде пишут в C через `#ifdef _WIN32`, `__linux__`,
  `__aarch64__`.
- **`defines`** — строками, как в документации C: `"SQLITE_THREADSAFE=1"`, без `-D`.
- **`sources`** принимает маски: `"vendor/zstd/lib/**/*.c"`, `"!vendor/zstd/lib/legacy/**"`.
- **`license` и `licenseFile` обязательны:** код библиотеки попадает в список чужого кода в exe,
  который печатает `tz licenses app.exe`.
- **Программа получает только записи, чьи функции объявила:** `tool.tz` без sqlite — sqlite в
  него не собирается и не линкуется.
- **`tz check` и `tz lsp`** библиотеки не ищут и C не собирают.
- **Не будет:** произвольных флагов clang и линковщика (`-ffast-math`, `/MD` ломают то, что
  обещает tz), скриптов сборки (`configure`, `build.rs`, install-скрипты: tz ничего из пакета
  при сборке не запускает; `config.h` кладут в `vendor` готовым), `.asm` (MASM, NASM).
- **Потом, когда понадобится:** `standard` (`"c89"`, `"c23"`; без поля — `gnu17`, как у
  clang), `cpuFeatures` (`"avx2"`; для части файлов — элемент `sources` объектом), `.S`, C++,
  `warnings` (без поля предупреждения C не показываются, ошибки — всегда), `uses` (взять
  `includes` и `defines` у другой записи).

## План реализации

На каждом шаге: тесты `tests/run` с маленькой библиотекой на C в `tests` (и исходниками, и
готовой `.lib`), ошибки в `tests/errors`, прогон под `TZ_CHECK_LEAKS=1`, обновление README.

1. **`cstring`** (сделано). Тип в `tz:string`: литерал с проверкой `\0` при сборке, `toCString()` →
   `cstring | undefined`, обратно в `string`, печать, сравнение. Представление — UTF-8 с `\0` в
   конце: объект рантайма (`runtime/core/string/cstring.c`), литерал — постоянный объект в exe.
   Тест `run/ffi_cstring`, ошибки — `errors/ffi_cstring`.
2. **`Symbol.dispose` и `using`.** `[Symbol.dispose]` как имя метода; вызов на последней ссылке;
   ошибки: вызов руками, наследник без `super[Symbol.dispose]()`, `this`, который переживает
   вызов, `using`, чьё значение переживает блок. Библиотека переходит с `defer` на
   `[Symbol.dispose]`.
   Сделано, кроме перехода библиотеки: `defer` в библиотеке пока остаётся. `dispose` зовёт функция
   освобождения объекта (у глобальных — в конце программы, в обратном порядке); на время вызова
   счётчик ссылок равен 1. Проверка `using` ищет выход значения из блока: `return`, присваивание,
   литерал массива или объекта, `new`, `yield`, замыкание; передачу аргументом в вызов она не
   ловит. Тесты `run/ffi_dispose`, `errors/ffi_dispose`.
3. **Объявления в программе.** `declare function` и `declare class` вне библиотеки; проверка
   типов на границе (раздел 2); вызов по правилам C: `cstring`, массивы и `Buffer` указателем,
   `Out<T>` ячейкой на стеке, проверка `NULL` с паникой; `Promise<T>` — в пул; правило «по одному
   внутри библиотеки» в анализе параллельности.
   Сделано, кроме `Promise<T>` (пока ошибка на объявлении). `Out<T>` — класс `tz:ffi`
   (`library/core/ffi.tz`), C пишет прямо в его поле. Вызовы C в анализе параллельности — пока один
   общий ресурс на все библиотеки C, не по библиотеке. Тесты `run/ffi_crt`, `run/ffi_null`,
   `errors/ffi_declare`, `errors/ffi_handle_class`.
4. **`native` с готовыми библиотеками.** Разбор и проверка записей, фильтр `os` / `cpu`,
   `libraries` и `systemLibraries` в линковку, `dlls` рядом с exe, лицензии в `.tzlic`, ошибка
   «нет функции» на строке `declare function`.
   Сделано. Библиотека линкуется, если в её таблице символов есть объявленная функция; текст
   лицензии вшит в exe; `tz run` находит DLL через `PATH`. Пути в записях могут быть и
   абсолютными.
5. **Исходники через clang.** Библиотеки clang в `tz.exe`, урезанные заголовки clang рядом;
   `.c` → биткод с флагами рантайма, `defines`, `includes`, `debug`, маски; кэш собранного
   биткода между сборками (хеш файла и настроек записи); ошибки C с именем записи.
   Сделано, кроме кэша: исходники собираются при каждой сборке. Флаги: `-O2 -Xclang
   -disable-llvm-passes` (оптимизирует tz вместе с программой), в `--debug` — `-O0 -g`,
   `-fms-runtime-lib=static`, `-w`. Тест `run/ffi_native`.
6. **Проба на LLVM-C.** Объявления для части LLVM-C, модуль с `add(a, b)`, вывод `.obj`, замер
   против того же на C++ — первый шаг к компилятору на tz.
   Сделано (вне репозитория, в рабочей папке сессии): программа на tz через `LLVM-C.dll` строит
   модуль, проверяет его и пишет `.obj`. 50 000 функций (`add` и `mul`) — 2193–2248 мс, та же
   программа на C через тот же `LLVM-C` — 2206–2263 мс. Нашлась дыра: текст из `char *`, который
   надо освобождать (`LLVMPrintModuleToString`), получить нельзя — нет функции «`cstring` из
   ручки».
