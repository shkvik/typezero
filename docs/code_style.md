# Стиль кода компилятора

Правила для C++ в `compiler/`, собраны 5–6 ноября 2026 года из замечаний при переделке
`parallel/analysis` и `ClassChecker`. Они дополняют раздел «Правила кода» в
[structure.md](structure.md) (папка на класс, `#include` от корня, скобки у `if`, нет комментариев)
и написаны и для человека, и для ИИ-помощника, который правит код. Старый код под них ещё не
переписан: правила действуют для нового кода и для того, что переделываем.

Все правила об одном: **по коду видно, что он делает, без прыжков по файлам и без знания
умолчаний.** Невозможное состояние нельзя записать, ошибка — значение с кодом, у каждого
промежуточного результата есть имя.

## Коротко

- **`struct` — только данные.** Есть конструктор, методы или правила — это `class` с
  `private`-полями.
- **Свои поля и свои методы — через `this->`**, поля без `_` в конце. Параметр конструктора
  называется иначе, чем поле.
- **Методы — не `static`**, даже если не читают поля. Исключение — классы `ErrorText…`.
- **Тела функций — в `.cpp`**, даже короткие. В заголовке только объявления и шаблоны.
- **Имена полностью**, без сокращений; числа — `int`, если в них нет особого смысла.
- **Функция, которая может не сработать, отдаёт `Result`**: сначала ошибки, `ok` последним,
  сообщает об ошибке вызывающий.
- **Ошибки — по enum и структуре на функцию**, коды в SCREAM_CASE, тексты в классах
  `ErrorText…` в `<имя>_errors.cpp`.
- **Вид вместо флагов — `Union`**: противоречивое состояние нельзя записать; ветвление —
  `switch` по `kind()` со всеми вариантами, без лямбд-обработчиков.
- **Промежуточный результат — в переменной с именем**, тип — `auto`, если он виден справа;
  вместо лямбды — метод с именем.
- **Указатель сравниваем явно** (`!= nullptr`), приведение (`dyn_cast`) — в переменной над `if`.
- **Функция с несколькими ошибками — список шагов**, каждый шаг — маленькая проверка `check…`
  с одной ошибкой. Ветки цикла — одной формы: `if` и один `return`.
- **Объявления в блоке — по длине строки**, псевдонимы `Result…` — одной длины.
- **Реестры `semantic/` создаёт только сборка `SemanticParts`**, соседей у реестра нет: чужая
  работа — задача машины (`state.walk.tasks`).
- **Имена встроенного — из таблиц `program/library/`**: глобальные объекты, свойства и методы
  встроенных типов не пишутся строками в проверке.
- **Обходы — циклом, не рекурсией**: списки и `for`, `while` по цепочке, стековая машина для
  ответов из детей, известный алгоритм — под своим именем (раздел «Обходы без рекурсии»).
- **Комментариев нет.** Форматирует `.clang-format`.

## Классы и структуры

`struct` — значение без поведения: поля и больше ничего. Как только появляется конструктор,
метод или правило, которое надо соблюдать, это `class`, а поля закрыты.

Так:

```cpp
struct CallEdge {
  int from = 0;
  int call = 0;
  int to = 0;
};

class CallGraph {
public:
  explicit CallGraph(int functionCount);

  int add(const CallEdge &edge);
  const std::vector<int> &outgoing(int function) const;

private:
  std::vector<CallEdge> edges;
  std::vector<std::vector<int>> outgoingEdges;
};
```

Не так: `struct CallGraph { std::vector<CallEdge> edges; int add(...); }` — у структуры
появились правила, а поля открыты.

## Свои поля и методы

Закрытые поля без `_` в конце. Внутри класса к своим полям и к своим методам обращаемся через
`this->`, чтобы было видно, что код работает с этим объектом, а не с локальной переменной или
свободной функцией.

Методы не делаем `static`, даже если метод не читает поля: все методы класса зовутся одинаково,
через `this->`, и не надо помнить, какой из них статический. Исключение — классы `ErrorText…`:
в них только статические функции текстов, объектов у них нет (`ErrorTextReadyThis::noSuper(...)`).

```cpp
class ClassChecker {
public:
  explicit ClassChecker(Checker &owner);

private:
  Checker &checker;
};

ClassChecker::ClassChecker(Checker &owner)
    : checker(owner) {}
```

В теле метода:

```cpp
VarSymbol *self = this->checker.ctx_.newVar();
self->declSeq = ++this->checker.seq_;
ResultThisComesFrom found = this->thisOf(this->checker.currentDecl());
```

Параметр конструктора называется не так, как поле (`owner`, а не `checker`): иначе MSVC с
`/W4` предупреждает, что параметр закрывает поле (C4458).

## Заголовки и файлы

- Тела всех функций — в `.cpp`, даже в одну строку. В `.h` остаются объявления, шаблоны и
  `constexpr`.
- В `.cpp` первым идёт конструктор класса, за ним остальные методы.
- Заголовок подключает только то, без чего не собрать; остальное — объявлением (`class Type;`).
- Круг подключений разрываем вынесенным файлом: общие типы части кода — в
  `<имя>_types.h` рядом (`analysis_types.h`), ошибки — в `<имя>_errors.h` и `<имя>_errors.cpp`.
- Геттеров, которые просто отдают поле, не заводим: поле или открыто в `struct`, или класс
  делает работу сам. Функция-«поле» допустима, когда она вычисляет ответ, как `fn->cls()` из
  `Union` (ниже).

## Части движка `semantic/`

Часть движка — реестр подгруппы (`CodeLoopsRegistry`, `LibraryFsRegistry`…) или объект-вопрос в
группе `SemanticState` (`state.types.relations`, `state.flow.proofs`). Все реестры один раз создаёт
сборка `SemanticParts` (`semantic/semantic_parts/`), а её — фасад `SemanticExternalFacade`.

1. **У реестра нет соседей.** Конструктор получает только `SemanticState &owner`; реестр не зовёт
   другие реестры. Чужая работа — задача в `state.walk.tasks` (`pushTask`), её выполняет машина;
   ответ задачи лежит на стеке ответов, проверенное выражение — на стеке проверенных.
2. **Машина — `SemanticParts`.** Реестры лежат в группах по подгруппам (`DeclarationRegistries`,
   `CodeRegistries`… в `<группа>/<группа>_registries.h`) и зовутся как `code.loops.walkFor(...)`.
   `walkProgram` держит порядок обхода, `runTasks` берёт задачи, пока они есть, `runTask` — один
   `switch` по виду задачи, который отдаёт её реестру подгруппы.
3. **В `walkProgram`, `runTasks` и `runTask` поля и методы сборки — без `this->`** — исключение из
   правила «свои поля и методы через `this->`»: эти три метода читаются как порядок шагов, а
   приставка у каждой из сотен строк его прячет.
4. **Данные живут в группах `SemanticState`**, а не в полях реестра: их сохраняет и возвращает снимок
   `state.saveBody()` / `state.restoreBody()`, а поле реестра прошло бы мимо снимка.
5. **Реестр создаёт только `SemanticParts`**, и только на месте поля группы
   (`.loops = CodeLoopsRegistry(owner)`): части большого реестра держат ссылки друг на друга, поэтому
   реестр не копируют и не переносят.

## Имена

- **Полностью, без сокращений:** `FileSystemError`, а не `FsError`; `thisSource`, а не `thisSrc`.
  Исключение — функции в классах `ErrorText…`: там имя до 10 символов, смысл дают класс и код.
- **Как в дереве:** флаг называется так же, как в `FunctionDecl` (`isAsync`), а не своим словом.
- **Пары — `from` и `to`**, а не `caller`/`callee` или `src`/`dst`.
- **Числа — `int`**, если это номер или счётчик без особого смысла. Не `unsigned` и не
  `enum class FunctionId : unsigned {}` — обёртка не говорит, что это за тип, и код обрастает
  приведениями.
- **Имя говорит, что лежит:** не `callees` для целей одного вызова, а `targets`; не `named` для
  списка мест вызова.
- **Геттер — `get` + имя типа:** метод, который только отдаёт хранимый объект, —
  `getScopeStack()`, `getDiagnostics()`, `getCurrentFunction()`; голое существительное
  (`scopes()`, `program()`) непонятно. Поле при этом называется просто: `scopeStack`.
- **`bool` — `is…`/`has…`/`can…`, и у методов, и у переменных:** `isInsideFunction()`, а не
  `insideFunction()`; `bool isInserted`, а не `bool inserted`; `bool hasNarrowings`, а не
  `bool narrowed`.
- **Остальные методы — действием по смыслу:** `rememberStatsClass`, `findClassNamed`,
  `markUsesAsync`, `openScope`; флаг «мы сейчас внутри …» — парой `enterPromiseCall()` /
  `leavePromiseCall()` над счётчиком глубины и проверкой `isInsidePromiseCall()`; метод, который
  отдаёт `bool`, всегда что-то проверяет, поэтому `enter…` ничего не возвращает.
- **Хранилище данных отдаёт действия, а не поля:** класс-хранилище (как `SemanticState` и его группы)
  держит поля закрытыми; снаружи только методы по смыслу, без доступа к своим контейнерам.

## Методы встроенных типов — из таблиц

Имя, которое компилятор узнаёт в программе, ищется в таблице `program/library/` один раз, дальше
код сравнивает `enum` и читает свойства строки таблицы, а не держит свой список строк:

- глобальные объекты и функции — `GlobalNameTable` (`GlobalName::MATH`), проверка в коде —
  `CallChecker::isGlobalName(expr, GlobalName::MATH)`;
- их статические члены — `GlobalMemberTable` (`Array.from`, `Object.keys`, `Symbol.iterator`),
  проверка — `CallChecker::isGlobalMember(member, GlobalMember::ARRAY_FROM)`;
- свойства встроенных типов — `BuiltinPropertyTable` (`length`, `byteLength`, `size`);
- имена встроенных типов в позиции типа — `TypeNameTable` (`Generator`, `Map`, `Record`, `void`);
  вид имени — в `form`: `IteratorTypeName{.async}`, `CollectionTypeName{.collection}`…;
- методы: массива — `ArrayMethodTable`, `Result` — `ResultMethodTable`, итератора —
  `IteratorMethodTable`, строки — `StringMethodTable`, `Map`/`Set` — `DictMethodTable`,
  `Object.prototype` — `ObjectMemberTable`, байтовых типов — `BytesMethodTable`.

Имя для созданного компилятором узла тоже берётся из таблицы (`DictMethodTable::getName(...)`).

У строки таблицы нет полей по умолчанию и необязательных полей: то, что есть только у части
методов, лежит в `form` — `Union` видов (правило «Вид вместо флагов» ниже). Пропустить `form` в
строке нельзя — не соберётся; записать полю чужого вида — тоже.

```cpp
{
    .name = "push",
    .method = ArrayMethod::PUSH,
    .builtin = Builtin::ArrPush,
    .form = ResizingArrayMethod{.change = LengthChange::GROWS},
},
```

```cpp
const ArrayMethodInfo *method = ArrayMethodTable::find(member->name);
bool isResizing = method != nullptr && (method->grows || method->shrinks);
```

Не так: `static const char *const resizing[] = {"push", "pop", "shift", "unshift", "splice"};` —
такие списки лежали в одиннадцати файлах и расходились между собой.

## Ошибки — `Result`

Функция, которая может не сработать, отдаёт `Result<T, E>` (`compiler/result/`, `ok(x)` /
`err(x)`). Внутри сначала идут проверки с ошибками, каждая в своём `if`, и `return ok(...)`
стоит последним. Сообщает об ошибке вызывающий: он знает место (`loc`) и решает, что делать
дальше.

У каждой функции — свой код ошибки и своя структура, в `<имя>_errors.h` рядом с классом. Имя —
класс + функция: `ClassChecker` + `readyThis` → `ClassCheckerReadyThisErrorCode`,
`ClassCheckerReadyThisError`. Коды — SCREAM_CASE.

```cpp
enum class ClassCheckerReadyThisErrorCode {
  IN_CLOSURE_BEFORE_FIELDS_SET,
  BEFORE_SUPER,
  BEFORE_FIELDS_SET,
};

struct ClassCheckerReadyThisError {
  ClassCheckerReadyThisErrorCode code;
  std::string message;
};

class ErrorTextReadyThis {
public:
  static std::string inClosure();
  static std::string noSuper(llvm::StringRef parentClass);
  static std::string unset(llvm::StringRef field);
};
```

Текст сообщения собирает класс `ErrorText<функция>` со статической функцией на каждый код;
тела — в `<имя>_errors.cpp`. Так в коде проверки нет строк с кавычками, а тексты лежат в одном
месте. Функция принимает ровно те данные, которые нужны её тексту.

```cpp
ClassChecker::ResultThisReadiness ClassChecker::readyThis(VarSymbol *var) const {
  bool isInInit = var == this->checker.initThis_;
  bool isInConstructor = isInInit && this->checker.currentDecl() == this->checker.initFn_;
  if (isInConstructor && this->checker.flow_.beforeSuper) {
    auto message = ErrorTextReadyThis::noSuper(this->checker.initFn_->cls()->parent->name);

    return err(ClassCheckerReadyThisError{
        .code = ClassCheckerReadyThisErrorCode::BEFORE_SUPER,
        .message = message,
    });
  }

  return ok(var);
}
```

Вызывающий:

```cpp
auto fieldsSet = checker_.classChecker().checkFieldsSet();
if (fieldsSet.isErr()) {
  checker_.diags_.error(fn->loc(), fieldsSet.error().message);
}
```

- **В ошибке только `code` и `message`.** Место (`loc`) выбирает вызывающий: у него свои узлы,
  и по коду он знает, что подчеркнуть (`switch` по коду, если места разные).
- **Несколько ошибок за один проход** — не список в одном `Result`, а несколько маленьких
  проверок, у каждой один код. Вызывающий зовёт их по очереди в прежнем порядке, сообщает о
  каждой ошибке и идёт дальше, поэтому все сообщения остаются.
- **После ошибки проверка продолжается:** вызывающий сообщает и оставляет в дереве свой узел
  (как раньше `return call;`), подставляет запасное значение сам (`element = Type::error()`).
- **«Не моё»** у функции-распознавателя — отдельный код без текста (`NOT_A_TIMER`), вызывающий
  идёт к следующему. Если сообщение уже выдано раньше, — код без текста (`ALREADY_REPORTED`).
- **Ошибка, которая передаёт текст внутренней ошибки дальше**, собирает свою структуру со своим
  кодом, а текст берёт у внутренней: `.message = found.error().message`. Это про чужую функцию;
  шаги своей функции отдают её же ошибку (см. «Функция с несколькими ошибками — список шагов»).
- **`Result` и на вопросы да/нет**, если так решили для класса: `isInitThis(...).isOk()`.
- **Структура для `ok`** собирается прямо в вызове так же, как для `err`, с запятой после
  последнего поля: `return ok(BuiltProgram{ .warnings = std::move(warnings), });` (11 ноября).
- **Внутренняя ошибка компилятора** — тоже код, текст начинается с `internal error:`.

### Псевдонимы и порядок объявлений

Длинные типы `Result` получают псевдоним внутри класса: `Result` в начале, имена одной длины,
чтобы имена функций встали в столбец. Строки блока — по длине.

```cpp
private:
  using ResultThisReadiness = Result<VarSymbol *, ClassCheckerReadyThisError>;
  using ResultThisComesFrom = Result<const FunctionDecl *, ClassCheckerThisOfError>;
  using ResultFieldHasValue = Result<const ClassField *, ClassCheckerFieldStartValueError>;
  using ResultReturnMatches = Result<const FunctionDecl *, ClassCheckerOverrideReturnError>;

  ResultThisReadiness readyThis(VarSymbol *var) const;
  ResultThisComesFrom thisOf(const FunctionDecl *current) const;
  ResultReturnMatches overrideReturn(const ClassDecl *cls, const FunctionDecl *method);
  ResultFieldHasValue fieldStartValue(const ClassDecl *cls, const ClassField &field) const;
```

### Функция с несколькими ошибками — список шагов

Если функция может закончиться одной из нескольких ошибок, её тело — шаги по порядку, а
проверки лежат под ней в порядке вызова (12 ноября, образец — `UtilityTypes::resolveUtilityType`).

```cpp
UtilityTypes::ResultUtilityApplied UtilityTypes::resolveUtilityType(const TypeRef &ref) {
  const auto *utility = this->findUtility(ref.name);
  if (utility == nullptr) {
    return err(UtilityTypesResolveUtilityTypeError{
        .code = UtilityTypesResolveUtilityTypeErrorCode::NOT_A_UTILITY,
        .message = "",
    });
  }
  auto argumentCount = this->checkArgumentCount(ref, *utility);
  if (argumentCount.isErr()) {
    return err(argumentCount.error());
  }
  auto operands = this->utilityOperands(ref, *utility);
  if (operands.back()->isError()) {
    return ok(operands.back());
  }
  auto operand = this->checkOperand(ref, *utility, operands.front());
  if (operand.isErr()) {
    return err(operand.error());
  }
  const auto *unevaluated = Type::typeOp(utility->op, operands);
  const auto *evaluated = this->typeOperators->evalTypeOp(unevaluated, operands);

  return ok(evaluated);
}
```

- **Шаг — маленькая проверка `check…` с одной ошибкой**; текст ошибки собирается в ней.
- **Шаги отдают ту же структуру ошибки, что и сама функция** (`Result<void, …Error>` с тем же
  типом ошибки), и функция передаёт её как есть: `return err(step.error());`. Свою структуру
  на каждый шаг не заводим: пришлось бы пересобирать ошибку в каждом `if`.
- **Что проверять, решает вид из таблицы, а не пачка `bool`**: вместо `isFunctionExpected`,
  `isTextExpected`, `isFunctionMissing` — `switch` по полю строки таблицы.

```cpp
switch (utility.operand) {
  case UtilityOperand::ANY_TYPE:
    return ok();
  case UtilityOperand::FUNCTION_TYPE:
    return this->checkFunctionOperand(ref, operand);
  case UtilityOperand::TEXT_TYPE:
    return this->checkTextOperand(ref, operand);
}

return ok();
```

## Вид вместо флагов — `Union`

Если от вида зависит, какие поля есть, это `Union` (`compiler/union/union/union.h`: место под
самый большой вариант и номер варианта), а не набор `bool`. Флаги `isStatic` и `isAbstract`
можно было поставить оба сразу; в `Union` такое состояние не записать, а у статического метода физически нет `thisVar`.

```cpp
enum class MethodKind {
  INSTANCE_METHOD,
  STATIC_METHOD,
  CONSTRUCTOR,
  ABSTRACT_METHOD,
};

struct StaticMethod {
  static constexpr MethodKind kind = MethodKind::STATIC_METHOD;
};

struct Constructor {
  static constexpr MethodKind kind = MethodKind::CONSTRUCTOR;
  VarSymbol *thisVar = nullptr;
};

struct ClassMember {
  ClassDecl *cls = nullptr;
  Union<InstanceMethod, StaticMethod, Constructor, AbstractMethod> method = InstanceMethod{};
};
```

Что можно делать с `Union`:

- `is<T>()` — да или нет; `as<T>()` — указатель на поля этого вида или `nullptr`.
- `switch (value.kind())` — со всеми вариантами и без `default`: забытый `case` ловит
  предупреждение C4062 (`/w44062` в CMake).

Чего нельзя: `std::get` и `std::visit` напрямую. Исключения выключены, и `std::get` на чужом
виде роняет компилятор. Набора лямбд по одной на вид (`match`) тоже нет: ветвление читается
сверху вниз только через `switch`.

```cpp
ClassMember *member = fn->member();
switch (member->method.kind()) {
  case MethodKind::INSTANCE_METHOD:
    member->method.as<InstanceMethod>()->thisVar = self;
    break;
  case MethodKind::STATIC_METHOD:
    break;
  case MethodKind::CONSTRUCTOR:
    member->method.as<Constructor>()->thisVar = self;
    break;
  case MethodKind::ABSTRACT_METHOD:
    member->method.as<AbstractMethod>()->thisVar = self;
    break;
}
```

Если `switch` выбирает значение или ветку, `return` стоит в каждом `case`, а после `switch` —
запасной `return` (без него MSVC считает функцию путём без возврата, C4715). `default` нет.

```cpp
switch (this->place.kind()) {
  case VarPlaceKind::GLOBAL_VAR:
    return nullptr;
  case VarPlaceKind::TOP_LEVEL_LOCAL_VAR:
    return &this->place.as<TopLevelLocalVar>()->state;
  case VarPlaceKind::FUNCTION_LOCAL_VAR:
    return &this->place.as<FunctionLocalVar>()->state;
}

return nullptr;
```

## Имена у промежуточных значений

Результат вызова, который дальше куда-то передаётся, сначала кладём в переменную с именем;
вложенных вызовов в аргументах избегаем.

```cpp
const auto *unevaluated = Type::typeOp(utility->op, operands);
const auto *evaluated = this->typeOperators->evalTypeOp(unevaluated, operands);
```

Длинное условие тоже получает имя: `bool isTopLevel = ...; if (!isTopLevel) { ... }`.

**Тип — `auto`**, если он виден из правой части: `auto message = ErrorTextUtility::count(...)`,
`auto operands = std::vector<const Type *>()`, `const auto *utility = this->findUtility(name)`,
`const auto &store = this->state.getClassStore()`. Тип пишем, когда справа его не видно и он
важен для чтения (`const Type *subject = operands[0];`), и у `bool`.

**Вместо лямбды — метод с именем** (12 ноября). Лямбда внутри функции прячет кусок логики и
захватывает всё подряд; метод виден в заголовке и зовётся через `this->`.

```cpp
bool UtilityTypes::isGenericKey(const Type *key) const {
  return key->isParam() && !key->isListParam() && this->isExpectingLiteral(key);
}

if (!key->isStringLiteral() && !this->isGenericKey(key)) {
```

## Указатели, приведения, ветки

**Указатель сравниваем явно:** `x != nullptr`, `x == nullptr`, а не `if (x)` и `if (!x)`. Так
видно, что это указатель, а не `bool`.

**Приведение — в переменной над `if`**, без `if` с инициализатором (8 ноября).

```cpp
auto *functionDecl = dyn_cast<FunctionDecl>(item);
if (functionDecl != nullptr && functionDecl->name == name) {
  return this->functionDeclType(functionDecl);
}
```

**Ветки цикла — одной формы** (12 ноября). Если цикл перебирает несколько случаев, каждый случай —
один `if` и один `return`; подробности случая — в методе с именем. `continue` вперемешку с
`return` и вложенный цикл внутри такого цикла не пишем.

```cpp
for (Stmt *item : from->file.items) {
  auto *functionDecl = dyn_cast<FunctionDecl>(item);
  auto *classDecl = dyn_cast<ClassDecl>(item);
  auto *varDecl = dyn_cast<VarDeclStmt>(item);

  if (functionDecl != nullptr && functionDecl->name == name) {
    return this->functionDeclType(functionDecl);
  }
  if (classDecl != nullptr && classDecl->name == name) {
    return this->classDeclType(classDecl);
  }
  if (varDecl != nullptr && this->hasDeclarator(varDecl, name)) {
    return this->varDeclType(varDecl, name);
  }
}
```

**Без «завели пустую, потом заполнили, потом проверили».** Вместо

```cpp
const VarDeclarator *declarator = nullptr;
if (varDecl != nullptr) {
  declarator = this->declaratorNamed(varDecl, name);
}
if (declarator != nullptr) {
  return this->declaratorType(declarator);
}
```

— один `if` с вопросом (`hasDeclarator`) и метод, который отдаёт ответ (`varDeclType`), как в
цикле выше. Повторный короткий поиск внутри двух методов — нормально.

## Обходы без рекурсии

Деревья и графы обходим циклом, а не рекурсией. Форма зависит от того, что спрашиваем. Приёмы с
примерами «было / стало» и тем, на что смотреть, — `docs/recursion_patterns.md`.

- **«Есть ли где-то X» по телу** — тело один раз раскладывается в список узлов с их местом
  (`PlacedNodes`, обход в ширину: очередь — сами списки), вопрос — обычный `for` с ранним
  `return`. Детей добавляют маленькие функции по смыслу (`addLoopParts`, `addBranchParts`):
  `dyn_cast` в переменную и `if (x != nullptr)`, без `switch` с пустыми `case`.
- **Цепочка** (`a.b.c`, снятие приведений, минусов, `T | undefined`) — `while` по одному ребёнку.
  Рекурсия на один уровень уходит выносом ветки в отдельную функцию.
- **Однородное дерево** (`&&`, `?:`, арифметика) — раскладка стеком в список слева направо
  (`ExprParts`), потом `for`.
- **Части типа** — `TypeParts::collect(type, маршрут)`. Маршрут — список переходов `TypeEdge`,
  куда функция **идёт** (не куда не идёт); все маршруты лежат рядом в `type_routes.h`.
- **Ответ узла из ответов детей** — стековая машина: запись «детьми вперёд» (post-order),
  потом проход со стеком ответов, родитель снимает ответы детей и кладёт свой. Ранний выход —
  либо не раскладывать детей, когда ответ ясен по узлу, либо остановить весь цикл на первой
  неудаче (правило «всё или ничего»). Кадры «узел + следующий ребёнок + накопленный ответ» с
  тремя функциями на вид узла — не пишем.
- **Известный алгоритм** (Тарьян и подобные) — свой стек кадров, но запись как в учебнике:
  метод и кадр носят имя алгоритма (`tarjanSearch`, `TarjanFrame`), тело цикла читается как сам
  алгоритм, ветки — в функциях с именами (`takeNextCall`, `finishTop`), без ссылок на элементы
  вектора, который растёт.
- **Лента событий** (обход с входом и выходом из областей) — обход пишет плоский список событий
  (`Union` видов), сканер — один `while`: `at = this->applyStep(tape[at], at + 1, scan);`.
  `applyStep` — `switch` по `kind()`, строка на вид; у всех обработчиков одна форма: событие и
  номер следующего шага на входе, номер шага, с которого продолжать, на выходе.
- **Поиск в отсортированном** пишем руками, с именем, которое говорит, что ищем:
  `binarySearchFirstGreater(enters, number)`.

```cpp
void CallCycles::tarjanSearch(int start, CycleSearch &search) {
  std::vector<TarjanFrame> frames;
  CallCycles::enter(start, search, frames);
  while (!frames.empty()) {
    int caller = frames.back().node;
    std::optional<int> callee = CallCycles::takeNextCall(frames.back(), search);
    if (callee.has_value()) {
      CallCycles::followCall(caller, *callee, search, frames);
    } else {
      this->finishTop(search, frames);
    }
  }
}
```

## Пустые строки

- Перед итоговым `return` функции — пустая строка, если до него есть код.
- В блоке ошибки — пустая строка между `auto message = ...;` и `return err(...)`.
- Группы объявлений (псевдонимы, функции, поля) разделены пустой строкой.

## Форматирование

`.clang-format` в корне (и копия в `tmp/`), запускается по Alt+Shift+F в VS Code. Сверх стиля
LLVM:

- `PackConstructorInitializers: Never` — каждый инициализатор конструктора на своей строке;
- `AllowShortFunctionsOnASingleLine: Empty` — тело функции на своих строках;
- `IndentCaseLabels: true` — `case` с отступом внутри `switch`;
- `MaxEmptyLinesToKeep: 2` — две пустые строки подряд не склеиваются в одну (14 ноября: ими
  разделены группы объявлений с разным типом результата).

Строка — до 100 знаков. Выравнивание объявлений пробелами не включаем: ровный столбец дают
имена одной длины. Правка чужого файла форматирует только изменённые строки
(`git clang-format`), чтобы не трогать остальное.

## Как ИИ-помощник правит код

- Сначала код, сборка и тесты — только когда попросили.
- Большую переделку сначала показывает на маленьком примере в `tmp/` (`tmp/refactor/examples/union_example.cpp`),
  потом переносит в компилятор.
- При переделке тексты сообщений не меняются ни на символ, поведение сохраняется; если где-то
  оно всё же меняется, это сказано отдельно.
- Комментарии в код не добавляет, закомментированный код удаляет.
- В ответе отделяет проверенное от гипотез.
