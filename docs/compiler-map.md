# Карта компилятора

Каждая папка `compiler/` — строка: что в ней. Как код устроен и почему — [structure.md](structure.md);
здесь только где что лежит. Папка класса названа как класс (`ExprChecker` — `expr_checker/`),
поэтому строка начинается с имени класса.

**Правило:** новая папка сразу получает строку здесь, убранная — теряет. Тест `docs/compiler_map`
(`tests/compiler_map.cmake`) падает, если папки нет в карте или карта называет папку, которой нет.
Вложенность — два пробела на уровень; имя папки кончается на `/`.

Порядок групп — по конвейеру: данные, проходы от загрузки до компоновки, темы библиотеки,
пакеты, команды, редактор и ИИ.

```
pipeline/         конвейер проходов
  build_options/  BuildOptions: вход, выход, пакеты, текст редактора, уровень проверки
  pass_times/     PassTimes: время каждого прохода для лога
  pipeline/       checkProgram и buildProgram
  runtime_files/  RuntimeFiles: где лежат tzrt.lib, tzrt.bc и lib/
```

```
program/                  что идёт по конвейеру: модули, дерево, типы, ошибки
  ast/                    дерево разбора: по папке на узел; ast.h — все разом
    access/               public / protected / private
    array_lit_expr/       узел [a, b]
    assign_expr/          узел a = b, a += b
    ast_context/          AstContext: хранилище узлов, переменных, функций, модулей
    await_expr/           узел await
    binary_expr/          узел a + b
    block_stmt/           узел { … }
    bool_lit_expr/        узел true / false
    break_stmt/           узел break
    builtin/              enum Builtin — встроенные вызовы (уйдёт, см. docs/structure.md «Сейчас»)
    call_expr/            узел f(…)
    cast_expr/            узел приведения (явного и неявного)
    class_decl/           узел class
    class_expr/           узел class как выражение
    class_field/          поле класса
    conditional_expr/     узел a ? b : c
    continue_stmt/        узел continue
    debugger_stmt/        узел debugger
    declaration_file/     разобранный .d.tz
    decorator/            декоратор
    do_while_stmt/        узел do … while
    empty_stmt/           узел ;
    enum_decl/            узел enum
    enum_member/          член enum
    export_decl/          export
    expr/                 Expr: основа узлов-выражений, тип
    expr_stmt/            узел выражение-инструкция
    float_lit_expr/       узел дробного литерала
    for_of_stmt/          узел for … of
    for_stmt/             узел for (;;)
    function_decl/        узел функции (объявление, стрелка, метод)
    function_expr/        узел функции-выражения
    if_stmt/              узел if
    import_decl/          import
    index_expr/           узел a[i]
    int_lit_expr/         узел целого литерала
    labeled_stmt/         узел метки
    member_expr/          узел a.b
    module_name/          имя в import / export
    name_expr/            узел имени
    nested_group/         группа вложенных функций
    new_expr/             узел new
    node/                 Node: основа узлов
    object_lit_expr/      узел { a: 1 }
    operators/            операторы
    optional_chain_expr/  узел a?.b
    param/                параметр функции
    regex_lit_expr/       узел /…/
    return_stmt/          узел return
    sequence_expr/        узел a, b
    stmt/                 Stmt: основа узлов-инструкций
    string_lit_expr/      узел строки
    switch_case/          case в switch
    switch_stmt/          узел switch
    template_expr/        узел шаблонной строки
    type_alias_decl/      узел type
    type_param_decl/      параметр типа <T>
    type_ref/             запись типа в коде
    unary_expr/           узел !a, -a
    undefined_expr/       узел undefined
    update_expr/          узел ++, --
    var_decl_stmt/        узел let / const
    var_declarator/       одно имя в let / const
    while_stmt/           узел while
    yield_expr/           узел yield
  diagnostic/             Diagnostic: место, вид, текст
  diagnostics/            Diagnostics: сбор ошибок при разборе и проверке
  errors/                 Errors: что нашёл проход
  module/                 Module: файл программы или встроенный модуль; bufferFiles — файлы буферов модуля из дерева library/
  program/                Program: модули и всё, что нашли проходы
  source_file/            SourceFile: разобранный файл
  source_places/          SourcePlaces: место в коде → файл, строка, столбец в UTF-16
  symbols/                символы
    func_symbol/          FuncSymbol: функция
    var_symbol/           VarSymbol: переменная
  types/                  типы: класс Type и функции над типами
    argument_lists/       списки аргументов вариативных обобщений
    field/                Field: поле типа-объекта
    intl_kind/            виды объектов Intl
    overload/             Overload: вариант перегрузки
    template_patterns/    шаблоны строковых типов
    type/                 Type: все виды типов и их фабрики
    type_op/              операции над типами (keyof и др.)
    type_queries/         вопросы к типу (держит ли объекты и т. п.)
    type_spelling/        текст типа для сообщений
    union_slot/           место варианта в объединении
    union_slots/          раскладка вариантов объединения
```

```
result/    Result как в neverthrow
  err/     Err и err()
  ok/      Ok и ok()
  result/  Result<T, E>
```

```
load/                    проход LoadPass: файлы, лексер, разбор
  call_parser/           CallParser: вызовы, члены, new
  class_parser/          ClassParser: классы, члены, декораторы
  decl_parser/           DeclParser: type, interface, enum
  declaration_parser/    DeclarationParser: файлы .d.tz
  expr_parser/           ExprParser: выражения от последовательности до унарных
  function_parser/       FunctionParser: функции, параметры
  function_type_parser/  FunctionTypeParser: типы функций
  json_module/           импорт JSON: текст модуля из файла .json
  lexer/                 Lexer: текст → токены
  library_files/         LibraryFiles: файл из library/ исходников tz → его встроенный модуль (D8)
  library_modules/       LibraryModules: встроенные модули на tz, неявные импорты глобальных имён
  literal_text/          текст строковых литералов, escape-последовательности
  load_pass/             LoadPass
  loader/                Loader: модули программы по импортам
  module_parser/         ModuleParser: import, export
  module_resolver/       ModuleResolver: путь импорта → файл, пакет или встроенный модуль
  object_type_parser/    ObjectTypeParser: типы объектов, кортежей, mapped
  parser/                Parser: файл целиком; копии функций для обобщений
  pattern_parser/        PatternParser: деструктуризация
  primary_parser/        PrimaryParser: литералы, скобки, имена
  source_overlay/        SourceOverlay: текст из редактора вместо файла на диске
  stmt_parser/           StmtParser: инструкции
  template_parser/       TemplateParser: шаблонные строки
  to_utf8/               перевод текста файла в UTF-8
  token/                 Token
  token_cursor/          TokenCursor: peek, accept, expect, восстановление после ошибки
  type_parser/           TypeParser: типы
  var_parser/            VarParser: let, const
```

```
declare/            проход DeclarePass: классы, функции, типы, импорты
  class_declarer/   ClassDeclarer: классы — поля, методы, перегрузки
  constant_folder/  ConstantFolder: значения констант при сборке, переполнение в них
  declare_pass/     DeclarePass: объявляет всё, что есть в модулях
  function_names/   имена функций библиотеки и встроенных вызовов
  import_binder/    ImportBinder: связывает импорты с экспортами
  module_declarer/  ModuleDeclarer: модули, их экспорты, поиск имени
  prelude/          Prelude: встроенные типы (FetchError, Result…) — строка kPrelude
  type_declarer/    TypeDeclarer: type, interface, enum
```

```
infer/              проход InferPass: типы возврата без подписи
  call_cycles/      CallCycles: кольца взаимных вызовов
  infer_pass/       InferPass: выводит типы возврата
  return_inferrer/  ReturnInferrer: тип возврата функции по её return
```

```
check/                           проход CheckPass: тип каждого выражения, чьи вызовы, ошибки в телах
  async/                         async, await, Promise, таймеры
    async_checker/               AsyncChecker: await, for await, тип возврата async-функций, забытый Promise
    promise_checker/             PromiseChecker: new Promise, then / catch / finally
    promise_static_checker/      PromiseStaticChecker: Promise.all / allSettled / any / race / resolve
    timer_checker/               TimerChecker: setTimeout, setInterval, node:timers/promises, scheduler, nextTick
  check_pass/                    CheckPass: проверяет тела всех модулей по порядку загрузки
  checker/                       Checker: общее состояние прохода и ссылки на все его классы (хаб)
  classes/                       классы: члены, наследование, перегрузки, декораторы
    class_bases/                 ClassBases: базовые классы и интерфейсы, поля от них
    class_check_helpers/         общие помощники классов проверки классов
    class_checker/               ClassChecker: тела классов, this до super(), неназначенные поля
    class_links/                 ClassLinks: номера классов, таблицы методов, переопределения
    class_member_checker/        ClassMemberChecker: доступ к полю и методу, private / protected
    class_value_checker/         ClassValueChecker: класс как значение, new по значению класса
    decorator_checker/           DecoratorChecker: декораторы классов, методов и полей
    inherit_checker/             InheritChecker: abstract, implements, super(…) и super.method()
    instance_of_checker/         InstanceOfChecker: instanceof и сужение по нему
    method_call_checker/         MethodCallChecker: вызов метода класса, new класса
    method_literal_checker/      MethodLiteralChecker: объект-литерал с методами там, где ждут интерфейс
    overload_resolver/           OverloadResolver: выбор перегрузки метода и конструктора
    static_checker/              StaticChecker: статические поля и методы
  declarations/                  файлы объявлений .d.tz
    declaration_checker/         DeclarationChecker: сверяет .d.tz с кодом модуля
  event_rounds/                  EventRounds: круги проверки, пока карты событий не перестанут меняться
  expr/                          выражения
    argument_checker/            ArgumentChecker: число и типы аргументов вызова
    assign_checker/              AssignChecker: =, +=, ??=, ++ и --
    binary_checker/              BinaryChecker: арифметика, сравнения, ==, ??, битовые операции
    call_checker/                CallChecker: вызов функции и значения-функции
    concat_checker/              ConcatChecker: сложение со строкой, ошибка number + string
    expr_checker/                ExprChecker: вход проверки выражения, ?., ?:, приведения
    literal_checker/             LiteralChecker: целые литералы, шаблонные строки
    member_checker/              MemberChecker: obj.x и a[i]
    name_checker/                NameChecker: имя в выражении, чтение до объявления
    new_checker/                 NewChecker: new
    target_checker/              TargetChecker: левая часть присваивания
    truth_checker/               TruthChecker: условия if / while, &&, ||, правило истинности
    unary_checker/               UnaryChecker: !, -, typeof, delete, in
  facts/                         Facts: линейные факты о целых (a < b + 1) для доказательств индексов
  flow/                          поток управления: сужение, факты, доказательства
    condition_facts/             ConditionFacts: факты из условий if
    field_facts/                 FieldFacts: сужение полей (obj.x, r.error) по путям
    flow_checker/                FlowChecker: слияние веток, что сбивают присваивания и вызовы
    index_proofs/                IndexProofs: доказательство, что индекс в границах массива
    loop_bounds/                 LoopBounds: границы переменной цикла из условия
    loop_scanner/                LoopScanner: что меняется в теле цикла
    predicate_checker/           PredicateChecker: функции-проверки (x is T) и факты из них
    union_narrowing/             UnionNarrowing: сужение объединений по kind, typeof, != undefined
  functions/                     функции и замыкания
    callback_checker/            CallbackChecker: функция-аргумент против ожидаемого типа
    closure_checker/             ClosureChecker: стрелки и function-выражения, их тела
    function_checker/            FunctionChecker: объявления функций и их тела
    function_method_checker/     FunctionMethodChecker: call / apply / bind
    function_value_checker/      FunctionValueChecker: функция библиотеки как значение, async-колбэки
    nested_functions/            NestedFunctions: вложенные функции, подъём, взаимные вызовы
  generics/                      обобщения: параметры, вывод, копии
    alias_instantiator/          AliasInstantiator: обобщённые type-алиасы
    arg_list_checker/            ArgListChecker: вариативные параметры (...args: T)
    generic_check_helpers/       общие помощники проверки обобщений
    generic_checker/             GenericChecker: видимый тип внутри копии, мёртвые ветки копии
    generic_class_checker/       GenericClassChecker: обобщённые классы и new с аргументами типа
    generic_class_instantiator/  GenericClassInstantiator: копия обобщённого класса
    generic_widening/          GenericWidening: класс с более широким аргументом типа (ковариантный параметр)
    generic_instantiator/        GenericInstantiator: копия обобщённой функции на представление
    inference/                   Inference: сопоставление типа параметра с типом аргумента
    type_arg_inferrer/           TypeArgInferrer: вывод аргументов типа вызова
    type_params/                 TypeParams: объявление, подстановка, ограничения extends
  iterators/                     итераторы и генераторы
    generator_checker/           GeneratorChecker: function*, yield, тип генератора
    iterable_checker/            IterableChecker: Symbol.iterator, тип элемента for…of
    iterator_methods/            IteratorMethods: map / filter / take… у итераторов, Array.from
  objects/                       объекты, словари, кортежи
    dict_check_helpers/          общие помощники проверки словарей
    dict_checker/                DictChecker: Map и Record — ключи, члены, индекс
    dict_methods/                DictMethods: методы Map, Object.fromEntries, groupBy
    dict_new_checker/            DictNewChecker: new Map(пары)
    keyed_field_checker/         KeyedFieldChecker: obj[ключ] с ключом-литералом или const
    object_check_helpers/        общие помощники проверки объектов
    object_checker/              ObjectChecker: Object.*, for…in, поле и вызов поля
    object_literal_checker/      ObjectLiteralChecker: объект-литерал, ...spread
    record_literal_checker/      RecordLiteralChecker: литерал там, где ждут Record
    tuple_checker/               TupleChecker: кортежи — литерал, элемент, методы
  scopes/                        Scopes: области видимости имён
  shared_helpers/                общие помощники всех классов проверки
  stmt/                          инструкции
    jump_checker/                JumpChecker: break, continue, метки
    loop_checker/                LoopChecker: while, do, for, for…of
    return_checker/              ReturnChecker: return, все ли пути возвращают
    stmt_checker/                StmtChecker: вход проверки инструкции, if, блок
    switch_checker/              SwitchChecker: switch, полнота по объединению
    var_checker/                 VarChecker: let / const, нужен ли тип, непрочитанный Result
  top_level_checker/             TopLevelChecker: код верхнего уровня, порядок загрузки модулей
  types/                         типы: разрешение, отношения, вычисляемые типы
    conditional_check_helpers/   общие помощники условных типов
    conditional_types/           ConditionalTypes: T extends U ? A : B, infer
    destination/                 Destination: тип, которого ждёт место, для литералов
    destination_check_helpers/   общие помощники Destination
    destination_scanner/         DestinationScanner: заранее ищет, куда идут литералы
    extends_relation/            ExtendsRelation: отношение extends у типов
    literal_types/               LiteralTypes: литеральные типы, const-литералы
    template_types/              TemplateTypes: шаблонные типы строк, Uppercase…
    type_operators/              TypeOperators: keyof, typeof, индексные типы
    type_relations/              TypeRelations: приводимость, преобразование, общий тип
    type_resolver/               TypeResolver: запись типа в коде → Type
    typeops_check_helpers/       общие помощники операторов типов
    union_check_helpers/         общие помощники объединений
    union_checker/               UnionChecker: член, метод и запись у объединения
    unknown_checker/             UnknownChecker: unknown — что с ним можно
    utility_types/               UtilityTypes: Partial, ReturnType, Parameters…
  values/                        значения встроенных типов
    array_literal_checker/       ArrayLiteralChecker: литерал массива
    array_methods/               ArrayMethods: методы массивов
    global_function_checker/     GlobalFunctionChecker: parseInt, encodeURI и др. глобальные функции
    number_methods/              NumberMethods: методы чисел и Number.*, преобразования
    string_methods/              StringMethods: методы строк
```

```
instances/            проход InstancesPass: копии обобщений, поздние классы
  constructor_calls/  ConstructorCalls: проверка вызовов конструкторов после копий
  instances_pass/     InstancesPass
  late_classes/       LateClasses: классы-выражения и вложенные классы
```

```
whole_program/         проход WholeProgramPass: по всей программе сразу
  flow_builtins/       FlowBuiltins: потоки значений через встроенные вызовы
  flow_calls/          FlowCalls: потоки значений через вызовы
  flow_exprs/          FlowExprs: потоки значений в выражениях
  flow_stmts/          FlowStmts: потоки значений в инструкциях
  flows/               Flows: граф, куда идут значения (для записи через широкий тип)
  load_order/          LoadOrder: чтение до загрузки модуля
  unused/              Unused: неиспользуемые функции
  whole_program_pass/  WholeProgramPass
  wide_writes/         WideWrites: запись чужого значения через более широкий тип
```

```
overflow/             проход OverflowPass: пределы целых по всей программе
  assign_evaluator/   AssignEvaluator: пределы при присваивании
  builtin_evaluator/  BuiltinEvaluator: пределы результатов Math, Date и др.
  call_ranges/        CallRanges: пределы параметров по всем вызовам
  evaluator/          Evaluator: пределы выражения
  executor/           Executor: проход по инструкциям
  loop_ranges/        LoopRanges: пределы в циклах до неподвижной точки
  narrowing/          Narrowing: сужение пределов условиями
  overflow_check/     OverflowCheck: весь анализ по шагам, бюджет
  overflow_helpers/   общие помощники анализа
  overflow_pass/      OverflowPass
  overflow_warnings/  OverflowWarnings: ошибки «may overflow» и почему
  program_scan/       ProgramScan: функции, классы и переменные программы
  slots/              Slots: пределы переменных, полей, элементов
  value_range/        Val, Bound: предел значения и откуда он
```

```
parallel/             проход ParallelPass: что можно выполнять параллельно
  alias_analysis/     AliasAnalysis: могут ли две ссылки быть одним объектом
  analysis/           Analysis: общее состояние анализа
  builtin_effects/    BuiltinEffects: что читают и пишут встроенные вызовы
  call_resolver/      CallResolver: какие функции может вызвать вызов
  call_walk/          CallWalk: обход вызовов
  code_walk/          CodeWalk: обход инструкций
  conflicts/          Conflicts: конфликтуют ли два куска
  escape_scan/        EscapeScan: уходит ли объект из функции
  explain_names/      ExplainNames: имена и места для --explain-parallel
  explain_titles/     ExplainTitles: заголовки кусков для --explain-parallel
  explainer/          Explainer: печать плана параллельности
  expr_walk/          ExprWalk: обход выражений
  library_effects/    LibraryEffects: эффекты вызовов библиотеки на tz
  ownership/          Ownership: объекты, которые принадлежат одному вызову
  parallel_analysis/  ParallelPlan: что нашёл анализ, для LowerPass
  parallel_helpers/   общие помощники анализа
  parallel_model/     модель анализа: ссылки, доступы, куски, корни
  parallel_pass/      ParallelPass
  pieces/             Pieces: куски кода и где они начинаются
```

```
memory/             проход MemoryPass: группы и владение (без сборщика циклов)
  cycle_escapes/    CycleEscapes: уходит ли значение из места создания
  cycle_exprs/      CycleExprs: связи объектов в выражениях
  cycle_finder/     CycleFinder: граф типов, кольца ссылок
  cycle_scanner/    CycleScanner: строит граф для CycleFinder
  held_members/     HeldMembers: члены, которые держат объекты группы
  memory_analysis/  MemoryAnalysis: весь анализ памяти по шагам
  memory_pass/      MemoryPass
  memory_plan/      MemoryPlan: что нашёл анализ, для LowerPass
  owner_scanner/    OwnerScanner: кто владеет объектом
  store_calls/      StoreCalls: сохраняет ли вызов аргумент
  store_exprs/      StoreExprs: сохранение значений в выражениях
  store_scanner/    StoreScanner: какие значения нигде не хранятся
  store_stmts/      StoreStmts: сохранение значений в инструкциях
```

```
lower/                         проход LowerPass: программа → LLVM IR
  async/                       async и Promise в IR
    async_builtin_lowerer/     AsyncBuiltinLowerer: встроенные вызовы с ожиданием (таймеры и др.)
    async_lowerer/             AsyncLowerer: async-функции как сопрограммы, async main
    promise_combine_lowerer/   PromiseCombineLowerer: Promise.all / race / any / allSettled
    promise_lowerer/           PromiseLowerer: раскладка Promise, создание, выполнение
    reaction_lowerer/          ReactionLowerer: then / catch / finally, реакции на Promise
  classes/                     классы в IR
    class_descriptors/         ClassDescriptors: описания классов и таблицы методов
    class_lowerer/             ClassLowerer: new, вызов метода, instanceof
    decorator_lowerer/         DecoratorLowerer: вызовы декораторов, метаданные
    inherit_lowerer/           InheritLowerer: выбор метода по классу, таблицы интерфейсов
  expr/                        выражения в IR
    assign_lowerer/            AssignLowerer: присваивания, ??=, ++
    binary_lowerer/            BinaryLowerer: арифметика и сравнения
    call_lowerer/              CallLowerer: вызовы
    equality_lowerer/          EqualityLowerer: == и ?? для всех видов значений
    expr_lowerer/              ExprLowerer: вход для выражения, ?., ?:, преобразования
    member_lowerer/            MemberLowerer: obj.x
    truth_lowerer/             TruthLowerer: &&, ||, истинность значения
    unary_lowerer/             UnaryLowerer: !, -, typeof, имя члена enum
  function_builder/            FunctionBuilder: функция LLVM — объявление, параметры, тело
  function_frame/              FunctionFrame: блоки, области, переходы внутри функции
  functions/                   функции и замыкания в IR
    closure_lowerer/           ClosureLowerer: значение-функция, код замыкания
    env_lowerer/               EnvLowerer: окружения замыканий, коробки переменных
    tail_call_lowerer/         TailCallLowerer: хвостовые вызовы как переходы
  iterators/                   итераторы и генераторы в IR
    async_generator_lowerer/   AsyncGeneratorLowerer: async function*
    coroutine_lowerer/         CoroutineLowerer: сохранение и возврат контекста сопрограммы
    generator_lowerer/         GeneratorLowerer: function* как объект-итератор
    inline_generator_lowerer/  InlineGeneratorLowerer: генератор, встроенный в for…of
    iter_lower_helpers/        общие помощники итераторов
    iterator_lowerer/          IteratorLowerer: for…of по итератору, spread, методы итераторов
    lazy_chain_lowerer/        LazyChainLowerer: цепочки map / filter без промежуточных массивов
  layouts/                     раскладки в памяти
    object_layouts/            ObjectLayouts: раскладка объекта, поля, освобождение
    type_descriptors/          TypeDescriptors: дескрипторы типов для рантайма
  lower_pass/                  LowerPass
  lowered/                     Lowered: результат LowerPass
  lowering/                    Lowering: общее состояние прохода и ссылки на его классы (хаб)
  memory/                      память в IR: счётчики, группы, публикация
    group_lowerer/             GroupLowerer: объекты в группах (кольца ссылок без сборщика)
    memory_debug/              MemoryDebug: проверки владения в отладочной сборке
    publish_lowerer/           PublishLowerer: публикация значений другим потокам
    ref_counts/                RefCounts: retain и release
    store_lowerer/             StoreLowerer: запись ссылки в поле, переменную, массив
  module_builder/              ModuleBuilder: модуль LLVM — глобальные, main
  objects/                     объекты и словари в IR
    dict_lowerer/              DictLowerer: раскладка Map и Record, поиск, вставка
    dict_method_lowerer/       DictMethodLowerer: методы Map, new Map, groupBy
    object_lowerer/            ObjectLowerer: объект-литерал, spread, кортежи, Object.*
    object_maker/              ObjectMaker: собирает объекты ошибок рантайма (поля-литералы kind)
  places/                      места в коде для рантайма
    place_lowerer/             PlaceLowerer: файл и строка кадра для трассировки
  runtime_functions/           RuntimeFunctions: объявления функций рантайма tz_*
  stmt/                        инструкции в IR
    exit_lowerer/              ExitLowerer: return и выход через finally-подобные места
    loop_lowerer/              LoopLowerer: циклы, break, continue
    stmt_lowerer/              StmtLowerer: вход для инструкции, let, if, return
    switch_lowerer/            SwitchLowerer: switch
  types/                       объединения, необязательные, unknown в IR
    optional_lowerer/          OptionalLowerer: T | undefined
    union_layout/              UnionLayout: раскладка объединений, атомы строк-литералов
    union_lowerer/             UnionLowerer: преобразования, ==, typeof у объединений
    union_member_lowerer/      UnionMemberLowerer: поле и метод у объединения
    unknown_cast_lowerer/      UnknownCastLowerer: проверка unknown на тип
    unknown_lowerer/           UnknownLowerer: упаковка в unknown, typeof, Array.isArray
  values/                      значения встроенных типов в IR
    array_lowerer/             ArrayLowerer: массивы — элементы, длина, push
    array_method_lowerer/      ArrayMethodLowerer: методы массивов
    callback_method_lowerer/   CallbackMethodLowerer: map / filter / sort / reduce с колбэком
    number_lowerer/            NumberLowerer: числа, проверенные преобразования
    string_lowerer/            StringLowerer: строки, сложение строк
    string_method_lowerer/     StringMethodLowerer: методы строк
```

```
optimize/          проход OptimizePass: рантайм в IR, оптимизация LLVM
  optimize_pass/   OptimizePass
  runtime_linker/  RuntimeLinker: подшивает tzrt.bc (TZ_HOT) к программе
  target/          Target: машина и тройка для LLVM
```

```
emit/           проход EmitPass: объектный файл
  emit_pass/    EmitPass: IR → .obj или .ll
  file_writer/  FileWriter: запись файла
  ir_writer/    IrWriter: --emit-llvm
  object_file/  ObjectFile: результат EmitPass
```

```
link/             проход LinkPass: exe
  licenses/       Embedded: что из чужого кода попало в exe
  licenses_pass/  LicensesPass: лицензии для tz licenses
  link_pass/      LinkPass: зовёт компоновщик
  linker/         вызов lld-link
```

```
library/                       C++-часть тем библиотеки: проверка и IR; у темы — <тема>_feature/
  builtin_modules/             встроенные модули node:* и tz:io — имена и импорты
    builtin_module_checker/    BuiltinModuleChecker: импорт встроенного модуля, его имена
    builtin_module_types/      BuiltinModuleTypes: типы встроенных модулей (ошибки fs, child_process), видны по импорту из модуля
    builtin_modules_feature/   BuiltinModulesFeature: тема
    node_check_helpers/        общие помощники модулей node:*
    runtime_object_checker/    RuntimeObjectChecker: объекты рантайма (Stats и др.) — члены
    timers_exports/            что экспортирует node:timers
  bytes/                       Buffer, Uint8Array, DataView, TextEncoder
    buffer_calls/              BufferCalls: IR вызовов Buffer
    buffer_checker/            BufferChecker: методы Buffer
    buffer_exports/            что экспортирует node:buffer
    buffer_static_checker/     BufferStaticChecker: Buffer.from / alloc / concat, embedBytes
    byte_proofs/               ByteProofs: доказательство, что запись байта в границах
    bytes_call_lowerer/        BytesCallLowerer: вход IR вызовов байтов
    bytes_call_site/           BytesCallSite: что за вызов байтов (функция, получатель, кодировка)
    bytes_callback_lowerer/    BytesCallbackLowerer: методы байтов с колбэком
    bytes_check_helpers/       общие помощники проверки байтов
    bytes_checker/             BytesChecker: индекс и запись элемента, кодировки
    bytes_feature/             BytesFeature: тема
    bytes_lower_helpers/       общие помощники IR байтов
    bytes_lowerer/             BytesLowerer: элемент, длина, члены байтов
    bytes_make_calls/          BytesMakeCalls: IR создания байтов
    bytes_new_checker/         BytesNewChecker: new Uint8Array, new TextDecoder
    number_access_calls/       NumberAccessCalls: IR readInt32LE и т. п.
    text_codec_calls/          TextCodecCalls: IR TextEncoder / TextDecoder
    typed_array_calls/         TypedArrayCalls: IR методов typed array
    typed_array_checker/       TypedArrayChecker: методы typed array и DataView
  child_process/               node:child_process
    child_checker/             ChildChecker: exec, execFile, spawn — аргументы и типы
    child_error_types/         ChildErrorTypes: 10 типов ошибок и 2 результата spawnSync, тип по вызову
    child_exports/             что экспортирует node:child_process
    child_lowerer/             ChildLowerer: IR вызовов child_process
    child_process_feature/     ChildProcessFeature: тема
  console/                     console
    console_checker/           ConsoleChecker: console.log и др.
    console_feature/           ConsoleFeature: тема
    console_lowerer/           ConsoleLowerer: IR console.log
  crypto/                      node:crypto и crypto.subtle
    crypto_api/                таблица функций и алгоритмов crypto
    crypto_args/               CryptoArgs: аргументы и результаты вызовов OpenSSL
    crypto_check_helpers/      общие помощники проверки crypto
    crypto_checker/            CryptoChecker: члены и new у классов crypto, X509
    crypto_error_types/        CryptoErrorTypes: 32 типа ошибок node:crypto, тип по вызову
    crypto_feature/            CryptoFeature: тема
    crypto_function_checker/   CryptoFunctionChecker: функции node:crypto
    crypto_lowerer/            CryptoLowerer: IR вызовов crypto
    crypto_method_checker/     CryptoMethodChecker: методы Hash, Hmac, Cipher и др.
    crypto_names/              имена хешей и шифров
    key_pair_checker/          KeyPairChecker: generateKeyPair
    subtle_algorithms/         алгоритмы crypto.subtle
    subtle_call_checker/       SubtleCallChecker: вызовы crypto.subtle
    web_crypto_checker/        WebCryptoChecker: мост tz:webcrypto к рантайму
  date/                        Date и Intl.DateTimeFormat
    date_checker/              DateChecker: new Date, Date.*, методы
    date_feature/              DateFeature: тема
    date_format_checker/       DateFormatChecker: toLocale… и Intl.DateTimeFormat
    date_lowerer/              DateLowerer: IR Date
    date_syntax/               разбор строк дат при сборке
  decimal/                     decimal
    decimal_checker/           DecimalChecker: литералы и операции decimal
    decimal_feature/           DecimalFeature: тема
    decimal_literal/           DecimalLiteral: точное значение литерала
    decimal_lowerer/           DecimalLowerer: IR decimal
  events/                      node:events и EventTarget
    emitter_checker/           EmitterChecker: on / emit у EventEmitter, правило «слушай error»
    event_map_inference/       EventMapInference: карта событий из вызовов emit
    event_maps/                EventMaps: карта событий класса, тип слушателя
    event_target_checker/      EventTargetChecker: EventTarget, once / on из node:events
    events_feature/            EventsFeature: тема
  feature/                     Feature: крючки темы для проходов
  feature_registry/            FeatureRegistry: все темы
  fs/                          node:fs, fs/promises, readline, tz:io
    fs_checker/                FsChecker: вызовы fs и tz:io
    fs_declarer/               FsDeclarer: классы fs (Stats, Dirent…)
    fs_error_types/            FsErrorTypes: 23 типа ошибок fs (ReadFileError…), тип по вызову, глагол для текста
    fs_exports/                что экспортирует node:fs
    fs_feature/                FsFeature: тема
    fs_lowerer/                FsLowerer: IR вызовов fs, объекты ошибок
    fs_promises_checker/       FsPromisesChecker: fs/promises
    fs_promises_lowerer/       FsPromisesLowerer: IR fs/promises и readline
    readline_checker/          ReadlineChecker: readline
  intl/                        Intl
    intl_call_lowerer/         IntlCallLowerer: IR вызовов Intl
    intl_check_helpers/        опции и службы Intl
    intl_checker/              IntlChecker: new Intl.*, статические, члены
    intl_feature/              IntlFeature: тема
    intl_lowerer/              IntlLowerer: IR опций и чисел Intl
    intl_method_checker/       IntlMethodChecker: format и toLocaleString
    intl_options_checker/      IntlOptionsChecker: объекты опций Intl
  json/                        JSON и тип Json
    json_checker/              JsonChecker: Json — члены, индекс, сужение
    json_feature/              JsonFeature: тема
    json_lowerer/              JsonLowerer: IR Json
    json_static_checker/       JsonStaticChecker: JSON.parse литерала при сборке
  math/                        Math и Bit
    math_checker/              MathChecker: вызовы Math и Bit
    math_feature/              MathFeature: тема
    math_lowerer/              MathLowerer: IR Math
  os/                          node:os
    os_checker/                OsChecker
    os_exports/                что экспортирует node:os
    os_feature/                OsFeature: тема
    os_lowerer/                OsLowerer
  path/                        node:path
    path_checker/              PathChecker
    path_exports/              что экспортирует node:path
    path_feature/              PathFeature: тема
    path_lowerer/              PathLowerer
  process/                     process
    process_checker/           ProcessChecker: process.*, env, argv, on / once / off
    process_exports/           что экспортирует node:process
    process_feature/           ProcessFeature: тема
    process_lowerer/           ProcessLowerer: IR process
  reflect/                     отражение: Reflect и метаданные декораторов
    class_info_lowerer/        ClassInfoLowerer: описание класса для отражения
    metadata_checker/          MetadataChecker: метаданные декораторов
    reflect_args_lowerer/      ReflectArgsLowerer: аргументы вызовов через отражение
    reflect_call_checker/      ReflectCallChecker: Reflect.apply и вызовы методов по имени
    reflect_checker/           ReflectChecker: вход проверки отражения
    reflect_dispatch_lowerer/  ReflectDispatchLowerer: выбор метода при вызове по имени
    reflect_feature/           ReflectFeature: тема
    reflect_lowerer/           ReflectLowerer: IR Reflect
  regex/                       регулярные выражения, собранные при сборке
    match_checker/             MatchChecker: результат match — группы с типами
    matcher_analysis/          MatcherAnalysis: план машины совпадений
    matcher_builder/           MatcherBuilder: IR машины совпадений
    matcher_chars/             MatcherChars: чтение и классы символов
    matcher_head/              MatcherHead: быстрый поиск начала
    matcher_instructions/      MatcherInstructions: инструкции машины
    matcher_memo/              MatcherMemo: память для защиты от взрыва перебора
    matcher_repeats/           MatcherRepeats: жадные и ленивые повторы
    regex_checker/             RegexChecker: литералы, методы RegExp
    regex_feature/             RegexFeature: тема
    regex_lowerer/             RegexLowerer: IR вызовов RegExp
    regex_native_lowerer/      RegexNativeLowerer: машина совпадений в машинном коде
    regex_syntax/              разбор регулярного выражения
    string_regex_checker/      StringRegexChecker: replace / match / split строк с RegExp
  result/                      Result: ok, err, isOk
    err_sites/                 ErrSites: где создана ошибка (файл, функция)
    result_checker/            ResultChecker: ok / err, члены Result, забытый Result
    result_combine_lowerer/    ResultCombineLowerer: Result.combine
    result_feature/            ResultFeature: тема
    result_lowerer/            ResultLowerer: раскладка Result
    result_method_checker/     ResultMethodChecker: map, andThen и др.
    result_types/              ResultTypes: тип Result, импорт neverthrow
  stream/                      node:stream
    chunk_types/               ChunkTypes: типы кусков потоков, крючки onRead…
    set_encoding_checker/      SetEncodingChecker: setEncoding сужает тип потока
    stream_checker/            StreamChecker: встроенные вызовы потоков
    stream_feature/            StreamFeature: тема
    stream_lowerer/            StreamLowerer: IR вызовов потоков
    stream_new_checker/        StreamNewChecker: new Readable и др.
    stream_pipeline/           StreamPipeline: pipeline, finished, compose
    stream_sources/            StreamSources: createReadStream, process.stdin как поток
  tz_modules/                  модули на tz: tz:http, fetch, URL, мосты declare function
    http_checker/              HttpChecker: new Response, Request, Headers
    library_call_lowerer/      LibraryCallLowerer: IR вызовов библиотеки
    library_calls/             LibraryCalls: вызовы библиотеки на tz
    tz_modules_checker/        TzModulesChecker: declare function → tz_* рантайма, колбэки
    tz_modules_feature/        TzModulesFeature: тема
    tz_modules_lowerer/        TzModulesLowerer: IR вызовов рантайма из библиотеки
    url_checker/               UrlChecker: new URL с литералом при сборке
  weakref/                     WeakRef
    weak_ref_checker/          WeakRefChecker
    weak_ref_lowerer/          WeakRefLowerer
    weakref_feature/           WeakRefFeature: тема
```

```
packages/           пакеты: tz.json, tz.lock, JSR, git, tz port
  dependency/       Dependency: зависимость из tz.json
  git_client/       скачивание пакета из git
  graph/            Graph: пакеты программы для загрузчика
  http_client/      HTTP для JSR
  import_ref/       ImportRef: импорт пакета в коде
  json_parser/      разбор JSON для tz.json и ответов JSR
  json_text/        строгая проверка текста JSON (и для импорта JSON)
  json_value/       JsonValue: разобранный JSON
  jsr_client/       JSR: версии и файлы пакета
  jsr_version/      JsrVersion: версия пакета JSR
  jsr_versions/     JsrVersions: список версий JSR
  lock/             Lock: tz.lock
  locked/           Locked: запись о пакете в tz.lock
  manifest/         Manifest: tz.json
  package/          Package: пакет в графе
  package_cache/    кэш скачанных пакетов
  package_graph/    граф пакетов программы из tz.json и tz.lock
  partial/          Partial: неполная версия (^1, ~1.2)
  paths/            папки пакетов
  port/             tz port — перенос библиотеки TypeScript
    edit/           Edit: правка текста при переносе
    file_port/      FilePort: перенос одного файла
    note/           Note: заметка о переносе
    port_context/   PortContext: общее для переноса пакета
    port_names/     имена, которые переименовывает перенос
  prepare_options/  PrepareOptions: как готовить пакеты
  program_runner/   запуск внешних программ (git)
  range/            Range: диапазон версий
  report/           сообщения о пакетах
  requirement/      Requirement: требование к версии
  resolver/         Resolver: выбор версий всех пакетов
  scanner/          импорты пакетов в коде
  source/           Source: откуда пакет (JSR, git, папка)
  specifier/        разбор jsr:… и npm:… в импорте
  staged_dir/       StagedDir: папка, которая появляется целиком или никак
  text_file/        чтение и запись текстовых файлов пакетов
  version/          Version: semver
```

```
cli/                  команды tz: разбор аргументов, по классу на команду
  add_command/        AddCommand: tz add — пакеты в tz.json
  args/               Args: разбор командной строки, текст справки
  build_command/      BuildCommand: tz build
  command_line/       CommandLine: выбирает команду по первому слову
  command_usage/      текст справки по командам
  entry_file/         EntryFile: какой файл собирать (аргумент или main из tz.json)
  error_printer/      ErrorPrinter: печать ошибок сборки в терминал
  exit_codes/         коды выхода tz
  explain_command/    ExplainCommand: --explain-parallel — что идёт параллельно и почему
  init_command/       InitCommand: tz init — tz.json, main.tz, .gitignore
  knowledge_command/  KnowledgeCommand: tz map / context / query / impact
  licenses_command/   LicensesCommand: tz licenses — лицензии чужого кода в exe
  lsp_command/        LspCommand: tz lsp — сервер редактора и tz lsp --check
  mcp_command/        McpCommand: tz mcp — инструменты для ИИ по MCP
  port_command/       PortCommand: tz port — перенос библиотеки TypeScript
  project_file/       поиск tz.json от текущей папки
  project_packages/   ProjectPackages: готовит пакеты tz.json перед проверкой
  remove_command/     RemoveCommand: tz remove
  run_command/        RunCommand: tz run — собрать и запустить
  update_command/     UpdateCommand: tz update
  version_command/    VersionCommand: tz version
```

```
knowledge/             записи о программе для редактора и ИИ (docs/lsp.md, шаг 2)
  knowledge/           Knowledge: символы, ссылки, типы, ошибки модулей
  knowledge_answers/   KnowledgeAnswers: ответы map / context / query / impact
  knowledge_json/      KnowledgeJson: записи в JSON и обратно
  knowledge_pass/      KnowledgePass: собирает записи после проверки
  knowledge_question/  KnowledgeQuestion: вопрос → ответ, общий для CLI и MCP
  knowledge_target/    KnowledgeTarget: имя или место файл:строка:столбец → символ
  knowledge_walk/      KnowledgeWalk: обход дерева для записей
  project_text/        ProjectText: упоминания имени в docs и тестах
  symbol_text/         SymbolText: текст символа для ответа (подпись, тело)
```

```
lsp/              сервер редактора tz lsp (docs/lsp.md, шаги 1, 4, 5)
  check_process/  CheckProcess: дочерний tz lsp --check
  check_report/   CheckReport: ошибки и файлы программы как данные
  file_uri/       FileUri: file:// ↔ путь
  lsp_channel/    LspChannel: сообщения протокола по stdin / stdout
  lsp_check/      LspCheck: tz lsp --check — проверка в дочернем процессе
  lsp_features/   LspFeatures: наведение, определение, ссылки, дополнение, переименование, цвета
  lsp_server/     LspServer: открытые файлы, очередь проверок, публикация ошибок
  program_entry/  ProgramEntry: с какой точкой входа проверять файл
  text_document/  TextDocument: текст открытого файла и правки к нему
```

```
mcp/           tz mcp — инструменты для ИИ
  mcp_server/  McpServer: протокол MCP, инструменты, дочерние tz
```
