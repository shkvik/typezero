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
- **Промежуточный результат — в переменной с именем**, лямбда — именованной переменной выше.
- **Объявления в блоке — по длине строки**, псевдонимы `Result…` — одной длины.
- **Части движка `semantic/` создаёт только сборка `SemanticParts`**, соседей часть берёт у неё в
  теле своего конструктора.
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

Часть движка — класс проверки внутри `compiler/semantic/` (`LoopChecker`, `CallChecker`…). Все
части один раз создаёт сборка `SemanticParts` (`semantic/semantic_parts/`), а её — фасад
`SemanticExternalFacade`. Части зовут друг друга по кругу, поэтому сами друг друга не создают.

1. **Соседей часть берёт у сборки в теле своего конструктора:** конструктор получает
   `SemanticParts &all`, каждый сосед — поле-указатель, которое тело заполняет адресом поля
   сборки.
   Сборка создаёт части строкой `имя(state, *this)`; часть без соседей — `имя(state)` и конструктор
   с одним `SemanticState &owner`.

   ```cpp
   class LoopChecker {
   public:
     LoopChecker(SemanticState &owner, SemanticParts &all);

   private:
     Scopes *scopes = nullptr;
     SemanticState &state;
     LoopScanner *loopScanner = nullptr;
   };

   LoopChecker::LoopChecker(SemanticState &owner, SemanticParts &all)
       : state(owner) {
     this->scopes = &all.scopes;
     this->loopScanner = &all.loopScanner;
   }
   ```

   В теле метода: `this->loopScanner->scanLoop(...)`.
2. **Конструктор части только берёт адреса соседей** и ничего у них не зовёт: соседи в этот момент
   ещё могут быть не созданы, известен только их адрес.
3. **Поля части — `SemanticState &state` и указатели на соседей.** Данные живут в группах
   `SemanticState`: их сохраняет и возвращает снимок `ReturnInferrer::enterBody`/`leaveBody`, а
   поле части прошло бы мимо снимка. Исключение — `DeclarationChecker`: `checkedModule` и
   `exportedNames` живут один обход файла `.d.tz` и обнуляются в его начале.
4. **Части создаёт только `SemanticParts`.** Создать часть на месте
   (`ExprChecker(this->state).checkExpr(...)`) или локальной переменной нельзя. Новая часть —
   открытое поле и строка в конструкторе `SemanticParts`; новый сосед — поле-указатель и строка в
   теле конструктора части.
5. **Поля сборки берут только конструкторы частей и фасад** (`this->parts->scopes`). В методах
   части соседа берут из своего поля, а не из сборки.
6. **Поля `SemanticParts` открыты, геттеров нет** — исключение из правила «у класса поля
   закрыты»: сборка — склад частей, а не объект с правилами. Подменить часть нельзя: у частей
   есть поле-ссылка `SemanticState &state`, поэтому присваивание компилятор запрещает сам.

Проверка правил 2–5: `python tmp/check_parts.py` печатает нарушения и выходит с кодом 1.

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
    std::string message =
        ErrorTextReadyThis::noSuper(this->checker.initFn_->cls()->parent->name);

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
ClassChecker::ResultEveryFieldSet fieldsSet = checker_.classChecker().checkFieldsSet();
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
  кодом, а текст берёт у внутренней: `.message = found.error().message`.
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

## Вид вместо флагов — `Union`

Если от вида зависит, какие поля есть, это `Union` (`compiler/union/union/union.h`, обёртка над
`std::variant`), а не набор `bool`. Флаги `isStatic` и `isAbstract` можно было поставить оба
сразу; в `Union` такое состояние не записать, а у статического метода физически нет `thisVar`.

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
вложенных вызовов в аргументах избегаем. Лямбду, которую передаём в `any_of` и подобное,
объявляем выше как переменную, а не пишем на месте.

```cpp
auto decorated = [](const FunctionDecl *fn) {
  return !fn->decorators().empty() ||
         llvm::any_of(fn->params, [](const Param &p) { return !p.decorators.empty(); });
};
auto decoratedField = [](const ClassField &f) { return !f.decorators.empty(); };

bool isDecoratedAnywhere = !cls->decorators.empty() || llvm::any_of(cls->methods, decorated) ||
                           llvm::any_of(cls->constructors, decorated) ||
                           llvm::any_of(cls->fields, decoratedField);
```

Длинное условие тоже получает имя: `bool isTopLevel = ...; if (!isTopLevel) { ... }`.

## Обходы без рекурсии

Деревья и графы обходим циклом, а не рекурсией. Форма зависит от того, что спрашиваем. Приёмы с
примерами «было / стало» и тем, на что смотреть, — `docs/recursion_patterns.md`.

- **«Есть ли где-то X» по телу** — тело один раз раскладывается в список узлов с их местом
  (`PlacedNodes`, обход в ширину: очередь — сами списки), вопрос — обычный `for` с ранним
  `return`. Детей добавляют маленькие функции по смыслу (`addLoopParts`, `addBranchParts`) из
  `if (auto *x = dyn_cast<X>(node))` без пустых `case`.
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
- В блоке ошибки — пустая строка между `std::string message = ...;` и `return err(...)`.
- Группы объявлений (псевдонимы, функции, поля) разделены пустой строкой.

## Форматирование

`.clang-format` в корне (и копия в `tmp/`), запускается по Alt+Shift+F в VS Code. Сверх стиля
LLVM:

- `PackConstructorInitializers: Never` — каждый инициализатор конструктора на своей строке;
- `AllowShortFunctionsOnASingleLine: Empty` — тело функции на своих строках;
- `IndentCaseLabels: true` — `case` с отступом внутри `switch`.

Строка — до 100 знаков. Выравнивание объявлений пробелами не включаем: ровный столбец дают
имена одной длины. Правка чужого файла форматирует только изменённые строки
(`git clang-format`), чтобы не трогать остальное.

## Как ИИ-помощник правит код

- Сначала код, сборка и тесты — только когда попросили.
- Большую переделку сначала показывает на маленьком примере в `tmp/` (`tmp/union_example.cpp`),
  потом переносит в компилятор.
- При переделке тексты сообщений не меняются ни на символ, поведение сохраняется; если где-то
  оно всё же меняется, это сказано отдельно.
- Комментарии в код не добавляет, закомментированный код удаляет.
- В ответе отделяет проверенное от гипотез.
