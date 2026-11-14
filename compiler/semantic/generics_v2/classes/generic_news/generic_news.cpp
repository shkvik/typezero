#include "compiler/semantic/generics_v2/classes/generic_news/generic_news.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

GenericNews::GenericNews(SemanticState &owner)
    : state(owner) {}

void GenericNews::checkGenericNew(const CheckGenericNew &task) {
  NewExpr *expr = task.newExpr;
  ClassDecl *generic = task.classDecl;
  auto &tasks = this->state.walk.tasks;
  expr->type = Type::error();
  if (!generic->library()) {
    return this->startGenericNew(task);
  }
  bool isEmitter = LibraryClassTable::isName(generic->name, LibraryClass::EVENT_EMITTER);
  if (isEmitter) {
    tasks.pushTask(
        AfterEmitterNew{.newExpr = expr, .classDecl = generic, .expected = task.expected});

    return tasks.pushTask(
        CheckEmitterNew{.newExpr = expr, .classDecl = generic, .expected = task.expected});
  }

  return this->afterLibraryTries(task);
}

void GenericNews::afterEmitterNew(const AfterEmitterNew &task) {
  Expr *emitter = this->state.walk.tasks.takeChecked();
  if (emitter != nullptr) {
    return this->state.walk.tasks.pushChecked(emitter);
  }

  return this->afterLibraryTries(CheckGenericNew{
      .newExpr = task.newExpr, .classDecl = task.classDecl, .expected = task.expected});
}

void GenericNews::afterLibraryTries(const CheckGenericNew &task) {
  if (!this->isDerivedStream(task.classDecl)) {
    return this->startGenericNew(task);
  }

  return this->state.walk.tasks.pushTask(CheckStreamNew{
      .newExpr = task.newExpr, .classDecl = task.classDecl, .expected = task.expected});
}

bool GenericNews::isDerivedStream(const ClassDecl *classDecl) const {
  auto *stream = this->state.library.declarations.findLibraryClass(LibraryClass::STREAM);

  return classDecl != nullptr && stream != nullptr && classDecl->derivesFrom(stream);
}

void GenericNews::startGenericNew(const CheckGenericNew &task) {
  NewExpr *expr = task.newExpr;
  ClassDecl *generic = task.classDecl;
  auto &tasks = this->state.walk.tasks;
  if (!expr->typeArgs.empty()) {
    auto draft = ExplicitNewDraft{
        .newExpr = expr,
        .classDecl = generic,
        .expected = task.expected,
        .args = {},
    };

    return this->nextExplicitNewArgument(NextExplicitNewArgument{.draft = std::move(draft)});
  }
  auto declared = this->declaredFirst(generic);
  if (declared.isErr()) {
    this->state.diagnostics.error(expr->loc(), declared.error().message);
  }
  auto concreteClass = this->concrete(generic);
  if (concreteClass.isErr()) {
    this->state.diagnostics.error(expr->loc(), concreteClass.error().message);
  }
  auto constructors = generic->findConstructors();
  auto constructor = this->pickedConstructor(expr, generic, constructors);
  if (!constructor.has_value()) {
    return this->walkArgsLoosely(expr);
  }
  auto *function = this->constructorFunction(generic, *constructor);
  auto *call = this->state.ast.make<CallExpr>(expr->loc(), nullptr, std::move(expr->args));
  expr->form = ClassNew{.ctor = call};
  tasks.pushTask(AfterGenericNewBinding{
      .newExpr = expr,
      .classDecl = generic,
      .call = call,
      .constructor = *constructor,
      .function = function,
  });

  return tasks.pushTask(BindGenericCall{
      .call = call,
      .function = function,
      .loc = expr->loc(),
      .expected = task.expected,
      .typeArgs = {},
  });
}

void GenericNews::walkArgsLoosely(NewExpr *expr) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(expr);
  for (Expr *&arg : llvm::reverse(expr->args)) {
    if (arg->type == nullptr) {
      tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
    }
  }
}

void GenericNews::nextExplicitNewArgument(const NextExplicitNewArgument &task) {
  ExplicitNewDraft draft = task.draft;
  NewExpr *expr = draft.newExpr;
  auto &tasks = this->state.walk.tasks;
  if (draft.args.size() == expr->typeArgs.size()) {
    ClassDecl *generic = draft.classDecl;
    auto args = std::move(draft.args);
    tasks.pushTask(AfterExplicitInstance{.draft = std::move(draft)});

    return tasks.pushTask(
        InstantiateClassType{.classDecl = generic, .args = std::move(args), .loc = expr->loc()});
  }
  const TypeRef &ref = expr->typeArgs[draft.args.size()];
  tasks.pushTask(AfterExplicitNewArgument{.draft = std::move(draft)});

  return tasks.pushTask(ResolveTypeAnswer{.ref = &ref, .use = TypeUse::STANDALONE_OR_VOID});
}

void GenericNews::afterExplicitNewArgument(const AfterExplicitNewArgument &task) {
  ExplicitNewDraft draft = task.draft;
  const Type *arg = this->state.walk.tasks.takeAnswer();
  if (arg->isError()) {
    return this->walkArgsLoosely(draft.newExpr);
  }
  draft.args.push_back(arg);

  return this->nextExplicitNewArgument(NextExplicitNewArgument{.draft = std::move(draft)});
}

void GenericNews::afterExplicitInstance(const AfterExplicitInstance &task) {
  const ExplicitNewDraft &draft = task.draft;
  NewExpr *expr = draft.newExpr;
  auto *instance = this->instanceOf(this->state.walk.tasks.takeAnswer());
  if (instance == nullptr) {
    return this->walkArgsLoosely(expr);
  }
  expr->typeArgs.clear();

  return this->state.walk.tasks.pushTask(
      CheckClassNew{.newExpr = expr, .classDecl = instance, .expected = draft.expected});
}

ClassDecl *GenericNews::instanceOf(const Type *type) const {
  if (type->isError()) {
    return nullptr;
  }

  return const_cast<ClassDecl *>(type->classDecl());
}

void GenericNews::afterGenericNewBinding(const AfterGenericNewBinding &task) {
  auto &tasks = this->state.walk.tasks;
  bool isBound = tasks.takeVerdict();
  if (!isBound) {
    return tasks.pushChecked(task.newExpr);
  }
  auto bound = tasks.takeAnswers(task.function->typeParams.size());
  tasks.pushTask(AfterGenericNewInstance{
      .newExpr = task.newExpr,
      .classDecl = task.classDecl,
      .call = task.call,
      .constructor = task.constructor,
      .function = task.function,
  });

  return tasks.pushTask(InstantiateClassType{
      .classDecl = task.classDecl,
      .args = std::move(bound),
      .loc = task.newExpr->loc(),
  });
}

void GenericNews::afterGenericNewInstance(const AfterGenericNewInstance &task) {
  NewExpr *expr = task.newExpr;
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  auto *instance = this->instanceOf(tasks.takeAnswer());
  if (instance == nullptr) {
    return tasks.pushChecked(expr);
  }
  expr->form = ClassNew{.cls = instance, .ctor = call};
  call->type = Type::voidTy();
  expr->type = instance->type;
  tasks.pushChecked(expr);
  if (this->hasInitializer(instance)) {
    tasks.pushTask(CalledFunction{});
  }
  FunctionDecl *constructor = task.constructor;
  if (constructor == nullptr) {
    return;
  }
  auto constructors = task.classDecl->findConstructors();
  auto own = instance->findConstructors();
  auto position = size_t(llvm::find(constructors, constructor) - constructors.begin());
  if (position < own.size()) {
    call->target = FunctionCall{.func = own[position]->sym};
  }

  return tasks.pushTask(CheckConstructorAccess{.newExpr = expr, .constructor = constructor});
}

bool GenericNews::hasInitializer(const ClassDecl *instance) const {
  const ClassDecl *classDecl = instance;
  while (classDecl != nullptr) {
    if (classDecl->init != nullptr || !classDecl->constructors.empty()) {
      return true;
    }
    classDecl = classDecl->parent;
  }

  return false;
}

GenericNews::ResultClassDeclared GenericNews::declaredFirst(const ClassDecl *generic) const {
  const auto &modules = this->state.declaration.modules;
  auto *loading = modules.getLoadingModule();
  bool isLoadingHome = loading != nullptr && modules.findDeclModule(generic) == loading;
  bool isEarly =
      !generic->declared && isLoadingHome && !this->state.walk.functions.isInsideFunction();
  if (isEarly) {
    auto message = ErrorTextDeclaredFirst::before(generic->name);

    return err(GenericsClassesRegistryDeclaredFirstError{
        .code = GenericsClassesRegistryDeclaredFirstErrorCode::USED_BEFORE_DECLARATION,
        .message = message,
    });
  }

  return ok(generic);
}

GenericNews::ResultClassConcrete GenericNews::concrete(const ClassDecl *generic) const {
  if (generic->isAbstract) {
    auto message = ErrorTextConcrete::abstract(generic->name);

    return err(GenericsClassesRegistryConcreteError{
        .code = GenericsClassesRegistryConcreteErrorCode::ABSTRACT_CLASS,
        .message = message,
    });
  }

  return ok(generic);
}

std::optional<FunctionDecl *>
GenericNews::pickedConstructor(const NewExpr *expr, const ClassDecl *generic,
                               llvm::ArrayRef<FunctionDecl *> constructors) {
  if (constructors.empty()) {
    return nullptr;
  }
  if (constructors.size() == 1) {
    return constructors[0];
  }
  auto picked = this->constructorFit(expr, generic, constructors);
  if (picked.isErr()) {
    this->state.diagnostics.error(expr->loc(), picked.error().message);

    return std::nullopt;
  }

  return picked.value();
}

GenericNews::ResultCtorWasPicked
GenericNews::constructorFit(const NewExpr *expr, const ClassDecl *generic,
                            llvm::ArrayRef<FunctionDecl *> constructors) const {
  auto fit = std::vector<FunctionDecl *>();
  for (FunctionDecl *constructor : constructors) {
    if (this->isTakingCount(constructor->sym, expr->args.size())) {
      fit.push_back(constructor);
    }
  }
  if (fit.empty()) {
    auto message = ErrorTextConstructorFit::inferArgs(generic->name, true, expr->args.size());

    return err(GenericsClassesRegistryConstructorFitError{
        .code = GenericsClassesRegistryConstructorFitErrorCode::NO_CONSTRUCTOR_FITS,
        .message = message,
    });
  }
  if (fit.size() > 1) {
    auto message = ErrorTextConstructorFit::inferArgs(generic->name, false, expr->args.size());

    return err(GenericsClassesRegistryConstructorFitError{
        .code = GenericsClassesRegistryConstructorFitErrorCode::SEVERAL_CONSTRUCTORS_FIT,
        .message = message,
    });
  }

  return ok(fit[0]);
}

bool GenericNews::isTakingCount(const FuncSymbol *function, size_t count) const {
  if (count < function->minArgs) {
    return false;
  }

  return function->hasRest || count <= function->params.size();
}

FuncSymbol *GenericNews::constructorFunction(ClassDecl *generic, FunctionDecl *constructor) {
  auto &copies = this->state.generics.copies;
  auto *known = copies.findConstructorFunction(generic, constructor);
  if (known != nullptr) {
    return known;
  }
  auto *function = this->state.ast.newFunc();
  copies.rememberConstructorFunction(generic, constructor, function);
  function->name = this->state.ast.keepName("new " + generic->name.str());
  function->loc = generic->loc();
  auto *functionDecl = this->state.ast.make<FunctionDecl>(generic->loc(), function->name);
  functionDecl->typeParams = generic->typeParams;
  functionDecl->sym = function;
  function->decl = functionDecl;
  function->typeParams = generic->typeArgs;
  function->typeDefaults = copies.getClassDefaults(generic);
  if (constructor != nullptr) {
    function->params = constructor->sym->params;
    function->minArgs = constructor->sym->minArgs;
    function->hasRest = constructor->sym->hasRest;
  }
  function->setReturnType(generic->type);

  return function;
}

}
