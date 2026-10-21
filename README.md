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
tz run fib.ts            # скомпилировать и запустить
tz build fib.ts          # получить fib.exe
tz build -O0 fib.ts      # без оптимизаций (по умолчанию -O2)
tz build --emit-llvm fib.ts   # посмотреть LLVM IR (fib.ll)
```

Примеры программ лежат в [examples](examples): типы и преобразования, Фибоначчи, простые
числа, последовательность Коллатца.

## Что уже работает

- Типы `int` (32 бита), `long` (64 бита), `double` (он же `number`), `boolean`, `void`.
- Функции с аннотированными параметрами, рекурсия, вызов до объявления.
- `let` / `const`, блочная область видимости, глобальные переменные верхнего уровня.
- `if` / `else`, `while`, `do ... while`, `for`, `break`, `continue`, `return`.
- Арифметика, сравнения, `&&` / `||` с коротким замыканием, побитовые операции и сдвиги,
  `?:`, `++` / `--`, составное присваивание, преобразования `as`.
- `console.log` с числами, `boolean` и строковыми литералами.
- Необязательные точки с запятой (ASI), комментарии, escape-последовательности в строках.

Код верхнего уровня — это точка входа программы.

## Чем отличается от TypeScript

- Есть целые типы. `7 / 2` для `int` равно `3`; переполнение целых заворачивается, как в Go.
- Деление целого на ноль — паника: сообщение в stderr и код выхода 2.
- Неявных преобразований с потерей нет: `int` сам расширяется до `long` и `double`, всё
  остальное — через `as`. `as` действительно преобразует значение: `double as int` отбрасывает
  дробную часть и насыщается на границах (NaN → 0).
- Целочисленный литерал принимает тип из контекста: `let x: double = 1 / 2` даёт `0.5`.
- Условия в `if` / `while` / `for` / `?:` обязаны быть `boolean`.
- Без аннотации тип возврата функции — `void`, а не выведенный.
- `var` нет.

## Сборка

Нужно (Windows x64):

- Visual Studio 2022 или Build Tools с компонентом C++ (MSVC, CMake, Ninja).
- LLVM 23.1.4 — архив `clang+llvm-23.1.4-x86_64-pc-windows-msvc.tar.zst` с
  [GitHub releases](https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.4)
  (не `.msi`: в инсталляторе нет библиотек для разработки). Архив сжат с длинным окном,
  встроенный `tar` его не открывает: `zstd -d --long=31 -c архив | tar -xf - --strip-components=1`.
- [vcpkg](https://github.com/microsoft/vcpkg): из него приходят zlib, zstd и libxml2, которые
  нужны LLVM, но не входят в архив.

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

Результат — `build/dev/bin/tz.exe` и рядом `tzrt.lib` (рантайм, который линкуется в каждую
программу). Готовый LLVM собран в Release со статическим CRT, без RTTI и исключений, поэтому
`tz` собирается так же; Debug-сборка не поддерживается.

## Как устроен компилятор

```
.ts → Lexer → Parser → AST → Sema → CodeGen → LLVM IR → оптимизации → .obj → LLD → .exe
```

| Каталог | Что внутри |
|---|---|
| `src/lex` | токены и лексер |
| `src/parse` | парсер рекурсивным спуском, ASI |
| `src/ast` | узлы AST и символы |
| `src/sema` | типы, разрешение имён, проверка типов; неявные преобразования становятся узлами `CastExpr` |
| `src/codegen` | генерация LLVM IR через `IRBuilder` |
| `src/driver` | командная строка, пайплайн оптимизаций, объектный файл, встроенный LLD |
| `runtime` | рантайм на C: буферизованный вывод, форматирование чисел как в JS, паника |
| `tests` | `run/*.ts` с ожидаемым выводом в `*.out`; `errors/*.ts` с `// expect-error:` |

## Тесты

Каждый `tests/run/name.ts` компилируется с `-O0` и `-O2`, запускается, и stdout сравнивается с
`name.out`. Код выхода и stderr задаются комментариями `// expect-exit: N` и
`// expect-stderr: текст`. Каждый `tests/errors/*.ts` обязан не скомпилироваться, а в выводе должна
быть строка из `// expect-error: текст` — комментарий ставится строкой выше ошибки.
