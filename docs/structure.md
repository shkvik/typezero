# Устройство кода компилятора

План переделки, принят 25 октября 2026 года. Работающий образец — игрушечный компилятор в
[tmp/](../tmp/README.md): там всё устроено так, как здесь описано, на четырёх проходах и двух фичах.

Переделка сделана 25–26 октября: код разложен по трём деревьям — `compiler/` (C++), `runtime/` (C)
и `library/` (tz), темы библиотеки в них называются одинаково (вариант 2 из
[tmp/layouts.md](../tmp/layouts.md)); `Sema` и `CodeGen` разрезаны на проходы и маленькие классы, темы
подключены через `Feature`. Поведение не менялось: все тесты проходят как до переделки. Где что
лежит сейчас и что из плана ещё не так — раздел «Сейчас»; разделы после него — цель, как она была
задумана.

## Зачем

Код вырос до 102 тысяч строк C++, и путь от файла `.tz` до exe по нему не прочитать:

- `Sema` — один класс на 634 метода и 145 полей общего состояния, разложенный по 50 файлам;
  `sema.cpp` — 5339 строк;
- `CodeGen` — такой же, на 472 метода и 173 поля;
- библиотека подключена цепочками `if (isGlobalName(..., "Math"))` в `Sema::checkCall` и
  `enum Builtin` на ~500 значений в `ast.h`, по которому `CodeGen` делает `switch`;
- конвейер есть, но спрятан: `driver.cpp` зовёт `Sema::check`, а тот внутри по очереди объявляет,
  выводит типы возврата, проверяет тела, делает копии generics и проверяет программу целиком;
- самые большие файлы: `sema.cpp` 5339, `parallel.cpp` 4674, `fs.c` 4390, `overflow.cpp` 4099,
  `stream.tz` 3989, `parser.cpp` 3670, `codegen.cpp` 3420, `codegen_cycles.cpp` 3337 строк.

## Коротко

- **Конвейер проходов.** `compiler/pipeline/pipeline.cpp` — весь путь от файла до exe: по
  проходу на несколько строк. Проход — класс с `run`.
- **Три дерева по виду кода.** `compiler/` — весь C++ (`tz.exe`), `runtime/` — весь C (`tzrt.lib`,
  `tzrt.bc`, `tzcrypto.lib`), `library/` — весь код на tz (`bin/lib`).
- **Библиотека — темы.** Всё, что программа берёт из глобального имени или модуля (`Math`,
  `node:crypto`, `Date`…), — тема с одним именем во всех деревьях: проверка и построение IR в
  `compiler/library/<тема>/`, рантайм на C в `runtime/library/<тема>/`, код на tz в
  `library/<тема>/`. Проходы не знают имён тем.
- **Каждый класс — в своей папке.** Маленькие классы вместо `Sema` и `CodeGen`.
- **Ошибки — значения.** Проход отдаёт свои `Errors`; `compile` отдаёт `Result` через `ok` и `err`,
  как в neverthrow.

## Правила кода

То, что решено на примере [tmp/](../tmp/README.md):

- **Каждый класс — в своей папке**, названной как класс: `ExprChecker` — это
  `expr_checker/expr_checker.h` и `expr_checker.cpp`. У класса из одних данных — только `.h`.
  Папки без файлов (`check/`, `ast/`, `compiler/library/crypto/`) только собирают классы в группы.
  Модуль на C — так же: `runtime/core/string/string.c` и его `.h`.
- **`#include` — от корня репозитория:** `"compiler/check/sema/sema.h"`,
  `"runtime/core/tzrt/tzrt.h"`.
- **Класс — одна работа.** Если в описании класса нужно «и», это два класса. Файл класса —
  до ~400 строк; больше — значит, внутри два класса.
- **Папки проходов без номеров**: порядок записан в `pipeline.cpp`.
- **Заголовок подключает только то, без чего не обойтись**, остальное — объявления
  (`class Type;`). `#include` — одним блоком, без пустых строк: свой заголовок, файлы проекта,
  системные.
- **Тела `if`, `else`, `for`, `while` — всегда в фигурных скобках.**
- **Перед итоговым `return` функции — пустая строка**, если до него есть другой код.
- **Форматирование** — `.clang-format` в корне (из `tmp/`): стиль LLVM, строки до 100 знаков,
  `InsertBraces`, `IncludeBlocks: Merge`, без комментариев у `namespace`. Пустую строку перед
  `return` он не ставит — её ставим сами.
- **Имена и отступы** — как сейчас.
- **Комментарии** — не решено, см. «Открытые вопросы».

## Верхний уровень

```
compiler/         весь C++ → tz.exe
  cli/            команды tz: разбор аргументов, build, run, init, add, port, licenses
  pipeline/       конвейер проходов от .tz до exe
  program/        что идёт по конвейеру
  load/ declare/ infer/ check/ instances/ whole_program/ overflow/ parallel/ memory/ lower/
  optimize/ emit/ link/                                                       проходы
  packages/       tz.json, tz.lock, JSR, git, tz port
  library/        C++-часть тем: feature/, feature_registry/, tz_modules/, <тема>/
runtime/          весь C → tzrt.lib, tzrt.bc, tzcrypto.lib
  core/           рантайм языка: память, строки, массивы, async, потоки
  library/        C-часть тем: <тема>/<модуль>/
library/          весь tz → bin/lib: <тема>/*.tz
editors/          vscode/ — расширение VS Code; types/ — объявления для будущего плагина tsserver
tests/ docs/ bench/ examples/ third_party/
```

`src/` больше нет. Тема, у которой есть код нескольких видов, лежит в нескольких деревьях под одним
именем: `node:crypto` — `compiler/library/crypto/`, `runtime/library/crypto/` и `library/crypto/`.

## Сейчас

```
compiler/
  CMakeLists.txt     все .cpp под compiler/ по маске
  pipeline/          pipeline/ — checkProgram и buildProgram; build_options/, runtime_files/
  cli/               main.cpp, args/, command_line/, *_command/ (build, run, init, add, remove,
                     update, port, licenses, version, explain), error_printer/
  program/           program/, module/, source_file/, ast/ (по папке на узел; ast.h — все разом),
                     symbols/, types/ (type/ — класс Type; type_op/, union_slot/, field/, overload/…
                     и модули функций над типами), diagnostic/, diagnostics/, errors/
  result/            result/, ok/, err/
  load/              load_pass/, loader/, module_resolver/, library_modules/, lexer/, token/,
                     literal_text/, parser/ и его части: token_cursor/, stmt_parser/, var_parser/,
                     pattern_parser/, expr_parser/, call_parser/, primary_parser/, template_parser/,
                     function_parser/, class_parser/, decl_parser/, module_parser/, type_parser/,
                     object_type_parser/, function_type_parser/
  declare/           declare_pass/, prelude/, module_declarer/, import_binder/, type_declarer/,
                     constant_folder/, class_declarer/, function_names/
  infer/             infer_pass/, call_cycles/, return_inferrer/
  check/             check_pass/, checker/ (Checker), event_rounds/, scopes/, facts/,
                     top_level_checker/; expr/ stmt/ flow/ types/ values/ objects/ functions/
                     classes/ generics/ errors/ iterators/ async/ — по папке на класс
  instances/         instances_pass/, late_classes/, constructor_calls/
  whole_program/     whole_program_pass/, load_order/, unused/, wide_writes/, flows/, flow_*/
  overflow/          overflow_pass/, overflow_check/ (общее), value_range/, program_scan/, slots/,
                     executor/, evaluator/, assign_evaluator/, builtin_evaluator/, call_ranges/,
                     loop_ranges/, narrowing/, overflow_warnings/
  parallel/          parallel_pass/, analysis/ (общее), pieces/, code_walk/, expr_walk/, call_walk/,
                     call_resolver/, alias_analysis/, escape_scan/, library_effects/,
                     builtin_effects/, conflicts/, parallel_analysis/, explainer/,
                     explain_names/, explain_titles/
  memory/            memory_pass/, memory_plan/ (MemoryPlan), memory_analysis/, cycle_scanner/ (+
                     cycle_exprs/, cycle_escapes/), cycle_finder/, held_members/, owner_scanner/,
                     store_scanner/ (+ store_stmts/, store_exprs/, store_calls/)
  lower/             lower_pass/, lowering/ (Lowering), module_builder/, runtime_functions/,
                     function_builder/, function_frame/, layouts/; expr/ stmt/ values/ objects/
                     functions/ classes/ types/ errors/ iterators/ async/ memory/
  optimize/          optimize_pass/, runtime_linker/, target/
  emit/              emit_pass/, object_file/, file_writer/, ir_writer/
  link/              link_pass/, linker/, licenses/, licenses_pass/
  packages/          по классу на папку: manifest/, lock/, resolver/, package_graph/, jsr_client/,
                     git_client/, http_client/, json_parser/, scanner/, port/…
  library/           feature/, feature_registry/; темы: builtin_modules bytes child_process
                     console crypto date decimal events fs intl json math os path process
                     reflect regex result stream tz_modules weakref — в каждой <тема>_feature/
                     и классы проверки и IR
runtime/
  core/              tzrt/ (только общее: TZ_HOT, дескрипторы типов, раскладки), output/ (← tzrt.c),
                     alloc/ group/ publish/ closure/ string/ unicode/ number/ array/ object/ map/
                     unknown/ error/ async/ io/ parallel/ — у каждого модуля свой заголовок, файл
                     подключает те, чьи имена берёт
  library/           темы: bytes child_process console crypto date decimal events fs http intl
                     json math net os path process reflect regex stream tls tz_modules;
                     fs/ — fs, fs_errors, fs_sync, fs_dirs, fs_async, fs_async_dirs, stdin,
                     readline, fs_stream (← fs.c); json/ — json, json_values, json_parse,
                     json_stringify, json_typed (← json.c)
library/             по теме на папку; stream/stream.tz и его части stream_readable.tz,
                     stream_writable.tz, stream_duplex.tz, stream_pipeline.tz
```

Что не так, как в цели, и почему:

- **`enum Builtin` остался.** Вызов темы несёт `call->builtin`, а не `call.feature` и `call.op`;
  `LowerPass` находит тему вызова через `FeatureRegistry::lowering(builtin)`. Значения `Builtin`
  встречаются в 147 файлах (2249 мест), по ним идут 71 `switch`, а несколько проверок сравнивают
  значения по порядку (`b >= Builtin::ArrForEach && b <= Builtin::ArrSortWith`): заменить их на
  списки операций тем — отдельный шаг со своей проверкой.
- **`Checker` и `Lowering` держат общее состояние прохода** — поля, которые нужны многим классам
  (`checker.h` — 1461 строка, `lowering.h` — 964), а не только ссылки.
- **Крючки тем** — те, что понадобились: `prepare`, `declare`, `checkAnyCall`, `checkStaticCall`,
  `checkMethodCall`, `methodReceiver`, `checkClassMethodCall`, `checkValueMethodCall`,
  `checkFunctionCall`, `checkLibraryCall`, `lowers` и `lowerCall`. Свойства, операторы, литералы,
  рантайм и библиотеки тем пока в ядре.
- **`node:stream` — части одного модуля.** `stream.tz` разрезан на пять файлов, но загрузчик
  читает их как один модуль (`kModuleParts` в `compiler/load/library_modules/`): у них одна область
  видимости, как у одного файла, а компилятор узнаёт классы `node:stream` по модулю.
- **Классы больше ~400 строк** ещё есть: `Type` (`type.cpp`, 1229 — его фабрики),
  `FilePort` (883) и десяток файлов по 500–600 строк; функции по 200–450 строк — большие `switch`
  по `Builtin`, которые уйдут вместе с ним.
- **Конвейер** чуть другой, чем в разделе «Конвейер»: `checkProgram` отдаёт `Errors`, а круги
  карт событий повторяют проходы от загрузки до `WholeProgramPass`.

## Конвейер

Сборка — две функции в `compiler/pipeline/pipeline.cpp`: проверить программу и построить exe.

```cpp
Result<Program, Errors> checkProgram(const BuildOptions &options,
                                     const FeatureRegistry &features) {
  EventRounds rounds;
  for (;;) {
    Program program(rounds.maps());

    LoadPass load(options);
    Errors loadErrors = load.run(program);
    if (loadErrors) {
      return err(loadErrors);
    }

    DeclarePass declare(features);
    Errors errors = declare.run(program);

    InferPass infer;
    errors.add(infer.run(program));

    CheckPass check(features);
    errors.add(check.run(program));
    if (rounds.again(program)) {
      continue;
    }

    InstancesPass instances;
    errors.add(instances.run(program));

    WholeProgramPass whole;
    errors.add(whole.run(program));

    OverflowPass overflow;
    errors.add(overflow.run(program));
    if (errors) {
      return err(errors);
    }

    return ok(std::move(program));
  }
}

Result<Build, Errors> build(const BuildOptions &options) {
  FeatureRegistry features = FeatureRegistry::standard();
  Result<Program, Errors> checked = checkProgram(options, features);
  if (checked.isErr()) {
    return err(checked.error());
  }
  Program &program = checked.value();

  ParallelPass parallel(options);
  ParallelPlan plan = parallel.run(program);

  MemoryPass memory;
  MemoryPlan memoryPlan = memory.run(program);

  LowerPass lower(features, plan, memoryPlan);
  std::unique_ptr<llvm::Module> ir = lower.run(program);

  OptimizePass optimize(options);
  Errors optimizeErrors = optimize.run(*ir);
  if (optimizeErrors) {
    return err(optimizeErrors);
  }

  EmitPass emit(options);
  Result<ObjectFile, Errors> object = emit.run(*ir);
  if (object.isErr()) {
    return err(object.error());
  }

  LinkPass link(options, features);
  Errors linkErrors = link.run(object.value(), program);
  if (linkErrors) {
    return err(linkErrors);
  }

  return ok(Build(options.output));
}
```

Что здесь видно:

- **Остановки.** После `load` — как сейчас: при ошибках разбора дальше не идём. Ошибки `declare`,
  `infer`, `check`, `instances` и `whole_program` собираются в одни `Errors` и проверяются вместе:
  сейчас это один `Sema::check`, и тесты `tests/errors` ждут их в одном прогоне (см. «Открытые
  вопросы»).
- **Круги карт событий.** Программа, чьи эмиттеры получают карту событий из `emit()`, проверяется
  кругами (сейчас — в `checkProgram` драйвера): `EventRounds` решает, нужен ли ещё круг, и ошибки
  круга, после которого будет следующий, выбрасываются.
- **Предупреждения.** `Errors` несёт и предупреждения (у `Diagnostic` есть вид); `if (errors)` —
  истина, только если есть ошибка. Предупреждения о переполнении печатаются и рядом с ошибками, как
  сейчас.
- **Проходы без ошибок** (`parallel`, `memory`, `lower`) отдают свой результат, а не `Errors`.

| Проход | Берёт | Меняет или отдаёт | Откуда сейчас |
|---|---|---|---|
| `LoadPass` | файл, пакеты | `Program`: модули, деревья | `load/loader/`, `load/lexer/`, `load/parser/`, `packages/` (подготовка пакетов) |
| `DeclarePass` | темы | `Program`: классы, функции, типы, импорты | начало `Sema::check`, `sema_decls.cpp`, `sema_modules.cpp` |
| `InferPass` | — | `Program`: циклы вызовов | `Sema::findCycles`, `sema_infer.cpp` |
| `CheckPass` | темы | `Program`: тип каждого выражения, чьи вызовы | тела в `Sema::check`, большая часть `sema*.cpp` |
| `InstancesPass` | — | `Program`: копии generics, поздние классы | `checkLateClasses`, `checkInstances`, `checkConstructorCalls` |
| `WholeProgramPass` | — | `Program`: порядок загрузки, неиспользуемое, номера классов | конец `Sema::check`, `sema_widen.cpp` |
| `OverflowPass` | — | предупреждения | `check/overflow/` |
| `ParallelPass` | — | `ParallelPlan` | `check/parallel/` |
| `MemoryPass` | — | `MemoryPlan`: группы, владение | анализ в `codegen_cycles.cpp` |
| `LowerPass` | темы, планы | `llvm::Module` | `lower/codegen/`, `library/*/codegen/` |
| `OptimizePass` | опции | рантайм в IR, оптимизация | `cli/driver/driver.cpp`: `linkRuntime`, `runPasses` |
| `EmitPass` | опции | объектный файл | `cli/driver/driver.cpp`: `emitObjectFile` |
| `LinkPass` | темы | exe | `link/linker/`, библиотеки тем |

## Ошибки и `Result`

```
compiler/result/
  result/        Result<T, E>: значение или ошибка; isOk, isErr, value, error
  ok/            Ok<T> и ok(value)
  err/           Err<E> и err(error)
compiler/program/
  diagnostic/    Diagnostic: место (llvm::SMLoc, как сейчас), вид (ошибка, предупреждение,
                 заметка), текст
  errors/        Errors: что нашёл проход; в if — есть ли ошибки; add
```

`Diagnostics` с записью и повтором (для кругов карт событий) уходит: круг просто выбрасывает свои
`Errors`. Печатает ошибки `cli`, а не проходы.

## Данные: `compiler/program/`

То, что идёт по конвейеру. Проходы общаются только через это.

```
compiler/program/
  program/          Program: модули, символы, типы и то, что нашли проходы
  module/           Module: файл программы, его импорты и экспорты (← ast.h)
  ast/              по папке на узел (~40 узлов, ← ast.h, 2299 строк):
    expr/ number_expr/ string_expr/ template_expr/ name_expr/ member_expr/ index_expr/
    call_expr/ new_expr/ binary_expr/ unary_expr/ assign_expr/ function_expr/ class_expr/
    object_expr/ array_expr/ await_expr/ yield_expr/ cast_expr/ ...
    stmt/ block_stmt/ var_decl_stmt/ if_stmt/ for_stmt/ for_of_stmt/ while_stmt/ switch_stmt/
    try_stmt/ return_stmt/ throw_stmt/ ...
    function_decl/ class_decl/ interface_decl/ type_alias_decl/ enum_decl/ import_decl/ ...
    pattern/ decorator/ type_ref/
  types/            по папке на вид типа (← sema/types.h, types.cpp):
    type/ type_context/ numeric_type/ union_type/ object_type/ class_type/ function_type/
    array_type/ dict_type/ promise_type/ iterator_type/ ...
  symbols/          var_symbol/ func_symbol/ class_symbol/ scope/
  findings/         что проходы оставляют следующим:
    generic_instances/  копии generics (общие для check и instances)
    unused_functions/   (← SourceFile::unusedFunctions)
    program_flags/      usesAsync, usesTls, topLevelAwait… (← поля SourceFile)
  diagnostic/ errors/
```

Что записывает проход, кроме типов: в вызов — его тему и операцию (`call.feature`, `call.op`)
вместо `call->builtin`.

## Проходы: `compiler/<проход>/`

У каждого прохода — класс прохода и его маленькие классы. Общее для классов одного прохода —
`Checker` у `check`, `Lowering` у `lower`: класс без логики, только ссылки на `Program`, `Errors`,
темы и на другие классы прохода (`checker.exprs().check(e)`). Состояние, которое сейчас лежит в
полях `Sema` (145) и `CodeGen` (173), расходится по классам, которым оно нужно.

### `load/` ← `load/loader/` (807), `load/lexer/` (850), `load/parser/` (3670)

```
load_pass/          LoadPass: вход и пакеты → модули Program
module_loader/      ModuleLoader: файлы программы по импортам, правила .tz
module_resolver/    ModuleResolver: путь импорта → файл, пакет или встроенный модуль
lexer/ token/       Lexer, Token
token_cursor/       TokenCursor: peek, accept, expect, synchronize, rewind (← Parser)
source_parser/      SourceParser: файл целиком
stmt_parser/        StmtParser: if, while, for, switch, return, throw, try, break
var_parser/         VarParser: let, const, деструктуризация
expr_parser/        ExprParser: от последовательности до первичных
template_parser/    TemplateParser: шаблонные строки
function_parser/    FunctionParser: функции, параметры, стрелки, async
class_parser/       ClassParser: классы, члены, декораторы
decl_parser/        DeclParser: type, interface, enum
module_parser/      ModuleParser: import, export
type_parser/        TypeParser: типы, кортежи, функции, отображённые типы, аргументы типов
pattern_parser/     PatternParser: образцы деструктуризации
template_copier/    TemplateCopier: новый разбор тела для копий generics (← parse*Copy)
```

### `declare/` ← начало `Sema::check`, `sema_decls.cpp` (705), `sema_modules.cpp` (808)

```
declare_pass/       DeclarePass
prelude/            Prelude: Error и его виды (← declarePrelude, declareErrorClass)
module_declarer/    ModuleDeclarer: функции и типы каждого файла (← declareModules)
import_binder/      ImportBinder: импорты к экспортам (← bindImports, useModule)
alias_declarer/ interface_declarer/ enum_declarer/ constant_folder/   (← sema_decls.cpp)
class_declarer/     ClassDeclarer: классы, их поля и методы, цепочки наследования
write_collector/    WriteCollector: какие поля где-то пишутся (← assignedFields_)
function_names/     FunctionNames: что упоминают функции (← collectNames, namesInFunctions_)
metadata_writers/   MetadataWriters: кто пишет метаданные (← findMetadataWriters)
```

Классы тем (`SystemError`, `Stats`, `ExecSyncError`, `Hash`…) объявляет крючок `declare` их темы,
а не `Prelude`.

### `infer/` ← `Sema::findCycles`, `sema_infer.cpp` (478)

```
infer_pass/         InferPass
call_cycles/        CallCycles: циклы вызовов функций без типа возврата (Тарьян)
return_inferrer/    ReturnInferrer: тип возврата по return; check зовёт его и сам, когда
                    вызов нужен раньше тела
```

### `check/` ← `sema.cpp` (5339) и большинство `sema_*.cpp`

```
check_pass/         CheckPass: верхний уровень файлов, тела функций, поднятые функции
checker/            Checker: ссылки на всё общее прохода
scopes/             Scopes, BlockNames: видимые имена (← pushScope, blockNames_)
expr/
  expr_checker/       ExprChecker: какому классу отдать выражение
  call_checker/       CallChecker: вызовы функций, методов, конструкторов; вызовы тем — темам
  member_checker/ index_checker/ name_checker/ literal_checker/ template_checker/
  binary_checker/ unary_checker/ assign_checker/ logical_checker/
  argument_checker/   ArgumentChecker: аргументы по параметрам (← sema_arglist.cpp)
  truth_checker/      TruthChecker: что может быть условием (← sema_truth.cpp)
stmt/
  stmt_checker/       StmtChecker: какому классу отдать оператор
  var_checker/ if_checker/ loop_checker/ switch_checker/ jump_checker/ label_checker/
  return_checker/ try_checker/
flow/
  flow/               Flow: что известно в этой точке (← sema_flow.cpp)
  facts/              Facts: факты для доказательств индексов (← facts.cpp)
  narrowing/          Narrowing: сужение объединений (← sema_union.cpp, часть)
  predicate_checker/  PredicateChecker: x is T, asserts, факты из функций (← sema_predicate.cpp)
types/
  type_resolver/      TypeResolver: TypeRef → Type
  type_relations/     TypeRelations: что во что входит, расширение (← sema_typeops.cpp)
  conditional_types/  ConditionalTypes: T extends X ? A : B, infer (← sema_conditional.cpp)
  destination/        Destination: тип литерала по тому, куда он идёт (← sema_destination.cpp)
  union_checker/      UnionChecker: поля и методы объединений (← sema_union.cpp)
  unknown_checker/    UnknownChecker: unknown и object (← sema_unknown.cpp)
values/
  number_methods/ string_methods/ array_methods/   методы значений (← sema_builtins.cpp)
objects/
  object_checker/     ObjectChecker: литералы, spread, Object.keys (← sema_object.cpp)
  dict_checker/       DictChecker: Record, Map, Set (← sema_dict.cpp)
functions/
  closure_checker/    ClosureChecker: замыкания, захват (← sema_closure.cpp)
  tail_calls/         TailCalls: где хвостовой вызов гарантирован
classes/
  class_checker/      ClassChecker: тела классов, поля, this (← sema_class.cpp)
  static_checker/     StaticChecker: статические члены
  overload_resolver/  OverloadResolver: какой вариант метода зовётся
  inherit_checker/    InheritChecker: extends, super, override, implements (← sema_inherit.cpp)
  class_value_checker/ ClassValueChecker: классы как значения, примеси (← sema_classvalue.cpp)
  decorator_checker/  DecoratorChecker: декораторы и их порядок (← sema_decorate.cpp)
generics/
  type_arg_inferrer/  TypeArgInferrer: аргументы типа из вызова (← sema_generic.cpp)
  generic_instantiator/ GenericInstantiator: копия функции под типы (← sema_generic.cpp)
  alias_instantiator/ AliasInstantiator: обобщённые псевдонимы
  generic_class_instantiator/ (← sema_generic_class.cpp)
errors/
  throw_checker/      ThrowChecker: throw, try, catch (← sema_error.cpp)
iterators/
  generator_checker/  GeneratorChecker: function*, yield (← sema_iter.cpp)
  iterable_checker/   IterableChecker: Iterable<T> (← sema_iterable.cpp)
async/
  async_checker/      AsyncChecker: async-функции, await, забытые Promise (← sema_async.cpp)
  promise_checker/    PromiseChecker: Promise.all и остальное статическое
  timer_checker/      TimerChecker: setTimeout, очереди
```

### `instances/`, `whole_program/`

```
instances/
  instances_pass/     InstancesPass
  late_classes/       LateClasses: классы из class-выражений (← checkLateClasses)
  constructor_calls/  ConstructorCalls (← checkConstructorCalls)
whole_program/
  whole_program_pass/ WholeProgramPass
  load_order/         LoadOrder: чтение до загрузки файла (← checkLoadOrder)
  unused/             Unused: неиспользуемые функции (← findUnused)
  wide_writes/        WideWrites: запись через более широкий тип (← sema_widen.cpp, 1347)
  untyped_globals/    UntypedGlobals (← reportUntyped)
  class_numbering/    ClassNumbering (← numberClasses)
  reflection_finisher/ ReflectionFinisher (← finishReflection)
```

### `overflow/` ← `overflow.cpp` (4099)

```
overflow_pass/      OverflowPass
value_range/        ValueRange: пределы значения
range_analysis/     RangeAnalysis: пределы по всей программе
call_ranges/        CallRanges: пределы через вызовы
loop_ranges/        LoopRanges: счётчики циклов
overflow_warnings/  OverflowWarnings: где предупредить и что предложить
```

### `parallel/` ← `parallel.cpp` (4674), `parallel.h`

```
parallel_pass/      ParallelPass
pieces/             Pieces: куски async-кода
access_analysis/    AccessAnalysis: что каждый кусок читает и пишет
conflicts/          Conflicts: какие куски не могут идти вместе
parallel_plan/      ParallelPlan: что идёт параллельно (← ParallelPlan)
explainer/          Explainer: --explain-parallel
```

### `memory/` ← анализ из `codegen_cycles.cpp` (3337)

```
memory_pass/        MemoryPass
cyclic_types/       CyclicTypes: какие типы образуют циклы
groups/             Groups: группы объектов в циклах
ownership/          Ownership: выход структуры из функции, владение
memory_plan/        MemoryPlan: что из этого нужно lower
```

### `lower/` ← `codegen.cpp` (3420) и остальные `codegen_*.cpp`

Подпапки — те же, что у `check/`: построение IR для части языка лежит там же, где её проверка.

```
lower_pass/         LowerPass
lowering/           Lowering: ссылки на всё общее прохода (← CodeGen.h, только состояние)
module_builder/     ModuleBuilder: порядок — объявления, глобальные, функции (← generate)
runtime_functions/  RuntimeFunctions: объявления функций рантайма (← declareRuntime)
layouts/            Layouts: раскладки объектов, дескрипторы типов
expr/ stmt/         по классу на вид выражения и оператора, как в check/
values/             number_lowerer/ string_lowerer/ array_lowerer/ (← codegen_array.cpp, …)
objects/            object_lowerer/ dict_lowerer/ (← codegen_object.cpp, codegen_dict.cpp)
functions/          closure_lowerer/ tail_call_lowerer/ (← codegen_closure.cpp, codegen_tail.cpp)
classes/            class_lowerer/ vtables/ itables/ decorator_lowerer/ inherit_lowerer/
types/              union_lowerer/ unknown_lowerer/
errors/             throw_lowerer/ may_throw/ stack_check/ (← codegen_throw.cpp, 1710)
iterators/          generator_lowerer/ async_generator_lowerer/
async/              state_machine/ promise_lowerer/ timer_lowerer/ (← codegen_async.cpp, 1779)
memory/             ref_counts/ group_lowerer/ (← codegen_cycles.cpp, IR)
```

### `optimize/`, `emit/`, `link/` ← `cli/driver/driver.cpp` (679), `link/linker/`, `link/licenses/`

```
optimize/
  optimize_pass/      OptimizePass: рантайм в IR и оптимизация
  runtime_linker/     RuntimeLinker: что копировать из tzrt.bc (TZ_HOT)
  target/             Target: машина под цель
emit/
  emit_pass/          EmitPass: объектный файл
link/
  link_pass/          LinkPass: lld, рантайм и библиотеки тем
  lld_linker/         LldLinker (← linker.cpp)
  linked_projects/    LinkedProjects: что вошло в exe, для tz licenses (← licenses.cpp)
```

## Темы библиотеки: `compiler/library/`, `runtime/library/`, `library/`

Тема — то, что программа берёт из глобального имени или модуля. У темы одно имя во всех трёх
деревьях; в каждом лежит её код своего вида.

```
compiler/library/
  feature/            Feature: крючки темы
  feature_registry/   FeatureRegistry: список тем; поиск по глобальному имени и модулю
  tz_modules/         TzModules: встроенные модули, написанные на tz, и их __tz_*
                      (← sema_library.cpp, codegen_library.cpp)
  <тема>/
    <тема>_feature/   <Тема>Feature: какие крючки и где они
    ...               классы проверки, IR, объявлений
runtime/library/
  <тема>/<модуль>/    её рантайм на C, по папке на модуль
library/
  <тема>/             её код на tz
```

### Крючки

У примера в `tmp/` их два — `checkCall` и `lowerCall`. Настоящим темам нужно больше; у каждого
есть пустая версия по умолчанию:

| Крючок | Проход | Что делает | Кому нужен |
|---|---|---|---|
| `globals`, `modules` | все | имена: `Math`, `node:crypto` | всем |
| `declare` | declare | свои классы и типы: `SystemError`, `Hash`, `Buffer` | fs, crypto, bytes, child_process, events, stream… |
| `checkCall`, `lowerCall` | check, lower | вызовы её функций: `Math.max`, `readFileSync` | всем |
| `checkMethod`, `lowerMethod` | check, lower | методы её типов: `buf.toString()`, `d.getTime()` | bytes, date, regex, decimal, json, result… |
| `checkProperty`, `lowerProperty` | check, lower | чтения: `Math.PI`, `process.argv` | math, process, crypto |
| `checkOperator`, `lowerOperator` | check, lower | операторы над её типом: `a + b` у `decimal` | decimal |
| `checkLiteral` | load, check | свои литералы: регулярки, точные `decimal` | regex, decimal |
| `afterCheck` | check | нужен ли ещё круг | events (карты из `emit`) |
| `narrow` | check | сужение после вызова: `setEncoding` | stream |
| `runtime` | link | модули на C, только если тема использована | всем с рантаймом |
| `link` | link | библиотеки: libcrypto, прямой или полный слой | crypto, tls |
| `lib` | load | файлы на tz | events, stream, http, crypto… |

Вместо `enum Builtin` на ~500 значений у каждой темы — свой список операций; в вызове лежат
`call.feature` и `call.op`.

### Список

Классы — в `compiler/library/<тема>/`, рантайм — модули в `runtime/library/<тема>/`, код на tz — в
`library/<тема>/`.

| Тема | Классы | Рантайм | tz | Откуда классы |
|---|---|---|---|---|
| `console` | `ConsoleFeature`, `ConsoleChecker`, `ConsoleLowerer` | `inspect` | | `sema.cpp`, `codegen_builtins.cpp` |
| `math` | `MathFeature`, `MathChecker`, `MathLowerer` | `math` | | `sema_builtins.cpp`, `codegen_builtins.cpp` |
| `json` | `JsonFeature`, `JsonChecker`, `JsonTypeChecker` (`JSON.parse<T>`), `JsonLowerer` | `json` (→ `parse`, `stringify`, `typed`) | | `sema_json.cpp`, `codegen_json.cpp` |
| `decimal` | `DecimalFeature`, `DecimalChecker`, `DecimalOperators`, `DecimalLiteral`, `DecimalLowerer` | `decimal` | | `sema_decimal.cpp`, `codegen_decimal.cpp`, `decimal_literal/` |
| `result` | `ResultFeature`, `ResultChecker`, `SafeTryChecker`, `ResultAsyncChecker`, `ResultLowerer` | | | `sema_result.cpp`, `codegen_result.cpp`, часть `sema_async.cpp` |
| `reflect` | `ReflectFeature`, `MetadataChecker`, `ReflectChecker`, `ReflectLowerer` | `reflect`, `meta` | | `sema_reflect.cpp`, `codegen_reflect.cpp` |
| `weakref` | `WeakRefFeature`, `WeakRefChecker` | | | `sema_weak.cpp` |
| `regex` | `RegexFeature`, `RegexLiteral`, `RegexChecker`, `RegexLowerer`, `regex_native/` (машинный код) | `regex`, `regex_compile` | | `sema_regex.cpp`, `codegen_regex.cpp`, `codegen_regex_native.cpp` (2310), `regex_syntax/` |
| `date` | `DateFeature`, `DateChecker`, `DateLowerer` | `date`, `date_parse`, `date_zone`, `date_format` | | `sema_date.cpp`, `codegen_date.cpp`, `date_syntax/` |
| `intl` | `IntlFeature`, `IntlChecker`, `IntlLowerer` | `intl`, `intl_number`, `intl_text`, `intl_names`, `icu` | | `sema_intl.cpp`, `codegen_intl.cpp` |
| `bytes` | `BytesFeature`, `TypedArrayChecker`, `BufferChecker`, `ByteProofs`, `BytesLowerer` | `bytes`, `buffer`, `encodings`, `cjk` | | `sema_bytes.cpp` (1970), `codegen_bytes.cpp` |
| `process` | `ProcessFeature`, `ProcessChecker`, `ProcessLowerer` | `process` | | `sema_process.cpp`, `codegen_process.cpp` |
| `fs` | `FsFeature`, `FsDeclarer` (`Stats`, `Dirent`, `SystemError`), `FsChecker`, `FsPromisesChecker`, `ReadlineChecker`, `FsLowerer` | `fs` (4390 → `fs_sync`, `fs_async`, `fs_errors`, `stdin`) | | `sema_node.cpp` (часть), `sema_async_io.cpp`, `codegen_node.cpp`, `codegen_async_io.cpp` |
| `path` | `PathFeature`, `PathChecker` | `path` | | `sema_node.cpp` (часть) |
| `os` | `OsFeature`, `OsChecker` | `os` | | `sema_node.cpp` (часть) |
| `child_process` | `ChildProcessFeature`, `ExecSyncErrorDeclarer`, `ChildChecker`, `ChildLowerer` | `child` | | `sema_child.cpp`, `codegen_child.cpp` |
| `events` | `EventsFeature`, `EventMapInference`, `EmitterChecker`, `EventTargetChecker`, `EventsLowerer` | `signal` | `events.tz`, `web.tz` | `sema_events.cpp` (1362), `codegen_events.cpp` |
| `stream` | `StreamFeature`, `StreamChecker`, `ChunkTypes`, `StreamLowerer` | `stream` | `stream.tz` (3989 → `readable`, `writable`, `duplex`, `pipeline`), `stream_consumers.tz`, `stream_promises.tz`, `string_decoder.tz`, `fs_stream.tz` | `sema_stream.cpp` (1453), `codegen_stream.cpp` |
| `net` | `NetFeature` | `net` | | — |
| `http` | `HttpFeature`, `FetchChecker`, `ServeChecker` | `http`, `decompress` | `http.tz`, `fetch.tz`, `url.tz` | части `sema_library.cpp` |
| `tls` | `TlsFeature`, `TlsLinker` | `tls` (с `tls_roots.h`) | | — |
| `crypto` | `CryptoFeature`, `CryptoDeclarer`, `HashChecker`, `KeyChecker`, `CipherChecker`, `X509Checker`, `WebCryptoChecker`, `CryptoLowerer`, `CryptoLinker` | по модулю на `crypto*.c` | `webcrypto.tz` | `sema_crypto.cpp` (1689), `sema_webcrypto.cpp`, `codegen_crypto.cpp`, `crypto_api/` |

Сейчас `fs`, `path` и `os` — одна тема `node` (см. «Сейчас»): их C++ — общий `sema_node.cpp`. На
шаге 5 она расходится на три, и модули `runtime/library/node/fs/`, `path/`, `os/` уходят в свои темы.

В ядре остаётся то, у чего свой синтаксис или что есть у любого значения: числа, строки, массивы
и их методы, объекты, `Record` / `Map` / `Set`, классы и декораторы, generics, исключения,
генераторы, async, `Promise` и таймеры.

## Рантайм: `runtime/core/` и `runtime/library/`

Рантайм на C — модули, а не классы; правило то же: модуль (`.c` и его `.h`) — в своей папке, таблицы
и их генераторы (`unicode_tables.h`, `gen_unicode_tables.mjs`) — в папке модуля, которому нужны.
Это уже сделано; осталось разрезать два больших файла:

```
runtime/core/
  tzrt/             tzrt.h — только общее: TZ_HOT, дескрипторы типов, раскладки
                    (← tzrt.h, 1784 строки → по заголовку на модуль)
  output/           вывод в stdout и stderr, граница стека, сбой (← output.c)
  alloc/ group/ publish/                       память
  string/ unicode/ number/ array/ object/ map/ closure/ unknown/   значения и таблицы к ним
  error/                                       исключение в полёте, стек
  async/ io/ parallel/                         цикл событий, IOCP, потоки
```

## Пакеты: `compiler/packages/`

Файлы уже по папкам (`project/`, `fetch/`, `net/`…); дальше — по классу на папку:

```
packages/
  manifest/        Manifest: tz.json (← project.cpp, 1111)
  lock/            Lock: tz.lock
  package_graph/   PackageGraph: подготовка пакетов к сборке (← preparePackages)
  semver/          Semver, VersionRange
  jsr_client/      JsrClient (← fetch.cpp)
  git_client/      GitClient (← fetch.cpp)
  http_client/     HttpClient: WinHTTP (← net.cpp)
  json_reader/     JsonReader (← packages/json/json.cpp)
  scanner/         Scanner: файлы пакета по импортам (← scan.cpp)
  port/
    porter/        Porter: tz port целиком (← port.cpp, 1388)
    rewriter/      Rewriter: автоматические замены
    port_notes/    PortNotes: что править руками
```

## Команды: `compiler/cli/` ← `cli/driver/driver.cpp`, `packages/commands/`

```
cli/
  main.cpp
  args/              Args: разбор командной строки (← parseArgs, usage)
  build_command/ run_command/ explain_command/ init_command/ add_command/
  remove_command/ update_command/ port_command/ licenses_command/ version_command/
  error_printer/     ErrorPrinter: печать Errors с местами
```

Команда — класс с `run(Args)`. `main` выбирает команду по первому слову.

## Сборка

Так уже собирается:

- [compiler/CMakeLists.txt](../compiler/CMakeLists.txt) собирает `tz.exe` из всех `.cpp` под
  `compiler/` по маске, без списка файлов; `#include` — от корня репозитория.
- [runtime/CMakeLists.txt](../runtime/CMakeLists.txt) — `tzrt.lib`, `tzrt_mt.lib`, `tzrt.bc` и
  `tzrt_mt.bc` из всех `.c` под `runtime/core/` и `runtime/library/`, кроме `crypto/` и `tls/`: те
  идут в `tzcrypto.lib`, по объекту на часть.
- [library/CMakeLists.txt](../library/CMakeLists.txt) копирует `library/*/*.tz` в `bin/lib` рядом
  друг с другом: загрузчик ищет их там по имени файла, как раньше.
- Тесты не меняются: `tests/run`, `tests/errors`, `tests/cli` проверяют поведение, а не устройство.

## Как переезжать

Шагами. После каждого шага все тесты проходят, и ты коммитишь.

0. **Три дерева** — сделано 25 октября. `src/` и корневой `runtime/` разложены по `compiler/`,
   `runtime/core/`, `runtime/library/` и `library/`, по папке на класс или модуль; логика та же.
1. **Основа** — сделано: `.clang-format` в корне (код отформатирован им), `Result`, `ok`, `err`,
   `Errors`, `Diagnostic`, `pipeline.cpp`.
2. **`cli` и `packages`** — сделано: по классу в папку (кроме `FilePort`, см. «Сейчас»).
3. **`load`** — сделано: загрузчик, лексер и парсер — классы из списка, текст литералов отдельно.
4. **Каркас тем** — сделано: `Feature`, `FeatureRegistry`; `call.feature` и `call.op` — нет, см.
   «Сейчас».
5. **Остальные темы** — сделано: части `Sema` и `CodeGen` ушли в классы тем, их ветки — в крючки;
   `node` разошёлся на `fs`, `path` и `os` во всех деревьях.
6. **`overflow`, `parallel`, `memory`** — сделано: классы; анализ памяти — свой проход
   `MemoryPass` с `MemoryPlan`.
7. **`Sema` → `declare`, `infer`, `check`, `instances`, `whole_program`** — сделано: пять
   проходов и классы по папкам; `Sema` стал `Checker`.
8. **`CodeGen` → `lower`** — сделано: классы по папкам; `CodeGen` стал `Lowering`.
9. **Уборка** — сделано: `sema.h` и `codegen.h` ушли, пустых папок нет, `tzrt.h` и большие файлы
   рантайма разрезаны; `enum Builtin` остался (см. «Сейчас»).

## Открытые вопросы

1. **Комментарии.** В `tmp/` их нет. В настоящем коде комментарии объясняют, почему так: как
   ведёт себя JavaScript или node, ссылки на разделы `docs/`. Убрать их везде, оставить только
   такие «почему» или оставить как есть?
2. **Остановка после каждого прохода.** В `tmp/` ошибка любого прохода останавливает следующий. Здесь
   ошибки `declare` … `whole_program` собраны вместе, потому что сейчас их показывает один прогон
   и на это рассчитаны тесты `tests/errors`. Останавливаться после каждого — тогда тесты, где в
   одном файле ошибки объявлений и тел, придётся разделить.
3. **Тема из одного класса** — `compiler/library/weakref/weak_ref_feature/` или прямо в
   `compiler/library/weakref/`?
